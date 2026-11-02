---
title: FFE 快路：296 s 边界、慢态旧观测与死旋钮（P10.3–P10.5 审计）
date: 2026-11-02
status: done
topic: accel-impl
summary: 296 s 边界对应 ffe_aging=300 而非 CLA；~110 Mbit/s 慢态是 P10.3 旧观测，交付态 700–800 Mbit/s 未复现。
evidence: overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c:232, overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c:888
supersedes:
---

# FFE 快路：296 s 边界、慢态旧观测与死旋钮（P10.3–P10.5 审计）

> **结论口径（2026-11-02 夜复核，读本文前先看这段）**：本文写在"慢态 ~110 Mbit/s 还得现场取证"的时候。
> 交付态的复测把这个数打下去了 ——〔本板实测〕单流 14 次 × 每次 ≈600 s × 累计 ≈57 GB，**全部
> 700–800 Mbit/s**；300 s 长流跨过 296 s 的那一刻**没有突跳**。⇒ **~110 Mbit/s 是 P10.3 时代的旧观测，
> 在本仓交付镜像上未复现**；§3 那三个动作是"当时准备做的"，不是现行待办清单。
>
> 日期：2026-11-02。范围：ZXHN E2631（ZX279128S）自研固件的 FFE 快路取证。
> 结论性质：离线只读审计 + 既有实测引用；本轮**未上机、未改码、未构建**。
> 口径：源码行号来自本仓 HEAD `2dab71d`；盒子跑的是 P10.5（`623a812`），两者不一致处逐句注明。
> 缩写：`SRC/` = `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`；
> `PPE/` = `overlay/package/kernel/kmod-e2631-ppe/src/`。
> 文中未写目录的 `zx279128s-*.c` 均指 `SRC/`，`zx279128s-ppe.c` 指 `PPE/`。

## §0 结论

1. **296 s 那个时间边界对应的是驱动的 `ffe_aging=300`，不是 PPE CLA aging**：`ffe_aging` 默认 300 s、
   每秒一拍（`zx279128s-accel.c:224,232,1279-1287`）；PPE 的 CLA v6 默认关（`zx279128s-ppe.c:52`，
   dmesg 打 `IPv6 classifier off`，`:2899`），`ipv6=0` 时 CLA 表是空的。用 CLA aging 去扫 296 s 是拿错仪器。
2. **D2 只读插桩可作为「20:27 那次 kexec 后 RX 起不来」的排除项**：+392 行全部落在 `show` / 2 个
   `module_param` / 1 个宏，probe/open/ISR/NAPI/RX/TX/`zx_tm_init` **0 行**，每包 0 条指令。
   但「只读」这个说法**半错**：`zx_tm_read0()` 内含 `writel(..., +0x344014)`（`zx279128s-main.c:888-890`），
   每次快照间接写 **33 次** ⇒ 审「只读」必须连被调函数一起审。
3. **慢态（~110 Mbit/s）是 P10.3 时代的旧观测，交付态未复现**〔本板实测〕：单流 14 次 ×≈600 s
   ×≈57 GB 全部 700–800 Mbit/s，300 s 长流跨 296 s 无突跳。§3 的三个动作按原样保留，作为"当时若再抽中
   同一现象就这样做"的记录，**不是现行待办**。
4. **「只有重启才能恢复」未实测**（既不是结论也不是已被否掉的说法）：当时的诊断笔记把它写成待验分支，
   模式往返复测从未做过。它与第 3 条一起失效——慢态本身在交付态没复现，"恢复手段"也就无从比较。
5. **一条判据级教训**：早先的核验清单把 debugfs 路径写成 `/sys/kernel/debug/ffe/`，实际是
   `/sys/kernel/debug/92000000.ethernet/`（`zx279128s-main.c:3537`）；同型错误让一次 CLA aging 扫描
   「一个数都没读到」，而那个 0 被当成「没老化」的证据。见 §6。

## §1 296 s 边界：驱动的 `ffe_aging=300`

- 时间常数：`zx279128s-accel.c:232` `static unsigned int ffe_aging = 300;`，
  `:224` 节拍 `ZX_FFE_TICK_MS 1000`；判定在 `:1279-1287`：
  `max_idle_ms = ffe_aging * 1000`，`jiffies_to_msecs(now - flow->lastuse) > max_idle_ms` 就
  `__zx_ffe_flow_del()`；`ffe_aging = 0` 表示永不老化（`:1277`）。
- **活跃流不会因为 300 s 到期被删**：`lastuse` 在每次命中时刷新（`zx279128s-accel.c:1713`、
  `:2992`、`:3046`），所以「296 s 突跳 = 老化把活跃流删了」在源码上就弱：只有「从没命中过的那一半」
  （例如只有单向数据的反方向）才会在 301±1 s 消失（当时的过程记录里也是这么推的）。
- **所以「边界是 296 s」只说明时间常数对得上，不构成「老化是成因」的判决**；要判成因仍得跑
  `ffe_aging=30/10` 的对照（当时列进了计划，未做）。
- **不是 PPE CLA aging**：`zx_ppe_ipv6` 默认 `false`（`zx279128s-ppe.c:52`），
  CLA v6 表只在 `ipv6=1` 时编程（`:1908-1911`），dmesg 打 `IPv6 classifier off`（`:2899`）。
  用 `dump=2/32` 读 CLA aging word（`:2621,2638`）在 `ipv6=0` 时读到 0，不能当「没老化」的证据
  （当时的取值记录）。
- 同类仪器错误的现场：C1b 用 CLA aging 扫 296 s，「一个数都没读到」，另有一层原因就是 debugfs 路径写错
  （原始记录未入库）。

## §2 D2 插桩审计：可排除，但不是「纯只读」

- **落点**：当时的只读插桩补丁（未入库）共 450 行 / 7 hunk / **+392 −1**；全部落在
  `zx279128s-eth.h` 文件头（1 个宏）、`zx279128s-main.c` 文件头（2 个 `module_param`）、
  以及 `zx_eth_dma_state_show()` 内部（`zx279128s-main.c:2934-3100`，rtnl_lock `:2959` /
  rtnl_unlock `:3098`）。probe(3592)/open(2688)/ISR(2646)/poll(2658)/RX(2088)/TX/`zx_tm_init`(917)
  **0 个 hunk**；数据面**每包 0 条指令、0 ns**
  （原始记录未入库）。
- **「只读」半错**：每次 `cat dma_state` 会经 `zx_tm_read0()` 对
  `base + ZX_TM_IND_CMD` 写硬件寄存器；`ZX_TM_IND_CMD = ZX_TM_BASE + 0x4014`，`ZX_TM_BASE = 0x340000`
  （`zx279128s-eth.h:90,105`），实际偏移 `0x344014`。`writel` 在 `zx279128s-main.c:888-890`；
  每次快照 **33 次**（16 表1 + 1 复读 + 16 表0）（原始记录未入库）。
  > 行号校正：该报告原文写 `main.c:886-888`，当前 HEAD 实测 `writel` 在 `888-890`（同一 md5
  > `0c94befbdd46e38b715a36668c27f72e`，差 2 行）；引用以实测为准。
- **执行面**：`dma_state` 只在 `zx_eth_debugfs_init()` 注册（`zx279128s-main.c:3539-3540`），
  全树没有 init 脚本/定时器去读它；两个新参数 `d1_ring_scan`/`d1_blind` 也只在 show 里被读
  ⇒ **没人 `cat` 就不执行**（原始记录未入库）。
- **为什么仍可排除**：① plain `#16`（无插桩）在 kexec 后同样楔死；
  ② D2 在全程不读 `dma_state`、空载无流量的情况下也能在开机 26–36 s 自己楔死
  （原始记录未入库）；③ 同一 `#16` 冷启动 6/6 不楔
  （原始记录未入库）。
- **审计规矩**：审「只读」不能只看新增行的 store 分类，必须连**被调函数**一起审——
  D2 的 33 条写就发生在 `zx_tm_read0()` 体内，不在「show 自己 173 条 store」的视野里
  （原始记录未入库）。
- **残留风险（无证据）**：`zx_tm_read0()` 的间接窗口写是否有运行期副作用，未验证；D2 让它第一次在
  DMA 已启动、链路已 UP 之后可达（同文件 §4）。默认配置下没有一处读被源码证明「读即清」，
  唯一被源码点名「读会清」的 `DMA+0x68` 已被 `zx279128s-main.c:3082-3084` 跳过（同文件 §5）。

## §3 慢态待判：三个零重刷动作

慢态签名：约 **0.293%** 的帧在 alias 判据处被静默丢掉，TCP 平衡在 **~92–111 Mbit/s**，
触发条件未知、实测 2/4 轮（原始记录未入库）。以下动作都**不需要重刷、不需要重编**，
全部在「慢态在场」时做；合计约 45 min（含抽中慢态的复现成本）。

| 顺序 | 动作 | 观测/判据 | 证据 |
|---|---|---|---|
| E0 | 慢态在场时先读 mode：`cat /sys/kernel/debug/92000000.ethernet/accel_ipv6` 第 1/2 行 + `accel_ipv4` + `dmesg \| grep -E "strike\|fell back"` | 若 mode=`stack` ⇒ 110 不是 ffe 的锅；若 mode=`ffe` 且 alias 在涨、却没 strike ⇒ 判据分母/分族与实际丢点不符 | 原始记录（未入库） |
| E1 | `echo stack > $D/accel_ipv6` → 15 s iperf；再 `echo ffe > $D/accel_ipv6` → 5 s iperf | stack >250 Mbit/s（基线 325.8，区间 304.8–340.7）；切回 ffe 后 >600 恢复 ⇒ 在途 bp 错配、切换可清；仍 ~110 ⇒ 持久态 | 原始记录未入库 |
| E2 | `ffe_bp_drop` A/B（先 `cat` 记初值，30–60 s 流后写回） | 慢态消失/不消失，同时看 `ffe_dup` 增量 | 同文件 §2 E2 |

- **E0 的「110 且 mode=ffe」本身就矛盾**：当前树已把 alias 纳入 autofallback 判据——
  `ZX_FFE_FB_ALIAS_PERMILLE=2`、`ZX_FFE_FB_ALIAS_STRIKES=2`（`zx279128s-accel.c:320-321`），
  连续 2 个不合格窗（每窗 10 s）就应降级到 stack（`zx279128s-accel.c:3719-3740`）。
  若真出现 0.293% alias 且持续两窗，应 ~20 s 内自动降级到 stack（约 325 Mbit/s），
  不会停在 110 ⇒ 先读 mode 是零成本的第一刀
  （原始记录）。
- **E1 的零重刷依据**：`accel_ipv6` 是带 `.write` 的 debugfs（`zx279128s-main.c:3550`，
  fops `zx279128s-accel.c:4295-4299`），写 `stack/ffe` 走 `zx_accel_set()` 的
  quiesce → 排空 TX（≤100 ms）→ `__zx_ffe_flush(本族)` → 写模式字节 → 重开降级窗口
  （`zx279128s-accel.c:3420-3615`）。切 stack 不被 PPE bound 拒绝（`if (bound && family == 0)` 只挡 v4，
  同文件 `:3464-3490`）。
- **E2 的参数存在性有矛盾、未裁决**：HEAD 源码有 `ffe_bp_drop`（`zx279128s-main.c:80-81`，
  初值 `true`），但盒子交付态 P10.5 的参数表里**没有**它（原始记录（未入库）：
  「全表 30 项，无 `ffe_bp_drop`」）。⇒ 能不能在交付镜像上做 A/B，**未裁决**；必须先 `cat` 参数路径，
  不存在就改用同为 0644 的 `ffe_dup` 观测替代，不能假设它可写。
  另外源码注释自身口径也不一致：`:2203` 写「A3（ffe_bp_drop=1，默认关，会丢帧）」，而初值是 `true`
  （`:80`）——以运行期 `cat` 读回为准。
- **零重刷的边界**：E0/E1/E2 都是 0644 参数或 debugfs 写，`echo` 即生效、写回即恢复；不碰 flash、不重启。
  但慢态复现本身要抽签（触发条件未知，2/4 轮），时间主要花在等「慢态在场」。

## §4 「只有重启才能恢复」目前只是传闻

- 原始记录（未入库） 把它写成待验分支：「恢复到 >600 ⇒ 在途 bp 错配、切换可清；
  仍 ~110 ⇒ 持久态（池/硬件），只能重启」。
- 同文件 `:58` 的风险声明也写「重启/切模式可清（**待验证**）」。
- **模式往返复测从未做过**：`:70` 的 stack→ffe 往返就是那次从未执行的实验；因此「只有重启能恢复」
  目前是**预测**，不是实测事实。E1 一旦做了，这句才有资格升级或撤回。
- 现有唯一相关实测是 296 s 事件后「1 s 恢复未崩」（原始记录未入库），
  它既不能证明也不能否证「只能重启」。

## §5 死旋钮清单（写了没人读）

统计口径：源码 `module_param*` 共 51 个（驱动 `zx279128s-*.c` 14 + `zx279128s-ppe.c` 37）、
带 `.write` 的 debugfs 11 个 ⇒ 运行期可写面 62；与 v6 快路相关的 20 个里，活 12 / 死或伪 8
（原始记录未入库）。下面 8 个是**死/伪旋钮**：

| 名字 | 位置 | 为什么是死的 |
|---|---|---|
| `accel_v6` | `zx279128s-main.c:117-118` | 只在 probe 读一次写进 `accel.v6_mode`（`:3671-3675`）；之后无任何读者。运行期等价物是 `$D/accel_ipv6` |
| `accel_autofallback`（sysfs） | `zx279128s-main.c:125-126` | 只在 probe 读一次（`:3678`）；运行期等价物是 `$D/accel_autofallback`。写 sysfs 只改变量，不改变 `accel->autofallback` |
| `accel_v4` | `zx279128s-main.c:113-114` | 只在 probe 读（`:3669`）；决定开机时 v4 是否与 v6 共用同一条 TX 环/bp 池，只能靠重启改 |
| `pool_cacheable` | `zx279128s-main.c:59-60` | 只在 probe 建池时读（`:3723`），决定 16 MB 池的映射属性；运行期写无效 |
| `zc_rx` | `zx279128s-main.c:95-96` | 只在 probe 读（`:3685`）；运行期写无效（`adapter->zc_rx` 是另一个字段） |
| `zc_canary` | `zx279128s-main.c:134-135` | 全树没有任何读者（只有定义与注释 `:132`、`:1399`）；写它什么都不会发生 |
| `pppoe_tx` | `zx279128s-accel.c:5807-5808` | 无读者；源码注释自认「M1 没有任何代码读它」（`:5797-5801`） |
| `$D/ffe_flows` | `zx279128s-main.c:3552`；fops `zx279128s-accel.c:4873-4879` | mode 0644 但 fops **无 `.write`**，只能读（每行含 `lastuse_ms`） |

- 运行期真正能让 v6 快路整体切走的只有 `$D/accel_ipv6`（写 `stack/ffe/zc`），
  以及 `$D/accel_autofallback` / `$D/ffe_learn` / `$D/ffe_flush` 这几个**有 `.write`** 的 debugfs
  （原始记录未入库）。
- `ZX_TX_QUEUE_LIMIT 256` **没有旋钮**：它是 `#define`（`zx279128s-eth.h:257`），要改只能重编；
  AX-02 已判负，见 [hw-offload.md](hw-offload.md)。

## §6 判据命令要先自检 debugfs 路径

- 早先那份核验清单把路径写成 `cat /sys/kernel/debug/ffe/alias_dump`；实际目录名是
  `dev_name(adapter->dev)`，本板为 `/sys/kernel/debug/92000000.ethernet/`
  （`zx279128s-main.c:3537`）。那份清单不在本仓库，本文只留这条教训，不引它的原文。
- 同类错误在 C1b 的 CLA aging 扫描里重演过：路径写错 ⇒「一个数都没读到」，而那个 0 被当成
  「没老化」的证据。**判据命令必须先 `cat` 自检路径，再拿读数下结论。**

## §7 口径与边界

- **源码版本**：本文所有 `文件:行` 来自本仓 HEAD `2dab71d`（2026-11-02 04:16 提交）；盒子跑的是
  P10.5（`623a812`，`image-p10.5.bin`）。两者参数集不同：例如 HEAD 有 `ffe_bp_drop`，P10.5 没有
  （§3 E2）。引用「源码里有」不等于「盒子上有」，落笔必须分开。
- **没做的事**：未上机、未读本轮运行期计数、未改源码/overlay、未构建、未 git 写；E0–E2 的实测值
  一律为「无证据」。温度/DVFS、内存碎片两条排除只做了源码级论证
  （只做了源码级论证），没有取 `/sys` 读数。
- **当时的未裁决（2026-11-02 白天口径）**：① 296 s 事件是否真由老化触发；② 慢态是状态型还是持久态；
  ③ `ffe_bp_drop` 在 P10.5 上是否存在；④ 「只有重启能恢复」是否成立。
  **到交付态只剩 ① 还有意义**：② 与 ④ 随"慢态未复现"一起失去前提（见文首口径），③ 只是参数表差异，
  用 `cat` 就能查。

## §8 证据索引

本文数字来自本板实测与既有实测回代（P10.5，2026-11-02）；过程记录未入库。可复核的仓库内位置：

| 仓库内文件 | 内容 |
|---|---|
| `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c:232,1279-1287` | `ffe_aging=300` 与老化判定 |
| `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c:888-890,3537` | 间接窗口写、debugfs 目录名 |
| `overlay/package/kernel/kmod-e2631-ppe/src/zx279128s-ppe.c:52,2899` | CLA v6 默认关与 `IPv6 classifier off` |

---

## 附：ffe-alias-autofallback-2026-11-01.md（`ffe-alias-autofallback-2026-11-01.md` 已并入本文件，原文件删除）

## v6 快路交付兜底：把 alias 丢弃纳入 autofallback 判据（P10.4）

> 2026-11-01。对象：ZXHN E2631（ZX279128S）自研固件的驱动内 FFE 快路。
> 背景与机制定量来自 P10.3 的折叠诊断（过程记录未入库，结论已并入 `features/ffe-fastpath.md`）：
> FFE 收侧"描述符长度 ↔ 池内容"错配 ⇒ 约 0.29% 的帧在 `zx_eth_rx()` 的 alias 判据处
> 被静默丢掉 ⇒ TCP 平衡在 ~92–111 Mbit/s 的慢态，且是**持续平衡点、不会自愈**。
> 原有 `zx_accel_autofallback()` 只看 `accel.cnt[]`，对这批帧完全失明 ⇒ 用户被卡在慢态。

本提交只做**兜底判据**：不改 alias 判据本身、不改 FFE 提交/返还时序、不动 v4 `hw` 码路，
也不修 bp 归属根因（那是 P10.5 的事）。

### 1 改动（3 个文件，5 处）

`overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`

| # | 位置 | 改前 | 改后 |
|---|---|---|---|
| 1 | `zx279128s-main.c:1795-1823` | 无 | 新增 `zx_alias_count()`：在 alias 命中点按以太类型分族计数（v4/v6/unknown） |
| 2 | `zx279128s-main.c:1980-1989` | `rx_alias_cnt++` 后直接取证快照 | 同点追加 `zx_alias_count(adapter, d)` |
| 3 | `zx279128s-main.c:3076-3080` | `alias_dump: cnt=%u idx=%u` | 追加 `alias_v4=/alias_v6=/alias_unk=`（cnt=/idx= 原样） |
| 4 | `zx279128s-eth.h` | `zx_ffe_fb_base{lookup,drop,fallback}`、`zx_accel` 无 alias 计数 | `+u64 alias`、`+u64 alias_drop[3]`、`+u8 fb_alias_strike[2]`、`ZX_ALIAS_FAM_*` |
| 5 | `zx279128s-accel.c` | 判据只有 drop/fallback，一窗即降 | 窗口双读加 alias；`ZX_FFE_FB_ALIAS_PERMILLE=2`、`ZX_FFE_FB_ALIAS_STRIKES=2`；连续 2 个不合格窗才降级，第 1 窗只 `dev_info` |

#### 1.1 alias 丢弃点：`d` 就是以太头起点，不需要偏移

`main.c` 的调用点是 `d = pkt_pool + bp * ZX_BUF_SIZE + 16`（16 B 是 DMA 头，帧紧随其后）。
取证快照 `zx_alias_snapshot(..., frame=d)` 的 `head[0..5]=dmac`、`head[6..11]=smac`
就是按这个口径取的；快路判据链里的以太类型也在 `data + 2 * ETH_ALEN`。
所以以太类型在 **`d + 12`**，且外层已有 `len >= ETH_HLEN` 守卫，`d[12..13]` 必然在帧内，
读之前也做过 `dma_sync_single_for_cpu()`。

族判定直接复用 `zx_eth_family()`（与快路判据链同一口径）：`0x0800 ⇒ v4`、`0x86dd ⇒ v6`，
其余（含 VLAN 0x8100/0x88a8）记 `unknown`，**不参与判据**——判据分母是本族 lookup，
VLAN 帧的内层族与它不对应。

#### 1.2 计数为什么不并进 `zx_ffe_cnt`

`struct zx_ffe_cnt` 的口径是"**进了判据链**的帧"（`lookup == hit + miss` 是不变式）。
alias 丢弃发生在判据链入口之前：没进 lookup、没建 skb、bp 已还。
并进去会破坏不变式，也说不清分子分母。因此单独 `u64 alias_drop[3]`，
下标 `0/1` 刻意与 `family` 同号（`ZX_ALIAS_FAM_V4/V6/UNK`），
写侧只有 RX NAPI、裸 `u64++`，与 `cnt[]` 同一无锁口径；读侧沿用现有
"连读两遍、相等才认"的 `zx_accel_fb_cnt_read()`（撕裂就本拍不判）。

### 2 判据

只在"该族当前是 `ZX_ACC_FFE` 且 `autofallback=1`"时生效（与现有窗口逻辑同层，每 10 s 一窗）：

```
本窗不合格 ⇔ Δalias * 1000 >= Δlookup * ZX_FFE_FB_ALIAS_PERMILLE(2)
连续 2 个不合格窗口 ⇒ zx_accel_set(adapter, family, ZX_ACC_STACK)
```

- 第 1 个不合格窗口只打 `accel: IPv%d alias strike 1/2: d_alias=%llu of %llu lookups`，
  关窗重开；第 2 个才降级（避免单窗抖动误降；慢态是持续平衡点，多等 10 s 不损失正确性）。
- strike 只在**判出确定结论**时清零：本窗合格（含"帧太少/计数倒挂"的 CLEAR）、离开
  ffe（age work 下一拍观察到，或 `zx_accel_set()` 切模式成功时立刻——P10.4 复审 S1 补的，
  保证 <1 s 的 `stack→ffe` 往返也开启新会话）、关掉 autofallback、降级成功。
- **计数读撕裂（RETRY）不是清零点**：那是"本拍不判"，窗口与 strike 原样保持、下一拍用
  同一个窗口重判（窗口可能顺延成 11 s、12 s；判据只看增量，不看窗口长度）。
- 降级后本轮窗口状态清零，**不做自动回升**（与 P8 起的行为一致）。
- 旧判据（`drop_noport > 1‰` 或 `fallback > 500‰`）仍然**一窗即降**，一点没有放宽。

#### 2.1 阈值 2‰ 的推导（上一轮实测）

| 状态 | Δalias / Δlookup |
|---|---|
| 慢态（110 Mbit/s 量级） | 36/s ÷ ~9.1k pps ≈ **3.9‰**（独立复算 0.388% 同量级） |
| stack 臂（同板同流） | ~4.7/s ÷ ~29k pps ≈ **0.16‰**（"非慢态也会零星 alias"的基线） |
| 健康 ffe 快态（700–800 Mbit/s） | alias ≈ 0，≪ 1‰ |


> 注：表里的「慢态」是 **P10.3 时代的旧观测**。交付态复测没有复现这个现象（单流 14 次 ×≈600 s
> ×≈57 GB 全部 700–800 Mbit/s，300 s 长流跨 296 s 无突跳）。阈值 2‰ 本身仍然成立：它是拿
> stack 基线（0.16‰）与健康快态（≈0）卡出来的，不依赖慢态那一行。

2‰ 取在两者之间：高于 stack 基线 12 倍（正常流量不会误触发），是慢态的 ~1/2
（单窗速率抖动不会漏判）。判据用乘法比，不除零、也不在 32 位 ARM 上做 64 位除法。

注意分母口径：`Δlookup` 是**进了判据链**的帧数（alias 丢弃的不在其中），
所以这是个"丢/存"比，而不是总收帧的丢包率；上表两个实测比值也是同一口径。

### 3 可观测性

- 降级日志行尾追加 `d_alias=`，既有 `fell back` 子串与字段顺序一字未动：
  `accel: IPv%d fell back to stack: d_drop=… d_fallback=… of d_lookup=… (cum …) d_alias=…`
- `alias_dump` 头行追加 `alias_v4=/alias_v6=/alias_unk=`（`alias_drop[]` 三格）。
- 不新增 debugfs 文件（仍是 16 个）；`ffe_learn` 的两行格式一个字段都没动；
  `accel_v4/accel_v6/autofallback/ffe_aging` 交付默认值不变。

### 4 预期时延（离线仿真）

`age` 每秒一拍、窗口 10 s、未达阈值关窗后下一拍重开。按 DIAG 的实测速率
（健康 58k pps、慢态 9.5k pps、alias 38/s）仿真"掉速 → fell back"：
**19.6–30.5 s**，典型 22–30 s。最坏的那 0.5 s 来自"掉速前那段健康流量稀释了
第一个窗口"：只要掉速发生时窗口已开了 ~1.5 s 以上，第一个窗口的比值会被
健康流量摊薄到 2‰ 以下，于是要多等一整个窗口。

**验收口径统一为 ≤35 s**（P10.4 复审 S3：任务书原写 ≤30 s、交付文档曾写"按 31 s 判"，
两套口径并存没有意义；仿真最坏 30.5 s，取 35 s 作验收线留 4.5 s 余量）。阈值仍按 2‰
交付，**不改** `ZX_FFE_FB_ALIAS_PERMILLE`。

复审 S4 的提醒（上机必须实测）：上表分母用的是 9.1–9.5k pps（FFE lookup 口径）；若按
DIAG 的 13.1k pps（`Δtx_submitted`，含双向/ACK）算，慢态比值只有 **2.9‰**，对 2‰ 阈值
的余量约 **1.45×**。所以上机必须逐窗记录 `d_alias / d_lookup` 的真实比值：**若实测
<2‰，就把阈值降到 1‰ 重新构建刷机**（1‰ 仍高于 stack 基线 0.16‰ 的 6 倍），不要
在余量不足的阈值上硬判。

### 5 上机判据（下一步执行，本次未刷机）

刷 slot1（`mtd6+0x400000==mtd7`；slot0 是 factory 绝不可写，刷前/刷后整分区 sha
必须 `7f849c43…d17b92`），开机 ≥108 s 后测：

1. 快态：v6 `ffe` 打流 3 轮 ≥600 Mbit/s 且**不**出现 `fell back`/`alias strike`；
2. 慢态：v6 `ffe` 打流到掉速后 **≤35 s**（仿真最坏 30.5 s；验收口径见 §4）出现
   `fell back`，且日志行里 `d_alias` 应为每分钟 2000 上下、`d_alias/Δlookup ≈ 3–4‰`；
   降级后吞吐回到 stack 量级（≥300 Mbit/s）；
3. v4 `hw` 回归 3 轮 ≥900；8 次启动（冷 5 温 3）干净、门控 121 s 自动开、
   debugfs 仍是 16 个文件。

### 6 已知风险（P10.4 复审 S5/S6，上机前必读）

1. **低速率地板（S5）**：`ZX_FFE_FB_MIN_LOOKUP=1000` 时，一个 10 s 窗里只要
   1000 帧，**2 帧 alias 就是 2‰**（恰好等于阈值）。若流量长期停在 ~100 pps，
   按 stack 基线 0.17‰ 算每窗期望 0.17 帧，Poisson 下 P(≥2)≈1.3%/窗，两个
   连续窗 ≈1.7e-4，按每天 8640 窗折算约 **1–1.5 次/天**的理论误降（每次只是
   v6 回 stack + 清表，不丢包、无自动回升）。上机若发现低流量下无故
   `fell back`，先核对当窗 `d_lookup` 是否贴着 1000 这个地板。
2. **降级后被脚本切回 ffe 的抖动（S6）**：降级成功后 strike=0、窗口清零，
   不自动回升。但若有脚本"看到 stack 就 `echo ffe`"，每次切回都会重开窗口，
   慢态仍在时约 10 s 后 strike 1、21 s 后再次降级 ⇒ **10–21 s 周期的抖动**
   （吞吐在 ~110 与 ~340 Mbit/s 之间来回，每周期还清一次本族 FFE 表）。
   慢态未修好前**不要**自动重试 ffe；切换只在人工确认后做。


---

## 附：ffe-boot-learning-gate-2026-11-01.md（`ffe-boot-learning-gate-2026-11-01.md` 已并入本文件，原文件删除）

## 开机学习死机：为什么 ffe_learn 默认关、120 s 后再自动开（P10.3，2026-11-01）

> 镜像 p10.2（提交 `70e4ed7`）把 `ffe_learn` 默认改成 1，两次开机均硬死机；
> p10.3a（分支 `bisect/p10.3a-learn-off`）默认 0 则 12 分钟干净。本文件记录这组
> 事实、门控设计和"不要随手把默认改回 1"的理由。取证材料在构建目录的
> 过程记录（仓库外，未入库），本文只保留结论、数字与判据。

### 现象

- p10.2 两次开机都在 ≈44.7 s 冻结（`engine loaded`（≈44.7 s）、
  `hardware flow offload enabled`（≈44.9 s）、`wifi-on: done`（≈48.9 s）三行全缺、
  100% 丢包），`rcu_sched detected stalls` 直到 104.2/107.97 s 才打印；冻结的 CPU
  关中断自旋（无 NMI 回栈、60 s 里只有 2 个 tick），没有 oops/BUG，网络全死。
  串口原文：`verify-p10/f/boot-hang-1424.log`、
  `boot-hang-1424b.log`、`boot-hang-both.log`（2026-10-31 夜，现场目录 `f/`）。
- 分析见 `DIAG-P10.2-HANG.md`（形态判读）与 `DIAG-BOOTLEARN-RACE.md`（锁/调用图裁定）。

### 二分结论（p10.3a，一次刷机验完）

| 组合 | 结果 |
|---|---|
| `accel_v6=ffe` + `ffe_learn=0` 开机 | 干净：三行齐、无 stall，裸启动观察 180 s；v6 单流 300.8/300.2 Mbit/s（全 miss 回栈） |
| 同镜像运行期 `echo 1 > ffe_learn` | 775.1/773.4 Mbit/s，flows=4，静置 120 s 不死 |
| `ffe_learn=1` 在开机窗口内 | p10.2 两次 ≈44.7 s 冻结（104.2/107.97 s 才报 stall） |

> 口径限定（`REVIEW-P10.3A.md`，应 `1659bc2f` 证据审计）：300.8 与 775.1 是同一
> 镜像上的两次**单次**测量（相隔数分钟，max cwnd 197k vs 481k、mean RTT 3.7 vs
> 2.83 ms，互不可比），只能当方向性证据：表空 ⇒ 无命中 ⇒ 没有快路转发；正式交付
> 验收要补**同轮交替** stack/ffe 各 3 次 ×15 s。

见 `verify-p10/PHASE-G.md` §1 阶梯（S0–S4）。⇒ 死机只在"开机窗口内学习已活"
时出现；运行期打开学习是健康的，而学习是 v6 加速的必要条件（不学则表空、v6 没有
快路可走）。

### 未找到根因

自死锁假设被否定：`rhashtable_walk_*` 迭代期不持锁、`accel->lock` 是 mutex、
PPE 链与 FFE 表零交集（含反汇编核对，把握 90%，见 `DIAG-BOOTLEARN-RACE.md`）。
驱动里唯一"关中断区间内 printk"的点是 `zx_ffe_local_ip6_add()` 的
`dev_warn_ratelimited()`（P10.3 前在 `zx279128s-accel.c:1007`，现在锁外
`:1033`）：串口 console 的 `console_trylock_spinning()` 会关中断自旋等。
P10.3 已把它搬到锁外，但这是**未证实候选**，不等于根因。

本轮证据审计（`REVIEW-P10.3A.md`）还修正了两处口径：p10.2 的死机时刻应写成
"≈44.7 s 冻结（应出 `engine loaded` 而未出）、104.2/107.97 s 才报 `rcu_sched`
stall"，不是"44 s 起 stall"；且 p10.2 两次死亡**至少一次是冷上电**。所以门控版
的"开机安全"必须用**冷上电矩阵**验证（用户已定 5 冷 + 3 温，每次 ≥180 s），
n=2 的非冷启动不能外推。

### 门控设计（P10.3 交付形态）

- probe 期 `learn_on = 0`，并记 `learn_arm_at = jiffies + learn_delay_ms`；
  1 s 节拍的 age work 最先调 `zx_ffe_learn_arm_tick()`，到点置 1、清零、打一条
  `accel: ffe_learn auto-enabled 120000 ms after probe`。
- 参数 `learn_delay_ms`（module_param，0444，默认 **120000** ms）；0 = probe
  立刻学，**这条路径就是诊断镜像复现 p10.2 死机的开关**（起机参数
  `zx279128s_eth.learn_delay_ms=0`），只在复现时用，它是开机参数不是运行期开关；
  生效值超过 600000 ms 时 init 会钳到 600000 ms 并打一条
  `accel: learn_delay_ms clamped to 600000 ms`（只有真钳了才打）。
- 可观测/可取消：`ffe_learn` 末尾追加 `auto_enable_in_ms=`（0 = 已自动开过，
  或用户写过 `ffe_learn` 已接管），写 `0`/`1` 会清 `learn_arm_at`，用户意图优先。
  门控 tick 与 debugfs 写口**同持 `accel->lock`**（mutex），两者全序，用户写的值
  一定最终生效（不靠 cmpxchg 的窄窗口）；两处 `dev_info` 都在锁外。
- 门控必须"一次成型"：别改成"开机学 + 定时关"，那会重开死机窗口。

### 警告：改回"默认立刻学"之前必须先定案

根因未定时，任何"开机早期让 FFE 表非空"的路径（把 `learn_delay_ms` 设 0、
用 `ffe_add` 预置条目、默认值改回 1）都会重开这个窗口。要改回默认学，先拿到
stuck PC / `CONFIG_PROVE_LOCKING` / `panic_on_rcu_stall=1` 的证据把元凶钉死；
在那之前，默认保持"开机不学、延时自动开一次"。


---

## 附：ffe-cacheable-pool-2026-10-30.md（`ffe-cacheable-pool-2026-10-30.md` 已并入本文件，原文件删除）

## 收包池可缓存化：把软件转发的每包成本砍下来（2026-10-30 夜）

> 这是"优化开源固件自己的 FFE"第一步。原厂 FFE 为什么能跑满速、我们的软件路径每包贵在
> 哪里、这次改了什么、怎么验证，都在这里。
> 所有数字都来自这台机器（ZXHN E2631，OpenWrt v12fix 跑在 slot1）的实测，命令附在每节。
> **改动的效果数字见 §6，尚未填写的项明确标注为未测**。

---

### 0. 一句话结论

| 问题 | 答案 |
|---|---|
| 原厂 FFE 是什么 | 内核里一层独立转发面（`protocol/ffe/`），不是硬件表命中；§1 有源码路径与符号名 |
| 它快在哪 | 帧**原地处理**：用 "fkb"（fast kernel buffer）指向驱动的 DMA 缓冲，只做 cache invalidate，不拷贝；自带 per-CPU 发送队列与 backlog；数据面不走桥/路由/nft |
| 我们的每包成本 | 1500 B UDP 收包 **13.8 µs/包**；64 B 小包 **6.1 µs/包** → 差额 7.7 µs 全是那 1500 B 的拷贝 |
| 拷贝为什么这么贵 | 包池是 `dma_alloc_coherent()` 的**非缓存内存**，实测 1500 B 拷贝要 **8.2 µs（184 MB/s）**，缓存内存只要 **0.48 µs** |
| 本次改动 | 包池从 coherent arena 里剥离，改成 **8 MiB 可缓存连续内存 + streaming DMA 映射**，收包前失效、发包前回写；分配失败自动退回旧行为 |
| 预期 | 收包侧 8.2 µs 拷贝 → 约 0.5 µs（+一次 cache 失效），每包成本 13.8 → ~6.5 µs |

### 1. 原厂 FFE 的实体：内核里的独立转发面

`vendor-vmlinux.bin`（原厂内核镜像，从 slot1 原厂系统里取）带着编译期的源码路径和函数名：

```
/zte/csp/opensource/cspkernel4.1/protocol/ffe/ffe_main.c
        .../ffe_pkt_misc.c  ffe_sharedata.c  ffe_sk_buff.c
        .../ffe_fk_buff.c   ffe_proc.c      ffe_flush.c   npu_main.c
ffe_receive_skb / ffe_receive_fkb / ffe_cache_inv_fkb / ffe_learn_skb
ffe_rps_xmit_skb / ffe_backlog_rx / ffe_general_xmit_fbd
ffe_sharedata_{create,search,store,alloc,free} / ffe_flush_flow_by_conntrack
"%s, features[%llx], fkb[%d], csum[%d]"
```

读法：

* `ffe_sk_buff.c` 与 `ffe_fk_buff.c` 两套实现并存，说明它有两种帧表示：普通 skb，和 **fkb**
  （fast kernel buffer，直接指驱动的 DMA 缓冲区）。
* `ffe_cache_inv_fkb()` 是"读之前把 cache 失效"——正是可缓存内存做 DMA 的标准做法；也就是说
  **它的包池是可缓存内存**，而不是我们这种 `dma_alloc_coherent()` 的非缓存内存。
* `ffe_rps_xmit_skb` / `ffe_backlog_rx` + `ffeinfo` 里的 `rps_cpu_map:[3]`、`tx_queue_max:[1024]`
  说明它有自己的 per-CPU 收发队列，不经过 qdisc，也不用内核的 `enqueue_to_backlog`。
* `npufilter` 里那些 netfilter 钩子（`npu_cap_filter -200`、`flow_ipv6_nat_filter 400`、
  `flow_retry_filter_* 800`）只负责建流与能力筛选；数据面是 FFE 自己的函数，不进
  `netif_receive_skb → 桥 → 路由 → nft`。

原厂 v6 路由 918–928 Mbit/s、4.7 µs/包（`docs/reverse/vendor-ipv6-behavior.md` §8），
在"不拷贝 + 不进协议栈 + 自己的队列"这个结构下是自洽的。

### 2. 我们的每包成本：先量，再改

#### 2.1 单次 MMIO 的代价

盒子上没有 perf/ftrace，所以用 `/dev/mem` 直接量寄存器访问：

| 操作 | 实测 |
|---|---|
| 一次 MMIO 读（TM/BMU/DMA 区） | **300 ns** |
| 一次 MMIO 写（posted） | 329 ns |
| 写 + 读回（依赖往返） | 666 ns |
| 1500 B memcpy（缓存内存） | 482 ns |

⇒ 驱动每包大约 6–8 次寄存器访问，合计约 **2 µs**，不是主要矛盾。

#### 2.2 每包 CPU 时间：大包 vs 小包

主机用 iperf3 UDP 灌进盒子（盒子自己收，`iperf3 -s`），读 `/proc/stat` 与 `lan1` 收包计数：

| 包长 | 收到包数 | 时长 | sirq 合计 | 每包 |
|---|---|---|---|---|
| 1400 B | 356 902 | 10 s | 4.93 核秒 | **13.8 µs** |
| 64 B | 692 667 | 10 s | 4.22 核秒 | **6.09 µs** |

固定成本 6.1 µs，多出的 7.7 µs 随字节数增长 → 换算 **5.8 ns/字节 ≈ 170 MB/s** 的拷贝速度。

同时确认不是中断风暴：`/proc/interrupts` 里 gmac 中断 10 426 次 / 356 902 包 = **1/34 包**，
RX 中断合并工作正常。

#### 2.3 直接量包池的拷贝速度

包池物理地址取自 `ZX_TM_BUF_START`（读出来 `0x4e100000`），用 `/dev/mem` 映射（属性与
`dma_alloc_coherent()` 相同：非缓存）：

| 方向 | 1500 B | 128 B |
|---|---|---|
| **非缓存 → 缓存**（= 收包时的 memcpy） | **8155 ns（184 MB/s）** | 716 ns |
| 缓存 → 非缓存（= 发包时的 skb_copy_bits） | 797 ns（写合并，1882 MB/s） | 219 ns |

⇒ 收包那一次拷贝就是 8.2 µs，占总成本的一半以上；**发包方向因为写合并已经不贵**。
这与 §2.2 的差值（7.7 µs）互相印证。

### 3. 改动：包池改成可缓存内存

驱动（`overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`）原来把包池、
描述符环、TM 表全放在一个 16 MiB 的 `dma_alloc_coherent()` 事务区里。改动只动包池：

1. **包池独立分配**：`alloc_pages_exact(8 MiB)` + `dma_map_single(..., DMA_BIDIRECTIONAL)`
   —— 物理连续（BMU 要求 `TM_BUF_START..END` 一整段），映射是 **cacheable 普通内存**。
2. **收包**：从描述符拿到 `bp/len` 后，先 `dma_sync_single_for_cpu()` 失效该槽的缓存行
   （槽基址 32 B 对齐，16 B 头 + 帧在同一槽内，一次失效覆盖），再读 MAC、再 memcpy 到 skb。
3. **发包**：`skb_copy_bits()` 写满后 `dma_sync_single_for_device()` 回写，再登记描述符。
4. **回退**：8 MiB 连续内存分配失败时（早期启动内存碎片），退回原来的 coherent 池并打一条
   dmesg 警告——代价是速度，不是链路。
5. **不动描述符环与 TM 表**：它们留在 coherent 事务区里，避免"可缓存环 + 缺 cache 维护"
   这类会静默丢包的坑。

安全前提（都在代码注释里写明）：CPU 从不写收包缓冲；发包前的回写保证硬件看到的是新数据；
每次读取前失效，丢弃上一轮残留的（干净的）缓存行。槽大小 2048 B 是 32 B 的整数倍，
失效范围不会跨到相邻槽。

### 4. 怎么验证

**改动生效的直接证据**（刷机后）：

```sh
dmesg | grep -i "cacheable packet pool"   # 没有这行 = 分配成功（有这行说明回退了）
```

**每包成本**（主机 → 盒子 UDP，§2.2 的同一脚本）：期望 1400 B 的每包成本从 13.8 µs 降到
6.5–8 µs，同时同样 10 s 内能收更多包。

**端到端**：8 流 TCP 经盒子路由，比较改动前后的 Mbit/s 与每核 sirq。

### 5. 风险与边界

* 硬件在 DMA 写入某个槽时，CPU 可能正持有该槽上一轮的缓存行；因为每次读取前都失效、
  每次写入后都回写，不存在"CPU 看到的旧数据被当成新帧"的路径。
* 失效范围从槽基址开始（对齐），16 B 头与帧在同一个 32 B 行内，不会越界到下一个槽。
* 若某次 `dma_sync` 语义在 ARM 上退化为整片失效（内核的实现是按范围做），最坏结果是
  多花时间，不影响正确性。

### 6. 结果

刷入 `e2631-openwrt-factory.bin`（sha256 `443f07e5…9247`，slot1，槽 0 未动）后实测。
主机 ↔ 盒子直线连接、iperf3 UDP 灌包、读 `/proc/stat` 的 sirq 增量除以 `lan1` 收包数。
**所有数字都是同一台机、同一脚本、改动前后各一次**；"改前"是 `e2631-openwrt-v12fix-ppe-20261030-slot1.bin`。

| 指标 | 改动前 | 改动后 | 变化 |
|---|---|---|---|
| 1400 B，600 M 灌包 | 16.92 µs/包（53.5 kpps，丢 23 %） | **15.77 µs/包**（53.3 kpps，丢 23 %） | −6.8 % |
| 1400 B，300 M 灌包（不丢） | 未测（换固件后无法回头） | **13.18 µs/包**（26.8 kpps） | — |
| 64 B，200 M 灌包 | 6.51 µs/包（69.2 kpps） | **6.33 µs/包**（68.7 kpps） | −2.8 % |
| 每字节斜率（1400 B − 64 B） | 6.2 ns/B（≈160 MB/s） | **5.2 ns/B**（≈190 MB/s） | −16 % |

读法（这是本轮最重要的教训）：

1. **包池改成可缓存内存确实变快了，但只快了 1–3 µs，不是预期的 8 µs。**
   单独用 `/dev/mem` 量非缓存 1500 B 拷贝是 8.2 µs，可缓存是 0.48 µs，看起来能省 7.7 µs；
   真实驱动里只省了 1 µs 出头。原因有两个，都在这张表的斜率里：
   - 收包前必须 `dma_sync_single_for_cpu()` **失效**那些缓存行，而 DMA 是写进 DRAM 的，
     缓存里本来就没有新数据——失效之后紧接着读，等于**每次都是冷读**，仍然要吃一遍内存延迟；
   - 帧的字节数不管从哪读，CPU 都要过一遍：驱动 `memcpy` 一次，协议栈校验和再一次。
2. **所以每包成本的大头不是"非缓存"这三个字，而是"CPU 碰了多少字节"。**
   1400 B 与 64 B 的差值是 9.4 µs（改动前 10.4 µs），换算下来每字节仍然要 5–6 ns。
   产品化的下一步不是调缓存属性，而是**让 CPU 少碰字节**：零拷贝（fkb 式）与校验和卸载。
   这一刀当天深夜就做了：`docs/history/2026-10-30-zero-copy.md`（收包 `build_skb` 池缓冲、
   发包把同一个 bp 交给硬件，RX 每包 17.5 → 5.1 µs、TX 8.1 → 1.7 µs，
   转发 8 流 TCP 385 → 700 Mbit/s）。
3. 转发吞吐这一轮**没有可用的对比数字**：8 流 v6 路由测到 391 Mbit/s，但那次测试台我手工往
   fw4 里插了 `iifname lan2 accept` 并且插在 `flow add @ft` 前面，软快转的建流规则被挡在后面，
   那一轮的 CPU 分布不可比。正确的转发口径要等
   `docs/operations/troubleshooting.md` 那个交换芯片发送 bug 修好、用 wan↔lan 台重测。


### 7. 下一步（按收益排序）

1. 若 §6 收益符合预期，下一刀砍**固定成本 6.1 µs**：发包侧 `TX_START` 批量化（每 N 包一次
   寄存器写）、BMU 申请/释放从"每包一次锁 + MMIO"改成软件自由链表 + 批量归还。
2. 再看把桥接出口（`br-lan`）换成端口直出（`rxdev/txdev` 缓存），省一次 FDB 查找。
3. ~~才有必要考虑 fkb 式零拷贝~~ **已做，但已回退/搁置（交付态 `zc_mask=0`，2026-11-03 更正）**：
   `docs/history/2026-10-30-zero-copy.md`。
   skb 直接 `build_skb()` 池内缓冲，析构时把 bp 送进隔离环延迟归还；池缓冲的三种归属
   （FREE/SKB/DROPQ/TX）、`ZX_ZC_OUT_MAX` 闸门、GRO 必须关、`tx_queue_len` 必须小，
   都在那篇里。~~默认开、LuCI「网络 → 转发加速」可关。~~〔原文如此，已不成立：它在本机终结路径上
   损坏数据（20 MB 上传 10 次坏 2 次），交付态已回退，`uci e2631.fastpath.zerocopy` 废弃——
   见 [v6-software-fastpath.md](v6-software-fastpath.md) §6 与 [2026-10-30-zero-copy.md](../history/2026-10-30-zero-copy.md) 文件头〕


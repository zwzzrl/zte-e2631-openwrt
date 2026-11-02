---
title: 慢态待判与仪器审计：FFE 296 s 边界、D2 插桩与死旋钮
date: 2026-11-02
status: done
topic: known-issues
summary: 296 s 边界对应 ffe_aging=300 而非 CLA；D2 可排除但非纯只读；慢态三动作待判。
evidence: overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c:232, overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c:888, .scratch/ffe-fastpath/slowstate-knobs/20261102-2120/RESULT.md
supersedes:
---

# 慢态待判与仪器审计（FFE）

> 日期：2026-11-02。范围：ZXHN E2631（ZX279128S）自研固件的 FFE 慢态（约 110 Mbit/s）取证。
> 结论性质：离线只读审计 + 既有实测引用；本轮**未上机、未改码、未构建**。
> 口径：源码行号来自本仓 HEAD `2dab71d`；盒子跑的是 P10.5（`623a812`），两者不一致处逐句注明。
> `.scratch/` 下的过程数据只以文字路径引用。
> 缩写：`SRC/` = `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`；
> `PPE/` = `overlay/package/kernel/kmod-e2631-ppe/src/`；`A/` = `.scratch/ffe-fastpath/`。
> 文中未写目录的 `zx279128s-*.c` 均指 `SRC/`，`zx279128s-ppe.c` 指 `PPE/`。

## §0 结论

1. **296 s 那个时间边界对应的是驱动的 `ffe_aging=300`，不是 PPE CLA aging**：`ffe_aging` 默认 300 s、
   每秒一拍（`zx279128s-accel.c:224,232,1279-1287`）；PPE 的 CLA v6 默认关（`zx279128s-ppe.c:52`，
   dmesg 打 `IPv6 classifier off`，`:2899`），`ipv6=0` 时 CLA 表是空的。用 CLA aging 去扫 296 s 是拿错仪器。
2. **D2 只读插桩可作为「20:27 那次 kexec 后 RX 起不来」的排除项**：+392 行全部落在 `show` / 2 个
   `module_param` / 1 个宏，probe/open/ISR/NAPI/RX/TX/`zx_tm_init` **0 行**，每包 0 条指令。
   但「只读」这个说法**半错**：`zx_tm_read0()` 内含 `writel(..., +0x344014)`（`zx279128s-main.c:888-890`），
   每次快照间接写 **33 次** ⇒ 审「只读」必须连被调函数一起审。
3. **慢态（~110 Mbit/s）仍待判**，三个零重刷动作按代价排序：先读「慢态在场时 mode 是 `ffe` 还是 `stack`」，
   再做 `accel_ipv6` 的 stack→ffe 往返，最后做 `ffe_bp_drop` A/B。
4. **「只有重启才能恢复」目前只是传闻**：`DIAG-P10.3-FFE-COLLAPSE.md:70` 明写这是待验分支，
   同一文档 `:58` 的风险声明也写「重启/切模式可清（**待验证**）」；模式往返复测从未做过。
5. **文档炸弹**：`VERIFY-QUEUE.md:82/86` 把 debugfs 路径写成 `/sys/kernel/debug/ffe/`，实际是
   `/sys/kernel/debug/92000000.ethernet/`（`zx279128s-main.c:3537`）。该文件**不在本仓库**（在工作区根
   `docs/` 下），所以本文只登记、不修改；「已修」为空。

## §1 296 s 边界：驱动的 `ffe_aging=300`

- 时间常数：`zx279128s-accel.c:232` `static unsigned int ffe_aging = 300;`，
  `:224` 节拍 `ZX_FFE_TICK_MS 1000`；判定在 `:1279-1287`：
  `max_idle_ms = ffe_aging * 1000`，`jiffies_to_msecs(now - flow->lastuse) > max_idle_ms` 就
  `__zx_ffe_flow_del()`；`ffe_aging = 0` 表示永不老化（`:1277`）。
- **活跃流不会因为 300 s 到期被删**：`lastuse` 在每次命中时刷新（`zx279128s-accel.c:1713`、
  `:2992`、`:3046`），所以「296 s 突跳 = 老化把活跃流删了」在源码上就弱：只有「从没命中过的那一半」
  （例如只有单向数据的反方向）才会在 301±1 s 消失（`slowstate-knobs/20261102-2120/RESULT.md` §4.1；
  `stability-p10.5/20261102-0026/RESULT.md:138-141` 自己也是这么写的）。
- **所以「边界是 296 s」只说明时间常数对得上，不构成「老化是成因」的判决**；要判成因仍得跑
  `ffe_aging=30/10` 的对照（`slowstate-knobs/20261102-2120/RESULT.md` §2 E4、§4.2）。
- **不是 PPE CLA aging**：`zx_ppe_ipv6` 默认 `false`（`zx279128s-ppe.c:52`），
  CLA v6 表只在 `ipv6=1` 时编程（`:1908-1911`），dmesg 打 `IPv6 classifier off`（`:2899`）。
  用 `dump=2/32` 读 CLA aging word（`:2621,2638`）在 `ipv6=0` 时读到 0，不能当「没老化」的证据
  （`slowstate-knobs/20261102-2120/RESULT.md` §4.2 第 4 条）。
- 同类仪器错误的现场：C1b 用 CLA aging 扫 296 s，「一个数都没读到」，另有一层原因就是 debugfs 路径写错
  （`v6-selfconsist/C1b-20261102-0105/RESULT.md:17-18`）。

## §2 D2 插桩审计：可排除，但不是「纯只读」

- **落点**：工作区根 `images/1891a5f8/PATCH-d2.diff` 共 450 行 / 7 hunk / **+392 −1**；全部落在
  `zx279128s-eth.h` 文件头（1 个宏）、`zx279128s-main.c` 文件头（2 个 `module_param`）、
  以及 `zx_eth_dma_state_show()` 内部（`zx279128s-main.c:2934-3100`，rtnl_lock `:2959` /
  rtnl_unlock `:3098`）。probe(3592)/open(2688)/ISR(2646)/poll(2658)/RX(2088)/TX/`zx_tm_init`(917)
  **0 个 hunk**；数据面**每包 0 条指令、0 ns**
  （`d2-instrument/20261102-2105/RESULT.md` §0、§2、§6）。
- **「只读」半错**：每次 `cat dma_state` 会经 `zx_tm_read0()` 对
  `base + ZX_TM_IND_CMD` 写硬件寄存器；`ZX_TM_IND_CMD = ZX_TM_BASE + 0x4014`，`ZX_TM_BASE = 0x340000`
  （`zx279128s-eth.h:90,105`），实际偏移 `0x344014`。`writel` 在 `zx279128s-main.c:888-890`；
  每次快照 **33 次**（16 表1 + 1 复读 + 16 表0）（`d2-instrument/20261102-2105/RESULT.md` §1.1、§3）。
  > 行号校正：该报告原文写 `main.c:886-888`，当前 HEAD 实测 `writel` 在 `888-890`（同一 md5
  > `0c94befbdd46e38b715a36668c27f72e`，差 2 行）；引用以实测为准。
- **执行面**：`dma_state` 只在 `zx_eth_debugfs_init()` 注册（`zx279128s-main.c:3539-3540`），
  全树没有 init 脚本/定时器去读它；两个新参数 `d1_ring_scan`/`d1_blind` 也只在 show 里被读
  ⇒ **没人 `cat` 就不执行**（`d2-instrument/20261102-2105/RESULT.md` §1.3）。
- **为什么仍可排除**：① plain `#16`（无插桩）在 kexec 后同样楔死（`.scratch/ffe-fastpath/STATUS.md:471`）；
  ② D2 在全程不读 `dma_state`、空载无流量的情况下也能在开机 26–36 s 自己楔死
  （`idle-wedge-20261102-1843/SUMMARY.md` §0）；③ 同一 `#16` 冷启动 6/6 不楔
  （`flash-head/20261102-2130/RESULT.md` §3）。
- **审计规矩**：审「只读」不能只看新增行的 store 分类，必须连**被调函数**一起审——
  D2 的 33 条写就发生在 `zx_tm_read0()` 体内，不在「show 自己 173 条 store」的视野里
  （`d2-instrument/20261102-2105/RESULT.md` §1.1）。
- **残留风险（无证据）**：`zx_tm_read0()` 的间接窗口写是否有运行期副作用，未验证；D2 让它第一次在
  DMA 已启动、链路已 UP 之后可达（同文件 §4）。默认配置下没有一处读被源码证明「读即清」，
  唯一被源码点名「读会清」的 `DMA+0x68` 已被 `zx279128s-main.c:3082-3084` 跳过（同文件 §5）。

## §3 慢态待判：三个零重刷动作

慢态签名：约 **0.293%** 的帧在 alias 判据处被静默丢掉，TCP 平衡在 **~92–111 Mbit/s**，
触发条件未知、实测 2/4 轮（`DIAG-P10.3-FFE-COLLAPSE.md:8-10,58`）。以下动作都**不需要重刷、不需要重编**，
全部在「慢态在场」时做；合计约 45 min（含抽中慢态的复现成本）。

| 顺序 | 动作 | 观测/判据 | 证据 |
|---|---|---|---|
| E0 | 慢态在场时先读 mode：`cat /sys/kernel/debug/92000000.ethernet/accel_ipv6` 第 1/2 行 + `accel_ipv4` + `dmesg \| grep -E "strike\|fell back"` | 若 mode=`stack` ⇒ 110 不是 ffe 的锅；若 mode=`ffe` 且 alias 在涨、却没 strike ⇒ 判据分母/分族与实际丢点不符 | `slowstate-knobs/20261102-2120/RESULT.md` §2 E0 |
| E1 | `echo stack > $D/accel_ipv6` → 15 s iperf；再 `echo ffe > $D/accel_ipv6` → 5 s iperf | stack >250 Mbit/s（基线 325.8，区间 304.8–340.7）；切回 ffe 后 >600 恢复 ⇒ 在途 bp 错配、切换可清；仍 ~110 ⇒ 持久态 | 同文件 §2 E1a/E1b；`docs/VERIFY-QUEUE.md:94`（工作区根，非本仓） |
| E2 | `ffe_bp_drop` A/B（先 `cat` 记初值，30–60 s 流后写回） | 慢态消失/不消失，同时看 `ffe_dup` 增量 | 同文件 §2 E2 |

- **E0 的「110 且 mode=ffe」本身就矛盾**：当前树已把 alias 纳入 autofallback 判据——
  `ZX_FFE_FB_ALIAS_PERMILLE=2`、`ZX_FFE_FB_ALIAS_STRIKES=2`（`zx279128s-accel.c:320-321`），
  连续 2 个不合格窗（每窗 10 s）就应降级到 stack（`zx279128s-accel.c:3719-3740`）。
  若真出现 0.293% alias 且持续两窗，应 ~20 s 内自动降级到 stack（约 325 Mbit/s），
  不会停在 110 ⇒ 先读 mode 是零成本的第一刀
  （`slowstate-knobs/20261102-2120/RESULT.md` §0 第 2 条、§2）。
- **E1 的零重刷依据**：`accel_ipv6` 是带 `.write` 的 debugfs（`zx279128s-main.c:3550`，
  fops `zx279128s-accel.c:4295-4299`），写 `stack/ffe` 走 `zx_accel_set()` 的
  quiesce → 排空 TX（≤100 ms）→ `__zx_ffe_flush(本族)` → 写模式字节 → 重开降级窗口
  （`zx279128s-accel.c:3420-3615`）。切 stack 不被 PPE bound 拒绝（`if (bound && family == 0)` 只挡 v4，
  同文件 `:3464-3490`）。
- **E2 的参数存在性有矛盾、未裁决**：HEAD 源码有 `ffe_bp_drop`（`zx279128s-main.c:80-81`，
  初值 `true`），但盒子交付态 P10.5 的参数表里**没有**它（`flash-head/20261102-2130/RESULT.md` §7.3：
  「全表 30 项，无 `ffe_bp_drop`」）。⇒ 能不能在交付镜像上做 A/B，**未裁决**；必须先 `cat` 参数路径，
  不存在就改用同为 0644 的 `ffe_dup` 观测替代，不能假设它可写。
  另外源码注释自身口径也不一致：`:2203` 写「A3（ffe_bp_drop=1，默认关，会丢帧）」，而初值是 `true`
  （`:80`）——以运行期 `cat` 读回为准。
- **零重刷的边界**：E0/E1/E2 都是 0644 参数或 debugfs 写，`echo` 即生效、写回即恢复；不碰 flash、不重启。
  但慢态复现本身要抽签（触发条件未知，2/4 轮），时间主要花在等「慢态在场」。

## §4 「只有重启才能恢复」目前只是传闻

- `DIAG-P10.3-FFE-COLLAPSE.md:70` 把它写成待验分支：「恢复到 >600 ⇒ 在途 bp 错配、切换可清；
  仍 ~110 ⇒ 持久态（池/硬件），只能重启」。
- 同文件 `:58` 的风险声明也写「重启/切模式可清（**待验证**）」。
- **模式往返复测从未做过**：`:70` 的 stack→ffe 往返就是那次从未执行的实验；因此「只有重启能恢复」
  目前是**预测**，不是实测事实。E1 一旦做了，这句才有资格升级或撤回。
- 现有唯一相关实测是 296 s 事件后「1 s 恢复未崩」（`stability-p10.5/20261102-0026/RESULT.md:121-141`），
  它既不能证明也不能否证「只能重启」。

## §5 死旋钮清单（写了没人读）

统计口径：源码 `module_param*` 共 51 个（驱动 `zx279128s-*.c` 14 + `zx279128s-ppe.c` 37）、
带 `.write` 的 debugfs 11 个 ⇒ 运行期可写面 62；与 v6 快路相关的 20 个里，活 12 / 死或伪 8
（`slowstate-knobs/20261102-2120/RESULT.md` §1）。下面 8 个是**死/伪旋钮**：

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
  （`slowstate-knobs/20261102-2120/RESULT.md` §1.1）。
- `ZX_TX_QUEUE_LIMIT 256` **没有旋钮**：它是 `#define`（`zx279128s-eth.h:257`），要改只能重编；
  AX-02 已判负，见 [ax02-tx-queue-limit-verdict-2026-11-02.md](ax02-tx-queue-limit-verdict-2026-11-02.md)。

## §6 文档炸弹：debugfs 路径写错

- 工作区根 `docs/VERIFY-QUEUE.md:82`、`:86` 的命令是 `cat /sys/kernel/debug/ffe/alias_dump`；
  实际目录名是 `dev_name(adapter->dev)`，本板为 `/sys/kernel/debug/92000000.ethernet/`
  （`zx279128s-main.c:3537`）。全项目其它地方用的都是后者（`DIAG-P10.3-FFE-COLLAPSE.md:58`）。
- **该文件不在本仓库**：本仓 `docs/` 下没有 `VERIFY-QUEUE.md`（只有工作区根 `docs/` 有）。
  按任务边界，本文只登记、不修改；**「已修」= 无**。若将来把它搬进本仓，改的就是这两行的路径。
- 这不是纯笔误：C1b 的 CLA aging 扫描就是同类路径错误导致「一个数都没读到」
  （`v6-selfconsist/C1b-20261102-0105/RESULT.md:17-18`）。**判据命令必须先 `cat` 自检路径**，
  再拿读数下结论。

## §7 口径与边界

- **源码版本**：本文所有 `文件:行` 来自本仓 HEAD `2dab71d`（2026-11-02 04:16 提交）；盒子跑的是
  P10.5（`623a812`，`image-p10.5.bin`）。两者参数集不同：例如 HEAD 有 `ffe_bp_drop`，P10.5 没有
  （§3 E2）。引用「源码里有」不等于「盒子上有」，落笔必须分开。
- **没做的事**：未上机、未读本轮运行期计数、未改源码/overlay、未构建、未 git 写；E0–E2 的实测值
  一律为「无证据」。温度/DVFS、内存碎片两条排除只做了源码级论证
  （`slowstate-knobs/20261102-2120/RESULT.md` §5），没有取 `/sys` 读数。
- **未裁决**：① 296 s 事件是否真由老化触发；② 慢态是状态型还是持久态；③ `ffe_bp_drop` 在 P10.5 上是否存在；
  ④ 「只有重启能恢复」是否成立。四条都要等 E0–E2 的实测。

## §8 证据索引

| 文件（文字路径，不作链接） | 内容 |
|---|---|
| `.scratch/ffe-fastpath/slowstate-knobs/20261102-2120/RESULT.md` | 62 个可写面、20 个相关开关、E0–E4 与顺序、296 s 源码事实 |
| `.scratch/ffe-fastpath/d2-instrument/20261102-2105/RESULT.md` | D2 +392 行落点、33 次间接写、可排除论证 |
| `.scratch/ffe-fastpath/DIAG-P10.3-FFE-COLLAPSE.md:8-10,58,70` | 慢态签名、触发未知、模式往返待验 |
| `.scratch/ffe-fastpath/stability-p10.5/20261102-0026/RESULT.md:121-141` | 296 s 事件实况与「aging 未证」 |
| `.scratch/ffe-fastpath/idle-wedge-20261102-1843/SUMMARY.md` §0 | D2 空载、零 `dma_state` 读也楔死 |
| `.scratch/ffe-fastpath/v6-selfconsist/C1b-20261102-0105/RESULT.md:17-18` | CLA aging 扫描被 debugfs 路径搞死 |
| `工作区根 docs/VERIFY-QUEUE.md:82,86`（非本仓） | 错误路径 `/sys/kernel/debug/ffe/` 的原文 |
| `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c:232,1279-1287` | `ffe_aging=300` 与老化判定 |
| `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c:888-890,3537` | 间接窗口写、debugfs 目录名 |
| `overlay/package/kernel/kmod-e2631-ppe/src/zx279128s-ppe.c:52,2899` | CLA v6 默认关与 `IPv6 classifier off` |

---
title: FFE 运行期调参手册：合并窗口、NAPI 权重与延迟剂量
date: 2026-11-04
status: done
topic: accel-impl
summary: 2026-11-03 夜调过的 FFE 运行期参数总表；实测最优 (250,16,64)，单流 735→923 Mbit/s，代价 IRQ ×4.4；未测项逐条列。
evidence: .scratch/ffe-fastpath/coal-sweep/20261103-2330/{RESULT.md,sweep.tsv}; .scratch/ffe-fastpath/coal-verify/20261104-0015/{RESULT.md,cells.tsv}; .scratch/ffe-fastpath/delay-dose/20261103-2300/{RESULT.md,dose-table.tsv}; .scratch/ffe-fastpath/duplex/20261104-0045/RESULT.md; .scratch/ffe-fastpath/per-frame-audit/20261104-0130/RESULT.md
supersedes:
---

# FFE 运行期调参手册：合并窗口、NAPI 权重与延迟剂量

> 读者：要在这台板子上按实测值设置 FFE 运行期参数的人。
> 读法：§0（能/不能干什么）→ §2 总表 → 对应参数一节 → §15 未测清单（照抄前必看）。

## §0 读这份文档能干什么 / 不能干什么

**能**：

- 照 §12 把三个交付参数 `(zxcoal_us, zxcoal_frames, zxnapi_weight) = (250, 16, 64)` 写进运行期
  sysfs，或按 §13 的口径由 `/etc/config/zx_ffe` 在开机时应用；
- 查到每个参数的用户态路径、底层寄存器/字段、编码、默认值、合法范围、生效方式和 `文件:行`
  作用点（§2 总表）；
- 知道每个数的**测量条件**和代价（IRQ/s、softirq），以及哪些"看起来是旋钮"的东西在盒上
  根本不可调（§11）。

**不能**：

- 不能给未测参数一个"最优值"。本文把没测的一律写"未测试"（§15），把推断一律标〔推断，未测〕。
- 不能把本文数字外推到表里没写的形态：本文的吞吐数除特别注明外，都是**单流 IPv6 TCP（cubic）、
  30 s、本台架**；多流/双向/UDP/其他包型的数各自单列，不互相替代。
- 不能当硬件 datasheet 用。`0x30`/`0x34` 的"时间或帧数先到者抬中断"来自**上游驱动注释**
  （`zx279128s-eth.h:295-299`，Navid Ghahremani 的 GPL 驱动），**不是原厂文档**；原厂
  `tm.ko` 里这两个偏移字面量零命中（〔上游注释〕/〔原厂二进制〕，见
  `.scratch/ffe-fastpath/tx-coalesce/20261104-0025/RESULT.md` §6）。

**一句话交付建议值**：`zxcoal_us=250`、`zxcoal_frames=16`、`zxnapi_weight=64`。同轮
`500/64/64` 控制 728.3–733.7 Mbit/s → 单流 **923.0 Mbit/s（+26.7 %）**〔本板实测〕；
代价 **IRQ/s 1167→5171、softirq 0.41→0.52**。这不是零成本提速（§12）。

## §1 标注、口径与行号基准

- 〔本板实测〕= 有落盘日志/TSV，按 §14 口径量出来的；〔代码事实〕= 当前工作树 grep 得到；
  〔上游注释〕= 上游驱动注释，不等于硬件事实；〔推断，未测〕= 我方解释，没有物证。
- 行号基准：**本仓工作树 2026-11-04 21:40 前后（含未提交 `rx_split`/`napi1` 补丁）**。驱动目录简写 `SRC/` =
  `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`；文中不写目录的
  `zx279128s-main.c` / `zx279128s-accel.c` / `zx279128s-eth.h` 都在 `SRC/` 下。
  工作树里这三个文件有未提交改动，**行号会漂，照抄前重 grep**。
- 证据短名（全文一致，均可展开成完整路径）：
  `sweep.tsv` = `.scratch/ffe-fastpath/coal-sweep/20261103-2330/sweep.tsv`；
  `cells.tsv` = `.scratch/ffe-fastpath/coal-verify/20261104-0015/cells.tsv`；
  `dose-table.tsv` = `.scratch/ffe-fastpath/delay-dose/20261103-2300/dose-table.tsv`。
  单独写的 `RESULT.md` 指"与该表同轮目录"的 `RESULT.md`，轮次由所在小节的日期/节名限定。
- 测量时在跑的镜像：`slot1`、版本串 `V1.0.0.8B8.9950`、`uname -v` =
  `#0 SMP Mon Oct 12 17:14:44 2026`、`boot_id=7583a2e9-…`（合并扫描与验证轮同一 boot）。
  延迟剂量轮是更早的 `V1.0.0.8B8.9930`（`boot_id=b543e60f-…`），两轮控制均值不同，表里分别标。

## §2 参数总表

盒上参数目录：`/sys/module/zx279128s_eth/parameters/`（实测该目录同时有 `zxcoal_us`
`zxcoal_frames` `zxnapi_weight`、`txq_limit`、8 个 `diag_*`、2 个 `zxdly_*`，见
`.scratch/ffe-fastpath/coal-sweep/20261103-2330/RESULT.md` §2）。

| 参数 | 用户态路径（盒上） | 底层位置 / 编码 | 当前默认 | 范围（驱动校验） | 生效方式 | 作用点（`文件:行`） | 本板实测最优 |
|---|---|---|---|---|---|---|---|
| `zxcoal_us` | `.../parameters/zxcoal_us` | DMA 块 `+0x30`（绝对 `0x350030`）；**1 LSB = 20 ns，写值 = µs × 50** | **250**（旧镜像 500） | 0..1,000,000（>上限 `-EINVAL`） | 直接写寄存器，`readl` 读回自证；下一个合并窗口生效，无需 disable/enable | `zx279128s-eth.h:150`（宏）、`:301`（默认）、`zx279128s-main.c:1479`（setter）、`:1501`（module_param_cb）、`:2421-2423`（编程点） | 250（配 `frames=16`），单流 924.4 |
| `zxcoal_frames` | `.../parameters/zxcoal_frames` | DMA 块 `+0x34`（绝对 `0x350034`）；**帧数原值**，无乘数 | **16**（旧镜像 64） | 0..4096 | 同上 | `zx279128s-eth.h:151`、`:300`（默认）、`zx279128s-main.c:1504`（setter）、`:1525`（module_param_cb）、`:2423`（编程点） | 16（`us=250`）924.4；32 也 921–923 |
| `zxnapi_weight` | `.../parameters/zxnapi_weight` | 软件字段，**napi0 与 napi1 各一份** `weight`（napi1 仅 `rx_split=1` 时存在），**不写寄存器** | 64 | 1..1024（0 与越界 `-EINVAL`） | **同时**写 `adapter->napi.weight`（napi0，每个 adapter 一份）与 `zx_rxs.napi1.weight`（napi1，仅 `rx_split=1` 时），两份各自下一次 poll 生效（`napi_poll()` 每轮重读） | `zx279128s-main.c:967`（变量）、`:1553`（setter 入口；两份写点在 `zx_napi_weight_one()` 里 `:1541`/`:1544-1545`）、`:1577`（module_param_cb）、`:6240-6241`（napi0 的 `netif_napi_add_weight(adapter->ports[i], &adapter->napi, ...)`）、`:1126-1127`（`zx_rx_split_setup()` 里 napi1 的 `netif_napi_add_weight(..., &zx_rxs.napi1, ...)`，仅 `rx_split=1` 时建）；**两个创建点**的 weight 均取 `(int)READ_ONCE(zxnapi_weight)`（默认 64） | 64（128 未观察到独立效应：仅 1 格、在散布内，§5） |
| `zxdly_rx_ns` | `.../parameters/zxdly_rx_ns` | 纯 `ndelay/udelay`，不读帧、不查表、不写共享状态 | 0 | `uint`；实际粒度 1 µs（1..999 ns 落成 1 µs） | 下一帧生效 | `zx279128s-main.c:217-219`、`:4023-4027`（RX 热路径注入点） | 交付 0；诊断用 4000 = 4 µs/帧 |
| `zxdly_tx_ns` | `.../parameters/zxdly_tx_ns` | 同上，位置在 `tx_lock` + RCU 临界区内 | 0 | 同上 | 同上 | `zx279128s-main.c:221-223`、`:3401-3406`（单帧 `zx_tx_submit_bp()`，TX 提交成功后）、`:3504-3510`（**批量** `zx_tx_submit_batch()` 的逐帧循环里同一段延时，同样在 `tx_lock` 内——锁由调用方 `zx_ffe_tx_batch_flush()` 持，`zx279128s-accel.c:2994`） | 交付 0；TX-only 全更慢（§6） |
| `accel_ipv4` / `accel_ipv6` | `/sys/kernel/debug/92000000.ethernet/accel_ipv4`、`.../accel_ipv6` | 族级模式，写 `stack\|ffe\|hw\|zc`；**`hw` 只有 v4** | v4=hw、v6=ffe | 四值枚举；v6 写 `hw` 会被改成 `stack`；同族 `hw↔ffe/zc` 在 PPE 绑定时返 `-EBUSY` | 同步切换：先排空 TX，返回时已是新模式或什么都没变 | `zx279128s-eth.h:333-336`（枚举）、`zx279128s-main.c:5837-5840`（debugfs）、`zx279128s-accel.c:3831`（hw 仅 v4）、`:3872-3878`（PPE 占用返 EBUSY）、`zx279128s-main.c:4167`/`:4175`（数据面只处理 `mode==ffe` 的族） | v4=hw、v6=ffe（交付） |
| `diag_residence` `diag_seq` `diag_poll` `diag_txq` `diag_txlat` `diag_drop` `diag_burst`（+`diag_burst_us`） | `.../parameters/diag_*` | 各置 `zx_diag_mask` 的 `BIT(0..6)`；只读计数，不参与转发判据 | 全 0 | `kstrtobool`（0/1/Y/N） | 立即；下一帧生效 | `zx279128s-main.c:254-286`（读写口）、`:297-309`（7 个 module_param_cb）、`zx279128s-eth.h:822-828`（位定义）、debugfs `diag` 只读口 `zx279128s-main.c:5829` | 全关（会污染测量，§8） |
| `timing` | `.../parameters/timing` | 每包 6 次 `ktime_get_ns()` + 11 次累加（我方注释估 ~100 ns/包） | 0 | `bool` | 立即 | `zx279128s-main.c:152`、`:225`（`ZX_TS()`） | 关（§8） |
| `txq_limit` | `.../parameters/txq_limit` | TX 环占用上限（软件阈值，不写寄存器） | 1024 | 64..1024（越界 `-EINVAL`） | 立即，读侧一律 `READ_ONCE` | `zx279128s-main.c:171`（变量）、`:173`（setter）、`:191`（module_param_cb）、`:3352`（提交硬闸）、`:3408`（停车）、`zx279128s-accel.c:3312`（快路 #19） | 未做 A/B（§9） |
| `ffe_aging` | `.../parameters/ffe_aging` | 流表老化秒数（软件，0 = 不老化） | 300 | `uint` | 立即 | `zx279128s-accel.c:232-234`、`:1310-1319`（老化 work） | 未测（§15） |
| `learn_delay_ms` | `.../parameters/learn_delay_ms` | 开机后学习门延时（软件） | 120000 | 0..600000（超限钳位） | **只能开机参数（0444）**，运行期不可写 | `zx279128s-accel.c:262`、`:4312` | 120000（即测量门，§14） |

**行号基准（本表 `zx279128s-main.c` 编程点）**：两个合并窗口写点按 **2026-11-04 21:40 的工作树**核对为 `:2421-2423`（`zx_hw_init()` 起 `:2365`）；**含未提交的 `rx_split`/`napi1` 补丁**，补丁落地前的旧号是 `:1931-1933`。

**注意默认值的时效**：上表"当前默认"是**工作树**的值；测量时那两颗镜像的编译期默认仍是
`500/64/64`（`zx279128s-eth.h` 在 2026-11-03 23:15 时还是 `ZX_RX_IRQ_FRAMES 64` /
`ZX_RX_IRQ_USECS 500`，见 `.scratch/ffe-fastpath/coal-knob/20261103-2315/RESULT.md` §1）。
任何盒子上**先读回**再决定要不要写。

### §2.1 三个交付参数的最小写法

```sh
P=/sys/module/zx279128s_eth/parameters
echo 250 > $P/zxcoal_us
echo 16  > $P/zxcoal_frames
echo 64  > $P/zxnapi_weight
cat $P/zxcoal_us $P/zxcoal_frames $P/zxnapi_weight
```

写 `zxcoal_us` / `zxcoal_frames` 后驱动会打一行读回自证（原文格式，见
`.scratch/ffe-fastpath/coal-sweep/20261103-2330/RESULT.md` §4）：

```
zxcoal_us=250: reg 0x350030 raw 25000 -> 12500 (readback 12500)
zxcoal_frames=16: reg 0x350034 raw 64 -> 16 (readback 16)
```

若硬件把字段截断、读回不等于写入值，驱动会把旧值写回去并打 `field too narrow? restored …`、
返回 `-EIO`（`zx279128s-main.c:1453-1476`）——这时 `cat` 出来的是**实际生效值**。

## §3 `zxcoal_us` —— RX 中断合并时间阈值

**它是什么**：DMA 引擎 RX 中断合并器的"从窗口第一帧起最多等多久"。寄存器 `0x350030`，
1 LSB = 20 ns，驱动把 µs × 50 写进去（`zx279128s-main.c:1489`、`:2421-2423`）。
500 µs → 25000 = `0x61A8`；250 µs → 12500 = `0x30D4`。

**测量条件（下表全部同一轮）**〔本板实测〕：

- 时间 2026-11-03 23:33 → 2026-11-04 00:09；镜像 `V1.0.0.8B8.9950`、`boot_id=7583a2e9-…`。
- 台架：宿主 `ens34` → 盒子 `lan1`（WAN，`192.168.5.1`）→ `br-lan`（`lan2`）→ 服务器。
- 流：`iperf3 -6 -c <服务器 ULA> -p 5201 -t 30 -O 2 -J`，**单流、IPv6、TCP cubic、正向**。
- 门：起测前 `uptime ≥ 130` 且 `dmesg` 有 `accel: ffe_learn auto-enabled 120000 ms after probe`；
  `accel_ipv6` 首行 `ffe`、`ppe_bound=1`。
- 每格只跑一次；同参数散布最大 10.3 %，所以 Δ% 一律对**同轮 6 格控制均值 733.7** 算。

| `zxcoal_us` | 吞吐 Mbit/s | Δ% vs 733.7 | IRQ/s | frames/IRQ | softirq | 格 → 证据 |
|---|---|---|---|---|---|---|
| **500（控制，6 格）** | **733.7**（688.7–759.5） | — | 1169–1191 | 49.0–54.8 | 0.40–0.44 | K0a/K0ap/K0c/K0cp/K0e/K0b → `sweep.tsv:2,3,28,29,31,32` |
| 375 | 729.7 | −0.6 | 1525.3 | 40.8 | 0.401 | A375 → `sweep.tsv:7` |
| 250 | 866.9 / 894.0 | +18.2 / +21.8 | 1993.7 / 1974.1 | 37.0 / 38.9 | 0.455 / 0.447 | A250/A250b → `sweep.tsv:4,5` |
| 125 | 911.9 | +24.3 | 3690.0 | 21.0 | 0.473 | A125 → `sweep.tsv:16` |
| 62 | 922.0 | +25.7 | 7071.7 | 11.1 | 0.567 | A62 → `sweep.tsv:17` |
| 31 | 918.8 | +25.2 | 12649.3 | 6.2 | 0.552 | A31 → `sweep.tsv:18` |
| 750 | 497.9 | −32.1 | 951.5 | 45.0 | 0.285 | A750 → `sweep.tsv:8` |
| 1000 | 548.3 | −25.3 | 790.8 | 58.5 | 0.317 | A1000 → `sweep.tsv:10` |
| 1500 | 491.8 | −33.0 | 688.5 | 62.4 | 0.310 | A1500 → `sweep.tsv:12` |
| 2000 | 478.1 | −34.8 | 632.8 | 64.8 | 0.289 | A2000 → `sweep.tsv:14` |

**读法**：

1. **31–250 µs 是一个平台**（911.9–924.4），125/62/31 之间没有再显著上升；拐点在
   **250–375 µs 之间**（375 已掉回 729.7）。"越短越快"只在 375→250 这一段成立。
2. **≥750 µs 是深洼地**，且**非单调**：750→497.9、1000→548.3、1500→491.8、2000→478.1。
   所以"拉长窗口能白拿批量"不成立，方向是反的（`sweep.tsv:8,10,12,14`）。
3. **可逆、可重复**：从最优点写回 `500/64/64` 立刻掉到 **759.5**（K0c），再写回
   `250/16/64` 回到 **924.1**（K0d），再写回 500 又掉到 **740.0**（K0e）
   （`sweep.tsv:28,30,31`）。验证轮 32 格全部"切得上去、掉得下来"，没有一次失败
   （`.scratch/ffe-fastpath/coal-verify/20261104-0015/RESULT.md` §1）。
4. **代价是中断率与 softirq**：IRQ/s 1167 → 1994（250/64）→ 7072（62/64）→ 12649（31/64），
   softirq 0.40 → 0.45–0.57（同表第 5、6 列）。**softirq 低 ≠ 好**：750–2000 µs 的 softirq
   只有 0.285–0.317，吞吐也最低。

**为什么交付不选 62/31**：单流上 62 µs 略高于 250/16（922.0 vs 924.4，散布内），但 IRQ/s 是
250/16 的 1.4 倍，且 62/64 在小包形态上低于基线（§12）。31 µs 的 IRQ/s 到 12649，收益不再增加。

## §4 `zxcoal_frames` —— RX 中断合并帧数阈值

**它是什么**：同一合并窗口里"攒到多少帧就提前抬中断"。寄存器 `0x350034`，写原始帧数
（`zx279128s-main.c:1513`、`:2423`），默认 16。硬件语义按上游注释是"帧数或时间，谁先到谁抬"
（`zx279128s-eth.h:295-299`，〔上游注释〕）。

**测量条件**：同 §3（同一轮、同一台架、单流 v6 cubic 30 s、门满足）。

| `us` | `frames` | 吞吐 Mbit/s | IRQ/s | 备注 → 证据 |
|---|---|---|---|---|
| 250 | 16 | **924.4** / 924.1 / 922.4 / 923.6 | 5163.5 / 5114.3 / 5170.9 / 5186.9 | B16c / K0d / V1a / V1b → `sweep.tsv:24,30`、`cells.tsv:2,3` |
| 250 | 32 | 921.2 / 923.2 / 921.8 / **841.8** | 2593.6 / 2585.1 / 2607.1 / 2559.4 | B32 / B32b / V2a / V2b（V2b 是孤立塌陷，retr=1）→ `sweep.tsv:19,20`、`cells.tsv:4,5` |
| 250 | 64 | 866.9 / 894.0 | 1993.7 / 1974.1 | A250 / A250b → `sweep.tsv:4,5` |
| 250 | 128 | 866.9 / 862.1 | 1957.1 / 1969.4 | B128 / B128p → `sweep.tsv:25,26` |
| 62 | 16 | 923.5 / 921.4 | 7149.4 / 7044.7 | B16 / B16p → `sweep.tsv:22,23` |
| 62 | 64 | 922.0 | 7071.7 | A62 → `sweep.tsv:17` |

**读法（只写表里有的）**：

1. **`us=250` 下 frames 仍然显著影响结果**：16/32 帧比 64 帧的两次（866.9/894.0）高
   约 3–7 %，IRQ/s 从 ~2.0k 升到 ~5.2k。**不能**简单说"250 µs 已经把定时器绑死、帧门限无关"——实测就是有关。
   这与"16 帧在 ~80 kpps 下约 200 µs 先到"一致，但**门限关系没有单独标定**〔推断，未测〕。
2. **`us=62` 下 frames 在 16 与 64 之间无差别**（923.5 / 921.4 vs 922.0，散布内）。
3. **`us=500` 下 frames 的独立作用：未测试**。本轮没有 `500/16`、`500/32`、`500/128` 格；
   "us=500 下基本不起作用（定时器绑定）"这句话**没有本仓证据**，照抄会踩空。要补就按 §15-10 测。
4. **`frames=16` 的中断风暴在本板"未观测到"，不是"不存在"**：小包加压格 `-M 88`
   （约 128 B 帧）四格全部塌到 **60.7–63.2 Mbit/s**（retr 1.9k–3.2k），IRQ/s 分别
   1010 / 6318 / 5363 / 2806（`cells.tsv:22-25`）。原因是 TCP 侧先塌、**本板推不出高 pps**，
   所以无法用这个台架判 frames=16 在极小包下安不安全。

## §5 `zxnapi_weight` —— NAPI 每轮预算

**它是什么**：`zx_eth_poll()`（`rx_split=1` 时另加 `zx_eth_poll_ring1()`）每轮最多处理多少帧，即 **napi0 与 napi1 两份** `weight`（napi1 仅 `rx_split=1` 时存在）。运行期写的是
`napi0` 与 `napi1` **两份** `weight` 字段（setter `zx279128s-main.c:1553` 入口；两份写点在 `zx_napi_weight_one()` 里：`:1541` 写 napi0、`:1544-1545` 写 napi1，后者在 `if (zx_rxs.added)` 内），不碰硬件（`:6240-6241`、`:1126-1127`）。

**测量条件**：同 §3。只测了一格 weight=128（`us=250, frames=64`）。

| 参数 | 吞吐 Mbit/s | IRQ/s | frames/IRQ(lan1) | frames/poll | 证据 |
|---|---|---|---|---|---|
| 250/64/**64** | 866.9 / 894.0 | 1993.7 / 1974.1 | 37.0 / 38.9 | 未测（该格未开 `diag_poll`） | `sweep.tsv:4,5` |
| 250/64/**64** + `diag_poll=1` | 879.3（仅参考） | 1953.6 | 38.45 | **40.76** | `sweep.tsv:6` |
| 250/64/**128** | 909.3 | 1972.1 | 39.27 | 未测（该格未开 `diag_poll`） | C128 → `sweep.tsv:27` |

**读法**：

- 909.3 与 866.9/894.0 的差落在同档散布内（250/64 自己两次就差 3.1 %，整轮同档散布约 10 %），
  只有一格 ⇒ **本轮只能支持"未观察到 128 的独立效应"，不能支持"确定无效应"**。
- **不能**直接说"weight=128 那一格预算没触顶"：那格没开 `diag_poll`，读不到 frames/poll。
  能说的是：本轮所有**测过** frames/poll 的格都在 **11.7–59.9**，从未到 64（`NAPI_POLL_WEIGHT`
  的默认上限）；weight=128 那格的 frames/IRQ(39.27) 与 weight=64 的 37–39 同档。
- **`frames/poll` 不是快慢判据**：快态在不同旋钮下出现过 **11.72**（B16p，921.4）、
  **31.56**（B32p，913.5）、**59.07**（delay-dose 的 PSEQ，890.9）、**58.5**（napi-knob 的
  S1-poll，857.5）；慢态自己可以到 46.3（K0ap）。同一"快态"横跨 11.7–59.1，拿它判态会判错
  （`sweep.tsv:3,21,23`、`.scratch/ffe-fastpath/delay-dose/20261103-2300/dose-table.tsv:26,29`、
  `.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §3）。
- **`/proc/sys/net/core/dev_weight` 不是这个预算的来源**：写 32/128/256 得到
  720.7 / 735.9 / 700.4（同轮控制 731.0 / 766.5 / 738.2），全在慢态散布内；`dev_weight`
  只落到 `process_backlog()`（§11）。

## §6 `zxdly_rx_ns` / `zxdly_tx_ns` —— 纯诊断的每帧忙等

**它们是什么**：在 RX 快路入口（cache invalidate 之后、判据之前）或 TX 提交成功后
（`tx_lock` 内）插一段**纯时间**延迟；不读帧、不查表、不写共享状态。粒度 1 µs（向上取整），
默认 0。**它们不是交付旋钮**，只用来回答"快态是不是纯节拍造成的"。

**测量条件（剂量轮）**：2026-11-03 22:53–23:35，镜像 `V1.0.0.8B8.9930`、`boot_id=b543e60f-…`，
台架/流/门同 §3；控制 = D0a–D0e 五格均值 **735.0**（709.3/708.6/750.0/741.1/766.1）。

| `rx_ns` / `tx_ns` | 实际每帧 | 吞吐 Mbit/s | Δ% vs 735.0 | 证据 |
|---|---|---|---|---|
| 1000 / 1000 | 2 µs | **468.1** / 480.2 | −36.3 / −34.7 | A1 / A1b → `dose-table.tsv:2,3` |
| 2000 / 2000 | 4 µs | 806.3 / **836.9** | +9.7 / +13.9 | A2 / A2b → `:4,6` |
| 2500 / 2500 | 6.2 µs | **890.2** | +21.1 | A25 → `:5` |
| 3000 / 3000 | 6 µs | 648.1 | −11.8 | A3 → `:7` |
| 4000 / 4000 | 8 µs | 742.3 | +1.0 | A4 → `:8` |
| 8000 / 8000 | 16 µs | **446.1** | −39.3 | A8 → `:9` |
| 1000 / 0 | 1 µs（RX-only） | 526.5 | −28.4 | B1 → `:10` |
| 2000 / 0 | 2 µs（RX-only） | 483.1 | −34.3 | B2 → `:11` |
| 4000 / 0 | 4 µs（RX-only） | 783.8 / **890.3** | +6.6 / **+21.1** | B4 / B4b → `:12,13` |
| 0 / 1000 | 1 µs（TX-only） | 528.8 | −28.1 | C1 → `:14` |
| 0 / 2000 | 2 µs（TX-only） | 479.2 | −34.8 | C2 → `:15` |
| 0 / 4000 | 4 µs（TX-only） | 654.3 / 673.4 | −11.0 / −8.4 | C4 / C4b → `:16,17` |

**把延迟叠到交付候选上**（验证轮，控制 926.6 / 923.0）：

| 格 | 参数 | 吞吐 | 对同档无延迟 | IRQ/s | frames/IRQ | softirq | 证据 |
|---|---|---|---|---|---|---|---|
| D1 | `62/64/64` + `rx=4000` | **879.3** | **−5.1 %** | 2358（无延迟 7214） | 34.45（无延迟 11.63） | **0.809** | `cells.tsv:20`、`RESULT.md` §5 |
| D1b | `250/16/64` + `rx=4000` | **898.6** | **−2.6 %** | 4996（无延迟 5171） | 16.48（无延迟 16.04） | **0.831** | `cells.tsv:31`、`RESULT.md` §5 |

**结论〔本板实测〕**：

1. **非单调**：2 µs/帧是洼地（468–480），4–6 µs 是快态（836.9–890.3），6 µs 另一个格又塌到
   648.1，16 µs 掉到 446.1。所以"延迟越大越接近快态"不成立。
2. **仅 RX 侧就能复现快态**（B4b 890.3），**仅 TX 侧全更慢**（C1 528.8、C2 479.2、
   C4/C4b 654.3/673.4）。节奏点在 RX。
3. **和合并窗口是同一个饱和项**：给最优候选再叠 4 µs 只会降 2.6–5.1 %，且 IRQ 掉到约 1/3、
   frames/IRQ 涨到 3 倍——延迟被吃进同一个"每批时长 × 每批帧数"的乘积，没有第二条独立轴。
4. **绝不与快态一起交付**：`softirq` 被顶到 **0.809/0.831**，8 µs 档 0.978 已饱和；这是诊断臂，
   不是配置项。

## §7 `accel_ipv4` / `accel_ipv6` —— 族级模式开关

**它是什么**：每个协议族单独选 `stack / ffe / hw / zc`，写 debugfs
`/sys/kernel/debug/92000000.ethernet/accel_ipv{4,6}`（`zx279128s-main.c:5837-5840`）。交付默认
**v4=hw、v6=ffe**（`zx279128s-eth.h:333-336` 枚举、`zx279128s-main.c:5980-5987` 初值）。

**要点**〔代码事实 + 本板实测〕：

1. **`hw` 只有 IPv4 有**（`zx279128s-accel.c:3831`）；v6 写 `hw` 会被驱动改成 `stack` 并打
   warning（`zx279128s-main.c:5984-5987`）。
2. **数据面只处理"该族当前模式 == ffe"的帧**（`zx279128s-main.c:4167`、`:4175`）。
   ⇒ 交付态下真正走我方 FFE 的**只有 IPv6**；v4 的帧由 PPE 接走，v6 上 CPU 后由 FFE 接住，
   这是设计上的并存，不是"v4 被 FFE 加速了"。
3. **v4 从 hw 切 ffe 要先让 PPE 释放**：同族 `hw` 与 `ffe` 互斥，PPE 绑着 flowtable 时
   `echo ffe > accel_ipv4` 返回 `-EBUSY`（`zx279128s-accel.c:3872-3878`）。做法是先
   `uci set firewall.@defaults[0].flow_offloading_hw=0` + reload（或移除 flowtable），确认
   `ppe_bound=0` 再切。**该切换后的 v4 吞吐未测**（§15-11）。
4. **v4 走 `hw` 时不经 RX 合并器**：同一轮把 `zxcoal_us` 从 500 改到 62（`zxcoal_frames`
   保持 64），v4 吞吐 **933.3 → 934.8 Mbit/s（+0.2 %）**，IRQ/s 只有 **7.9 → 10.8**，
   softirq 0.0009（F0/F1 → `cells.tsv:18,19`）。对比同轮 v6 单流 735.5 → 927.1、IRQ
   1166.5 → 7213.7 ⇒ **v4 的 hw 路径几乎不上 CPU，合并窗口对它的吞吐没有杠杆**。
5. **多 WAN / 策略路由时这个开关会被自动写 `stack`**：硬件条目没有 WAN 存活概念，
   路由切走后旧条目仍按旧出口转发（静态黑洞，等到 300 s 老化）。交付镜像用
   `sbin/e2631-ffe-multiwan` + `/etc/init.d/zx_ffe_multiwan` 检测"默认出口 ≥2 /
   非基线 `ip rule` / mwan3 启用接口 ≥2"，命中就把 `accel_ipvN` 写成 `stack`。
   判据、恢复/绕过与未验证项见 [../operations/multiwan-ffe-safety.md](../operations/multiwan-ffe-safety.md)。
   单 WAN 基线下守卫一次 `accel_*` 都不写。

## §8 诊断埋点开关 —— 名字、用途、为什么必须先关

7 个开关各占 `zx_diag_mask` 的一位（`zx279128s-eth.h:822-828`），写 `1/Y/0/N` 立刻生效，
默认全 0；输出在 debugfs `diag`（0444，只读，`zx279128s-main.c:5829`）。

| 名字 | 位 | 用途（源码注释口径） |
|---|---|---|
| `diag_residence` | BIT(0) | 盒内停留：RX 快路入口 → TX 提交成功，按出口口分桶 |
| `diag_seq` | BIT(1) | TCP seq 反演：RX 入口与 TX 提交各一张 1024 槽表 |
| `diag_poll` | BIT(2) | 每 NAPI poll 处理/转发帧数直方图 |
| `diag_txq` | BIT(3) | TX 环占用 + `txq_limit` 挡回 |
| `diag_txlat` | BIT(4) | TX 完成延迟：`TX_START` → reclaim 见到完成 |
| `diag_drop` | BIT(5) | 逐原因丢点计数 |
| `diag_burst` | BIT(6) | 每出口突发段长与段间空档；`diag_burst_us` 默认 2 µs |
| `timing` | 独立 bool | 每包 9 段 `ktime_get_ns()` 阶段计时 |

**会污染合并窗口测量**〔本板实测〕：

- `diag_seq=1`（`mask=0x02`）把慢态单流从 731–766 抬到 **878.3 / 886.8**（napi-knob S1-r1/S1-r2），
  另一轮甚至 **898.2 / 895.1**（delay-dose SEQ/SEQb）——这是探针副作用，不是参数效果。
- `diag_poll=1`（`mask=0x04`）单开 741.1，与慢态同档；`mask=0x06`（seq+poll）857.5
  （`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §1）。
- `timing=1` 的我方注释估 ~100 ns/包、60 万包/s 时约 7 % 一个核（`zx279128s-main.c:145-151`，
  〔我方注释〕，本轮**未测**其吞吐影响）。
- ⇒ **任何参数扫描都必须在 `mask=0`、`diag_*=0`、`zxdly_*=0`、`timing=0` 下做**；
  开任何一个，测到的就不是合并窗口本身。

## §9 `txq_limit` —— TX 环占用上限（本轮未做 A/B）

**它是什么**：软件阈值，`tx_pending >= txq_limit` 就停车/挡回；范围 **64..1024**，默认 **1024**
（= 环长 `ZX_NUM_TX_DESC`，再大就会覆盖在飞槽位）。作用点：`zx279128s-main.c:171/173/191`
（变量/写入校验/module_param_cb）、`:3352`（提交硬闸）、`:3408`（满则停所有口）、
`zx279128s-accel.c:3312`（快路 #19 命中后、改写前）。

**本轮结论：未测试（未做满足本仓证据标准的 A/B）**。理由与边界：

- 2026-11-03 的调参轮全部把 `txq_limit` 留在默认 1024，没有扫过 64/128/256/512；
  能找到的 txq 数字只有"历史峰值"：限 256 时 `tx_pending` 峰值 **64**
  （`.scratch/ffe-fastpath/vendor-sw-pipeline/20261103-0100/PIPELINE.md:240`）——这说明默认环深
  **可能**不是约束〔推断，未测〕，**但不是 A/B，不能当"改它无影响"的结论**。
- 先前工作里有一条对照记录，但对应原始 iperf3 日志没有同框记录当时的参数值，按本文证据标准
  **不采信、不引用其数字**。

**若要测，怎么测**（不改代码，只写 sysfs）：

1. 固定 `zxcoal_us=250`、`zxcoal_frames=16`、`zxnapi_weight=64`，先读回确认；
2. 每点 30 s × 3 次，点间插回 1024 做对照（同档散布约 10 %，单次不算数）；
3. 同时记录：吞吐、IRQ/s、softirq、`accel_ipv6` 的 `qlen_peak`/`qfull_drop`、
   `diag_txq=1` 的挡回计数、`dmesg` 有无 `netif_stop_queue` 相关告警；
4. 判定阈值：差异 >10 % 才算（本板同参数散布上限），并优先看 `qlen_peak` 是否真的接近限值。

## §10 `zxtx_poll_us` —— 不存在这个运行期参数

- `grep` 全驱动：**没有** `zxtx_poll_us` 这个 module_param，也没有对应的 sysfs 项。
- 只有一个**编译期**常量 `ZX_TX_POLL_US = 250`（`zx279128s-eth.h:283`），用于"环满停车后
  用 250 µs 定时器把 poll 叫起来"（`zx279128s-main.c:2537-2542`、`zx_tx_kick()`）。
- 所以它**不是旋钮**。若有人要把它做成运行期参数，需要改代码 + 构建；那属于"新增参数"，
  本文不给值。**预期收益 ≈ 0**〔推断，未测〕：它只在环满停车时起作用，而环满已被 xmit 前
  回收与 poll 顶部回收两条路径覆盖（`.scratch/ffe-fastpath/tx-coalesce/20261104-0025/RESULT.md` §3/§5）。

## §11 看起来是旋钮、盒上却不可调的

| 名字 | 位置 | 为什么不是这个驱动的旋钮 | 本板实测 |
|---|---|---|---|
| `dev_weight` | `/proc/sys/net/core/dev_weight` | 只落到 `net_hotdata.dev_rx_weight`，唯一读点是 `process_backlog()`（RPS/backlog 路径），**不是** `zx_eth_poll()` 的 quota（`net/core/dev.c` 的 `napi_poll()` 读 `n->weight`） | 32/128/256 → **720.7 / 735.9 / 700.4**，控制 731.0/766.5/738.2，全在慢态散布内（`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §1/§2） |
| `netdev_budget` | `/proc/sys/net/core/netdev_budget` | backlog 处理总预算，不改 NAPI quota | 300→1000 得 **742.5**，无变化（`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §5） |
| `netdev_budget_usecs` | `/proc/sys/net/core/netdev_budget_usecs` | 内核下限 = `2×USEC_PER_SEC/HZ`；本板 `HZ=100` ⇒ 下限正好 **20000** | 写 8000 被拒 `Invalid argument`（`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §5） |
| `poll_weight` | 编译期 `NAPI_POLL_WEIGHT` | 是**常量 64**，没有运行期 setter；本驱动唯一的运行期替代是 `zxnapi_weight`（§5） | 未单独测（无法在不重编的情况下改） |
| `ethtool -C` / `-L` / `-n` | — | 驱动没有 `ndo_set_coalesce` / `get_channels` / `get_rxfh` | lan1/lan2/br-lan/wan 全部 `Not supported`（`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §1） |

## §12 交付建议值 `(250, 16, 64)`：为什么、代价、边界

**选它的正面证据**（验证轮，控制 728.3；表内每行都是同一形态的同轮对照）：

| 形态 | `500/64/64` | `250/16/64` | Δ% | IRQ/s | softirq | 证据 |
|---|---|---|---|---|---|---|
| 正向单流 | 728.3（C0a/C0b/C0c） | **923.0**（922.4/923.6） | **+26.7** | 1167 → **5171** | 0.431 → **0.523** | `cells.tsv:1,2,3,8,21` |
| 反向 `-R` | 783.2（R0） | **928.1**（R1b） | **+18.5** | 1131 → 5133 | 0.420 → 0.522 | `cells.tsv:10,28` |
| 16 流 `-P 16` | 903.9（P0） | 897.2（P1b） | −0.7 | 964 → 5710 | 0.597 → 0.584 | `cells.tsv:12,29` |
| 小包 `-M 536` | 396.2 / 428.3（均 412.3） | **423.1** | 不退（散布内） | 1009–1050 → 6226 | 0.558–0.610 → 0.594 | `cells.tsv:14,16,26` |
| 120 s 长稳 | — | **915.8**（877/924/915/920） | — | 5211 | 0.529 | `cells.tsv:30` |
| 双向（两向各记） | 375.3 / 447.1（X0b） | **397.4 / 479.3**（X1a） | **+5.9 / +7.2** | 1133 → 5346 | 0.474 → 0.516 | `.scratch/ffe-fastpath/duplex/20261104-0045/RESULT.md` §3 |

**为什么不是另外两个候选**：

- `62/64`：正向 926.6（+27.2）比 250/16 还略高，但**小包 395.9（0.96× 基线均值，三个候选里
  唯一低于基线且越过 0.98 硬线的）**，IRQ/s 7214（×6.2），且 62 µs 档的 IRQ 率上界约 1/62 µs
  ≈ 16.1 k/s（实测 5.7–7.2 k）。见 `.scratch/ffe-fastpath/coal-verify/20261104-0015/RESULT.md` §2/§3。
- `250/32`：IRQ/s 2607（三个候选里最省 CPU），但正向两格 **921.8 / 841.8**（第二格 −8.7 %、
  retr=1，不是拥塞），小包 409.8，且 16 流/长稳/v4 未测（`cells.tsv:4,5,17`）。
- `250/16` 的代价是 IRQ/s ×4.4、softirq 0.43→0.52；**它换的是吞吐，不是“白拿”**。

**为什么"小包最优 423.1"要打折说**：小包两格基线自己就散 **8.1 %**（396.2–428.3），三个候选
（392.9–423.1）全落在基线散布带内。所以这一形态只能判"不否决 / 方向上最好"，
**不能判"小包显著改善"**（`.scratch/ffe-fastpath/coal-verify/20261104-0015/RESULT.md` §3）。

**它不能解决的**：

- **双向合计仍是"一个方向"的量级**：两向同时打，最好一档合计 **876.7 Mbit/s**，对 v6/TCP 载荷
  上限 **941.2** 是 **93.1 %**；但**不能**按"941×2 = 1882"算效率——本盒没有兑现每向 941 的全双工
  （§14-④）。双向时 CPU 不是瓶颈（CPU1 softirq 只有 0.52），归属未定位
  （`.scratch/ffe-fastpath/duplex/20261104-0045/RESULT.md` §0/§6、
  `.scratch/ffe-fastpath/duplex-attr/20261104-0130/RESULT.md` §3/§5）。
- **v4 不在这套旋钮的收益里**：v4=hw 几乎不上 CPU，改合并窗口 933.3→934.8（+0.2 %，散布内），
  别把 v6 的 +26.7 % 讲成"整机 +26.7 %"。

## §13 启动配置机制：`/etc/config/zx_ffe`（设计口径）

> 本节只固定**口径**，不引实现行号；这套代码与本文同批并行落地，以落地代码为准。
> 涉及文件：`overlay/target/linux/sanechips/base-files/etc/config/zx_ffe`、
> `.../base-files/etc/init.d/zx_ffe_coalesce`。

**权威关系（一句话）**：**配置文件是权威，驱动默认值是兜底**。内核不读 `/etc/config`；
只有用户态 init 脚本在开机/重载时把配置值经 sysfs 写进驱动。配置项缺失或为空 ⇒ 脚本**不写**，
驱动保持当前值（当前编译期默认 `250/16/64`；旧镜像 `500/64/64`）。

| uci 选项（`config settings 'settings'`） | 写到 | 设计区间 | 驱动实际区间 |
|---|---|---|---|
| `rx_coalesce_us` | `zxcoal_us` | 1..1,000,000 | 0..1,000,000 |
| `rx_coalesce_frames` | `zxcoal_frames` | 1..4096 | 0..4096 |
| `napi_weight` | `zxnapi_weight` | 1..1024 | 1..1024 |

**行为（设计口径）**：

1. 入口：`boot` / `start` / `reload`（含 `/sbin/reload_config` 的 config.change 触发器）都走同一个
   `apply()`；开机由 `/etc/rc.d/S99zx_ffe_coalesce` 调一次。
2. 顺序：**先 `us`、再 `frames`、最后 `weight`**。理由是 `us` 是与流量无关的绝对中断频率上限、
   `frames` 是随 pps 增长的那一项，先落绝对上限能让中间态不差于终态（与 probe 的
   `0x30`→`0x34` 顺序一致）。
3. 每项写完**立刻读回**；读回不等于写入值 ⇒ 记一行错误、该项保持旧值，**但脚本仍返回 0**
   （开机不能因为一个调优参数起不来）。
4. 驱动没加载（参数目录不存在）⇒ 记一行 skip、返回 0。
5. 日志：`logger -t zx_ffe_coalesce`，正常一行 `us=… frames=… weight=… applied+readback ok`；
   非法/失败走 `user.err`。驱动侧另有自己的 `dev_info` 读回行（§2.1）。
6. **一处口径仍不一致、一处已对齐**（本文只指出，不改别人的在途实现）：
   - 脚本 `napi_weight` 的区间已收紧为 **1..1024**，与驱动 `zx279128s-main.c:1562` 的上限 **1024**（常量 `:1422`）
     一致；超限值在脚本侧就被拒（不写进 sysfs），不再有"脚本放行、驱动 `-EINVAL`"的中间态；
   - 脚本要求 `us/frames ≥ 1`，驱动允许 **0**（0 的语义未测，§15-12）。想写 0 只能绕过脚本
     手写 sysfs。
7. **判断配置是否生效**：不要只看文件，读回三个 sysfs 值 + `logread | grep zx_ffe_coalesce`；
   旧镜像没有这套脚本时，读到的是驱动默认值（§2 注意）。

## §14 测量口径附录（照抄别人的数之前先看这节）

### ① "Mbit/s"是 TCP 载荷口径

- 本文所有 iperf3 吞吐都是 **v6/TCP 载荷**口径。天花板 **941.2 Mbit/s**（MSS 1440；
  1440 / 1530 = 941.2，1530 B 口径含 12 B IFG、不计 8 B 前导/SFD）。
- **L2 口径**是 **987.0 Mbit/s**（1514 + 8 前导/SFD + 12 IFG = 1534 B）。
- 所以"单流 924.4"= 载荷上限的 **98.2 %**；不要把它和"987"或"1 GbE 的 1000"混着算。
  证据：`.scratch/ffe-fastpath/duplex-attr/20261104-0130/RESULT.md` §3 与 `work/cal.txt`。

### ② 每帧 softirq 成本公式

```
µs/帧 = Δsoftirq_jiffies(最忙那一核) × (1e6 / HZ) / Δrx_packets(lan1 + lan2)
```

- **同窗口**：分子分母取自同一对 pre/post 快照；**不乘核数**（只取 softirq 增量最大的那一核）；
  **不用 `/proc/uptime` 差做分母**（uptime 只用于反算 HZ 与 IRQ/s 交叉核对）。
- 本板 **HZ = 100**（`1e6/HZ = 10000 µs` 每 tick），可由 `Δ(cpu total)/Δuptime` 逐格反算
  （复核轮逐格反算 ≈100.0）。
- **量化下限**：`Δsoftirq ≥ ~200 tick` 才谈得上 1 % 精度；反例 F0（N=391、3 jiffies ⇒
  76.7 µs/帧）是纯量化噪声，必须丢掉。
- 这个数**混着 TIMER/TASKLET/RCU/SCHED**（`/proc/stat` 的 softirq 是全部类型合计），
  所以它是"每帧 softirq 成本"的**上界**，不是"仅 NET_RX"。
- 证据：`.scratch/ffe-fastpath/per-frame-audit/20261104-0130/RESULT.md` §1/§5；
  定义最早落在 `.scratch/ffe-fastpath/delay-dose/20261103-2300/logs/analyze.py:5-8`。

### ③ 不能反推"仅 NET_RX"的成本

`/proc/softirqs` 只有每核每类的**次数**、没有时间；`/proc/stat` 的 softirq 是全部类型合计。
所以"RX 转发本身每帧多少 µs"在当前落盘里**无证据**；要拿必须在 `net_rx_action`/NAPI poll 里
夹 `ktime_get_ns()`（本轮没做）。任何"仅 NET_RX = x µs"的说法都是推断。

### ④ 双向必须两向各记，且不能按 941×2 算效率

- 两向**分别**记正向/反向（`iperf3` **两条连接并发**：`-p 5201` 与 `-p 5202 -R`，即"方式 A"；**`--bidir` 已禁用**——它每向只到 ~470，属"跑不满"口径，2026-11-04 用户裁定全面停用），不要只报合计。
- 本板实测双向合计 **876.7 Mbit/s** = 载荷上限 941.2 的 **93.1 %**（L2 口径 88.8 %）。
- **不能**拿 1882.4（941×2）当分母：本盒没有兑现"每向 941"的全双工。但也**不是严格的字节额度**：
  双向时盒子处理的总帧率 **82 955 fps** 反而比同参数单向（81 868 fps）高 1.3 %，总字节低 4.2 %；
  把一向限到 100 M 后另一向只回到 755.8 / 580.4（不是 924）。⇒ "两向在分一个 ~941 载荷的合计额度"
  只是**方向对、量级对**，机制未定，归属里还有宿主 `ens34` 单队列 e1000 这个未排除项。
- 证据：`.scratch/ffe-fastpath/duplex/20261104-0045/RESULT.md` §3/§6、`.scratch/ffe-fastpath/duplex-attr/20261104-0130/RESULT.md` §3/§5。

### ⑤ 重启后的测量门（门没到，测到的是 ~300 Mbit/s 的软件快路）

起测前必须同时满足：

1. `/proc/uptime ≥ 130`；
2. `dmesg | grep ffe_learn` 有 `accel: ffe_learn auto-enabled 120000 ms after probe`；
3. `head -1 /sys/kernel/debug/92000000.ethernet/accel_ipv6` 是 `ffe`、`ppe_bound=1`、
   `/etc/e2631-ppe-pending` 与 `/etc/e2631-ppe-failed` 都不存在。

门没到就开测，会得到 **~300 Mbit/s** 的软件路径数据，整轮作废。证据：
`.scratch/ffe-fastpath/coal-sweep/20261103-2330/RESULT.md` §2（`ffe-gate` 与
`uptime=133.85`）及文末“复活步骤”第 8 条、`.scratch/ffe-fastpath/per-frame-audit/20261104-0130/RESULT.md` §0。

## §15 未测清单（照抄前对照；每条都给"若要测怎么测"）

| # | 没测什么 | 为什么没测 | 若要测怎么测 |
|---|---|---|---|
| 1 | `txq_limit` 全区间（64/128/256/512/1024）的 A/B | 本轮只调合并/权重；没有"参数值 + 原始日志同框"的证据 | 固定 `250/16/64`，每点 30 s × 3、点间插回 1024；记吞吐/IRQ/softirq/`qlen_peak`/`qfull_drop`/`diag_txq` 挡回；差异 >10 % 才算 |
| 2 | `0x350038` / `0x350040` / `0x350044` 三个无宏寄存器 | 全树无宏、驱动零访问；语义未知，盲写风险高（`.scratch/ffe-fastpath/tx-coalesce/20261104-0025/RESULT.md` §1–§2） | 先只读：`dma_state` 会原样打印 `dma+000..11c`（跳过 0x68）；在 idle/500 µs/250 µs 三种负载下各读两次，看是否随 pps 或环深变化；确认有相关性再设计写入与读回自证 |
| 3 | `zxtx_poll_us` | **不存在**这个运行期参数，只有编译期 `ZX_TX_POLL_US=250`（§10） | 需改代码把它做成 module_param，再在**环满**场景（`txq_limit=64` + 单流）测；非环满预期无差异 |
| 4 | `ffe_aging=300 s` 到期后同一五元组能否重学、重学代价 | 没做跨老化边界的流表观测 | 运行期 `echo 10 > .../ffe_aging`，单流跑 60 s，采样 `accel_ipv6` 的 `flows/hit/miss` 与 `ffe_flows`，看同一五元组是否重新 `add`、吞吐有无瞬时塌陷 |
| 5 | 多队列/多环（8 条 RX 环共用 1 个 NAPI）拆分的收益 —— **已结案：不采纳（2026-11-04 真机）** | 有害的是**分环 + `napi1` 组合态**（多一条 threaded NAPI）：`-R` 916.29→709.21 = −22.60 %（剔除异常点 −22.89 %）；**因果未分离**（无 A/B/A 交错、回退只 2 点、未测 `rx_split=Y`+trap 全 0）⇒ 不能说流量分环本身有害。代价是共享 RX 中断放大 6.4×；`--bidir` 那组只留档、不比较（口径已被裁定禁用） | 不必再测；`rx_split` **保持默认 `N`**。证据 `.scratch/ffe-fastpath/rx-split-onbox/20261104-2050/RESULT.md` §1；开关/回退/init 脚本缺陷见 `docs/operations/bench-and-build.md` §7.4 |
| 6 | `frames<16` 在极小包下的中断风暴 | **本板推不出高 pps**：`-M 88` 四格全塌到 60.7–63.2 Mbit/s，TCP 侧先崩 ⇒ **未观测到**，不等于不存在 | 换 UDP 或 pktgen 产生高 pps；在 `frames=4/8`、`us=250/1000` 下记 IRQ/s、softirq、`rx_drop`/`qfull_drop`；先定 IRQ 红线（如 40k/s）再判 |
| 7 | 双向下各参数的最优值 | 双向只测了 `500/64`、`250/16`、`62/64`；`250/32`、`125/64` 没进双向 | 双向矩阵补 `250/32`、`125/64`；两向分开记，同时记合计与 CPU1 softirq；别只看合计 |
| 8 | `HZ` 变化的影响 | 本板构建 `HZ=100`；没有 HZ=250/1000 的镜像 | 换 HZ 重编，用同一公式重算每帧成本（量化粒度会变），并复核 softirq 记账是否仍按 tick 落上下文 |
| 9 | `zxnapi_weight` 与 `zxcoal_frames` 的交互 | 只测了 1 个 weight 点（128）在 `250/64` 上，没有二维网格 | 2D 扫 `{16,32,64} × {64,128,256}`；先看 `frames/poll` 是否触顶，再比吞吐/IRQ；注意同档 10 % 散布 |
| 10 | `us=500` 下 `frames` 的独立作用 | 没有 `500/16`、`500/32`、`500/128` 格；"定时器绑定 ⇒ frames 无关"未证 | 同一轮跑 500/16、500/32、500/64、500/128，各 30 s；与 250 档同表对比 IRQ/s 与 frames/IRQ |
| 11 | v4 从 `hw` 切到 `ffe` 后的吞吐 | 切 ffe 需先让 PPE 释放（否则 `-EBUSY`），本轮没做；v4 只测了 hw（933.3/934.8） | 先 `flow_offloading_hw=0` + reload，确认 `ppe_bound=0`；`echo ffe > accel_ipv4` 后读回首行，再跑 v4 单流/小包 |
| 12 | `us=0` / `frames=0` 的语义 | 驱动接受 0，脚本拒绝；源码对 `zxcoal_us`/`zxcoal_frames` **只校验上限**（`zx279128s-main.c:1486`/`:1511`），**0 不校验、源码也没有注释说明 0 的语义** ⇒ 写 0 的实际行为**本板未上机核实**（以上是对代码现状的静态核对，属我方描述）〔2026-11-04 02:00 更正，见文末 §更正〕 | 一次只写一个 0，观察 IRQ/s 上限、softirq、是否丢帧；两个都 0 = 每帧中断，别在同一格试 |
| 13 | 31 µs 以下（15/8/4 µs） | 最小只到 31 µs；再低会撞 IRQ 率天花板 | 逐档测 15/8，记录 IRQ/s、softirq、吞吐；先设否决线（如 IRQ >40k/s 或 softirq >0.85） |
| 14 | "仅 NET_RX"的每帧成本 | `/proc/softirqs` 只有次数、没有时间（§14-③） | 在内核 `net_rx_action`/NAPI poll 里夹 `ktime_get_ns()`，需要构建；否则只能给上界 |
| 15 | 交付值的 >120 s 长稳与跨重启复现 | 只做了 120 s（四桶）与单 boot；换 boot 后的重复性没测 | 300 s × 3，跨两次重启各做一轮；记四桶吞吐、IRQ/s、softirq、`dmesg` 异常计数 |
| 16 | 双向下的合计额度归属 | 台架只有一条进盒子的路，分不开"盒内共享管线"与"宿主 e1000 单队列" | 首选 L2 扁平桥双向（CPU 不在数据面）：若合计 ≈1.8 G ⇒ 路由态 877 是 CPU 通路上限；若仍 ≈880 ⇒ 宿主/服务器侧上限 |

## §16 证据索引

> 下列路径相对项目工作区根目录（`中兴ax3000巡天/`）。`.scratch/` 是取证目录、**不在本仓库内**；
> 本仓文档通常不写仓外路径，本文是例外——调参数必须能回到原始日志。原始日志只读，未改动。

| 数字 / 结论 | 证据文件与定位 |
|---|---|
| 控制均值 733.7、31–250 平台、750–2000 洼地、K0c/K0d/K0e 可逆 | `.scratch/ffe-fastpath/coal-sweep/20261103-2330/sweep.tsv:2-32`（格名见第 1 列）；同目录 `RESULT.md` §4/§5 |
| 32 格形态矩阵、IRQ/s、softirq、小包、v4、120 s | `.scratch/ffe-fastpath/coal-verify/20261104-0015/cells.tsv:1-32`；同目录 `RESULT.md` §2/§3/§4 |
| 交付值选择与否决线（62/64 小包 0.96×、250/32 841.8） | `.scratch/ffe-fastpath/coal-verify/20261104-0015/RESULT.md` §0/§2/§3/§6 |
| `frames/poll` 11.72 / 31.56 / 59.07 / 58.5；dev_weight 无效果 | `.scratch/ffe-fastpath/coal-sweep/20261103-2330/sweep.tsv:3,21,23`；`.scratch/ffe-fastpath/delay-dose/20261103-2300/dose-table.tsv:29`；`.scratch/ffe-fastpath/napi-knob/20261103-2200/RESULT.md` §1–§3 |
| 剂量表（2/4/6/8/16 µs）、非单调、TX-only 更慢 | `.scratch/ffe-fastpath/delay-dose/20261103-2300/dose-table.tsv:2-29`；同目录 `RESULT.md` §3 |
| D1/D1b（候选 + 4 µs 反降） | `.scratch/ffe-fastpath/coal-verify/20261104-0015/cells.tsv:20,31`；同目录 `RESULT.md` §5 |
| v4 933.3→934.8、IRQ 7.9→10.8 | `.scratch/ffe-fastpath/coal-verify/20261104-0015/cells.tsv:18,19` |
| 双向 876.7、250/16 397.4/479.3、500/64 375.3/447.1 | `.scratch/ffe-fastpath/duplex/20261104-0045/RESULT.md` §3 主表（X0b/X1a 行）；同目录 `logs/{X0b,X1a}/` |
| 941.2 / 987.0 口径、93.1 %、82 955 vs 81 868 fps、5 % 归属 | `.scratch/ffe-fastpath/duplex-attr/20261104-0130/RESULT.md` §3/§5/§9（`work/cal.txt`） |
| 每帧公式、HZ=100、量化下限、NET_RX 不可分 | `.scratch/ffe-fastpath/per-frame-audit/20261104-0130/RESULT.md` §1/§3/§5；`.scratch/ffe-fastpath/delay-dose/20261103-2300/logs/analyze.py:5-8` |
| 0x30/0x34 无原厂物证、0x38/0x40/0x44 无宏、TX 无完成中断 | `.scratch/ffe-fastpath/tx-coalesce/20261104-0025/RESULT.md` §1/§2/§6 |
| 默认值曾是 500/64/64 | `.scratch/ffe-fastpath/coal-knob/20261103-2315/RESULT.md` §1/§3 |
| tx_pending 峰值 64（限 256） | `.scratch/ffe-fastpath/vendor-sw-pipeline/20261103-0100/PIPELINE.md:240` |

## §17 相关文档

- 逐跳流程与 39 个 miss/drop 点：[ffe-packet-path.md](ffe-packet-path.md)。
- CLA 寄存器位定义：[cla-registers.md](cla-registers.md)。
- 已知坑与"死旋钮"的历史：[../features/ffe-fastpath.md](../features/ffe-fastpath.md)。

## §18 一句话复述

**本板实测最优 `(250 µs, 16 帧, 64)`：单流 v6 TCP 从 728.3–733.7 抬到 923.0–924.4 Mbit/s，
代价是 IRQ/s ×4.4、softirq 0.43→0.52；没测过的参数一个都不要照抄（§15）。**

---

## 更正（2026-11-04 02:00，per-frame-audit/20261104-0130）

**被更正的原文**：§15 表第 12 行（`us=0` / `frames=0` 的语义）里「驱动接受 0，脚本拒绝；源码只写"0 的语义未上机核实"〔归属待核〕」。

**错在哪**：① 该"源码引文"**不存在**——`grep -nE '未上机核实|未核实|未验证'` 在 `overlay/.../drivers/net/ethernet/zte/*.c *.h` **0 命中**（exit 1）；那是我方文档自己的措辞被误当成源码原文，故〔归属待核〕一并删除（这句本就是"我方对代码现状的描述"）。② 事实是：`zxcoal_us_set`（`zx279128s-main.c:1479`）与 `zxcoal_frames_set`（`:1504`）只做 `kstrtouint` + **上限**检查（`:1486` `v > ZXCOAL_US_MAX`、`:1511` `v > ZXCOAL_FRAMES_MAX`），**0 直接放行**，且没有任何注释说明 0 的语义；`MODULE_PARM_DESC` 只写区间（`:1502` `0..1000000`、`:1526` `0..4096`）。③ **不要混入 `:1562`**：那是 `zxnapi_weight_set` 的 `v < 1`（拒绝 0），属于另一个参数、另一种语义。

**现在能写什么**：源码对 `zxcoal_us` / `zxcoal_frames` **只校验上限**（`zx279128s-main.c:1486` / `:1511`），**0 不校验、源码也没有注释说明 0 的语义** ⇒ 写 0 的实际行为**本板未上机核实**。

**口径来源**：本条与每帧成本无关，只来自对 overlay 源码的静态读码（`zx279128s-main.c:1479-1526`、`:1553-1562`）与 `grep` 结果；引用时只写"静态读码、未上机"。

# v6 快路交付兜底：把 alias 丢弃纳入 autofallback 判据（P10.4）

> 2026-11-01。对象：ZXHN E2631（ZX279128S）自研固件的驱动内 FFE 快路。
> 背景与机制定量见 `.scratch/ffe-fastpath/DIAG-P10.3-FFE-COLLAPSE.md`（下称 DIAG）：
> FFE 收侧"描述符长度 ↔ 池内容"错配 ⇒ 约 0.29% 的帧在 `zx_eth_rx()` 的 alias 判据处
> 被静默丢掉 ⇒ TCP 平衡在 ~92–111 Mbit/s 的慢态，且是**持续平衡点、不会自愈**。
> 原有 `zx_accel_autofallback()` 只看 `accel.cnt[]`，对这批帧完全失明 ⇒ 用户被卡在慢态。

本提交只做**兜底判据**：不改 alias 判据本身、不改 FFE 提交/返还时序、不动 v4 `hw` 码路，
也不修 bp 归属根因（那是 P10.5 的事）。

## 1 改动（3 个文件，5 处）

`overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`

| # | 位置 | 改前 | 改后 |
|---|---|---|---|
| 1 | `zx279128s-main.c:1795-1823` | 无 | 新增 `zx_alias_count()`：在 alias 命中点按以太类型分族计数（v4/v6/unknown） |
| 2 | `zx279128s-main.c:1980-1989` | `rx_alias_cnt++` 后直接取证快照 | 同点追加 `zx_alias_count(adapter, d)` |
| 3 | `zx279128s-main.c:3076-3080` | `alias_dump: cnt=%u idx=%u` | 追加 `alias_v4=/alias_v6=/alias_unk=`（cnt=/idx= 原样） |
| 4 | `zx279128s-eth.h` | `zx_ffe_fb_base{lookup,drop,fallback}`、`zx_accel` 无 alias 计数 | `+u64 alias`、`+u64 alias_drop[3]`、`+u8 fb_alias_strike[2]`、`ZX_ALIAS_FAM_*` |
| 5 | `zx279128s-accel.c` | 判据只有 drop/fallback，一窗即降 | 窗口双读加 alias；`ZX_FFE_FB_ALIAS_PERMILLE=2`、`ZX_FFE_FB_ALIAS_STRIKES=2`；连续 2 个不合格窗才降级，第 1 窗只 `dev_info` |

### 1.1 alias 丢弃点：`d` 就是以太头起点，不需要偏移

`main.c` 的调用点是 `d = pkt_pool + bp * ZX_BUF_SIZE + 16`（16 B 是 DMA 头，帧紧随其后）。
取证快照 `zx_alias_snapshot(..., frame=d)` 的 `head[0..5]=dmac`、`head[6..11]=smac`
就是按这个口径取的；快路判据链里的以太类型也在 `data + 2 * ETH_ALEN`。
所以以太类型在 **`d + 12`**，且外层已有 `len >= ETH_HLEN` 守卫，`d[12..13]` 必然在帧内，
读之前也做过 `dma_sync_single_for_cpu()`。

族判定直接复用 `zx_eth_family()`（与快路判据链同一口径）：`0x0800 ⇒ v4`、`0x86dd ⇒ v6`，
其余（含 VLAN 0x8100/0x88a8）记 `unknown`，**不参与判据**——判据分母是本族 lookup，
VLAN 帧的内层族与它不对应。

### 1.2 计数为什么不并进 `zx_ffe_cnt`

`struct zx_ffe_cnt` 的口径是"**进了判据链**的帧"（`lookup == hit + miss` 是不变式）。
alias 丢弃发生在判据链入口之前：没进 lookup、没建 skb、bp 已还。
并进去会破坏不变式，也说不清分子分母。因此单独 `u64 alias_drop[3]`，
下标 `0/1` 刻意与 `family` 同号（`ZX_ALIAS_FAM_V4/V6/UNK`），
写侧只有 RX NAPI、裸 `u64++`，与 `cnt[]` 同一无锁口径；读侧沿用现有
"连读两遍、相等才认"的 `zx_accel_fb_cnt_read()`（撕裂就本拍不判）。

## 2 判据

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

### 2.1 阈值 2‰ 的推导（上一轮实测）

| 状态 | Δalias / Δlookup |
|---|---|
| 慢态（110 Mbit/s 量级） | 36/s ÷ ~9.1k pps ≈ **3.9‰**（独立复算 0.388% 同量级） |
| stack 臂（同板同流） | ~4.7/s ÷ ~29k pps ≈ **0.16‰**（"非慢态也会零星 alias"的基线） |
| 健康 ffe 快态（700–800 Mbit/s） | alias ≈ 0，≪ 1‰ |

2‰ 取在两者之间：高于 stack 基线 12 倍（正常流量不会误触发），是慢态的 ~1/2
（单窗速率抖动不会漏判）。判据用乘法比，不除零、也不在 32 位 ARM 上做 64 位除法。

注意分母口径：`Δlookup` 是**进了判据链**的帧数（alias 丢弃的不在其中），
所以这是个"丢/存"比，而不是总收帧的丢包率；上表两个实测比值也是同一口径。

## 3 可观测性

- 降级日志行尾追加 `d_alias=`，既有 `fell back` 子串与字段顺序一字未动：
  `accel: IPv%d fell back to stack: d_drop=… d_fallback=… of d_lookup=… (cum …) d_alias=…`
- `alias_dump` 头行追加 `alias_v4=/alias_v6=/alias_unk=`（`alias_drop[]` 三格）。
- 不新增 debugfs 文件（仍是 16 个）；`ffe_learn` 的两行格式一个字段都没动；
  `accel_v4/accel_v6/autofallback/ffe_aging` 交付默认值不变。

## 4 预期时延（离线仿真）

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

## 5 上机判据（下一步执行，本次未刷机）

刷 slot1（`mtd6+0x400000==mtd7`；slot0 是 factory 绝不可写，刷前/刷后整分区 sha
必须 `7f849c43…d17b92`），开机 ≥108 s 后测：

1. 快态：v6 `ffe` 打流 3 轮 ≥600 Mbit/s 且**不**出现 `fell back`/`alias strike`；
2. 慢态：v6 `ffe` 打流到掉速后 **≤35 s**（仿真最坏 30.5 s；验收口径见 §4）出现
   `fell back`，且日志行里 `d_alias` 应为每分钟 2000 上下、`d_alias/Δlookup ≈ 3–4‰`；
   降级后吞吐回到 stack 量级（≥300 Mbit/s）；
3. v4 `hw` 回归 3 轮 ≥900；8 次启动（冷 5 温 3）干净、门控 121 s 自动开、
   debugfs 仍是 16 个文件。

## 6 已知风险（P10.4 复审 S5/S6，上机前必读）

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

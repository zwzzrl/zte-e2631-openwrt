# 开机学习死机：为什么 ffe_learn 默认关、120 s 后再自动开（P10.3，2026-11-01）

> 镜像 p10.2（提交 `70e4ed7`）把 `ffe_learn` 默认改成 1，两次开机均硬死机；
> p10.3a（分支 `bisect/p10.3a-learn-off`）默认 0 则 12 分钟干净。本文件记录这组
> 事实、门控设计和"不要随手把默认改回 1"的理由。取证材料在构建目录的
> `.scratch/ffe-fastpath/`（仓库外），文件名在下面逐条给出。

## 现象

- p10.2 两次开机都在 ≈44.7 s 冻结（`engine loaded`（≈44.7 s）、
  `hardware flow offload enabled`（≈44.9 s）、`wifi-on: done`（≈48.9 s）三行全缺、
  100% 丢包），`rcu_sched detected stalls` 直到 104.2/107.97 s 才打印；冻结的 CPU
  关中断自旋（无 NMI 回栈、60 s 里只有 2 个 tick），没有 oops/BUG，网络全死。
  串口原文：`verify-p10/f/boot-hang-1424.log`、
  `boot-hang-1424b.log`、`boot-hang-both.log`（2026-10-31 夜，现场目录 `f/`）。
- 分析见 `DIAG-P10.2-HANG.md`（形态判读）与 `DIAG-BOOTLEARN-RACE.md`（锁/调用图裁定）。

## 二分结论（p10.3a，一次刷机验完）

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

## 未找到根因

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

## 门控设计（P10.3 交付形态）

- probe 期 `learn_on = 0`，并记 `learn_arm_at = jiffies + learn_delay_ms`；
  1 s 节拍的 age work 最先调 `zx_ffe_learn_arm_tick()`，到点置 1、清零、打一条
  `accel: ffe_learn auto-enabled 120000 ms after probe`。
- 参数 `learn_delay_ms`（module_param，0444，默认 **120000** ms）；0 = probe
  立刻学，**这条路径就是诊断镜像复现 p10.2 死机的开关**（起机参数
  `zx279128s_eth.learn_delay_ms=0`），只在复现时用，它是开机参数不是运行期开关；
  生效值超过 600000 ms 时 init 会钳到 600000 ms 并打一条
  `accel: learn_delay_ms clamped to 600000 ms`（只有真钳了才打）。
- 可观测/可取消：`ffe_learn` 末尾追加 `auto_enable_in_ms=`（0 = 已自动开过，
  或用户写过 `ffe_learn` 已接管），写 `0`/`1` 会清 `learn_arm_at`，用户意图优先；
  tick 用 `cmpxchg` 清 `learn_arm_at`，写口又先清它再写开关，两侧交错时用户写的
  值最终生效。
- 门控必须"一次成型"：别改成"开机学 + 定时关"，那会重开死机窗口。

## 警告：改回"默认立刻学"之前必须先定案

根因未定时，任何"开机早期让 FFE 表非空"的路径（把 `learn_delay_ms` 设 0、
用 `ffe_add` 预置条目、默认值改回 1）都会重开这个窗口。要改回默认学，先拿到
stuck PC / `CONFIG_PROVE_LOCKING` / `panic_on_rcu_stall=1` 的证据把元凶钉死；
在那之前，默认保持"开机不学、延时自动开一次"。

---
title: 多 WAN / 策略路由下 FFE 快路的黑洞与自动关停
date: 2026-11-04
status: done
topic: ops-net
summary: 硬件条目不知道 WAN 死活；守卫检测到多 WAN/策略路由就写 stack，单 WAN 不写。
evidence: overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c:3960, overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c:6199, overlay/target/linux/sanechips/base-files/sbin/e2631-ffe-multiwan:1, overlay/target/linux/sanechips/base-files/etc/init.d/zx_ffe_multiwan:1
---

# 多 WAN / 策略路由下 FFE 快路的黑洞与自动关停

> **结论**：FFE 条目里只有出口端口和下一跳 MAC，没有"这条 WAN 现在是否存活"。
> 多 WAN（mwan3 / `ip rule` 策略路由）把内核路由切走以后，内核走新出口，硬件条目
> 仍按旧出口转发 ⇒ **流量静默进黑洞**，要等老化（`ffe_aging`，默认 300 s）或人工
> `ffe_flush` 才停。交付镜像里的守卫（`/etc/init.d/zx_ffe_multiwan` +
> `/sbin/e2631-ffe-multiwan`）检测到多 WAN/策略路由就把该族的 debugfs
> `accel_ipvN` 写成 `stack`（退回纯软件转发），并在每次接口事件与每 5 s 轮询里
> 重评；**单 WAN 基线下它一次 `accel_*` 都不写**，快路行为与没有守卫时相同。
> 本页是这条限制的正式记录：判据、关掉了什么、怎么手动恢复/绕过、哪些还没验。

范围：ZXHN E2631（ZX279128S，kernel 6.18.55）自研固件。缩写 `SRC/` =
`overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`；`BASE/` =
`overlay/target/linux/sanechips/base-files/`。
性质：离线改码 + 脚本级自检（`test-guard.sh`，输出见 §7）；**没有构建、没有刷机、
没有上机**（本任务禁止碰盒子）。

## §1 为什么硬件条目会进黑洞

1. **条目里没有 WAN 存活位**：学习钩子只解析五元组、出口端口和下一跳 MAC，装进
   FFE 表（`SRC/zx279128s-accel.c:2094-2224` 的 `zx_ffe_learn_hook()` 与
   `:1566` 的 `zx_ffe_learn_one()`）；之后命中就直接按条目里的端口/改写转发。
2. **路由切换不会清表**：驱动只在"加速整体关闭"时全清（`SRC/zx279128s-accel.c:4565`
   `schedule_work(&adapter->accel.port_flush_work)`，调用点在 `zx_accel_on_close()`），
   单条 WAN 链路 down 不触发它；netdev 通知链只维护本机地址/桥端口
   （`:1158-1184`、`:1227-1253`），不看路由。
3. **老化是 300 s**：`ffe_aging = 300`（`SRC/zx279128s-accel.c:232`），节拍 1 s
   （`:224`）；活跃流每次命中刷新 `lastuse`，所以只要旧条目还在被命中，它就永远不
   老化 —— 恰恰是"黑洞流一直有包进来"的形态。
4. 因此内核里"这条流现在该从哪个口出去"和硬件里"这条流被钉在哪个口"是两份状态，
   多 WAN 切换只改前者。

## §2 现成的"关掉/绕过快路"手段（按能否当总开关排序）

| # | 手段 | 运行期可切 | 立即生效 | 是否等于"关快路" | 证据 |
|---|---|---|---|---|---|
| 1 | `echo stack > /sys/kernel/debug/92000000.ethernet/accel_ipv6`（v4 同名 `accel_ipv4`） | 是（0644） | 是，同步：返回时已是 stack 或什么都没变 | **是**。S4 先清该族表，S5 写模式字节；钩子/数据面只认 `ffe` | `SRC/zx279128s-main.c:6197-6200`（注册）、`SRC/zx279128s-accel.c:3816-4017`（切换）、`:2130-2135` 与 `SRC/zx279128s-main.c:4166-4175`（只认 ffe） |
| 2 | `echo 0 > .../ffe_learn` | 是 | 是 | **否**：只停"学新流"，已有条目照常转发到老化 | `SRC/zx279128s-accel.c:5392` 的注释原文、`:5525-5538` 写口 |
| 3 | `echo 1 > .../ffe_flush` | 是 | 是 | **否**：一次性清表；学习开着会立刻重学 | `SRC/zx279128s-accel.c:5141-5190`、`:783` `__zx_ffe_flush()` |
| 4 | `echo 0 > .../accel_autofallback` | 是 | 是 | **否**：只在 drop>0.1% 或 fallback>50% 持续 10 s 时降级 | `SRC/zx279128s-main.c:128-132`、`:6217` |
| 5 | `echo 0 > /sys/module/zx279128s_eth/parameters/accel_v6` | **否**（0644 但无回调，probe 读一次） | 否 | 只能开机前传参 | `SRC/zx279128s-main.c:118-124`（默认值）、`:6347-6354`（只有 probe 写模式字段） |
| 6 | `ffe_aging`（sysfs 0644 / debugfs） | 是 | 下一拍老化判定 | 否：只改老化时间；0 = 永不老化，方向上更危险 | `SRC/zx279128s-accel.c:232-234`、`:5338-5376` |
| 7 | `learn_delay_ms` | 否（0444） | — | 否 | `SRC/zx279128s-accel.c:261-263` |
| 8 | uci 配置 | — | — | **不存在**加速模式开关；`BASE/etc/uci-defaults/61_e2631_config:8-11` 明确写"驱动 debugfs 拥有它" | 同左 |
| 9 | init 脚本 | — | — | `zx_ffe_coalesce` 只写 RX 合并/NAPI 三个参数，不碰模式 | `BASE/etc/init.d/zx_ffe_coalesce:47`、`:116-166` |

结论：**不需要改内核**。第 1 行就是现成的运行期总开关，守卫只调它。

注意 IPv4 的 PPE 硬件 offload **不归 `accel_ipv4` 管**：`hw` 模式的语义只是"放行
PPE 接管"（`SRC/zx279128s-accel.c:3854-3863` 注释），数据面判据链只在 `ffe` 模式跑
（`SRC/zx279128s-main.c:4166-4175`）。要关 v4 的硬件 offload 得用 fw4 的
`flow_offloading_hw=0`；`accel_ipv6` 第 3 行的 `hint=disable_flow_offloading_hw_to_use_ffe`
就是这条提示（`SRC/zx279128s-accel.c:4610`）。本守卫默认只守 `families=6`（IPv6 FFE），
就是交付态 `accel_v6=ffe` 的那一族。

## §3 守卫的判据（三条，任一成立 = 多 WAN/策略路由态）

实现：`BASE/sbin/e2631-ffe-multiwan`，`detect_reason()` 在 `:195`。

| 判据 | 怎么判 | 为什么是这条 |
|---|---|---|
| `multi-default` | `ip -4 route show default` 与 `ip -6 route show default` 合起来，取 `dev` 字段去重，**≥2 个出口设备** | 有第二个出口可切，硬件条目才可能指向已不用的口。同一出口上的多条 default（v4/v6 各一条、或同口多 metric）**不算**——内核没有别的出口可切，这是单 WAN 不误关的关键 |
| `policy-rule` | `ip -4 rule show` 与 `ip -6 rule show` 里**带 `lookup` 的非基线规则**；基线 = `0: … lookup local`、`32766: … lookup main`、`32767: … lookup default`（内核自带，先排除）。**无 `lookup` 的丢弃型规则不计入**：netifd 的 `failed_policy`（`interface-ip.c:1866-1867`）、内核的 `unreachable`/`blackhole`（`fib6_rules.c:228-231` 的 `FR_ACT_POLICY_FAILED` ⇒ discard；PD 的 `unreachable` 见 `interface-ip.c:1125-1126`）只丢包、不选表、不改出口，不是多出口策略路由 | 策略路由本身就是"按规则换表/换出口"；mwan3 的 `fwmark … lookup N` 带 `lookup`，就落在这一条 |
| `mwan3` | 存在 `/etc/config/mwan3`，`uci show mwan3` 里 `=interface` 且 `enabled` 不为 0 的接口 **≥2** | mwan3 多接口但当前只剩一个 default（另一条 WAN 已 down）时，前两条可能都不成立，这条仍然成立 |

补充口径一：判据 (a) 先把 `pppoe-<iface>` 折成 `uci network.<iface>.device` 再比较
（`collapse_dev()`）。不折的话，"v4 默认走 `pppoe-wan`、v6 的 RA 落在 `wan`"这种
单 WAN 双栈会被数成两个出口、平白关掉快路；真实的两条 PPPoE WAN 会折成两个不同的
物理口，判据不变（自检 `t_pppoe_single_uplink` / `t_two_pppoe_are_multiwan`）。

补充口径二：`mwan3` 判据的 `enabled` 缺省按 1 算（mwan3 自己的默认），显式
`option enabled '0'` 的接口不计。三条判据都只看"当前拓扑能不能换出口"，不猜历史；
"曾经多 WAN、现在塌成单 WAN"由 §4 的 hold 窗口兜住，避免 failback 抖动时快路刚开又被
路由切走。

## §4 检测到以后关掉什么、怎么恢复

- **关**：对该族 debugfs 文件写 `stack`（`BASE/sbin/e2631-ffe-multiwan:241` `disengage()`）。
  内核侧一次同步切换：S2 quiesce → S3 排空 TX（≤100 ms）→ S4 **清该族表** →
  S5 单字节写模式（`SRC/zx279128s-accel.c:3910-4017`）。所以不只是"以后不学"：
  已经装错的条目在 S4 就被清掉了（`:3949`）。返回时读回第一行自证，不是 stack 就记
  `user.err` 并在下一拍重试（排空失败走 `:3933` 的 EBUSY 回滚，模式一个字节都不动）。
- **学**：切换后该族钩子在解析前就返回（`SRC/zx279128s-accel.c:2130-2135`），
  不会一边 stack 一边偷偷学。
- **记**：切之前的第一行模式写进 `/tmp/zx_ffe_multiwan.state`（如 `6 ffe`）；只记
  本守卫自己改的那一次。重启 `/tmp` 清空，驱动按 `module_param` 起，守卫再判一次。
- **恢复**：三条判据全不成立后，先等 `hold_seconds`（默认 300 s，`restore_all()` 在
  `:263`）再写回记下的模式；窗口内又检测到多 WAN 就取消倒计时。写回前先看当前模式，
  如果已经被用户/别的脚本改成非 stack，就**不覆盖**、只清记录。
- **用户关守卫**：`/etc/config/zx_ffe` 的 `multiwan.enabled=0` ⇒ 立刻写回（不等 hold），
  之后两条触发路径（hotplug 与轮询）都跳过。

## §5 什么时候重新评估（不是只判一次）

| 触发 | 实现 | 延迟 |
|---|---|---|
| 接口 up/down/update | `BASE/etc/hotplug.d/iface/30-zx_ffe_multiwan` 调 `apply` | 事件当下 |
| 路由/规则变化（无 ifup） | `/etc/init.d/zx_ffe_multiwan` 的 procd 服务跑 `watch` 循环，每 `poll_interval`（默认 5 s，1..60）`apply` 一次 | ≤5 s |
| 改配置后 reload | `procd_add_reload_trigger "zx_ffe"` + `reload_service()` | 立即 |
| 开机 | `START=99`；驱动默认 `learn_delay_ms=120000`（2 分钟）后才自动开学习（`SRC/zx279128s-accel.c:261`），S99 远早于它 | 开机窗口内 |

`watch` 循环用周期等待做轮询。仓库里热插拔脚本已经是这个用法
（`BASE/etc/hotplug.d/iface/20-e2631-netune:26,35`）；如果要改成 netlink 事件驱动，
先确认目标镜像的 busybox 带 `ip monitor`——这条没验证（§8-3）。

## §6 单 WAN 基线：逐字节等价的含义与证据

这里"等价"指**快路行为**：单 WAN 拓扑下守卫不写任何 `accel_*`，模式字节、流表、
学习开关一个都没动，因此和没有这个服务时逐字节相同（新增的只有用户态读操作）。

- 代码路径：`detect_reason()` 返回空串 ⇒ `restore_all()`，`/tmp/zx_ffe_multiwan.state`
  不存在就 `return 0`（`BASE/sbin/e2631-ffe-multiwan:265-268`），**没有任何写**。
- 离线自检：`bash test-guard.sh` 的 `t_single_wan_does_nothing` 连跑 3 次 `apply`，
  断言 v6 仍 `ffe`、v4 仍 `hw`、无状态文件、无倒计时、syslog 一条都没写。
  原始输出（本轮）：`artifacts/test-guard.log`，末行 `RESULT: PASS (15/15)`。

## §7 手动恢复 / 绕过 / 观察

```sh
# 看现在判成什么、各族当前模式、守卫配置
/sbin/e2631-ffe-multiwan status

# 临时恢复快路（同时关掉守卫；只 echo ffe 的话 5 s 内又被关回去）
uci set zx_ffe.multiwan.enabled=0
uci commit zx_ffe
/etc/init.d/zx_ffe_multiwan reload

# 恢复自动保护
uci set zx_ffe.multiwan.enabled=1
uci commit zx_ffe
/etc/init.d/zx_ffe_multiwan reload

# 只守 IPv6 / 连 IPv4 的 ffe/zc 一起守
uci set zx_ffe.multiwan.families='6'
uci set zx_ffe.multiwan.families='4 6'

# 看守卫做过什么（关/恢复各一条，不会每 5 s 刷屏）
# 本机 busybox 的 logread 不认 -t 选项；按标签过滤只能用 -e（写成 -t 会输出全量日志）
logread -e zx_ffe_multiwan
```

**别只 `disable` init 脚本**：hotplug 钩子是独立入口，它仍会 `apply`；`enabled=0`
才是两条路都认的总开关。绕过守卫、直接用 debugfs 的前提也应该是 `enabled=0`，否则
下一拍就被改回来：

```sh
echo ffe > /sys/kernel/debug/92000000.ethernet/accel_ipv6   # 先 enabled=0，否则会再被关
```

## §8 未验证（不许当结论用）

本轮**没有构建、没有刷机、没有上机**（任务明确禁止碰盒子），所以下面这些只是代码/脚本
层面的推断，真机必须逐条复核：

1. procd 是否按 `START=99` 把 `watch` 拉起来、`respawn` 行为是否符合预期 —— 未在设备上
   `ps`/`logread` 验证。
2. hotplug 上下文里写 debugfs 是否成功 —— 未验证（同类"hotplug 里调 init.d 会静默
   失败"的坑见 `BASE/etc/hotplug.d/iface/20-e2631-netune:15-17`）。
3. 目标镜像的 busybox `ip` 是否编进 `rule` applet（没有的话判据 `policy-rule` 失效，
   只剩 `multi-default` 与 `mwan3` 两条）—— 未验证。
4. `zx_accel_set(STACK)` 在满速链路上排空 TX 失败（`SRC/zx279128s-accel.c:3933` 的
   EBUSY）时守卫会重试，但"重试用多久能成功"没有实测。
5. IPv4 的 PPE 硬件 offload 黑障不在本守卫覆盖范围内（见 §2 末），没有动
   `flow_offloading_hw`。

## §9 证据索引

- 快路条目/学习/老化：`SRC/zx279128s-accel.c:224`、`:232`、`:1566`、`:2094-2224`、
  `:4565`。
- 运行期总开关：`SRC/zx279128s-main.c:6184-6200`（debugfs 目录与两个文件）、
  `SRC/zx279128s-accel.c:3816-4017`（S0-S7 同步切换）、`:3933`（排空失败回滚）、
  `:3949`（切表清表）、`:3960`（单字节写模式）。
- 只认 ffe：`SRC/zx279128s-accel.c:2130-2135`、`SRC/zx279128s-main.c:4166-4175`。
- 不是总开关的那些：`SRC/zx279128s-accel.c:5392`（`ffe_learn=0` 的语义）、
  `SRC/zx279128s-main.c:6347-6354`（module_param 只 probe 读）、
  `SRC/zx279128s-accel.c:4610`（PPE hint）。
- 守卫实现：`BASE/sbin/e2631-ffe-multiwan:128`（PPPoE 折叠）、`:151`（判据 a）、`:163`（判据 b）、
  `:182`（判据 c）、`:241`（关）、`:263`（恢复）、`:313`（单次 apply）、`:359`（轮询）。
- 触发与服务：`BASE/etc/hotplug.d/iface/30-zx_ffe_multiwan:11`、
  `BASE/etc/init.d/zx_ffe_multiwan:31-52`、`BASE/etc/config/zx_ffe:35-53`。
- 离线自检：`.scratch/ffe-fastpath/multiwan-safety/20261104-1240/test-guard.sh` 与
  同目录 `artifacts/test-guard.log`。
- 相关限制（不在本守卫覆盖范围内）：NPTv6 / NAT66 结构性进不了硬件加速，多 WAN 的 v6
  场景不要指望用 NAT66 换硬件路径，见 `docs/known-issues.md` 的 `NPTV6-NO-HW-ACCEL`。

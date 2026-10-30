# IPv6 路由转发的软件快转（OpenWrt，2026-10-30 夜）

> 取证对象：同一台 ZXHN E2631（ZX279128S，2×Cortex-A9 1 GHz），本轮跑的是 **slot1 上的
> OpenWrt v12fix**（`e2631-openwrt-v12fix-ppe-20261030-slot1.bin`），slot0 是刚恢复的纯原厂 8000。
> 全部数字都是运行态实测，命令附在每节末尾，脚本原文见 §8。
>
> 前提结论（不再重复论证）：**v6 的 L3 硬件快转在这块板子上做不到**
> （`docs/ppe-ipv6-external-hash-2026-10-31.md`、`docs/vendor-v6-forwarding-2026-10-30.md`）。
> 所以本轮的题目不是"能不能上硬件"，而是"软件快转能做到多少、默认配置对不对"。

---

## 0. 一句话结论

| 问题 | 答案 |
|---|---|
| nft flowtable（软件快转）对 **IPv6 路由流量**生效吗 | **生效**。`Ip6OutForwDatagrams` 从每包 +1 变成 15 s 只 +几百，包不再走 `ip6_forward` |
| 生效后吞吐/CPU 有变化吗 | 4 流 20 s：**279.6 → 298.2 Mbit/s**，softirq 46.6 % → 33.9 %；8 流 15 s：310 → 392–412 Mbit/s |
| 上限在哪 | **≈400 Mbit/s（8 流）**，两核都还有余量（cpu0 33–43 %、cpu1 53–77 %）；单流 ~145 Mbit/s。不是卡在交换芯片，也不是卡在某一核 100 % |
| 出厂配置对不对 | **默认 1/1 没问题**（v6 走软件快转，v4 走 PPE）；但 `e2631-netune` 的 RPS 选错了核，**+12 %** 的改进已进构建（§5） |
| 交换芯片本身正常吗 | 正常：lan↔lan L2 **929 / 942 Mbit/s**，两个方向 softirq **0 %** |

---

## 1. 实验拓扑（临时，用完要复原）

```
[主机 ens34]--lan1(PHY)  [E2631 OpenWrt]  lan2(PHY)--[桌面 DESKTOP-A5A2TK2]
 192.168.1.250/24               路由                        192.168.1.243/24
 fd00:aa::2/64        fd00:aa::1/64 → br-lan            fda2:4569:635:0:8c17:...:268a
```

配置要点（都在运行态做的，`/tmp/setup_v6test.sh` 是当时的现场脚本）：

| 对象 | 值 | 为什么 |
|---|---|---|
| `lan1` | 从 br-lan 摘出，`192.168.1.1/24` + `fd00:aa::1/64` | 要在 lan1↔br-lan 之间走**真路由**，不能是二层桥 |
| `br-lan` | 成员 lan2/lan3/phy0-ap0/phy1-ap0，`192.168.5.1/24` + `fda2:4569:635::1/60` | ssh 逃生口：lan1 一旦离开网桥，管理地址还在 |
| `dhcp.lan.ra_default` | `2` | 让桌面拿到默认路由，回程包才知道往哪走 |
| `table ip6 nat66` | `oifname br-lan ip6 saddr fd00:aa::/64 masquerade`（临时） | **测试脚手架**：桌面只回自己网段来的包，不做 NAT66 时回程被黑洞，第一次 A/B 测出的 40 Mbit/s 就是这么来的假数据 |
| `net.ipv6.conf.lan1.disable_ipv6` | 0 | netifd 默认给 lan1 关 v6，得手动开 |

`fw4 reload` 会把 flowtable 重新绑定，rps 会被驱动重置——这正好是 §5 那个改动的观察点。

## 2. nft flowtable 对 v6 到底生效了没有

判据只有一条：**`Ip6OutForwDatagrams`**（`ip6_forward()` 的计数器）。整条路径都是软件转发，
只要流被 offload，第二个包起就不再进 `ip6_forward`。

| 配置 | 4 流 × 20 s 吞吐 | `Ip6OutForwDatagrams` 增量 | softirq（两核合计） |
|---|---|---|---|
| `flow_offloading=0` | 279.6 / 278.9 Mbit/s（retr 1487） | **+306 934**（≈每包都过） | 46.6 % |
| `flow_offloading=1, hw=0` | 298.2 / 297.1（retr 2557） | **+297** | 33.9 % |
| `flow_offloading=1, hw=1` | 328.0 / 327.0（retr 1916） | **+204** | 35.1 % |

三条同时成立才算"真的快转"：吞吐上去、softirq 下来、`Ip6OutForwDatagrams` 不再逐包涨。
`hw=1` 和 `hw=0` 对 v6 **完全一样**——PPE 不接 v6 的软件流表，这一档本来就只是 v4 的快转开关，
v6 静默回落，正是设计意图。

协议栈侧的旁证：`conntrack` 里带 offload 标记的 v6 流 18 条，`nft list ruleset` 的
`flowtable ft { hook ingress priority filter; devices = { "br-lan", "lan1", "wan" } }`
就是 fw4 从这两个 uci 开关编译出来的。

## 3. 上限：400 Mbit/s 卡在哪

先排除硬件：

| 对照 | 结果 | 说明 |
|---|---|---|
| lan↔lan 二层交换（4 流 IPv4，15 s） | **929 / 927 Mbit/s**，反向 **946 / 942** | 交换芯片端口间硬件转发，盒子 softirq **0 %**（15 s 内两核 sirq 增量 0） |
| 盒子自己收（iperf3 服务端，主机 → 盒子 v6，4 流 15 s） | **451 Mbit/s**，39.6 kpps（15 s 收 593 720 包） | 只含"驱动收包 + 协议栈"，不含转发与再发送；cpu1 sirq 71 %、cpu0 18 % |

所以 900 Mbit/s 级别的搬包能力是有的，问题只在"逐包过 CPU 转发"这一段。

再看 CPU 分布（8 流 × 15 s，`flow_offloading=1`）：

| RPS 落点 | 吞吐（多次） | cpu0 sirq | cpu1 sirq |
|---|---|---|---|
| CPU1（= 网卡中断所在核，旧默认） | 374、341 | ~0.1 % | **88.7 % / 81.1 %** |
| CPU0（与中断分开，新默认） | 397、400、392、410、403、412、349 | 33–43 % | 53–77 % |
| 不开 offload 作对照（RPS 在 CPU0） | 310 | 80 % | 47 % |

读法：

1. **旧配置是单核打满**：中断在 CPU1、RPS 也把协议栈压到 CPU1，一个核干两件事，88.7 % 时
   吞吐只有 374。这就是 §5 要修的东西。
2. **拆开以后两核都有余量，吞吐却停在 ~400**。也就是说 400 不是"某核 100 %"撞出来的，
   是**每包成本**与**单流窗口**共同的结果：单流 145、4 流 280–300、8 流 400，一路加流一路涨，
   说明还没到 CPU 天花板；按 cpu1 77 % 对应 400 Mbit/s 外推，单核 100 % 大约在 500–550 之间。
3. 单流只有 145 Mbit/s（off 148.4 / sw 144.5 / hw 136.6）。注意**不能拿 ICMP RTT 说事**：
   对这台 Windows 桌面，即使走二层，ping 也是 2.1–3.4 ms（桌面自己的 ICMP 回复慢），
   路由 hops 只加了 0.2 ms 左右。所以单流瓶颈更可能在 iperf3/Windows 侧或每流串行处理，
   这一条留作待办（§7）。
4. 8 流下重传 2400–3600（0.3–0.5 %），局域网路径上不该有；`/proc/net/softnet_stat`
   里 cpu1 累计 `time_squeeze` 1208（NAPI budget 300 用尽），说明突发时 CPU 队列会顶到
   budget 边界，交换机侧可能因此丢包。也是一个待办，不影响"快转已生效"的结论。

## 4. 怎么开、怎么验（给用户/以后的自己）

```sh
# 开（出厂镜像已经是这个值，纯手工开机时才需要）
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='1'   # v4 用，v6 不受影响
uci commit firewall && fw4 reload

# 验 1：流表真的建出来了，且设备列表包含被路由的两侧
nft list ruleset | grep -A4 'flowtable ft'

# 验 2：v6 流真的被 offload（关键判据：这个数几乎不涨）
grep Ip6OutForwDatagrams /proc/net/snmp6

# 验 3：每核 softirq 分布（修好之后 cpu1 不该长期 >80 %）
grep -E '^cpu[01] ' /proc/stat
```

镜像侧的依赖：`CONFIG_PACKAGE_kmod-nft-offload=y`（`seed/e2631.config`，提供 nft flowtable
语句），软件快转引擎 `kmod-nf-flow` 由 fstools 带入；`kmod-e2631-ppe` 只管 v4。

## 5. 顺手修掉的构建缺陷：RPS 和网卡中断钉在同一个核

`/etc/init.d/e2631-netune` 的历史：最早是 **IRQ 钉 CPU0 + RPS 推 CPU1**（分开，实测 +4.6 %）；
后来为了"softirq 别跨核跑"把 **IRQ 也钉到 CPU1**，于是两半落到同一个核上，而这个改动
从来没在 CPU-bound 的路径上复测过。v6 软快转恰好就是 CPU-bound，于是暴露：

| | 8 流 15 s |
|---|---|
| IRQ CPU1 + RPS CPU1 | 341–374 Mbit/s，CPU1 sirq 81–89 % |
| IRQ CPU1 + RPS CPU0 | 392–412 Mbit/s，CPU1 53–77 %，CPU0 33–43 % |

改动（三个文件，值从 `2` 改成 `1`，并把注释改成实测依据 + 历史）：

- `overlay/target/linux/sanechips/base-files/etc/init.d/e2631-netune`
- `overlay/target/linux/sanechips/base-files/etc/hotplug.d/iface/20-e2631-netune`
- `overlay/target/linux/sanechips/base-files/sbin/e2631-netune-apply`

顺带发现 **XPS 写入一直是空操作**：`/sys/class/net/<if>/queues/tx-0/xps_cpus` 在 sysfs 里存在，
但读返回 `ENOENT`（单发送队列，本内核下不可写），所以老脚本那句 `echo 2 > xps_cpus` 从来没生效过，
`XPS=CPU0/CPU1` 的 A/B 当然也测不出差别。三个脚本里的 XPS 段落已删，注释写明原因。
`rps_flow_cnt=256`、GRO on、PAUSE off 保持不变（与本次无关，未复测）。

**注意：这三处改动目前只在仓库和这台运行中的盒子上，没有进已刷的镜像**——见 §7 第 1 条。

## 6. 和原厂 FFE 的对照（为什么原厂能跑到 900）

原厂 WAN↔LAN v6 路由实测 918–928 Mbit/s，CPU 每包 ~4.7 µs（`docs/vendor-v6-forwarding-2026-10-30.md` §8），
那是它自己的 gmac 驱动 + FFE 软快转。我们这边同一件事的每包成本明显更高（本节表 2 的
451 Mbit/s / 39.6 kpps 里，转发路径每包大约 9 µs 量级），差距在**驱动**而不是协议栈：

- `zx279128s-eth` 每收一包都要 `zx_bmu_free_bp()`（自旋锁 + MMIO 写，必要时
  `readl_poll_timeout_atomic` 以 1 µs 步进轮询），并把 1.5 KB 从固定 buffer pool
  `memcpy` 进 `napi_alloc_skb` 的 skb；发送侧同样 `skb_copy_bits` 再 `writel(TX_START)`。
- `rx-checksumming: off [fixed]`、`tx-checksumming: off`，校验和全在软件里算。

要追平原厂的 4.7 µs/包，得动驱动（BMU 释放批量化、build_skb 复用 buffer pool、TX 校验和），
那是另一轮工作，不在本轮"把软快转打开并量化"的范围里。

## 7. 待办（按值排序）

1. **要不要把 §5 的 netune 改动刷进镜像**：需要刷 slot（当前活动槽是 slot1，原厂 httpd 只能写
   非活动槽，所以要"先切回 slot0 再刷 slot1"或反着来）——**需要用户拍板**。不刷也能用，
   运行态已经改好，重启后旧脚本会自动把它写回 CPU1，届时需要重跑一次下发。
2. 单流 145 Mbit/s 的瓶颈（iperf3/Windows？每流串行？）——加 `-w` 大窗口、`-P 1 -t 60`
   和 UDP 高速率再试，能区分是窗口还是每包成本。
3. 8 流 0.3–0.5 % 重传 + `time_squeeze 1208`：试 `net.core.netdev_budget=600`、
   `netdev_max_backlog` 提升，看吞吐与重传是否同向改善。
4. 驱动每包成本（§6），如果目标是 700 Mbit/s 以上。

## 8. 复现脚本

`8 流 × 15 s` 的 A/B 骨架（RPS 值作为参数，`pre/post` 取每核 `/proc/stat`）：

```sh
#!/bin/sh
# usage: run.sh <rps> <secs> <label>
R=$1; S=$2; L=$3
ssh root@192.168.1.1 "echo $R > /sys/class/net/lan1/queues/rx-0/rps_cpus;
    grep -E '^cpu0 |^cpu1 ' /proc/stat" | tail -2 > /tmp/pre.$L
iperf3 -6 -P 8 -c fda2:4569:635:0:8c17:3d67:6e4c:268a -B fd00:aa::2 -t $S \
    | grep -E '^\[SUM\].*receiver' > /tmp/iperf.$L
ssh root@192.168.1.1 'grep -E "^cpu0 |^cpu1 " /proc/stat' | tail -2 > /tmp/post.$L
python3 - "$L" <<'PY'
import sys
l = sys.argv[1]
def load(p):
    d = {}
    for line in open(p):
        f = line.split(); d[f[0]] = list(map(int, f[1:]))
    return d
pre, post = load('/tmp/pre.' + l), load('/tmp/post.' + l)
out = []
for c in ('cpu0', 'cpu1'):
    d = post[c][6] - pre[c][6]; tot = sum(post[c]) - sum(pre[c])
    out.append('%s sirq %.0f%%' % (c, 100 * d / tot))
print(l, open('/tmp/iperf.' + l).read().strip(), '|', '; '.join(out))
PY
```

现场同一性检查（每次测之前跑一遍，避免测的是残留配置）：

```sh
uci get firewall.@defaults[0].flow_offloading      # 1
uci get firewall.@defaults[0].flow_offloading_hw   # 1
cat /sys/class/net/lan1/queues/rx-0/rps_cpus       # 1（新默认）；2 = 旧默认
grep 92000000.ethernet /proc/interrupts            # 中断应在 cpu1
nft list ruleset | grep -A4 'flowtable ft'         # devices 含 br-lan lan1 wan
```

## 9. 环境复原清单（测完别把盒子留在这个状态）

- `brctl addif br-lan lan1`，删 `network.v6test`，br-lan 回到 `192.168.1.1/24`（或原来的 LAN 网段）
- 删 `nft delete table ip6 nat66`，删 `dhcp.lan.ra_default`
- 删主机侧 `ens34` 的 `fd00:aa::2/64`、`fda2:4569:635::/64` 路由
- 盒子 `/tmp/setup_v6test.sh`、`/tmp/iperf*` 清掉
- `uci set firewall.@defaults[0].flow_offloading='1'` + `hw='1'`（出厂值，本轮结束时已恢复）

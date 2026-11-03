---
title: FFE 快路包转发流程：WAN→LAN 与 LAN→WAN 的每一跳
date: 2026-11-04
status: done
topic: accel-impl
summary: 驱动内 FFE 快路（v6 accel_ipv6=ffe，v4 accel_ipv4=ffe 同一份代码）从 RX 描述符到 TX 门铃的完整链路，两个方向各一张流程表，逐条列出 39 个「不走快路/被丢」的位置与对应计数器。
evidence: overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-accel.c, overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c, overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-eth.h, overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts
supersedes:
---

# FFE 快路包转发流程：WAN→LAN 与 LAN→WAN 的每一跳

**结论先行**（详细论证在各节，每条断言后带 `文件:行`）：

1. **这条快路只接"目的 MAC 是本机、目的 IP 不是本机、TCP/UDP、不分片、无扩展头"的转发帧**，
   命中后**不建 skb、不进协议栈**，把 RX 那个缓冲指针 `bp` 原地改写 L2 头 + TTL/hop_limit
   后直接写进**同一个发送环**（`zx279128s-accel.c:2887-3087` 的 `zx_ffe_forward()`，
   `zx_ffe_rewrite()` 在 `:2727-2751`，提交在 `main.c:2832-2918` 的 `zx_tx_submit_bp()`）。
2. **两个方向走的是同一份代码**，差异只在三处：入口 netdev 是谁（独立口还是桥端口）、
   从 conntrack 哪一侧的元组取键、NAT 位是 SNAT 还是 DNAT——见 §9。
3. **学习的 120 s 是"开机后学习开关延时打开"，不是流表老化时间**：`learn_delay_ms` 默认
   120000 ms（`accel.c:261`），probe 期 `learn_on=0`（`:3962-3965`），1 s 节拍到点置 1 并打一条
   `accel: ffe_learn auto-enabled 120000 ms after probe`（`:3876-3893`）；老化是另一个参数
   `ffe_aging=300` 秒（`:232`、`:1286-1304`）。
4. **"v4 走 hw、v6 走 ffe"是交付默认值，不是全体**：模式有 `stack/ffe/hw/zc` 四档
   （`accel.c:5-10`、`eth.h:333-336`），`hw` 只有 IPv4 有（`accel.c:3486`、`main.c:5129-5133`）；
   而 FFE 数据面**只处理"该族当前模式 == ffe"的帧**（`main.c:3498-3511`），
   所以交付态（v4=hw + v6=ffe）下真正走本文这条链路的是 **IPv6**。
5. **RX 中断合并窗口是本轮唯一被实测证实的吞吐旋钮，只是最优点比上游注释标的位置更靠左**：
   缩短 `zxcoal_us` 500→250(或 62/31) 使单流 **735→866–924 Mbit/s**，代价是中断率
   ×1.7–10.6、softirq 占比 0.40→0.50–0.57；拉长到 750–2000 µs 掉到 **478–548**
   〔本板实测，见 §4.3 与 §11〕。

## 0. 口径、缩写与四分类

- 行号基准：**本仓工作树 2026-11-04**（`git status` 里 `zx279128s-{main,accel}.c`、
  `zx279128s-eth.h` 有三处未提交改动，本文行号按**改动后**的工作树 grep）。
  本仓行号会漂，引用前重 grep。
- 缩写：`SRC/` = `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`。
  文中不带目录的 `*.c` / `*.h` 均指 `SRC/`；`DTS` =
  `overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts`。
- **来源四分类**（每条都标）：〔代码事实〕上游驱动代码（`main.c`/`accel.c`/`eth.h` 源自
  Navid Ghahremani 的 GPL-2.0 驱动演化，**不是中兴源码**，注释不是硬件权威）；
  〔原厂物证〕原厂二进制/原厂系统读数；〔本板实测〕本仓台架上的测量；〔推断〕我方推断、未验证。
- **不按行号引用**任何私有记忆文件；证据只引代码、DTS、仓库内文档或本板实测四段式
  （结论 + 关键数字 + 测法 + 判据）。

## 1. 范围与前提

### 1.1 只写这条 FFE，不写别的加速路径

in scope：驱动内 FFE 快路（RX 环 → 改写 → 同一个 TX 环），即 `SRC/zx279128s-accel.c` 的
`zx_ffe_*` 一族 + `SRC/zx279128s-main.c` 的 RX/TX 段。
out of scope（各一句话，不展开）：零拷贝 `zc`（`main.c:3653-3709`，交付态 `zc_mask=0`）、
CLA 硬件流表 v6（`kmod-e2631-ppe`，见 [known-issues.md](../known-issues.md) `PPE-IPV6-TCP`）、
NPU 卸载、PPPoE 卸载（`main.c:3481-3491` 只是窥探钩子）。

### 1.2 四档模式，以及"哪一族真的走 FFE"

| 值 | 名字 | 谁能用 | 语义（代码位置） |
|---|---|---|---|
| 0 | `stack` | v4/v6 | 驱动什么都不做，帧全走协议栈（`accel.c:7`） |
| 1 | `ffe` | v4/v6 | 驱动内快路，本文的对象（`accel.c:8`） |
| 2 | `hw` | **只有 v4** | 只表示"放行 PPE 接管"，驱动不碰帧（`accel.c:9`）；v6 在 probe 被降成 stack（`main.c:5129-5133`），运行期写 `hw` 被 `zx_accel_set()` 拒（`accel.c:3486`） |
| 3 | `zc` | v4/v6 | 零拷贝（`accel.c:10`），本文不写 |

- 默认值：`accel_v4 = ZX_ACC_HW`（`main.c:114`）、`accel_v6 = ZX_ACC_FFE`（`main.c:118`）；
  这两个 **module_param 只在 probe 读一次**（`main.c:5125-5128`），运行期等价物是 debugfs
  `accel_ipv4` / `accel_ipv6`（`main.c:4982-4985`，写口 `accel.c:4294-4327` 接受
  `stack|ffe|hw|zc`）。**两者不是同一个东西，别混**。
- **数据面的族门控**：`zx_eth_rx()` 只在 `zx_accel_cur_mode(accel, fam) == ZX_ACC_FFE` 时才调
  `zx_ffe_rx()`（`main.c:3498-3502`）；非 IP 帧（`fam < 0`）则"只要有一族在 ffe 就送进判据链
  记 miss"（`main.c:3503-3511`）。⇒ **交付态 v4=hw + v6=ffe 时，v4 帧不进本文任何一跳**；
  要测 v4 的 FFE 必须先把 PPE 放开（见 §1.4）。

### 1.3 `ppe_bound` 是什么，它挡住谁

`accel_ipv4`/`accel_ipv6` 第 3 行有 `ppe_bound=`（`accel.c:4259-4265`）。它是
`zx_ppe_has_bound_blocks()` 的即时结果：**有没有 nft flowtable 通过 `ndo_setup_tc(TC_SETUP_FT)`
绑到本 adapter 上**（`accel.c:3494-3496` 读，`4234-4236` 读）。

- `ppe_bound=1` **不等于**"FFE 在跑"，只表示"有硬件流表绑着"。切 v4 到 `ffe` 会被拒并返回
  `-EBUSY`（`accel.c:3527-3534`，提示 `disable_flow_offloading_hw_to_use_ffe`）；
  **切 v6 到 ffe 不看 bound**（`accel.c:3519-3526`），因为 PPE 没有 v6 分类表。
- 所以"v4 用 FFE"的唯一合法姿势是：`uci set firewall.@defaults[0].flow_offloading_hw=0` +
  reload（释放 flowtable）→ `ppe_bound=0` → `echo ffe > accel_ipv4`。

### 1.4 学习门闩（120 s）到底是什么

- 参数 `learn_delay_ms`（module_param，**0444 只读**，默认 **120000**，`accel.c:261-263`）。
- probe 期：`learn_on = 0`、`learn_arm_at = jiffies + 120 s`（`accel.c:3962-3965`）；
  每秒一拍的 age work 先调 `zx_ffe_learn_arm_tick()`（`accel.c:3909`），到点置
  `learn_on = 1`、打一行 dev_info（`accel.c:3876-3893`）。
- 语义边界（**别写成"学习窗口"或"流表 120 s"**）：
  ① 它只控制**学习**（`ffe_learn`），数据面查表/转发与它无关；
  ② 只自动开**一次**，之后不再干预；
  ③ 用户写 `debugfs/ffe_learn` 会清 `learn_arm_at` 并立即生效（`accel.c:5186-5189`），
     这是给开机死机问题留的"用户意图优先"；
  ④ `learn_delay_ms=0` 是**诊断镜像的复现开关**（起机参数，不是运行期开关，`accel.c:256-259`）。
- **为什么是 120 s**：`learn_on=1` 默认值在开机窗口里两次硬死机（≈44.7 s 冻结），关掉后干净
  ——这条历史见 [features/ffe-fastpath.md](../features/ffe-fastpath.md) 附二；**根因未定**。

### 1.5 两种工作态对路径的影响

| 工作态 | 两侧 netdev 关系 | 帧怎么走 | FFE 有没有活干 |
|---|---|---|---|
| **路由态** | WAN 侧与 LAN 侧是**不同 netdev**，帧必须过 IP 层（转发/路由） | 上 CPU → 路由 → 出另一个口 | **有**：帧会进 `NF_INET_POST_ROUTING`（学习）和本机 MAC 判据（数据面） |
| **扁平态** | 两侧同网段/同一个桥，帧是**桥接**帧 | 同一 `br-lan` 内的已知单播由交换机硬件直通（`zx279128s-switch.c:20-39`） | **没有**：桥接帧不过 IP 层，不经过 `POST_ROUTING`（学习钩子挂在 `POST_ROUTING`，`accel.c:4067-4072`），且目的 MAC 不是本机 ⇒ 数据面 #2 直接 miss（`accel.c:3144-3146`） |

> 扁平态那一行是〔代码事实 + 推断〕：钩子位置与本机 MAC 判据是代码事实；"所以完全不经 FFE"
> 是对这两条的直接推论，**未单独上机验证**。

## 2. 拓扑与方向定义

### 2.1 驱动里的 5 个口与 DTS 的对应

`ZX_NUM_PORTS = 5`（`eth.h:721`），probe 按 DTS 子节点的 `reg` 填 `adapter->ports[reg]`，
netdev 名字取节点的 `label`（`main.c:5273-5274`、`5286-5288`、`5318`）。本板 DTS：

| ports[] 下标 | DTS 节点 | label（netdev 名） | PHY | MAC（nvmem tag+0x1e） |
|---|---|---|---|---|
| 0 | `&gmac_port0` | `lan3` | phy10 | base+0 |
| 1 | `&gmac_port1` | `lan2` | phy11 | base+0 |
| 2 | `&gmac_port2` | `lan1` | phy12 | base+0 |
| 3 | `&gmac_port3` | `wan` | phy13 | **base+1** |
| 4 | `&gmac_port4` | —（`status = "disabled"`） | — | — |

（DTS:156-190 四个口，192-195 关掉第五个；MAC 分配的理由在 DTS:143-155。）

`ZX_BRPORT(p) = p + 1`（`eth.h:228`），RX 描述符里的端口号是**桥端口号**，
`zx_eth_rx()` 解析时减 1 得到 `ports[]` 下标（`main.c:3233`）。

### 2.2 ⚠ 两种配置下"哪个口是 WAN 侧"是不同的

| 配置 | LAN 侧 | WAN 侧 | 证据 |
|---|---|---|---|
| **板级默认**（overlay 的 board.d） | `br-lan = lan1 lan2 lan3` | `wan`（独立 netdev） | `base-files/etc/board.d/02_network:13`；DTS:184 |
| **本仓实测台架**（路由态） | `br-lan = lan2 lan3`（+`phy0-ap0`/`phy1-ap0`） | **`lan1`**，`192.168.5.1/24` | 台架 uci：`network.@device[0].ports='lan2 lan3'`、`network.wan.device='lan1'`、`wan.ipaddr=192.168.5.1`〔本板实测，台架配置原样〕 |

**本文两个方向按"角色"定义，不按口名定义**：

- **WAN→LAN**：从 WAN 侧 netdev 收包（板级默认是 `wan` = ports[3]；台架是 `lan1` = ports[2]），
  出口是 LAN 侧（台架是 `br-lan` 里的 `lan2`/`lan3` = ports[1]/[0]）。
- **LAN→WAN**：从 LAN 侧桥端口收包（台架 `lan2`/`lan3`），出口是 WAN 侧口。
- 代码注释里出现的"回程向入口 lan2、出口 lan1"（`accel.c:1667`）是按**台架**写的；
  板级默认要换成"入口 lan2/lan3、出口 wan"。

> 本板台架 bulk 方向：iperf3 服务器在 `br-lan` 侧（`192.168.1.243` / `fdc1:3d26:9802:…`），
> 客户端在 WAN 侧宿主上 ⇒ **实测的 735→924 Mbit/s 是 LAN→WAN 方向**〔本板实测，台架拓扑〕。

### 2.3 入口为什么会上 CPU（不是所有帧都上）

1. 交换机默认把 IPv4/IPv6/ARP/VLAN 的以太类型全部 trap 到 CPU（`main.c:2003-2012` 的
   `zx_hw_init()` 段）。
2. 一旦**硬件 LAN 交换开着**（`adapter->sw_members` 非 0）**或 PPE 活跃**（`ppe_active`），
   这两条 ethertype trap 就被关掉（`main.c:1654-1656`），改由 MAC 表决定谁上 CPU：
   - `ppe_active=0`：本机 MAC 写进 **trap DMAC 表**（`ZX_SPA_TRAP_DMAC`，2 项，
     `main.c:1702-1704`；表大小 `eth.h:57-58`）；
   - `ppe_active=1`：本机 MAC 写进 **ONU MAC 表**并标"routed"，让分类器先看
     （`main.c:1697-1701`、注释 `1667-1673`）。
3. 三个 LAN 口共用 base+0、WAN 口用 base+1（DTS:143-155）⇒ **不同本机 MAC 只有 2 个**，
   正好塞满 2 条 trap DMAC 表项〔代码事实，`n` 的去重见 `main.c:1684-1695`〕。
4. 广播/组播/未知单播照旧上 CPU（交换机只直通"已知单播 + 同桥"，`zx279128s-switch.c:20-39`）。

## 3. 两条总览流程

符号：`→` 顺序调用；每个方框第一行是函数名，第二行是 `文件:行`。
两条流程**共用** `zx_ffe_forward()` 之后的一切；差异用 ★ 标出。

### 3.1 WAN→LAN（台架：入口 `lan1`/ports[2]，出口 `br-lan` → `lan2`/`lan3`）

```
[线缆 → 交换机]  dest MAC = 本机 WAN MAC(B+1)
   │  硬件 LAN 交换不看这一帧（目的 MAC 命中本机 MAC 表）
   ▼
main.c:1600-1605 / 1697-1704   trap DMAC 表 / ONU MAC 表 → 上 CPU
   ▼
main.c:1931-1933   RX 中断合并：0x350030 = us*50 ticks(默认 25000=500µs)、0x350034 = 64 帧
   ▼
main.c:3817-3827   zx_eth_isr()：读 TM_INT_STATUS(0x340100) → 掩掉 RX → napi_schedule
   ▼
main.c:3829-3856   zx_eth_poll()：tx_reclaim → 满则 kick → defer drain → starve recover
   │                                    → zx_eth_rx(adapter, budget)
   ▼
main.c:3200-3813   zx_eth_rx()
   │  :3214  读 RX_QUEUE_CNT(q)（8 个队列）
   │  :3231-3233  解描述符 → bp / len / port(桥端口号-1)
   │  :3236   bp/len 合法性（bp<4096、14<=len<=1536）
   │  :3339-3344  池可缓存时 dma_sync_single_for_cpu（只失效 16+len）
   │  :3394-3425  src MAC 是本机 ⇒ alias 丢弃（rx_alias）
   │  :3442-3444  h_proto → fam（0=v4, 1=v6, -1=其它）
   │  :3498-3502  fam 那一族 mode==ffe ? 否则 goto accel_slow（→协议栈）
   ▼
accel.c:3301-3346  zx_ffe_rx()：填 struct zx_ffe_pkt（data=池槽+16, token=bp）
   ▼
accel.c:3108-3183  zx_ffe_ingress_switch_run()
   │  #0 入口口存在  #1 目的非组播/广播  #2 目的 MAC 属本机集合  #3 runt(v6 64 / v4 60)
   │  :3161 读 h_proto（帧第 12 字节）  :3167-3171 按族分派判据链
   ▼
accel.c:2559-2617  zx_ffe_v6_check()（IPv4 是 :2469-2527）
   │  #4 ethertype  #5 version  #6 长度(claim vs avail±6)  #7 扩展头/分片
   │  #8 hop_limit<=1  #9 目的组播  #10 目的属本机  #11 源 fe80::/10  #12 只 TCP/UDP
   │  → 填 key（v6: saddr+daddr+sport+dport+proto+family）
   ▼
accel.c:2887-3087  zx_ffe_forward()
   │  #13 rhashtable 查表（:2908）  → 未命中 = MISS
   │  快照 flow→rt（:2919）  #13' daddr_local 且无 DNAT ⇒ DROP_LOCAL
   │  #14 出口 usable：存在 + running + carrier（:2947-2951，zx_ffe_out_ok :1277）
   │  #15 同口 ⇒ FALLBACK   #16 NAT 字段齐全   #17 len<=1514   #18 pkt_len<=mtu
   │  #19 取 tx_lock → tx_pending < txq_limit（:3002-3022）
   ▼
accel.c:2727-2751  zx_ffe_rewrite()：h_dest←dmac、h_source←smac、ttl--/hop_limit--、
   │                                (v4: NAT 改写 + 重算 IP 头 csum)
   ▼
accel.c:2798-2835  zx_ffe_egress_switch_submit() → zx_tx_submit_bp()
main.c:2832-2918     · 写 16 B 描述符（出口 TM 队列 = ZX_PORT_TX_QUEUE(eport)）
                     · dma_wmb() → writel(1, ZX_DMA_TX_START)（门铃，每包一次）
                     · tx_cur_idx++ / tx_pending++ / bp_state[bp]=TX
accel.c:3070-3072    · flow->hits++/bytes+=/lastuse=jiffies
   ▼
accel.c:3201-3263  zx_ffe_ingress_switch_complete()：hit++/fwd++/bytes+ → FORWARDED
   ▼
main.c:3768-3800   ffe_done：zx_rx_consume(描述符)、rx_cur_idx++、work_done++、
                   **不还 bp**（bp 已归发送硬件）
   ▼
main.c:2055-2119   zx_tx_reclaim()（下一次 poll 里）：读 ZX_DMA_TX_DONE → tx_pending-=n →
                   bp_state[tp]=FREE（BMU 自己把槽位还给空闲表）
```

### 3.2 LAN→WAN（台架：入口 `lan2`/ports[1]，出口 `lan1`/ports[2]）

```
[线缆 → 交换机]  dest MAC = 网关 MAC = br-lan 的 MAC = 三个 LAN 口的 base+0
   ▼
★ 同一台机器上、同一套 trap 机制（main.c:1654-1704）；区别只是"本机 MAC 命中"的是
  br-lan/lan 口那一个地址，而 lan2 是**桥端口**（板级默认时入口是 lan2/lan3，出口是 wan）
   ▼
main.c:3817-3856   zx_eth_isr() → zx_eth_poll()（与 WAN→LAN 逐字相同）
   ▼
main.c:3200-3813   zx_eth_rx()（同上，rx_port = 1）
   ▼
accel.c:3301-3346  zx_ffe_rx()
   ▼
accel.c:3108-3183  zx_ffe_ingress_switch_run()：#0-#3 同上
   ▼
accel.c:2469-2527 / 2559-2617  判据链（按族）
   ▼
accel.c:2887-3087  zx_ffe_forward()
   │  ★ 出口是 WAN 口（独立 netdev）：usable 判 carrier/running；#15 同口保护在这里
   │    才真正有意义（LAN 口进、WAN 口出，不会误判）
   │  ★ 命中条目的 flags 通常带 NAT_S（上传方向 masquerade 改写 saddr），
   │    改写段走 zx_ffe_v4_nat_rewrite()（accel.c:2648-2683）；v6 无 NAT66，恒不带位
   ▼
accel.c:2798-2835 / main.c:2832-2918   提交（与 WAN→LAN 同一段）
   ▼
accel.c:3201-3263 → main.c:3768-3800 → main.c:2055-2119   收尾与回收（同）
```

### 3.3 旁路学习（两个方向都走这条，与数据面**并行**、不参与转发）

```
[转发帧到 POST_ROUTING]  accel.c:4067-4084 两条 hook（NFPROTO_IPV4/IPV6，priority=NF_IP*_PRI_LAST）
   ▼
accel.c:2070-2352  zx_ffe_learn_hook()
   │  :2091 learn_on? :2106-2111 该族 mode==ffe? :2117-2125 按 pf 选 parse4/parse6
   │  :2127 pkt_type==PACKET_HOST   :2155/#7 出口（state->out 或 skb_dst 兜底）
   │  :2186-2205 #8 入口（skb_iif 快路径 / 桥 FDB 反查源 MAC）
   │  :2216-2221 #10 post-NAT daddr 属本机 ⇒ 不学
   │  :2245-2254 #11 conntrack：CTINFO2DIR 选 tuple[ORIGINAL]/tuple[REPLY]
   │  :2275-2308 组装候选：键 = 该方向 pre-NAT 元组；输出 = 当前(post-NAT)报头；
   │             NAT 位 = "键与输出不一致"的那一维
   │  :2326-2346 每 CPU 无锁环（256 槽）+ 1 s 去重
   ▼
accel.c:3899-3916  zx_accel_age_work()：每秒一拍
   │  :3909 门闩 tick   :3911 drain   :3912 age   :3913 autofallback
   ▼
accel.c:2362-2390  zx_ffe_learn_drain()：排空每 CPU 环
   ▼
accel.c:1542-1746  zx_ffe_learn_one()：入口/出口端口解析 → 邻居查 MAC → 同口保护 →
   │                                 填模板 flow → __zx_ffe_store()
   ▼
accel.c:783-945    __zx_ffe_store / __zx_ffe_flow_replace：落表（同 key 整体替换）
```

## 4. 入口层：从 DMA 到 FFE 钩子

### 4.1 描述符与缓冲池

- 池：4096 个 2048 B 槽（`eth.h:231-233`），每槽前 16 B 是 DMA 头，帧从 +16 开始
  （`accel.c:3324`）。
- RX 环：8 个队列、每队列 1024 个 16 B 描述符（`eth.h:302-303`、`313-314`）；
  发送环：1024 个描述符（`eth.h:256`）。
- `zx_eth_rx()` 先读 `ZX_DMA_RX_QUEUE_CNT(q)` 得到"这一圈硬件交上来几帧"（`main.c:3214`），
  再按 `rx_cur_idx[q]` 逐条解描述符：`bp`（byte7 bit1.. + byte8）、`len`（desc[3] 的
  13:2 位）、`port`（byte6 bit3.. 减 1）（`main.c:3231-3233`）。
- 合法性：`bp < 4096 && 14 <= len <= 1536`，否则走"跳过/重同步"两条路，计
  `rx_bad_cnt` / `rx_resync_cnt`（`main.c:3236-3288`）。
- **池的可缓存性是两套路径的分界**：默认非缓存（`pool_cacheable=false`，`main.c:60-61`）；
  可缓存时 RX 每帧一次 invalidate（`main.c:3339-3344`）、快路改写后一次 clean
  （`accel.c:3044-3048` 是 clean 的调用点，长度 `clean` 在 `accel.c:3032-3043` 算）。

### 4.2 中断合并 → 软中断 → NAPI

| 环节 | 位置 | 要点 |
|---|---|---|
| 合并寄存器 | `eth.h:150-151` | `DMA+0x30` 时间阈值（**单位 20 ns**）、`DMA+0x34` 帧数阈值 |
| 编码 | `main.c:965`、`1931-1933` | `us * 50` 写 0x30（默认 500 µs → 25000）；0x34 写帧数（默认 64） |
| 硬件语义 | `eth.h:295-301`（上游注释） | "从第一帧起计时，时间或帧数任一先到就抬中断" |
| ISR | `main.c:3817-3827` | 只读 `TM_INT_STATUS`、`writel(~0, TM_INT_MASK)`、`napi_schedule`；**没有软件 rearm**（合并计数在硬件里） |
| poll | `main.c:3829-3856` | `tx_reclaim` → 环满/停口则 `kick` → defer drain → `starve_recover` → `zx_eth_rx(budget)` |
| 收尾 | `main.c:3852-3853` | `work_done < budget` 且 `napi_complete_done()` 才 `writel(~ZX_TM_INT_RX, TM_INT_MASK)` 放中断 |
| TX 完成无中断 | `main.c:2042-2052` | 靠每次 xmit/poll 里的 `zx_tx_reclaim()` + 250 µs hrtimer 兜底（`eth.h:283`） |

### 4.3 合并窗口的实测作用（实测与机理分开写）

**〔本板实测〕** 同一台架、同一条 v6 单流（`iperf3 -6 -c <服务器> -t 30 -O 2`），
只改三个旋钮，31 格：

| 档位（`us`/`frames`/`weight`） | 单流 Mbit/s | frames/IRQ | softirq 占比 |
|---|---|---|---|
| 500 / 64 / 64（默认，控制组 6 格） | 688.7–759.5，均值 **733.7** | 49–55 | 0.40–0.44 |
| 250 / 64 / 64 | **866.9 / 894.0** | 37–39 | 0.45–0.49 |
| 250 / 16 或 32 / 64 | **921.2–924.4** | 15.3–30.2 | 0.50–0.52 |
| 62 / 64 / 64 | **922.0** | 11.1 | 0.567 |
| 750 / 1000 / 1500 / 2000 | **478–548** | 45–65 | 0.28–0.33 |

- 可逆性（同一启动内往返三次）：`250/16` → 924.4；写回 `500/64` → 759.5；再写回
  `250/16` → 924.1 ⇒ **状态跟随当前寄存器值**，不是一次性跃迁〔本板实测〕。
- 读回自证：每次写入都打
  `zxcoal_us=250: reg 0x350030 raw 25000 -> 12500 (readback 12500)`（`main.c:981-1005` 的
  读回逻辑，日志原文由该函数打印）⇒ **运行期直接写就锁存**，不需要 disable→写→enable〔本板实测〕。
- 代价：中断率 1.2 k/s → 2.6–5.2 k/s（×1.7–10.6），softirq 0.40 → 0.50–0.57。
  **"frames/poll ≥ 57 且吞吐高且 softirq 不升"这一格从未出现**（frames/poll 全程 11.7–46.3）。

**〔推断，未验证〕机理**：合并窗口直接决定"一帧在池里等多久才被 poll 读到"，而 ACK 方向
（同一台机器、反方向的另一个口）也吃同一个窗口。窗口 500 µs 时，一条 ~60 kpps 的单流每
≈30 帧才被服务一次，回程 ACK 的最坏额外时延接近窗口量级 ⇒ 发送端 RTT 被抬高、cwnd 长不快。
窗口缩到 250 µs 以下后每批更小、ACK 更快离开，吞吐跳上 866–924。
支持（**不构成证明**）的观察：本轮两组测量的 max cwnd 与 mean RTT 同向变化
（197k/3.7 ms vs 481k/2.83 ms，见 [features/ffe-fastpath.md](../features/ffe-fastpath.md) 附二）。
要把它变成事实，需要"只改回程方向合并窗口"的单向实验（本仓未做）。

**与上游注释的关系（位置不同，不是符号相反）**：`eth.h:296-298` 的原文是
"Larger batches cost less per frame: 500 us receive faster than polling every 1 ms did and still answer sooner." ——它其实是两个**可分开**的主张：(a)「批量越大，**每帧**成本越低」；
(b)「500 µs 合并比每 1 ms 轮询**更快**」。
本板只对 (b) 给了数据：窗口从 500 往上拉是**变慢**（750→497.9、1000→548.3、1500→491.8、
2000→478.1），往下拉继续**变快**（250→866.9、62→922.0）⇒ 与 (b) 的"越短越快"**同向**，
两者的差别只在**最优点位置**：本板最优点落在 62–250 µs，比上游注释标的位置**更靠左**；
本板的 A1000 格是"1 ms 窗口"而非"1 ms 轮询"（机制不同，只能当近似），即便如此
500 µs 的 733.7 仍高于它的 548.3，同样与 (b) 同向。**所以不能概括成"符号相反"。**
(a) 讲的是**每帧 CPU 成本**。本板能测到的同源量是**每帧 softirq 时间**：
Δsoftirq_jiffies(最忙那一核) × (1e6/HZ) ÷ Δrx_packets(lan1+lan2)，同窗口、HZ=100，
本板 31 格实测 **5.458–6.896 µs/帧**（`coal-sweep/20261103-2330/sweep.tsv` 第 15 列；
生成处 `logs/measure2.py:144`，口径出处 `delay-dose/20261103-2300/logs/analyze.py:5-8`），
控制档按同一单核口径是 **5.74–5.97**（K0a 5.739 / K0c 5.941 / K0b·K0e 5.972）。
但它与上游那条注释讲的"每帧成本"**口径不同**（本列是单核 softirq、rx 包分母；上游那个类比是
双核合计、tx 包），**不可直接相比**。这列且能反映每帧工作量（注入 4 µs/帧 ⇒ 本列 +3.43 µs），
所以它的"变化小"既不能反推"与合并不相关"，更不等于"零成本"。
真正**无证据**的是**"仅 NET_RX"的每帧成本**：`/proc/softirqs` 只有每核每类的**次数**、没有时间，
`/proc/stat` 的 softirq 是**全部类型**的合计 ⇒ 这一层标〔无证据〕，既不能说相符也不能说相反。
另需注意量化下限：Δsoftirq ≥ ~200 tick 才谈得上 1 % 精度（反例 F0：N=391、3 jiffies ⇒ 76.7 µs/帧 是纯量化产物）。
能下的结论只有一条，也正是本节要提醒的：**上游注释里的"500 µs 即最优"不是本板硬件行为的权威**。

## 5. 快路判定：key、查表、分支

### 5.1 key 的构成

`struct zx_ffe_key`（`accel.c:350-362`）：`v4[2]` 或 `v6[32]`（同一片 union）+ `sport` +
`dport` + `proto` + `family`（+ `ipport`/`vid`，**两族都填 0**，填别的永远不命中，
`accel.c:1692-1695`）。

| 族 | 键字段来源 | 代码 |
|---|---|---|
| v4 | `iph->saddr/daddr`、L4 前 4 B（sport,dport）、`protocol`、`family=4` | `accel.c:2518-2525` |
| v6 | `ip6->saddr`(16B)+`daddr`(16B)、L4 前 4 B、`nexthdr`、`family=6` | `accel.c:2608-2615` |

- **没有 VLAN id、没有 ipport**：所以带 VLAN tag 的外层帧在 #4 就 miss（`accel.c:2478`），
  开了 VLAN filtering 的桥也不做 FDB 反查（`accel.c:1483-1484`）。
- v6 **不解析扩展头**：逐跳/路由/分片/目的选项整类回栈（`accel.c:2579-2585`）。

### 5.2 查的是哪张表

- **一张** rhashtable（`accel->flows`，`eth.h:531`），容量 `ZX_FFE_FLOWS_MAX = 1024`
  （`accel.c:220`），`family` 在键里 ⇒ v4/v6 共表。查表是
  `rhashtable_lookup(&accel->flows, key, zx_ffe_ht_params)`（`accel.c:2758-2762`），
  在 `rcu_read_lock()` 里（`accel.c:2907`），条目由 `kfree_rcu` 释放。
- 这张表**和 PPE/CLA 的 127 条硬件表是两套东西**（PPE 见
  [architecture.md](../architecture.md) §"The capacity ceiling"）。FFE 表满计 `tbl_full`
  并拒绝新条目（`accel.c:1741-1744`），已有条目继续命中。

### 5.3 命中/未命中的分支条件（一览）

```
zx_ffe_v4_check/v6_check 全通过
        │
        ├─ lookup == NULL ────────────────→ MISS（:+ 回栈）
        ├─ daddr_local && !NAT_D ─────────→ DROP_LOCAL（:+ 回栈）
        ├─ 出口 not usable ───────────────→ DROP_NOPORT
        ├─ egress == rx_port ─────────────→ FALLBACK
        ├─ NAT 位在但字段缺（或 v6 带 NAT 位）→ FALLBACK
        ├─ len > 1514 ────────────────────→ FALLBACK
        ├─ mtu == 0 || pkt_len > mtu ─────→ FALLBACK
        ├─ tx_pending >= txq_limit ───────→ QFULL（回栈）/ QFULL_DROP（丢）
        ├─ submit 失败 ───────────────────→ RX_DROP（丢）
        └─ 提交成功 ──────────────────────→ FWD（hit++/fwd++）
```

## 6. 学习：首包怎么上送、钩子挂哪、双向、老化、学不到

### 6.1 首包不进"快路学习"，进的是 netfilter

- 快路**不做首包学习**：首包按普通路径进栈（`main.c:3526` 的 `accel_slow`），
  同一个包稍后到 `NF_INET_POST_ROUTING` 时被 `zx_ffe_learn_hook()` 看见——
  **钩子在路由与 NAT 之后**（`accel.c:4069`、`4072`，priority `NF_IP_PRI_LAST`/`NF_IP6_PRI_LAST`
  `accel.c:4081-4084`），所以它看到的是 **post-NAT** 的报头。
- 钩子恒返回 `NF_ACCEPT`，只做 memcpy + 环写，任何分配/邻居查询都留到 work
  （`accel.c:2351`、`45-47`）⇒ **学习失败不影响转发**。

### 6.2 键与输出：为什么"两个方向都学"

- 键取自 **conntrack 元组按方向挑的那一侧**（`CTINFO2DIR(ctinfo)`，`accel.c:2250-2251`）：
  ORIGINAL 侧取 `tuple[ORIGINAL]`、REPLY 侧取 `tuple[REPLY]`；两侧存的都是"该方向报文在
  链路上的 pre-NAT 形态"，正好是数据面从帧头拼出来的键。
- 输出（`*_out`）永远是**当前报头**（post-NAT，`accel.c:2281-2284`）。
- NAT 位 = "键与输出在这一维不一致"（`accel.c:2303-2308`）：
  上传方向 masquerade ⇒ 源地址/端口变了 ⇒ `NAT_S`；端口映射/下载回程 ⇒ 目的变了 ⇒ `NAT_D`。
- ⚠ 历史坑：`ctinfo & IP_CT_IS_REPLY` 是**错的**（NEW=2 会被当成回程），必须用
  `CTINFO2DIR()` 的 `>=`（`accel.c:2235-2243` 记录了这个 bug 与症状：回程条目 hits 恒 0）。

### 6.3 落表：出口与下一跳

| 钩子拿到的出口 | 下一跳 MAC 怎么查 | 出口端口怎么定 | smac | 代码 |
|---|---|---|---|---|
| 本驱动端口（WAN 口直出） | 该口 + `arp_tbl`/`nd_tbl`，查不到再按该口查路由取网关 | 就是它 | 该口 dev_addr | `accel.c:1647-1663`、`1344-1418` |
| 桥（WAN→LAN 常态） | **先用桥**查邻居（LAN 主机的 ARP/ND 挂在 br-lan 上） | `br_fdb_find_port(桥, dmac, 0)` 反查物理口，再复核是本驱动端口 | **桥**的 dev_addr | `accel.c:1602-1646`、`1468-1512` |

- 入口端口：`skb_iif` 直接对 `ports[]`（独立口那一路）；桥端口进来的帧 `skb_iif` 已被改成
  桥的 ifindex，于是记"源 MAC + 桥 ifindex"，在 drain 里 FDB 反查（`accel.c:2173-2205`、
  `1574-1594`）——这就是 P9 修的"回程永远学不到"。
- 反查失败/命中无线口 → `learn_nonport`；FDB 未命中 → `learn_nobrport`；查不到邻居 →
  `learn_noneigh`。**绝不猜**（`accel.c:1626-1632`、`1619-1625`、`1612-1617`）。

### 6.4 老化与失效

- `ffe_aging` 默认 **300 s**（`accel.c:232-234`），每秒一拍扫 `flow_list`，
  `jiffies_to_msecs(now - lastuse) > aging*1000` 就删（`accel.c:1286-1304`）；
  `0` = 不老化。`lastuse` 在每次成功转发时刷新（`accel.c:3070-3072`），
  **环满回栈也算活跃**（`accel.c:3010`）。
- 其他失效源：模式切换清表（`zx_accel_set()` S4，`accel.c:3598` 起）、端口 down 触发的
  per-port flush（`accel.c:4220` 的 work）、自动降级（§8.3）。

### 6.5 "学不到 / 学错"会看到什么

| 现象 | 先看 | 判据 |
|---|---|---|
| `ffe_flows` 0 行，`lookup` 在涨、`hit` 恒 0 | `ffe_learn` 第 1 行 + `auto_enable_in_ms` | 第 1 行 `0` 且 `auto_enable_in_ms>0` ⇒ 还在 120 s 门闩里；uptime < 130 s 时测得的数据一律作废 |
| `cand` 不涨 | 该族 mode、钩子是否在该族 | v6 时 `accel_ipv6` 首行必须是 `ffe`（`accel.c:2106-2111`） |
| `cand` 涨、`add` 不涨、`noneigh` 涨 | 邻居表 | 目的不直连且路由查不到网关时会一直 `noneigh`（`accel.c:1612-1617`） |
| `nobrport` 涨 | 桥 FDB | 目的 MAC 还没学到/要泛洪（`accel.c:1619-1625`） |
| `nonport` 涨 | 出口/入口命中了无线口 | 桥→无线必须回栈（`accel.c:1626-1632`） |
| 有 `cand` 无 `add`、`tbl_full` 涨 | 表满 1024 | `accel.c:1741-1744` |
| 条目在、hits 不涨 | 数据面判据 | 按 §8 的顺序读 miss 分解 |

## 7. 命中后的处理

### 7.1 改写（`zx_ffe_rewrite()`，`accel.c:2727-2751`）

| 动作 | v4 | v6 |
|---|---|---|
| `h_dest` ← 条目 `dmac` | ✔ `:2733` | ✔ |
| `h_source` ← 条目 `smac`（出口口或桥的 MAC） | ✔ `:2734` | ✔ |
| NAT 改写 | 有 NAT 位时走 `zx_ffe_v4_nat_rewrite()`（`:2741-2742`） | **不做**（无 NAT66，`#16` 已回栈） |
| TTL / hop_limit | `ip->ttl--`（`:2744`） | `ip6->hop_limit--`（`:2738`） |
| IPv4 头校验和 | 整头重算（`:2749-2750`） | **不碰任何校验和**（`:2736-2740`，v6 固定头无 csum，伪首部不含 hop_limit） |

### 7.2 NAT 改写细节（v4，`accel.c:2648-2683`）

1. 先按**旧值**做 L4 校验和增量：每改 4 B 地址 `csum_replace4()`、每改 2 B 端口
   `csum_replace2()`（`:2663-2670`，RFC 1624）。
2. **UDP 校验和为 0 时整段跳过**（发送方没算，不许替它编一个，`:2662`）；增量算出 0 要按
   全 1 发（`CSUM_MANGLED_0`，`:2671-2672`）。
3. 三个操作数必须同一字节序约定：取端口用 `get_unaligned((__be16 *)l4)`（内存约定），
   用 `get_unaligned_be16()` 会算错（注释里给了对拍值，`:2640-2646`）。
4. 最后才写新地址/端口（`:2675-2682`）。

### 7.3 出口选择

- 条目只存 `egress_type + egress_id`（`accel.c:393`、`411-412`），今天只有
  `switch-port` 一项（`accel.c:2837-2844`），`egress_id` 就是 `ports[]` 下标。
- usable 判据 = `存在 + netif_running + netif_carrier_ok`（`accel.c:1252-1264`、`1277-1280`），
  **判据段与提交段用同一个函数**（F3 的 TOCTOU 修复，`accel.c:2812-2818`）。
- 桥/无线**不会**出现在条目里：学习侧就挡掉了（`accel.c:2061-2068`）。

### 7.4 TX 提交与回收

| 步 | 位置 | 说明 |
|---|---|---|
| 描述符 | `main.c:2859-2865` | `desc[0]` 里带 `ZX_PORT_TX_QUEUE(eport)`（定向到该口的 TM 队列），byte7/8 放 bp |
| 屏障 | `main.c:2867` | `dma_wmb()`：描述符必须先于门铃可见 |
| 门铃 | `main.c:2868` | `writel(1, ZX_DMA_TX_START)`，**每包一次**（没有批量门铃） |
| 记账 | `main.c:2870-2871`、`2878-2881` | `tx_cur_idx++`、`tx_pending++`、`tx_bp[tx_idx]=bp`、`bp_state[bp]=TX` |
| 水位 | `main.c:2907-2916` | `tx_pending >= txq_limit` ⇒ 停所有端口队列 + `zx_tx_kick()`（250 µs 后必有一次 poll） |
| 回收 | `main.c:2055-2119` | 读 `ZX_DMA_TX_DONE`（低 16 位）⇒ `tx_pending -= n` ⇒ 影子环把 `bp_state` 清回 FREE |
| 唤醒 | `main.c:2111-2117` | `pending < limit` 且非 quiesce 才 `netif_wake_queue()` |
| 记账（/proc/net/dev） | `accel.c:3085`（入口 rx）、`accel.c:2832-2833`（出口 tx） | 快路两侧都补，保证"从 lanX 进、从 lanY 出"的计数照涨 |

> **bp 的所有权**是这条快路最容易错的地方：FORWARDED 的帧**绝不能**再由 RX 侧还给 BMU
> （同一个槽位两个属主 ⇒ RX 环立刻重填、发送硬件还在读它）。收尾逻辑与理由逐行写在
> `main.c:3768-3800`；`zx_bp_return()` 再用 `bp_state[bp]==TX` 兜一次（`main.c:1458-1469`）。

## 8. 未命中 / 降级 / 丢弃点：逐个枚举

### 8.1 数据面（按代码顺序，39 处 = 12 个"进链之前" + 27 个"判据链/提交段"）

| # | 条件 | 所在 | 裁决 | 可观测 |
|---|---|---|---|---|
| D1 | `bp >= 4096` 或 `len < 14` 或 `len > 1536`（且该队列已同步） | `main.c:3236-3263` | 跳过该槽 | `rx_bad`（`main.c:3248`） |
| D2 | 同上但该队列未同步 ⇒ 向后找下一个合法描述符 | `main.c:3266-3287` | 重同步 | `rx_resync`（`:3286`） |
| D3 | `bp == 0`（硬件没给缓冲） | `main.c:3291-3298` | 消费描述符、丢帧 | `rx_nobuf`（`:3292`） |
| D4 | `bp_state[bp] == TX` 且 `ffe_bp_drop=1` | `main.c:3320-3329` | 丢帧（不还 bp） | `ffe_dup`（`:3322`）、`bp_drop` |
| D5 | 帧首仍是毒值（`ffe_poison`） | `main.c:3388-3391`、`3456-3457` | 只计数，且**不进快路**（回栈） | `rx_poison` |
| D6 | src MAC 是本机（alias） | `main.c:3394-3425` | 丢帧 + 快照取证 | `rx_alias`、`alias_dump` 的 `alias_v4/v6/unk` |
| D7 | `quiesce`（模式切换中） | `main.c:3458-3459` | 回栈 | `accel_ipvN` 的 `sw_in/sw_out` |
| D8 | 该族 mode != ffe | `main.c:3498-3511` | 回栈，**不进判据链**（v4 帧在交付态走这条） | 该族 `lookup` 不涨 |
| D9 | 非 IP 帧且两族都不在 ffe | `main.c:3503-3510` | 回栈 | 无一族 `lookup` 涨 |
| D10 | 慢路径：`claimed > l3_avail`（真截断） | `main.c:3615-3629` | 驱动内丢帧 | `rx_len_short` |
| D11 | 慢路径：`l3_avail - claimed > 6`（长副本） | `main.c:3630-3638` | **只计数不丢**（交给 pskb_trim） | `rx_len_long` |
| D12 | `dest_dev == NULL` / `!IFF_UP` / skb 分配失败 | `main.c:3711-3749` | 丢帧 | `diag` 的 `rx_drop_nondev` / `rx_drop_down` / `skb_nomem`（需 `diag_drop=1`） |
| F1 | #0 入口端口越界/未 probe | `accel.c:3137-3140` | MISS | `lookup`↑ `miss`↑（无子集） |
| F2 | #1 目的 MAC 是组播/广播 | `accel.c:3141-3143` | MISS | 同上 |
| F3 | #2 目的 MAC 不在本机集合 | `accel.c:3144-3146` | MISS | 同上（桥接/他人单播走这里） |
| F4 | #3 runt：v4 `< 60`、v6 `< 64` | `accel.c:3157-3158` | MISS | 同上；两族下限**刻意不同**（P10.2 F1，`:3148-3155`） |
| F5 | #4 ethertype 不是 0x0800/0x86dd | `accel.c:2478`、`2568` | MISS | 同上（ARP/VLAN/PPPoE 外层） |
| F6 | #5 v4 `version!=4` 或 `ihl!=5`；v6 `version!=6` | `accel.c:2487-2488`、`2572-2573` | MISS | 同上（带选项的 v4 一律不接） |
| F7 | #6 长度：`claimed > avail` | `accel.c:2440-2443` | MISS | `len_short` |
| F8 | #6 长度：`avail - claimed > 6` | `accel.c:2444-2447` | MISS | `len_long` |
| F9 | #6 v4 IPv4 头校验和错 | `accel.c:2493-2494` | `CSUM_ERR` | `csum_err`（v6 恒 0） |
| F10 | #7 v4 分片 / v6 扩展头（0/43/44/60） | `accel.c:2496-2497`、`2579-2585` | MISS | 无子集 |
| F11 | #8 TTL/hop_limit `<= 1` | `accel.c:2511-2512`、`2587-2588` | `TTL_ERR` | `ttl_err`（要由栈回 ICMP Time Exceeded） |
| F12 | #9 v6 目的地址是 `ff00::/8` | `accel.c:2590-2591` | MISS | 无子集（ND/MLD 全在这里） |
| F13 | #10 v6 目的属本机 | `accel.c:2593-2594` | `DROP_LOCAL` | `drop_local` |
| F14 | #11 v6 源地址 `fe80::/10` | `accel.c:2596-2597` | MISS | 无子集 |
| F15 | #8/#12 非 TCP/UDP | v4 `:2508-2509`（本机则 `DROP_LOCAL`）、v6 `:2599-2600` | MISS / `DROP_LOCAL` | `drop_local`（v4 的 ICMP ping 本机走这条） |
| F16 | v4 取端口长度不够（`tot_len < 24`） | `accel.c:2514-2515` | MISS | 无子集 |
| F17 | #13 查表未命中 | `accel.c:2908-2912` | MISS | `miss`↑，`hit` 不动 |
| F18 | #13' 命中但 `daddr_local` 且条目无 `NAT_D` | `accel.c:2935-2938` | `DROP_LOCAL` | `drop_local` |
| F19 | #14 出口类型未注册 / 出口不存在 / down / carrier off | `accel.c:2947-2951` | `DROP_NOPORT` | `drop_noport` |
| F20 | #15 出口 == 入口口（hairpin） | `accel.c:2953-2956` | `FALLBACK` | `fallback` |
| F21 | #16 带 NAT 位但输出地址/端口缺一（或 v6 带 NAT 位） | `accel.c:2964-2973` | `FALLBACK` | `fallback` |
| F22 | #17 帧长 `> 1514` | `accel.c:2979-2982` | `FALLBACK` | `fallback` |
| F23 | #18 出口 MTU 未知（0）或 `pkt_len > mtu` | `accel.c:2994-2997` | `FALLBACK` | `fallback` |
| F24 | #19 发送环满（`tx_pending >= txq_limit`） | `accel.c:3003-3022` | `QFULL`（回栈，qdisc 背压） | `fallback` + `fallback_qfull` |
| F25 | #19 同上但 `ffe_qfull_drop=1` | `accel.c:3020`、`3243-3251` | `QFULL_DROP`（**丢帧**） | `fallback_qfull` + `qfull_drop` |
| F26 | 提交失败（出口刚掉链 `-ENETDOWN` / 环刚满 `-ENOSPC`） | `accel.c:2813-2825`、`3057-3061` | `RX_DROP`（帧**已改写**，只能丢） | `rx_drop` |
| F27 | 改写后与"回栈"的边界 | `accel.c:2698-2701` | 提交段之后**没有回栈这条路**（改过的帧回栈就是发脏包） | — |

计数器口径（`accel.c:3190-3196`、`eth.h:374-413`）：`lookup == hit + miss` 是**不变式**，
`hit == fwd`（P2 起）；`csum_err/ttl_err/drop_local/drop_noport/fallback/fallback_qfull/`
`rx_drop/len_short/len_long` 全是 **miss 的子集**。所以"miss 大"本身不是故障，
要看子集分解。

### 8.2 学习面（只影响"以后能不能进快路"，不影响当前转发）

| 位置 | 条件 | 计数器 |
|---|---|---|
| `accel.c:2091` | `learn_on == 0` | 无（静默） |
| `accel.c:2106-2111` | 该族 mode != ffe | 无 |
| `accel.c:2127` | `pkt_type != PACKET_HOST` | 无 |
| `accel.c:2155-2171` | 出口解析不出（既不是端口也不是桥，`skb_dst` 兜底也失败） | 无 |
| `accel.c:2200-2201` | 入口不是本驱动的桥端口且 `skb_iif` 对不上 | 无 |
| `accel.c:2203-2205` | 同口进出（快路径可判的那一半） | 无 |
| `accel.c:2216-2221` | post-NAT 目的地址属本机 | 无 |
| `accel.c:2245-2249` | 无 conntrack / 未 confirm / dying | 无 |
| `accel.c:2252-2254` | 元组的 l3num/protonum 与包不符 | 无 |
| `accel.c:2317-2320` | v6 候选带 NAT 位（无 NAT66） | `learn_cnt[1].nat6` |
| `accel.c:2328-2333` | 每 CPU 环满（256） | `learn_drop`、`learn_cnt[].drop` |
| `accel.c:2339-2344` | 1 s 内同一条流已投过 | `learn_dedup`、`learn_cnt[].dedup` |
| `accel.c:1574-1594` | 入口桥 FDB 反查失败/不是交换机口 | `learn_nonport` |
| `accel.c:1602-1611` | 出口桥被删/ifindex 复用 | `learn_nobrport` |
| `accel.c:1612-1617` | 拿不到下一跳 MAC | `learn_noneigh` |
| `accel.c:1619-1632` | 出口 FDB 未命中 / 命中无线口 | `learn_nobrport` / `learn_nonport` |
| `accel.c:1639-1656` | 出口端口指针空或没 running | `learn_nonport` |
| `accel.c:1671-1672` | 解析后的同口进出 | 无 |
| `accel.c:1730-1744` | 同 key 已存在（整体替换，算成功）/ 表满 | `learn_dup` / `tbl_full` |

### 8.3 自动降级（把整族踢回 stack）

- 判据（`accel.c:298-337`、`3731-3865`）：**10 s 固定窗**，窗内 `lookup` 至少 1000 帧，
  `drop_noport > 1‰` 或 `fallback > 500‰` **一窗即降**；`alias/lookup >= 2‰` 需
  **连续 2 窗**（第 1 窗只打 `alias strike 1/2`）。
- 动作：`zx_accel_set(adapter, family, ZX_ACC_STACK)`（`accel.c:3861`）⇒ 清该族表、
  `accel_ipvN` 首行变 `stack`、dmesg 一行 `accel: IPv%d fell back to stack: …`。
- **不自动回升**（降级分支只做 `zx_accel_set(…, ZX_ACC_STACK)`，之后没有任何回升路径，
  `accel.c:3855-3865`）。降级后现象：`lookup` 停涨（D8 把帧挡在链外）。

## 9. 反向（LAN→WAN）的差异点

| 维度 | WAN→LAN | LAN→WAN | 证据 |
|---|---|---|---|
| 入口 netdev | WAN 侧口（板级默认 `wan`/ports[3]；台架 `lan1`/ports[2]），**不在任何桥里** | **桥端口**（`lan2`/`lan3`，ports[1]/[0]），帧从 br-lan 的某个成员进来 | `02_network:13`、DTS:156-190 |
| 入口坐标从哪来（学习时） | `skb_iif` 直接对上 `ports[]` | `skb_iif` 已被改写成桥的 ifindex ⇒ 记"源 MAC + 桥 ifindex"，drain 里 FDB 反查 | `accel.c:2173-2205`、`1574-1594` |
| 出口坐标 | 桥出口 ⇒ 先用**桥**查下一跳 MAC，再 FDB 反查物理口；smac = **桥**的 dev_addr | 独立口 ⇒ 邻居按该口查；smac = 该口的 dev_addr | `accel.c:1602-1663` |
| NAT 方向 | 端口映射/下载回程通常带 `NAT_D`（改 daddr/dport） | 上传通常带 `NAT_S`（改 saddr/sport） | `accel.c:2303-2308`、`2648-2683` |
| 出口队列 | 目标 LAN 口的 TM 队列（`ZX_PORT_TX_QUEUE(eport)`） | WAN 口的 TM 队列 | `main.c:2859` |
| 同口保护 #15 | 进出必不同口，几乎不触发 | 若条目错成"从 lan2 进、又发 lan2"就回栈 | `accel.c:2953-2956` |
| `/proc/net/dev` | 入口记在 WAN 口、出口记在 LAN 口 | 入口记在 **lan2/lan3 端口 netdev**（不是 br-lan）、出口记在 WAN 口 | `accel.c:3085`、`2832-2833` |

**最容易搞错的三点**（都踩过）：

1. **"WAN 是 lan1"只在台架成立**（§2.2）。写判据/脚本时不要用口名，用角色。
2. **`drop_local` 不是"丢包"**：v4 的延迟判定（#13'）会让"目的地址是本机但条目带 DNAT"的帧
   **正常转发**；只有"命中但无 DNAT"才算 `drop_local`（`accel.c:2935-2938`）。把 `drop_local`
   当丢包会误判成故障。
3. **两个方向的条目是两条**，各自有自己的 `hits`。一条 TCP 连接要两个方向都学到才有
   "双向都快"；只有一侧在表里时，另一侧仍走栈（`accel.c:46-49` 的 P5 教训）。

## 10. 可观测点（debugfs / sysfs）

目录：`/sys/kernel/debug/92000000.ethernet/`（`dev_name(adapter->dev)`，`main.c:4969`）。

| 文件 | 权限 | 关键字段 | 看什么 |
|---|---|---|---|
| `accel_ipv4` / `accel_ipv6` | 0644 rw | 第 1 行=模式名（`stack/ffe/hw/zc`）；第 2 行 `lookup hit miss fwd fallback fallback_qfull drop_noport drop_local csum_err ttl_err bytes qlen_peak rx_drop len_short len_long qfull_drop`；第 3 行 `ppe_bound sw_in sw_out drain_ms drain_frames autofallback zc_mask flows tbl_full hint=` | 全部快路计数（`accel.c:4225-4266`） |
| `ffe_flows` | 0644 **只读** | 每行一条流的键/出口/NAT/hits/bytes/lastuse_ms + `L`(learned)/`S`(static) | 表里到底有什么（`accel.c:4878-4935`） |
| `ffe_learn` | 0644 rw | 第 1 行开关；第 2 行两族合计 `cand add dup drop noneigh nobrport nonport dedup flows tbl_full add_replace`；第 3 行 `ring=`每 CPU 占用；第 4/5 行 v4/v6 分族（v6 多 `nat6`）；末行 `auto_enable_in_ms=` | 学习是否在学、卡在哪一格（`accel.c:5081-5149`） |
| `ffe_aging` | 0644 | 当前老化秒数 | 与 `ffe_aging` 模块参数同一变量（`main.c:4993-4995`） |
| `accel_autofallback` | 0644 | 0/1 | 运行期真开关（系统参数只在 probe 读，`main.c:5134`） |
| `ffe_add` / `ffe_del` / `ffe_flush` | 0644 | 写命令行、读回最后一条结果 | 手工灌条目（`main.c:4988-4992`） |
| `ffe_migrate` | 0644 | 写 1 取 PPE 快照（**不落表**） | hw→ffe 手工迁移前统计（`accel.c:5205-5240`） |
| `dma_state` | 0444 | `tx_submitted/tx_completed/tx_pending/tx_done_excess/rx_resync`；`rx_alias/rx_nobuf/rx_rel_fail/rx_bad/rx_len_short/rx_len_long`；`ffe_dup bp_double_free tx_dup_reject rx_poison bp_state_stale bp_tx_now bp_poison bp_drop`；`bp_hot`；`ppe_active/ppe_flows/...`；`timing=1` 时还有 rx/tx 分段耗时 | bp 生命周期与 PPE 侧计数（`main.c:4115` 起；字段在 `4143-4150`、`4189-4197`、`4199-4201`、`4233-4261`） |
| `diag` | 0444 | 只在 `diag_*=1` 时累加：盒内停留、seq 反演、每 poll 帧数、TX 环占用、TX 完成延迟、丢点逐项、突发 | `main.c:4967-4974`、`4870-4951` |
| `alias_dump` | 0444 | `cnt idx alias_v4 alias_v6 alias_unk` + 每项 `hits=` | alias 丢帧取证（`main.c:4976-4977`） |

sysfs 参数：`/sys/module/zx279128s_eth/parameters/`（`module_param*`，共 29 项在本轮镜像上）。

### 10.1 容易误读的字段（逐条）

| 字段 | 误读 | 正确读法 |
|---|---|---|
| `lookup` | "网口收到的帧数" | 只有**进了判据链**的帧；alias/runt 之前的丢弃不在内（`eth.h:375`） |
| `hit` / `fwd` | 两个不同指标 | P2 起恒等（`eth.h:376`、`378`）；`hit` 涨=真的交给硬件了 |
| `miss` | "故障数" | 含全部 9 个子集；正常流量里 `drop_local`、`fallback` 都会涨（`eth.h:377`） |
| `fallback_qfull` | "环满回栈次数" | 它 = `QFULL`(回栈) + `QFULL_DROP`(丢帧)（`accel.c:3232-3248`）；要区分就看 `qfull_drop` |
| `qlen_peak`（`accel_ipv6` 里） | "v6 的环占用峰值" | 整环共享，只有 `cnt[0]` 被写，v6 行显示的是 `cnt[0]` 的值，`cnt[1].qlen_peak` **恒 0 是设计如此**（`accel.c:4238-4244`、`eth.h:406-412`） |
| `tx_submitted` / `tx_completed`（dma_state） | "队列占用" | 都是**开机累计**值（`main.c:4143-4146`）；占用看 `tx_pending`、峰值看 `qlen_peak` |
| `bp_tx_now` | "泄漏" | 账上仍在发送环里的 bp 个数，应 ≤ `tx_pending`；短暂 > 0 正常（`main.c:4158`） |
| `ffe_dup` | "丢了多少帧" | 只是"RX 拿到的 bp 还在发送环里"的**观测**次数；`ffe_bp_drop=1`（默认）时才真丢（`main.c:3320-3329`、`4193`） |
| `bp_double_free` | "已经 double free" | 是"**被挡下**的二次归还"次数，非 0 不等于已损坏（`main.c:4188`） |
| `auto_enable_in_ms=0` | "还没到点" | 0 = 已自动开过 **或** 用户写过 `ffe_learn` 已接管；要与第 1 行开关一起读（`accel.c:5137-5147`） |
| `ppe_bound=1` | "FFE 没在工作" | 只表示有 flowtable 绑着；它**只**阻止把 v4 切到 ffe（`accel.c:3527-3534`） |
| `TM_INT_MASK` 读回"全 1" | "中断被永久屏蔽" | poll 结束就 `writel(~ZX_TM_INT_RX, …)` 放 RX 位（`main.c:3852-3853`）；在 NAPI 窗口内读到掩掉是正常时序 |
| `frames/IRQ` 与 `frames/poll` | 同一个量 | 本轮实测二者不共变（控制格 54.6 vs 46.3；快态格 30.0 vs 31.6）〔本板实测〕 |
| `softirq` 占比下降 | "合并起效了" | 方向相反：最差的三档（478–508 Mbit/s）softirq 最低（0.28–0.33）〔本板实测〕 |

## 11. 可调参数总表

`RW=运行期可写`；"probe-only"= 只在 probe 读一次，之后写无效。

| 名字 | 路径 | 默认 | 范围 | 作用 | 实测影响 |
|---|---|---|---|---|---|
| `accel_v4` | sysfs / module | `2`(hw) | 0..3 | v4 加速路径（probe-only） | 改 v4 走 FFE 前必须先放开 PPE（§1.3） |
| `accel_v6` | sysfs / module | `1`(ffe) | 0,1,3 | v6 加速路径（probe-only） | 运行期用 `debugfs/accel_ipv6` |
| `accel_ipv4`/`accel_ipv6` | debugfs 0644 | 同上 | `stack/ffe/hw/zc` | **运行期**切族 | 切换会 quiesce + 排空 TX（≤100 ms）+ 清该族表（`accel.c:3565-3598`） |
| `txq_limit` | sysfs 0644 | **1024** | 64..1024 | TX 环水位（已提交未回收的帧数） | 满则快路回栈（`fallback_qfull`）/栈路径 `NETDEV_TX_BUSY`；上限=环长 |
| `zxcoal_us` | sysfs 0644 | **250** | 0..1000000 | RX 合并**时间**阈值（写 `v*50` 到 0x350030）；启动时可由 `/etc/config/zx_ffe` 覆盖，见 §11 脚注 | **500→250/62/31：733.7→866.9/922.0/918.8 Mbit/s**；750→497.9；2000→478.1〔本板实测〕 |
| `zxcoal_frames` | sysfs 0644 | **16** | 0..4096 | RX 合并**帧数**阈值；启动时可由 `/etc/config/zx_ffe` 覆盖，见 §11 脚注 | 在 `us=250` 下 `16/32` 再抬到 921–924；`us=500` 下 `frames` 的独立作用**未测**（没有 `500/16`、`500/32`、`500/128` 格）〔未测〕 |
| `zxnapi_weight` | sysfs 0644 | **64** | 1..1024 | NAPI 每轮帧数上限 | `250/64/128` → 909.3（仅 1 格、在散布内，只能写"未观察到 128 的独立效应"）〔本板实测〕 |
| `ffe_aging` | sysfs 0644 / debugfs | **300** | uint，0=永不 | 流表老化秒数 | 活跃条目不会被删（命中会刷 `lastuse`） |
| `learn_delay_ms` | module **0444** | **120000** | ≤600000（超出钳位） | 开机后延时自动开学习 | 必须等 dmesg `ffe_learn auto-enabled 120000 ms after probe` 再测吞吐 |
| `ffe_qfull_drop` | sysfs 0644 | `off` | bool | 环满时"回栈"还是"丢帧" | 打开后 `qfull_drop` 涨、`fallback` 不涨（`accel.c:3243-3251`） |
| `ffe_poison` | sysfs 0644 | `on` | bool | 归还 bp 时投毒并检测毒帧 | 毒帧不进快路（D5） |
| `ffe_bp_drop` | sysfs 0644 | `on` | bool | bp 还在发送环时丢帧而非放行 | 关掉只剩 `ffe_dup` 计数 |
| `accel_autofallback` | sysfs（probe-only）+ debugfs（运行期） | `on` | 0/1 | 连续不合格窗口是否降级 | 降级后 dmesg `fell back` |
| `pool_cacheable` | sysfs（probe-only） | `off` | bool | 包池映射属性 | 决定 RX invalidate / TX clean 是否存在 |
| `zc_rx` | sysfs（probe-only） | `off` | bool | 老的零拷贝总开关（=两族 zc） | 交付态回退，见 features 文档 |
| `timing` | sysfs 0644 | `off` | bool | 每包分段耗时（会污染合并窗口测量） | 与 `zxcoal_*` 实验**不要同时开** |
| `diag_residence/seq/poll/txq/txlat/drop/burst` + `diag_burst_us` | sysfs 0644 | 全 0（`diag_burst_us=2`） | 0/1 | 7 类诊断插桩 | 只影响 `diag` 文件内容；默认关时每包只多一次位测试（`eth.h:902-905`） |
| `zxdly_rx_ns` / `zxdly_tx_ns` | sysfs 0644 | 0 | ns | 纯延迟剂量臂 | 只在做"每包成本 vs 吞吐"实验时用 |

> **§11 脚注（`zxcoal_*` / `zxnapi_weight`）**：默认值 = 本板实测最优 `250 / 16 / 64`
> （编译期常量 `ZX_RX_IRQ_USECS` / `ZX_RX_IRQ_FRAMES` 与 `module_param` 初值同源，
> 见 `zx279128s-eth.h:300-301`、`zx279128s-main.c:961-963`）。**启动时可由
> `/etc/config/zx_ffe` 覆盖**：`/etc/init.d/zx_ffe_coalesce`（`START=99`）在 boot 与
> reload 时读 `zx_ffe.settings.{rx_coalesce_us,rx_coalesce_frames,napi_weight}` 写进
> sysfs 并读回自证；缺失的选项不写（保持驱动当前值），非法值不写并记 syslog。
> 上机临时调仍可直接 `echo` 进 sysfs，但重启后会重新按该配置应用。

## 12. 排查顺序（从"完全不通"到"慢"）

| 步 | 要读的字段 | 期望值 / 判据 | 结论 |
|---|---|---|---|
| 0 | `/proc/uptime`、`dmesg \| grep ffe_learn` | uptime ≥ 130 且出现 `auto-enabled 120000 ms` | 不到点测吞吐**一律作废**（学不到 ⇒ 全 miss） |
| 1 | `accel_ipv4/ipv6` 第 1 行 | 要测的那族是 `ffe` | 不是 ⇒ 先解决模式（§1.3 的 PPE 冲突） |
| 2 | `accel_ipvN` 第 3 行 `ppe_bound` / `hint` | v4 要 ffe 时必须 `ppe_bound=0` | =1 ⇒ 关 flow_offloading_hw |
| 3 | 两个口的 `/proc/net/dev` + `dma_state` 的 `tx_submitted/tx_completed` | 两侧都在涨、`tx_completed` 跟得上 | 完全不涨 ⇒ 帧没上 CPU（trap/MAC 表/线路），不在快路范畴 |
| 4 | `accel_ipvN` 的 `lookup` | 打流时在涨 | 不涨 ⇒ 该族 mode 不是 ffe（D8）或帧在 D1–D7 就被丢 |
| 5 | `hit`/`fwd` | 3–5 s 内明显 > 0 | 恒 0 ⇒ 按 §8.1 顺序读子集：`csum_err/ttl_err/drop_local/drop_noport/fallback/len_short/len_long` |
| 6 | `ffe_flows` 行数 / `ffe_learn` 的 `cand/add` | flows > 0 且打流首秒内增长 | flows=0 ⇒ 学习没工作（§6.5 逐格判） |
| 7 | `ffe_flows` 里那条的 `hits` | 与打流字节数同量级在涨 | 不涨 ⇒ 条目在但数据面判据挡住了（回第 5 步） |
| 8 | `qlen_peak`、`fallback_qfull`、`qfull_drop` | `fallback_qfull` 远小于 `lookup` | 三者接近 `lookup` ⇒ 发送侧是瓶颈（TX 环/出口口/QoS），不是 FFE |
| 9 | `dma_state` 的 `tx_pending`、`ffe_dup`、`bp_double_free`、`bp_state_stale` | `tx_pending` 波动、后三者≈0 | `ffe_dup` 涨 ⇒ bp 所有权/时序问题（P10.5 的账） |
| 10 | 吞吐"慢"但 `hit` 正常涨 | `zxcoal_us/frames`、`zxnapi_weight`、softirq、`diag_poll=1` 的 frames/poll | 单流 < 800 Mbit/s 且中断率 ~1.2 k/s ⇒ 合并窗口就是那个旋钮（§4.3） |
| 11 | `dmesg \| grep "fell back\|alias strike"` | 无 | 有 ⇒ 已自动降级到 stack，别在 stack 上归因 FFE |

## 13. 证据清单

| 断言 | 证据 |
|---|---|
| 四档模式与 `hw` 只属 v4 | `accel.c:5-10`、`eth.h:333-336`、`accel.c:3486`、`main.c:5129-5133` |
| 交付默认 v4=hw / v6=ffe | `main.c:114`、`main.c:118` |
| 数据面只处理 mode==ffe 的族 | `main.c:3498-3511` |
| `ppe_bound` 的语义与它只挡 v4 | `accel.c:3494-3496`、`4234-4236`、`3527-3534`、`3519-3526` |
| 学习 120 s 门闩的机制 | `accel.c:261-263`、`3956-3969`、`3876-3893`、`3909`、`5186-5189` |
| 老化 300 s | `accel.c:232-234`、`1286-1304`、`3070-3072` |
| ports[] 与口名/DTS 的对应 | `DTS:156-195`、`main.c:5273-5318`、`eth.h:228,721`、`main.c:3233` |
| 板级默认 LAN/WAN 划分 | `base-files/etc/board.d/02_network:13` |
| 入口为什么上 CPU（trap/MAC 表） | `main.c:2003-2012`、`1654-1656`、`1600-1605`、`1697-1704`、`eth.h:57-63`、`zx279128s-switch.c:20-39` |
| 合并寄存器与编码 | `eth.h:150-151`、`295-301`、`main.c:965`、`1931-1933` |
| ISR / NAPI 时序 | `main.c:3817-3827`、`3829-3856`、`3852-3853` |
| 描述符解析与合法性 | `main.c:3214`、`3231-3233`、`3236-3288` |
| 判据链（两族） | `accel.c:3108-3183`、`2469-2527`、`2559-2617`、`2435-2449` |
| 查表与提交段 | `accel.c:2758-2762`、`2887-3087` |
| 改写（含 v4 NAT / v6 不碰 csum） | `accel.c:2648-2683`、`2727-2751` |
| TX 提交/门铃/回收 | `main.c:2832-2918`、`2859-2868`、`2055-2119`、`2042-2052` |
| bp 所有权（FORWARDED 不还 bp） | `main.c:3768-3800`、`1458-1469` |
| 学习钩子与候选组装 | `accel.c:4067-4084`、`2070-2352`、`2250-2308` |
| 出口/入口解析与 FDB 反查 | `accel.c:1468-1512`、`1542-1746`、`1758-1780` |
| 学习计数器与 `ffe_learn` 格式 | `eth.h:495-505`、`accel.c:5081-5149` |
| 丢弃点全表 | §8.1 每行的行号（当天 grep） |
| 自动降级判据 | `accel.c:298-337`、`336-337`、`3731-3865` |
| debugfs 文件清单 | `main.c:4967-5009`、`accel.c:4225-4266`、`eth.h:1411-1412` |
| 合并窗口实测 735→924、代价 ×1.7–10.6 中断、softirq 0.40→0.50–0.57 | 〔本板实测〕同一台架、同一条 v6 单流（`iperf3 -6 … -t 30 -O 2`）、31 格扫描，控制组 6 格 688.7–759.5（均值 733.7）；可逆性三次往返；过程记录未入库 |
| 台架拓扑（`lan1`=WAN/`br-lan`=`lan2 lan3`） | 〔本板实测〕台架 uci：`network.@device[0].ports='lan2 lan3'`、`network.wan.device='lan1'`、`wan.ipaddr=192.168.5.1`；`br-lan` 成员 `lan2 lan3 phy0-ap0 phy1-ap0` |
| 扁平态不进 FFE | 〔代码事实 + 推断〕钩子在 `POST_ROUTING`（`accel.c:4069`）、帧键判据 #2（`accel.c:3144-3146`）、硬件直通（`zx279128s-switch.c:20-39`）；**未单独上机验证** |

## 14. 与其它文档的关系

- 概述、慢态旧观测、死旋钮、开机死机史：[features/ffe-fastpath.md](../features/ffe-fastpath.md)
- v6 的 PPE/CLA 为什么不通：[known-issues.md](../known-issues.md) `PPE-IPV6-TCP`、
  `PPE-IPV6-TRAP-CPU`；寄存器层：[reference/cla-registers.md](cla-registers.md)、
  [reference/cla-hash.md](cla-hash.md)
- 原厂 v6 的实测与归属：[reverse/vendor-ipv6-behavior.md](../reverse/vendor-ipv6-behavior.md)
- 整体结构（驱动/模块分工、trap 代价）：[architecture.md](../architecture.md)

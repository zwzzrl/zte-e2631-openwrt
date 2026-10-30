# 零拷贝直通：skb 直接落在 BMU 包池上（2026-10-30）

## 0. 一句话结论

- 收包不再 `memcpy` 进新 skb，而是把这个池缓冲本身 `build_skb()` 成 skb；发包时同一个 `bp`
  直接交给硬件。省掉每包两次 payload 搬运（收一次、发一次）。
- 转发实测（同一镜像 A/B，`ubus call e2631 accel_set` 切换，其余条件不动）：

  | 场景 | zc 关 | zc 开 | 变化 |
  |---|---|---|---|
  | 8 流 v6 TCP 转发（15 s） | 385–472 Mbit/s | **598–722 Mbit/s** | +27 ~ +85 % |
  | UDP 900 M 单流转发 | 606–628 Mbit/s | **716–756 Mbit/s** | +14 ~ +24 % |
  | 驱动内每包 RX 成本 | 17.5 µs | **5.1 µs** | −71 % |
  | 驱动内每包 TX 成本 | 8.1 µs | **1.7 µs** | −79 % |

- 代价是**本机终结**的流量变慢：ssh 收 20 MB 113–124 → 84–88 Mbit/s（−30 %），
  UDP 到盒子 437 → 288 Mbit/s；极端并发（`iperf3 -P4` 到盒子）会撞上池上限而被暂停。
  所以它是**默认开、可在 LuCI 关**的选项，不是写死的：`/etc/config/e2631` 里
  `fastpath.zerocopy`（`1` 开 / `0` 关），页面在**网络 → 转发加速**。

## 1. 为什么这块板子上可行（硬件前提）

这些都是 `docs/vendor-*.md` 那几轮逆向 + 本轮实测确认过的：

- BMU 包池是 4096 × 2048 B 连续内存，`ZX_NUM_BUFFERS`/`ZX_BUF_SIZE` 就是它的几何；
  槽位地址 = `pool_base + bp * 2048`，**帧数据永远从 `+16` 开始**（`ZX_PKT_HEADROOM`）。
- TX 描述符**没有物理地址字段**，只有 `bp`；所以"把一个 bp 交给硬件发送"本来就是
  驱动的原生接口，不需要额外映射。
- TX 缓冲由**硬件**在发送完成后归还 BMU（1.8 M 个发送包后 `bmu_alloc_fail` 仍为 0），
  所以零拷贝发送侧不需要任何 bp 记账。
- 池的 DMA **非相干**（原厂驱动自己调 `dma_cache_maint` + `outer_cache`）。默认映射成
  非缓存（`pool_cacheable=0`），因此零拷贝路径**一笔缓存维护都不做**；可缓存映射下
  每包要失效 1 条 cache line（~190 ns）才能读，实测没有优势，见
  `docs/ffe-cacheable-pool-2026-10-30.md`。

## 2. bp 的四种归属与状态机

`adapter->zc_state[bp]`（每缓冲一字节）：

| 状态 | 含义 | 谁能改 |
|---|---|---|
| `FREE` | 在 BMU 空闲表里，硬件可发可收 | 驱动（申请/归还时） |
| `SKB` | 栈里拿着一个 `build_skb()`，数据就在池里 | 接收路径 |
| `DROPQ` | skb 已析构，bp 进隔离环排队等归还 | destructor |
| `TX` | 交给硬件发送，完成后由硬件归还 | 发送路径 |

记账等式（debugfs 的 `out`）：`out = rx_ok - tx_ok - free_ok`，即"当前不在 BMU 里的缓冲数"。
正常转发时它是**个位数**（突发时几十）。

**隔离环（quarantine）**：skb 销毁时不能立刻把 bp 还给 BMU——`br_netfilter`、
`nf_flow_xmit_xfrm` 这些路径会提前 `skb_orphan()`，此时 skb 本体还在 DMA 里被读，立刻复用
就是数据损坏。所以 destructor 把 bp 塞进 512 条的环（`ZX_ZC_Q_LEN`），**延迟 2 个 tick**
才由 NAPI 里的 `zx_zc_reap()` 归还。环满时挤掉最老的一条并立刻归还（`q_ovf` 计数）：
这是安全性退化（没有隔离窗口），只在极端压力下出现，持续增长说明环太小。

**池页引用垫高**：`build_skb()` 的内存必须是"能 `put_page()` 的页"，而 `dma_alloc_coherent`
给的是一串独立的 order-0 页。直接 `page_frag_free()` 会把页还回伙伴系统。所以每个 4 KiB
页把引用计数垫到 `ZX_ZC_PARK = 1<<20`，NAPI 里每秒 re-park 一次（防止有人 release 掉
我们的垫高），拆除时再 `set_page_count(page, 1)`。

## 3. 三条必须同时维持的约束

任何一条破了，症状都是"慢慢变卡 / 池被抽干 / 串包"，都不会立刻崩，所以写在这里：

1. **GRO 必须关。** 理由**不是**"合并会漏 bp"——本轮实测推翻了那条（见 §6.6：`out`
   一直平，`frag_list` 成员的析构会跑），而是发送侧：合并出来的聚合体要拆段，而发送
   描述符一个 `bp` 只能放一整帧、没有 SG/TSO，第 2..N 段只能 `memcpy` 出新 skb，零拷贝
   的收益被抵消一半以上。GRO 这个 feature 位**只由驱动写**（`zx_eth_open()` 与
   `zx_zc_set()` 里的 `zx_gro_set()`），**不要用 ethtool**：实测 `ethtool -K lan1 gro off`
   反而把 `features` 位留成 on（`ethtool -k` 显示 `on [fixed]`）——`register_netdevice()`
   会强加 `NETIF_F_SOFT_FEATURES`（含 GRO），而 ethtool 在特性不在 `hw_features` 时走
   `wanted_features` 那条路，正好反着来。
2. **`tx_queue_len = 64`。** qdisc 里排队的 skb 每个占一个池缓冲，默认 1000 会把池抽干。
3. **`ZX_ZC_OUT_MAX = 2048`。** 栈最多能同时占住 4096 个池缓冲里的一半；超过就整段退回
   拷贝路径（`capped` 计数 + 一条 ratelimited 内核日志）。这个闸门是"池被抽干导致整台
   机器失联"那次事故的直接产物，见 §6.1。

## 4. 开关的五个入口（按用户可见度排序）

| 入口 | 路径 | 说明 |
|---|---|---|
| 网页 | **网络 → 转发加速** | LuCI 页 `e2631-accel.js`，勾选后立刻生效（不重启） |
| 配置 | `uci get e2631.fastpath.zerocopy` | 默认 `1`；由 `/etc/uci-defaults/61_e2631_fastpath` 建 |
| 开机应用 | `/etc/init.d/e2631-netune` 的 `apply_zc` | 把 uci 值写进 debugfs 旋钮，`reload` 就是"重新应用" |
| 运行态 | `/sys/kernel/debug/92000000.ethernet/zc` | 读 = 状态 + 计数，写 `0`/`1` = 立刻切换 |
| 编译期 | 模块参数 `zc_rx`（builtin，只能 cmdline） | 默认 `true`；`timing` 参数见 §4.1 |

LuCI 页 -> rpcd `e2631 accel` / `accel_set` -> 写 uci -> `/etc/init.d/e2631-netune reload`
-> 写 debugfs。**只有这一条写路径**，页面不直接碰 sysfs。

### 4.1 每包阶段计时（`timing`，默认关）

`echo 1 > /sys/module/zx279128s_eth/parameters/timing` 后，`dma_state` 里会多出
RX 5 段 / TX 4 段的每包均值。一次包要 6 个 `ktime_get_ns()` + 11 次累加，在 1500 B/包
的转发路径上是可测的（~100 ns/包，60 万包/秒 ≈ 7 % 一个核），所以发布版本默认不跑它。

## 5. 实测

### 5.1 驱动内阶段（`timing=1`，同一次测量，单位 µs/包）

| 阶段 | 拷贝路径 | 零拷贝 |
|---|---|---|
| RX 取描述符 | 0.43 | 0.43 |
| RX cache 失效 | 0.10 | 0（非缓存池，不需要） |
| RX 分配/bcopy 或 build_skb | 11.6 | 1.37 |
| RX 交协议栈（`napi_gro_receive`） | 2.94 | 2.94 |
| RX 归还 bp | 0.33 | 0.33 |
| **RX 合计** | **17.5** | **5.1** |
| TX 回收 + 申请 + 拷贝 + 启动 | 8.1 | 1.7 |

### 5.2 转发吞吐（同一镜像、同一时段、只切 zc）

用 `iperf3` 从测试机经盒子转发到桌面（v6 ULA），盒子是纯路由：

| 时段 | 项目 | zc 关 | zc 开 |
|---|---|---|---|
| A | 8 流 TCP | 385–396 | 702–722 |
| A | UDP 900 M 单流 | 606–607 | 745–756 |
| B（重刷后） | 8 流 TCP | 472 | 598 |
| B | UDP 900 M 单流 | 628（丢 18 %） | 716（丢 6.4 %） |

两次时段差 20 % 上下（桌面的链路状态/并发/邻居表都变），但**方向和量级一致**：
zc 开一直更快。会话 A 的 741 / 会话 B 的 716 对得上更早那轮"UDP 900M 612 → 741"的记录。

### 5.3 本机终结（代价所在）

| 场景 | zc 关 | zc 开 |
|---|---|---|
| ssh 收 20 MB（单流 TCP，`dd of=/dev/null`） | 113–124 Mbit/s | 84–88 Mbit/s |
| UDP 600 M 灌到盒子 | 437 Mbit/s（丢 27 %） | 288 Mbit/s（丢 52 %） |
| `iperf3 -P4` 到盒子 | 608 Mbit/s | 34 Mbit/s（异常，见 §6.2） |

原因很直接：数据留在池里，协议栈（校验和、`skb_copy_datagram_iter`）读的是**非缓存**内存，
每字节 ~5 ns；转发路径不碰 payload，所以只有本机终结吃这个成本。

## 6. 坑（都踩过，按疼的程度排）

### 6.1 池被抽干 → 机器失联

第一版没有上限：从宿主机 `ssh` 推 20 MB 固件进盒子（本机收包 + 池缓冲被 socket 队列占住），
`out=3132`、`q_ovf=2974`、`bmu_alloc_fail=23`，ssh 在 819 KB 处 RST，盒子彻底不应答（要串口）。
修复是三条一起上：`ZX_ZC_OUT_MAX` 闸门、`tx_queue_len` 1000→64、隔离环 128→512。
之后再没出现过（最极端的一次本机灌包测试里 `bmu_alloc_fail=0`，连接没断，只是慢）。

### 6.2 本机终结 TCP 的极端案例

`iperf3 -P4` 打到盒子上时，`out` 一路涨到 2048（撞上限）、`capped=1183`、`q_ovf` 37 万，
速率掉到 34 Mbit/s；`bmu_alloc_fail=0`，连接没断，测试结束后 `out` 回到 56。
ssh / HTTP 这类单流只有 20~30 % 损失，所以这是"4 个满速 socket 同时往盒子里灌"的
边界情形，不是常态。**待办**：以 BMU 空闲槽位为水位的自适应闸门（现在的 `bmu_free_slots`
只是缓存的读回值，不能直接当水位用），以及给本机终结路径留 headroom。

### 6.3 GRO 位被注册流程强加

在 probe 里清 `features`/`hw_features` 的 GRO 位是**无效**的：`register_netdevice()`
在 probe 之后才 `dev->features |= NETIF_F_SOFT_FEATURES` 并重算 `wanted_features`。
最终落点是 `ndo_open`（每张网口 up 时按当前 zc 状态定）与 `zx_zc_set()`，
两处都写 `features + wanted_features`。

### 6.4 这个 uci 建不出新的配置包

`uci set e2631.fastpath.zerocopy=1` 在 `/etc/config/e2631` 不存在时直接失败
（`uci: Entry not found`，rc=1，实测）。所以 uci-defaults 与 rpcd 里都是**直接写文件**，
文件已存在时才走 `uci set`/`commit`。

### 6.6 GRO 与 zc：实测不能共存（原先的理由是错的）

第一版写的是"GRO 合并后 frags 指向别的槽位、那条 bp 没人回收"。本轮实测**推翻了这一条**：
把 GRO 打开再跑转发，`out` 一直在 0–120 之间、`bmu_alloc_fail=0`、`free_ok` 正常增长——
`frag_list` 成员的析构确实会跑。真正的代价在发送侧。同机同时段 A/B（zc 都开着，只切 GRO）：

| 项目 | GRO 关 | GRO 开 |
|---|---|---|
| 8 流 TCP 转发 | **641 Mbit/s** | 473 Mbit/s |
| UDP 900 M 单流 | **747 Mbit/s（丢 0.95 %）** | 565 Mbit/s（丢 22 %） |
| 每包软中断（两核合计，同口径 sirq÷收包数） | **15.2 µs** | 28.7 µs |

机制：GRO 把几十个包并成一个聚合体，聚合体在 TX 侧必须拆段才能发（1200–1500 B/帧，驱动
没有 SG/TSO），`skb_segment()` 的第 1 段复用原来的池 skb（零拷贝仍生效），第 2..N 段因为
发送描述符一个 `bp` 只能放一整帧，只能 `memcpy` 出新的 skb。等于把每包一次拷贝又要回来
一半以上；UDP 那 22 % 丢包是 RX 侧来不及收、FIFO 溢出。

**结论（也解释了为什么"用 GRO 摊薄栈成本"这条路走不通）**：发送描述符没有 scatter-gather，
一个 `bp` = 一整帧，所以"合并"永远要在拆段时付拷贝。zc 想省的东西和 GRO 想省的东西在
这份硬件上互相抵消。

顺带记一个未复现现象：那一轮刚开始的 8 流 TCP 里盒子重启过一次（计数器与 uptime 归零），
之后用 syslog 流抓了两轮同样负载都没再复现，也没有任何内核警告。不写进结论，只留档：
以后真要做 zc + GRO，先解释它。

### 6.5 `q_ovf` 的含义

隔离环满时挤掉最老条目并**立刻**归还 bp —— 等价于"这条没有隔离窗口"。
它增长说明隔离环太小或瞬时释放太集中，不代表泄漏（`bmu_alloc_fail` 才是泄漏信号）。

## 7. 刷机与验证

正常走 `sysupgrade -n`（见 `工作文件/01-文档/路由器当前状态与回滚手册.md` §7.2），
槽 0 不参与。刷完必看四样：

```sh
ssh root@<盒子> 'cat /proc/uptime; e2631-fwslot info | tail -1'   # 槽 1 序号 +1、uptime 变小
ssh root@<盒子> 'uci -q get e2631.fastpath.zerocopy'              # 1
ssh root@<盒子> 'head -1 /sys/kernel/debug/92000000.ethernet/zc'   # 1
ssh root@<盒子> 'ethtool -k lan1 | grep generic-receive'           # off（zc 开时）
ssh root@<盒子> 'ubus call e2631 accel'                            # JSON：avail/uci/drv/counters
```

命令行关门（不想开网页时）：

```sh
ssh root@<盒子> "uci set e2631.fastpath.zerocopy=0 && uci commit e2631 && /etc/init.d/e2631-netune reload"
```

## 8. 下一步（按收益排序）

0. **（本轮已证伪）靠 GRO 摊薄栈入口**：见 §6.6，发送侧没有 SG，合并必然在拆段时拷贝，
   实测吞吐与丢包都变差。留在协议栈里的路只剩下面第 1 条那种"少碰字节"的优化，
   想追平原厂 4.7 µs 只能走驱动内流快路（不进 `netif_receive_skb`）。
1. **本机终结路径**：§6.2 的水位闸门 + 给 `build_skb` 留 headroom，让协议栈少碰非缓存字节。
2. **校验和卸载**：`rx-checksumming`/`tx-checksumming` 现在都是 off，本机终结的 TCP 每包
   还要在非缓存内存上算一遍 checksum。
3. **TX 批量化**：`TX_START` 每包一次 MMIO（固定成本 ~1.7 µs 里的一部分）。
4. 桥接出口直出（省一次 FDB 查找），与零拷贝正交，见
   `docs/ffe-cacheable-pool-2026-10-30.md` §7。

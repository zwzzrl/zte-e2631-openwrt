# 收包池可缓存化：把软件转发的每包成本砍下来（2026-10-30 夜）

> 这是"优化开源固件自己的 FFE"第一步。原厂 FFE 为什么能跑满速、我们的软件路径每包贵在
> 哪里、这次改了什么、怎么验证，都在这里。
> 所有数字都来自这台机器（ZXHN E2631，OpenWrt v12fix 跑在 slot1）的实测，命令附在每节。
> **改动的效果数字见 §6，尚未填写的项明确标注为未测**。

---

## 0. 一句话结论

| 问题 | 答案 |
|---|---|
| 原厂 FFE 是什么 | 内核里一层独立转发面（`protocol/ffe/`），不是硬件表命中；§1 有源码路径与符号名 |
| 它快在哪 | 帧**原地处理**：用 "fkb"（fast kernel buffer）指向驱动的 DMA 缓冲，只做 cache invalidate，不拷贝；自带 per-CPU 发送队列与 backlog；数据面不走桥/路由/nft |
| 我们的每包成本 | 1500 B UDP 收包 **13.8 µs/包**；64 B 小包 **6.1 µs/包** → 差额 7.7 µs 全是那 1500 B 的拷贝 |
| 拷贝为什么这么贵 | 包池是 `dma_alloc_coherent()` 的**非缓存内存**，实测 1500 B 拷贝要 **8.2 µs（184 MB/s）**，缓存内存只要 **0.48 µs** |
| 本次改动 | 包池从 coherent arena 里剥离，改成 **8 MiB 可缓存连续内存 + streaming DMA 映射**，收包前失效、发包前回写；分配失败自动退回旧行为 |
| 预期 | 收包侧 8.2 µs 拷贝 → 约 0.5 µs（+一次 cache 失效），每包成本 13.8 → ~6.5 µs |

## 1. 原厂 FFE 的实体：内核里的独立转发面

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

原厂 v6 路由 918–928 Mbit/s、4.7 µs/包（`docs/vendor-v6-forwarding-2026-10-30.md` §8），
在"不拷贝 + 不进协议栈 + 自己的队列"这个结构下是自洽的。

## 2. 我们的每包成本：先量，再改

### 2.1 单次 MMIO 的代价

盒子上没有 perf/ftrace，所以用 `/dev/mem` 直接量寄存器访问（`.scratch/ffe-opt/regtiming.c`）：

| 操作 | 实测 |
|---|---|
| 一次 MMIO 读（TM/BMU/DMA 区） | **300 ns** |
| 一次 MMIO 写（posted） | 329 ns |
| 写 + 读回（依赖往返） | 666 ns |
| 1500 B memcpy（缓存内存） | 482 ns |

⇒ 驱动每包大约 6–8 次寄存器访问，合计约 **2 µs**，不是主要矛盾。

### 2.2 每包 CPU 时间：大包 vs 小包

主机用 iperf3 UDP 灌进盒子（盒子自己收，`iperf3 -s`），读 `/proc/stat` 与 `lan1` 收包计数：

| 包长 | 收到包数 | 时长 | sirq 合计 | 每包 |
|---|---|---|---|---|
| 1400 B | 356 902 | 10 s | 4.93 核秒 | **13.8 µs** |
| 64 B | 692 667 | 10 s | 4.22 核秒 | **6.09 µs** |

固定成本 6.1 µs，多出的 7.7 µs 随字节数增长 → 换算 **5.8 ns/字节 ≈ 170 MB/s** 的拷贝速度。

同时确认不是中断风暴：`/proc/interrupts` 里 gmac 中断 10 426 次 / 356 902 包 = **1/34 包**，
RX 中断合并工作正常。

### 2.3 直接量包池的拷贝速度

包池物理地址取自 `ZX_TM_BUF_START`（读出来 `0x4e100000`），用 `/dev/mem` 映射（属性与
`dma_alloc_coherent()` 相同：非缓存）：

| 方向 | 1500 B | 128 B |
|---|---|---|
| **非缓存 → 缓存**（= 收包时的 memcpy） | **8155 ns（184 MB/s）** | 716 ns |
| 缓存 → 非缓存（= 发包时的 skb_copy_bits） | 797 ns（写合并，1882 MB/s） | 219 ns |

⇒ 收包那一次拷贝就是 8.2 µs，占总成本的一半以上；**发包方向因为写合并已经不贵**。
这与 §2.2 的差值（7.7 µs）互相印证。

## 3. 改动：包池改成可缓存内存

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

## 4. 怎么验证

**改动生效的直接证据**（刷机后）：

```sh
dmesg | grep -i "cacheable packet pool"   # 没有这行 = 分配成功（有这行说明回退了）
```

**每包成本**（主机 → 盒子 UDP，§2.2 的同一脚本）：期望 1400 B 的每包成本从 13.8 µs 降到
6.5–8 µs，同时同样 10 s 内能收更多包。

**端到端**：8 流 TCP 经盒子路由，比较改动前后的 Mbit/s 与每核 sirq。

## 5. 风险与边界

* 硬件在 DMA 写入某个槽时，CPU 可能正持有该槽上一轮的缓存行；因为每次读取前都失效、
  每次写入后都回写，不存在"CPU 看到的旧数据被当成新帧"的路径。
* 失效范围从槽基址开始（对齐），16 B 头与帧在同一个 32 B 行内，不会越界到下一个槽。
* 若某次 `dma_sync` 语义在 ARM 上退化为整片失效（内核的实现是按范围做），最坏结果是
  多花时间，不影响正确性。

## 6. 结果

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
   这一刀当天深夜就做了：`docs/ffe-zero-copy-2026-10-30.md`（收包 `build_skb` 池缓冲、
   发包把同一个 bp 交给硬件，RX 每包 17.5 → 5.1 µs、TX 8.1 → 1.7 µs，
   转发 8 流 TCP 385 → 700 Mbit/s）。
3. 转发吞吐这一轮**没有可用的对比数字**：8 流 v6 路由测到 391 Mbit/s，但那次测试台我手工往
   fw4 里插了 `iifname lan2 accept` 并且插在 `flow add @ft` 前面，软快转的建流规则被挡在后面，
   那一轮的 CPU 分布不可比。正确的转发口径要等
   `docs/wanport-tx-deaf-2026-10-30.md` 那个交换芯片发送 bug 修好、用 wan↔lan 台重测。


## 7. 下一步（按收益排序）

1. 若 §6 收益符合预期，下一刀砍**固定成本 6.1 µs**：发包侧 `TX_START` 批量化（每 N 包一次
   寄存器写）、BMU 申请/释放从"每包一次锁 + MMIO"改成软件自由链表 + 批量归还。
2. 再看把桥接出口（`br-lan`）换成端口直出（`rxdev/txdev` 缓存），省一次 FDB 查找。
3. ~~才有必要考虑 fkb 式零拷贝~~ **已做**：`docs/ffe-zero-copy-2026-10-30.md`。
   skb 直接 `build_skb()` 池内缓冲，析构时把 bp 送进隔离环延迟归还；池缓冲的三种归属
   （FREE/SKB/DROPQ/TX）、`ZX_ZC_OUT_MAX` 闸门、GRO 必须关、`tx_queue_len` 必须小，
   都在那篇里。默认开、LuCI「网络 → 转发加速」可关。

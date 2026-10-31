# 能否把主线零拷贝 RX 代码/范式引入 ZX279128S 以太网驱动 —— 评估报告

> 对象：ZXHN E2631（ZX279128S，2×Cortex-A9，248 MiB，内核 6.18.55，ARM32）
> 问题：把 Linux 主线的零拷贝 RX 代码/范式搬进本驱动的自研实现，替换它
> 范围：**纯离线代码取证**。没有碰串口/盒子/源码；没有刷机；所有源码与内核树只读。
> 本文只给「可行 / 有条件可行 / 不可行 + 条件 + 代价」，**不做路径选择**。
> 生成日期：2026-11-01

## 证据路径前缀约定

正文与附录里的 `X/文件:行号` 用下面三个前缀，避免每条都写全路径：

| 前缀 | 实际路径（相对仓库根 `工作文件/`） |
|---|---|
| `D/` | `08-源码与构建树/zte-e2631-openwrt/overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/`（`zx279128s-main.c` / `zx279128s-eth.h`） |
| `K/` | `08-源码与构建树/openwrt/build_dir/target-arm_cortex_a9_musl_eabi/linux-sanechips_generic/linux-6.18.55/`（内核 6.18.55 源码树） |
| `DOC/` | `08-源码与构建树/zte-e2631-openwrt/docs/` |

**章节与问题的对应**：§1=Q1（硬件约束）；§2=Q2（主线范式清单）；§3=Q2 的对照；§4=Q3（netmem 专项）；§5=Q4（主线规避机制）；§6=Q5（候选路径评分）；§7=Q6（不可行清单，为便于阅读单列一节）；§8=未证实清单；§9=证据行号索引。

本文所有行号都是**当前 HEAD 的实读结果**（`zx279128s-main.c` 3074 行、`zx279128s-eth.h` 506 行）。凡文档与代码冲突的地方以代码为准，并在正文标出。

---

## 0. 结论摘要

### 0.1 一张表

| 方案 | 可行性 | 改动量 | 风险 | 关键证据行号 |
|---|---|---|---|---|
| ① 保留自研影子机制，改设计（解队头阻塞 + epoch + 偷 head 只认泄漏 + 泄漏上限） | **有条件可行**（条件见 §6.1） | 中：`D/zx279128s-main.c` 的 `zx_zc_*` 约 480 行里改 150~250 行 | 中高：仍是"假页计数 + 影子引用"两层自造语义，只能做到"宁漏不双还" | `D/zx279128s-main.c:1297-1383`、`:1390-1446`、`:1119-1132`；`K/include/linux/skbuff.h:1298-1314`；`K/net/core/skbuff.c:6122-6130` |
| ② 整体换 page_pool（page_pool + build_skb + napi_gro_receive） | **不可行** | ——（等于重写收发两侧，600+ 行） | 极高：硬件地址模型不匹配，做不出来 | `D/zx279128s-main.c:866-867`、`:1734`、`:1662-1663`；`K/net/core/page_pool.c:277-304` |
| ③ head 用普通 skb、数据只挂 frag（`skb_add_rx_frag`） | **有条件可行**（前置：1 个 bp = 1 个 4 KiB 页；该前置未证实） | 中：删掉 `D/zx279128s-main.c:1075-1555` 整块，重写交付段与 TX 拷贝 | 中：改池几何属硬件实验；TX 侧必然退回拷贝 | `K/include/linux/skbuff.h:361-365`、`:5267-5270`、`:2586-2589`；`D/zx279128s-main.c:2793`、`:866-867` |
| ④ 放弃零拷贝，回到 copy（`napi_alloc_skb` + `memcpy`） | **可行**（现成，`zc_rx=0` 就是这个行为） | 小：关开关即可；彻底删约 600 行 | 低（但吞吐回落到实测 385–472 Mbit/s） | `D/zx279128s-main.c:1907-1913`、`:1920`；`DOC/ffe-zero-copy-2026-10-30.md:10-14` |
| ⑤ 零拷贝 + 驱动内转发快路（不进 `netif_receive_skb`，原厂 FFE 那种） | **可行但要重做一个转发面** | 大：新增 800~1500 行，绕开 bridge/nft | 高：功能回归面最大 | `DOC/ffe-cacheable-pool-2026-10-30.md:21-49`；`D/zx279128s-main.c:1920` |

### 0.2 三句话先说清楚

1. **硬件不允许驱动按包给地址。** RX/TX 描述符里只有一个 15 bit 的缓冲号 `bp`，物理地址由 BMU 自己用「窗口基址 + bp×2048」算出来（`D/zx279128s-main.c:1734`、`:1662-1663`、`:866-867`）。所以"让网卡 DMA 进 page_pool 的页"这件事在本硬件上**不存在实现路径**——不是难，是没有入口。§1 给出完整取证。
2. **我们现在的 bug 不是"用错了 API"，而是把权威计数放错了对象。** 自研机制把"最后一个引用"押在 `skb_shared_info->dataref` 上，而 `dataref` 只在 `skb_release_data()` 里减；主线有一条合法的"只跑 destructor、不走 `skb_release_data`"的路径（`kfree_skb_partial(skb, true)`），而且我们**恰好会走进它**（TCP 收包合并偷 head）。主线自己不出这个问题，是因为它把权威计数放在**内存对象**（page 的 refcount / `pp_ref_count`）上，而不是放在 skb 上。§4、§5 给出链条。
3. **真正能在本硬件上久跑的设计，只有"bp 的所有权状态机"这一类**（①③⑤ 都属于这类），区别只是"把最后一次归还的判断建立在什么之上"。能不能把它做成不变量、还是只能做成"宁漏不双还"，是本报告 §6 的核心分歧点。

---

## 1. 硬件约束取证（Q1，决定性问题）

### 1.1 池从哪来、多大、多少个、是否连续

| 问题 | 结论 | 证据 |
|---|---|---|
| 几何 | **4096 个 × 2048 B = 8 MiB** | `D/zx279128s-eth.h:220-222`（`ZX_NUM_BUFFERS 4096`、`ZX_BUF_SIZE 2048`、`ZX_BUF_POOL_SIZE`） |
| 从哪分配（默认） | **16 MiB `dma_alloc_coherent` 事务区的头 8 MiB**，不是单独分配 | `D/zx279128s-main.c:2752-2757`（`dma_alloc_coherent(ZX_DMA_ARENA_SIZE=0x1000000)`）；`:2792-2795`（拿不到 cacheable 池就 `pkt_pool = buffer_pool`，偏移 0）；`D/zx279128s-eth.h:260-265`（arena 布局：tab0 在 0x800000、rx 在 0xa00000、tx 在 0xc00000，说明池占 `[0, 0x800000)`） |
| 从哪分配（`pool_cacheable=1`） | `alloc_pages_exact(8 MiB)` + `dma_map_single(DMA_BIDIRECTIONAL)`，**普通可缓存内存 + streaming 映射** | `D/zx279128s-main.c:2775-2791`；释放路径 `:2677-2688` |
| 物理连续 | **必须连续**。BMU 只有「基址 / 结束地址」两个寄存器，槽地址 = 基址 + bp×2048，没有任何 per-buffer 地址表 | `D/zx279128s-main.c:866-867`；代码注释明说 "8 MiB, physically contiguous" `D/zx279128s-main.c:2759` |
| 运行时物理基址 | 实测读出 `0x4e100000`（1 MiB 对齐，**不是** 8 MiB 对齐） | `DOC/ffe-cacheable-pool-2026-10-30.md:81-82` |
| 是否 coherent | 默认是（非缓存，`dma_alloc_coherent`）；`pool_cacheable=1` 时是 cacheable + 每包一次 `dma_sync_single_for_{cpu,device}` | `D/zx279128s-main.c:1810-1815`、`:1643-1647`；两条路径的实测代价见 `D/zx279128s-main.c:2758-2774` |
| 在 struct page 体系内吗 | **在**。`virt_to_page()` / `virt_to_head_page()` / `page_ref_add()` 都在用 | `D/zx279128s-main.c:1127`、`:1130`、`:1156`、`:1378` |
| 页的形态（是否复合页） | **不是复合页，而且必然是 CMA 路径**。本镜像 `CONFIG_DMA_CMA=y`、`CONFIG_CMA_SIZE_MBYTES=32`（`K/.config:4096`、`:4101`），ARM 启动时 `dma_contiguous_reserve()` 就建好默认 CMA 区（`K/arch/arm/mm/init.c:199-200`），`dev_get_cma_area()` 因此必然返回默认区（`K/include/linux/dma-map-ops.h:102-107`）→ `cma_allocator` → `dma_alloc_from_contiguous()` → `cma_alloc()` → `__cma_alloc(..., GFP_KERNEL \| __GFP_NOWARN)`；**gfp 里没有 `__GFP_COMP`**，所以 `alloc_contig_range()` 走 `split_free_pages()` 分支，把整块拆成 4096 个**各自 refcount=1 的独立 order-0 页** | `K/arch/arm/mm/dma-mapping.c:572-578`（有 CMA 区选 `cma_allocator`）、`:391-420`、`K/kernel/dma/contiguous.c:316-322`、`K/mm/cma.c:933` → `:842`、`K/mm/page_alloc.c:7122-7126`（`!__GFP_COMP` 分支）→ `:6925-6949`（`split_free_pages`）、`:3097-3108`（`split_page` 逐页置 1）；`alloc_pages_exact` 路径同形：`K/mm/page_alloc.c:5487-5491`、`:5446-5466`；万一没有 CMA 区则落到 `simple_allocator`（`alloc_pages`+`split_page`，`K/arch/arm/mm/dma-mapping.c:141-158`），形状仍相同 |
| 每个槽能拿到独立 `struct page*` 吗 | **不能，但原因不是复合页**。`virt_to_head_page(任意槽)` 返回的是该槽所在的那一页本身（页是独立计数的）；问题在于**页粒度 4 KiB ≠ 槽粒度 2048 B，一个页里有两个槽**，所以不存在"per-slot 的页引用计数"。`pool_cacheable=1` 时（`alloc_pages_exact()`，同样非复合、逐页 1）形状完全一致 | 页粒度：`D/zx279128s-main.c:1120-1132`（park 按 `PAGE_SIZE` 步进，8 MiB 池 = 2048 页对应 4096 个槽）、`D/zx279128s-eth.h:220-222`；`K/mm/page_alloc.c:5487-5491`、`:5446-5466`（`alloc_pages_exact` 剔除 `__GFP_COMP` 并逐页置 1） |
| 一个槽能放多大的帧 | 帧从槽内 `+16` 开始，最大 1536（驱动硬校验 `len > 1536` 直接当坏帧） | `D/zx279128s-main.c:1739`、`:1889-1890`；实测单描述符可发到 2032 B `DOC/tx-descriptor-probe-2026-10-31.md:40-45` |

### 1.2 描述符里写的是 buffer ID 还是物理地址？

**只有 buffer ID（bp），没有物理地址字段。** RX 和 TX 都看过了：

- **RX**（硬件填、驱动读）：`bp = desc[7] >> 1 | desc[8] << 7`（15 bit），`len = (desc[3] >> 2) & 0x3fff`，端口 `desc[6] >> 3`，release class `desc[6] & 1`。全部解析见 `D/zx279128s-main.c:1734-1736`；16 B 描述符里没有一处被当作地址用。
- **TX**（驱动填、硬件读）：`desc[0]/[1]/[2]/[3]` 是队列号、长度、标志，`byte7/byte8` 里塞 bp（`((u8*)desc)[7] = (bp & 0x7f) << 1; ((u8*)desc)[8] = bp >> 7;`）——`D/zx279128s-main.c:1657-1663`。
  这一条特别硬：**如果描述符里有地址字段，驱动就必须自己写**；驱动没写，硬件也照发不误，说明地址是硬件从 bp 算出来的。

### 1.3 BMU 内部 buffer→物理地址映射：硬件固定布局，软件不可编程

驱动向 BMU/TM 写的全部"几何"寄存器只有这些（`D/zx279128s-main.c:862-900`）：

```c
writel((u32)adapter->tab0_dma,              base + ZX_TM_TAB0_ADDR);   /* 空闲索引表0 地址 */
writel((u32)adapter->tab1_dma,              base + ZX_TM_TAB1_ADDR);   /* 空闲索引表1 地址 */
writel(0x08000800,                          base + ZX_TM_TAB_LIMIT);   /* 语义未知 */
writel((u32)adapter->pkt_pool_dma,          base + ZX_TM_BUF_START);   /* 池基址 */
writel((u32)adapter->pkt_pool_dma + 8MiB,   base + ZX_TM_BUF_END);     /* 池结束 */
writel((u32)adapter->rx_ring_dma,           base + ZX_TM_BUF_F0);
writel(16,                                  base + ZX_TM_CONFIG_START);
...
writel(ZX_NUM_BUFFERS / 32 - 1, base + ZX_BMU_LIMIT1);   /* 池大小，单位=32 个缓冲 */
writel(15,                      base + ZX_BMU_LIMIT2);
writel(ZX_NUM_BUFFERS << 16,    base + ZX_BMU_THRES1);
writel(0x2000000,               base + ZX_BMU_THRES2);
writel(ZX_BMU_CTRL_EN,          base + ZX_BMU_CTRL);
```

结论与依据：

1. **软件能改的只有"窗口"**：基址、结束地址、条数（"单位 = 32 个缓冲"`D/zx279128s-main.c:891-895`）。没有任何寄存器能把"第 N 个 bp 指向物理地址 P"写成一张表。
2. **`tab0`/`tab1` 不是地址表，是空闲 bp 索引表**：内容是 `cpu_to_be16(i)`，即 0…4095 / 0…511（`D/zx279128s-main.c:849-855`）。16 位放不下这颗芯片的物理地址，所以它只能是 bp 值。
3. bp 的分配序是**散列**的，不是顺序的：实测 `1024 / 515 / 772 / 2305 / 3586 …`（`DOC/tx-descriptor-probe-2026-10-31.md:71-73`）。这既证明"bp→地址"是固定步长的映射（否则地址会乱），也顺带否掉了"靠相邻 bp 拼大帧"的想法。
4. 槽内固定 16 B 头、帧从 `+16` 起（`D/zx279128s-main.c:1889-1890` 的 `skb_reserve(skb, 16)`、`:1820` 的 `pkt_pool + bp*2048 + 16`）。这一个不变量在整个驱动里被硬编码了十几处，也说明槽长/槽内布局不是软件随便定的。

**所以：不能。** 驱动无法让硬件把 RX 数据 DMA 到任意物理页（page_pool 给的页），也无法让硬件从任意页取数据发送。

### 1.4 page_pool 在本硬件上：**不可以**

- **无条件的不可行**，理由就是 §1.2/§1.3：page_pool 的前提是"驱动按包提供 `netmem`（页或 net_iov），网卡 DMA 地址由驱动写进描述符"。本硬件的描述符没有地址字段，地址由 BMU 用一个连续窗口 + 固定步长算出来。page_pool 分配出来的页是散落的 order-0 页，既不能拼成这个窗口，也不能逐包塞进去。
- 唯一在纸面上能"接上"的通路是 6.18 的 netmem 内存提供者（memory provider）：把自研池包装成 `net_iov`，让 page_pool 用它当后备内存。这条路**被 §4 的三道门挡死**（只有 dmabuf/io_uring 两个提供者、只能从 netlink 装、net_iov 内存对 CPU 不可读）。也就是说：**即使用最激进的手段，page_pool 也只能承载"CPU 永远不读"的包；而我们要修的 bug 恰好发生在"CPU 要读"的本机终结路径上。**
- 条件（如果非要说"有条件"）：只有当 (a) 硬件槽长可编程为 4096 且 (b) 驱动愿意把池交给 page_pool 自己的分配器（意味着放弃 8 MiB 连续窗口）时，才谈得上 page_pool。而 (b) 与 BMU 的窗口模型直接冲突，所以 (a) 成立也救不了 page_pool——(a) 只对 §6 的路径 ③ 有用。

### 1.5 顺带取到的两条硬限制（后面 Q6 要用）

- **没有 SG/TSO。** `byte7 bit0` 实测不是链式位：两个描述符会发成两帧（`DOC/tx-descriptor-probe-2026-10-31.md:29-39`）；一帧写多个描述符时第二个 bp 永不归还（同文档 `:54-58`）；门铃必须一描述符一次（`:59-64`）。
- **netdev 没有非线性能力。** `hw_features`/`features` 只加了 `NETIF_F_HW_TC`（`D/zx279128s-main.c:2880-2881`），没有 `NETIF_F_SG`/`NETIF_F_FRAGLIST`/TSO。

---

## 2. 主线范式清单（Q2）

前置说明：6.18 里 RX 侧的"零拷贝"只有一条主线正统路线——**内存由可回收的内存对象承载（page_pool 或 page_frag），skb 只是持有者**。下面四条按"是否零拷贝 + 权威计数在哪"排列。

### a) page_pool + `build_skb`/`napi_build_skb` + `napi_gro_receive`

**代码位置（主线实现）**

| 环节 | 位置 |
|---|---|
| 池创建 | `K/drivers/net/ethernet/marvell/mvneta.c:3372`；`K/drivers/net/ethernet/stmicro/stmmac/stmmac_main.c:2056`；`K/drivers/net/ethernet/mediatek/mtk_eth_soc.c:2137` |
| 取页 → 组 skb | mvneta：`K/.../mvneta.c:2634`（`build_skb(data, frag_size)`）；stmmac：`K/.../stmmac_main.c:5670`（`napi_build_skb(page_address(buf->page), …)`）；mtk：`K/.../mtk_eth_soc.c:2519`、`:2560` |
| 标记回收 | `skb_mark_for_recycle(skb)`：`K/include/linux/skbuff.h:5360-5365`；调用点 `K/.../mvneta.c:2410`、`K/.../stmmac_main.c:5685`、`K/.../mtk_eth_soc.c:2532` |
| 上送 | `napi_gro_receive`：`K/.../mvneta.c:2516/2604/2654`、`K/.../stmmac_main.c:5734`、`K/.../mtk_eth_soc.c:2612` |
| 组包函数本体 | `K/net/core/skbuff.c:498-509`（`build_skb`，设 `head_frag=1`）、`:566-576`（`napi_build_skb`，走 NAPI per-cpu skb 缓存） |

**前置条件**

1. 驱动**按包**把 DMA 地址写进描述符（地址来自 `page_pool_get_dma_addr()` 或 `netmem` 的 `dma_addr`）；
2. 池的后备内存必须是 page_pool 能分配/回收的对象：order-0 页（或 order-N 页 + `page_pool_alloc_frag`），**或者**由内存提供者给出的 `net_iov`；
3. DMA 映射与同步由驱动或池负责（`PP_FLAG_DMA_MAP` / `PP_FLAG_DMA_SYNC_DEV`）；
4. `skb_mark_for_recycle()` 让 `skb_free_head()` 走 `skb_pp_recycle()` 而不是 `put_page()`（`K/net/core/skbuff.c:1071-1081`、`:1028-1038`）；
5. 销毁顺序：先停 NAPI/停 DMA，再 `page_pool_destroy()`。

**我们现状差距**：条件 1 不成立（§1.3），条件 2 也不成立（池必须物理连续、由一个窗口寄存器描述）。**不是工作量问题，是模型不匹配。**

### b) `napi_alloc_skb` + `skb_add_rx_frag`（头线性、数据挂 frag）

**代码位置**

| 环节 | 位置 |
|---|---|
| 头 + frag 混合组包 | `K/drivers/net/ethernet/stmicro/stmmac/stmmac_main.c:5686-5696`（`dma_sync_single_for_cpu` 后 `skb_add_rx_frag(skb, nr_frags, page, page_offset, len, truesize)`） |
| 另一种"frag 来自 page_frag"的经典写法 | `K/drivers/net/ethernet/mediatek/mtk_eth_soc.c:2536-2538`（`napi_alloc_frag(ring->frag_size)`） |
| frag 的数据结构 | `K/include/linux/skbuff.h:361-365`（`skb_frag_t = { netmem_ref netmem; u32 len; u32 offset; }`） |
| frag 不额外取引用 | `K/include/linux/skbuff.h:2586-2589`（注释原文 "Does not take any additional reference"） |
| frag 的 ref/unref | `K/include/linux/skbuff_ref.h:18-21`（`__skb_frag_ref` → `get_netmem`）、`:37-57`（`__skb_frag_unref` → `skb_page_unref` → 可选 `napi_pp_put_page` → `put_netmem`） |
| frag 加进 skb | `K/net/core/skbuff.c:870-878`（`skb_add_rx_frag_netmem`） |

**前置条件**：数据所在内存必须是 `netmem_ref`（`struct page *` 或 `struct net_iov *`），并且**引用计数是真实的**——因为 frag 的释放只认 `put_netmem()`/`put_page()`，驱动拿不到任何回调。另一个隐含条件是：头是 kmalloc 头（`head_frag=0`）时 `skb_head_is_locked()` 恒为真（`K/include/linux/skbuff.h:5267-5270`），所以栈**永远不会**把这种头偷进 frag，这条路线天然免疫 §5 的坑。

**我们现状差距**：`skb_frag_t` 要的是 `netmem_ref`，而引用计数是按 **4 KiB 页**记的，不是按 bp 记的——一个页里两个槽（`D/zx279128s-main.c:1120-1132`、`D/zx279128s-eth.h:220-222`），所以**不存在 per-slot 的页引用计数**（页本身是独立 order-0 页，不是共享的复合页，见 §1.1）。要让 b) 成立，必须"1 个 bp ↔ 1 个 4 KiB 页"，即槽长改成 4096（未证实可编程，见 §8 #3），或者在归还路径上自己维护 per-bp 引用账、把页引用计数只当成"arena 的归属标记"来用（`zx_zc_park_pages()` 现在正是这么绕的，代价见 §5.4）。此外本驱动没有 `NETIF_F_SG`，frag 化之后任何从本网口发出的帧都会被 `__skb_linearize()` 线性化（`K/net/core/dev.c:4044` → `K/include/linux/skbuff.h:4343-4348`），转发路径白付一次拷贝。

### c) XDP（`xdp_convert_buff_to_frame` / page_pool）

**代码位置**：`K/include/net/xdp.h:430-446`（`xdp_convert_buff_to_frame`，内部在 `:439` 调 `xdp_update_frame_from_buff`，后者声明在 `:400-406`）；典型驱动用法 `K/.../mvneta.c:2183`、`K/.../stmmac_main.c:5051`、`K/.../mtk_eth_soc.c:2377-2391`。

**前置条件**：`ndo_bpf`/`ndo_xdp_xmit`、每 RX 队列一个 `xdp_mem_info`（page_pool 注册 xdp mem id）、`xdp_buff` 需要有 `data_hard_start`/`frame_sz` 且帧在页内线性可读、以及 `bpf_prog_run_xdp()` 返回码的全套处理。**我们完全没有**：`zx_netdev_ops` 只有 7 个回调，没有 `ndo_bpf`（`D/zx279128s-main.c:2070-2078`）。

**仅作对照**：XDP 的价值是把"每帧是否上送"的决策放到驱动里，从而可以把 `XDP_DROP/XDP_REDIRECT` 的帧直接从 page_pool 归还，**CPU 一个字节都不读**。这恰好是 net_iov（不可读内存）能用的场景，也是主线唯一能容忍"不可读 netmem"的地方（`K/include/net/page_pool/types.h:25-32`）。我们的问题是"CPU 要读"，所以 XDP 帮不上忙。

### d) 纯 copy（`napi_alloc_skb` + memcpy）

**代码位置**：`K/drivers/net/ethernet/realtek/8139too.c:2042-2058`（`napi_alloc_skb` → `skb_copy_to_linear_data` → `skb_put` → `netif_receive_skb`）。

**前置条件**：无。这是所有驱动都能用的基线。

**我们现状**：这就是 `zc_rx=0` 的行为（`D/zx279128s-main.c:1907-1913` 拷贝、`:1920` 上送）。实测代价：RX 17.5 µs/包、转发 8 流 TCP 385–472 Mbit/s（`DOC/ffe-zero-copy-2026-10-30.md:101-123`）。

### e) 补充：本树里还有一类"page_frag + build_skb"（不走 page_pool）

`napi_alloc_skb` 在 `len ≤ SKB_WITH_OVERHEAD(PAGE_SIZE)` 时用 `page_frag_alloc()` 拿头，然后 `skb->head_frag = 1`（`K/net/core/skbuff.c:812-866`，`:859`）。所以**连我们的拷贝路径的 skb 头也是 `head_frag=1`**，也就是说栈同样可以把拷贝路径的 head 偷进 frag——但那是安全的，因为那块内存的页计数是真的（还挂在 `napi_alloc_cache` 的 page_frag 上）。这一条是 §5 的关键反例：**"head 被偷"本身不是 bug，计数放错地方才是 bug。**

---

## 3. 逐范式与我们现状对照

| 范式 | 权威计数在哪 | 网卡能否写进去 | 我们缺什么 | 现有代码位置 |
|---|---|---|---|---|
| a) page_pool + build_skb | page `pp_ref_count` / `net_iov.pp_ref_count` | 需要描述符带地址 → **不能** | 硬件地址模型（无法补） | 现有最接近的写法 `D/zx279128s-main.c:1880-1882`（`build_skb`） |
| b) 普通 skb 头 + frag | page refcount | 需要页级地址 → **部分不能**（一页两槽） | 1 bp = 1 页；`NETIF_F_SG` | 无 |
| c) XDP | page_pool mem id | 同 a | ndo_bpf 全套 | 无（`D/zx279128s-main.c:2070-2078`） |
| d) 纯 copy | 无（slab/page_frag 自带） | 不需要 | 无 | `D/zx279128s-main.c:1907-1920` |
| 我们现状（自研） | `skb_shared_info->dataref` 影子 + `zx_state[bp]` + 隔离环 | 不需要（硬件自己找地址） | 一个"最后一个引用"的权威信号 | `D/zx279128s-main.c:1075-1555` |

**对照出的三条结论**

1. **我们的方案在"网卡寻址"这个维度上是独特且合理的**：正因为描述符只有 bp，才可能做到"RX 的槽位直接当 skb 头再交给硬件发送"，主线没有这个形态（主线的 page_pool 页是驱动分配的，发出去要重新 map）。
2. **我们在"引用计数"这个维度上借错了对象。** `dataref` 是"这一块数据上有几个 skb 引用"，它**不是内存对象的生命周期**；主线从来不用它做"驱动什么时候能回收缓冲"的依据。
3. 现有实现其实已经踩到了主线的两个禁区却用补丁兜住：`skb->cloned=1`（`D/zx279128s-main.c:1430`，副作用是后续 `pskb_expand_head` 会多拷一次，文档 §4 已记）和"池页引用垫高"（`:1119-1132`，见 §5.4 的隐患）。

---

## 4. netmem 专项（Q3）

### 4.1 6.18 的抽象长什么样

```c
/* K/include/net/netmem.h */
struct netmem_desc { ... };                 /* 覆盖在 struct page 上，:32-57 */
typedef unsigned long __bitwise netmem_ref; /* 低位 bit0 标记是不是 net_iov，:66-70 */
enum net_iov_type { NET_IOV_DMABUF, NET_IOV_IOURING, NET_IOV_MAX }; /* :68-75 */
struct net_iov { union{struct netmem_desc desc; ...}; struct net_iov_area *owner; enum net_iov_type type; }; /* :99-119 */
struct net_iov_area { struct net_iov *niovs; size_t num_niovs; unsigned long base_virtual; }; /* :121-128 */
DECLARE_STATIC_KEY_FALSE(page_pool_mem_providers);  /* :61 */
```

关键事实：

- `net_iov` 是**给"没有 `struct page` 的内存"用的**（netmem.h:77-87 的注释原文："generally for non-pages memory that doesn't have its corresponding struct page"，例子只举 dmabuf / io_uring）。
- `net_iov` 的非 pp 引用计数**恒为 1**：`netmem_ref_count()` 对 iov 直接返回 1（`K/include/net/netmem.h:231-240`），生命周期只由 `pp_ref_count` 管。
- `put_netmem()` 对 net_iov 只在 "devmem iov" 时做真实操作，否则**什么都不做**（`K/net/core/skbuff.c:7486-7499`）。也就是说，页池外的 net_iov 一旦离开 page_pool，没有任何回收钩子。

### 4.2 能不能把"驱动私有内存"挂成 netmem / `skb_frag_t` / page_pool 的一员？

分三层回答。

**第一层：`skb_frag_t` 能不能装驱动私有内存？——不能。**
`skb_frag_t` 的三个字段就是 `{netmem_ref netmem; u32 len; u32 offset;}`（`K/include/linux/skbuff.h:361-365`），`netmem_ref` 只能是 `struct page*` 或 `struct net_iov*`（`netmem_is_net_iov()`，`K/include/net/netmem.h:172-175`）。没有"void * 私有指针"这个口子。

**第二层：能不能自己造 `net_iov`？——不能，被类型枚举 + 提供者注册两道门挡住。**

1. `enum net_iov_type` 只有 `NET_IOV_DMABUF` / `NET_IOV_IOURING` 两个值（`K/include/net/netmem.h:68-75`），没有任何"驱动自定义"类型；`net_iov->type` 只被这两个提供者使用。
2. `page_pool` 只在**一个**地方拿到 `mp_ops`：

```c
/* K/net/core/page_pool.c:277-304 */
if (pool->slow.flags & PP_FLAG_ALLOW_UNREADABLE_NETMEM) {
	netdev_assert_locked(pool->slow.netdev);
	rxq = __netif_get_rx_queue(pool->slow.netdev, pool->slow.queue_idx);
	pool->mp_priv = rxq->mp_params.mp_priv;     /* ← 只能来自 rxq */
	pool->mp_ops  = rxq->mp_params.mp_ops;
}
if (pool->mp_ops) {
	if (!pool->dma_map || !pool->dma_sync) { err = -EOPNOTSUPP; goto ...; }
	if (WARN_ON(!is_kernel_rodata((unsigned long)pool->mp_ops))) { err = -EFAULT; ...; }
	err = pool->mp_ops->init(pool);
	...
	static_branch_inc(&page_pool_mem_providers);
}
```

   注意 `struct page_pool_params` 里**根本没有 `mp_ops` 字段**（`K/include/net/page_pool/types.h:78-97` 只有 order/pool_size/nid/dev/napi/dma_dir/max_len/offset/netdev/queue_idx/flags/init_callback/init_arg）。驱动在 `page_pool_create()` 时无法指定自己的内存提供者。
3. `rxq->mp_params` 唯一的写法在 `net_mp_open_rxq()` / `__net_mp_open_rxq()`（`K/net/core/netdev_rx_queue.c:113-176`），而且：
   - 它**没有 EXPORT_SYMBOL**（同文件只导出了 `netif_rxq_has_unreadable_mp` 和 `netdev_rx_queue_restart`，见 `:19`、`:112`）；
   - 它要求 `dev->cfg->hds_config == ETHTOOL_TCP_DATA_SPLIT_ENABLED` 且 `hds_thresh == 0`（`:126-131`）；
   - 调用它的只有两个使用者：`K/net/core/devmem.c:153`（dmabuf）和 `K/io_uring/zcrx.c:925`（io_uring），两者都挂在 netlink 队列配置上（`K/net/core/netdev-genl.c:395-415` 读 `rxq->mp_params`）。
4. `mp_ops->release_netmem()` 就是我们要的那个钩子——"池子里最后一个引用掉了，通知提供者收回内存"：

```c
/* K/net/core/page_pool.c:765-790 */
put = true;
if (static_branch_unlikely(&page_pool_mem_providers) && pool->mp_ops)
	put = pool->mp_ops->release_netmem(pool, netmem);
else
	__page_pool_release_netmem_dma(pool, netmem);
...
if (put) { page_pool_clear_pp_info(netmem); put_page(netmem_to_page(netmem)); }
```

   这一步的语义与"把 bp 还给 BMU"完全同构。**它是本报告最重要的"接口存在但门关着"的证据。**
5. 另外 `netmem_priv.h`（`netmem_set_pp`、`netmem_set_dma_addr`、`netmem_is_pp` 这一套）放在 `K/net/core/netmem_priv.h`，**不在 `include/` 下**，驱动连手动伪造 page_pool 归属都做不到。

**第三层：本镜像连这两个提供者都没编译。**
`CONFIG_PAGE_POOL=y`，但 `CONFIG_DMA_SHARED_BUFFER` 未设（`net/Kconfig:75-79` 里 `NET_DEVMEM` 依赖它）→ `net/core/devmem.o` 不参与构建；`CONFIG_IO_URING` 未设 → `io_uring/zcrx.o` 不参与构建。**结果：`page_pool_mem_providers` 这个 static key 在本机永远不可能被打开，net_iov 这条路在本固件里是死代码。**

### 4.3 如果可行，接口与调用顺序会是什么样（用于判断"差多少"）

假设门是开的，最小调用序列会是：

```c
/* 1. 造 area：一片 net_iov 描述符，每个描述符对应我们的一个槽 */
area->niovs  = kvmalloc_array(n, sizeof(struct net_iov), GFP_KERNEL);
area->num_niovs = n;
for (i = 0; i < n; i++) { niovs[i].owner = area; niovs[i].type = ...; }
/* 2. 给每个 niov 写 DMA 地址 + 绑到 pool */
net_mp_niov_set_dma_addr(&niovs[i], pool_dma + i * 2048);   /* K/net/core/page_pool.c:1351 */
net_mp_niov_set_page_pool(pool, &niovs[i]);                 /* K/net/core/page_pool.c:1359 */
/* 3. 让 pool 用这个提供者（唯一入口） */
net_mp_open_rxq(dev, rxq_idx, &(struct pp_memory_provider_params){
	.mp_priv = my_priv, .mp_ops = &my_ops });            /* 未导出，且要 HDS */
/* 4. 提供者要实现 alloc_netmems/release_netmem/init/destroy/nl_fill/uninstall */
```

限制条件（逐条都有据）：

- `mp_ops` 必须是 `static const`（rodata），否则 `WARN_ON(!is_kernel_rodata())` 直接 `-EFAULT`（`K/net/core/page_pool.c:291-294`）；
- `PP_FLAG_ALLOW_UNREADABLE_NETMEM` 打开后，**该池里所有 netmem 都必须允许"不可读"**，驱动得自己保证不读 payload（`K/include/net/page_pool/types.h:25-32`）；
- `net_iov` 的 `netmem_address()` 返回 `NULL`（`K/include/net/netmem.h:368-374`），`skb_frag_address()` 也返回 NULL（`K/include/linux/skbuff.h:3727-3733`），`__skb_fill_netmem_desc()` 会顺手把 `skb->unreadable = true`（`:2598-2601`）；
- 池必须同时开 `PP_FLAG_DMA_MAP|PP_FLAG_DMA_SYNC_DEV`（`K/net/core/page_pool.c:286-289`）；
- 它只能挂在**真实存在且被驱动使用的 rx queue** 上，而本驱动只有 1 个 NAPI + 8 个自管 RX 环，没有用 `netdev_rx_queue`（`D/zx279128s-main.c:1717-1720`、`:2960`）。

### 4.4 结论

- **net_iov 抽象本身是"为不可读内存准备的"**，它的设计目的（devmem 给 AF_XDP 用、io_uring 零拷贝收包）与"把帧交给本机 TCP 栈"这件事正交。
- **驱动私有内存成为 netmem**：`skb_frag_t` 层不允许；page_pool 层被"提供者只能由 netlink 装 + 只有两个内建提供者"挡住；本镜像这两个提供者都没编译。
- **所以 netmem 不是解题关键**，它只是把"最后一个引用"的钩子做成了一个**只对dmabuf/io_uring 开放**的接口。它证明了这个设计思路（提供者收回内存）是主线认可的正规形态，但本硬件够不到。
- 一条需要写清的边界：**"有条件可行"只存在于"我们自己造一个等价的钩子"这个意义**上（即 §6 路径 ①：在驱动内部实现"最后一个引用 → 还 bp"），而不是"用主线的 netmem 实现"。

---

## 5. 主线如何规避我们的 bug（Q4）

### 5.1 五个函数/标志的真实关系（逐个取证）

| 名字 | 实际语义 | 位置 |
|---|---|---|
| `skb_head_is_locked(skb)` | `!skb->head_frag \|\| skb_cloned(skb)`。**头是 kmalloc 的（head_frag=0）或者被克隆过 → 锁住，不许偷** | `K/include/linux/skbuff.h:5267-5270` |
| `skb_try_coalesce(to, from, &fragstolen, &delta)` | 能把 `from` 并进 `to` 就并；若 `from` 是线性数据，就把 `from->head` 所在**页**当成 `to` 的一个 frag 挂上去，并置 `*fragstolen = true` | `K/net/core/skbuff.c:6139-6233`，偷 head 的三行在 `:6185-6196` |
| `skb_fill_page_desc()` | 只写 frag 描述符，**不额外取引用**（引用是从 `from` 那边转移过来的） | `K/include/linux/skbuff.h:2586-2589`、`:2533-2547` |
| `kfree_skb_partial(skb, head_stolen=true)` | 只跑 `skb_release_head_state()`（会调 destructor）然后释放 sk_buff 壳，**不跑 `skb_release_data()`** | `K/net/core/skbuff.c:6122-6130` |
| `skb_release_data()` / `skb_data_unref()` | 数据引用计数 `dataref` **只在这里**被减 | `K/net/core/skbuff.c:1084-1101`、`K/include/linux/skbuff.h:1298-1314` |
| `pp_recycle` / `skb_mark_for_recycle()` | 告诉 `skb_free_head()`："这块头是 page_pool 的，请还给池，别 `put_page`" | `K/include/linux/skbuff.h:5360-5365`；`K/net/core/skbuff.c:1071-1081`、`:1028-1038` |

### 5.2 主线为什么从不出事——**设计上排除，不是 refcount 兜住**

主线的两条不变量：

1. **"数据的所有权"永远由一个内存对象（page）的引用计数表示**，skb 只是持有者之一。
   - `build_skb(data, frag_size)` 要求 `data` "必须来自 page allocator 或 vmalloc"（`K/net/core/skbuff.c:466-478` 的注释原文）；它把 `head_frag=1`（`:498-509`）。
   - 头被偷进 frag 时，`from` 的那一份 page 引用**原样转移**给 `to` 的 frag，没有第二份凭空产生（`skb_fill_page_desc` 不取引用）。
   - 所以 `kfree_skb_partial(skb, true)` 跳过 `skb_release_data()` **是对的**：那份引用已经不在 `from` 身上了。`skb_release_head_state()` 仍然会跑 destructor/conntrack/skb_ext，**该跑的都跑了**。
2. **驱动要的"什么时候能回收缓冲"，只能挂在内存对象的释放路径上**：`put_page()` → `page_pool_return_netmem()` / `page_frag_free()`。主线**没有**任何驱动用 `dataref` 或 `skb->destructor` 当作"缓冲回收"信号——因为这两者都跟"skb 这个壳"绑定，而不是跟"这块内存"绑定。

**因此答案是：设计上排除。** page_pool 的 refcount **不是**用来"兜住某个漏掉的释放"，它本身就是那个唯一权威；`pp_recycle` 只是把"释放"从 `put_page` 改道到"还池"。换句话说：主线不是"更小心地处理了 head 被偷"，而是**根本不需要小心**——丢了 head 也无所谓，因为计数器不在这里。

### 5.3 我们恰好会走进那条"跳过 `skb_release_data`"的路径（逐步取证）

以本机 TCP 收包（in-order）为例：

1. 驱动 `build_skb(槽位, 2048)` + `skb_reserve(16)`（`D/zx279128s-main.c:1880-1890`）→ `head_frag=1`、`cloned=0`、`dataref=1`（`K/net/core/skbuff.c:380-400` 的 `__finalize_skb_around()` 里 `atomic_set(&shinfo->dataref, 1)`）。
2. `napi_gro_receive()`（`D/zx279128s-main.c:1920`）→ 协议栈 → `tcp_v4_rcv()` → `tcp_data_queue()` → `tcp_queue_rcv()`。
   **关键顺序**：`tcp_queue_rcv()` **先**调 `tcp_try_coalesce()`，**后**调 `skb_set_owner_r()`：

```c
/* K/net/ipv4/tcp_input.c:5313-5333 */
eaten = (tail && tcp_try_coalesce(sk, tail, skb, fragstolen)) ? 1 : 0;
tcp_rcv_nxt_update(...);
if (!eaten) { tcp_add_receive_queue(sk, skb); skb_set_owner_r(skb, sk); }
```

   也就是说：**在栈决定"偷不偷 head"的那一刻，我们的 skb 还没被 orphan，`cloned` 还是 0**。
3. 于是 `skb_head_is_locked(from)` 为 **false**（`head_frag=1`、`cloned=0`）→ 偷 head：

```c
/* K/net/core/skbuff.c:6185-6196 */
if (skb_head_is_locked(from)) return false;
delta = from->truesize - SKB_DATA_ALIGN(sizeof(struct sk_buff));
page   = virt_to_head_page(from->head);
offset = from->data - (unsigned char *)page_address(page);
skb_fill_page_desc(to, to_shinfo->nr_frags, page, offset, skb_headlen(from));
*fragstolen = true;
```
4. `eaten > 0` → `kfree_skb_partial(skb, fragstolen)`（`K/net/ipv4/tcp_input.c:5480`）→ `skb_release_head_state()` → **我们的 destructor 跑了**（`D/zx279128s-main.c:1390-1446`：`atomic_inc(dataref)` 1→2、`skb->cloned=1`、bp 进隔离环、state=SHADOW）→ 然后 sk_buff 壳被释放，**`skb_release_data()` 没跑**。
5. **后果 A（已由代码确定）**：`dataref` 永久停在 2，没有任何人会把它减 1。`zx_zc_reap()` 的第一道判断就是 `if (zx_zc_refs(sp) != 1) break;`（`D/zx279128s-main.c:1319-1321`）——**队头一旦卡住，整个隔离环就再也不会前进**（FIFO），所有后续槽位都堵在后面。
6. **后果 B（已由代码确定）**：环是定长的 4096（`D/zx279128s-eth.h:230`），堵满之后 destructor 走"环满"分支：`atomic_dec(dataref)`（2→1）、`state = LOST`、`zx_zc_leak()`（`D/zx279128s-main.c:1437-1440`）。**这一刻 `dataref` 回到 1，而那个槽位仍然被 `to` 的 frag 引用着。**
7. **后果 C（方向一致，因果未证实）**：此刻的客观事实是——**一块仍在被 `to` 的 frag 引用的槽位，其 `dataref` 已经回到 1**。之后任何"看到 1 就归还 bp"的路径都会把活着的槽位交给硬件，RX DMA 一覆写就串包。但要说清楚：HEAD 现在这三道闸（`inring` 位图闸 `D/zx279128s-main.c:1329-1338`、`state==SHADOW` 闸 `:1339-1347`、交付前 `state==FREE` 闸 `:1867-1879`）**本来会把这种场景降级成 `badstate`/`dup_reap` + 泄漏**，而不是直接双还——这也解释了为什么现场征象只有约 1/27k 包，而不是持续坏包（`DOC/zc-rx-lifetime-2026-10-31.md:168-185`）。所以真正需要抓的是"这两道闸同时没拦住"或"同一个 bp 进了两份环项"的那一次；**具体在哪失效，仍属未证实**（见 §8 #2）。

补充两个次要但同源的坑：

- `skb->cloned = 1` 是在 destructor 里给一个**还活着**的 skb 打的标志（`D/zx279128s-main.c:1430`）。之后若走到 `pskb_expand_head()`（`skb_cow_head()` 会触发），就会多拷一份并改 `dataref`，这条路径已被文档记为"追不到 → 扣住不还"（`D/zx279128s-main.c:1403-1413`；`DOC/zc-rx-lifetime-2026-10-31.md:119-134`）。
- `skb->destructor` 不是"最后一个引用"的信号：`skb_set_owner_r()` 第一件事就是 `skb_orphan()`（`K/include/net/sock.h:2423-2430` → `K/include/linux/skbuff.h:3389-3398`），跑完 destructor 立刻把它换成 `sock_rfree`。文档 §2 已把这条踩坑记录在案（`DOC/zc-rx-lifetime-2026-10-31.md:31-64`）。

### 5.4 顺带发现的一个独立隐患：池页引用是"假账"，账本粒度是 4 KiB 而不是 bp

这一条与 Q4 的问题不同源，但同样属于"把权威计数放错对象"，值得单独标出来：

- 默认配置下池 = arena 首 8 MiB（`D/zx279128s-main.c:2752-2757`、`:2792-2795`），而 arena 是一次 16 MiB 的 `dma_alloc_coherent`。这块内存**不是复合页**（离线已定论，见 §1.1）：`CONFIG_DMA_CMA=y` + 32 MiB 默认区保证走 `cma_allocator`（`K/arch/arm/mm/init.c:199-200`、`K/include/linux/dma-map-ops.h:102-107`、`K/arch/arm/mm/dma-mapping.c:572-578`）→ `:391-420` → `K/kernel/dma/contiguous.c:316-322` → `K/mm/cma.c:933`（`__cma_alloc(..., GFP_KERNEL | __GFP_NOWARN)`，**没有 `__GFP_COMP`**）→ `:842` → `K/mm/page_alloc.c:7122-7126` 因此走 `split_free_pages()` 分支 → `:6925-6949`：整块被切成 4096 个**各自 refcount=1 的独立 order-0 页**。（落到 `simple_allocator` 时是 `alloc_pages()`+`split_page()`，`pool_cacheable=1` 时是 `alloc_pages_exact()` 显式剔除 `__GFP_COMP` —— 三条路形状一致。）
- 所以 `virt_to_head_page(任意槽)` 拿到的是**那一页本身**，`page_frag_free()` 的 `put_page_testzero()` 减的也是这一页（`K/mm/page_frag_cache.c:164-170`）。**而页是 4 KiB、槽是 2048 B —— 一个页里有两个槽。**
- 于是每页那一个计数器同时被"两个 bp 的归还路径"和 `zx_zc_reap()` 的 `put_page(virt_to_head_page(slot))`（`D/zx279128s-main.c:1378`）改写；`zx_zc_park_pages()` 把 2048 个页各自垫到 `1<<20` 以上（`:1120-1132`，按 `PAGE_SIZE` 步进；`D/zx279128s-eth.h:231`），靠 NAPI 每秒 re-park 补足（`:1712-1715`）。
- 这里要改口的是"爆炸半径"：**不会**出现"计数归零 → 整个 16 MiB 还给 buddy"。1 GbE 线速约 8 万帧/秒，每页每秒最多被减十万量级，离 `1<<20` 差一个数量级，re-park 又每 HZ 补一次，所以计数实际上到不了 0。真正的后果是账**永远不可观测**：
  - 该扣的账被 `put_page()` 静默吃掉，错账与对账在计数器上长得一样；本来唯一能把"页账不平"喊出来的地方是 `cma_release()` → `free_contig_range()` 的 `WARN(count != 0, "%lu pages are still in use!")`（`K/mm/page_alloc.c:7248-7271`），而 `zx_pkt_pool_free()` 在释放前先调 `zx_zc_unpark_pages()` 把每页 `set_page_count(page, 1)`（`D/zx279128s-main.c:2678-2680`、`:1155-1156`），**把这个信号也主动抹平了**。
  - 于是风险从"页没了"变成"页看起来都还在，但 bp 已经乱了"：内存子系统永远看不到异常，错账最终只能在 bp 层暴露成双还/漏还（现场 type 9 ×2 与约 1/27k 漏还，见 §5.3）。**这正是 §5.3 那条链能够不惊动内存子系统地跑下去的原因。**
  - 还有一个只在设备拆除时才会露头的窗口：`zx_zc_unpark_pages()` 用 `set_page_count(page, 1)` 一次性抹平（`:1156`），若此刻仍有在途 zc skb，之后它被释放时 `page_frag_free()` 会把那一页真还进 buddy，而 CMA 仍认为自己拥有它 → 拆除时 `cma_release()` → `free_contig_range()` 对 `page_count == 0` 的页再做一次 `__free_page()`（同时在 dmesg 打 "pages are still in use"）。这属于"卸载期崩"而不是"运行期崩"，但同样是账做错的表现。

结论：`ZX_ZC_PARK` 不是"多余的保险"，它是这条设计的**承重墙之一**——它把页级错误压成静默。而它之所以是假账，是因为 `page` 的 refcount 本来表达"这页归 arena"，被借用来表达"这个 buf 还活着"；两者粒度不同（1 页 = 2 buf）、归属不同（arena vs bp），所以垫高之后这张账就只能"不炸"，不能"对"。

---

## 6. 候选路径评分（Q5）

评分维度：改动面（函数/行数）、风险等级、验证方法、失败回退成本、与当前 HEAD 的兼容性。
**下面的顺序不代表推荐顺序。**

### 6.1 ① 保留自研影子机制，改设计

**做什么**（四件事，缺一不可）

1. **解掉隔离环的队头阻塞**：`zx_zc_reap()` 现在遇到 `dataref != 1` 就 `break`（`D/zx279128s-main.c:1319-1321`）。改为"带时间戳的队头：超过 N 个 tick 仍未掉到 1 → 记 `zc_lost`、丢环项、`state = LOST`，继续看后面"。代价是**放弃"归还"**（漏一个缓冲），换来环不死锁。
2. **给 tag 加世代 epoch**：`ZX_ZC_TAG_VAL | bp`（`D/zx279128s-main.c:1070-1091`）只够识别"这个槽位现在的主人是谁"，加 8~16 bit epoch 后可以识别"这一代"。环项、`zc_state`、tag 三处都要带 epoch。
3. **给"head 被偷"建立明确归宿**：destructor 里已经能判断 `skb->head` 是否还在槽位（`:1403-1413`），但对"head 在槽位、可 dataref 永远不降"这种情况目前是**无限等待**。应改为"超时即判为追不到 → LOST + 泄漏"，并单独计数（便于事后确认 §5.3 的链条是否真的发生过）。
4. **把假页计数收进明确的边界**：要么保持 `ZX_ZC_PARK` 但把它写进文档/断言（并加一个"池页 refcount 低于阈值就关 zc"的保护），要么改成"永不 put_page"（需要同时保证栈侧永不走到 `skb_free_head`，即发送交接时不要把 `dataref` 降到 1——这条会牵动 `zx_zc_tx_take()` 的 `:1481-1498`）。

| 维度 | 评估 |
|---|---|
| 改动面 | `zx_zc_reap()`（`:1297-1383`，约 90 行重写）、`zx_zc_destructor()`（`:1390-1446`）、`zx_zc_tx_take()`（`:1455-1508`）、tag 读写（`:1077-1091`）、RX 交付段（`:1850-1904`）、debugfs 计数（`:2367-2386`）≈ **改 150~250 行 / 保留 480 行骨架** |
| 风险 | **中高**。它能消掉"环死锁"和"陈旧归还"两类，但**不能把"追不到"变成"追得到"**：`dataref` 语义与 `kfree_skb_partial(…)` 的结构性错位依然存在，只是被降级成"有界泄漏 + 自动关 zc"。 |
| 验证 | ①`debugfs .../zc`：`rx_ok` 涨、`free_ok` 跟着涨、`out` 回 0、`lost` 有界、`dup/reap/badstate/rxdrop` 为 0；②`zc_ev` 快照里同一个 bp 不允许出现两条 `type 9`；③100 轮 20 MB ssh 上传全 md5 对（`DOC/zc-rx-lifetime-2026-10-31.md:224-229`）；④`canary` 全程 0；⑤12~24 h 转发 + 本机终结混合长稳，盯 `zc_lost` 斜率。 |
| 失败回退 | **低**：`zc_rx=0` 一个开关就回到 copy 路径，源码不用回滚。 |
| 与 HEAD 兼容 | 完全兼容，是在 HEAD 上演进（HEAD 的 3 道闸 + canary + 事件流水可以直接复用）。 |

**条件**：接受"宁可漏缓冲，不可双还"（当前代码注释已经这么写了，`:1281-1295`），并把 `ZX_ZC_LOST_MAX=64`（`:331`）的自动关断当作正常兜底而不是异常。

### 6.2 ② 整体换 page_pool

| 维度 | 评估 |
|---|---|
| 改动面 | 池分配（`:2752-2795`）、硬件几何（`:866-867`、`:895-898`）、RX 全程（`:1706-1958`）、TX 全程（`:1564-1691`）、拆机清理（`:2677-2688`）≈ **600+ 行重写**，而且**改完也不会工作**。 |
| 风险 | **不可行**。page_pool 要求"驱动按包写 DMA 地址"（§1.4），本硬件描述符没有地址字段；net_iov 路线被 §4 挡死且内存不可读。 |
| 验证 | 无法进入验证阶段。可以在 5 分钟内做的事：写一个"读 `ZX_TM_BUF_START` + 反算 bp→物理地址"的 debugfs，确认地址确由硬件线性推导（预期成立，符合 §1.3）。 |
| 失败回退 | 高（架构级改动，回退=整体回滚）。 |
| 与 HEAD 兼容 | 不兼容（推翻 ZC 全部设计）。 |

**条件（如果非要谈）**：只有当 SoC 支持"描述符带物理地址"或"per-buffer 地址表"时才成立；这两者都需要新的硬件文档/逆向，**不在本报告可证范围内**。

### 6.3 ③ 只把数据挂成 frag，head 用普通 skb

**做什么**：RX 侧先用 `napi_alloc_skb()` 造一个**线性头**（够放 L2 头 + 转发时需要的改写空间，例如 `ETH_HLEN + 32`，等价于主线的 header-split 形态：`K/drivers/net/ethernet/intel/idpf/idpf_txrx.c:3434-3466` 的 `idpf_rx_hsplit_wa()`、`K/drivers/net/ethernet/stmicro/stmmac/stmmac_main.c:5686-5696`），再用 `skb_add_rx_frag(skb, 0, virt_to_page(slot), off, len, truesize)` 把槽位里的帧挂成 frag。槽位对 CPU 本来就是可读的（coherent 与 cacheable 两种映射都能读），所以 frag 里的数据栈可以直接读——这正是本路径与 net_iov 路线的根本区别（net_iov 不可读，见 §4.3）。

| 维度 | 评估 |
|---|---|
| 改动面 | 删掉整个 `D/zx279128s-main.c:1075-1555`（约 480 行：tag/环/reap/destructor/park/tx_take）、重写 RX 交付段（`:1843-1904`，约 60 行）、TX 侧放弃零拷贝（`:1605-1611` 删掉，一律走 `:1613-1635` 的 memcpy）、池几何（若要做 1 bp = 1 页，见"前置"）≈ **净减 400 行左右 + 一处硬件实验** |
| 风险 | **中**。正面收益：`skb_head_is_locked()` 对 kmalloc 头恒为真（`K/include/linux/skbuff.h:5267-5270`），§5.3 那条坑**结构上消失**；frag 的引用转移是主线原生行为（`K/include/linux/skbuff.h:2586-2589`）。负面：本驱动没有 `NETIF_F_SG`（`:2880-2881`），frag 化的帧从本网口发出前会被 `__skb_linearize()`（`K/net/core/dev.c:4044`），转发路径白拷一次，**零拷贝收益基本只剩本机终结侧**。 |
| 验证 | ①`/proc/net/softnet_stat` + 每包耗时（`timing=1`，`DOC/ffe-zero-copy-2026-10-30.md:99-110`）看 RX 阶段是否真降；②转发吞吐不得低于 copy 基线（否则说明 linearize 吃掉收益）；③本机终结 20 MB md5 ×100 轮；④`skb_shared_info` 里不许出现 `nr_frags` 越界/`SKBFL_SHARED_FRAG` 异常。 |
| 失败回退 | **中**：删掉的 ZC 块要能恢复（建议先保留在 git 分支上），池几何若动过还要恢复 `ZX_TM_BUF_START/END` 与 `ZX_BMU_LIMIT1`。 |
| 与 HEAD 兼容 | 不兼容（等于放弃 TX 零拷贝与整块自研机制）。 |

**前置（也是本路径唯一的硬条件）**：**1 个 bp 必须等于 1 个 4 KiB 页**，否则页引用计数无法区分同一页里的两个槽（§1.1）。这需要槽长从 2048 变成 4096，而槽长是否可编程**未证实**——唯一可疑的寄存器是 `ZX_TM_TAB_LIMIT = 0x08000800`（`D/zx279128s-main.c:865`，两个 `0x0800` = 2048，语义未知）与 `ZX_BMU_CFG1/CFG2 = 0x0104c040`（`:889-890`，其中 `0x40` = 2048/32）。见 §8 #3。

### 6.4 ④ 放弃零拷贝，回到 copy

| 维度 | 评估 |
|---|---|
| 改动面 | 最小：`zc_rx=0` 即行为（`D/zx279128s-main.c:64-66`、`:1848-1913`）。若彻底删除：`1075-1555`（ZC 块，480 行）+ RX 分支（60 行）+ TX 分支（20 行）+ probe 里的表/park（`:2797-2832`，35 行）+ debugfs（`:2367-2460`）≈ **净减 600 行** |
| 风险 | **低**（唯一被 6/6 实测证明干净的路，`DOC/zc-rx-lifetime-2026-10-31.md:11-17`） |
| 验证 | 现成：md5 ×100 轮 + 吞吐基线（`DOC/ffe-zero-copy-2026-10-30.md:111-123`、`:99-110`） |
| 失败回退 | **零**（只是打开开关） |
| 与 HEAD 兼容 | 完全兼容（HEAD 已经支持运行态切换） |
| 代价 | 每包 RX 17.5 µs、转发 8 流 TCP 385–472 Mbit/s、UDP 606–628 Mbit/s（`DOC/ffe-zero-copy-2026-10-30.md:101-123`）；也就是放弃 +27~85 % 的转发收益。 |

### 6.5 ⑤ 零拷贝 + 驱动内转发快路（不进 `netif_receive_skb`）

**做什么**：把"帧交给内核网络栈"这一步绕开，驱动自己用 `bp` 做转发决策（查 FDB/流表 → 直接下发送描述符）。此时帧的整个生命周期都在驱动手里，`dataref`/`destructor`/frag 全部不参与，§5.3 的结构性错位自然消失。这正是原厂 FFE 的做法（`DOC/ffe-cacheable-pool-2026-10-30.md:21-49`：`fkb` 直接指 DMA 缓冲、自带 per-CPU 队列、不走 `netif_receive_skb`）。

| 维度 | 评估 |
|---|---|
| 改动面 | **最大**：新增转发决策/流表查询/自有发送队列 800~1500 行；还要决定"哪些帧进快路、哪些仍进栈"（ARP/IPv6 ND/本机终结必须进栈）。 |
| 风险 | **高**：功能面最广（bridge、nft、conntrack 全部要旁路或对齐）；与现有 PPE 硬件流表（`D/zx279128s-ppe.h`）的边界要重新划。 |
| 验证 | 需要一整套功能回归矩阵（IPv4/IPv6/组播/分片/ACL/NAT/本机终结），不是几轮 md5 能覆盖的。 |
| 失败回退 | 高。 |
| 与 HEAD 兼容 | 兼容（ZC 块可原样保留给"进栈的帧"用，或整体替换）。 |
| 附带好处 | 顺带解决 §5.4 的假页计数问题（那些帧根本不会碰 skb 头释放路径）。 |

**注意**：这条路的"零拷贝"与①③不是同一个东西——它不依赖任何内核 skb 语义，因此**也解决了本报告的根因**。代价是要自己做一个转发面。

### 6.6 横向对比

| | ①影子改设计 | ②page_pool | ③frag | ④copy | ⑤驱动内快路 |
|---|---|---|---|---|---|
| 能不能真做到"不双还" | 只能"漏不双还" | 理论可以（但做不出来） | 取决于前置（1bp=1页） | 是（不需要） | 是（语义自持） |
| 是否依赖假页计数 | 是 | 否 | 否（若前置成立） | 否 | 否 |
| 转发收益 | 保持现状 | 保持现状 | 大幅缩水（linearize） | 归零 | 可能更高（原厂 918–928 Mbit/s，`DOC/ffe-cacheable-pool-2026-10-30.md:47-48`） |
| 本机终结收益 | 保持现状（-30 %，`DOC/ffe-zero-copy-2026-10-30.md:125-134`） | —— | 可能改善（头在线性内存） | 基线 | 不进栈，无从比较 |
| 需要硬件实验 | 否 | —— | **是**（槽长） | 否 | 否 |
| 能否先做完再决定 | 是 | —— | 是（先做 §8 #3 的实验） | 是（随时可切） | 否（一次性大改） |

---

## 7. 不可行清单（Q6）

| 不能套用的东西 | 为什么 | 证据 |
|---|---|---|
| **page_pool 直接做 RX 零拷贝** | 描述符只有 bp，没有地址字段；BMU 用单窗口 + 固定步长算地址，page_pool 的散页进不去 | `D/zx279128s-main.c:1734`、`:1662-1663`、`:866-867` |
| **把驱动私有池做成 net_iov / memory provider** | `net_iov_type` 只有 dmabuf/io_uring；`page_pool_params` 无 `mp_ops` 字段；唯一入口 `net_mp_open_rxq()` 未导出且要求 HDS；本镜像两个提供者都没编译 | `K/include/net/netmem.h:68-75`；`K/include/net/page_pool/types.h:78-97`；`K/net/core/page_pool.c:277-304`；`K/net/core/netdev_rx_queue.c:113-176`；`K/net/Kconfig:75-79`；`.config`（`CONFIG_IO_URING` 未设） |
| **用 net_iov 承载"要交给本机栈"的帧** | net_iov 是不可读内存：`netmem_address()` 返回 NULL，`skb_frag_address()` 返回 NULL，`skb->unreadable=true` | `K/include/net/netmem.h:368-374`；`K/include/linux/skbuff.h:3727-3733`、`:2598-2601`；`K/include/net/page_pool/types.h:25-32` |
| **XDP 改造** | 驱动没有 `ndo_bpf`/`ndo_xdp_xmit`；XDP 的核心价值（不读字节就丢弃/重定向）与本问题（CPU 要读）方向相反 | `D/zx279128s-main.c:2070-2078`；§2c |
| **多描述符/链式/SG**：让一帧跨多个槽位 | `byte7 bit0` 不是链式位，实测两个描述符发成两帧；一帧写两个描述符时第二个 bp 永不归还；门铃必须一描述符一次 | `DOC/tx-descriptor-probe-2026-10-31.md:29-39`、`:54-58`、`:59-64` |
| **GRO + 零拷贝共存** | 发送侧没有 SG/TSO，聚合体拆段必然拷贝；实测吞吐与丢包都变差 | `DOC/ffe-zero-copy-2026-10-30.md:166-191`；`DOC/tx-descriptor-probe-2026-10-31.md:65-77` |
| **靠 `skb->users` 做生命周期** | 有一整类路径不经过它（`__kfree_skb`、`kfree_skb_partial(…, true)`），且 socket 收包走的就是这条路 | `DOC/zc-rx-lifetime-2026-10-31.md:86-101`；`K/net/core/skbuff.c:6122-6130`、`:1184-1188` |
| **靠 `skb->destructor` 做"最后一个引用"** | `skb_set_owner_r()` 先 `skb_orphan()` 跑掉 destructor，再把 destructor 换成 `sock_rfree`；此后这个 skb 再没有任何我们的回调 | `K/include/net/sock.h:2423-2430`；`K/include/linux/skbuff.h:3389-3398`；`DOC/zc-rx-lifetime-2026-10-31.md:31-64` |
| **`SKBFL_ZEROCOPY_ENABLE` + 自建 `ubuf_info`** | `__pskb_pull_tail()` 在 `!skb->data_len` 时会无条件 `skb_zcopy_clear(skb, false)`，对"数据全在线性头里"的我们就是提前归还；`pskb_expand_head()` 还会对 `ubuf_info` 做 `refcount_inc()` | `DOC/zc-rx-lifetime-2026-10-31.md:138-141`（文档结论，本轮未复验代码，见 §8 #9） |
| **`orphan` 时 `pskb_expand_head()` 把帧搬出槽位** | 能安全还 bp，但 socket 路径白付一次 1.5 KB 拷贝 + 2 KB 分配，零拷贝收益全丢；克隆体的情形仍然追不到 | `D/zx279128s-main.c:1403-1413`；`DOC/zc-rx-lifetime-2026-10-31.md:142-143` |
| **纯页引用计数当权威（不给每槽一页）** | 一个 4 KiB 页里有两个槽，逐槽无法区分 | `D/zx279128s-main.c:2793`（池=连续 arena 首段）+ 池几何 `D/zx279128s-eth.h:220-222`；文档同样结论 `DOC/zc-rx-lifetime-2026-10-31.md:144-145` |
| **把 `tx_queue_len` 调大 / 让 qdisc 深排** | 每个排队的 skb 占一个池缓冲，900 M UDP 灌本机时曾把池抽干导致失联 | `D/zx279128s-main.c:2888-2895`；`DOC/ffe-zero-copy-2026-10-30.md:138-143` |

---

## 8. 未证实清单

| # | 未证实的东西 | 为什么重要 | 怎么证实（离线可做的已标注） |
|---|---|---|---|
| 1 | 垫高之下的**页账到底平不平**（每页 `page_ref_count` 与"应剩值"的偏差分布） | §5.4 说的是"账不可观测"；偏差若持续偏负（少扣/多还）说明还有未知的 release 路径在动这 2048 个页。它不影响"PARK 是承重墙"这个结论，但能提前暴露更糟的情形 | 在 `zx_zc_unpark_pages()` 的 `set_page_count(page,1)` **之前**加一段只读打印：对 2048 个页各打 `page_ref_count()` 与 `1<<20 + 已归还 bp 数 - 已交付 bp 数` 的差，统计分布。**需上机**（只读，但要在 debugfs 里加代码，属改源码 → 需负责人批准） |
| 2 | 现场那个"同一个 bp 被还两次(type 9 ×2)"的**确切**触发点 | 决定 ① 里第 1/2/3 项是否足够 | 已有 `zc_ev` 流水（`D/zx279128s-main.c:1165-1175`、`:2398-2425`）。需要的是"抓到一次完整 8 条事件"而不是限流后的残缺样本；建议把 `net_ratelimit()` 换成"每 bp 只打一次 + 全局 64 条"。**需上机** |
| 3 | **槽长 2048 是否可编程**（能不能变成 4096，使 1 bp = 1 页） | 决定路径 ③ 是否有前置条件可满足；也决定"页计数当权威"这条更干净的路是否存在 | 候选寄存器：`ZX_TM_TAB_LIMIT=0x08000800`（`D/zx279128s-main.c:865`，两个 0x0800 恰为 2048）、`ZX_BMU_CFG1/CFG2=0x0104c040`（`:889-890`，0x40=2048/32）、`ZX_TM_CONFIG_START=16`（`:869`）。**需上机 + 串口**，且必须先停 DMA。离线无法证实 |
| 4 | BMU 空闲表是否**可能**重复发同一个 bp（"硬件/厂商归还协议"层面） | 文档把"下次查厂商归还协议"列为待办（`DOC/zc-rx-lifetime-2026-10-31.md:230-234`）；若有硬件层面的重复项，驱动侧闸门只能"漏"不能"治" | 目前唯一的观测手段是 `rx_dup`/`type 9` 计数（`D/zx279128s-main.c:1867-1879`、`:393-396`）。要区分"驱动双还"和"硬件双发"，需要在归还前给槽位写一个不可混淆的世代戳，再看硬件是否把同一戳的 bp 又发下来 |
| 5 | 32 MiB CMA 区在本机的**实际占用**（16 MiB arena 是否真在里面、还剩多少） | 已离线证实"必然走 CMA 路径"（§1.1 页的形态）；未证实的是余量：CMA 区 32 MiB，仅 arena 就占 16 MiB，`pool_cacheable=1` 那条路又要占 8 MiB（如果它从 CMA 之外的普通内存走则不占）。余量决定"池能不能换成别的分配方式" | 运行时看 `/proc/meminfo` 的 `CmaTotal`/`CmaFree`（应看到 `CmaTotal=32768 kB`、`CmaFree` 比初始少约 16384 kB），以及 `/sys/kernel/debug/cma/*`。**未做** |
| 6 | 描述符 `byte0-5` / `word1` / `word2` 的语义（是否残留地址字段） | 若有地址字段，page_pool 结论要重写 | 驱动只解析 `byte6/7/8` 与 `word3`（`D/zx279128s-main.c:1734-1736`）；其余位从未被读过。要证实只能靠厂商源码或让驱动打印原始描述符做对照实验 |
| 7 | RX 槽位前 16 B 头的内容 | 现在被整体 `skb_reserve(16)` 丢弃（`:1889`）；若里面有硬件元数据（哈希/端口/时间戳），属于被浪费的卸载能力 | 打印 `slot[0..15]` 与已知报文字段对照。**未做** |
| 8 | page_pool 在本硬件上的性能收益 | 无法验证（不可行） | 只能给出"做不到"，不能给"做了会快多少" |
| 9 | `SKBFL_ZEROCOPY_ENABLE` / `__pskb_pull_tail()` 的结论 | §7 里那条引自文档，本轮没有回读 `K/net/core/skbuff.c` 的 `__pskb_pull_tail()` | 读 `K/net/core/skbuff.c` 的 `__pskb_pull_tail()` 即可（离线可做，本轮未做） |
| 10 | `dataref` 的高位（`SKB_DATAREF_SHIFT`）语义 | 代码里只用了低 16 位（`D/zx279128s-main.c:1189-1192`），`nohdr` 情形已被 destructor 直接排除（`:1414-1418`） | 读 `K/include/linux/skbuff.h` 里 `SKB_DATAREF_MASK`/`SHIFT` 定义即可 |

---

## 9. 附录：证据行号索引

### 9.1 驱动（`D/`）

| 行号 | 内容 |
|---|---|
| `D/zx279128s-eth.h:220-222` | 池几何：4096 × 2048 = 8 MiB |
| `D/zx279128s-eth.h:230` | `ZX_ZC_Q_LEN = ZX_NUM_BUFFERS`（4096；文档里写的 512 已过期） |
| `D/zx279128s-eth.h:231` | `ZX_ZC_PARK = 1<<20`（页计数垫高） |
| `D/zx279128s-eth.h:237` | `ZX_ZC_OUT_MAX = 2048` |
| `D/zx279128s-eth.h:260-265` | 16 MiB arena 布局（池占 `[0,0x800000)`） |
| `D/zx279128s-eth.h:318-341` | `buffer_pool` / `pkt_pool` 两套指针与 dma 地址 |
| `D/zx279128s-main.c:64-66` | `zc_rx` 模块参数（默认 on） |
| `D/zx279128s-main.c:324-331` | 状态常量 FREE/SKB/SHADOW/TX/LOST、`ZX_ZC_LOST_MAX=64` |
| `D/zx279128s-main.c:345-378` | `zx_bmu_alloc_bp()`（MMIO 申请，含 pending 处理） |
| `D/zx279128s-main.c:380-415` | `zx_bmu_free_bp()`（归还；含调试插桩与 `bmu_free_slots` 缓存） |
| `D/zx279128s-main.c:849-855` | tab0/tab1 填 0..4095 / 0..511（**空闲 bp 索引，不是地址**） |
| `D/zx279128s-main.c:862-868` | TAB0/TAB1/BUF_START/BUF_END/F0/CONFIG_START 写入 |
| `D/zx279128s-main.c:891-898` | BMU 池大小/门限寄存器 |
| `D/zx279128s-main.c:1070-1091` | tag 编码/解码（`0x5ac00000 \| bp`） |
| `D/zx279128s-main.c:1093-1105` | `zx_zc_in_pool()`：head → bp（整槽寻址） |
| `D/zx279128s-main.c:1119-1132` | `zx_zc_park_pages()`（`virt_to_page` + `page_ref_add(1<<20)`） |
| `D/zx279128s-main.c:1139-1157` | `zx_zc_unpark_pages()`（`set_page_count(page,1)`） |
| `D/zx279128s-main.c:1165-1175` | 每 bp 8 条事件流水 |
| `D/zx279128s-main.c:1181-1186` | `zx_zc_shinfo()`：shinfo 落在槽位 +1728 |
| `D/zx279128s-main.c:1225-1250` | 隔离环 push |
| `D/zx279128s-main.c:1297-1383` | `zx_zc_reap()`：`refs!=1 → break`（1319-1321）、位图闸（1329-1338）、state 闸（1339-1347）、`put_page`+还 bp（1378-1380） |
| `D/zx279128s-main.c:1390-1446` | `zx_zc_destructor()`：`atomic_inc(dataref)`（1424）、`skb->cloned=1`（1430）、环满 → `atomic_dec`+LOST（1437-1440） |
| `D/zx279128s-main.c:1455-1508` | `zx_zc_tx_take()`（发送侧接管；`dataref` 判定 1481-1487） |
| `D/zx279128s-main.c:1518-1555` | `zx_gro_set()` / `zx_zc_set()` |
| `D/zx279128s-main.c:1657-1663` | TX 描述符只写 bp |
| `D/zx279128s-main.c:1734-1736` | RX 描述符只解析 bp/len/port |
| `D/zx279128s-main.c:1739` | `len > 1536` 即判坏帧 |
| `D/zx279128s-main.c:1850-1904` | ZC 交付段（`build_skb`+`reserve(16)`+canary 快照） |
| `D/zx279128s-main.c:1867-1879` | `zc_rx_dup` 闸（state 必须 FREE） |
| `D/zx279128s-main.c:1907-1920` | copy 路径（`napi_alloc_skb` + `memcpy` + `napi_gro_receive`） |
| `D/zx279128s-main.c:2070-2078` | `netdev_ops` 全部 7 个回调（无 `ndo_bpf`/`ndo_xdp_xmit`） |
| `D/zx279128s-main.c:2752-2795` | arena 分配 + 池选择（coherent 首段 / `alloc_pages_exact`） |
| `D/zx279128s-main.c:2797-2832` | ZC 状态表分配 + probe 期 park |
| `D/zx279128s-main.c:2880-2881` | 只开 `NETIF_F_HW_TC`（无 SG/FRAGLIST/TSO） |
| `D/zx279128s-main.c:2367-2386` | `debugfs .../zc` 计数（验证入口） |
| `D/zx279128s-main.c:2398-2425` | `debugfs .../zc_ev` 快照 |
| `D/zx279128s-main.c:2888-2895` | `tx_queue_len = 64` 及其理由 |

### 9.2 内核（`K/`）

| 行号 | 内容 |
|---|---|
| `K/include/linux/skbuff.h:361-365` | `skb_frag_t = {netmem_ref, len, offset}` |
| `K/include/linux/skbuff.h:1298-1314` | `skb_data_unref()`（`!cloned` 直接返回 true；否则按 bias 减 `dataref`） |
| `K/include/linux/skbuff.h:2586-2589` | `skb_fill_page_desc()` 不额外取引用 |
| `K/include/linux/skbuff.h:2598-2601` | net_iov frag → `skb->unreadable = true` |
| `K/include/linux/skbuff.h:3389-3398` | `skb_orphan()`（跑 destructor 后清空） |
| `K/include/linux/skbuff.h:3523-3526` | `skb_free_frag()` → `page_frag_free()` |
| `K/include/linux/skbuff.h:3669-3713` | `skb_frag_is_net_iov/net_iov/page/netmem` |
| `K/include/linux/skbuff.h:3727-3733` | `skb_frag_address()` 对 net_iov 返回 NULL |
| `K/include/linux/skbuff.h:4343-4348` | `skb_needs_linearize()` |
| `K/include/linux/skbuff.h:5267-5270` | `skb_head_is_locked() = !head_frag \|\| cloned` |
| `K/include/linux/skbuff.h:5360-5365` | `skb_mark_for_recycle()` |
| `K/include/linux/skbuff_ref.h:18-21` / `:37-57` / `:66-72` | frag 的 ref/unref → `get_netmem`/`put_netmem` |
| `K/include/net/netmem.h:32-57` | `netmem_desc` 覆盖 `struct page` |
| `K/include/net/netmem.h:61` | `page_pool_mem_providers` static key |
| `K/include/net/netmem.h:68-75` | `enum net_iov_type` 只有 DMABUF/IOURING |
| `K/include/net/netmem.h:77-128` | `struct net_iov` / `net_iov_area` 与其用途说明 |
| `K/include/net/netmem.h:172-175` | `netmem_is_net_iov()`（bit0） |
| `K/include/net/netmem.h:231-240` | `netmem_ref_count()`：net_iov 恒为 1 |
| `K/include/net/netmem.h:368-374` | `netmem_address()`：net_iov 返回 NULL |
| `K/include/net/page_pool/types.h:25-32` | `PP_FLAG_ALLOW_UNREADABLE_NETMEM` 的语义/约束 |
| `K/include/net/page_pool/types.h:78-97` | `struct page_pool_params`（**无 mp_ops**） |
| `K/include/net/page_pool/types.h:161-165`、`:226-227` | `pp_memory_provider_params` / `pool->mp_ops` |
| `K/include/net/page_pool/memory_provider.h:12-24` | `memory_provider_ops`（含 `release_netmem`）+ `net_mp_niov_*` |
| `K/net/core/page_pool.c:277-304` | `mp_ops` 唯一来源 + rodata 校验 + `init()` |
| `K/net/core/page_pool.c:609-690` | `page_pool_alloc_netmems()`（有 mp_ops 走提供者） |
| `K/net/core/page_pool.c:765-790` | `page_pool_return_netmem()`：`release_netmem()` 或 `put_page()` |
| `K/net/core/page_pool.c:1351`、`:1359` | `net_mp_niov_set_dma_addr()` / `set_page_pool()` |
| `K/net/core/netdev_rx_queue.c:113-176` | `__net_mp_open_rxq()` / `net_mp_open_rxq()`（要求 HDS；未导出） |
| `K/net/core/netmem_priv.h:1-63` | `netmem_set_pp/dma_addr/is_pp`（**私有**，驱动不可用） |
| `K/net/core/devmem.c:153`、`:528-535` | dmabuf 提供者 |
| `K/io_uring/zcrx.c:845-940` | io_uring 提供者 |
| `K/net/core/netdev-genl.c:395-415` | netlink 侧读 `rxq->mp_params` |
| `K/net/Kconfig:75-79` | `NET_DEVMEM` 依赖 `DMA_SHARED_BUFFER=y` |
| `K/net/core/skbuff.c:380-400` | `__finalize_skb_around()`：`dataref=1`、shinfo 清零 |
| `K/net/core/skbuff.c:498-509` / `:566-576` | `build_skb()` / `napi_build_skb()`（设 `head_frag=1`） |
| `K/net/core/skbuff.c:812-866` | `napi_alloc_skb()`（page_frag 路径也设 `head_frag=1`） |
| `K/net/core/skbuff.c:870-878` | `skb_add_rx_frag_netmem()` |
| `K/net/core/skbuff.c:1028-1042` / `:1045-1064` | `skb_pp_recycle()` / `skb_pp_frag_ref()` |
| `K/net/core/skbuff.c:1071-1081` | `skb_free_head()`（`head_frag` → `skb_pp_recycle` 或 `skb_free_frag`） |
| `K/net/core/skbuff.c:1084-1101` | `skb_release_data()`（**唯一**减 `dataref` 的地方；frag_list/frag 释放） |
| `K/net/core/skbuff.c:1154-1166` | `skb_release_head_state()`（跑 destructor） |
| `K/net/core/skbuff.c:6122-6130` | `kfree_skb_partial()`（跳过 `skb_release_data`） |
| `K/net/core/skbuff.c:6139-6233` | `skb_try_coalesce()`（偷 head 在 6185-6196） |
| `K/net/core/skbuff.c:7472-7499` | `get_netmem()` / `put_netmem()`（net_iov 非 devmem 时无操作） |
| `K/net/ipv4/tcp_input.c:5313-5333` | `tcp_queue_rcv()`：**先 coalesce 后 `skb_set_owner_r`** |
| `K/net/ipv4/tcp_input.c:5480` | `kfree_skb_partial(skb, fragstolen)`（in-order 合并后） |
| `K/net/ipv4/tcp_input.c:5130` / `:5222` | 同上，ofo/ooo 两条路径 |
| `K/include/net/sock.h:2423-2430` | `skb_set_owner_r()`：`skb_orphan()` + 换 destructor |
| `K/mm/page_frag_cache.c:164-170` | `page_frag_free()`：`virt_to_head_page` + `free_frozen_pages(page, compound_order)` |
| `K/include/net/xdp.h:430-446` | `xdp_convert_buff_to_frame()` |
| `K/net/core/dev.c:4044` | `skb_needs_linearize() → __skb_linearize()` |
| `K/drivers/net/ethernet/marvell/mvneta.c:2634`、`:2410`、`:2516` | page_pool + `build_skb` + recycle + GRO |
| `K/drivers/net/ethernet/stmicro/stmmac/stmmac_main.c:5670`、`:5685`、`:5692`、`:5734` | `napi_build_skb` + recycle + `skb_add_rx_frag` + GRO |
| `K/drivers/net/ethernet/mediatek/mtk_eth_soc.c:2519`、`:2532`、`:2536`、`:2612` | `napi_build_skb` / `napi_alloc_frag` / GRO |
| `K/drivers/net/ethernet/realtek/8139too.c:2042-2058` | 纯 copy 范式 |

### 9.3 文档（`DOC/`）

| 行号 | 内容 |
|---|---|
| `DOC/zc-rx-lifetime-2026-10-31.md:11-17` | zc=1 时 md5 坏、zc=0 干净（A/B 实测） |
| `DOC/zc-rx-lifetime-2026-10-31.md:31-64` | `skb_set_owner_r` 顶掉 destructor 的踩坑 |
| `DOC/zc-rx-lifetime-2026-10-31.md:86-101` | 哪些路径不走 `skb->users` |
| `DOC/zc-rx-lifetime-2026-10-31.md:103-134` | 影子引用方案的代价与"追不到"的处理 |
| `DOC/zc-rx-lifetime-2026-10-31.md:168-185` | 双还征象（`rx_dup`/`dup_push`/`dup_reap`） |
| `DOC/zc-rx-lifetime-2026-10-31.md:219-238` | 现状、待验证判据、长期方向（page_pool / napi_build_skb） |
| `DOC/ffe-zero-copy-2026-10-30.md:10-34` | zc 的收益表 + 硬件前提（4096×2048、TX 无地址字段、非相干池） |
| `DOC/ffe-zero-copy-2026-10-30.md:36-59` | 四种 bp 归属 + 隔离环 + 页计数垫高 |
| `DOC/ffe-zero-copy-2026-10-30.md:61-76` | 三条必须维持的约束（GRO/tx_queue_len/OUT_MAX） |
| `DOC/ffe-zero-copy-2026-10-30.md:99-134` | 分阶段耗时与吞吐/本机终结代价 |
| `DOC/ffe-zero-copy-2026-10-30.md:138-143` | 池被抽干导致失联的事故 |
| `DOC/ffe-zero-copy-2026-10-30.md:166-191` | GRO + zc 的实测结论 |
| `DOC/ffe-cacheable-pool-2026-10-30.md:21-49` | 原厂 FFE 的形态（fkb / 自带队列 / 不进栈） |
| `DOC/ffe-cacheable-pool-2026-10-30.md:81-90` | 池物理地址 0x4e100000 与拷贝速度实测 |
| `DOC/ffe-cacheable-pool-2026-10-30.md:92-109` | 包池改为 cacheable 的改动 |
| `DOC/tx-descriptor-probe-2026-10-31.md:29-39` | `byte7 bit0` 不是链式位（实测） |
| `DOC/tx-descriptor-probe-2026-10-31.md:54-64` | 多描述符不可靠 / 门铃逐个敲 |
| `DOC/tx-descriptor-probe-2026-10-31.md:65-77` | GRO + zc 正式关闭 |
| `DOC/tx-descriptor-probe-2026-10-31.md:71-73` | bp 分配是散的（1024/515/772/2305/3586） |
| `DOC/known-issues.md:1870-1893` | `ZC-RX-DOUBLE-FREE` 条目（与 `zc-rx-lifetime` §6 同源） |

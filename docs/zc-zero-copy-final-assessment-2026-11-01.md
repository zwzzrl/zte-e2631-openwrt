# 零拷贝终版评估（2026-11-01）

> 范围：ZX279128S（ZXHN E2631）OpenWrt 以太网驱动的 RX 零拷贝，以及"能否改用 Linux 主线范式"
> 方法：A 完整评估、B 独立第二意见、C 池页属性复核、D 修法对抗验证、E 报告审计、F 原厂路径对照、G 原厂 vmlinux 反汇编；主 agent 交叉验收
> 状态：**纯离线**，未改源码、未刷机；盒子停在 slot0 原厂，slot1 header 已擦

---

## 一、结论（三句）

1. **主线零拷贝范式（page_pool / netmem / frag 化）在这颗芯片上不能原样引入**——RX/TX 描述符只有 15 bit 缓冲号，地址由 BMU 用「窗口基址 + bp×2048」自己算，硬件没有"按包给地址"的入口。
2. **我们的 bug 是结构性的，不是 API 用错**：把权威计数放在 `skb_shared_info->dataref` 上，而主线把它放在内存对象（page refcount / `pp_ref_count`）上；`kfree_skb_partial(skb, true)` 跳过 `skb_release_data()` 是主线合法路径，我们恰好会走进去。
3. **原厂也没有 IPv6 硬件加速**：它的 900 Mbit/s 是 CPU 逐包的软件快转；NPU `hit:0`、NAT66 不支持。可抄的是"帧不进标准栈"，不是"打开某个加速开关"。

---

## 二、硬件事实（已钉死）

| 事实 | 证据 |
|---|---|
| RX/TX 描述符只含 15 bit `bp`，无物理地址字段 | `main.c:1662-1663`、`:1734` |
| 地址 = `ZX_TM_BUF_START + bp×2048`；只有窗口基址/长度寄存器，无 per-buffer 表 | `main.c:862-869`、`:895-900` |
| 池 = 16 MiB coherent arena 的前 8 MiB（4096 槽 × 2048 B） | `main.c:2752-2757`、`:2792-2795`；`eth.h:220-222` |
| 池页 = **2048 个独立 order-0 页，每页 refcount=1，非 compound** | `kernel/dma/mapping.c:641-642` → `mm/cma.c:930-933` → `mm/page_alloc.c:7122-7123` → `:6925-6949` |
| `dma_alloc_attrs()` 遇 `__GFP_COMP` 直接 WARN+NULL；`alloc_pages_exact` 显式剥掉 COMP | 同上；`mm/page_alloc.c:5487-5488` |

**推论**：page_pool 要求"一页一包 + 页计数真实"，与"一页两槽 + 硬件自管地址"根本对不上；"compound 池"不是风险高，而是 dma 层不接受。

---

## 三、内核 API 事实（为什么 netmem 救不了）

- `skb_frag_t` 在 6.18 已是 `netmem_ref`（`skbuff.h:361-365`），但 net_iov 只能由 devmem/io_uring 创建；本镜像 `CONFIG_NET_DEVMEM`、`CONFIG_IO_URING` 都未开（`net/core/devmem.c`、`.config`）。
- `get_netmem()/put_netmem()` 对非 devmem 的 iov 是空操作（`skbuff.c:7472-7497`）→ 自造 iov 的引用计数全空转。
- `page_pool` 的 `mp_ops` 只能来自 rxq 的 mp_params，且必须 `is_kernel_rodata()`（`page_pool.c:291-294`）→ 模块私有 provider 过不了。
- `kfree_skb_partial(skb, true)` 只跑 `skb_release_head_state` + 释放 skb 壳，**跳过 `skb_release_data()`**（`skbuff.c:6122-6130`）；`dataref` 全树只在 `skb_release_data()` 里减（`:1089`）。

---

## 四、我们 bug 的根因（结构性）

1. **影子引用永久停 2**：`skb_try_coalesce()` 偷 head 后 `fragstolen=true`，走 `kfree_skb_partial` 跳过 `release_data`，destructor 已 `atomic_inc` 的影子引用永远停在 2 → reap 永不触发。
2. **FIFO 被堵死**：`zx_zc_reap()` 遇 `dataref != 1` 就 `break`（`main.c:1320-1321`），后面所有环项再也不回收，`out` 涨到 `ZX_ZC_OUT_MAX` → zc 自动失效。
3. **潜在双 put**：`zx_zc_refs()` 用 `dataref & SKB_DATAREF_MASK`（`main.c:1189-1192`），而 `__skb_header_release()` 写入 `1+(1<<16)`（`skbuff.h:2079-2083`），掩码后仍 ==1 → 可能 reap 一次 + `skb_free_head` 一次。今天不可达（zc 开时 GRO 强关），但必须改整值比较。
4. **假账**：页计数被 `ZX_ZC_PARK=1<<20` 垫高、每秒补一次（`main.c:1119-1132`、`:1712-1715`），因为交付时没有 `get_page`，只有 reap 一次 `put_page`，净 -1。

**可修复的部分**（D 已对抗验证）：交付 `get_page()` + reap `put_page()` → 一次交付恰好一次 put，且不会在 `dataref` 2→1 时触发（`skbuff.c:1084-1090`），**净零成立**，可删 PARK。前提：整值比较、TX 释放路径不变、teardown 的 `set_page_count(page,1)` 保留。

---

## 五、原厂路径（反汇编坐实）

- **FFE 是收包前置过滤器**：`ffe_backlog_poll` 里 pop skb → `bl ffe_receive_skb` → `cmp r0,#0` → **非 0 才**落 `eth_type_trans` + `netif_receive_skb_sk`。摘帧点在 `netif_receive_skb_sk` **之前**。
- 命中后 `ffe_receive_pkt` 直接 `blx [out+0x28]` 发送并返回 0；FFE 区段对 `ip_rcv`/`ip6_forward`/`nf_hook_slow`/`__dev_queue_xmit` 引用数 = 0。
- **不成立的部分**：skb 形态出口 `fbd_xmit_skb→ffe_rps_xmit_skb_default→dev_queue_xmit_sk` **仍过 qdisc**；只有 fbd/fkb 形态走驱动 ops。
- **学习与插表不相连**：`ffe_learn_pkt` 只调 `ffe_copy_flow_key`、不插表；插表在 `*_insert_flow→ffe_general_insert_flow`，这些在核内 0 调用者。"首包进栈学习"未证实。
- **unknow**：真正摘帧那一跳在以太驱动模块（`ffe_receive_fkb_locked` 核内 0 调用点，只有 `__ksymtab`）；模块不在已解密的 rootfs 里。

### 原厂 vs 我们

| 段 | 原厂 | 我们 |
|---|---|---|
| 摘帧点 | `netif_receive_skb_sk` **之前**（FFE 先判） | 必经 `netif_receive_skb → bridge/nft/conntrack` |
| 命中后 | 直接交硬件发送 | 走完协议栈再 `dev_queue_xmit` |
| 发送队列 | FFE 私有 napi + per-CPU 队列，未走 `netif_napi_add` | 五口共享环 + `tx_lock`，每描述符一次门铃 |
| 成本 | 转发 0 拷贝，≈4.7–7.7 µs/包（推算），900M 用 0.4 核 | 0 拷贝，驱动内 5.1+1.7 µs，整栈 13.18 µs/包 |
| IPv6 | NPU `hit:0`，CPU 软件快转；NAT66 不支持 | 纯软件栈 + zc（598–722 Mbit/s） |

---

## 六、"折中论"判定

> 原话："原厂 FFE 是折中：首包进栈学习，已建流驱动内快转，自带队列和计数面。它不是'完全不过内核'，而是用覆盖面换速度。"

| 分句 | 判定 |
|---|---|
| 首包进栈学习 | **不成立/证据不足**：学习与插表在静态代码里不相连 |
| 已建流驱动内快转 | **基本成立**：命中后直接发；但摘帧跳板在模块里，且 skb 形态仍过 qdisc |
| 自带队列和计数面 | **成立** |
| 不是完全不过内核，用覆盖面换速度 | **正确，最准确的一句** |

更准确的表述：**FFE 是收包前置过滤器——先查流表，命中自己发走，未命中把 skb 交给标准栈。**

---

## 七、路径选项

| # | 方案 | 改动 | 风险 | 收益 |
|---|---|---|---|---|
| 1 | 止血 + 真账：队头超时踢出 + 整值比较 + `get_page/put_page` + epoch | 150–250 行 | 中 | 根治双还与假账，保持现有吞吐 |
| 2 | 最小止血：只做队头超时 + 整值比较 | 30–50 行 | 低 | 先长稳，假账保留 |
| 3 | 槽长实验（2048→4096）→ frag 化 | 中 + 硬件实验 | 中高 | 前提未证实；TX 会被 linearize |
| 4 | 放弃零拷贝（`zc_rx=0`） | 0（或删 600 行） | 零 | 放弃 +27~85% 转发收益 |
| 5 | 驱动内转发快路（原厂 FFE 路线） | 800–1500 行 | 高 | 可能最高，但等于自建转发面 |
| — | page_pool / netmem | — | 不可行 | 硬件无入口、net_iov 无 provider |

**已否决**：page_pool、netmem、compound 池、frag 化"1 bp = 1 页"（前提未证实且仍要假账）。

---

## 八、未证实清单

1. 以太驱动模块里 `ffe_receive_fkb_locked` 的调用者（需从固件别处取 `.ko`）。
2. `ffe_backlog` napi 未经 `netif_napi_add` 注册，如何进 `net_rx_action` 的 poll_list。
3. 槽长能否从 2048 改 4096（`ZX_TM_TAB_LIMIT=0x08000800` 高半字语义）。
4. 原厂"首包进栈学习"的时序（静态代码里不相连）。
5. `ffeinfo` 里 `statics: rx pkts:0` 与 `slowpathcnt:546` 并存的原因。
6. 原厂 fkb 的结构体、引用计数与归还时机。

---

## 九、我的判断（建议，不代选）

- **先做选项 2**（30–50 行，把"必现的 FIFO 堵死"和"潜在双 put"修掉）→ 长稳 12–24 h → 再决定是否升级到选项 1。
- **不建议现在做选项 3**：前提未证实，且 TX 侧 linearize 会吃掉大半收益。
- **选项 4 永远保留**：`zc_rx=0` 是零成本安全网。
- **选项 5 是另一条产品线**，不是本 bug 的修复路径；它对应的是"自建转发面"，原厂自己也没用它解决 v6 硬件加速。

---

## 十、证据文件索引

- `docs/zc-mainline-adoption-assessment-2026-11-01.md`（A 报告，633 行，主干可读，细节按 §六勘误）
- `.scratch/zc-mainline/second-opinion.md`（B）
- `.scratch/zc-mainline/c/verify-pool-refcount.md`（C）
- `.scratch/zc-mainline/d/verify-getpage-fix.md`（D）
- `.scratch/zc-mainline/e/audit.md`（E）
- `.scratch/zc-mainline/vendor-path/vendor-vs-ours-path.md`（F）
- `.scratch/zc-mainline/vendor-path/disasm-ffe-callers.md`（G）
- `.scratch/zc-mainline/vendor-path/ffe-compromise-check.md`（折中论核实）
- `.scratch/zc-mainline/ACCEPTANCE.md`（验收档案与亲核事实）

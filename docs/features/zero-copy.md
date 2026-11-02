---
title: 零拷贝终版评估（2026-11-01）
date: 2026-11-01
status: done
topic: accel-impl
summary: 零拷贝上线→本机终结数据损坏→回退；主线 page_pool 范式在这颗芯片上没有入口。
evidence: overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c
supersedes:
---

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

> **口径先行（2026-11-03 补）**：下面这张表原来只有三列，"我们"那一列没说清在讲哪条臂。同一列里
> 混了**源码/反汇编事实**、**已回退的 zc 臂**（交付态 `zc_mask=0`，见本文文件头 summary 与
> [v6-software-fastpath.md](v6-software-fastpath.md) §6）和**本机接收口径**，原厂侧那个 µs 又只是
> **反算区间**。逐行的口径见第 4 列；**µs 类数字只在同一口径内可比**。

| 段 | 原厂 | 我们 | 口径（这条讲的是哪条臂 / 什么证据） |
|---|---|---|---|
| 摘帧点 | `netif_receive_skb_sk` **之前**（FFE 先判） | 必经 `netif_receive_skb → bridge/nft/conntrack` | 两侧都是**代码/反汇编事实**、不是实测吞吐：原厂=本节「五」的静态反汇编（`ffe_backlog_poll`）；我们=我方驱动/协议栈源码路径 |
| 命中后 | 直接交硬件发送 | 走完协议栈再 `dev_queue_xmit` | 同上，代码/反汇编事实。注意原厂"直接交硬件发送"是**驱动 ops**，本节「五」已写明 skb 形态出口仍过 qdisc |
| 发送队列 | FFE 私有 napi + per-CPU 队列，未走 `netif_napi_add` | 五口共享环 + `tx_lock`，每描述符一次门铃 | 原厂=静态反汇编（本节「五」）；我们=**源码事实**：`tx_lock` 见 `zx279128s-accel.c:2984`；一门铃一描述符是**实测**（§2.5：批量提交后退化成三秒一个包）。**「五口共享环」一句在本仓文档里没有出处 ⇒ 口径未注明（待补）** |
| 成本 | 转发 0 拷贝，≈4.7–7.7 µs/包（推算），900M 用 0.4 核 | 0 拷贝，驱动内 5.1+1.7 µs，整栈 13.18 µs/包 | **三个数三种口径，互不可比**：原厂 4.7–7.7 = `sirq%÷kpps` **反算区间**、**转发**口径（[vendor-ipv6-behavior.md](../reverse/vendor-ipv6-behavior.md) §8.4 第 1 条，"0.4 核"同段推算）；我们 5.1+1.7 = **已回退的 zc 臂**的驱动内 RX/TX 计时（[ffe-fastpath.md](ffe-fastpath.md) §6 读法第 2 条）；我们 13.18 = **本机接收**口径（1400 B、300 M UDP 灌包，sirq 增量 ÷ `lan1` 收包数，[ffe-fastpath.md](ffe-fastpath.md) §6），**不含转发与再发送** |
| IPv6 | NPU `hit:0`，CPU 软件快转；NAT66 不支持 | 纯软件栈 + zc（598–722 Mbit/s） | 原厂=上机读 NPU 命中计数（[vendor-ipv6-behavior.md](../reverse/vendor-ipv6-behavior.md) §8.4 第 4 条）+ `CONFIG_CSPKERNEL_NPU_SUPPORT_NAT6 is not set`（同文 `:699`）；我们 598–722 = **已回退的 zc 臂**的 8 流 TCP **转发** A/B（§3 选项 4 的代价段），不是交付态数字 |

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

- 主报告：[2026-11-01-zc-mainline-assessment.md](../history/2026-11-01-zc-mainline-assessment.md)（633 行；主干可读，细节按它的勘误节打折）
- 其余六路取证（B 独立第二意见、C 池页属性复核、D 修法对抗验证、E 报告审计、F 原厂路径对照、
  G 原厂 vmlinux 反汇编）的原始材料未入库，结论已并入本文；引用时以本文与主报告为准。

---

## 附：zc-mainline-adoption-verdict-2026-11-01.md（`zc-mainline-adoption-verdict-2026-11-01.md` 已并入本文件，原文件删除）

## 主线零拷贝能否引入：评估裁定与路径选项

> 日期：2026-11-01
> 范围：ZTE ZX279128S（ZXHN E2631）OpenWrt 以太网驱动的 RX 零拷贝
> 方法：五路取证（A 完整评估、B 独立第二意见、C 池页属性复核、D 修法对抗验证、E 报告审计），主 agent 交叉验收
> 状态：**纯离线**，未改源码、未刷机；盒子仍停在 slot0 原厂，slot1 header 已擦

---

### 0. 裁定（先看这三句）

1. **主线零拷贝 RX 范式（page_pool / netmem / frag 化）在本硬件上不能原样引入。**
   RX/TX 描述符里只有 15 bit 的缓冲号 `bp`，物理地址由 BMU 用「窗口基址 + bp×2048」自己算（`zx279128s-main.c:866-867`、`:1662-1663`、`:1734`），硬件没有 per-buffer 地址入口。这不是"难"，是**没有入口**。
2. **我们的 bug 不是用错 API，而是把权威计数放错了对象。**
   自研机制把"最后一个引用"押在 `skb_shared_info->dataref` 上；主线把权威计数放在内存对象（page refcount / `pp_ref_count`）上。而 `kfree_skb_partial(skb, true)` 会跳过 `skb_release_data()`（`net/core/skbuff.c:6122-6130`），我们恰好会走进它（TCP 收包合并偷 head）。
3. **但主线的"真账"思想可以借：页引用净值可以做成真的。**
   对抗验证确认：RX 交付时 `get_page()`、reap 时 `put_page()`，一次交付恰好一次 put，且不会在 `dataref` 2→1 时触发（`net/core/skbuff.c:1084-1090`）→ **可以删掉 `ZX_ZC_PARK` 的假账**。代价是要同时修两处，见 §3 选项 1。

---

### 1. 四路取证交叉

| 证据源 | 核心结论 | 与其它路的关系 |
|---|---|---|
| A 完整评估（633 行） | 5 条候选路径：①影子改设计 ②page_pool ③frag ④copy ⑤驱动内快路；主干事实扎实 | 成文前读过竞争材料，**独立性不成立**；E 审计出 7 错/4 未证实/4 夸大，引用时以本裁定书为准 |
| B 独立第二意见 | page_pool 不可行、netmem 不可用；①最划算 | 与 A 结论一致；但"池改 `__GFP_COMP`"一条被源码否定（见 §2） |
| C 池页属性复核 | 8 MiB 池 = **2048 个 4 KiB 页，每页 refcount=1，非 compound**；`dma_alloc_attrs()` 遇 `__GFP_COMP` 直接 WARN+NULL（`kernel/dma/mapping.c:641-642`） | 同时否定 A 的"单个复合页"与 B 的"首页 1/尾页 0" |
| D 修法对抗验证 | 净零成立；但 head 被偷后 `dataref` 永久停在 2 → reap 永不触发 → `break`（`main.c:1320-1321`）把环形 FIFO **永久堵死**，后续 bp 全部不回收 | 解释"`out` 涨到 `ZX_ZC_OUT_MAX`、zc 自动失效"的机制 |
| E 报告审计 | 确认 20 / 错误 7 / 未证实 4 / 夸大 4 | 见 §2 |

**A/B 两路在池页属性上都错了，以 C 的源码取证为准。**

---

### 2. A 报告必须勘误的地方（E 审计，按严重度排序）

1. **路径③（frag 化）的"有条件可行"被高估。** `skb_add_rx_frag()` 不取引用（`net/core/skbuff.c:870-879`），就算做到"1 bp = 1 页"，frag 释放仍把页计数打到 0 还给 buddy → **仍躲不开 per-slot 假账**；且它用"kmalloc 头恒锁"论证免疫，却指定用 `napi_alloc_skb()`（`:859` 置 `head_frag=1`，恒不被锁），论据自相矛盾。改动量还漏算槽长→缓冲数的连带修改。
2. **7 处 `文件:行号` 指错行**（`skbuff.h:2533-2547` 实为 `skb_frag_fill_*_desc`、`zcrx.c:925` 实为 `io_pp_uninstall`、release class 实为 `:459-461`、`tcp_queue_rcv` 实为 `:5320-5335` 等），报告"所有行号都是实读结果"的声明不成立。
3. **§5.4 数量级错约 3 个数量级**（每页实际约 40 次/秒，不是"十万量级"），结论不受影响。
4. **"主线没有任何驱动用 dataref/destructor 当回收信号"是全称否定**，证据只覆盖 4 个驱动。
5. **"与 HEAD 完全兼容"过强**：加 epoch 要同步改 tag 编码、环项布局、`zc_ev` 解码与既有脚本。

---

### 3. 路径选项（供你决定，我不代选）

#### 选项 1：止血 + 真账（推荐度：高）
**做什么**：① `zx_zc_reap()` 的 `break` 改成"超时踢出 + `zx_zc_leak()` + 继续看后面"；② `zx_zc_refs()` 改整值比较 `== 1`（防 `1+(1<<16)` 双 put）；③ RX 交付 `get_page()`、reap `put_page()`，删 `zx_zc_park_pages()` 与每秒补引用（保留 teardown 的 `set_page_count(page,1)`）；④ tag 加 epoch。
**改动**：`zx_zc_*` 约 150–250 行（`main.c:1297-1383`、`:1390-1446`、`:1455-1508`、`:1119-1132`、`:1850-1904`、`:2367-2386`）。
**收益**：消掉"双还"与"假账"两类根因；保持现有 zc 吞吐。
**风险**：中。head 被偷时仍只能"有界泄漏"，不能"追得到"——但泄漏会被踢出并计数，不再堵死 FIFO。
**回退**：`zc_rx=0` 运行态零成本。
**验证**：`zc` 计数（`rx_ok/free_ok` 同步涨、`out` 回 0、`lost` 有界、`dup/reap/badstate/rxdrop=0`）+ `zc_ev` 同一 bp 不得出现两条 type 9 + 100 轮 20 MB md5 + 12–24 h 长稳。

#### 选项 2：最小止血（推荐度：中）
**做什么**：只做 ①②（队头超时踢出 + 整值比较），不碰页计数与 PARK。
**改动**：约 30–50 行。
**收益**：先让长稳跑起来，代价最小。
**风险**：低；假账仍在（但现状已能用）。
**回退**：`zc_rx=0`。
**验证**：同选项 1 的判据。

#### 选项 3：先验硬件（槽长 4096）→ 再决定 frag 化
**做什么**：在盒子上验证 `ZX_TM_TAB_LIMIT = 0x08000800` 的高半字是否就是"每槽 2048 B"、能否改成 4096。
**前提**：需要刷机；盒子目前在 slot0，**刷机前必须确认目标为 slot1**。
**收益**：若成功，池变成 2048 个整页，frag 化才谈得上干净。
**风险**：硬件语义未证实；即使成功，本驱动无 `NETIF_F_SG`，frag 帧出网口会被 `__skb_linearize`，转发收益大幅缩水。
**判断**：**不优先**，除非你想保留 frag 化的可能性。

#### 选项 4：放弃零拷贝（推荐度：作为安全网）
**做什么**：`zc_rx=0` 设为默认；彻底删 ZC 代码约 600 行。
**收益**：零风险，行为已被 6/6 实测证明干净。
**代价**：放弃转发热路径收益（8 流 TCP 385–472 → 598–722 Mbit/s；UDP 606–628 → 716–756）。
**回退**：零（只是开关）。

#### 选项 5：驱动内转发快路（原厂 FFE 路线）
**做什么**：绕开 `netif_receive_skb`，驱动自己查表转发，帧生命周期全在驱动手里。
**改动**：新增 800–1500 行 + 功能回归面（bridge/nft/conntrack/本机终结）。
**收益**：可能最高（原厂实测 918–928 Mbit/s），且天然不碰 skb 语义。
**风险**：最高，回退成本最高。

#### 已被否决：② page_pool
不可行。硬件地址模型不允许按包给地址（§0 第 1 句）；netmem 也无法救——`skb_frag_t` 虽是 `netmem_ref`，但 net_iov 只能由 devmem/io_uring 创建，本镜像 `CONFIG_NET_DEVMEM` 与 `CONFIG_IO_URING` 都没开，`mp_ops` 还要求落在 kernel rodata（`net/core/page_pool.c:291-294`）。

---

### 4. 还缺的事实（只有盒子能给）

| 事实 | 影响 | 怎么验 |
|---|---|---|
| 槽长能否从 2048 改 4096 | 决定选项 3 是否成立 | 盒子刷机后改 `ZX_TM_TAB_LIMIT` 试收包 |
| 16 MiB arena 的 CMA 归属 | 已由 `.config` 间接确认（`CONFIG_CMA=y`、`DMA_CMA=y`、32 MiB），`dmesg | grep -i cma` 可定死 | 盒子 |

---

### 5. 我的判断（建议，不是决定）

- **最稳的推进顺序**：先做选项 2（30–50 行，把"必现的 FIFO 堵死"和"潜在双 put"修掉）→ 长稳跑 12–24 h → 若 `lost` 斜率可接受，再升级到选项 1（真账 + epoch）。
- **不建议现在做选项 3**：前提未证实，且即使成功，TX 侧 linearize 会把收益吃掉大半。
- **选项 4 永远保留**：`zc_rx=0` 是零成本安全网，任何阶段都可一键回落。
- **选项 5 是另一条产品线**，不是本 bug 的修复路径；要做应单独立项。

---

### 6. 附：证据文件

- `docs/history/2026-11-01-zc-mainline-assessment.md`（A 报告，主干可读，细节按 §2 勘误）
- B/C/D/E 四路取证的原始材料未入库；结论已并入本裁定与终版评估，引用以这两篇为准（见 `features/zero-copy.md`）。


---

## 附：zc-rx-lifetime-2026-10-31.md（`zc-rx-lifetime-2026-10-31.md` 已并入本文件，原文件删除）

## 零拷贝 RX 的数据损坏（本地终结）2026-10-31

> 对象：ZXHN E2631，slot1 serial=68 的镜像（零拷贝默认开）。
> 结论先说：**零拷贝 RX 在“帧要交给本机 socket”时会损坏数据**，不是偶发网络抖动，
> 是 bp 被提前还回 BMU 的结构性错误。20 MB ssh 上传 10 次坏 2 次，关掉零拷贝 6/6 干净。

### 1. 现象

宿主往盒子推 20 MB 随机数据，盒子侧 `cat > /tmp/y.bin` 后比字节数：

| 配置 | 次数 | 成功 | 失败表现 |
| --- | --- | --- | --- |
| `zc=1` | 3 | 2 | `Broken pipe` / `Connection reset by peer` |
| `zc=1` | 3 | 2 | 同上 |
| `zc=1` | 10 | 8 | 同上 |
| `zc=0` | 3 | 3 | —— |
| `zc=0` | 3 | 3 | —— |

盒子 `logread` 里的失败点只有 dropbear 一句：

```
dropbear[...]: Exit (root) from <<宿主地址>:...>: Error decrypting
```

“Error decrypting” = 收到的一串字节 MAC 校验不过，也就是**明文被改过**。内核没有任何
告警（`rx_bad=0`、`bmu_alloc_fail=0`、`q_ovf=0`），TCP 校验和也早已通过——说明数据是在
校验和算完之后、被进程读走之前被改写的。

### 2. 根因

零拷贝 RX 的 bp 归还依赖 `skb->destructor`：

```c
static void zx_zc_destructor(struct sk_buff *skb)  /* bp 进隔离环，2 tick 后归还 */
```

但协议栈把帧交给 socket 时，`skb_set_owner_r()`（`include/net/sock.h`）是这么写的：

```c
static inline void skb_set_owner_r(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);              /* 触发我们的 destructor：bp 进隔离环 */
	skb->sk = sk;
	skb->destructor = sock_rfree; /* 我们的 destructor 被顶掉 */
	...
}
```

于是：

1. `skb_orphan()` → 我们的 destructor 把 bp 放进隔离环，标记 `DROPQ`，到期时间 `jiffies + 2`；
2. 紧接着 destructor 被换成 `sock_rfree`，**这个 skb 活着的后半生再也没有任何回调**；
3. 2 个 tick 后 `zx_zc_reap()` 看到 `DROPQ` 到期，把 bp 还回 BMU；
4. skb 此时还躺在 socket 接收队列里（大文件传输时队列里能压几百 KB，也就是几十上百个
   skb，停留时间远超 2 tick），硬件把新收到的帧 DMA 进同一个槽位；
5. dropbear 读出来的就是被覆盖过的数据。

代码里的老注释其实已经描述了危险（“本机收包时 skb 会长期留在 socket 队列里”，
`ZX_ZC_OUT_MAX` 闸门就是为它加的），但闸门只限制**同时在外的 bp 数量**，不解决
“skb 还活着但 bp 已经还了”的时序问题。

为什么转发没暴露：转发的 skb 在 `skb_orphan()` 之后很快会被 `zx_zc_tx_take()` 拿走——
发送路径会先把 bp 从隔离环里摘掉再交给硬件，不存在“2 tick 后提前归还”的窗口。
所以只有**本机终结**（ssh / LuCI / DNS / 任何到盒子自己的 TCP、UDP）会坏。

同一类风险还包括任何让 skb 在栈里停留超过 2 tick 的处理：NFQUEUE、`tc` 整形
（netem/tbf）、面板抓包队列等。现在的默认配置里这几样都没开，但都不安全。

### 3. 第一版修法的翻车（skb->users 同样不可靠）

第一版修法（serial=69 的镜像）在隔离环里替槽位持有一份 **skb 引用**：destructor 里
`skb_get(skb)`，`zx_zc_reap()` 里等 `refcount_read(&skb->users) == 1` 才还 bp。损坏确实
消失了，但换来更糟的东西——**内核 panic**：

```
[  716.500211] Internal error: Oops - undefined instruction: 0 [#1] SMP ARM
[  716.592419] PC is at kfree+0x15c/0x25c
[  716.596179] LR is at skb_release_data+0x15c/0x1e4
[  716.600889] Register r2 information: slab skbuff_small_head start c23ad400 pointer offset 0 size 640
[  716.889564] Call trace:
[  716.889575]  kfree from skb_release_data+0x15c/0x1e4
[  716.897085]  skb_release_data from sk_skb_reason_drop+0x3c/0x84
[  716.903015]  sk_skb_reason_drop from zx_eth_poll+0xb7c/0xce4
```

`skbuff_small_head` 的对象在 `skb_release_data()` 里被二次释放。根因：**`skb->users` 不是
数据生命周期的权威计数**，内核有一整类释放路径根本不经过它：

| 路径 | 是否走 `users` | 说明 |
| --- | --- | --- |
| `kfree_skb()` | 走 | `skb_unref()` → `refcount_dec_and_test` |
| `__kfree_skb()` | **不走** | 直接 `skb_release_all()` + `kfree_skbmem()` |
| `sk_eat_skb()` | **不走** | `net/core/sock.c`，就是 `__kfree_skb()` |
| `kfree_skb_partial(skb, true)` | **不走** | 只 `skb_release_head_state()` + 释放结构体 |
| `skb_data_unref()`（数据） | 走 **`dataref`** | 所有释放路径都经 `skb_release_data()` |

socket 收包恰好走第二条：`tcp_recvmsg_locked()`（`net/ipv4/tcp.c:1585`）每读完一个 skb
就 `__kfree_skb(skb)`。于是 `skb_get()` 拿到的那份“引用”被无声忽略，sk_buff 结构体当场
被释放、slab 立刻被别人复用，隔离环里剩下的只是悬空指针；`reap` 之后 `kfree_skb()` 它，
释放的是**别人的** skb，那个 skb 的主人再释放一次就是上面这个 panic。所以它只在
“帧交给本机 socket”时出现，和损坏问题的触发条件完全一样。

### 4. 最终修法：在 `skb_shared_info->dataref` 上拿影子引用

`dataref` 是内核对“这片数据”唯一的权威引用计数，`skb_release_data()` →
`skb_data_unref()` 是所有路径的必经之路（`__kfree_skb` 也走它，只是不碰 `users`）。
做法等价于克隆一份数据引用，但不分配 `sk_buff`：

- destructor 里（`skb->head == 槽位` 且 `skb->nohdr == 0`）：
  `atomic_inc(&shinfo->dataref)` + `skb->cloned = 1`，把 **bp** 挂进隔离环，
  状态 `SKB → SHADOW`。这两个动作和 `skb_clone()` 内部做的完全一样，只是没有结构体。
- `zx_zc_reap()`：`dataref & SKB_DATAREF_MASK == 1` 说明只剩我们这份（协议栈、克隆体
  都放完了）→ 替 head 还掉那次 `page_frag_free()` 的页引用（`put_page`），再把 bp 还回
  BMU。环里只存 bp，不存 skb 指针，悬空指针这条路彻底消失。
- `zx_zc_tx_take()`：`dataref == 1(+我们那份)` 才允许把槽位交给硬件，否则退回拷贝；
  交给硬件前摘掉环项、`atomic_dec(dataref)`、清掉 tag 和 destructor（bp 归硬件）。
- 陈旧 tag：`zc_skb[bp]` 记着槽位当前的主人，destructor 里指针对不上就不动手。

**代价（实测待补）**：影子引用要 `skb->cloned = 1`，于是这个 skb 再走
`skb_cow_head()` 时 `skb_header_cloned()` 为真、会 `pskb_expand_head()` 拷贝一份。
这就是为什么它只能套在“已经不会再被改写”的 skb 上：
  * 本机终结（socket 队列）：只读，没有 cow；
  * 桥/flowtable 转发：实测**根本不 orphan**——716 s 内 `rx_ok=49 tx_ok=45 free_ok=0`，
    帧在 destructor 之前就被 `zx_zc_tx_take()` 拿走了，所以这条路上没有影子引用、也就
    没有 cloned 位。
反过来说，如果哪天转发路径开始 orphan（换 nft 配置、加 tc 动作），这些帧会退化成
“一次 expand 拷贝 + 一次发送拷贝”，比关掉 zc 更差。观察指标：`zc` 里 `fallback` 和
`lost` 同时涨、`out` 居高不下，就说明这条假设破了。

**唯一追不到的情况**：`pskb_expand_head()` 把数据搬出槽位后，`skb->head != 槽位`，
而槽位有没有被别的克隆体引用从这个 skb 上看不出来（克隆过的 expand 会把旧 head 留给
克隆体）。这类槽位直接扣着不还，记 `zc_lost`；累计到 `ZX_ZC_LOST_MAX`(64) 自动把
`zc_rx` 关掉并在串口告警。宁可少 64 个缓冲，也不还一个可能还在被读的缓冲。
正常转发路径不做 expand（16 B headroom 够 L2 重写），本机终结路径也不会——实测待补。

被评估过但没选的路子：

- `SKBFL_ZEROCOPY_ENABLE` + 自建 `ubuf_info`，借 `skb_release_data()` 的
  `skb_zcopy_clear()` 拿“最后一个引用”回调：`__pskb_pull_tail()` 在 `!skb->data_len`
  时会无条件 `skb_zcopy_clear(skb, false)`，对“数据全在线性头里”的我们就是提前归还；
  另外 `pskb_expand_head()` 会对 `ubuf_info` 做 `refcount_inc()`。
- orphan 时 `pskb_expand_head(skb,0,0)` 把帧搬出槽位、立刻还 bp：socket 路径白付一次
  1.5 KB 拷贝 + 2 KB 分配，零拷贝收益全丢；克隆体的情形同样还不了。
- 依赖 `page` 引用计数（page_pool 那套）：池子是一片连续 DMA 内存，一个 4 KB 页里有两个
  槽位，逐槽位的页引用没法区分。

### 5. 复现脚本

```bash
## 宿主：socket 路径（会损坏 / 会 panic）
head -c 20000000 /dev/urandom > /tmp/fw20.bin
for i in $(seq 1 10); do
  cat /tmp/fw20.bin | ssh root@192.168.1.1 'cat > /tmp/y.bin; wc -c < /tmp/y.bin'
done
## 盒子（对比组）
echo 0 > /sys/kernel/debug/92000000.ethernet/zc
```

判定：输出不是 `20000000` 就是坏；`dmesg | tail` 里出现 `skbuff_small_head` / `kfree`
的 Oops 就是 §3 那条二次释放。修好后的判据（`debugfs .../zc`）：

```
rx_ok 涨、free_ok 跟着涨、out 回到 0、lost=0、stale=0、q_n=0
```

`lost` 非 0 说明有帧被 expand 搬走了，需要再想别的办法追。

### 6. 影子引用之后还剩的问题：同一个 bp 被还两次（2026-10-31 夜）

§4 的修法把 skbuff 二次释放（§3 的 panic）消掉了，但**数据损坏仍在**：宿主连续
20 MB ssh 上传，第 2 轮就 md5 不对。把 `zc_canary`（交付快照 + reap 前比对）打开后
抓到的征象是同一个 bp 被 `zx_zc_reap()` 归还了两次：

- 第一次还完，硬件立刻把这个 bp 又发了下来（RX DMA 覆写槽位 → canary 记 `corrupt`），
  于是两个 skb 指向同一个槽位，内容互相覆盖 → 上传的字节坏掉；
- 计数三元组同时出现：`rx_dup`（交付时 state 不是 FREE）、`dup_push`（同一个 bp 在
  隔离环里出现第二份）、`dup_reap`（摘环项时位图已经被清过）。

27k 包一次的量级：`rx_dup=1 dup_push=1 dup_reap=1`，`corrupt` 被 ratelimit 吞掉。
用**旧代码**（环项只在真正归还时清位图）能稳定跑 26k 包不 panic，说明"三件套"是罕见
事件；它一旦发生就连续坏包，因为重复项会连环把活着的槽位还回去。

注意 `badfree` 抓不到这种双还：`reap` 是**先把 `zc_state[bp] = FREE` 再调
`zx_bmu_free_bp()`**，等 `zx_bmu_free_bp()` 里的检查跑起来时状态已经变成 FREE 了。
真正能看见双还的是事件流水（见下）里的两次 `type 9`。

#### 6.1 诊断插桩（本轮加的，正式版要能一键关掉）

| 东西 | 位置 | 说明 |
|---|---|---|
| `zc_canary`（module_param，默认 1） | `zx279128s-main.c` | 交付时快照槽位前 64 B，reap 归还前比对，被写过就 `corrupt++` |
| 事件流水 `zc_ev[bp][8]` | 同上 | `type<<24 \| aux<<16 \| (jiffies & 0xffff)`。type：1=RX交付(aux=len>>6) 2=影子入环 3=reap归还(aux=dataref) 4=发送申请(aux=state) 5=zc发送 6=陈旧tag 7=追不到 **9=还bp给BMU(aux=state\|0x10 在环里)** |
| debugfs `zc_ev` | `92000000.ethernet/zc_ev` | 把所有"不在 BMU 空闲表里 / 还在环上"的 bp 连同 8 条事件打出来，一次读就是一张快照，不用重启 |
| 异常点告警 | 同上 | `rx_dup`/`dup_push`/`dup_reap`/`badstate`/canary 命中时 `zx_zc_ev_dump()` 把 state、dataref、tag、`zc_skb[bp]`、destructor、`q_n`、位图、8 条事件一起打进 dmesg（`net_ratelimit()` 限流，会吞掉一部分） |

读事件流水要注意：8 条是**环形**写入（`zc_ev_n[bp]++ & 7`），所以 `n>8` 时打印出来的
顺序是被旋转过的，最早的一条在 `n & 7` 位置上。

#### 6.2 两道闸：一个 bp 只能被还一次

现在的规则（顺序本身就是不变量，写错就出事）：

1. `reap` 先看 `dataref`：`!= 1` 就 `break`，**原位不动** —— 不清位图、不摘环项。
2. 真要归还时：位图必须还是 set（否则是同一 bp 的第二份环项 → 丢项不还）；
   并且 `zc_state[bp]` 必须是 `SHADOW`（否则槽位已经被别人接管/归还 → 丢项不还）。
3. "丢项"等于把一个缓冲留在 BMU 外面：记 `zc_lost`（`zx_zc_leak()` 统一记账），
   累计到 `ZX_ZC_LOST_MAX`(64) 自动 `zc_rx=false` 并告警 —— 宁漏不双还。
4. RX 交付前 `zc_state[bp]` 必须是 `FREE`，否则整帧丢弃、**也不还 bp**
   （`rx_dup` + `rxdrop`）：BMU 空闲表里出现了重复项，这一帧和别的 skb 共用槽位，
   交付出去就是串包；把 bp 还回去又会把别人正在读的槽位交给硬件。

**踩过的坑（顺序写反的代价）**：第一版把位图测试放在 `dataref` 检查**之前**。队头那条
常常因为 skb 还在 socket 队列里而 `dataref != 1`，这时应该原地 `break`，但代码先清了
位图再 break —— 下一次进来就把自己这半截动作判成"重复环项"，一次 20 MB 上传（0.48 s、
1743 个包）立刻爆出 `dup_reap=67`、`badstate` 之外的 `out=68`（泄漏 68 个缓冲），
`corrupt=9`、`rx_dup=3`。改回"先 dataref、后位图"之后误判消失。**这说明位图和状态闸
只能在"确定要归还"的那一刻用，不能当探针。**

#### 6.3 现状与下一步（交接）

- 已构建未验证：当轮打包出的镜像（02:54，`make world` rc=0，未入库），
  含 §6.2 的正确顺序 + type 9 归还流水。盒子当前跑的是**顺序写反的那一版**
  （所以它现在会误判、会泄漏），要么刷 02:54 那版，要么先把 `zc` 关掉再离开。
- 待验证判据（`debugfs .../zc`，跑 100 轮 20 MB 上传 = 2 GB）：

  ```
  rx_ok 涨、free_ok 跟着涨、md5 全对、out 回 0
  corrupt=rx_dup=tx_dup=badfree=badstate=rxdrop=dup_push=dup_reap=lost=q_ovf=0
  ```
- 若 `rx_dup`/`rxdrop` 还会涨：那就是 BMU 空闲表真的会重复发同一个 bp。下一步查
  **厂商的归还协议**（`ZX_BMU_BUF_FREE` 的写时序、`ZX_BMU_FREE_STATUS` 里
  `bmu_free_slots` 缓存是否可信、reap 一次连还 256 个是不是太快），再考虑给 bp 加
  世代 epoch（tag 里带 epoch，环项存 epoch，三处都要求匹配）。epoch 只能把"双还"
  降级成"泄漏一次"，治不了根，但能让损坏彻底消失。
- 长期：按主线的 page_pool 思路重做，把"页引用计数"恢复成唯一真相（现在
  `ZX_ZC_PARK = 1<<20` 把页计数垫成了假账，所以只能自己维护 dataref 影子）；
  顺带评估收包侧换 `napi_build_skb()`（每 CPU 批量 sk_buff 缓存，省一次
  `skbuff_head_cache` 分配，现在 `napi_gro_receive` 单项 2.94 µs 是最大开销）。


---

## 附：tx-descriptor-probe-2026-10-31.md（`tx-descriptor-probe-2026-10-31.md` 已并入本文件，原文件删除）

## TX 描述符能力实测（链式 / 超长帧）2026-10-31

> 目的：`docs/history/2026-10-30-zero-copy.md` §6.6 把“GRO + 零拷贝”判死，理由是合并出的
> 聚合体在发送侧必须拆段，而这份驱动的发送描述符“一个 bp 只能放一整帧、没有 SG/TSO”。
> 这个结论一直是**从代码结构推出来的**，没被硬件验证过。本轮用实验入口直接构造描述符，
> 把三件事一次问清：单描述符能不能带超长帧、能不能跨槽位读、`byte7 bit0` 是不是链式位。

### 1. 实验入口

驱动里加了 debugfs 文件 `txtest`（root-only，`0644` 里写口 `0200`）：

```
echo "<len> <which> <off2> <seglen> [dev]" \
	> /sys/kernel/debug/92000000.ethernet/txtest
```

- `len`：整帧字节数；`which`：0 = 单描述符，bit0/bit1 = 给第 1/第 2 个描述符的
  `byte7 bit0` 置位；`off2`：链式时第二段从槽位内哪个偏移读；`seglen`：1 = 两个描述符
  都写整帧总长；`dev`：只在这个口发（如 `lan1`）。
- 帧内容：广播目的 + 本口 MAC + ethertype `0x88b5`，从帧偏移 14 起是小端 32 位序号
  （`0x5a5a0000 | k`），十六进制一眼能看出内容连不连续。
- 每次调用还会打印目标口 MAC 的 `tx_octets`/`tx_frames`（硬件 MIB，绝对值），
  用来区分“MAC 拒发”和“对端没收到”。

宿主侧没有 tcpdump，用 `AF_PACKET` 自己抓。

### 2. 结论

#### 2.1 `byte7 bit0` 不是链式位

两个描述符各 600 字节、第一个描述符置位（`1200 1 16 0`），端口 MIB 的增量是
**+1208 字节 / +2 帧**（604×2），也就是硬件把两个描述符当成**两个独立帧**发了；
宿主收到的是一帧 600 字节、内容正好是第一段（序号 0..149）。
`which=2`（只置第二个）、`which=3`（两个都置）、`off2=0`、`seglen=1` 都是同样的
“一描述符一帧”，没有任何拼帧迹象。

`seglen=1` 那次更说明问题：第一段只有 400 字节内容、描述符却写 800，收到的帧长度是
800、后 400 字节是槽位里的旧内容 —— 硬件完全按描述符的长度字段读，不跨描述符。

#### 2.2 单描述符的长度字段硬件接受，但实际能发多大受槽位限制

`len=600/800/1200/1514` 都完整到达宿主，内容逐字节正确（1514 那帧 375 个序号一个不差）。
`len=2032`（槽位内上限）在 MIB 上计入已发送（+2036 字节 = 2032+4），但宿主收不到，
原因见 2.3，不是盒子的错。

#### 2.3 观测通道的天花板是 1514 字节

宿主 `<宿主网卡>` 的 MTU 设成 9000 后，盒子发 1514 能到、1516 起一帧都到不了；
反方向宿主发 1600 字节帧，盒子的 MAC **收到了**（`rx_bad=1`，描述符里 len=1600）。
即上行能过巨帧、下行到这台虚拟机不能 —— 宿主是 VMware 虚拟机，物理网卡/虚拟交换机
的入向被 1500 MTU 挡着。所以在当前实验台上，“盒子能不能发巨帧”只能靠 MIB 侧证，
没法端到端验证。

#### 2.4 多描述符发送不可靠（真坑）

一次调用里写两个描述符（各自一个 bp、各自敲一次门铃）：MIB 记了两帧，宿主只到第一帧，
第二个 bp 永远不回（`tx_pending` 卡 1）。**不要在一帧里写多个描述符。**

#### 2.5 门铃必须一个描述符一次

第一轮想“批量提交”：4 个描述符只写一次 `TX_START`。硬件只处理了第一个，另外 3 个
永远卡在 ring 里，随后整条发送路径退化成**三秒一个包**（`ping` 稳定 3000 ms、丢 40%），
复现两次，每次都只能把 TX 描述符环重新初始化（`TX_START` 需逐描述符写、环要重建）才恢复；
**重启/复位的对照实验没做**，所以「只有重启能恢复」是未实测的推断，不要当结论引。

### 3. 对 GRO 的意义

GRO 合并出来的聚合体一般是 8×1448 ≈ 11.5 KB。现在有三条硬限制同时成立：

1. 一个描述符最多带一个槽位（2032 字节，含 16 字节头留空）；
2. 多描述符不拼帧、而且第二个描述符根本发不出去；
3. 想要“单描述符跨槽位读”得先拿到物理相邻的 bp，BMU 分配出来的 bp 是散的
   （实测 1024/515/772/2305/3586…），跨槽实验在 2048/4000/8000 三档全部因
   “槽位不相邻”跳过。

⇒ **GRO + 零拷贝这条路正式关闭**，理由从“拆段拷贝太贵”升级成“硬件根本不支持把
一帧拆到多个描述符”。§6.6 的实测数字（每包 15.2 → 28.7 µs）继续有效，不必再回头。

### 4. 顺带修正的记录

- **宿主接在 lan1**，不是 lan2。`brctl showmacs` 里宿主 MAC 的端口号 1 对应 lan2
  （按端口添加顺序），不能拿来跟驱动里的端口索引对应。可靠的判法：往某个口发一帧，
  看宿主 `/sys/class/net/<宿主网卡>/statistics/rx_packets` 是否 +1。
- `txtest` 的链式分支已经在实机上证明没有价值，留着只为复现实验；正常发送路径
  不受影响（它永远只写一个描述符）。

### 5. 工具

| 工具 | 用途 |
| --- | --- |


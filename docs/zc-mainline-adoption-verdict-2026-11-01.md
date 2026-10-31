# 主线零拷贝能否引入：评估裁定与路径选项

> 日期：2026-11-01
> 范围：ZTE ZX279128S（ZXHN E2631）OpenWrt 以太网驱动的 RX 零拷贝
> 方法：五路取证（A 完整评估、B 独立第二意见、C 池页属性复核、D 修法对抗验证、E 报告审计），主 agent 交叉验收
> 状态：**纯离线**，未改源码、未刷机；盒子仍停在 slot0 原厂，slot1 header 已擦

---

## 0. 裁定（先看这三句）

1. **主线零拷贝 RX 范式（page_pool / netmem / frag 化）在本硬件上不能原样引入。**
   RX/TX 描述符里只有 15 bit 的缓冲号 `bp`，物理地址由 BMU 用「窗口基址 + bp×2048」自己算（`zx279128s-main.c:866-867`、`:1662-1663`、`:1734`），硬件没有 per-buffer 地址入口。这不是"难"，是**没有入口**。
2. **我们的 bug 不是用错 API，而是把权威计数放错了对象。**
   自研机制把"最后一个引用"押在 `skb_shared_info->dataref` 上；主线把权威计数放在内存对象（page refcount / `pp_ref_count`）上。而 `kfree_skb_partial(skb, true)` 会跳过 `skb_release_data()`（`net/core/skbuff.c:6122-6130`），我们恰好会走进它（TCP 收包合并偷 head）。
3. **但主线的"真账"思想可以借：页引用净值可以做成真的。**
   对抗验证确认：RX 交付时 `get_page()`、reap 时 `put_page()`，一次交付恰好一次 put，且不会在 `dataref` 2→1 时触发（`net/core/skbuff.c:1084-1090`）→ **可以删掉 `ZX_ZC_PARK` 的假账**。代价是要同时修两处，见 §3 选项 1。

---

## 1. 四路取证交叉

| 证据源 | 核心结论 | 与其它路的关系 |
|---|---|---|
| A 完整评估（633 行） | 5 条候选路径：①影子改设计 ②page_pool ③frag ④copy ⑤驱动内快路；主干事实扎实 | 成文前读过竞争材料，**独立性不成立**；E 审计出 7 错/4 未证实/4 夸大，引用时以本裁定书为准 |
| B 独立第二意见 | page_pool 不可行、netmem 不可用；①最划算 | 与 A 结论一致；但"池改 `__GFP_COMP`"一条被源码否定（见 §2） |
| C 池页属性复核 | 8 MiB 池 = **2048 个 4 KiB 页，每页 refcount=1，非 compound**；`dma_alloc_attrs()` 遇 `__GFP_COMP` 直接 WARN+NULL（`kernel/dma/mapping.c:641-642`） | 同时否定 A 的"单个复合页"与 B 的"首页 1/尾页 0" |
| D 修法对抗验证 | 净零成立；但 head 被偷后 `dataref` 永久停在 2 → reap 永不触发 → `break`（`main.c:1320-1321`）把环形 FIFO **永久堵死**，后续 bp 全部不回收 | 解释"`out` 涨到 `ZX_ZC_OUT_MAX`、zc 自动失效"的机制 |
| E 报告审计 | 确认 20 / 错误 7 / 未证实 4 / 夸大 4 | 见 §2 |

**A/B 两路在池页属性上都错了，以 C 的源码取证为准。**

---

## 2. A 报告必须勘误的地方（E 审计，按严重度排序）

1. **路径③（frag 化）的"有条件可行"被高估。** `skb_add_rx_frag()` 不取引用（`net/core/skbuff.c:870-879`），就算做到"1 bp = 1 页"，frag 释放仍把页计数打到 0 还给 buddy → **仍躲不开 per-slot 假账**；且它用"kmalloc 头恒锁"论证免疫，却指定用 `napi_alloc_skb()`（`:859` 置 `head_frag=1`，恒不被锁），论据自相矛盾。改动量还漏算槽长→缓冲数的连带修改。
2. **7 处 `文件:行号` 指错行**（`skbuff.h:2533-2547` 实为 `skb_frag_fill_*_desc`、`zcrx.c:925` 实为 `io_pp_uninstall`、release class 实为 `:459-461`、`tcp_queue_rcv` 实为 `:5320-5335` 等），报告"所有行号都是实读结果"的声明不成立。
3. **§5.4 数量级错约 3 个数量级**（每页实际约 40 次/秒，不是"十万量级"），结论不受影响。
4. **"主线没有任何驱动用 dataref/destructor 当回收信号"是全称否定**，证据只覆盖 4 个驱动。
5. **"与 HEAD 完全兼容"过强**：加 epoch 要同步改 tag 编码、环项布局、`zc_ev` 解码与既有脚本。

---

## 3. 路径选项（供你决定，我不代选）

### 选项 1：止血 + 真账（推荐度：高）
**做什么**：① `zx_zc_reap()` 的 `break` 改成"超时踢出 + `zx_zc_leak()` + 继续看后面"；② `zx_zc_refs()` 改整值比较 `== 1`（防 `1+(1<<16)` 双 put）；③ RX 交付 `get_page()`、reap `put_page()`，删 `zx_zc_park_pages()` 与每秒补引用（保留 teardown 的 `set_page_count(page,1)`）；④ tag 加 epoch。
**改动**：`zx_zc_*` 约 150–250 行（`main.c:1297-1383`、`:1390-1446`、`:1455-1508`、`:1119-1132`、`:1850-1904`、`:2367-2386`）。
**收益**：消掉"双还"与"假账"两类根因；保持现有 zc 吞吐。
**风险**：中。head 被偷时仍只能"有界泄漏"，不能"追得到"——但泄漏会被踢出并计数，不再堵死 FIFO。
**回退**：`zc_rx=0` 运行态零成本。
**验证**：`zc` 计数（`rx_ok/free_ok` 同步涨、`out` 回 0、`lost` 有界、`dup/reap/badstate/rxdrop=0`）+ `zc_ev` 同一 bp 不得出现两条 type 9 + 100 轮 20 MB md5 + 12–24 h 长稳。

### 选项 2：最小止血（推荐度：中）
**做什么**：只做 ①②（队头超时踢出 + 整值比较），不碰页计数与 PARK。
**改动**：约 30–50 行。
**收益**：先让长稳跑起来，代价最小。
**风险**：低；假账仍在（但现状已能用）。
**回退**：`zc_rx=0`。
**验证**：同选项 1 的判据。

### 选项 3：先验硬件（槽长 4096）→ 再决定 frag 化
**做什么**：在盒子上验证 `ZX_TM_TAB_LIMIT = 0x08000800` 的高半字是否就是"每槽 2048 B"、能否改成 4096。
**前提**：需要刷机；盒子目前在 slot0，**刷机前必须确认目标为 slot1**。
**收益**：若成功，池变成 2048 个整页，frag 化才谈得上干净。
**风险**：硬件语义未证实；即使成功，本驱动无 `NETIF_F_SG`，frag 帧出网口会被 `__skb_linearize`，转发收益大幅缩水。
**判断**：**不优先**，除非你想保留 frag 化的可能性。

### 选项 4：放弃零拷贝（推荐度：作为安全网）
**做什么**：`zc_rx=0` 设为默认；彻底删 ZC 代码约 600 行。
**收益**：零风险，行为已被 6/6 实测证明干净。
**代价**：放弃转发热路径收益（8 流 TCP 385–472 → 598–722 Mbit/s；UDP 606–628 → 716–756）。
**回退**：零（只是开关）。

### 选项 5：驱动内转发快路（原厂 FFE 路线）
**做什么**：绕开 `netif_receive_skb`，驱动自己查表转发，帧生命周期全在驱动手里。
**改动**：新增 800–1500 行 + 功能回归面（bridge/nft/conntrack/本机终结）。
**收益**：可能最高（原厂实测 918–928 Mbit/s），且天然不碰 skb 语义。
**风险**：最高，回退成本最高。

### 已被否决：② page_pool
不可行。硬件地址模型不允许按包给地址（§0 第 1 句）；netmem 也无法救——`skb_frag_t` 虽是 `netmem_ref`，但 net_iov 只能由 devmem/io_uring 创建，本镜像 `CONFIG_NET_DEVMEM` 与 `CONFIG_IO_URING` 都没开，`mp_ops` 还要求落在 kernel rodata（`net/core/page_pool.c:291-294`）。

---

## 4. 还缺的事实（只有盒子能给）

| 事实 | 影响 | 怎么验 |
|---|---|---|
| 槽长能否从 2048 改 4096 | 决定选项 3 是否成立 | 盒子刷机后改 `ZX_TM_TAB_LIMIT` 试收包 |
| 16 MiB arena 的 CMA 归属 | 已由 `.config` 间接确认（`CONFIG_CMA=y`、`DMA_CMA=y`、32 MiB），`dmesg | grep -i cma` 可定死 | 盒子 |

---

## 5. 我的判断（建议，不是决定）

- **最稳的推进顺序**：先做选项 2（30–50 行，把"必现的 FIFO 堵死"和"潜在双 put"修掉）→ 长稳跑 12–24 h → 若 `lost` 斜率可接受，再升级到选项 1（真账 + epoch）。
- **不建议现在做选项 3**：前提未证实，且即使成功，TX 侧 linearize 会把收益吃掉大半。
- **选项 4 永远保留**：`zc_rx=0` 是零成本安全网，任何阶段都可一键回落。
- **选项 5 是另一条产品线**，不是本 bug 的修复路径；要做应单独立项。

---

## 6. 附：证据文件

- `docs/zc-mainline-adoption-assessment-2026-11-01.md`（A 报告，主干可读，细节按 §2 勘误）
- `.scratch/zc-mainline/second-opinion.md`（B）
- `.scratch/zc-mainline/c/verify-pool-refcount.md`（C）
- `.scratch/zc-mainline/d/verify-getpage-fix.md`（D）
- `.scratch/zc-mainline/e/audit.md`（E）
- `.scratch/zc-mainline/ACCEPTANCE.md`（主 agent 验收档案与亲核事实）

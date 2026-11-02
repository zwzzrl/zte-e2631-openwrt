---
title: v6 extract-index 按端口 provision：真机实测 + 原厂 v6 六十字节表项逐字 dump
date: 2026-11-02
status: done
topic: vendor-re
summary: v6 卸载全史：表项格式、窗口类型、命中实验（19/20 窗口）、rule/extr=2 方案。
evidence: tools/vendor-emu
supersedes:
---

# v6 extract-index 按端口 provision：真机实测 + 原厂 v6 六十字节表项逐字 dump

> **后记（2026-11-02，读本文前先看这段）**：本文写于 2026-10-31，当时把卡点判成"我们的 v6 表项格式
> 不对、原厂那套短 key 才是对"。**这个判断已被推翻**：查表通不通与表项格式无关 —— 改成
> **`rule/extr=2` 的窗口类型**之后，我们自己按 v4 形状写的那套表项**照样命中**〔本板实测〕：
> 命中槽就是软件 CRC 算出的槽，20 个候选窗口里 19 个可用。所以"表项从不命中、要改 `w0` 的 bit3"
> 这句话作废。真正的卡点在**命中之后**：帧被吃掉而不是被转发，交付态 v6 因此仍走 CPU。
> 位定义与表布局以 [cla-registers.md](../reference/cla-registers.md) 为准；本文 §1–§7 保留为过程记录，
> 引用时不得当作现状。

- 日期：2026-10-31
- 对象：真机（OpenWrt v12fix，slot1，内核 6.18.55，模块 `zx279128s-ppe`）+ 离线仿真（`tools/vendor-emu`）
- 起点：`docs/reference/cla-registers.md` §4.4「给 index 3 + rule 56 的可执行写入流水」
  和 §8 未解决项；`docs/known-issues.md` PPE-IPV6-TCP 的 finding 3（key 字符串）
- 约束：**本轮没有刷机、没有改驱动源码**，真机实验全靠模块参数（新加的调试参数一个都没用），
  仿真只新增了一个脚本

**标注约定**：`确定` = 本轮真机读数或仿真捕获的原始值；`推断` = 由两条以上证据推出；`未解决` = 写进 §7。

---

## 1. 一句话结论

**"按入端口 provision extract index 1..8"这条路线在真机上确实把 v6 分类器唤醒了——但唤醒之后
硬件既不转发也不交给 CPU，帧直接消失；表项铺满全部 520 个哈希槽仍然一样。** 同时，从原厂
`zte_api_fast_l3_session_add` 真身跑出来的 v6 六十字节表项看，**原厂 v6 表项的 key 只有 3 个字
（0x11140043 / 0x50400300 / 0x50800300），而我们的驱动按 v4 的形状写了 40 字节 19 窗口 key**。
所以这条路不是"少 provision 一张表"，而是**表项格式本身就不是 v4 那一套**。

> **这句是当时的判断，已撤回**（2026-11-02）：格式差异真实存在，但它不是不命中的原因；见文首横幅。

三条被本轮钉死的结论：

1. `确定` index 3（lan1）provision 之后，v6 分类是**活的**：同一股 v6 流，`ipv6=0` 时 60,000 帧
   有 52,137 帧回到主机、CPU 提交 58,400 次；index 3 生效后，回程 0 帧、CPU 提交 17~26 次。
   也就是说帧被分类器吃掉，既没走 CPU，也没从任何口发出去。
2. `确定` 把同一份 v6 表项铺满 0..383 槽、再扩到 0..519 槽（含原厂另外三个库 4/5/6），
   结果不变（回程 0 帧、CPU +21）。**"槽算错了"不是唯一原因，条目本身不被接受。**
3. `确定` 原厂 v6 会话表项：`w0=0x0100004c`（FWD、egress 字段 0 = PON 侧、flow=0、多一个 bit3），
   `w1=0xf513e100`，`w2=0x00000608`，`w3=0`，`w4=0x11140043`（byte16=0x43，后面是 `14 11`），
   `w5=0x50400300`，`w6=0x50800300`，`w7..w14` 全 0。我们驱动写的同一条流是
   `w0=0x03003044`（flow=1、egress=3、无 bit3）、`w3=0x80000000`、`w4..w14` = 40 字节 19 窗口 key。
   **两边的表项形状根本不同**（§5 有逐字对比）。

---

## 2. 实验设置

### 2.1 台子

和之前几轮一致：主机 `fdee:70df:d02e::2` 发 UDP 1400 B，
目的 `fd88::2`，盒子把这个目的地址按 `/128 via 主机` 路由回同一个 LAN 口，所以**回程帧数就是
"硬件到底转发没有"的判据**；同时读 `dma_state` 的 `tx_submitted` 看 CPU 有没有碰这些包。
判据（本轮反复用了 9 次，读数稳定）：

| 读数 | 含义 |
|---|---|
| 回程 ≈ 52,000 / 60,000，`tx_submitted` +55,000 左右 | 全部走 CPU（分类器没管） |
| 回程 0，`tx_submitted` +20 左右 | 分类器吃掉并丢弃（**不是**硬件转发） |
| 回程 ≈ 60,000，`tx_submitted` +20 左右 | 真·硬件转发（v4 就是这个形状：+138/128,000） |

测量注意（踩过一次）：主机 AF_PACKET 用 `ETH_P_ALL` 抓包会把**自己发出去的帧**也数进来
（目的 MAC 是盒子的 MAC）；`bench6.py sniff` 绑的是 `ETH_P_IPV6`，只收到回来的帧，所以别用
`ethall.py` 的总数当回程数——要按目的 MAC 分。

### 2.2 载入配置（全部是模块参数，没有改源码）

```sh
rmmod zx279128s_ppe
insmod /lib/modules/6.18.55/zx279128s-ppe.ko ipv6=1 \
  v6_ext=3 v6_rule=56 v6_key=0x38 v6_entry=3 \
  v6_idx="0x33323130,0x37363534,0x3b3a3938,0x3f3e3d3c,0x00190100"
/etc/init.d/firewall reload
```

含义（全部来自 `docs/reference/cla-registers.md` §4.2/§4.4）：

- `v6_ext=3`：index 3 = lan1（DTS `gmac_port2`），把索引项写到 3 而不是 10；
- `v6_idx=...`：`aclRamInit` 给 index 3 的 16 个 field id `0x30..0x3f`，word4 = `0x00190100`
  （`ext_mode=2`、`index_valid` 只使能 slot 8）；
- `v6_rule=56`：`(3<<4)|8`，把 19 窗口 v6 规则写到组 3 的 slot 8；
- `v6_key=0x38`：45 字节哈希键里的规则字节 `(3<<4)|8`；
- `v6_entry=3`：60 字节表项 byte16 = `0x40|3 = 0x43`。

## 3. 真机结果（9 组，同一台子、同一五元组）

| # | 配置 | 装流后 `ppe_flows` | 回程帧（主机侧） | `tx_submitted` | 结论 |
|---|---|---|---|---|---|
| 0 | `ipv6=0`（对照） | 0 | 52,137 / 60,000 | +58,400 | 全部走 CPU，台子正常 |
| 1 | index3 + rule56 + w4 `0x00190100`（§2.2 原样，不玩 trap） | 0 | 0 | +26 / 128,000 | 分类器吃掉并丢弃 |
| 2 | 同 1，但先 `trap_override=0x86dd` 装流再撤 trap | 6 | 0 | +19 / 60,000 | 有表项也不转发 |
| 3 | 同 2，w4 换 `0x00190154`（抄 index 10 的 valid 位） | 2 | 0 | +18 / 60,000 | 同上 |
| 4 | 同 2，加 `v6_shotgun=1`（铺满 0..383） | 2 | 0 | ≈0 / 60,000 | 槽不是唯一原因 |
| 5 | 同 4，再加 `v6_shot_ext=1`（铺满 0..519，含库 4/5/6） | 2 | 0 | +21 / 60,000 | 520 个槽全铺也不行 |
| 6 | index3 有效、但规则写到 8 号（rule 56 空着） | — | 0 | +17 / 60,000 | 不是"我们写的 r56 把帧拦了" |

第 2~5 组里 `ppe_flows` 都能装到 2~6 条（说明软件侧认为 v6 key 自洽了），装流后撤 trap
（`echo 0 > .../trap_override`）再打流，仍然是 0 回程 + CPU 几乎不涨。第 2 组 trap 还开着时
回程 52,798 / 60,000（正常走 CPU），撤 trap 后立刻变成 0 —— 同一批流、同一秒内，只改 trap。

**第 6 组值得单独说**：它排除了"我们的 v6 规则内容把 ACL 变成 deny"这个解释——把规则写去
8 号、56 号留空，丢包照样发生，所以丢包是"index 有效 + 分类器接管"这件事本身带来的，
不是某一份规则项的副作用。

## 4. 原厂 v6 表项：逐字 dump

原厂那条路（`zte_api_fast_l3_session_add`，入端口 2 = lan1，五元组
`fd99::2:48211 -> fd88::2:9999`，UDP）写下的 60 字节表项（新增脚本
`tools/vendor-emu/t_v6flowdump.py 2`，`确定`）：

```
slot 27:
0100004c f513e100 00000608 00000000 11140043 50400300 50800300
00000000 00000000 00000000 00000000 00000000 00000000 00000000
byte16 = 0x43
```

同一次会话添加**只写了这一个 hash 表项**：没有 `cla_set_extra_rule_table`，也没有
`cla_set_extra_index_table`（§0 的 `t_v6extra.py flow` 早先也看到 v6 只读 `CLA+0x90/0x94`
两个寄存器）。也就是说原厂 v6 快转条目**不依赖我们写的那份 19 窗口规则**——这正好解释了
§3 第 6 组的结果。

### 4.1 和驱动写法的逐字差异（同一条 hairpin 流，`确定`）

驱动（`ipv6=1` + index3 那套参数，`fdee:70df:d02e::2 -> fd88::2`）在硬件里的表项：

```
03003044 fa11c000 00000608 80000000 00000043 dffdee11 00d02e70 00000000
02000000 00fd8800 00000000 00000000 02000000 0fd8ff00 00000027
```

| 字 | 原厂 v6 | 驱动 v6 | 差异解读 |
|---|---|---|---|
| w0 | `0100004c` | `03003044` | 原厂 flow=0、egress=0（PON 侧）、带 bit3；驱动 flow=1、egress=3（lan1）、无 bit3 |
| w1 | `f513e100` | `fa11c000` | 不知道语义，但两套 v6 用的是不同常数 |
| w2 | `00000608` | `00000608` | 相同 |
| w3 | `00000000` | `80000000` | 驱动抄的是 v4 stock 的常数，原厂 v6 是 0 |
| w4 | `11140043` | `00000043` | byte16 都是 `0x43`（`0x40|idx`）；原厂后面还带 `0x14`、`0x11`（协议号在 byte19） |
| w5.. | `50400300 50800300` 之后全 0 | 40 字节 19 窗口 key | **原厂 v6 key 只有 3 个字，驱动写了整份 19 窗口** |

`推断`：驱动是把 v4 的表项形状（`ZX_CLA_W0_FLOW/FWD/EGRESS/BITS` + 19 窗口 key）直接套到 v6 上，
而原厂 v6 表项是另一套形状：`flow=0`、`egress=PON(0)`、多一个 bit3、key 短到只剩"协议 + 前缀/端口"
三个字。这和我们 2026-10-29 记录的那条"没解的矛盾"（原厂 type-5 hash 表项只写 v4 那 13 字节窗口）
其实是同一件事的正面证据。

### 4.2 仿真器里发现的两个坑（对以后读这些表很重要）

1. **168 字节会话条目里的 IPv6 地址不是原始字节，是掩码编码**。`switch.ko` 交给 `tm.ko` 的
   条目 `+0x04/+0x14` 存的是 `00 40 03 00 50 ...` / `00 80 03 00 50 ...` 这种压缩形式，
   不是 `fd99...`/`fd88...`；所以"改目的地址再 dump 一次看哪几个字变"这个办法在
   仿真里不成立（改了也找不到那 16 字节）。
2. **`t_extidx.py flow` 打出来的槽号只能当"idx 参与键"的证据，不能当"厂商的键布局"**。
   它那条路把 `switch.ko` 内存里的 sip/dip 指针直接喂给另一个 Unicorn 实例，地址读成 0，
   所以槽号会随 inport 变、却不一定反映真实地址参与后的结果。**"改目的地址看槽变不变"
   这个 A/B 至今没做成**：会话条目里没有 `fd88...` 这样的原始地址字节，往 `fill_desc`
   的 dip 缓冲里写新地址也改不到会话条目（2026-10-31 复核，试过写缓冲和改指针两种，
   168 字节条目都不变），所以"槽是否依赖地址"仍是未解决项（见 §7）。
   已有的一次复核：会话添加全程只调用 `cla_get_hash_poly_config` 和
   `cla_set_hash_table` 两个 hash 相关函数，**算槽的代码没有独立符号**（内联在调用者里），
   所以抓不到键；下一轮得直接反汇编调用 `cla_set_hash_table` 的那段内联代码。

---

## 4.3 追加（同日）：厂商 v6 的哈希键与槽算法已经对上

把 `cla_acl_hash_addr_gen` / `aclGetAvailableHashAddr` / `cla_set_hash_table` 的入参用
poke（只读、不替换函数）打出来之后，v6 的软件键和槽第一次有了逐字证据（`确定`）：

* 会话添加只调用 `cla_get_hash_poly_config` + `cla_set_hash_table`；`cla_set_hash_table`
  的 `r0` 就是算好的槽，`r1` 指向栈上的 **45 字节键**（紧随其后 45 字节处是 60 字节表项的
  临时缓冲），`r2 = 0x43`（`0x40|idx`），`r3 = 0x40`。
* 我们这条流（inport 2 = lan1、fd99::2:48211 -> fd88::2:9999、UDP）的 45 字节键是：

```
00 00 00 18 22 00 06 80 a0 00 06 00 a1 00 00 ... (后面全 0)
```

  也就是**只有前 13 个字节有意义**，规则字节 `0xa0 = 10<<4` 在 **byte 8**，不是我们
  驱动放在 bit 23 的那个位置。
* **我们的 crc 公式是对的**：用 `工作文件` 里驱动同一套 `crc(0x04c11db7, key, 45) & 0xff`
  算这个键，得到 **27**，正是厂商写进的槽。把会话里 0x08/0x10/0x20 三段各 8 字节改成
  标记值后再跑，槽变成 214 / 125 / 17，同一个公式用对应的键也全部复现（四个样本全中）。
  所以不是哈希算法的问题，是**键的内容/布局**的问题。
* 表项里的 13 字节（w4 起）和哈希键不是同一个字符串：
  表项 = `43 00 14 11 00 03 40 50 00 03 80 50 00`，哈希键 = `00 00 00 18 22 00 06 80 a0 00 06 00 a1`。
  两者长得像（都有 `06 80`、`40 50`、`80 50` 这类片段），说明它们是同一种"压缩前缀"
  编码的两个视角，但**不是同一份字节**。

**还没解决**：这 13 个字节里地址是怎么压的。仿真里 168 字节会话的 `+0x04/+0x14` 是
掩码编码的空壳（不是 `fd99...`），所以键里也看不到真实地址；下一步要先用一个**地址字段
已知**的会话（手工构造 168 字节条目，把 `+0x04/+0x14` 按 `switch.ko` 的掩码编码写死），
再扫一遍"会话字节 -> 键字节"的映射，就能把 13 字节的位布局定下来。

## 5. 这对"实现 IPv6 硬件加速"意味着什么

按证据强弱排：

1. `确定` **按端口 index 的 v6 分类器存在、而且生效**（§3 第 1/6 组）。以前"开了 ipv6=1
   也没变化"是因为 index 1..8 从来没 provision 过，分类器根本没进这条路径。
2. ~~`确定` **我们的 v6 表项格式不对**~~ **已撤回**（2026-11-02）：原厂 v6 表项确实与 v4 形状不同
   （短 key + `egress=0/flow=0`），但形状不同**不是**不命中的原因 —— 换成 `rule/extr=2` 窗口类型后，
   驱动写的那套 v4 形状长 key 照样命中（19/20 窗口）。本节 §4 只留作两套格式的对照，**别当根因引**。
3. `推断` 原厂 v6 快转的 egress 是 **PON 侧**（字段 0），也就是它的 v6 快路径本来就是
   "上行到 PON / PON 下行"这类场景，而不是 LAN↔LAN 的 hairpin。我们用 hairpin 台子测，
   可能从一开始就不是原厂 v6 快路径对应的拓扑。
4. `确定` 本机 OpenWrt **没有 PON netdev**（`ip -br link` 只有 lan1/2/3、wan、br-lan、wlan；
   wan 是 gmac_port3、当前 DOWN），所以"原厂那种 egress=PON 的 v6 快转"在这台机器上
   现在也没有可测的出口。

## 6. 下一步（可执行，按性价比排序）

1. **先把原厂 v6 表项的真实 key 拿全**（离线，不用刷机）：修 `t_v6flowdump.py` 的会话数据，
   让 168 字节条目里的掩码编码前缀换成可控值（或直接调用 `tm_acl_get_fastHashRule` 手工喂
   一个已知的 168 字节会话），拿到"地址如何进 key、key 有多长、hash 用什么算法"。
   判据：软件算出的槽 == `cla_set_hash_table` 的槽；key 里能找到我们喂进去的前缀。
2. **给驱动加一个"整条 60 字节 v6 表项直写"的调试参数**（例如 `v6_w0..v6_w14` 或
   `v6_raw=`），这样换格式不用重新编译内核；先用它验证 §4 的 `w0=0100004c/flow=0/egress=0`
   形状在真机上是否被判为合法表项（配合 `v6_shotgun` 全铺，避免槽的问题）。
3. 如果 §6.2 证明"原厂形状的表项能被接受并转发"，再谈驱动按 port idx 生成这种表项
   （以及 egress=PON 在 OpenWrt 侧到底对应哪个 netdev）；如果证明不能，这条路就该正式
   记为"此 SoC 的 v6 快转在 OpenWrt 驱动模型下不可达"，把 `ipv6=1` 的定位收敛在
   "LAN↔LAN v6 走硬件交换、routed v6 走 CPU"。

## 7. 未解决

- 原厂 v6 表项 w1/w5/w6 的语义（需要 §6.1 的真实 key）。
- ~~`w0` 的 bit3、`w3` 的 bit31 是"合法位"还是"动作位"~~ **不再是卡点**：2026-11-02 的实验在不动
  这两个位的前提下就命中了，先动它们没有必要。
- 本机没有 PON 出口，无法直接复现原厂 egress=PON 的 v6 快转。

## 8. 复现命令

```sh
# 真机（台子、参数、量法见 §2.2；主机侧驱动脚本未入库）
# 一轮 = 用 §2.2 的 insmod 装流 → 撤 v6 trap → 单向 3×20000 个 1400 B 包 → 数回程与 CPU 包数
# word4 取 0x00190100 / 0x00190154 两档各跑一轮；最后一轮附加 `v6_shotgun=1 v6_shot_ext=1`
# 复原
rmmod zx279128s_ppe && modprobe zx279128s_ppe
echo 1 > /sys/module/zx279128s_ppe/parameters/ipv6
/etc/init.d/firewall reload

# 离线：原厂 v6 表项 dump（镜像 e2631-uni:py312，挂 /ko = 原厂 kmodule-unsquashfs）
cd tools/vendor-emu
docker run --rm -v "$PWD":/w -v <原厂kmodule目录>:/ko -w /w e2631-uni:py312 \
    t_v6flowdump.py 2
```

---

## 附：vendor-acl-hardfast-re-2026-10-30.md（`vendor-acl-hardfast-re-2026-10-30.md` 已并入本文件，原文件删除）

## 厂商 ACL L3 hardfast 反汇编复现（E2631 / ZX279128S）

- 日期：2026-10-30
- 对象：`switch.ko`（224808 B，未 strip）、`tm.ko`（1270520 B）、`vendor-vmlinux.bin`（FFE 相关字符串）
- 方法：离线反汇编（`tools/vendor-emu/t_hfset.py`，走 `km.py` 的段布局+重定位，避免 `.LC` 局部标签在 `ld -r` 合并后重名）+ Unicorn 仿真（`tools/vendor-emu/t_hfsess.py`，两段式：switch.ko 产条目 → tm.ko 吃条目并拦硬件写）
- 约束：全程离线，不连路由器、不刷机、不改仓库内 C 源码

**本文件里 `确定` / `推断` / `未解决` 三分标注是硬要求**：每条结论要么能追到反汇编地址，要么能追到仿真里被截到的值。

---

### 1. 一句话结论

**ACL L3 hardfast 的硬件条目（60 字节 / 15 字）里没有“入端口 / 端口组 / 入方向 bitmap”这类匹配字段**——条目中唯一的端口字段是 `word0` 的 bit12..23，由会话的**出口设备名**填进去（实测线性 0..0xff，`0x9a=port → word0 |= port<<12`）；入端口只被 tm.ko 用来**选 extract index**（v4 硬编码 9/10，v6 走端口映射表 1..8），不进条目。因此 `npu_cap_array` 里“eth→eth 不做 IPv6”是**厂商 FFE 的软件策略**，不是条目格式的物理限制——**结构上乐观**。但落地仍偏悲观：厂商自己的 v6 分支在 tm.ko 里没有把 extract index 1..8 的任何窗口配过（`tm_acl_fast_init` 只 provision 了 9 和 10），说明这条 v6 路径原厂从未跑通，自己下发要连带把 index 表一起补上。

---

### 2. 写路径：从 `hf_set_l3_entry` 到一个寄存器

#### 2.1 调用链（含地址）

| # | 位置 | 地址 | 做什么 |
|---|---|---|---|
| 1 | `switch.ko: hf_set_l3_entry` | `0x2f64` (0x98) | 空指针打 `.LC57`（`input ERROR: ptSession=NULL`）；否则 `0x2f70 bl sw_acl_l3_hardfast_session_add`，成功返回 1，失败按 `g_switch_debug_level` 打 `.LC58` |
| 2 | `switch.ko: sw_acl_l3_hardfast_session_add` | `0x28e0` (0x684) | `0x2924 __memzero(sp+0x30, 0xa8)` → 造 **168 B `zteHffEntry`**；填方向/地址/端口/协议/MAC；`0x2af4 bl zte_api_fast_l3_session_add(entry, out)`；把返回的 6 个字（句柄）抄进 `desc+0x64..0x78` 供 del/status 用 |
| 3 | `tm.ko: zte_api_fast_l3_session_add` | `0x5ce74` (0x1590) | 拷内部会话；`cla_list_hash_addr_gen(session+4, 0x28)` 算候选 slot；`tm_findFastIdxByTuple`；按会话 type 分派（见 3.3） |
| 4 | `tm.ko: tm_acl_fast_add` | `0x51964` (0xcc) | 跳转表 `0x51998`：type→(L3 标志, TCP 标志)；调 `tm_acl_fast_add_v4v6` |
| 5 | `tm.ko: tm_acl_fast_add_v4v6` | `0x4d2b4` (0x7a4) | 取 `inport`（`ldr r0,[r7,#8]`）与 rule type；`subne r3,#3`/`subeq r3,#4`+`clz`+`lsr #5` 置两个瞬时全局；`bl aclGetExtIdxRuleRamAddrByPort(port,&out)`；**失败打 `ERROR:invalid sorce port %d` 并返回** |
| 6 | `tm.ko: aclGetExtIdxRuleRamAddrByPort` | `0x4b19c` (0xc8) | port>0x1f → `-0xb`；有方向标志 → 固定 9 或 10；否则线性搜 `s_aclPortExtIdxTblMap`（`.rodata` `0x1006d848`，24 项 `{port,extidx}`）→ 1..8；未命中 → `-0xb` |
| 7 | `tm.ko: tm_acl_get_fastHashRule` / `aclGetAvailableHashAddr` | 见 `0x4d2b4` 内 `bl` | 组 60 B 规则（15 字），并找空 hash 槽 |
| 8 | `tm.ko: cla_set_hash_table(slot, entry60)` | `0x5a24` | `slot<0x208`；`0x5b54` 分段映射到 `ram_id`；`cla_set_indirect_rw_cmd(0, ram_id, slot-base)`；再 `data_id=14..0` 降序写 15 个字（`data_id k` ↔ 条目内偏移 `4*k`） |
| 9 | `tm.ko: cla_set_indirect_rw_cmd` | `0x44c` | 校验 `ram_id<=9`、`ram_addr<=0x400000`、`rw<=1`；`value = ram_addr \| (ram_id<<22) \| (rw<<27)`；`tmOnuRegWrite(0, value, 0, &claRegTable)` |
| 10 | `tm.ko: cla_set_indirect_rw_data` | `0x5b8` | 校验 `data_id<=0x10`；`tmOnuRegWrite(2, val, data_id, &claRegTable)` |
| 11 | `tm.ko: tmOnuRegWrite` | `0x3609c` | `addr = entry->base + entry->stride*idx`；RMW：`readl` → `(old & ~(mask<<shift)) \| ((val & mask)<<shift)` → `writel` |
| 12 | 内核 | — | `readl/writel(0xF4000000 + 4*addr)` |

`claRegTable`（`tm.ko` `.data`+0x70，28 B/项）用到的三行：

| regno | base | stride | cnt | mask | 作用 | 虚拟地址 | 物理地址 |
|---|---|---|---|---|---|---|---|
| 0 | `0x0e3005` | 0 | 1 | `0x0fffffff` | CLA 命令字 | `0xF438C014` | `0x9238C014` |
| 1 | `0x0e3006` | 0 | 1 | `0x1` | 命令/状态 | `0xF438C018` | `0x9238C018` |
| 2 | `0x0e3007` | 1 | 17 | `0xffffffff` | 数据字 `data_id` | `0xF438C01C + 4*data_id` | `0x9238C01C + 4*data_id` |

#### 2.2 地址换算（确定）

- `fpga_read_reg/fpga_write_reg` 就是 `readl/writel(0xF4000000 + 4*off)`，`0xF4000000` 是 phys `0x92000000` 的 ioremap 虚拟址，窗口 4 MiB；所以 `phys = 0x92000000 + 4*off`。
- `off = base + stride*idx`，与上表逐行对得上：`base=0x0e3024` → `phys=0x9238C090`（多项式配置）、`base=0x0e3028 stride 1 cnt 4` → `0x9238C0A0`（local6）、`base=0x0e303f stride 1 cnt 8` → `0x9238C0FC`（ONU S/D IPv6）。

#### 2.3 值流水（一次 add 的完整字节流）

```
desc(会话描述, u8[] )
  └─ sw_acl_l3_hardfast_session_add @0x28e0
       ├─ E = sp+0x30, 0xa8 B, __memzero 后逐字段填（偏移见 §3.1）
       ├─ 0x2af4 bl zte_api_fast_l3_session_add(E, out16)
       │    └─ tm @0x5ce74
       │         ├─ 拷贝 E 进内部会话结构（读 +0x00/+0x9d/+0x98/+0x9a/+0x9c/+0x9e/+0x2e/+4../+0x84..）
       │         ├─ slot = cla_list_hash_addr_gen(...)
       │         ├─ tm_acl_fast_add → tm_acl_fast_add_v4v6
       │         │     ├─ ext_idx = 9/10（type 3/4）或 端口表 1..8（type 5..8）
       │         │     └─ 组 60 B 规则 R[15]
       │         └─ cla_set_hash_table(slot, R)
       │              ├─ cla_set_indirect_rw_cmd(0, ram_id, slot-base)  → 写 0xF438C014
       │              └─ for k=14..0: cla_set_indirect_rw_data(k, R[4k])  → 写 0xF438C01C+4k
       └─ desc+0x64..0x78 = out 的 6 个字（句柄）
```

`slot → ram_id` 分段（`0x5b54`，`确定`）：`<0x100`→2、`0x100..0x17f`→3、`0x180..0x1bf`→4、`0x1c0..0x1ff`→5、`0x200..0x207`→6；`ram_addr` 取段内偏移。

---

### 3. 条目布局

一共有**两份**条目，别混：

- **(A) 会话条目 `zteHffEntry`，168 B** —— `switch.ko` 产、`tm.ko` 读，**不进硬件**。
- **(B) CLA 规则条目，60 B / 15 字** —— `tm.ko` 产、**直接写进 CLA 哈希表**。

#### 3.1 (A) 168 B 会话条目字段表

| 偏移 | 宽 | 内容 | 来源 | 置信 |
|---|---|---|---|---|
| +0x00 | 1 | 方向：`TO_WAN(desc+0x18)` 返回 0 时 =1，否则 0 | `0x2930`–`0x2950` | 确定 |
| +0x04 | 16 | 源 IPv6 地址（`rev` 前的原始 16 B） | `sw_acl_setMtchInfo` `0x1e24` | 确定 |
| +0x14 | 16 | 目的 IPv6 地址 | 同上（`desc+0x0c & ~2 == 0` 时只拷 4 B） | 确定 |
| +0x24 | 2 | 源端口，`rev16`（大端存储） | `0x1e24` | 确定 |
| +0x26 | 2 | 目的端口，`rev16` | 同上 | 确定 |
| +0x28 | 1 | 协议号 | `desc+0x10` | 确定 |
| +0x2e | 2 | **会话 type**：`desc+0x14!=0`→2；否则按 `desc+0x0c` 映射 0→0 / 1→1 / 2→3 | `0x296c`–`0x2978`、`0x2b38` | 确定 |
| +0x30 | 2 | 全 `0xff`（掩码） | 实测 | 确定 |
| +0x32 | 2 | VLAN | 实测 | 推断 |
| +0x36 | 6 | 目的 MAC | 实测 | 确定 |
| +0x3c | 6 | 源 MAC | 实测 | 确定 |
| +0x64..0x78 | 24 | 返回句柄（del/status 用） | `0x2b18`–`0x2b2c` | 确定 |
| +0x78 | 16 | 源 IPv6 地址**字节逆序**（降序排布，`*(desc+0x48)` 4 字 `rev` 后写） | `0x2d64` 起 | 确定 |
| +0x88 | 16 | 目的 IPv6 地址字节逆序（`*(desc+0x4c)`） | 同上 | 确定 |
| +0x9a | 2 | **出口端口号** = `get_sw_port_from_devname(desc+0x18)`（`g_sw_cap` 查表） | `0x2abc`/`0x2ae4` | 确定 |
| +0x9c | 1 | `desc+0x55` | `0x2ae4` 后 | 确定 |
| +0x9d | 1 | `desc+0x58` | 同上 | 确定 |
| +0x9e | 1 | **inport**：仅会话 type==2 时填；`get_sw_port_from_devname(desc+0x1c)`，名字是 `pon`/`ptm0` → **5**，否则端口号 | `sw_acl_setMtchInfo` `0x1f08`–`0x1fac` | 确定 |

> `sw_acl_setMtchInfo` 自己的调试串把 +0x9e 叫 **inport**：
> `"[sw_acl_setMtchInfo] ptSession->inputdev_name %s, p_zteHffEntry->inport %d, g_pon_work_mode[0x%x]"`。
> `get_sw_port_from_devname` @`0x7914`：`ethN`→`g_sw_cap[0x0c+N]`（要求 `N <= g_sw_cap+0x04`）；`pon`/`ptm0`/`CPU`→`g_sw_cap+0x190`；wlan→6 / `0x10+n` / 7 / `0x18+n`。

#### 3.2 (B) 60 B CLA 规则条目

仿真截到的一条实测（v6 地址 + v4 形状的 rule type 3，slot 237）：

```
word0  0100004c   word1  f513e100   word2  00000608   word3  00000000
word4  11140049   word5  50400300   word6  50800300   word7  00000000
word8  00000000   word9  00000000   word10 00000000   word11 00000000
word12 00000000   word13 00000000   word14 00000000
```

| 字段 | 位 | 值/来源 | 置信 |
|---|---|---|---|
| `word0[11:0]` | 0..11 | `0x04c`，常量（与 hash/多播标志有关，语义未解） | 确定值 / 未解决义 |
| `word0[23:12]` | 12..23 | **出口端口号**，实测 0..0xff 线性 `word0 = 0x0100004c \| (port<<12)` | 确定 |
| `word0[31:24]` | 24..31 | `0x01`，常量 | 确定值 / 未解决义 |
| byte16 | `word4[7:0]` | `0x40 \| extract_index`，本例 `0x49` = `0x40\|9` | 确定（与既有文档一致） |
| `word4[15:8]` | — | `0x14` = 20（pps？）与能力表 w1 同值 | 推断 |
| `word4[22:16]` | — | `0x14`=20 | 未解决 |
| `word4[23]` | — | 0 | 未解决 |
| `word1` | — | hash 值（`0xe113f5` 一类），随五元组变 | 推断 |
| `word2` | — | `0x0608`，随地址族变 | 未解决 |
| `word5`,`word6` | — | `0x00034050` / `0x00038050`（len/标志，v4 会话里出现） | 未解决 |

> 60 B 条目里**没有任何**入端口 bitmap / 端口组 id / “WAN 侧”标记。**（本文件最关键的否定结论，见 §4）**

#### 3.3 会话 type → rule type → extract index（`确定`）

`tm_acl_fast_add` @`0x51964` 的跳转表：

| 会话 type | L3 标志 | TCP 标志 | rule type | tm 后续 | extract index |
|---|---|---|---|---|---|
| 0 | — | — | 3 | `0x5dd08`（L2/VLAN） | 9 |
| 1 | — | — | 4 | `0x5d4b4` | 10 |
| 2 | 1 | 1 | 5 | `0x5d6bc` | **端口表 1..8** |
| 3 | 1 | 1 | 3 | `tm_acl_setMtchInfo.constprop.52` | 9 |

`tm_acl_fast_add_v4v6` 里 `aclGetExtIdxRuleRamAddrByPort` 的端口映射表（`s_aclPortExtIdxTblMap`，24 项）：

```
0→1  1→2  2→3  3→4  4→5  5→8  0x10..0x17→6  0x18..0x1f→7
```

（表尾还有 `0x10→9`、`0x10→0xa` 两条，因首次匹配永远不可达。）

**这里就是核心问题的答案来源**：入端口只在 `aclGetExtIdxRuleRamAddrByPort` 里被消费一次，产出的是 **extract index**，不是条目里的匹配字段。

#### 3.4 IPv6 TCP 条目的 hex 模板

按 §3.1 字段表拼出来的 168 B（`fd99::2:48211 → fd88::2:9999`，出口端口 2，inport 2）：

```
   +0x00  00 00 00 00 fd 99 00 00 00 00 00 00 00 00 00 00
   +0x10  00 00 00 02 fd 88 00 00 00 00 00 00 00 00 00 00
   +0x20  00 00 00 02 bc 53 27 0f 11 00 00 00 00 00 02 00
   +0x30  ff ff 00 01 01 02 50 54 aa bb cc dd d8 43 ae 68
   +0x40  18 28 00 00 00 00 00 00 00 00 00 00 00 00 00 00
   +0x50  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
   +0x60  00 00 00 00 00 00 00 00 01 00 27 0f 00 00 00 00
   +0x70  00 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
   +0x80  00 00 00 00 00 00 99 fd 02 00 00 00 00 00 00 00
   +0x90  00 00 00 00 00 00 88 fd 00 00 02 00 01 00 02 00
   +0xa0  00 00 00 00 00 00 00 00
```

逐字段出处：

| 字节 | 值 | 出处 |
|---|---|---|
| `+0x00` | `00` | 确定（TO_WAN 分支） |
| `+0x04..0x13` | `fd99::2` | 确定（`sw_acl_setMtchInfo` 拷 16 B） |
| `+0x14..0x23` | `fd88::2` | 确定 |
| `+0x24` / `+0x26` | `bc 53` / `27 0f` | 确定（`rev16` 大端） |
| `+0x28` | `11` | 确定（UDP=17） |
| `+0x2e` | `02 00` | **推断**：想让 tm 走 rule type 5（v6）就必须 =2；=3 会被当成 v4-tcp 走 index 9 |
| `+0x30` | `ff ff` | 实测 |
| `+0x32..0x35` | `00 01 01 02` | 推断（VLAN/prio/端口，实测同位置出现过这些字节） |
| `+0x36..0x3b` | `50 54 aa bb cc dd` | 确定（实测，目的 MAC 原样） |
| `+0x3c..0x41` | `d8 43 ae 68 18 28` | 确定（实测，源 MAC） |
| `+0x68` / `+0x6a` | `01` / `27 0f` | 实测（dport 副本） |
| `+0x78` / `+0x88` | 地址逆序 | 确定（`0x2d64` 起 4 字 `rev`） |
| `+0x9a` | `02 00` | 确定（出口端口，进 `word0[23:12]`） |
| `+0x9c` / `+0x9d` | `01` / `00` | 确定（`desc+0x55` / `desc+0x58`） |
| `+0x9e` | `02` | 确定（inport，只影响 extract index） |

**60 B 侧的 hex 模板**（extract index 3、出口端口 2）：

```
word0  = 0x0100004c | (2 << 12) = 0x0100204c
word4  = 0x11140043        # byte16 = 0x40|3
```
其余 13 字按 index 3 的窗口模板填（`未解决`，见 §7）。

---

### 4. 核心问题：入端口/端口组/方向，到底有没有？

#### 4.1 静态证据（`确定`）

1. **60 B 条目里没有入端口字段**。`cla_set_hash_table` @`0x5a24` 只把 15 个字原样写进 CLA 数据寄存器；在这之前构造这 15 个字的 `tm_acl_fast_add_v4v6`/`tm_acl_get_fastHashRule` 里，唯一从 `inport` 派生出来的量是 `aclGetExtIdxRuleRamAddrByPort(port, &out)` 的返回值，而它落在 `byte16` 的低 6 位（`0x40|idx`）。
2. **条目里唯一的端口字段是出口端口**，位域 `word0[23:12]`，值来自 `E+0x9a`，即 `get_sw_port_from_devname(desc+0x18)`——`desc+0x18` 是 **outdev_name**（对照 `desc+0x1c` 是 `inputdev_name`，后者填 `E+0x9e` inport）。
3. **`word0[23:12]` 是线性的端口号，不是 bitmap**：A/B 扫 0..0xff 时 `word0` 恰好等于 `0x0100004c | (port<<12)`，没有出现“某一位被置起”的形态；配合 `aclGetExtIdxRuleRamAddrByPort` 里 `port > 0x1f → -0xb` 的合法性校验，有效位只用到 `bit12..16`。
4. **方向不是条目字段，是“选哪张表”**。`0`/`1`/`3` 类会话直接落到固定 index 9/10，index 本身就编码了 up/down。

#### 4.2 仿真证据（`确定`）

`t_hfsess.py` 两段式：switch.ko 造条目 → tm.ko 吃条目。

- 基线（v6 地址、type 3）：`ret=0`，写 **slot 237**，15 字如上；`byte16=0x49`。
- **A/B `entry+0x9e`（inport）= 0 / 3 / 5 / 0x99 → 15 个字逐字节完全相同**。即入端口对硬件条目**零影响**（v4 路径下 extract index 固定 9，端口根本不参与）。
- **A/B `entry+0x9a`（出口端口）**：

| `0x9a` | `word0` |
|---|---|
| 0 | `0100004c` |
| 1 | `0100104c` |
| 2 | `0100204c` |
| 3 | `0100304c` |
| 16 | `0101004c` |
| 31 | `0101f04c` |
| 32 | `0102004c` |
| 255 | `010ff04c` |

即 `word0 = 0x0100004c | (port << 12)`，**唯一变化的字段就是它**。

#### 4.3 结论

> 硬件侧：**没有**“入端口 / 端口组 / WAN 侧”这类匹配字段可用，因此硬件并不因为“从 eth 进来”而拒绝一条 IPv6 条目。厂商的 eth→eth 限制发生在 **FFE 软件策略**层（`npu_cap_array` → `npu_register_driver` → FFE，见 §5）。
> 我们要注意的约束只有两条：`word0[23:12]` 要填**出口**端口，`byte16` 的低 6 位要填**按入口端口选出来的** extract index。

---

### 5. 能力表语义与消费方

#### 5.1 `npu_cap_array` 是“流特征描述表”，不是 44 B 记录数组（`确定`）

- 位置 `switch.ko` `.data`+0x1b4（`0x1001d6b4`），大小 **1968 B = 0x7b0 = 6 × 0x148**。
- 6 组里 **g0..g4 有内容、g5 全零**（哨兵），正好对应 FFE 打印的 `====flow_feature index[%d]====`（`vendor-vmlinux.bin` 文件偏移 ~0x5877b8，同处还有 `rxdev:` / `txdev:` / `layer[%d] type[%d] (%s):` / `\t pps:%d` / `\t frag:%d` / `\t layer_cap[%d]:%08x`）。

各组内容（`rxdev`/`txdev` 是设备名匹配串，`确定`）：

| 组 | rxdev | txdev | 挂的能力记录 |
|---|---|---|---|
| g0 | `pon0 pon gpon0 epon0 ptm0` | `eth* wlan*` | `npu_ipv4_l2down_cap`、`npu_ipv6_l2down_cap`、`npu_ipv4_l3tcpdown_cap`、`npu_ipv4_l3udpdown_cap`、`npu_dslite_down_cap`×2、`npu_ipv6_l3tcpdown_cap`、`npu_ipv6_l3udpdown_cap` |
| g1 | `eth* wlan*` | `eth* pon ptm0` | `npu_ipv4_l2up_cap`×2、`npu_ipv6_l2up_cap`×2、**`npu_ipv4_l3tcpup_cap`**、**`npu_ipv4_l3udpup_cap`**、`npu_dslite_up_cap`×2、**`npu_ipv6_l3tcpup_cap`**、**`npu_ipv6_l3udpup_cap`** |
| g2 | `eth*` | `eth0 ptm0` | `npu_ipv4_l2up_cap`（L2 上行） |
| g3 | `eth0 pon ptm0` | `eth*` | `npu_ipv4_l2down_cap`（L2 下行） |
| g4 | `pon0 pon gpon0 epon0 ptm0` | `eth*` | `npu_ipv6_mc_down_cap`、`npu_ipv4_mc_down_cap` |
| g5 | — | — | 全零 |

设备名池（`.rodata` `0x1001d170`–`0x1001d1b8`）：`pon0` `pon` `gpon0` `epon0` `ptm0` `eth` `eth%u` `eth0` `eth*` `wlan*`。

**g1 这一行是本次最有力的反证**：rxdev `eth*`/`wlan*`、txdev `eth*`/`pon`/`ptm0`，并且**同时挂着 `npu_ipv6_l3tcpup_cap` 和 `npu_ipv6_l3udpup_cap`**。也就是说厂商自己在能力矩阵里**声明了 eth→eth 的 IPv6 L3 TCP/UDP 硬转能力**。

#### 5.2 44 B 能力记录：11 个字

16 个 `npu_*_cap` 记录（`switch.ko` `.data` 0x964..0xc24，每个 44 B），实测两组：

```
npu_ipv6_l3tcpup_cap : w0=0 w1=0x14 w2=1 w3=0x001f0000 w4=0x001d1c1f w5=0     w6=0x1c1c0000 w7=0x001c0000 w8..w10=0
npu_ipv4_l3tcpup_cap : w0=0 w1=0x14 w2=1 w3=0x001f0000 w4=0x001d1c1f w5=0x1c1c0000 w6=0     w7=0x001c0000 w8..w10=0
```

- `w1` = pps（`0x14`=20、`0x80`=128、`0x100`=256），对应 FFE 的 `\t pps:%d`；`确定`
- `w2` = frag 标志 / layer 数，对应 `\t frag:%d`；`推断`
- `w3`、`w4` 两记录**完全相同**（`0x001f0000`、`0x001d1c1f`），是两层共享位；`推断`
- `w5..w7` 是“每层能力位”，v6 比 v4 **整体后移一个字**（v4 在 w5/w7，v6 在 w6/w7）；**位值相同** → **v6 L3 的能力声明不比 v4 弱**；`推断`（位域精确含义仍未解，见 §7）

#### 5.3 消费方：只给 FFE，不给硬件（`确定`）

- `npu_cap_array` 在整个 `switch.ko` 里**只有一处引用**：`npu_drv`（`.data`+0x100）的 `+0x00` 字段；`npu_drv` 交给 `npu_register_driver`。xref 只有 `file+0x1d600`（即 `npu_drv` 的初始化项）。
- `switch.ko` / `tm.ko` 里**没有任何代码读这些 44 B 记录、也没有把它们写进任何寄存器**。
- `npu_hff_register` @`0x11e94` 把 `l3_hardfastReg` 填的 4 个函数指针（`hf_set_l3_entry` / `hf_del_l3_entry` / `hf_query_l3_status` / `hf_entry_police2`）拷进 `npu_hff_func_tbl`（`.bss` 0x120c），printk `"register npu driver %d"`（参数 0xc），再 `ffe_register_notifier(ffe_insert_notifier)`。
- FFE 侧存在 `npu_cap_filter`（priority −200）与上述打印串。

→ **`npu_cap_array` 是软件策略表（FFE 查它决定要不要给某个 flow 走硬转），不是硬件能力寄存器。**

---

### 6. 最小落地步骤（乐观方向）

前提：**这条 v6 ACL 路径原厂从未跑通**——`tm_acl_fast_init` @`0x51130` 只写了 rule `0x90`/`0x91`、rule `0x02`/`0x03`，然后只对 extract index **9**（`0x51600`–`0x51614`：`cla_get_extra_index_table(9)` → `word4 |= 1` → `cla_set_extra_index_table(9)`）和 **10**（`0x51658`：`word4 |= 4`）做过 provision。**v6 需要的 index 1..8 一次都没写过**。这与第十轮真机结论“v6 路径对 CLA 窗口 0 次写”自洽。

所以落地顺序必须是“先补 index，再下发条目”：

1. **provision extract index 1..8**（我们自己的驱动里已有 `cla_set_extra_index_table` 封装，直接复用；窗口表 + `word4` 位都要写）。
2. **手工下发一条 60 B 条目**到空槽：`word0 = 0x0100004c | (出端口<<12)`；`byte16 = 0x40 | idx`，`idx` 取自 `{0→1,1→2,2→3,3→4,4→5,5→8}` 或 `{0x10..0x17→6, 0x18..0x1f→7}`；key 字段按该 index 的窗口模板填。
3. **判据**：`/sys/kernel/debug/92000000.ethernet/dma_state` 的 `tx_submitted` 增量 + 对端收包数（老化位不可用作判据）。
4. 命中后再谈 FFE 集成：`npu_cap_filter` 对 eth→eth 的过滤点在哪，是本轮未解决项。

**风险与预期**：结构上不排除（§4），但 §5 显示“能力声明有、代码从未 provision”这种状态在原厂代码里很常见，大概率我们要把 index 窗口语义自己定死，而不是照抄厂商模板。

---

### 7. 未解决项

1. **60 B 条目 `word0[11:0]`/`[31:24]`、`word2`、`word5`、`word6` 的位语义**——本轮只确定 `word0[23:12]`（出口端口）与 `byte16`（extract index）。
2. **`word0[23:12]` 到底是“出口端口”还是“出端口对应的 bucket id”**——值确实由 outdev_name 派生，但语义（匹配侧 or 动作侧）无硬件验证；现已排除“匹配侧入端口位图”这一可能。
3. **extract index 1..8 的窗口/模板字节级布局**——`未解决`。我们已有的 v6 hash 路径（第九轮真机：窗口 0 = `0x1100`、sip、dip、端口 `0xbc53`/`0x270f`）是**另一张表**（hash 表），不能直接当 ACL 窗口模板。
4. **44 B 能力记录的精确位域**（`w3`..`w10` 十层/两层能力位的编号与 `cap1..cap10` 的映射）。
5. **`npu_cap_filter`（FFE 侧，priority −200）的代码级消费点**——`vendor-vmlinux.bin` 扫 `movw/movt` 只命中 `0xc0607fb8` 一处（`.rodata`/iotable 区），未定位到函数体。
6. **会话 type 2 分支（rule type 5）为什么在仿真里产出近乎空的条目**——本轮 `desc+0x14=0` 走的是 type 3；type 2 需要的 `desc` 字段还没补齐，属“待补测”。

---

### 附：复现命令

```bash
## 带符号标注反汇编（走 km.py 重定位，.LC 不会再撞名）
docker run --rm -v "$PWD/<原厂 kmodule 解包目录>":/ko:ro \
  -v "$PWD/tools/vendor-emu":/w -v "<harness 目录>":/o -w /w \
  --entrypoint python3 e2631-uni:py312 t_hfset.py dis sw_acl_l3_hardfast_session_add

## 两段式取证：switch.ko 造条目 → tm.ko 吃条目并拦 CLA 写
python3 t_hfsess.py sess          # 产出 /o/ztehff.bin（168 B）
python3 t_hfsess.py hw            # 打印被截到的 15 个字与调用序列
python3 t_hfsess.py ab 0x9a 31    # 改一个字节做 A/B
```


---

## 附：vendor-ipv6-acl-write-sequence-2026-10-30.md（`vendor-ipv6-acl-write-sequence-2026-10-30.md` 已并入本文件，原文件删除）

## 原厂 IPv6 卸载的硬件访问序列（离线反汇编，2026-10-30 第二版）

> 对象：`switch.ko`（224808 B）、`tm.ko`（1270520 B），取自
> 原厂固件解包出的 kmodule 目录（未入库）。
> 方法：`arm-openwrt-linux-objdump -dr`，靠重定位拿符号名。除标「推断」外都是指令级证据。
> 中间产物：`/tmp/sbrg_add.txt`（`sbrg_add_ipv6table` 全文）、`/tmp/sbrg_lookup.txt`。

### ⚠ 勘误：第一版把两条不同的路径混成了一条

第一版文档写的是
`zte_api_fast_l3_session_add → sbrg_add_ipv6table`。
**这是错的。** 反汇编 `zte_api_fast_l3_session_add`（0x5ce74，0x1590 B）的全部调用：

```
37 printk   4 __memzero   3 _raw_spin_unlock_bh   3 cla_list_hash_addr_gen
1 tm_findFastIdxByTuple   1 tm_add_acl_flow_rule   1 _raw_spin_lock_bh
1 kmem_cache_alloc        1 get_flowidx
```

它**根本不碰 `sbrg_*`**，走的是 **CLA**（`cla_list_hash_addr_gen`）+ ACL 规则队列
（`tm_add_acl_flow_rule`）。

---

### 1. 原厂至少有三套互相独立的表（这一条是本轮最大的修正）

| 表族 | 访问窗口 | 用途 | 对应我们驱动里的 |
|---|---|---|---|
| **CLA**（classifier）哈希表 | ONU 寄存器 **1（命令/状态）、2（数据）** | 分类器哈希查找；`cla_set_hash_table` / `cla_get_hash_table` / `cla_acl_hash_addr_gen` / `cla_list_hash_addr_gen` | `zx_ppe_sbrg_*` 的兄弟：`ZX_CLA_*` 那一套（第七～九轮在打的） |
| **sbrg** IPv6 表 | ONU 寄存器 **19（命令）、20（忙）、76/77/78（数据）、29（table_sel）** | 字符串里明写 `ipv6 multicast address table`，即**组播/MLD 表**（12 字 = 48 B 表项） | `zx_ppe_sbrg_*`（第八轮移植的，格式是对的） |
| **fast_l3_session / PP-RAM** | `addFlowOperInfo` → `pp_pm_set_*_ram_info` | 隧道/封装会话加速（`pp_pm_set_b4_ipv6_ram_info`、`aftr_ipv6`、`pon_pp_set_dslite_*`、`pon_pp_set_6rd_sip`） | 我们完全没有 |

所以「原厂的 IPv6 加速走 ACL L3 hardfast 而不走 CLA」这个说法要收窄：
`sw_acl_l3_hardfast_session_add` 这个名字里的 "acl" 指的是它下发的规则形态，
而它最终落的表是 **CLA 哈希 + 封装 RAM 信息**，不是独立的一张 ACL IPv6 表。

### 2. 调用链（改正后）

```
netfilter flowtable 卸载
  └─ switch.ko  sw_acl_l3_hardfast_session_add(desc)    0x28e0, 0x684 B
       ├─ TO_WAN(desc+24)                    定方向
       ├─ sw_acl_setMtchInfo(desc, stack)    0x1e24, 填匹配域
       ├─ get_sw_port_from_devname()         端口号
       └─ tm.ko zte_api_fast_l3_session_add()  0x5ce74, 0x1590 B
            ├─ tm_findFastIdxByTuple()        0x57c50  ← 按 5 元组在硬件里找现成表项
            ├─ cla_list_hash_addr_gen()  ×3   0x71a0   ← 算 3 个列表地址
            └─ tm_add_acl_flow_rule()         0x51a30
                 └─ addFlowOperInfo()         0x4fe70, 0xcb4 B
                      └─ pp_pm_set_modify_ram_info()
                         pp_pm_set_{pppoe_session_id,b4_ipv6,aftr_ipv6}_ram_info()
                         pon_pp_set_{dslite_traffclass_hoplimit,dslite_flow_lable,6rd_sip}()
```

`sw_acl_l3_hardfast_session_add` 的 22 个调用里 **14 个是 printk**，
真正的硬件动作只有 1 个（`zte_api_fast_l3_session_add`）。
`hf_set_l3_entry`（0x2f64）/ `hf_del_l3_entry`（0x3040）都是薄壳，
只做「调 session_add/del → 返回 0 则返回 1 → 否则按 `g_switch_debug_level` 打日志」。

### 3. CLA 的间接访问协议（`cla_set_indirect_rw_cmd`, 0x44c）

```
value = addr + (sel << 22) + (rw << 27)
tmOnuRegWrite(1, value)
```

* 写数据：`cla_set_indirect_rw_data` → `tmOnuRegWrite(2, ...)`
* 读状态：`cla_get_indirect_rw_status` → `tmOnuRegRead(1, ...)`
* 读数据：`cla_get_indirect_rw_data`（0x668）

**打包格式与 sbrg 完全相同**（`addr | sel<<22 | rw<<27`），只是基址寄存器不同：
CLA 是 **1/2**，sbrg 是 **19/20 + 76/77/78**。

### 4. CLA 的哈希配置：8 个 dword 整块写 ONU 寄存器 12

`cla_set_hash_poly_config(cfg)`（0x103c，0x170 B）把 `cfg[0..7]`（8 个 dword = 32 B）
一次性写进 ONU 寄存器 **12**；`cla_get_hash_poly_config`（0x11ac）读回。
即 **CLA 哈希的多项式/种子是可配置的 8 字块**。

> 这条很关键：如果硬件用的是 A 配置、我们软件算槽号时用的是 B 算法，
> 表项就会写进「硬件永远不会查的那个槽」——症状正好是"写进去了、读回来了、就是不命中"。
> **下一步就该查这 8 个字实际是什么值**（谁在开机时写它）。

### 5. 两个哈希函数（都拿到了）

#### 5.1 `cla_list_hash_addr_gen(key, len)`（0x71a0，60 B，全文）

```
if (len == 0) return 0;
crc = 0;
for (i = 0; i < len; i++) {
        u8 b = key[i];
        crc = tab[(b ^ (crc >> 24)) & 0xff] ^ (crc << 8);   // 表在 .LANCHOR0+0xa7c
}
return crc;          // 完整 32 位，不截断、不收尾异或
```

即 **32 位 MSB 先行 CRC（CRC-32/MPEG-2 风格，init = 0，无 final xor）**。
`zte_api_fast_l3_session_add` 调它 **3 次**（三个列表/三级地址）。

#### 5.2 `sbrg_hash(key, unused, type)`（0xc4b8；IPv6 用 type=0x10c 那条）

* `type` 是**类型选择**（0x4c / 0x10c / 0x3c 各一条路径），不是多项式
* key 的前 **34 字节按字节倒序**后跑表驱动 CRC-16/CCITT（init 0、poly 0x1021）
* 结果取 **低 10 位**
* 34 B = sip(16) + dip(16) + vlan(2)

> 我们的 `zx_ppe_sbrg_hash()` 与此**逐位一致**。

### 6. sbrg 表项的字段（由 `sbrg_lookup_ipv6table` 的取值反推）

`lookup` 把三个字读回来后这样取字段（指令级）：

| 打印串 | 取值 |
|---|---|
| `output:valid = %d` | 查找结果位（不是表项里的位） |
| `output:mode = %d` | `ubfx w0, #2, #1`（0xeec c） |
| `output:age = %d` | `ubfx w0, #1, #1`（0xeec8） |
| `output:pt_mask = 0x%x` | `uxtb`（一个字节） |
| 另有 | `ubfx w0, #3, #12` = **vlan 12 位**；`lsr w0, #15` 起是**地址位** |

**这与我们驱动里那份"bits 3..14 vlan、15.. 地址"的布局是一致的。**
也就是说第一版说"表项布局对不上"**证据不足**——至少 sbrg 这张表没有对不上，
而且它本来就是组播表，不是单播快转的落点。

### 7. sbrg 表项的 4 块写（保留，格式已核实）

表项 = **4 块 × 3 字 = 12 字（48 B）**，块地址 = 槽基址 | 块号：

* 探测：`sbrg_get_indreg_wr_cfg(sel, addr)` 后读 76/77/78，**三字 OR 为 0 才判空槽**
* 写入：`sbrg_set_indreg_wr_cfg(sel, addr)` 后 `tmOnuRegWrite(78→77→76)`
* 三字在栈上的位置 `sp+0x68/0x6c/0x70`（块0）、`+0x74/0x78/0x7c`（块1）…
* **写入时 sel 恒为 0**（第一版猜"要写进四个存储体"，已排除）

槽基址：`sel==1` → `((hash & 0x7f) << 2) | 0x200`；否则 `((hash << 2) & 0xff) | 0x300`。
`table_sel`（reg 29）必须是 **1 或 2**，0/3 原厂自己会拒写。

### 8. 下一步（按性价比）

1. **查 CLA 哈希那 8 个字（ONU reg 12）实际值**，以及开机时谁写它。
   这是现在最可疑的一处：硬件查表用的哈希若与我们算槽号的算法不一致，
   表现就是"表项写对了但不命中"。
2. `cla_acl_hash_addr_gen`（0x6cec，0x4b4 B）是**纯计算函数**（只调 printk/memset），
   里面就是 ACL 哈希的实现。把它读出来，与 1 的结果对照。
3. `cla_set_hash_table`（0x5a24，0x50c B）是我们 CLA 写表逻辑的**原厂对照物**：
   逐条比对它的间接写序列（reg 1/2 的顺序、数据字数、写完是否回读校验）。
4. 别忘了 `tm_findFastIdxByTuple`（0x57c50）：原厂自己有一套"按 5 元组在硬件里
   找现成表项"的例程，正是我们验证命中与否时缺的那个**读侧证伪工具**。

### 8.5 追加：单播加速的真正落点是 `tm_acl_fast_add_v4v6`

`zte_api_fast_l3_session_add` 那条是隧道会话；**裸单播 v4/v6 的加速入口**另有其人：

* **`tm_acl_fast_add_v4v6`**（0x4d2b4，0x570 B）：函数开头在栈上清三块缓冲
  （45 / 60 / 32 字节），然后把 `cla_get_hash_poly_config(&cfg32)` 读进 `sp+20`，
  即**每次加流都先读一遍哈希配置**，再用它算槽。
* **`aclGetAvailableHashAddr`**（0x4cbf0 附近，`.constprop.16`）：找可用槽位，
  与我们的「探测空槽」对应。
* 键的打包方式从指令能看出来：大量 `ubfx rX, rX, #7, #8`（**从第 7 位起取一个字节**）
  和 `lsr rX, #15`，说明 key 是「若干标志位 + 从 bit 7 / bit 15 起按字节排下来」的
  位流——和我们驱动里那份 384 位位流的思路同源，但**字段位置需要逐条重推**。

还有一条重要事实：**tm.ko 里没有任何地方调用 `cla_set_hash_poly_config`**
（全模块只有 2 处引用，都是 `cla_get_hash_poly_config`，分别在 0x4d318 和 0x4e188）。
也就是说这 8 个字不是 tm.ko 写的 —— 要么 `switch.ko` 写，要么开机时由别的东西配好。
**这正是下一步要查的**：如果硬件用的是 A 配置而我们算槽用别的算法，就会"写进去但不命中"。

### 9. 复查命令

```sh
OD=<工具链目录>/bin/arm-openwrt-linux-objdump
K=<原厂 kmodule 解包目录>

$OD -dr --start-address=0x5ce74 --stop-address=0x5e404 $K/tm.ko | grep -A1 R_ARM_CALL   # session_add 的真实调用
$OD -dr --start-address=0x71a0  --stop-address=0x71dc  $K/tm.ko                        # cla_list_hash_addr_gen 全文
$OD -dr --start-address=0x44c   --stop-address=0x520   $K/tm.ko                        # CLA 间接命令 (reg 1)
$OD -dr --start-address=0x103c  --stop-address=0x11ac  $K/tm.ko                        # CLA 哈希 8 字配置 (reg 12)
$OD -dr --start-address=0xeb50  --stop-address=0xf004  $K/tm.ko                        # sbrg lookup 的字段取值
$OD -dr --start-address=0xf004  --stop-address=0xf8d0  $K/tm.ko                        # sbrg_add_ipv6table
```


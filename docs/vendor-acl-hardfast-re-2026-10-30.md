# 厂商 ACL L3 hardfast 反汇编复现（E2631 / ZX279128S）

- 日期：2026-10-30
- 对象：`switch.ko`（224808 B，未 strip）、`tm.ko`（1270520 B）、`vendor-vmlinux.bin`（FFE 相关字符串）
- 方法：离线反汇编（`tools/vendor-emu/t_hfset.py`，走 `km.py` 的段布局+重定位，避免 `.LC` 局部标签在 `ld -r` 合并后重名）+ Unicorn 仿真（`tools/vendor-emu/t_hfsess.py`，两段式：switch.ko 产条目 → tm.ko 吃条目并拦硬件写）
- 约束：全程离线，不连路由器、不刷机、不改仓库内 C 源码

**本文件里 `确定` / `推断` / `未解决` 三分标注是硬要求**：每条结论要么能追到反汇编地址，要么能追到仿真里被截到的值。

---

## 1. 一句话结论

**ACL L3 hardfast 的硬件条目（60 字节 / 15 字）里没有“入端口 / 端口组 / 入方向 bitmap”这类匹配字段**——条目中唯一的端口字段是 `word0` 的 bit12..23，由会话的**出口设备名**填进去（实测线性 0..0xff，`0x9a=port → word0 |= port<<12`）；入端口只被 tm.ko 用来**选 extract index**（v4 硬编码 9/10，v6 走端口映射表 1..8），不进条目。因此 `npu_cap_array` 里“eth→eth 不做 IPv6”是**厂商 FFE 的软件策略**，不是条目格式的物理限制——**结构上乐观**。但落地仍偏悲观：厂商自己的 v6 分支在 tm.ko 里没有把 extract index 1..8 的任何窗口配过（`tm_acl_fast_init` 只 provision 了 9 和 10），说明这条 v6 路径原厂从未跑通，自己下发要连带把 index 表一起补上。

---

## 2. 写路径：从 `hf_set_l3_entry` 到一个寄存器

### 2.1 调用链（含地址）

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

### 2.2 地址换算（确定）

- `fpga_read_reg/fpga_write_reg` 就是 `readl/writel(0xF4000000 + 4*off)`，`0xF4000000` 是 phys `0x92000000` 的 ioremap 虚拟址，窗口 4 MiB；所以 `phys = 0x92000000 + 4*off`。
- `off = base + stride*idx`，与上表逐行对得上：`base=0x0e3024` → `phys=0x9238C090`（多项式配置）、`base=0x0e3028 stride 1 cnt 4` → `0x9238C0A0`（local6）、`base=0x0e303f stride 1 cnt 8` → `0x9238C0FC`（ONU S/D IPv6）。

### 2.3 值流水（一次 add 的完整字节流）

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

## 3. 条目布局

一共有**两份**条目，别混：

- **(A) 会话条目 `zteHffEntry`，168 B** —— `switch.ko` 产、`tm.ko` 读，**不进硬件**。
- **(B) CLA 规则条目，60 B / 15 字** —— `tm.ko` 产、**直接写进 CLA 哈希表**。

### 3.1 (A) 168 B 会话条目字段表

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

### 3.2 (B) 60 B CLA 规则条目

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

### 3.3 会话 type → rule type → extract index（`确定`）

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

### 3.4 IPv6 TCP 条目的 hex 模板

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

## 4. 核心问题：入端口/端口组/方向，到底有没有？

### 4.1 静态证据（`确定`）

1. **60 B 条目里没有入端口字段**。`cla_set_hash_table` @`0x5a24` 只把 15 个字原样写进 CLA 数据寄存器；在这之前构造这 15 个字的 `tm_acl_fast_add_v4v6`/`tm_acl_get_fastHashRule` 里，唯一从 `inport` 派生出来的量是 `aclGetExtIdxRuleRamAddrByPort(port, &out)` 的返回值，而它落在 `byte16` 的低 6 位（`0x40|idx`）。
2. **条目里唯一的端口字段是出口端口**，位域 `word0[23:12]`，值来自 `E+0x9a`，即 `get_sw_port_from_devname(desc+0x18)`——`desc+0x18` 是 **outdev_name**（对照 `desc+0x1c` 是 `inputdev_name`，后者填 `E+0x9e` inport）。
3. **`word0[23:12]` 是线性的端口号，不是 bitmap**：A/B 扫 0..0xff 时 `word0` 恰好等于 `0x0100004c | (port<<12)`，没有出现“某一位被置起”的形态；配合 `aclGetExtIdxRuleRamAddrByPort` 里 `port > 0x1f → -0xb` 的合法性校验，有效位只用到 `bit12..16`。
4. **方向不是条目字段，是“选哪张表”**。`0`/`1`/`3` 类会话直接落到固定 index 9/10，index 本身就编码了 up/down。

### 4.2 仿真证据（`确定`）

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

### 4.3 结论

> 硬件侧：**没有**“入端口 / 端口组 / WAN 侧”这类匹配字段可用，因此硬件并不因为“从 eth 进来”而拒绝一条 IPv6 条目。厂商的 eth→eth 限制发生在 **FFE 软件策略**层（`npu_cap_array` → `npu_register_driver` → FFE，见 §5）。
> 我们要注意的约束只有两条：`word0[23:12]` 要填**出口**端口，`byte16` 的低 6 位要填**按入口端口选出来的** extract index。

---

## 5. 能力表语义与消费方

### 5.1 `npu_cap_array` 是“流特征描述表”，不是 44 B 记录数组（`确定`）

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

### 5.2 44 B 能力记录：11 个字

16 个 `npu_*_cap` 记录（`switch.ko` `.data` 0x964..0xc24，每个 44 B），实测两组：

```
npu_ipv6_l3tcpup_cap : w0=0 w1=0x14 w2=1 w3=0x001f0000 w4=0x001d1c1f w5=0     w6=0x1c1c0000 w7=0x001c0000 w8..w10=0
npu_ipv4_l3tcpup_cap : w0=0 w1=0x14 w2=1 w3=0x001f0000 w4=0x001d1c1f w5=0x1c1c0000 w6=0     w7=0x001c0000 w8..w10=0
```

- `w1` = pps（`0x14`=20、`0x80`=128、`0x100`=256），对应 FFE 的 `\t pps:%d`；`确定`
- `w2` = frag 标志 / layer 数，对应 `\t frag:%d`；`推断`
- `w3`、`w4` 两记录**完全相同**（`0x001f0000`、`0x001d1c1f`），是两层共享位；`推断`
- `w5..w7` 是“每层能力位”，v6 比 v4 **整体后移一个字**（v4 在 w5/w7，v6 在 w6/w7）；**位值相同** → **v6 L3 的能力声明不比 v4 弱**；`推断`（位域精确含义仍未解，见 §7）

### 5.3 消费方：只给 FFE，不给硬件（`确定`）

- `npu_cap_array` 在整个 `switch.ko` 里**只有一处引用**：`npu_drv`（`.data`+0x100）的 `+0x00` 字段；`npu_drv` 交给 `npu_register_driver`。xref 只有 `file+0x1d600`（即 `npu_drv` 的初始化项）。
- `switch.ko` / `tm.ko` 里**没有任何代码读这些 44 B 记录、也没有把它们写进任何寄存器**。
- `npu_hff_register` @`0x11e94` 把 `l3_hardfastReg` 填的 4 个函数指针（`hf_set_l3_entry` / `hf_del_l3_entry` / `hf_query_l3_status` / `hf_entry_police2`）拷进 `npu_hff_func_tbl`（`.bss` 0x120c），printk `"register npu driver %d"`（参数 0xc），再 `ffe_register_notifier(ffe_insert_notifier)`。
- FFE 侧存在 `npu_cap_filter`（priority −200）与上述打印串。

→ **`npu_cap_array` 是软件策略表（FFE 查它决定要不要给某个 flow 走硬转），不是硬件能力寄存器。**

---

## 6. 最小落地步骤（乐观方向）

前提：**这条 v6 ACL 路径原厂从未跑通**——`tm_acl_fast_init` @`0x51130` 只写了 rule `0x90`/`0x91`、rule `0x02`/`0x03`，然后只对 extract index **9**（`0x51600`–`0x51614`：`cla_get_extra_index_table(9)` → `word4 |= 1` → `cla_set_extra_index_table(9)`）和 **10**（`0x51658`：`word4 |= 4`）做过 provision。**v6 需要的 index 1..8 一次都没写过**。这与第十轮真机结论“v6 路径对 CLA 窗口 0 次写”自洽。

所以落地顺序必须是“先补 index，再下发条目”：

1. **provision extract index 1..8**（我们自己的驱动里已有 `cla_set_extra_index_table` 封装，直接复用；窗口表 + `word4` 位都要写）。
2. **手工下发一条 60 B 条目**到空槽：`word0 = 0x0100004c | (出端口<<12)`；`byte16 = 0x40 | idx`，`idx` 取自 `{0→1,1→2,2→3,3→4,4→5,5→8}` 或 `{0x10..0x17→6, 0x18..0x1f→7}`；key 字段按该 index 的窗口模板填。
3. **判据**：`/sys/kernel/debug/92000000.ethernet/dma_state` 的 `tx_submitted` 增量 + 对端收包数（老化位不可用作判据）。
4. 命中后再谈 FFE 集成：`npu_cap_filter` 对 eth→eth 的过滤点在哪，是本轮未解决项。

**风险与预期**：结构上不排除（§4），但 §5 显示“能力声明有、代码从未 provision”这种状态在原厂代码里很常见，大概率我们要把 index 窗口语义自己定死，而不是照抄厂商模板。

---

## 7. 未解决项

1. **60 B 条目 `word0[11:0]`/`[31:24]`、`word2`、`word5`、`word6` 的位语义**——本轮只确定 `word0[23:12]`（出口端口）与 `byte16`（extract index）。
2. **`word0[23:12]` 到底是“出口端口”还是“出端口对应的 bucket id”**——值确实由 outdev_name 派生，但语义（匹配侧 or 动作侧）无硬件验证；现已排除“匹配侧入端口位图”这一可能。
3. **extract index 1..8 的窗口/模板字节级布局**——`未解决`。我们已有的 v6 hash 路径（第九轮真机：窗口 0 = `0x1100`、sip、dip、端口 `0xbc53`/`0x270f`）是**另一张表**（hash 表），不能直接当 ACL 窗口模板。
4. **44 B 能力记录的精确位域**（`w3`..`w10` 十层/两层能力位的编号与 `cap1..cap10` 的映射）。
5. **`npu_cap_filter`（FFE 侧，priority −200）的代码级消费点**——`vendor-vmlinux.bin` 扫 `movw/movt` 只命中 `0xc0607fb8` 一处（`.rodata`/iotable 区），未定位到函数体。
6. **会话 type 2 分支（rule type 5）为什么在仿真里产出近乎空的条目**——本轮 `desc+0x14=0` 走的是 type 3；type 2 需要的 `desc` 字段还没补齐，属“待补测”。

---

## 附：复现命令

```bash
# 带符号标注反汇编（走 km.py 重定位，.LC 不会再撞名）
docker run --rm -v "$PWD/工作文件/03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs":/ko:ro \
  -v "$PWD/工作文件/08-源码与构建树/zte-e2631-openwrt/tools/vendor-emu":/w -v "$PWD/.scratch/hf2":/o -w /w \
  --entrypoint python3 e2631-uni:py312 t_hfset.py dis sw_acl_l3_hardfast_session_add

# 两段式取证：switch.ko 造条目 → tm.ko 吃条目并拦 CLA 写
python3 t_hfsess.py sess          # 产出 /o/ztehff.bin（168 B）
python3 t_hfsess.py hw            # 打印被截到的 15 个字与调用序列
python3 t_hfsess.py ab 0x9a 31    # 改一个字节做 A/B
```

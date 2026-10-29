# ACL extract index 1..8：端口映射、provision 流水与取键侧证据

- 日期：2026-10-30
- 对象：`tm.ko`（未 strip，`.text` 与 `.rodata` 直接用符号表定位）、`switch.ko`、板级 DTS
- 方法：离线反汇编（`tools/vendor-emu/t_hfset.py`，走 `km.py` 的段布局+重定位）+
  Unicorn 仿真（新增 `tools/vendor-emu/t_extidx.py`，只拦 `fpga_*` 边界，让
  `aclRamInit` / `tm_acl_fast_init` / `cla_set_extra_index_table` /
  `cla_set_extra_rule_table` / `zte_api_fast_l3_session_add` 全部跑真身）
- 约束：全程离线，不连路由器、不刷机、不改仓库内 C 源码
- 起点：`docs/vendor-acl-hardfast-re-2026-10-30.md`（第十一轮）。本文只回答它 §7 的
  未解决项 3（extract index 1..8 的窗口/模板布局）和 §6 的第 1 步（怎么 provision）。

**标注约定**：`确定` = 有反汇编地址或仿真拦到的写值；`推断` = 由两条以上证据推出来的；
`未解决` = 本轮没定下来，写进 §8。

---

## 1. 一句话结论

**缺的是两件事叠在一起，只补一件都不够：**

1. **extract index 1..8 的索引项和它们各自对应的规则项，我们的内核从来没写过。**
   厂商确实写过，但不是 `tm_acl_fast_init` 写的，而是同一模块里的开机函数
   **`aclRamInit`（`tm.ko` `0x518cc` → `aclRamInit.part.9` `0x5166c`）**：它把
   index **1..10** 每项 5 个字全量写一遍，word 0..3 是 16 个 `extract_index` 字节
   （对 index n 就是 `n<<4 .. n<<4|15`），word4 是 `0x00190000`（index 1..8，ext_mode=2）
   或 `0x00150000`（index 9/10，ext_mode=1）。`tm_acl_fast_init` 只做 index 9/10 的
   "读-改-写 word4 低 16 位"（OR 1 / OR 4）。我们的 OpenWrt 只跑后者的一部分语义，
   `aclRamInit` 一次都没跑过，所以 index 1..8 的**提取配置**在板上处于未知状态
   （bootloader 留的），而 1..8 的**规则项**（rule `(n<<4)|k`）则确定是空的。

2. **我们的 v6 条目把 index 写死成 10、规则号写死成 153，二者都不对。**
   v6（会话 type 2 → rule type 5/6）的 extract index 在厂商代码里是
   `aclGetExtIdxRuleRamAddrByPort(inport)` 用**入端口**查表得到的 **1..8**，不是 10；
   并且这个 idx 会同时进入 60 字节表项的 byte16（`0x40|idx`）和 45 字节哈希键
   （`idx<<4`），所以 **idx 参与哈希槽计算**。仿真里同一五元组、只改 `inport`：
   byte16 从 `0x41` 变到 `0x48`，槽从 312 变到 231（`t_extidx.py flow`）。
   我们按 idx=10 算槽、硬件按入端口 idx 算槽，条目根本不在同一个格子里。
   另外 rule 153 = `0x99` 属于 extract 组 9（`0x99>>4=9`），而 idx 10 的组是
   0..15（见 §4.3），rule 153 连组都对不上。

**所以落地顺序**：先 provision index 1..8 + 8 份规则（或先 dump 确认 1..8 已有），
再把驱动改成"idx 取自入端口、entry/key/slot 三处同一个 idx"。

---

## 2. 端口 → extract index 映射表：谁写的、内容是什么

### 2.1 表本体（`确定`）

| 项 | 值 |
|---|---|
| 符号 | `s_aclPortExtIdxTblMap` |
| 位置 | `.rodata` 文件偏移 `0x1cd8`，大小 `0x30` = 48 B |
| 结构 | 24 项 × 2 B，**`{u8 port, u8 ext_idx}`**，线性首次匹配 |
| 唯一读者 | `aclGetExtIdxRuleRamAddrByPort`（`tm.ko` `0x4b19c`，长 `0xc8`） |

原始字节（`t_extidx.py map` 从 ELF 直接读，不是抄文档）：

```
0001 0102 0203 0304 0405 1006 1106 1206 1306 1406 1506 1606 1706
1807 1907 1a07 1b07 1c07 1d07 1e07 1f07 0508 1009 100a
```

解码（首次匹配，后两条 `0x10→9` / `0x10→10` 永远不可达）：

| port | ext index | 本板对应 |
|---|---|---|
| `0x00` | 1 | **lan3** |
| `0x01` | 2 | **lan2** |
| `0x02` | 3 | **lan1** |
| `0x03` | 4 | **wan** |
| `0x04` | 5 | 未接（DTS 里 `gmac_port4` disabled） |
| `0x05` | 8 | pon/ptm0/CPU 口（`get_sw_port_from_devname` 的 `pon`/`ptm0`/`CPU` 分支） |
| `0x10..0x17` | 6 | wlan 侧端口号段 |
| `0x18..0x1f` | 7 | wlan 侧端口号段 |

**本板四个口的结论（`确定`，来源是板级 DTS + 上表）**：

```
lan3 → port 0 → index 1        lan2 → port 1 → index 2
lan1 → port 2 → index 3        wan  → port 3 → index 4
```

端口号的来源是板级 DTS（`openwrt/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts`）：

```
&gmac_port0 { label = "lan3"; ... }   &gmac_port1 { label = "lan2"; ... }
&gmac_port2 { label = "lan1"; ... }   &gmac_port3 { label = "wan";  ... }
&gmac_port4 { status = "disabled"; }
```

### 2.2 它是"软件算的"还是"硬件寄存器里本来就有"（`确定`）

**软件算的，而且是编译期常量，运行时没有任何人初始化它。**

- 表在 `.rodata`（只读段），`nm -S` 给的是 `r` 类型；模块里没有任何代码写它。
- `aclGetExtIdxRuleRamAddrByPort` 的读法（`0x4b19c`）：

```
0x1004b1a8  cmp  r0, #0x1f        ; port > 31 -> -0xb（EINVAL 类）
0x1004b1b0  movw r3, #0xa1dc      ; r3 = &g_ipv4FastEn - 0xcbc?
0x1004b1b4  movt r3, #0x100c
0x1004b1b8  ldrb r2, [r3, #-0xcbc]   ; g_ipv4FastEn (0x100c9520)
0x1004b1c0  bne  -> 输出 9
0x1004b1c4  ldrb r3, [r3, #-0xcbb]   ; g_ipv6FastEn (0x100c9521)
0x1004b1cc  ldreq lr, [pc, #0x8c]    ; lr = 0x1006d848 = s_aclPortExtIdxTblMap
0x1004b1e4  cmp  r3, #0x18           ; 循环上限 24 项
0x1004b1ec  ldrb r2, [lr, r3, lsl #1]   ; {port}
0x1004b1fc  cmp  r2, r0
0x1004b210  ldrb r3, [ip, #0x49]     ; 0x1006d848 + 1 + 2*i = {ext_idx}
```

- 两个"瞬时全局"是 `tm.ko` `.bss` 的单字节：`g_ipv4FastEn` @`0x100c9520`、
  `g_ipv6FastEn` @`0x100c9521`。它们只在 `tm_acl_fast_add_v4v6`（`0x4d2b4`）里
  被写一次、调用完立刻清零（`0x4d340`/`0x4d344` 写，`0x4d354`/`0x4d358` 清）。
  规则类型和取值的对应（`tm_acl_fast_add` `0x51964` 的跳转表
  @`0x10051998`：type 3→`0x519c8`、4→`0x519e0`、5→`0x519c8`、6→`0x519e0`、
  7→`0x519f8`、8→`0x519b0`，每个分支给 `tm_acl_fast_add_v4v6` 的 r0/r1）：

| rule type | (r0,r1) | 效果 |
|---|---|---|
| 3 | (1,1) | `g_ipv4FastEn=1` → index **9** |
| 4 | (1,0) | `g_ipv6FastEn=1` → index **10** |
| 5 / 6（v6 上行/下行） | 同 3 / 4 的 (r0,r1)，但 type≠3/4，两个全局都写成 0 | **端口表 1..8** |
| 7 / 8（L2） | (0,1)/(0,0) | 端口表 1..8 |

- 表项里只有 `port` 这一个输入；**没有任何运行时从设备树/寄存器填充这张表的代码**
  （反汇编里对 `0x1006d848` 的引用只有 0x4b1cc 这条 PC 相对字面量，读的是常量）。
- 端口号本身是动态的：厂商内核里由 `switch.ko` 读 `g_sw_cap[0x0c+N]`，
  `g_sw_cap` 定义在厂商 4.1 vmlinux（`switch.ko` 里是 `U g_sw_cap`）；
  我们的主线驱动等价物是 DTS 的 `reg = <0..4>` + `label`。
  **也就是说"映射表"是软件常量，"端口号"是设备树/板级配置，二者都不在硬件寄存器里。**

> 这一条把上一轮的说法精确化了：映射不是硬件查表，硬件那边即使有 per-port 选择器，
> 也是另一套我们离线看不到的东西（见 §5 的"推断"）。

---

## 3. provision 一个 extract index 的完整写流水

### 3.1 三张表、三个寄存器（地址换算）

CLA 间接窗口在 switch 的 `0x38c000` 字节偏移上（物理 `0x9238c000`）：

| 用途 | CLA 偏移 | 物理地址 | 说明 |
|---|---|---|---|
| 间接命令字 | `+0x14` | `0x9238c014` | `cmd = index \| (ram_id<<22) \| (rw<<27)` |
| 间接状态字 | `+0x18` | `0x9238c018` | bit0=1 放行（厂商"等非 0"） |
| 数据字 0..16 | `+0x1c + 4*i` | `0x9238c01c + 4*i` | 17 个字；index 表只用 0..4 |

`ram_id`：**0 = extract index 表（每项 5 字）**，**1 = 规则表（每项 17 字 = 68 B，
rule_id ≤ 0x9f）**，2/3 = 哈希库，8 = aging。

### 3.2 index 表项 5 个字的布局（`确定`）

`cla_set_extra_index_table`（`0x4200`）写 `ram_id=0`、`index_id ≤ 0xf`；厂商自己的
调试打印（`0x10073364` / `0x100733b8`~`0x100734bc`）把 20 字节拆成：

| 字节 | 字段 | 说明 |
|---|---|---|
| 0 | `extract_index0` | 第 0 个提取字段 id |
| … | … | … |
| 15 | `extract_index15` | 第 15 个提取字段 id |
| 16..17 | `index_valid` | 16 位位图，哪几个 `extract_indexN` 有效 |
| 18 bit0 | `valid_en` | 表项使能 |
| 18 bit2..3 | `ext_mode` | 9/10 是 1；**1..8 是 2** |
| 18 bit4 | `nomactch_cfg` | 未命中配置 |
| 19 | — | 0 |

`aclRamInit` 对 index n 写入的值（仿真逐字捕获，`t_extidx.py init`）：

| index | word0..3（16 个 field id，小端打包） | word4 |
|---|---|---|
| 1 | `13121110 17161514 1b1a1918 1f1e1d1c` | `00190000` |
| 2 | `23222120 27262524 2b2a2928 2f2e2d2c` | `00190000` |
| 3 | `33323130 37363534 3b3a3938 3f3e3d3c` | `00190000` |
| 4 | `43424140 47464544 4b4a4948 4f4e4d4c` | `00190000` |
| 5 | `53525150 57565554 5b5a5958 5f5e5d5c` | `00190000` |
| 6 | `63626160 67666564 6b6a6968 6f6e6d6c` | `00190000` |
| 7 | `73727170 77767574 7b7a7978 7f7e7d7c` | `00190000` |
| 8 | `83828180 87868584 8b8a8988 8f8e8d8c` | `00190000` |
| 9 | `93929190 97969594 9b9a9998 9f9e9d9c` | `00150000` |
| 10 | `03020100 07060504 0b0a0908 0f0e0d0c` | `00150000`（**特例：字段 id 是 0..15，即第 0 组**） |

规律：**index n 的 16 个 field id 就是 `n<<4 | 0 .. n<<4 | 15`**（index 10 例外，等于
第 0 组的 0..15）。word4 的 `0x19` / `0x15` 差别只在 `ext_mode`（2 vs 1）。

`tm_acl_fast_init`（`0x51130`）对 9/10 做的是 **RMW**，只动 word4 的低 16 位：

```
0x100515f4  mov r0,#9   ; 0x100515fc bl cla_get_extra_index_table
0x10051604  ldrh r3,[sp,#0x1c]   ; word4 低 16 位
0x1005160c  orr  r3,r3,#1        ; v4 fast on
0x10051614  bl cla_set_extra_index_table
0x1005163c  mov r0,#10  ; 0x10051644 get；0x10051654 orr #4（v6 fast on）
0x1005165c  bl cla_set_extra_index_table
```

仿真（`t_extidx.py init9`）用真机 dump 的 9/10 做基线，结果：

```
read  [ 9] -> write [ 9] 93929190 97969594 9b9a9998 9f9e9d9c 00150151（bit0 已置，值不变）
read  [10] -> write [10] 03020100 07060504 0b0a0908 0f0e0d0c 00150154（bit2 已置，值不变）
```

**这就证明 `tm_acl_fast_init` 对 index 1..8 一个字都没写**（它根本没有 1..8 的 get/set 调用）；
1..8 只在 `aclRamInit` 里被全量写过。

`EN_V4FAST_EXTIDXTBL`（`0x4a614`）/ `EN_V6FAST_EXTIDXTBL`（`0x4a670`）给出 word4 低 4 位的
语义（虽然 tm.ko 里零引用、是死代码）：

```
v4: word4[1:0] = enable ? 3 : 1     v6: word4[3:2] = enable ? 0xc : 4
```

即 bit0 = v4 "on"，bit2 = v6 "on"，bit1/bit3 = "fast"。

### 3.3 规则表项 17 个字（`确定`）

`cla_set_extra_rule_table`（`0x46e8`）：`rule_id ≤ 0x9f`，`ram_id=1`，数据字 **16 → 0**
逐个读-改-写（word0 最后写、落表），每项 68 B。字段位图上一轮已经解完
（`docs/vendor-cla-extra-rule-2026-10-30.md` §3.2）：窗口偏移 `7n`、掩码 `140+16n`、
类型 `480+3n`、hash_len `468`、mem_ctrl `473`。

v6 的 19 窗口规则 17 字（与厂商 rule 2 逐字节相同，`t_extidx.py init9` 复现）：

```
e1828406 16284880 01e3868c 94264889 fff0000a
ffffffff ffffffff ffffffff ffffffff ffffffff
ffffffff ffffffff ffffffff 0fffffff 01300000
db6db6db 00db6db6
```

（下行版只差 word14 的 bit25 = `mem_ctrl`，rule 3 是 `03300000`。）

### 3.4 别的寄存器：CLA 的 +0x80/+0x88/+0x94/+0x98（`确定`）

它们**不是** per-extract-index 的 provision，而是 `tm_pon_pp_cla_initial` 在开机时写一次的
全局配置。仿真真身跑出来（`fpga_*` 边界）：

| CLA 偏移 | 物理 | 厂商写值 | 我们的驱动现状 |
|---|---|---|---|
| `+0x80` | `0x9238c080` | `0x00000e00` | 不写（bootloader 留 `0x0000301a`） |
| `+0x88` | `0x9238c088` | `0x00003fff` | `0x00007fff` |
| `+0x94` | `0x9238c094` | `0x00000008`（outer_hash_num=2） | `0x00000004` |
| `+0x98` | `0x9238c098` | `0x3fff3fff` | `0x7fff7fff` |
| `+0xcc` | `0x9238c0cc` | `0x00000000` | 不写（底层驱动留 1） |

> 上一轮已经在真机上把这些厂商值试过（known-issues PPE-IPV6-TCP finding 4 的
> "Excluded by measurement"），没有改变结果。**所以 provision 一个 index 不需要动这五个
> 寄存器**；本文列出来只是为了回答"还有没有别的"。它们与 index 1..8 无关。

### 3.5 给 index 3 + rule 56 的逐次访问流水（`确定`，`t_extidx.py trace 3`）

这是 `cla_set_extra_index_table(3, ...)` + `cla_set_extra_rule_table(56, ...)` 真身跑出来的
（状态字在我们的模型里恒 1；真机也是 1）：

```
#   方向 物理地址      CLA+off  值           说明
00  r  0x9238c018   +0x0018  0x00000001   状态 bit0=1 放行
01  r  0x9238c014   +0x0014  0x00000000   读旧命令字
02  w  0x9238c014   +0x0014  0x00000003   ram_id=0 index=3 rw=0
03  r  0x9238c02c   +0x002c  0x00000000   data4 读-改-写
04  w  0x9238c02c   +0x002c  0x00190100   data4
05  r  0x9238c028   +0x0028  0x00000000   data3
06  w  0x9238c028   +0x0028  0x3f3e3d3c   data3
07  r  0x9238c024   +0x0024  0x00000000   data2
08  w  0x9238c024   +0x0024  0x3b3a3938   data2
09  r  0x9238c020   +0x0020  0x00000000   data1
10  w  0x9238c020   +0x0020  0x37363534   data1
11  r  0x9238c01c   +0x001c  0x00000000   data0
12  w  0x9238c01c   +0x001c  0x33323130   data0（最后写）
13  r  0x9238c018   +0x0018  0x00000001   下一条命令前再等状态
14  r  0x9238c014   +0x0014  0x00000003   读旧命令字
15  w  0x9238c014   +0x0014  0x00400038   ram_id=1 index=56 rw=0
16..49   data16..data0 各一次读-改-写，写入值为 §3.3 的 17 个字
```

> 说明：index 表的 5 个字是"先读后写"（`cla_set_extra_index_table` 每个数据字做
> `tmOnuRegWrite` 的 RMW）；规则表的 17 个字同样 RMW。**读回值在 §3.5 的第 16..49 步
> 在我们模型里会带出上一条命令的数据寄存器残留**，那是仿真模型的边界（真机读回的是该
> 项旧内容），不影响写下去的值。

---

## 4. index 1..8 该填什么：能抄 9/10 吗？

### 4.1 先说结论：**"把 1..8 的 5 字项抄成 9/10 的内容"在结构上不成立**

index 表项**不是窗口模板**，它只是 16 个 `extract_index` 字段 id + 一组标志位；
真正决定"取哪 19 个窗口"的是 **rule 表里那 68 字节**。而且：

- index 1..8 的 word4 是 `0x0019xxxx`（`ext_mode=2`），9/10 是 `0x0015xxxx`
  （`ext_mode=1`）；把 9/10 的 word4 抄过去会改掉 `ext_mode`。
- index n 的 16 个 field id 必须是 `n<<4..n<<4|15`（index 10 是唯一例外，见 §3.2）。
  把 index 9 的 `0x90..0x9f` 抄到 index 1，会让 index 1 指向第 9 组的规则。

**能抄的是规则，不是索引项**：把同一条 19 窗口 v6 规则（§3.3 的 17 字）写到每个
extract 组的一个 rule id 上，例如组 n 的 slot 8 → rule `(n<<4)|8`。这就是"1..8 全部
用同一个 19 窗口模板"的可执行等价物。

### 4.2 组 n 的规则 id 为什么取 `(n<<4)|8`

- 组 n 的 16 个 field id 是 `n<<4..n<<4|15`（`aclRamInit`，`确定`），所以组内规则
  只能在 `n<<4 + 0..15` 里选。
- v4 的现成工作组合是：index 9 + rule **152 = 0x98 = 9<<4|8**；反过来，index 9 的
  stock `index_valid=0x0151`（bit 0,4,6,8）恰好**使能了 slot 8**，而 rule 152 正是
  slot 8。这条是"组内 slot 8 可用"的直接旁证（`确定`值 + `推断`语义）。
- 所以镜像 v4：组 n 选 slot 8，`index_valid` 置 bit8，rule id 写 `(n<<4)|8`。

### 4.3 我们驱动现在的 v6 配置在哪一步就断了（`确定`）

| 项 | 驱动现状 | 应该是（按组模型） |
|---|---|---|
| 索引表 | 只写 index 9（v4，`00150151`）和 index 10（v6，`00150154`）；1..8 不写 | 还要写 1..8（§4.2） |
| 规则表 | v4 rule 152；v6 rule **153 = `0x99`** | v6 写 `(idx<<4)|8` |
| 表项 byte16 | `0x40|10 = 0x4a` | `0x40|入端口 idx` |
| 键里的规则字节 | `0x99`（=153） | `(idx<<4)|8` |
| 槽 | 用 idx 10 / rule 153 算 | 用 idx / rule 算 |

两个立刻能看的矛盾：

1. `0x99 >> 4 = 9` → rule 153 属于 **extract 组 9**，和 index 9 的 field id 列表
   `0x90..0x9f` 同组；而驱动说它是 index 10 的规则。index 10 的 field id 是 `0x00..0x0f`
   （组 0），`0x99` 不在其中。**rule 153 根本不在 index 10 的组里。**
2. 即使硬件的 idx 真的取 10，index 10 的 `index_valid=0x0154`（bit 2,4,6,8）也没有
   bit9，rule 153 同样不是"有效规则"。

这说明"v6 不命中"不是单点问题；extract idx、规则组、槽三者必须一起改。

### 4.4 可执行的写入序列（仿真验证过，`t_extidx.py prov`）

对 n = 1..8 各做两组写（地址见 §3.1）：

**A. index 项（ram_id 0，5 个字）**

```
cmd  0x9238c014 <- n                       （index=n, ram_id=0, rw=0）
data 0x9238c02c <- 0x00190100              （word4）
data 0x9238c028 <- n<<4|0x0c ... 依次打包  （word3 = 字节 12..15）
data 0x9238c024 <- word2
data 0x9238c020 <- word1
data 0x9238c01c <- word0                   （最后写）
```

word0..3 的具体值就是 §3.2 表里 n 对应的 4 个字；word4 = **`0x00190000 | (1<<8)`
= `0x00190100`**（`index_valid` 只使能 slot 8）。

**B. 规则项（ram_id 1，17 个字）**

```
rule_id = (n<<4)|8        # 24, 40, 56, 72, 88, 104, 120, 136
cmd  0x9238c014 <- rule_id | (1<<22)       （ram_id=1, rw=0）
data 16..0 = §3.3 的 17 个字，word0 最后写
```

八组都写同一份 19 窗口 v6 模板（上/下行区别只在 word14 bit25，本方案以上行为准；
下行的 v6 流如果需要，另写 `rule_id | (1<<25)` 的版本，见 §8）。

**C. index 10 保持现在的写法**（v6 引擎使能位）

```
index[10] = 03020100 07060504 0b0a0908 0f0e0d0c 00150154
```

理由：真机上 `v6_idxw4=0x00150004` 是唯一让 v6 引擎"从过 CPU 变成丢弃"的值
（known-issues PPE-IPV6-TCP，第十轮），说明 index 10 的 word4 bit2 是 v6 引擎开关；
它和 per-port 提取配置是两件事，**不要动**。

**D. v4 的 index 9 / rule 152 一律不动**（v4 靠它工作）。

---

## 5. 硬件到底按哪个 index 取键

### 5.1 结论

**按"入端口查表决定的 index"取键；表项 byte16 里的 index 是同一个值的副本，不是选择器。
两者必须一致——因为 idx 参与哈希键，槽和取键用的是同一个 idx。**

### 5.2 反汇编证据

1. `tm_acl_fast_add_v4v6`（`0x4d2b4`）只从会话里取 **inport** 去算 index：

```
0x1004d324  ldr   r0, [r7, #8]          ; inport
0x1004d340  strbne r3, [g_ipv4FastEn]   ; type3 -> v4 on
0x1004d344  strbeq r3, [g_ipv6FastEn]   ; type4 -> v6 on
0x1004d34c  bl    aclGetExtIdxRuleRamAddrByPort   ; (inport, &out)
0x1004d354  strb  r3, [g_ipv4FastEn]    ; 清零
0x1004d358  strb  r3, [g_ipv6FastEn]    ; 清零
0x1004d35c  subs  r6, r0, #0            ; r6 = extract index
...
0x1004d3d4  strb  r2, [sp, #0x37]       ; r2 = r6 & 0x1f -> 45 字节 key 的 byte0
```

   index 的唯一来源就是 `inport`（v4 走两个瞬时全局 9/10，v6 走端口表）。
2. 表项 byte16 由 `tm_acl_get_fastHashRule`（`0x48ec8`，在 `0x4d374` 被调）填成
   `0x40|idx`；**tm.ko 里没有任何代码读表项 byte16 去配置提取**——表项的读路径
   （`cla_set_hash_table`/dump）只把它当数据。

### 5.3 仿真证据（决定性）

`t_extidx.py flow`：同一股 v6 流（`fd99::2:48211 -> fd88::2:9999`，会话 type 2），
168 字节条目只改 `+0x9e`（inport）：

| inport | 板级口 | 端口表 idx | 表项 byte16 | 厂商代码选的 hash 槽 |
|---|---|---|---|---|
| 0 | lan3 | 1 | `0x41` | 312 |
| 1 | lan2 | 2 | `0x42` | 17 |
| 2 | lan1 | 3 | `0x43` | 27 |
| 3 | wan | 4 | `0x44` | 67 |
| 5 | pon | 8 | `0x48` | 231 |

同一五元组、同一个条目模板，只有 inport 一个字节变，**byte16 和槽一起变**。
这只能解释为：idx 进了哈希键，而这个 idx 是端口表算出来的。

### 5.4 为什么"按表项 byte16 取键"在因果上不成立

硬件做的是"提取 → 算哈希 → 定位槽 → 读表项 → 比较"。**在定位槽之前，它还没有读到
表项**，所以不可能用表项里的 byte16 来决定怎么提取。因此提取配置只能来自查表前就已知
的东西：入端口（v6）/方向（v4）。byte16 和 key 里的 idx 是软件写下去的镜像，用来保证
"软件算的槽 = 硬件算的槽"。

> 硬件那个 per-port 选择器具体长什么样（是不是就是 index 1..8 这 16 个 field id，
> 还是一个独立寄存器）**离线看不到**，标 `推断`：我们用"index 1..8 的 field id 列表"
> 去喂它，是唯一能由厂商数据（`aclRamInit`）支撑的写法。§7 的实验就是验证这一步。
>
> **2026-10-31 更新：§4.4 的流水已经在真机上原样跑完（纯模块参数，没刷机），
> 结果是"分类器被唤醒、然后丢帧"的负结果；详细读数与解释见
> `docs/vendor-v6-entry-format-2026-10-31.md`。本文 §4.4 / §6 的方案因此不再需要重跑，
> 结论已经从"缺 provision"推进到"表项格式不对"（原厂 v6 表项 key 只有 3 个字，
> 且 flow=0、egress=0=PON 侧）。**

### 5.5 上一轮实验为什么必然全 miss（复核）

上一轮在真机上试过 rule 2/3/152/153/160 + idxw4 + 384 槽全灌，全部不命中。按本文：

- 这些组合的 idx 都是 9/10，而 lan 口进来时硬件用 3（lan1）/1（lan3）/2（lan2）——
  **键和槽都不同**，把条目灌满 384 槽也没用（槽是按 idx 9/10 的键算的，
  而硬件按 idx 3 算 slot；两个键不同，槽分布也不同）。
- rule 2/3（组 0）只有在 idx=10 时才和索引表自洽；rule 152/153 属于组 9，
  只有 idx=9 时自洽。**没有一次实验是"idx=端口值 + 同组规则"**。

---

## 6. 我们驱动要补什么（伪代码级，不改真源码）

改动集中在 `工作文件/08-源码与构建树/openwrt/package/kernel/kmod-e2631-ppe/src/zx279128s-ppe.c`。
下面只是方案，**本轮不动 C 源码**。

```c
/* 端口 -> extract index，照 tm.ko .rodata:0x1cd8 的 24 项表 */
static const u8 zx_ppe_port_ext[32] = {
	1, 2, 3, 4, 5,             /* port 0..4  */
	8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* port 5 = pon/CPU -> 8 */
	6, 6, 6, 6, 6, 6, 6, 6,    /* 0x10..0x17 */
	7, 7, 7, 7, 7, 7, 7, 7,    /* 0x18..0x1f */
};

/* 每个组 slot 8 一条 19 窗口 v6 规则；index 项 = aclRamInit 形状 + valid bit8 */
static int zx_ppe_v6_groups(struct zx_eth_adapter *a)
{
	for (int n = 1; n <= 8; n++) {
		u32 idx[5] = {0};
		/* word0..3：n<<4|0 .. n<<4|15，按小端字节打包 */
		for (int k = 0; k < 16; k++)
			idx[k / 4] |= (u32)(((n << 4) | k) & 0xff) << (8 * (k % 4));
		idx[4] = 0x00190000 | BIT(8);   /* ext_mode=2；index_valid slot 8 */
		if (zx_pp_write(a, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, n, idx, 5))
			return -EIO;
		if (zx_pp_write(a, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
				(n << 4) | 8, zx_ppe_v6_rule, 17))
			return -EIO;
	}
	return 0;
}

/* 流添加时：入口端口 -> idx -> 表项/键/槽 三处一致 */
static int zx_ppe_flow_add(...)
{
	int inport;            /* 现有代码在 META 分支里已经找到 i，只是丢掉了 */
	u8 ext = zx_ppe_port_ext[i];
	u16 rule = (u16)((ext << 4) | 8);
	...
	zx_ppe_slots(&t, slot);          /* t 里带 ext/rule，CRC 用它们 */
	words = zx_ppe_hash_entry(&t, flow, port, w);
	/* zx_ppe_hash_entry(): key[0] = 0x40|ext；bit23 字段 = rule */
}

/* v6 表初始化顺序：先 1..8 组，再 index 10（引擎位），最后 v4 rule/index */
zx_ppe_v6_groups(adapter);
zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, 10, zx_ppe_v6_index, 5);
```

要点：

1. `zx_ppe_v6_ext_idx` 这个模块参数（默认 10）对 v6 不再适用，应改成"由 inport 决定"；
   可保留一个调试参数强制覆盖（用来做 A/B）。
2. `zx_ppe_v6_rule_idx` 默认 0x99 必须按 `(ext<<4)|8` 生成；`ZX_PPE_V6_RULE 0x99`
   这个常量本身是错的（组 9），要么删掉、要么只留作 v4-down 兼容。
3. `zx_ppe_slots()` 用的 45 字节键必须包含 `0x40|ext` 和 `(ext<<4)|8`；现有的
   `zx_ppe_put_bits(key, 23, 8, rule)` 编码不用改，改的是传进去的 `rule`。
4. `zx_ppe_flow_add()` 里 `META` 分支已经有 `i`（`adapter->ports[i]->ifindex ==
   ingress_ifindex`，源码第 862–872 行），只要把它存进 `struct zx_ppe_tuple` 即可。
5. **v4 路径（idx 9、rule 152）一个字都不要改**。
6. 是否写 index 1..8 取决于 §7 的第一次 dump；如果板上已经有内容，也可以只补规则，
   但驱动里做一次幂等重写更省事（值固定，不会变坏）。

---

## 7. 真机验证步骤与判据

### 7.1 第 0 步：先 dump，不猜（只读，5 分钟）

```
echo 1 > /sys/module/zx279128s_ppe/parameters/dump
dmesg | grep cla-index
```

驱动 `zx_ppe_dump_tables()` 本来就打印 `cla-index[0..15]`（每项 5 字）。要看：

- `cla-index[1]..[8]` 是不是 `...0019xxxx` 形状、16 个 field id 是否等于 `n<<4..`。
- 如果 1..8 全是 0 或乱值 → 板上确实没有提取配置，§4.4 的 A 步必须写。
- 如果是 `00190100` 一类值 → bootloader/原厂已经写过，只补 B 步（规则）即可。

**判据**：`cla-index[9]` 应等于 `93929190 97969594 9b9a9998 9f9e9d9c 00150151`
（我们的 v4 项），`cla-index[10]` 应等于 `...00150154`（我们的 v6 引擎项）。这两个是
对照，证明 dump 通道本身是好的。

### 7.2 第 1 步：写 1..8，回读验证

按 §4.4 写 index 1..8 + rule 24/40/56/72/88/104/120/136，然后：

```
echo 1 > /sys/module/zx279128s_ppe/parameters/dump
dmesg | grep -E 'cla-index|cla-rule'
```

**判据**：

- `cla-index[n]` 逐字等于 §3.2 的第 n 行（word4 = `00190100`）；
- `cla-rule[56]`（index 3 / lan1 用的那条）逐字等于 §3.3 的 17 个字；
- 写之前和写之后都读一次，确认 9/10 没被顺手改掉。

### 7.3 第 2 步：看真实流的表项 byte16 与槽

在单向回环台上打一股 v6 流后，读 hash 表项：

```
# 驱动 dump mask bit1：两个 hash 库 + 每个占用槽的 aging
echo 2 > /sys/module/zx279128s_ppe/parameters/dump
dmesg | grep -i 'hash'
```

**判据**：如果流的入口是 lan1（port 2），表项 **byte16 必须是 `0x43`**（不是 `0x4a`），
且这条表项落在"用 idx=3 / rule=0x38 算出来的槽"上。可以用
`09-工具脚本/cla_slot.py` 的同一套 CRC 重算（把 idx/rule 换成 3/0x38）对上。
byte16 还是 `0x4a` 就说明驱动的 idx 侧没改对。

### 7.4 第 3 步：tx_submitted 判据（沿用第十一轮的台子）

流：`fd99::2:48211 -> fd88::2:9999`，UDP，64,000 包，跑两轮；同拓扑 IPv4 做对照。

1. 第 1 轮后记 `/sys/kernel/debug/92000000.ethernet/dma_state` 的 `tx_submitted = A`；
2. 第 2 轮后记 `B`；
3. **判据**：`B - A < 1000` 为命中（IPv4 对照 600 包约 98）；`≈64000` 为仍然全过 CPU。

同时必须看**远端计数**（发包主机自己收到多少）：远端非零才算"转发"，否则 `tx_submitted`
低只是"被丢了"（第九轮的教训）。aging 位不可用作判据。

**失败时的分流**（按顺序做，不要回到 CLA 哈希侧扫参数）：

1. byte16 是否已经是 `0x40|idx`？（不是 → 驱动 idx 侧没改对）
2. 槽是否用同一个 idx/rule 算的？（不是 → `zx_ppe_slots()` 的输入没改）
3. `cla-index[n]` / `cla-rule[(n<<4)|8]` 是否写进去了？（不是 → provision 没生效）
4. 以上都对仍 miss → §8 的"硬件选择器"假设不成立，需要换思路（不要再扫哈希键长/窗口）。

---

## 8. 未解决项

1. **硬件 per-port 选择器的实现**：它到底读不读 index 1..8 的 16 个 field id，还是另有
   寄存器/表，离线不可观测。本文的写法是"由 `aclRamInit` 的数据形状推出来的唯一候选"，
   `推断`。
2. **16 个 `extract_indexN` 字节的语义**：是"规则 id 列表"（本文采用的组模型，证据是
   v4 rule 152 = `0x98` 恰好在 index 9 的列表里且 `index_valid` bit8 置位），还是
   "字段 id"（厂商字段名直译）。两种读法在"index n 对应 n<<4..n<<4|15"这一点上一致，
   但"硬件是否按 index_valid 选组内规则"没有被直接观测。
3. **组内 slot 选 8 还是 0**：本文镜像 v4（slot 8）。如果 slot 8 在 v6 组里不工作，
   下一顺位是 slot 0（厂商 v4 用 144 = 9<<4|0）。这是唯一还需要 A/B 的一处。
4. **word4 的 `ext_mode`（1..8 是 2，9/10 是 1）到底控制什么**：只知道 `aclRamInit`
   对 1..8 写 2、对 9/10 写 1；我们照抄。改错会不会让提取停摆，未知。
5. **板上 1..8 现在的真实内容**：只能上板 dump（§7.1）。它决定 A 步要不要写。
6. **下行 v6 规则**：厂商 rule 3 与 rule 2 只差 word14 bit25（mem_ctrl）。本方案只写
   上行版；如果 WAN→LAN 方向也需要 fast path，要再写 `(n<<4)|8` 的 bit25 版本并验证
   硬件按方向选哪一条。
7. **wlan / pon 的 index 6/7/8**：本板 PPE 驱动不管 wlan；pon（port 5）不存在。本文把
   8 也 provision 了，但没有流量能验证它。
8. **index 10 的 `00150154` 与 per-port idx 的关系**：真机上 bit2 是 v6 引擎开关
   （第十轮），但它是否也参与提取（比如作为"默认组"），没有实验能分开；本方案保留它。

---

## 附：复现命令

```bash
cd 工作文件/08-源码与构建树/zte-e2631-openwrt/tools/vendor-emu
KO=/绝对路径/工作文件/03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs

# 端口映射表原始字节 + 解码
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py map

# aclRamInit 写下的 index 1..10 + rule 144/145/2/3（含 tm_acl_fast_init 的 RMW）
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py init

# 只跑 tm_acl_fast_init，用真机 dump 的 9/10 做 RMW 基线
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py init9

# index 1..8 + rule (n<<4)|8 的写入
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py prov

# 单个 index 的逐次物理访问（默认 n=3）
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py trace 3

# 同一 v6 流只改 inport：byte16 与槽
docker run --rm -v "$KO":/ko:ro -v "$PWD":/w -w /w \
  --entrypoint python3 e2631-uni:py312 t_extidx.py flow
```

脚本说明：`t_extidx.py` 只拦 `fpga_read_reg` / `fpga_write_reg` / `__memzero` /
`memset` / `printk`，其余（`aclRamInit`、`tm_acl_fast_init`、`cla_*`、`tmOnu*`）全部跑
真身。CLA 间接接口按 `(ram_id, index)` 建了一个最小表模型，所以读-改-写是真 RMW。
另外 `km.py` 不保证 `.bss` 为 0（实测 `g_aclRamInited` 读出来是 `0x7b6`），脚本里显式
清零，否则 `aclRamInit` 会直接返回"已初始化"。

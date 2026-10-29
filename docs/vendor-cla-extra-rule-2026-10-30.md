# `cla_set_extra_rule_table()` 到底写到哪儿 —— 第十轮（2026-10-30）

本文只回答一件事：**`tm.ko` 的 `cla_set_extra_rule_table()` 把什么写到了硬件的哪个位置，
以及它是不是我们 IPv6 快转缺的那一步。**

结论先给：**它不是缺口。它写的就是我们驱动已经在写的那张表（CLA 间接口 ram_id=1，
即 `ZX_CLA_EXTRA_RULE`），17 个字的内容和驱动里的 `zx_ppe_v6_rule` 逐字节相同；
并且 IPv6 的流添加路径根本不调它（IPv4 也不调），只有开机 init 调，而且 v4/v6 的规则
都是同一次 init 写下去的。**

复现命令（本目录）：

```
docker run --rm -v "$PWD:/w" \
  -v "<rootfs>/kmodule-unsquashfs:/ko:ro" -w /w \
  --entrypoint python3 e2631-uni:py312 t_v6extra.py all
```

---

## 1. 追查方法

### 1.1 先定位：符号、大小、被谁调

`tm.ko` 没有 strip，`.symtab` 直接给：

| 符号 | 节 | 地址（st_value） | 大小 |
|---|---|---|---|
| `cla_set_extra_rule_table` | `.text` | `0x46e8` | `0x958` = 2392 B |
| `cla_get_extra_rule_table` | `.text` | `0x5040` | `0x9e4` |
| `cla_set_indirect_rw_cmd` | `.text` | `0x44c` | 212 B |
| `cla_get_indirect_rw_status` | `.text` | `0x520` | 152 B |
| `cla_set_indirect_rw_data` | `.text` | `0x5b8` | 176 B |
| `tmOnuRegWrite` / `tmOnuRegRead` | `.text` | `0x3609c` / `0x36034` | 120 / 104 B |

调用者（扫 `.rel.text`，按函数区间归属）：

| 调用点 | 所在函数 | 说明 |
|---|---|---|
| `0x4ac38` `0x4ac50` `0x4ac8c` `0x4aca8` | `tm_acl_l2_fast_init` | L2 快转的规则（**不在本轮范围**） |
| `0x4e27c` `0x4e3c0` `0x4eee4` `0x4efbc` | `aclBuildHashAndAclRule` | 通用 ACL 构建器；快转路径不经过它（第三/九轮已证） |
| `0x515d8` `0x515f0` `0x51620` `0x51638` | `tm_acl_fast_init` | **开机 init**，v4 的 144/145 和 v6 的 2/3 都从这里下 |

`tm_acl_fast_init` 一次调用（`t_v6extra.py init`，实参捕获）写的东西按顺序是：

```
rule[144] rule[145] index[9] rule[2] rule[3] index[10]
```

也就是 **v4 和 v6 的规则是同一次 init 写的**，不存在"只在 v6 加"。

### 1.2 为什么以前的脚本看不到它写了什么

`flowadd.py` 的 `INTERCEPT` 里有 `cla_set_extra_rule_table`，`t_init.py`、`t_v6chain.py`、
`t_v6rule.py` 也一样。`km.py` 对 `intercept` 里的符号一律替换成 stub
（`mov r0,r0 ; bx lr`），所以**这个函数的函数体在之前所有轮次里一次都没执行过**，
大家看到的只是它的两个实参 `(rule_id, 68 字节缓冲区)`。这一轮把它从拦截列表里拿掉。

### 1.3 只拦真正的边界，地址从 fpga_* 反推

`t_v6extra.py` 里 `BOUNDARY = ('printk', '__memzero', 'memset',
'fpga_read_reg', 'fpga_write_reg')`：`cla_set_extra_rule_table`、
`cla_set_indirect_rw_cmd/_data`、`cla_get_indirect_rw_status`、`tmOnuRegWrite/Read`、
`tmOnuGlbRegValidation` 全部跑模块自己的代码，只在最后一级 `fpga_read_reg/fpga_write_reg`
上记流水。这一级的实参是"描述符字偏移"，换算规则前面几轮已经用两个真机锚点验证过：

```
phys = 0x92000000 + 4 * offset
```

于是每条记录都能直接写成"CLA 窗口 `0x9238c000` + 偏移"。

**踩到的坑（记下来，别人别再踩）**：不能把 `tmOnuGlbRegValidation` 也拦掉。
它是 `tm.ko` 自己的函数，除了校验还把"寄存器描述符地址"写到第 2 个实参指向的位置；
拦掉它（`t_init3.py` 的做法）以后 `tmOnuRegWrite/Read` 拿到 NULL 描述符，
所有偏移都算成 0，第一次跑出来的流水全是 `0x92000000`。这一条也是为什么之前
`t_init3.py` 只能打印"逻辑 reg 号"而给不出物理地址。

---

## 2. 被拦到的写动作

### 2.1 一次调用的完整形态

`cla_set_extra_rule_table(rule_id, buf)`（`buf` 是 68 字节 = 17 个字）静态结构：

```
0x46e8  r6 = rule_id, r4 = buf
0x4714  cmp r6, #0x9f ; bhi -> 打印 "input invalid parameter" 并返回 1
                                                    ← rule_id 合法范围 0..159
0x4730  cla_get_indirect_rw_status(&st) 最多 19 次，直到 st != 0（厂商是"等非 0"）
0x4788  cla_set_indirect_rw_cmd(rw=0, ram_id=1, ram_addr=rule_id)
0x47a4  for (i = 16; i >= 0; i--) cla_set_indirect_rw_data(i, buf[i]);
0x47c0  r0 == 0 ? 0 : -1
```

`cla_set_indirect_rw_cmd(rw, ram_id, ram_addr)` 的命令字（`0x4a8..0x4bc`）：

```
cmd = ram_addr | (ram_id << 22) | (rw << 27)     ; rw=1 是读
→ tmOnuRegWrite(reg 0, cmd, idx 0)
```

厂商自己的 `printk` 也是这么叫的（打开 `g_tm_debug_level=7` 后原文）：

```
[TM][cla_set_indirect_rw_cmd]input: ind_rw_en = 0, ram_id = 1, ram_addr = 2
```

### 2.2 rule 2 的逐次硬件访问（`t_v6extra.py trace`，共 37 次访问）

| # | 方向 | phys | CLA 内偏移 | 值 | 含义 |
|---|---|---|---|---|---|
| 00 | read | `0x9238c018` | `+0x18` | `0x00000001` | 间接状态字，bit0=1 放行 |
| 01 | read | `0x9238c014` | `+0x14` | `0x00000000` | 命令字（读-改-写里的"读"） |
| 02 | **write** | `0x9238c014` | `+0x14` | **`0x00400002`** | `ram_addr=2, ram_id=1` |
| 03 | read | `0x9238c05c` | `+0x5c` | `0` | 数据字 16（RMW 读） |
| 04 | **write** | `0x9238c05c` | `+0x5c` | `0x00db6db6` | 数据字 16 |
| 05 | read | `0x9238c058` | `+0x58` | `0` | 数据字 15 |
| 06 | **write** | `0x9238c058` | `+0x58` | `0xdb6db6db` | 数据字 15 |
| 07 | read | `0x9238c054` | `+0x54` | `0` | 数据字 14 |
| 08 | **write** | `0x9238c054` | `+0x54` | `0x01300000` | 数据字 14 |
| 09 | read | `0x9238c050` | `+0x50` | `0` | 数据字 13 |
| 10 | **write** | `0x9238c050` | `+0x50` | `0x0fffffff` | 数据字 13 |
| 11..26 | read+write | `0x9238c04c`..`0x9238c030` | `+0x4c`..`+0x30` | `0xffffffff` | 数据字 12..5 |
| 27/28 | read+**write** | `0x9238c02c` | `+0x2c` | `0xfff0000a` | 数据字 4 |
| 29/30 | read+**write** | `0x9238c028` | `+0x28` | `0x94264889` | 数据字 3 |
| 31/32 | read+**write** | `0x9238c024` | `+0x24` | `0x01e3868c` | 数据字 2 |
| 33/34 | read+**write** | `0x9238c020` | `+0x20` | `0x16284880` | 数据字 1 |
| 35/36 | read+**write** | `0x9238c01c` | `+0x1c` | `0xe1828406` | **数据字 0，最后写（落表）** |

rule 2 的 17 个字（与驱动 `zx_ppe_v6_rule` 逐字相同）：

```
w00 e1828406 16284880 01e3868c
w03 94264889 fff0000a ffffffff
w06 ffffffff ffffffff ffffffff
w09 ffffffff ffffffff ffffffff
w12 ffffffff 0fffffff 01300000
w15 db6db6db 00db6db6
```

### 2.3 四条规则的实际参数（`t_v6extra.py init`）

| 规则号 | 命令字 | 用途 | 与同族另一条的差别 |
|---|---|---|---|
| 144 (`0x90`) | `0x00400090` | IPv4 上行 | `w14 = 0x00700000` |
| 145 (`0x91`) | `0x00400091` | IPv4 下行 | `w14 = 0x02700000` |
| 2 | `0x00400002` | IPv6 上行 | `w14 = 0x01300000` |
| 3 | `0x00400003` | IPv6 下行 | `w14 = 0x03300000` |

同族两条**只差 `w14` 的 bit25**（下面 3.2 会说明它就是 `mem_ctrl`，即方向/动作位）。

---

## 3. 这些写动作的语义

### 3.1 哪张表

CLA 的间接访问只有三个寄存器（`tm.ko` 的 `claRegTable` 描述符，`phys = 0x92000000 + 4*base`）：

| reg | base | phys | mask / shift | 用途 |
|---|---|---|---|---|
| 0 | `0xe3005` | `0x9238c014` | `0x0fffffff` / 0 | 命令字 |
| 1 | `0xe3006` | `0x9238c018` | `0x00000001` / 0 | 状态 bit0（读到 1 放行） |
| 2 | `0xe3007`，stride 1，count 17 | `0x9238c01c`..`0x9238c05c` | `0xffffffff` / 0 | 数据字 0..16 |

`ram_id` 的含义就是驱动头文件里那套（`zx279128s-eth.h`）：

```
0 = ZX_CLA_EXTRA_INDEX   （extract index 表，5 个字）
1 = ZX_CLA_EXTRA_RULE    （规则表，17 个字）   ← 本文追的就是这个
2 = ZX_CLA_HASH0 (256 项) 3 = ZX_CLA_HASH1 (128 项) 8 = ZX_CLA_AGING
```

所以"extra rule 表"= **CLA ram_id 1，按 rule_id 寻址，0..159 项，每项 68 字节**。
它就在我们已经映射的那块 `0x92000000` 窗口里，不需要新的 ioremap，
也不需要新的访问路径——和 hash 表用的是同一个命令/状态/数据三元组。

### 3.2 一条 rule 项的字段布局（68 字节 = 544 位）

这是本轮附带产出的**完整位图**，来源有两条、互相印证：

1. `cla_set_extra_rule_table` 打开 `g_tm_debug_level=7` 时，厂商自己把 17 个字拆成
   `rule_id / winoffset0..19 / winmask0..19 / outport_mask / inport_mask /
   taglevel_mask / l2info_mask / pppoe_mask / no_match_act / rule_mode /
   hash_len / mem_ctrl / cvlan_mask / offset0..19_type` 打印出来（字段名就是它代码里的名字）；
2. 逐位扫描（`t_v6extra.py bits`，17×32 = 544 次，每次只点亮一位，看哪个打印字段变了）。

| 字段 | 位 | 宽度 | 说明 |
|---|---|---|---|
| `winoffset0..19` | `7n` (0..139) | 各 7 | 窗口 n 在 L3 头里的字节偏移 |
| `winmask0..19` | `140+16n` (140..459) | 各 16 | 窗口 n 的掩码 |
| `outport_mask` | 460 | 1 | 出端口掩码使能 |
| `inport_mask` | 461 | 1 | 入端口掩码使能 |
| `taglevel_mask` | 462 | 1 | tag 层数参与 |
| `l2info_mask` | 463 | 1 | L2 信息参与 |
| `pppoe_mask` | 464 | 1 | pppoe 参与 |
| `no_match_act` | 465..466 | 2 | 未命中动作 |
| `rule_mode` | 467 | 1 | 规则模式 |
| `hash_len` | 468..472 | 5 | **参与哈希的半字数**（v4=7，v6=19） |
| `mem_ctrl` | 473 | 1 | 厂商上下行两条规则只差这一位（推测是方向/动作选择，未单独验证） |
| `cvlan_mask` | 474 | 1 | cvlan |
| `offset0..19_type` | `480+3n` (480..539) | 各 3 | 窗口类型：2=IPv4，3=IPv6 |

和驱动现有编码完全一致（`zx_ppe_v6_tune()` 用的就是 `7n / 140+16n / 480+3n / 468`），
所以**驱动的编码器不需要改**。

### 3.3 厂商解码器对 rule 2 的判读

```
rule_id = 2
winoffset0..18 = 6,8,10,...,42        (19 个窗口)
winmask0 = 0xff00, winmask1..18 = 0xffff
outport/inport/taglevel/l2info/pppoe/no_match = 0
rule_mode = 0, hash_len = 19, mem_ctrl = 0
offset0..18_type = 3 (IPv6)
```

这就是驱动 `zx_ppe_v6_rule` 的原样，**逐字节相同**。顺带把驱动注释里那句
"当年只抓到 17 个字里的 16 个、少一个 0xffffffff" 结掉了：第 17 个字是
`0x00db6db6`，`w13 = 0x0fffffff` 不是缺失的掩码而是 `winmask18 = 0xffff`，
`w14 = 0x01300000` 是 `hash_len = 19`。驱动表里现在这版是对的，不需要"补回来"。

---

## 4. 驱动要补什么

### 4.1 结论：不用补

| 厂商动作 | 驱动里对应的东西 |
|---|---|
| status 轮询 `0x9238c018` bit0 | `zx_pp_wait(adapter, ZX_CLA_BASE)` |
| 写 `0x9238c014 = rule_id \| 1<<22` | `zx_pp_write()` 里 `writel(index \| table << 22, ...)`，`table = ZX_CLA_EXTRA_RULE = 1` |
| 数据字 16..0 写 `0x9238c01c + 4i` | `zx_pp_write()` 的 `for (i = words-1; i>=0; i--)`，CLA 的 `words = 17` |
| rule 2 的 17 个字 | `zx_ppe_v6_rule[]`，逐字节相同 |
| rule 144/145 的 16+1 个字 | `zx_ppe_v4_rule[]`，逐字节相同（第 17 字写 0） |
| `rule_id <= 0x9f` 的参数检查 | 驱动没有检查，但写的是 152/153，合法 |

唯一的差别是**规则号**：厂商 v6 用 2（上行）/3（下行），驱动用 153（`ZX_PPE_V6_RULE = 0x99`，
`v6_rule` 参数可改）。这一条真机上已经扫过（2 / 3 / 152 / 153 / 160，配合
`v6_idxw4 = 0x00150004 / 0x00150154`、384 槽全灌），全部 0 命中，见
`docs/known-issues.md` 的 finding 4。**所以"把规则表写对"不是缺口。**

### 4.2 如果将来还要按厂商方式写（伪代码，仅供对照，不建议现在改）

```c
/* 已经存在的调用，等价于厂商 rule 2： */
zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, /*rule_id=*/2,
            zx_ppe_v6_rule, 17);

/* 厂商的"下行"版本，与上行只差 mem_ctrl（全局 bit 473 = word14 bit25）： */
static u32 v6_rule_down[17];
memcpy(v6_rule_down, zx_ppe_v6_rule, sizeof(v6_rule_down));
v6_rule_down[14] |= 1u << 25;
zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, 3, v6_rule_down, 17);
```

要不要写第二条只取决于硬件是否按方向选规则；驱动现在两个方向共用一条、
而 v4 也是共用一条且能用，所以这更像对称性而不是缺口。

### 4.3 流添加路径：一张表都不写

`t_v6extra.py flow`（真身跑 `zte_api_fast_l3_session_add`，只拦边界）：

| 输入 | 返回值 | CLA 窗口写 | 备注 |
|---|---|---|---|
| IPv6（family 2，entry 里是 fd99/fd88） | 0 | **0 次**（只有两次读：`+0x90`、`+0x94`） | 只调 `cla_set_external_hash_table(0x263a, 0x13, buf)` |
| IPv4（family 2） | 0 | 34 次访问：命令 `0x00800077`（table 2, index 119）+ 15 个数据字（各一次读-改-写） | 内部 hash 表 |
| IPv6/IPv4 family 0/3 | -12 | — | 路径中途失败，和 `t_v6sess.py` 一致 |

两条结论：

1. **IPv6 的流添加不写规则表，也不写 index 表**（`cla_set_extra_rule_table` /
   `cla_set_extra_index_table` 一次都没进）；`EN_V6FAST_EXTIDXTBL` 这个符号在 `tm.ko`
   里**没有任何引用**（`.rel.*` 里搜不到，`__ksymtab` 也没有），是死代码。
2. 它写的是 `cla_set_external_hash_table()`，而那个函数**一个寄存器都不碰**：
   地址是算出来的，然后直接 `str`——

   ```
   dst = ((0x3c40000 + 0x800 + index) << 6)          ; 64 字节一项
   memcpy(dst+8, buf[0..0x3b], 52 字节);
   dst[0x17] |= 0x40;                                 ; 由 buf[0x10] 的 bit6 来
   ```

   本轮的 v6 一条流拿到的是 `index = 0x263a`、`arg1 = 0x13`（=19，窗口数）。
   这块窗口上一轮已经在真机上探过：virt `0xF1000000` 在厂商 iotable 里映射到
   phys `0x00000000`，实测读出来是 ARM 异常向量表 + 启动代码（6 个 64 KiB 间隔的
   地址逐字相同），不是表存储，见 `docs/known-issues.md` finding 5。**这条路也已经关掉。**

---

## 5. 下一步真机实验

### 5.1 先把这条线关掉（只读，5 分钟）

目的：证明"我们写进去的规则就是厂商那条"，而不是再猜。

1. 加载引擎（`ipv6=1` 是当前镜像默认，确认一下）：
   `cat /sys/module/zx279128s_ppe/parameters/ipv6`，或 `dmesg | grep -i ppe`；
2. 打开回读：`echo 1 > /sys/module/zx279128s_ppe/parameters/dump`
   （mask bit0 = `cla-index[0..15]` + 一批 `cla-rule[]`，规则号列表就是
   `0..8,16..19,144,145,152,153,160,161`，写死在 `zx_ppe_dump_tables()` 里）；
3. `dmesg` 里抓 `cla-rule[  2]`、`cla-rule[  3]`、`cla-rule[144]`、`cla-rule[145]`、
   `cla-rule[153]` 五行；
4. **判据**：`cla-rule[153]` 必须和本文 §2.2 的 17 个字逐字相同（它就是厂商的 rule 2）；
   `cla-rule[144]` 必须等于驱动 `zx_ppe_v4_rule` 的 16 个字 + 末尾 0。
   不相等才说明写坏了；相等则说明"规则内容/落点"这一项彻底结案。

### 5.2 如果还想再试一次 v6 命中（唯一没被排除的组合）

第九轮做过 rule 2 / 152 + `v6_idxw4=0x00150004/0x00150154` + 384 槽全灌，全 miss。
但那时 `v6_key` 是默认的 -1，也就是"跟着 `v6_rule` 走"（驱动里
`zx_ppe_v6_key_byte()` 的规则），所以关键字里那个字节是 `2` 或 `0x98`，
**从没显式设成厂商的 `0xa0`**。厂商 IPv6 上行流的关键字里那个字节是 `0xa0`
（= 10<<4，第二轮用 `t_poly.py` 跑出来的）。这两者从没一起上过真机。

```
# 单向回环台：LAN 主机 fd99::2 --- 路由器 --- fd88::2（同口回环）
insmod zx279128s-ppe.ko ipv6=1 v6_ext=10 v6_entry=10 \
       v6_rule=2 v6_key=0xa0 v6_idxw4=0x00150004
```

步骤：

1. 先跑一轮 64000 包（`iperf3 -u -b 100M -t 5 -c fd88::2 -p 9999 -B fd99::2`，
   或现有的 UDP 打流脚本），记 `dma_state` 里的 `tx_submitted = A`；
2. 停 5 s，再跑同样一轮，记 `tx_submitted = B`；
3. **判据**：`B - A`。硬件快转应该 < 1000（IPv4 同拓扑是 600 包只花 98）；
   仍然 ~64000 就是没命中（本轮预期就是这个结果）。

注意 `v6_rule=2`、`v6_key=0xa0` 这条组合的意义：它是**厂商那一对**（规则表第 2 项
+ 关键字 0xa0）。如果它也 miss，那么"规则表 / 关键字规则号"这条线可以彻底关掉。

> 不要再用 `rule 160`：`cla_set_extra_rule_table` 自己就拒绝 `rule_id > 0x9f`，
> 160 只能靠驱动绕过检查硬写——这本身就是"它不是厂商的用法"的证据。

### 5.3 真正的下一步不在 CLA

三轮的结论叠起来是：CLA 的 v6 表项、规则、index 都写对了，硬件仍然不命中；
而厂商 v6 流添加写的"外部表"在这块板上是启动 ROM。剩下的候选只有
"硬件到底用什么 key 做 v6 查找"（extract 侧）和"原厂 v6 单播快路其实在 NPU"。
前者在 mainline 里没有可观测手段，后者要把 `switch.ko` 的
`sw_acl_l3_hardfast_session_add` / `npu_drv_create_flow` 那一套搬过来（驱动核在
4.1 vmlinux 里）。在这之前，`ipv6=1` 的价值仍然只是 LAN-to-LAN 交换机快转。

---

## 6. 没搞清的、以及为什么

1. **厂商为什么写外部表**：在仿真里 `cla_set_external_hash_table` 是每次 v6 添加都调的，
   但这块板子的那个地址是启动 ROM。两种解释（另一颗 SoC 的遗留代码 / 本板走不到的兜底）
   都没有证据分开，需要 4.1 vmlinux 里调用链的上层来判断。
2. **外部表的 index 怎么算**：本轮只拿到一条流的值 `0x263a`。试过把同一次运行里
   45 字节 key、40 字节 `sip+dip+ports+proto` 缓冲喂给 `cla_list_hash_addr_gen()`
   （CRC-32，`0x71a0`），结果 `0xb6e80fb3` / `0x348633a7`，和 `0x263a` 对不上
   （低 14 位、取模都对不上）。要定下来得再逆一层，而这条路已经被真机排除，
   所以没有继续。
3. **关键字里的"规则号"对 v6 到底是什么**：v4 上 `9<<4 = 144` 正好等于规则表项号，
   v6 上 `10<<4 = 160` 却**超过本函数接受的 0x9f 上限**，而厂商 v6 用的是 2/3。
   所以 v6 的这个字节大概率不是"规则表项号"，而是"extract index 左移 4 位"或别的编码。
   这解释了为什么把 `v6_rule` 改成 2 也没用（关键字里仍然写着 153）。
4. **`mem_ctrl`（bit 473）在硬件里究竟做什么**：只知道厂商为上行/下行各写一条、
   这一位不同，没有真机实验能把它和"方向选择"直接对上。
5. **仿真本身的边界**：capture 模式下 `cla_get_extra_index_table` /
   `cla_get_extra_rule_table` 是桩（返回全 0），所以厂商那段"读-改-写"在仿真里是从 0
   开始的，`index[9]/[10]` 的**最终值**在板子上会和这里不同（板上有原厂内容）。
   本文对这些表只对"写下去的命令字和数据字"负责，最终值请以真机 dump 为准。

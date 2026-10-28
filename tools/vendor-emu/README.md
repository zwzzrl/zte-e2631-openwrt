# tm.ko / switch.ko under an ARM interpreter

Reading the vendor's classifier code is not enough to answer "what exactly does the
stock firmware hand the hardware"; these scripts run the code instead.  A 32-bit ARM
`.ko` is a relocatable object, so `km.py` lays its sections out at addresses of our
choosing, applies the relocations from its own `.rel.*` sections, and turns every
undefined (or deliberately intercepted) symbol into a stub whose first instruction is
hooked.  A handler does what the real function would do at that boundary - record a
table write, return a plausible hardware value, zero a buffer - and the emulation
continues.  `unicorn` executes everything else for real, which is why the loops and
multiplications in the vendor's bit packing come out right without being modelled.

    cd tools/vendor-emu
    docker build -t e2631-uni:py312 .
    docker run --rm \
      -v /path/to/vendor-rootfs/kmodule-unsquashfs/tm.ko:/ko/tm.ko:ro \
      -v "$PWD":/w -w /w --entrypoint python3 e2631-uni:py312 t_init.py tm_acl_fast_init

The image is `python:3.12-slim` plus `unicorn` and `pyelftools`.

## Scripts

| file | what it does |
| --- | --- |
| `km.py` | ELF layout, relocations, stubs, memory helpers, `run()` |
| `flowadd.py` | one run of the vendor's flow-add path with a synthetic flow descriptor; records every table setter |
| `t_init.py` | runs an initialiser (default `tm_acl_fast_init`) and prints the index/rule entries it writes |
| `t_tag.py` | tags each descriptor byte with its own offset, so the resulting hash key shows which descriptor byte landed where |
| `t_xh.py` | the same run with the extract index forced to 10 (IPv6), printing the hash entry, the external-hash call and the index re-patch |
| `t_init3.py` | runs `tm_pon_pp_cla_initial` (or any initialiser) and prints every CLA configuration call: `cla_set_config`'s 14-word struct, `cla_set_outspace_cfg`'s two arguments, the trap-enable, the polynomial config and every `fpga_write_reg` |
| `t_real3.py` | the flow-add path with the tables and the polynomial config filled with the values the hardware really holds (read back with the driver's `dump` parameter), so the vendor's read-modify-write of an index entry produces its real final value |
| `t_gfhr.py` | calls `tm_acl_get_fastHashRule()` on its own with a descriptor we control and prints, for every descriptor byte, which byte of the 60-byte hash rule it lands in (`map 3 5` sweeps IPv4 and IPv6) |
| `t_key.py` | the whole `tm_add_acl_flow_rule()` path with a hook on each key builder; `paths` shows which builder a type takes, `map` tags descriptor bytes, `key` prints the 45-byte key handed to `aclGetAvailableHashAddr()` |
| `t_v6win.py` | block-sweeps the descriptor and reports, for every buffer the run produces (entry, 45-byte key, 60-byte hash rule, external hash), which descriptor offset landed in which output byte |
| `t_v6path.py` | pokes every acl/fast function entry and prints the order they actually run, per rule type and fast flag |
| `t_v6tbl.py` | decodes the ONU register descriptors out of the ELF (`regs`) and emulates `sbrg_add_ipv6table()` (`call`) |
| `t_poly.py` | the flow-add path with `cla_get_hash_poly_config` answered as the hardware answers it (ONU register 12 = 0x00e400e4 -> 0,1,0,3,0,1,0,3). Without this the vendor code takes its "invalid parameter" fallback and every type comes out looking like IPv4 |

`flowadd.py` grew two hooks for that: `handlers=` replaces any intercepted call (the
`get_*` stubs were returning zeros, which made every read-modify-write a fiction) and
`pre_run=` installs breakpoint pokes before the first run.

`t_init.py` is the self-check: it must reproduce `tm_acl_fast_init`'s rule 144/145 and
rule 2/3 byte for byte, and rule 144 must stay identical to `zx_ppe_v4_rule` in the
driver, which is the pair that has been offloading IPv4 on hardware since v1.0.  If a
change to `km.py` breaks that agreement, the change is wrong, not the driver.

What the runs established is written up in `docs/known-issues.md` (PPE-IPV6-TCP,
finding 4, and the first two items under `next`).  The short version of the last
round: `cla_get_hash_poly_config` splits ONU register 12 into eight 2-bit fields
(0x00e400e4 -> 0,1,0,3,0,1,0,3); the vendor's CLA init writes rule 144/145 (IPv4)
and rule 2/3 (IPv6, identical to `zx_ppe_v6_rule`) plus index[9] word 4 = 1 and
index[10] word 4 = 4, and programmes CLA+0x80 = 0x00000e00, 0x94 = 8, 0x88/0x98 =
0x3fff/0x3fff3fff, 0xcc = 0; and `cla_set_hash_table` spans 520 slots over five
table ids (2..6), not the 384 over two that the driver assumed.

## 2026-10-29 第二轮：描述符 -> 表项、以及 key 里的规则号

`t_gfhr.py map 3 5`（零描述符，逐字节打 0xff）给出一张确定的映射表，**IPv4 与 IPv6
完全一样**：

| 描述符偏移 | 落到 60 字节 hash rule 的哪里 |
| --- | --- |
| `+0x10..+0x13` | 控制字 `e1/e2`，并且**打开窗口拷贝**：只有这几位非零时 `e19..e32` 才会被写 |
| `+0x18` | `e0` 的 `0x44 -> 0x47`（表项控制字的一部分） |
| `+0x62` | `e19`（协议） |
| `+0x64..+0x67` | `e22 e21 e24 e23`（源地址，每 16 位内部字节互换） |
| `+0x6c..+0x6f` | `e26 e25 e28 e27`（目的地址，同上） |
| `+0x74..+0x77` | `e29 e30 e31 e32`（端口，不互换） |

`t_poly.py`（喂真实多项式配置；IPv4 强制 index 9、IPv6 强制 10）在零描述符下跑出：

| 类型 | slot | 表项 `e16` | 45 字节 key 里 `e3` | 由此得到的 key 规则号 |
| --- | --- | --- | --- | --- |
| 3 (IPv4 上行) | 362 | `0x49` = `0x40\|9` | `0x48` | `0x90` = 144 |
| 4 (IPv4 下行) | 232 | `0x49`（`e13` = `0x20`） | `0x49` | `0x92` |
| 5 (IPv6 上行) | 246 | `0x4a` = `0x40\|10` | `0x50` | **`0xa0` = 160 = 10<<4** |
| 6 (IPv6 下行) | 183 | `0x40`（`e13` = `0x20`） | `0x01` | `0x02` |

两条结论，第二条是新的、也是目前唯一还没被真机排除的方向：

1. 表项里的 extract index 字节（`e16`）我们是**对的**：原厂 IPv6 就是 `0x4a`，和
   `zx_ppe_hash_entry()` 写的一样。
2. 45 字节 key 里的规则号不一样。原厂 IPv6 上行写 `0xa0`，也就是 extract index 左移
   4 位；而 `zx_ppe_slots()` 写的是 `0x99`（`v6_rule` 的默认值）。这条差异在 v4 上
   同样存在（原厂 `0x90` = 144，我们 `0x98` = 152）却没有挡住命中，所以不能直接判成
   v6 不命中的原因；能做的实验很明确：真机上把 `v6_key` 设成 `0xa0`（`zx_ppe_slots()`
   会把它写进 key 的 bit 23），再看同一股 v6 流的 aging bit 0 有没有置起来。

**还没解决的矛盾**（记下来，别再重复推）：喂了真实多项式配置之后，type 5 的表项窗口
仍然只有 v4 那 13 字节（`e19..e32`），描述符 `+0x00..+0x124` 里没有任何一段 IPv6
地址被读进窗口；而驱动写给硬件的 v6 表项是 19 个窗口 38 字节。两种读法都还没被排除：
(a) 原厂 v6 快表项确实只用 `/32` 前缀 + 端口做 key，地址靠规则表里的窗口掩码在硬件
侧比对；(b) `tm_acl_fast_add_v4v6()` 不是原厂 IPv6 快路径真正走的那条，地址是在
`switch.ko` 里装进另一个结构的。真机一回来就能用现有 `dump`/`v6_key` 两个参数分辨。

静态反汇编也核对过一遍，排除「这是仿真器的假象」这个可能：`tm_acl_fast_add_v4v6`
（`tm.ko` `.text` `0x4d2b4`，长 `0x7a4`）里 `0x4d380..0x4d600` 是一整块展开的打包
代码，写完整个表项，其中没有任何按协议族选择窗口数的分支 —— 唯一按族判断的是
`0x4d328`/`0x4d32c` 的 `subne #3` / `subeq #4`，它只决定方向（上行/下行）。所以
「软件侧窗口布局与协议族无关、由 extract index 决定」是代码本身的性质。



## 2026-10-29 第三轮：13 vs 19 窗口的矛盾解开了——是两条不同的 IPv6 通路

第二轮留下的矛盾是：喂了真实多项式配置之后，type 5（IPv6 上行）的 60 字节哈希表项
仍然只写 v4 那 13 字节窗口，而驱动写 19 窗口 38 字节。这一轮把它查成了**结论**。

三个新探针（都在本目录）：

| 探针 | 做了什么 |
|---|---|
| `t_v6win.py` | 描述符按 4 字节块打不同标记（0xd1..0xd4），跑 `tm_add_acl_flow_rule`，报**每一个输出缓冲区**（60 字节表项、45 字节 key、60 字节 hash rule、external hash）里每个字节来自描述符哪个偏移 |
| `t_v6path.py` | 在每个 acl/fast 函数入口下断点，打印真实执行顺序（类型 × fast 标志） |
| `t_v6tbl.py` | 从 ELF 重定位解出 `tmOnuRegWrite` 用的寄存器描述符表，并可仿真 `sbrg_add_ipv6table()` |

### 结论 1：`tm_acl_fast_add_v4v6` 就是 IPv6 也走的唯一构建器，但它没有 v6 窗口

`t_v6path.py trace`（类型 3/5/6，fast=1 和 fast=0 各跑一遍）：

```
type 3 fast 1 idx 9  -> order tm_acl_fast_add > tm_acl_fast_add_v4v6 > aclGetExtIdxRuleRamAddrByPort > tm_acl_get_fastHashRule
type 5 fast 1 idx 10 -> order tm_acl_fast_add > tm_acl_fast_add_v4v6 > aclGetExtIdxRuleRamAddrByPort > tm_acl_get_fastHashRule
type 6 fast 1 idx 10 -> order tm_acl_fast_add > tm_acl_fast_add_v4v6 > aclGetExtIdxRuleRamAddrByPort > tm_acl_get_fastHashRule
```

`addAclRule` / `aclBuildHashAndAclRule` **一次都没进**，`fast` 全局标志也换不了路径。
`t_v6win.py 3` 与 `t_v6win.py 5` 的块扫描给出**逐字节相同**的映射（只有表项的 e16 与方向位
不同）：

| 描述符偏移 | type 3 落点 | type 5 落点 |
|---|---|---|
| `+0x62` | 表项 e19 | 表项 e19 |
| `+0x64..+0x67` | e22 e21 e24 e23 | **同上** |
| `+0x6c..+0x6f` | e26 e25 e28 e27 | **同上** |
| `+0x74..+0x77` | e29 e30 e31 e32 | **同上** |
| `+0x00..0x124` 其余 | 不进任何输出 | 不进任何输出 |

也就是说：这条路径给 IPv6 的表项里**只有 13 字节窗口**，v6 地址的其余部分它根本不读
（描述符里没有它读的地方）。所以第二轮的两个候选里，**(a)「原厂 v6 快表项只用前缀+端口做
key、地址靠规则掩码比」不是原厂的设计**——它是这条 ACL/哈希路径的固有形状，而不是 v6 加速。

### 结论 2：原厂的 IPv6 加速是另一张表，tm.ko 里叫 ipv6table

`tm.ko` 里有一套跟 ACL 哈希完全无关的 IPv6 机制，之前没被列进候选：

| 符号 | 含义 |
|---|---|
| `tm_ipv6table_add_set` / `_delete_set` / `_lookup_get` / `_print_get` | 对外 API（`__ksymtab` 导出） |
| `dev_attr_v6Table` | 用户态入口（vendor userland 往里写条目） |
| `sbrg_add_ipv6table` (`tm.ko` `0xf004`) / `sbrg_delete_ipv6table` / `sbrg_lookup_ipv6table` | 交换芯片侧实现 |
| `g_ipv6FastEn` / `g_fast_entry_v6` / `g_v6_total_entry` / `g_IPv6_Snat_Ram_Length` / `g_IPv6_Dnat_Ram_Length` / `g_nptv6_sipv6` / `g_nptv6_dipv6` | 这套机制的全局状态 |

`sbrg_add_ipv6table` 的写入协议（静态反汇编 + 描述符解码，`0xf004..0xf8d0`）：

* 4 轮「间接读-改-写」：每轮 `sbrg_get/set_indreg_wr_cfg(i, ...)` 之后对 **ONU 寄存器 78/77/76**
  各读或写一个字 —— 4 × 3 × 4 B = **表项 48 字节**，不是我们的 60 字节哈希表项。
* 这三个寄存器在 `tm.ko` 的寄存器描述符表里是连续字节地址：

  | 寄存器 | 描述符 base | mask | shift | 备注 |
  |---|---|---|---|---|
  | 76 | `0x000e2007` | `0xffffffff` | 0 | 数据字 |
  | 77 | `0x000e2008` | `0xffffffff` | 0 | 数据字 |
  | 78 | `0x000e2009` | `0xffffffff` | 0 | 数据字 |
  | 25 | `0x000e2060` | `0x00000001` | 2 | 访问前读的状态位 |

  （描述符 28 字节一条，`tmOnuGlbRegValidation` 按 `table + 28*reg` 索引、entry[0] 自校验
  寄存器号；表在 `.data` 里，由 `0xf3f4` 处的 `R_ARM_MOVW_ABS_NC` 重定位指向。）

* 入口 `tm_ipv6table_add_set`（`0x3bfcc`）在 `0x3c078` 调 `sbrg_add_ipv6table`，之前校验
  `[rec+0xc] < 0x1000`、`[rec+0x20] <= 0xff`，并对记录里 0/1/2/4/8/12/16/20/24/28/32/36/40
  这些偏移取字段。

### 这对我们的驱动意味着什么

第二次那种「在 CLA 哈希表里换 `v6_key` / `v6_hwn` / `v6_type` / `v6_idxw4` / shotgun」的扫法
**结构上不可能命中**：原厂给 IPv6 转发用的根本不是那张表。真机上所有组合 0 命中、而
`tx_submitted`/远端的计数证明帧确实被转发了 —— 与这个结论一致。

下一步（都还不需要真机）：

1. 解 `sbrg_set_indreg_cmd` / `sbrg_hash`，拿到这张表的 table id、索引位宽和 start/end 序列；
2. 给 `sbrg_add_ipv6table` 补上 `sbrg_get_table_sel` / `sbrg_hash` 的真实语义后完整仿真，
   让它自己吐出一条 48 字节表项，再和 `g_IPv6_*_Ram_Length` 之类的边界校验对上；
3. 把 ONU 地址 `0xe2007..0xe2009` 换算成物理地址（我们已知 CLA 窗口在 `0x9238c000`，
   转换规则还差一步），确认这张表在主线 Linux 里可读写。

### 结论 3：ONU 地址换算规则（`fpga_read_reg` 那条链）

`tm.ko` 里的「寄存器号」最终变成什么地址，链条只有两段：

1. `tmOnuRegWrite(reg, val, idx, table)` 先按 28 字节描述符算出 **描述符字偏移**
   `off = base + stride * idx`（`0x360d0..0x360dc`：`ldr r2,[desc+0x14]; ldr r3,[desc+8];
   mla r4, r2, r4, r3`，其中 r4 是第 3 个实参，不是寄存器号）；
2. `zx_ponreg.ko` 的 `fpga_read_reg/fpga_write_reg`（`0x0`/`0x14`，各 20 字节）做的就是
   `ldr/str [g + 4*off]`，而 `fpga_module_init` 往这个全局里存的是 **`0xF4000000`**
   （`0x198: mov lr, #-201326592` → `0xF4000000`）。按厂商 iotable，virt `0xF4000000`
   ← **phys `0x92000000`**（switch，4 MiB）。

于是：

```
phys = 0x92000000 + 4 * (descriptor.base + descriptor.stride * idx)
```

**两个已经在我们驱动上真机验证过的锚点**（都读过回值）：

| 描述符 | base / stride | 算出 phys | 对应 |
|---|---|---|---|
| `claRegTable` reg 12 | `0xe3024` / 0，mask `0x00ff00ff` | `0x9238c090` | CLA+0x90 多项式配置（我们读回 `0x00e400e4`） |
| `claRegTable` reg 19 | `0xe3028` / 1 | `0x9238c0a0 + 4*idx` | CLA+0xa0 local6 四个字（我们写过并读回） |

### 结论 4：原厂 IPv6 表的访问口就在我们已经映射的那块 switch 里

同一个公式用在 `sbragRegTable`（`sbrag` = switch 寄存器组，88 条描述符）上：

| 用途 | 描述符（reg） | base / stride / mask | **物理地址** |
|---|---|---|---|
| 间接命令寄存器 | 19 | `0xe2005` / 0，mask `0x8fc00fff` | **`0x92388014`** |
| 表项数据字 0..2 | 22（索引式）/ 76 / 77 / 78 | `0xe2007` / 1，`0xe2007/8/9` / 0 | **`0x9238801c` / `0x92388020` / `0x92388024`** |
| 状态位 0..5 | 23..28 | `0xe2060` / 0，mask 1，shift 0..5 | **`0x92388180`** |
| 另两个位域 | 29 / 30 | `0xe2061` mask 3；`0xe2062` | `0x92388184` / `0x92388188` |
| 表访问配置字段 | 1..21 | `0xe2001..0xe2006` | `0x92388004..0x92388018` |

命令字编码由 `sbrg_set_indreg_cmd`（`tm.ko` `0xc694`）给出：

```
cmd = (rw << 31) | (start_stop << 27) | (table << 22) | (index & 0xfff)
约束：table <= 4，index < 0x1000，rw/start_stop 只能是 0/1（越界直接返回错误）
```

跟我们驱动自己在 CLA 上用的 `index | table << 22` 是**同一套编码**，只是块不同
（CLA 的命令口是 `0x38c014`，这张 v6 表是 `0x388014`）。

**这意味着不需要新的 ioremap**：`0x92388014/0x9238801c..24/0x92388180` 都落在
`0x92000000` 起 4 MiB 的 switch 窗口内，而驱动的 `adapter->base` 就是这块的映射，
读一个表项就是 `writel(cmd, base + 0x388014)` + 轮询 `base + 0x388180` +
`readl(base + 0x38801c/20/24)`。这条结论只在厂商代码层面成立，**板上是否真的实现这几个
偏移还没测**（盒子等串口），所以先记成"可访问性待测"，不当成已验证。

### 接下来（仍离线）

1. ~~解 48 字节表项的字段布局~~ —— 第四轮做完，见下。
2. 把 48 字节表项与 `g_IPv6_Snat_Ram_Length` / `g_IPv6_Dnat_Ram_Length` / `g_v6_total_entry`
   的边界校验对上，确认 SNAT/DNAT 两个 RAM 的分区。
3. 表项字段定了，再决定是「沿用 mainline `nf_flow_table` 控制面、把 v6 流写成这张表」
   （与 v4 的 PPE 路径并列），还是另起一条。

---

## 2026-10-29 第四轮：48 字节表项的字段布局全部解出，并用随机记录复验

第三轮留下的两件事——"表项里哪个字段是 sip、哪个是 dip"和"w8/w11 到底是什么"——这一轮
都解开了，而且不是猜的：先让厂商代码自己把字段名打出来，再用逐字节/逐位的实验对上表项，
最后写了一个 20 行的组装函数，拿它的输出和仿真器的输出逐字比对。

### 手段 1：打开厂商自己的调试打印，字段名是它打的

`tm.ko` 里 `sbrg_add_ipv6table` 有一组 `printk`：

```
[TM][sbrg_add_ipv6table]input:vlan_id = %d
[TM][sbrg_add_ipv6table]input:dip = %x:%x:%x:%x
[TM][sbrg_add_ipv6table]input:sip = %x:%x:%x:%x
[TM][sbrg_add_ipv6table]input:valid = %d, / mode / age / pt_mask
```

这些打印被全局 `g_tm_debug_level`（`0x100b43c8`，初值 1）挡住，`cmp r3,#6` 才走。
在仿真器里把它写成 7，打印就出来了（`t_v6entry.py prints`），`printk` 的实参直接给出
每个字段的值。把记录的一个字节置 1、其它置 0，看哪个字段的实参跟着变，就得到**输入记录**
的布局——这些偏移同时被开头的参数校验逐条印证（`ldr r3,[r0,#12]; cmp r3,#0x1000` 等）：

| 偏移 | 类型 | 字段 | 校验 |
|---|---|---|---|
| `0x00` | u32 | `valid` | ≤ 1 |
| `0x04` | u32 | `age` | ≤ 1 |
| `0x08` | u32 | `mode` | ≤ 1 |
| `0x0c` | u32 | `vlan_id` | < `0x1000` |
| `0x10` | u8[16] | `dip`（网络序） | — |
| `0x20` | u32 | `pt_mask` | ≤ `0xff`（高 3 字节必须为 0） |
| `0x24` | u8[16] | `sip`（网络序） | — |

合计 `0x34` = 52 字节，与 `tm_ipv6table_add_set` 里 `malloc(0x34)` 的大小一致。

### 手段 2：逐位扫描，把 12 个输出字逐位对上

`t_v6entry.py` 的位级扫描（dip/sip 各 128 位，每次只置一位，共 256 次）：

| 输出字 | 内容 |
|---|---|
| `w0` | `[0]` valid，`[1]` age，`[2]` mode，`[14:3]` vlan_id，`[31:15]` dip 第 64..80 位 |
| `w1` | dip 第 81..112 位 |
| `w2` | `[14:0]` dip 第 113..127 位，`[31:15]` 恒 0 |
| `w3` | `[7:0]` pt_mask，`[31:8]` dip 第 0..23 位 |
| `w4` | dip 第 24..55 位 |
| `w5` | `[7:0]` dip 第 56..63 位，`[31:8]` 恒 0 |
| `w6` | sip 第 64..95 位 |
| `w7` | sip 第 96..127 位 |
| `w8` | **恒 0** |
| `w9` | sip 第 0..31 位 |
| `w10` | sip 第 32..63 位 |
| `w11` | **恒 0** |

其中"dip 第 n 位"= 记录 `0x10 + n/8` 那个字节的第 `n%8` 位。换个说法：表项是一条 384 位的
小端位流，`bit0..2` 是三个标志、`bit3..14` vlan、`bit15..78` 是 dip 的**后 8 字节**（小端 u64）、
`bit96..103` 是 pt_mask、`bit104..167` 是 dip 的**前 8 字节**、`bit192..255` 是 sip 的后 8 字节、
`bit288..351` 是 sip 的前 8 字节，其余全 0。两个地址都是"后 8 字节在前、前 8 字节在后"。

### 手段 3：写一个组装函数，和仿真器对 60 条随机记录

`t_v6entry.py layout 60` 里 `build()` 就是上面那张表（20 行），对随机记录（合法范围内
随机 valid/age/mode/vlan/pt_mask 和两个地址）与仿真器输出逐字比对：**60/60 一致**。
这比"扫一遍看着像"强得多——它就是可以直接搬到驱动里的表项构造函数。

### w8/w11 之谜：不是读回保留，是硬写 0

反汇编那 12 个字进栈的片段（`0xf20c` 起）：

```
f20c: mov r2, #0
f25c: str r2, [sp, #0x88]      @ w8
f260: str r2, [sp, #0x94]      @ w11
```

两个 `str` 用的是同一个刚清零的 `r2`。所以**读-改-写里"读"那一步读回来的旧表项对写出去
的内容没有任何影响**——`readmap` 实验（给 48 次读各返回一个可辨识标记）也印证：12 个字一个
都没变。这条要记住：我们自己写这条表项时，w8/w11 就写 0。

### 访问协议（这一轮从反汇编定下来的）

`t_v6entry.py raw` 把两个 cfg 包装函数换成真实代码后，序列变成：读 reg 20 → 发命令 →
读 reg 20 → ……于是把 `sbrg_access_timeout`、`sbrg_set_indreg_cmd` 也反汇编了：

1. `sbrg_access_timeout()`（`0xc600`）：最多循环 9 次读 **ONU 寄存器 20**（`0x92388018`
   bit0），**读到非 0 才返回 0**；10 次都是 0 就返回 -1 并打 timeout。注意是"等非 0"，
   不是"等为 0"。
2. `sbrg_set_indreg_cmd(rw, start_stop, table, index)`（`0xc694`）：
   `cmd = index + (table << 22) + (start_stop << 27) + (rw << 31)`；校验 `rw≤1`、
   `start_stop≤1`、`table≤4`、`index<0x1000`，越界返回 1；然后
   `tmOnuRegWrite(19, cmd)`（→ `0x92388014`，mask `0x8fc00fff`），写失败返回 -1。
   函数内部**不做**完成轮询，轮询在调用者那边。
3. 两个包装：`sbrg_get_indreg_wr_cfg(a,b)` = 先 `access_timeout` 再 `cmd(rw=0, start=1,
   table=a, index=b)`；`sbrg_set_indreg_wr_cfg(a,b)` = 先 `access_timeout` 再
   `cmd(rw=0, start=0, table=a, index=b)`。返回值是两个结果按位或。
4. 数据字仍是 `tmOnuRegWrite(78/77/76)` → `0x9238801c/0x20/0x24`，每轮 3 个。
5. 读阶段是 4 张 table × 4 个块 × 3 字 = 48 次读（`table=0..3`，块 `0x248..0x24b`），
   写阶段只写 `table=0` 的同一组块。

### 还没定的（下一轮）

- 表项**槽号**从哪来：写阶段用的是固定 `table=0`、块 `0x248..0x24b`，槽号应该是另一条
  路径写进某个寄存器（`sbrg_hash` 的真实算法、或 reg 22 的索引式写法）。
- 4 张 table 各是什么（SNAT/DNAT/组播…），以及 `g_IPv6_{Snat,Dnat}_Ram_Length` 的边界。
- 这两条都卡在同一个点上：仿真里 reg 20 必须返回非 0 才能让 `access_timeout` 通过，
  而它的真实语义（哪一位表示"可访问"）只能到板上读。真机一回来先打印一次 reg 20 和
  reg 19 的复位值，这条链就能接下去。

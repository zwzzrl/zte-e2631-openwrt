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

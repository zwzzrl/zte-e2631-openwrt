# 原厂 IPv6 卸载的写入序列（ACL L3 hardfast / sbrg IPv6 表）—— 离线反汇编

> 对象：`switch.ko`（224808 B）与 `tm.ko`（1270520 B），
> 取自 `03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/`（原厂 kmodule.img 解开）。
> 方法：`arm-openwrt-linux-objdump -dr`，靠重定位拿到调用符号名。
> 本文除显式标「推断」的段落外，都是指令级证据。
> 反汇编全文留在 `/tmp/sbrg_add.txt`（685 行，可复查）。

---

## 0. 一句话结论

原厂给 IPv6 写硬件表，是**四步**：算哈希 → 选槽 → 探空 → 写 12 个字；
落到寄存器上只有四个：**19（间接命令）、20（忙）、76/77/78（三个数据字）、29（表选择）**。
我们驱动里这四步**已经全部对上了**（含 CRC 与槽位公式），
**唯一对不上的是那 12 个字的打包方式**——我们用的是从组播/MLD 代码反推的布局。

---

## 1. 调用链（指令级）

```
netfilter flowtable 卸载
  └─ switch.ko  sw_acl_l3_hardfast_session_add(desc)     0x28e0, 0x684 B
       ├─ TO_WAN(desc->dev @+24)                          → 方向位
       ├─ sw_acl_setMtchInfo(desc, stack_desc)            0x1e24
       ├─ get_sw_port_from_devname()                      端口号
       └─ zte_api_fast_l3_session_add(...)          ← 真正的表操作在 tm.ko
            └─ tm.ko  sbrg_add_ipv6table(t)        0xf004, 0x8cc B
                 ├─ sbrg_get_table_sel(&sel)       读 ONU 寄存器 29
                 ├─ sbrg_get_multi_vlan_mode(&m)   读（mode==0 时用 vlan）
                 ├─ sbrg_hash(key, unused, 0x10c)  10 位哈希
                 ├─ sbrg_get_indreg_wr_cfg(sel, addr)   探空（读 76/77/78）
                 ├─ sbrg_set_indreg_wr_cfg(sel, addr)   写前设地址
                 └─ tmOnuRegWrite(78→77→76)            每块 3 字，共 4 块
```

`hf_set_l3_entry`（0x2f64）本身是薄壳：调 `sw_acl_l3_hardfast_session_add`，
返回 0 就返回 1，否则按 `g_switch_debug_level` 打日志。
`hf_del_l3_entry`（0x3040）同理调 `sw_acl_l3_hardfast_session_del`。
`sw_acl_l3_hardfast_session_add` 的 22 个调用里 **14 个是 printk**，
真正的硬件动作只有 1 个：`zte_api_fast_l3_session_add`。

## 2. 寄存器协议（完整位域）

### 2.1 寄存器 19 = 间接访问命令（`sbrg_set_indreg_cmd`, 0xc694）

```
r5 = addr + (sel << 22) + (rw << 27) + (start << 31)
tmOnuRegWrite(19, r5)
```

* `addr`：**必须 < 0x1000**（`cmp r5, #4096`，否则返回错误 1）
* `sel`：`< 4`（`cmp r8, #4`）
* `rw`：`0 = 写`、`1 = 读`（`cmp r7, #1`）
* `start`：两个调用点都传 0，恒为 0

调用点：
* `sbrg_set_indreg_wr_cfg(sel, addr)` → `set_indreg_cmd(0, 0, sel, addr)` → **rw=0 写**
* `sbrg_get_indreg_wr_cfg(sel, addr)` → `set_indreg_cmd(0, 1, sel, addr)` → **rw=1 读**

### 2.2 寄存器 20 = 忙标志（`sbrg_access_timeout`, 0xc600）

```
for (i = 0; i < 9; i++) {         ← 注意：只有 9 次，不是 64
        read reg 20
        if (value == 0) break;
}
i > 9 → 超时，返回 -1
```
即 **读到 0 才算空闲**。

### 2.3 寄存器 76/77/78 = 三个数据字（一条 12 字节表项 = 3 字）

* **写序：78 → 77 → 76**（先写高字，最后写低字）
* 读序：76、77、78，三个值 **OR 起来为 0** 才判"空槽"

### 2.4 寄存器 29 = table_sel（`sbrg_set_table_sel`, 0xacb0）

* 取值 **0..3**，超范围打日志并返回 1
* `sbrg_add_ipv6table` 里**只接受 1 或 2**：
  `cmp r2,#0; cmpne r2,#3` → 命中就报错返回 −1。
  即 IPv6 表的 `table_sel` 是 1 或 2，0 和 3 会被原厂自己拒掉。

## 3. 槽位与哈希

### 3.1 槽位公式（`sbrg_add_ipv6table` @0xf184）

```
sel == 1 :  addr_base = ((hash & 0x7f) << 2) | 0x200
sel != 1 :  addr_base = ((hash << 2) & 0xff) | 0x300
block b (0..3):  addr = addr_base | b
```

### 3.2 哈希（`sbrg_hash`, 0xc4b8；type=0x10c 分支 @0xc554）

* 签名 `sbrg_hash(key, unused, type)`；`type` 是**类型选择**，不是多项式：
  0x4c / 0x10c / 0x3c 各一条路径（IPv6 单播走 0x10c）
* 做法：把 key 的**前 34 字节按字节倒序**拷进栈缓冲，再跑表驱动 CRC-16/CCITT
  （初值 0，`crc = tab[(b ^ (crc>>8)) & 0xff] ^ (crc<<8)`）
* 结果 **取低 10 位**（`ubfx r0, r0, #0, #10`）
* 34 字节 = sip(16) + dip(16) + vlan(2)，与 `key` 的布局一致

> 我们的 `zx_ppe_sbrg_hash()` 与此**逐位一致**（含倒序、初值、0x1021 表、低 10 位）。

## 4. 表项的 12 个字怎么来

`sw_acl_l3_hardfast_session_add` 先在栈上清零 168 字节（`__memzero(sp+0x30, 0xa8)`），
把描述符填进去交给 tm.ko；`sbrg_add_ipv6table` 再从入参 `fp`（描述符）取字段，
在栈 `sp+0x68 .. sp+0x94` 拼出 12 个字，然后以 `sp+0x70` 为起点、**stride 12 字节**写 4 块。

打包代码（0xf1b0–0xf258）用到的描述符字段与位宽：

| 描述符字段 | 用法 |
|---|---|
| `fp[0]` | 取 **1 bit**（`and r1, r1, #1`）→ 进第 0 块 |
| `fp[4]` | 取 **1 bit**（`lsl #1; and #2`） |
| `fp[8]` | 取 **1 bit**（`lsl #2; and #4`） |
| `fp[12]` | 低 12 位（`ubfx #0,#12`）→ mult-vlan-mode==0 时当 vlan |
| `fp[16..28]` | 4 个字 = 一个 16 字节 IPv6 地址 |
| `fp[32]` | 与 `r6` 拼（`orr r0, ip, r6, lsl #8`） |
| `fp[36]` | 进入 `r9`（后面当写表的 `sel` 用，实际被置 0，见 §5） |
| `fp[40]`, `fp[44]`, `fp[48]` | 参与寻址/2 号地址 |
| `fp[85]`(0x55), `fp[81]`(0x51), `fp[88]`(0x58), `fp[92]`(0x5c) | switch.ko 侧填的端口/标志 |

第 0 块的低字里有 `valid/age/mode` 之类的位（`sp+0x68` 的构造：
`(fp[0]&1) | (r4<<15) | ((fp[4]&2) | (fp[8]&4))`），
与我们按 MLD 代码推出的 "bits 0..2 = valid/age/mode" 精神一致，但**字段位置不同**。

## 5. 已排除的假设：不是"写错存储体"

读代码第一遍容易得出"原厂把表项写进 4 个存储体（sel 0..3），我们只写 table 0，
所以查不到"——**这个结论是错的**，别再去追：

* `sp[16]` 在 0xf244 被初始化成 **4**（`mov r3,#4; str r3,[sp,#16]`）；
* 写循环前（0xf3a4）`sp[16] = (sp[16] == 4) ? r9 : sp[16]`，而 `r9` 在 0xf234 被
  `mov r9, r2` 置为 **0**；
* 写循环（0xf3d8）把 `sp[16]`（= 0）当 `sel`，`r5`（0..3）当块号。

所以原厂**写入时 sel 就是 0**，探测时（0xf27c `mov r0, r9`，r9=0）也是 0。
我们驱动"表项写 table 0、探测读四个表"与原厂一致。

## 6. 我们驱动现有实现 vs 原厂（逐项）

| 环节 | 原厂 | 我们的 `zx_ppe_sbrg_*` | 判定 |
|---|---|---|---|
| 间接命令打包 | `addr \| sel<<22 \| rw<<27 \| start<<31`，reg 19 | `index \| table<<22 \| ZX_SBRG_START` | **一致** |
| 忙等待 | reg 20 读到 0，最多 9 次 | `zx_pp_wait()` / `readl(...IND_STATUS) & 1`，最多 64 次 | 一致（次数不同，无关） |
| 数据字 | 76/77/78，写序 78→77→76 | `ZX_SBRG_WORDS=3`，写时 `for (i=n-1;i>=0;i--)` | **一致** |
| 表项块数 | 4 块 × 3 字 = 12 字 = 48 B | `ZX_SBRG_ENTRY_WORDS=12`，`index+b`，`&words[3*b]` | **一致** |
| 写入的 sel | 0 | 0 | **一致** |
| 探空 | 读 76/77/78，OR==0 为可写 | 读四个表（值不用） | 行为不完全一致，但不影响命中 |
| 哈希 | 34 B 倒序 + CRC-16/CCITT + 低 10 位 | 同 | **一致** |
| 槽位 | `sel==1` 用 `0x200`，否则 `0x300` | `zx_ppe_sbrg_slot()` 同款 | **一致** |
| table_sel 前置条件 | reg 29 必须是 1 或 2，否则拒写 | 未检查 | 缺（应补） |
| **12 字的字段布局** | 由描述符 `fp[0..48]` 打包（单播专用） | 从 **MLD/组播**路径反推（"bits 0..2 valid/age/mode，3..14 vlan，15..78 dip…"） | **不一致 ← 最可疑** |

## 7. 由此得到的下一步（按性价比排序）

1. **把单播表项的布局按 §4 重推一遍**，而不是沿用 MLD 的。
   具体做法：在 `tools/vendor-emu/` 里按 `fp` 字段造记录，
   用 §4 的位运算生成 12 个字，与 `sbrg_add_ipv6table` 的
   实际输出逐字对比（`t_v6entry.py` 已有骨架，需要换成本文这套字段）。
2. **补 `table_sel` 检查**：写 IPv6 表前先读 reg 29，若不是 1/2 就拒绝并记日志。
   这条能防止"表写进去但引擎按另一张表查"的整类故障，也是最便宜的证伪手段。
3. 若 1 做完仍不命中，下一个怀疑对象是**转发引擎侧的 IPv6 查表使能位**
   （`switch.ko` 的 `sw_set_default_protocol_rule`，0x3e24，0x2c0 B），
   以及 `npu_cap` 里那张 (入端口组, 出端口组) 白名单——我们还没读过这两处的实现。

## 8. 附：怎么复查

```sh
OD=工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-objdump
K=03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs

# 一张表看全：谁调谁
$OD -dr $K/tm.ko | sed -n '/<sbrg_add_ipv6table>:/,/^$/p' | grep -A1 R_ARM_CALL

# 寄存器协议
$OD -dr --start-address=0xc694 --stop-address=0xc720 $K/tm.ko   # set_indreg_cmd, reg 19
$OD -dr --start-address=0xc600 --stop-address=0xc694 $K/tm.ko   # access_timeout, reg 20
$OD -dr --start-address=0xacb0 --stop-address=0xadf8 $K/tm.ko   # table_sel, reg 29

# 哈希
$OD -dr --start-address=0xc4b8 --stop-address=0xc600 $K/tm.ko

# 表项打包
$OD -dr --start-address=0xf1b0 --stop-address=0xf260 $K/tm.ko
```

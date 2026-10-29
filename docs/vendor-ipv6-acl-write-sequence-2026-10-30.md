# 原厂 IPv6 卸载的硬件访问序列（离线反汇编，2026-10-30 第二版）

> 对象：`switch.ko`（224808 B）、`tm.ko`（1270520 B），取自
> `03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/`。
> 方法：`arm-openwrt-linux-objdump -dr`，靠重定位拿符号名。除标「推断」外都是指令级证据。
> 中间产物：`/tmp/sbrg_add.txt`（`sbrg_add_ipv6table` 全文）、`/tmp/sbrg_lookup.txt`。

## ⚠ 勘误：第一版把两条不同的路径混成了一条

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

## 1. 原厂至少有三套互相独立的表（这一条是本轮最大的修正）

| 表族 | 访问窗口 | 用途 | 对应我们驱动里的 |
|---|---|---|---|
| **CLA**（classifier）哈希表 | ONU 寄存器 **1（命令/状态）、2（数据）** | 分类器哈希查找；`cla_set_hash_table` / `cla_get_hash_table` / `cla_acl_hash_addr_gen` / `cla_list_hash_addr_gen` | `zx_ppe_sbrg_*` 的兄弟：`ZX_CLA_*` 那一套（第七～九轮在打的） |
| **sbrg** IPv6 表 | ONU 寄存器 **19（命令）、20（忙）、76/77/78（数据）、29（table_sel）** | 字符串里明写 `ipv6 multicast address table`，即**组播/MLD 表**（12 字 = 48 B 表项） | `zx_ppe_sbrg_*`（第八轮移植的，格式是对的） |
| **fast_l3_session / PP-RAM** | `addFlowOperInfo` → `pp_pm_set_*_ram_info` | 隧道/封装会话加速（`pp_pm_set_b4_ipv6_ram_info`、`aftr_ipv6`、`pon_pp_set_dslite_*`、`pon_pp_set_6rd_sip`） | 我们完全没有 |

所以「原厂的 IPv6 加速走 ACL L3 hardfast 而不走 CLA」这个说法要收窄：
`sw_acl_l3_hardfast_session_add` 这个名字里的 "acl" 指的是它下发的规则形态，
而它最终落的表是 **CLA 哈希 + 封装 RAM 信息**，不是独立的一张 ACL IPv6 表。

## 2. 调用链（改正后）

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

## 3. CLA 的间接访问协议（`cla_set_indirect_rw_cmd`, 0x44c）

```
value = addr + (sel << 22) + (rw << 27)
tmOnuRegWrite(1, value)
```

* 写数据：`cla_set_indirect_rw_data` → `tmOnuRegWrite(2, ...)`
* 读状态：`cla_get_indirect_rw_status` → `tmOnuRegRead(1, ...)`
* 读数据：`cla_get_indirect_rw_data`（0x668）

**打包格式与 sbrg 完全相同**（`addr | sel<<22 | rw<<27`），只是基址寄存器不同：
CLA 是 **1/2**，sbrg 是 **19/20 + 76/77/78**。

## 4. CLA 的哈希配置：8 个 dword 整块写 ONU 寄存器 12

`cla_set_hash_poly_config(cfg)`（0x103c，0x170 B）把 `cfg[0..7]`（8 个 dword = 32 B）
一次性写进 ONU 寄存器 **12**；`cla_get_hash_poly_config`（0x11ac）读回。
即 **CLA 哈希的多项式/种子是可配置的 8 字块**。

> 这条很关键：如果硬件用的是 A 配置、我们软件算槽号时用的是 B 算法，
> 表项就会写进「硬件永远不会查的那个槽」——症状正好是"写进去了、读回来了、就是不命中"。
> **下一步就该查这 8 个字实际是什么值**（谁在开机时写它）。

## 5. 两个哈希函数（都拿到了）

### 5.1 `cla_list_hash_addr_gen(key, len)`（0x71a0，60 B，全文）

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

### 5.2 `sbrg_hash(key, unused, type)`（0xc4b8；IPv6 用 type=0x10c 那条）

* `type` 是**类型选择**（0x4c / 0x10c / 0x3c 各一条路径），不是多项式
* key 的前 **34 字节按字节倒序**后跑表驱动 CRC-16/CCITT（init 0、poly 0x1021）
* 结果取 **低 10 位**
* 34 B = sip(16) + dip(16) + vlan(2)

> 我们的 `zx_ppe_sbrg_hash()` 与此**逐位一致**。

## 6. sbrg 表项的字段（由 `sbrg_lookup_ipv6table` 的取值反推）

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

## 7. sbrg 表项的 4 块写（保留，格式已核实）

表项 = **4 块 × 3 字 = 12 字（48 B）**，块地址 = 槽基址 | 块号：

* 探测：`sbrg_get_indreg_wr_cfg(sel, addr)` 后读 76/77/78，**三字 OR 为 0 才判空槽**
* 写入：`sbrg_set_indreg_wr_cfg(sel, addr)` 后 `tmOnuRegWrite(78→77→76)`
* 三字在栈上的位置 `sp+0x68/0x6c/0x70`（块0）、`+0x74/0x78/0x7c`（块1）…
* **写入时 sel 恒为 0**（第一版猜"要写进四个存储体"，已排除）

槽基址：`sel==1` → `((hash & 0x7f) << 2) | 0x200`；否则 `((hash << 2) & 0xff) | 0x300`。
`table_sel`（reg 29）必须是 **1 或 2**，0/3 原厂自己会拒写。

## 8. 下一步（按性价比）

1. **查 CLA 哈希那 8 个字（ONU reg 12）实际值**，以及开机时谁写它。
   这是现在最可疑的一处：硬件查表用的哈希若与我们算槽号的算法不一致，
   表现就是"表项写对了但不命中"。
2. `cla_acl_hash_addr_gen`（0x6cec，0x4b4 B）是**纯计算函数**（只调 printk/memset），
   里面就是 ACL 哈希的实现。把它读出来，与 1 的结果对照。
3. `cla_set_hash_table`（0x5a24，0x50c B）是我们 CLA 写表逻辑的**原厂对照物**：
   逐条比对它的间接写序列（reg 1/2 的顺序、数据字数、写完是否回读校验）。
4. 别忘了 `tm_findFastIdxByTuple`（0x57c50）：原厂自己有一套"按 5 元组在硬件里
   找现成表项"的例程，正是我们验证命中与否时缺的那个**读侧证伪工具**。

## 9. 复查命令

```sh
OD=工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-objdump
K=03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs

$OD -dr --start-address=0x5ce74 --stop-address=0x5e404 $K/tm.ko | grep -A1 R_ARM_CALL   # session_add 的真实调用
$OD -dr --start-address=0x71a0  --stop-address=0x71dc  $K/tm.ko                        # cla_list_hash_addr_gen 全文
$OD -dr --start-address=0x44c   --stop-address=0x520   $K/tm.ko                        # CLA 间接命令 (reg 1)
$OD -dr --start-address=0x103c  --stop-address=0x11ac  $K/tm.ko                        # CLA 哈希 8 字配置 (reg 12)
$OD -dr --start-address=0xeb50  --stop-address=0xf004  $K/tm.ko                        # sbrg lookup 的字段取值
$OD -dr --start-address=0xf004  --stop-address=0xf8d0  $K/tm.ko                        # sbrg_add_ipv6table
```

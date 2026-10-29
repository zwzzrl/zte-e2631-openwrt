# 原厂 v6 快表项到底写到哪里：外部哈希表 = phys 0x20000，而这台机器上那块地是启动 ROM

- 日期：2026-10-31（第二轮，接 `docs/vendor-v6-entry-format-2026-10-31.md`）
- 对象：离线仿真（`tools/vendor-emu`，原厂 `tm.ko` 4.1.25 真身跑在 Unicorn 上）+ 原厂
  `vendor-vmlinux.bin` 的 iotable + 真机（OpenWrt v12fix / slot1 / 6.18.55）
- 约束：**本轮没有刷机、不改原厂分区**；真机只做只读探测（`dump 8`）和模块重载

**标注**：`确定` = 本轮仿真或真机读数的原始值；`推断` = 由两条以上证据推出；`未解决` 见 §7。

---

## 1. 一句话结论

**把 `t_v6flowdump.py` 那条会话换成"地址真实存在"的形状之后，原厂 v6 会话添加既不写内部
哈希表、也不写规则表/索引表，它唯一写的是 `cla_set_external_hash_table()`——落点是
`0xF1020000 + 64*idx` 的"外部哈希表"。而这块地址在原厂内核里是 iotable 的
`virt 0xF1000000 <- phys 0x00000000 len 0x520000`，也就是物理 `0x00020000 + 64*idx`；
本板 2026-10-29 已经实测过那里是启动 ROM 的向量表，写会触发 imprecise external abort。**
所以"我们的 v6 表项不被硬件接受"和"原厂 v6 快表项比我们多写一张表"这两件事现在闭合了：
多写的那张表在这颗芯片的这块板子上不存在，CLA 的 v6 单播快转在这里**没有可写的表**。

顺带钉死两条结构事实：

1. `cla_set_indirect_rw_cmd(sel, table, index)` 的第一个参数 **不是"外部空间选择"**，而是
   **读写方向**（0 = 写、1 = 读）；命令字 = `index | table<<22 | sel<<27`，`table ≤ 9`。
   表号全表：0 = 索引表、1 = 规则表、2..6 = 内部哈希五档库、7 = cpu queue id、8 = aging、
   9 = dscp/tc/qid。**没有任何"外部表"的口子**。
2. 外部表的字节序：落盘时**每 8 字节一组逆序**（不是整条逆序、也不是按 32 位字节交换）。

---

## 2. 怎么得到的（方法）

上一轮的 `t_v6flowdump.py` 只截三个表写入函数的**入参**，等于把真身整个桩掉了。本轮换了做法
（`tools/vendor-emu/t_v6ops.py`）：

* 只桩**函数栈底部**：`cla_set_indirect_rw_cmd/data`、`cla_set_indirect_rw_status`、
  `cla_get_indirect_rw_data`、`fpga_read_reg/write_reg`、`printk`、`__memzero/memset/memcpy`、
  分配器。
* 其余**真身跑**：`cla_set_hash_table`、`cla_set_extra_rule_table`、`cla_set_extra_index_table`、
  `cla_set_external_hash_table`、`aclBuildHashAndAclRule` ……
* 把底部桩收到的 `(table, index, word)` 记进一个 RAM 模型，跑完打印——这就是"原厂自己
  往 CLA 里写了什么"的完整流水。
* 同时挂 `UC_HOOK_MEM_WRITE`，凡落在模块自身内存之外的写都记下来（外部表走的是**直接
  内存写**，不走间接接口，只有这个钩子能看见）。

会话用 `t_hfsess.build_desc(d14=1, type=10, port=inport)`（`desc[0x0c] = 10 = AF_INET6`，
16 字节地址才会真正进会话），五元组 `fdee:70df:d02e::2:40000 -> fd88::2:9999` UDP，
inport 2 = lan1。作为对照，同一个脚本加 `LEGACY=1` 跑上一轮那条"地址是掩码编码"的会话。

---

## 3. 新事实 A：地址真实的 v6 会话，只写外部表（`确定`）

`t_v6ops.py`（默认会话）的调用序列，逐条是实际命中顺序：

```
cla_list_hash_addr_gen
tm_acl_fast_add
tm_acl_fast_add_v4v6
cla_get_hash_poly_config
aclGetExtIdxRuleRamAddrByPort(port=2)
tm_acl_get_fastHashRule
aclGetAvailableHashAddr(sel=0)            + cla_acl_hash_addr_gen ×4
aclGetAvailableHashAddr(sel=1)            + cla_get_outspace_cfg + fpga_read_reg +
                                            cla_acl_hash_addr_gen ×1
cla_set_external_hash_table(0x3c00, 0x13, entry, 0x43)
→ 返回 0
```

RAM 模型（间接写流水）**是空的**：没有 `cla_set_hash_table`、没有 `cla_set_extra_rule_table`、
没有 `cla_set_extra_index_table`。全部写操作只有 75 次直接内存写，落在
`0xF1110000..0xF111003B`。

同样脚本加 `LEGACY=1`（上一轮那条会话）走的却是**内部表**：`cla_set_hash_table` 被调用，
打印 `[TM][cla_set_hash_table]indirect access time out`（我们自己把状态桩成"永远空闲"，
所以它超时了），返回 `0xfffffff4`。两条路的分岔点在**会话内容**里，不在配置里。

> 上一轮"原厂 v6 表项 = `0100004c ...` 短 key、只写 hash 表"的观察，就是 `LEGACY` 这条
> 分支。它描述的是同一个函数在**另一种会话形状**下的行为，不是原厂运行态走的那条。

---

## 4. 新事实 B：外部表的落点与字节序（`确定`）

`cla_set_external_hash_table(addr, idx_byte, entry, e16)` 的真身只做两件事：

1. 目的地址 = `((0x3c40800 + addr) << 6) & 0xffffffff`，32 位回绕。
   `addr = 0x3c00` → `0xF1110000`；等价写法 `0xF1020000 + 64*addr`。
2. 把 `entry` 的 60 字节按 **每 8 字节一组逆序**写过去（同时顺手改调用方缓冲里的
   `entry[0x10]` 的 bit6）。

源缓冲（`cla_set_external_hash_table` 的 `r2`，也就是内部表会原样写进去的那 15 个字）：

```
+000  4c 00 00 01 00 e1 13 f5 08 06 00 00 00 00 00 30
+016  43 00 14 00 11 ee fd df 70 2e d0 00 00 00 00 00
+032  00 00 00 02 00 88 fd 00 00 00 00 00 00 00 00 00
+048  00 00 00 02 00 00 00 00 00 00 00 00
```

同一个 run 里 `0xF1110000` 的落盘镜像（内存写钩子抓到 + 跑完回读，两路一致）：

```
+000  f5 13 e1 00 01 00 00 4c 30 00 00 00 00 00 06 08
+016  df fd ee 11 00 14 00 43 00 00 00 00 00 d0 2e 70
+032  00 fd 88 00 02 00 00 00 00 00 00 00 00 00 00 00
+048  00 00 00 00 02 00 00 00 00 00 00 00 00 00 00 00
```

七组 8 字节逐组对得上逆序（`4c 00 00 01 00 e1 13 f5` ↔ `f5 13 e1 00 01 00 00 4c` …），
所以"每 8 字节一组逆序"是完整规则，不是只对前几组成立。

---

## 5. 新事实 C：间接命令寄存器没有"外部空间"选择位（`确定`）

`cla_set_indirect_rw_cmd(sel, table, index)` 反汇编（`tm.ko` 0x44c，212 B）里的校验和命令字：

```
cmp r7, #9          ; table <= 9
cmpls r5, #0x400000 ; index < 4 Mi
cmp r6, #1          ; sel <= 1
cmd = index + (table << 22) + (sel << 27)
tmOnuRegWrite(reg 0, cmd)
```

再核对 12 个调用点传的 `sel`（`/tmp` 里那份 `callsel` 脚本的输出，全部实测）：

| 函数 | sel | table |
|---|---|---|
| `cla_set_extra_index_table` / `cla_get_extra_index_table` | 0 / 1 | 0 |
| `cla_set_extra_rule_table` / `cla_get_extra_rule_table` | 0 / 1 | 1 |
| `cla_set_hash_table` / `cla_get_hash_table` | 0 / 1 | 2..6 |
| `cla_set_cpu_queue_id` / `cla_get_cpu_queue_id` | 0 / 1 | 7 |
| `cla_set_acl_aging_table` / `cla_get_acl_aging_table` | 0 / 1 | 8 |
| `cla_set_acl_dscp_tc_qid_table` / `cla_get_acl_dscp_tc_qid_table` | 0 / 1 | 9 |

**写函数一律 sel=0、读函数一律 sel=1**，没有例外。所以 `sel` 是读写方向（和我们驱动里
`ZX_PP_IND_READ` 那一位同一个意思），不是"内部表 / 外部表"。

真机侧再验一次（只读）：`echo 8 > .../parameters/dump` 走的就是这张表号表，`table 2..6`
读得回我们写进去的 520 个槽，`7..15` 全空/读不回——和上表一致，**不存在第 5 张可用表**。

---

## 6. 结论：外部表在这块板上不存在，CLA 的 v6 单播快转没有表可写

三条独立证据：

1. `确定` 原厂地址真实的 v6 会话添加**只**写外部表（§3）。
2. `确定` 外部表的落点 = 原厂 iotable 的 `virt 0xF1000000 <- phys 0x00000000, len 0x520000`
   这条映射里的 `0x00020000 + 64*idx`（`vendor-vmlinux.bin` 文件偏移 0x606440，同表里还有
   `0xF4000000 <- phys 0x92000000` 等 20 条）。
3. `确定` 本板 2026-10-29 用模块参数 `xprobe` 实测：物理 `0x1000 / 0x8000 / 0x20000 /
   0x28000 / 0x40000 / 0x100000` 读回**同一份 16 个字**（`e59ff120 eafffffe …` = ARM 异常
   向量表 + 启动桩），六个地址相隔 64 KiB 完全一样 ⇒ 是启动 ROM 的别名；写 `0x20840`
   触发 `imprecise external abort`。**没有可写的表存储**。

把这三条放在一起，本轮之前那个"为什么表项写对了也命不中"的问题就有了结构性的答案：
**CLA 内部哈希在这颗芯片上不查 v6**（上一轮已经用"单窗口 key + 铺满 520 槽 + 原厂配置字"
从反面证过），而 CLA 的 v6 单播快表在**另一块存储**上，这块板子没有那块存储。

原厂固件在这台机器上的 v6 加速因此只能来自另外两层（`06-分析与对比/原厂IPv6硬件加速-实测事实-2026-10-30.md`）：
**FFE**（编进内核的 conntrack 五元组软件快转，ipv4/ipv6 全覆盖）和 **NPU**（挂在他们
switch.ko 的 ACL L3 hardfast 上，能力矩阵里 ipv6 只有 tcp/udp/mc，且只允许 WAN↔LAN）。

---

## 7. 未解决

- NPU 的路由 v6 快转需要原厂 `switch.ko` 的 `npu_drv_create_flow` 那条链，我们既没有它的
  寄存器手册也没有可加载的 4.1 模块；要不要投入是另一个量级的决定。
- `cla_set_external_hash_table` 里那个 `entry[0x10]` bit6 的读改写（`0x43 → 0xc3`）在这块板上
  没有可验证的落点；只在原厂代码里成立。
- 外部表的 16 个库 × 1024 槽这个规模来自 `s_aclHashUsedCnt` 的 66056 字节
  （`520 + 16*1024` 个 4 字节计数），不是直接读硬件得到的。

## 8. 这对驱动意味着什么

- `ipv6=1` 的定位不变，而且更清楚了：它给的是**二层 IPv6 硬件交换**（摘掉 0x86dd 陷阱后
  同网段帧由交换芯片直接过）和**IPv6 组播硬件表**（`sbrg` 参数那套，0x388000），
  **不**包含路由 v6 快转——后者在这颗 SoC 的 CLA 上没有表。
- 驱动里这轮加的 `v6_tag` / `v6_w1` / `v6_w3` 三个参数（改表项 byte18/word1/word3）保留，
  它们是"整条表项直写"的实验口，以后换 SoC 复用得上。

## 9. 复现命令

```sh
cd tools/vendor-emu     # 镜像 e2631-uni:py312，挂 /ko = 原厂 kmodule-unsquashfs

# 1) 地址真实的 v6 会话：只写外部表
docker run --rm -v "$PWD":/w -v <原厂kmodule目录>:/ko:ro -w /w e2631-uni:py312 t_v6ops.py
# 2) 对照：上一轮那条会话走内部表
docker run --rm -e LEGACY=1 -v "$PWD":/w -v <原厂kmodule目录>:/ko:ro -w /w e2631-uni:py312 t_v6ops.py
# 3) 只截调用序列（不跑真身）
docker run --rm -v "$PWD":/w -v <原厂kmodule目录>:/ko:ro -w /w e2631-uni:py312 t_v6trace.py

# 真机（只读）：表号 0..15 扫一遍，确认只有 0..6 有存储
./rsh 'echo 8 > /sys/module/zx279128s_ppe/parameters/dump; dmesg | tail -40'
```

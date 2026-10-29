# 原厂硬件调试面：`/sys/devices/platform/tm/` 全清单与用法（2026-10-30 实测）

> 取证环境：原厂系统（`V1.0.0.8B8.9900`）跑在 slot1，root 壳经 `vsh.py`。
> 所有内容都是 `ls` / `cat` / `echo` 实测，或从 `tm.ko` 的字符串与符号读出来的。
> **这一份是"读侧"工具**：之前我们只有写侧（驱动里硬编码），现在有了原厂自己的
> 表内容导出与哈希计算器，可以直接对照。

---

## 0. 怎么找到的

`switchtst`（`/bin/switchtst`，62220 B）的字符串里写死了它调用的文件路径：

```
-----------------get fast info-----------------
cat /sys/devices/platform/np/test/gfastentrynum
cat /sys/devices/platform/np/test/showfastinfoall
-----------------get acl info-----------------
cat /sys/devices/platform/np/test/showaclinfoall
------------get v4v6 mul table info------------
cat /sys/devices/platform/np/table_debug/v4_table
cat /sys/devices/platform/np/table_debug/v6_table
```

`np` 这个路径在本机不存在（`No such file or directory`），真正的前缀是
**`/sys/devices/platform/tm/`**：

```
$ find /sys -maxdepth 5 -name showfastinfoall
/sys/devices/platform/tm/ppCla/showfastinfoall
```

## 1. 顶层结构

```
/sys/devices/platform/tm/
├── ppCla/    分类器（CLA）：哈希表、规则表、fast 规则、哈希地址计算  ★
├── ppPm/     包处理（PM）
├── ppBrg/    桥：macTable / brdTransTable / multTransTable / gvlancfg …
├── ppAdm/    流限速与统计
├── nppPm/    新 PM：spmflowrule / spmportrule / spmzterule / azte …
├── nppSpa/   交换：ghashram / gmatchram / sonehashramentry / sonerule / gportpvid …
├── nppGreg/  端口：gportstatus / sportstatus / phyLdoTest …
├── nppSdet/  sonumtu
├── tmRed/    RED 队列
└── tmTest/   测试
```

## 2. `ppCla/` 全清单（22 个节点）

权限只有两种：`-r--------`（只读，`cat` 取内容）与 `--w-------`（只写，`echo` 送参数）。

**只写（送命令）**

| 节点 | 用法（来自 `tm.ko` 里的帮助串） |
|---|---|
| `addfastrule` | `echo [dir][sport][dport][proto][sip][dip] > addfastrule` |
| `delfastrule` | `echo [dir][sport][dport][proto][sip][dip] > delfastrule`，`dir` up:0 down:1 |
| `ghashtable` | `echo [hash_addr] > ghashtable` — **按哈希地址读回规则表** |
| `gextraindextable` | `echo [index_id] > gextraindextable` — 按 index 读索引表 |
| `gextraruletable` | `echo [rule_id] > gextraruletable` — 按 rule id 读规则表 |
| `calculatehashaddr` | `echo <29 个 hex 字> > calculatehashaddr` — **算哈希地址（预言机）** |
| `gagingstatus` | `echo [hashAddr] > gagingstatus` — 查表项老化状态 |
| `setmaxl3entry` | `echo [up][dn] > setmaxl3entry` |
| `setmaxfastentry` | `echo [enable][upnum][dnnum] > setmaxfastentry`（上限 16000） |
| `flowInfoList` | `operFlowInfoListReset()` |
| `modhashtable` | — |

**只读（取内容）**

`desIn`、`desOut`、`gclacfg`、`gparsehashkey`、`helpCla`、`reason`、`statics`、
`showcommrule`、`showfastrule`、`showfastinfoall`、`showaclinfoall`、`gfastentrynum`

> 注意：`gclacfg` / `gparsehashkey` / `helpCla` / `desIn` / `desOut` 在**本版本上读出来是空的**
> （0 字节）。它们不是不存在，而是内容依赖前序操作或该版本没实现填充。
> `statics` / `showfastrule` 各回 1 字节（基本等于 `0\n`）。

## 3. 哈希预言机：`calculatehashaddr`（实测可用）

```
echo "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29" \
     > /sys/devices/platform/tm/ppCla/calculatehashaddr
→ 内核日志： calculate hash addr is :0x4e1b
```

* 输入格式：**29 个 `%x`**（格式串在 `tm.ko` 的字符串里，就是 29 个 `%x` 连写）
* 输出：`calculate hash addr is :0x%x`，走 printk
* 这个节点是 **0200（只写）**，`show` 指针为 NULL，所以 `cat` 会得到
  `Permission denied`——不是权限问题，是它本来就没有读侧

第一轮字段影响测试（全零基线 + 单独把第 k 个字段置 1）：

| 输入 | 结果 |
|---|---|
| 全 0 | `0x0` |
| 第 1 个 = 1 | `0x1db7` |
| 第 5 个 = 1 | `0x21c0` |
| 第 13 个 = 1 | `0xb1bd` |
| 第 21 个 = 1 | `0x8d6e` |
| 第 29 个 = 1 | `0x0`（与全零相同 → 第 29 个字段可能不参与，或解析上限 < 29） |
| 1..29 递增 | `0x4e1b` |

结论：这台机器上的 `calculatehashaddr` 是一个**真实可用的哈希预言机**，
可以拿它当"原厂标准答案"来校验我们驱动里算的槽号。

## 4. 怎么读内核日志（配套）

这些节点的输出全部走 printk，而 `/proc/kmsg` 被 `logcat` 占着。
可读副本在 **`/dev/logger_main`**（二进制记录流，每次打开回放整段开机积压）：

```sh
timeout -t 2 cat /dev/logger_main > /tmp/lg.bin      # 注意是 -t，原厂 busybox 的 timeout
grep -a -o "calculate hash addr is :0x[0-9a-f]*" /tmp/lg.bin
```

`grep -a -o` 直接在二进制流里抽匹配片段，避免把几十万字节灌进终端。

## 5. 这些节点由谁实现（`tm.ko` 内部）

* `dev_attr_calculatehashaddr`（0x8d04）是个标准的 `struct device_attribute`：
  `name = .rodata.str1.4`、`mode = 0x8000|0200`、`show = NULL`、
  `store = cla_info_store`。
* **`ppCla/` 下所有节点共用同一个 store**：`cla_info_store`（0x6627c，0x10b4 B），
  靠 11 次 `strcmp` 按属性名分派；它内部只调 1 次 `sscanf`——
  就是 §3 那个 29 个 `%x` 的解析（也是全模块唯一的 `sscanf` 调用点）。
* 读侧共用 `cla_info_show`（0x67330，0x151c B）。

## 6. 用它做什么（对我们的直接价值）

1. **校验哈希**：把同一组键分别喂给 `calculatehashaddr` 和我们驱动的 `zx_ppe_sbrg_slot()`
   / CLA 槽号算法，一致才说明"写进去的位置"就是"硬件会查的位置"。
2. **读回真实表项**：`echo [hash_addr] > ghashtable` 后从日志里拿表内容，
   与我们写进去的 12 个字逐字对比——这是当初 2026-10-27 想干而没干成的"拿原厂运行时表内容"。
3. **看真机 fast 表规模**：`cat gfastentrynum`、`cat showfastinfoall`。

## 7. 一条安全警告

**不要随手跑 `switchtst`。** 实测 `switchtst -h` 并不打印帮助，而是执行了默认动作，
输出 `Close switch success!` ——它是个选项不认识就往下干的工具，可能把交换机关掉。
要看用法就读它的字符串（`arm-openwrt-linux-strings /bin/switchtst`），别在机器上试。

## 8. 复查命令

```sh
# 清单
ls -l /sys/devices/platform/tm/ppCla/
ls /sys/devices/platform/tm/nppSpa/

# 哈希预言机
echo "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29" \
     > /sys/devices/platform/tm/ppCla/calculatehashaddr
timeout -t 2 cat /dev/logger_main > /tmp/lg.bin
grep -a -o "calculate hash addr is :0x[0-9a-f]*" /tmp/lg.bin

# 帮助串（离线，权威）
ST=工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-strings
$ST 03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/tm.ko | grep -B2 -A30 'calculatehashaddr'
```

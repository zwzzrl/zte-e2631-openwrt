# 原厂系统的 IPv6 转发实测（2026-10-30，router 跑在 slot1 的原厂 + root 壳）

> 取证对象：ZXHN E2631（ZX279128S），**原厂系统 + 我们追加的 root 壳**（`e2631-vendor-slot1-append-up.pkg`
> 的 body）刷在 **slot1**，`/sbin/shd` 在 2323 端口给 uid=0。
> 全部数字都是在这台机器的运行态上量出来的，命令附在每节末尾。
> 本轮同时发现 **slot0 已经不再是出厂镜像**（见 §5），这条比 v6 数字更要紧。

---

## 0. 一句话结论

| 路径 | 实测 | 谁在转 |
|---|---|---|
| **网桥/扩展器路径**（eth ↔ Wi-Fi 上行，L2） | iperf3 over `fe80::` **899–932 Mbit/s**，4 并发 910–919，反向 928；537 MB 流量期间盒子 **CPU 侧端口计数只增 789 B** | **交换芯片硬件转发**（线速） |
| **路由路径**（`ip6_forward`，本机进本机出，出 br0） | 263 Mbit/s 突发 500/500 全转；373 Mbit/s 突发 20000 包只转 7672（≈**143 Mbit/s**），CPU 打满；`ffeinfo` 里 `curr:0 / ipv6_udp:0 / npu:0` | **纯 CPU 软件转发**，连 FFE 都没进 |
| **WAN↔LAN 真路由**（eth1 进 eth2 出，§8 补测） | iperf3 v6 TCP **918–928 Mbit/s**（60 s 6.41 GB）；UDP 716 Mbit/s@67 kpps；`ipv6_tcp:5 npu:1`、`npu_flows:1`、`cmd:[tcp_skip]->[ipv6_set]->[eth_set]` | **NPU 表项装上了、但硬件没命中（`hit:0`），包仍是 CPU 走 FFE 软快转**（≈4.7 µs/包） |
| 出厂默认的路由转发 | `net.ipv6.conf.br0.forwarding = 0`（`all=1`），即**默认不转发** | — |

所以"原厂的 IPv6 转发"在这台机器上真实存在的是三种形态：**硬件二层转发（线速、CPU 零参与）**、
**br0 出方向的 CPU 三层转发（百兆级、FFE 都没进）**，以及 §8 补测的 **WAN↔LAN 形态：
NPU 表项确实装得上，但数据面仍由 CPU 的 FFE 软快转承担（~900 Mbit/s / 0.4 核）**。

> 修正：本文第一版据此写过"eth↔eth 的 v6 永远落不到 NPU 上 / v6 NPU 在这块板上不可能"——
> **这条已被 §8 的实测推翻**。能力表 `index[1]`（rxdev `eth*` → txdev `eth*`）里确实带
> `ipv6_tcp`/`ipv6_udp`，原厂真会把 v6 流写进 NPU 表（`ipv6_set`+`eth_set` 命令链、真实端口/MAC），
> 只是那张表在硬件侧不生效（`hit` 恒 0，`createfail` 一路涨）。

---

## 1. 刷入结果（cspboot 自己打的判决）

串口（`~/ch343-bridge.log`）在 WebUI 上传完成、盒子自重启后打出：

```
| Seq |   Kernel   |    Romfs   |   Header   |
|  0  | 0x00700000 | 0x00a40000 | 0x01b60000 |
|  1  | 0x02300000 | 0x02640000 | 0x03760000 |
zteboot_do_settings:490 >>s=256M, select=1
```

- `select=1` → 起的是 slot1；fs 在 `0x2640000`（= slot1+0x340000）、版本头在 `0x3760000`
  （= slot1+0x1460000），正是 append 几何，说明 **WebUI 写的就是 slot1**。
- 运行态：`uname -a` → `Linux E2631 4.1.25 #2 SMP Wed Aug 26 13:36:12 CST 2026 armv7l`，
  `id` → `uid=0(Z500_w9bZ) gid=0(root)`，`/sbin/shd` 17268 B，2323 端口可用。
- slot1 的版本头（`hexdump -C -s 0x3760000`）：`body=0x1460000`、`k_size=0x33fdcc`、
  `fs_size=0x1120000`、`k_crc=0x2edf94cd`、`fs_crc=0xe84708cf`、`slot=0x2300000`、
  **`serial=0x33(51)`**、`H_NEW=1`。
  → 原厂 httpd 的 serial 规则是 **max+1**：上传前 slot0 是 50（我们用 `e2631-fwslot switch`
  盖的），写出来就是 51，于是 51 > 50，重启选 slot1。`retitle_pkg.py --serial` 不决定这个值。

复现：

```sh
# OpenWrt 里：切到 slot0（系统工具，只改 slot0 那 512 字节版本头）
./工作文件/05-路由器操作脚本/rsh 'e2631-fwslot switch && reboot'
# 原厂起来后（192.168.5.1）：
cd 工作文件/05-路由器操作脚本 && ./vendor-web.py status
./vendor-web.py upload ../02-固件产物/vendor-root-shell/e2631-vendor-slot1-append-up.pkg --yes
# 约 1 分半后盒子自重启；串口能看到上面那张表
```

---

## 2. 环境：原厂在这台机器上是"无线扩展器"

`ip -4 addr` / `ip -6 addr` / `iwconfig apcli5g0` / `ls /sys/class/net/br0/brif`（实测）：

```
br0: 192.168.43.119/24（DHCP 自 192.168.43.1）+ 192.168.5.1/24
     2408:8459:1630:2fd3:e660:4dff:fe86:71ef/128（SLAAC）+ fe80::e660:4dff:fe86:71ef/128
br0 成员：apcli5g0 eth1 eth2 eth3 nbif0 wlan0
apcli5g0: ESSID"KESLAYSSR" Mode:Managed Ch149 AP:AA:BB:CC:DD:EE:FF 1.201 Gb/s -35 dBm
eth0 carrier=0, eth1=1, eth2=1, eth3=0（四个口同一个 MAC，都是交换芯片端口）
```

即：**Wi-Fi 上行（apcli5g0）与以太网口桥在一起**，WebUI 报 `curWorkMode=Extender`。
本机（192.168.1.250 / 192.168.5.50）接在它的 eth 口上，desktop 也在同一侧，所以
主机 ↔ desktop 的 v6 流量是"进一个口、出一个口"的**二层转发**。

---

## 3. 二层（桥）转发：线速，且完全不经过 CPU

对端 = `desktop-a5a2tk2` 的 fe80（`fe80::e8d5:aa55:4bc3:42a8`，MAC `d8:43:ae:68:18:28`），
iperf3 服务器在 5201，防火墙只放 192.168.0.0/16、fe80::/10、fc00::/8（用户给的截图）。

```
ping6 -c3 -I ens34 fe80::e8d5:aa55:4bc3:42a8      → 3/3，1.7 ms
iperf3 -c fe80::e8d5:aa55:4bc3:42a8%ens34 -p 5201 -t 5
   [  5]   0.00-5.00   sec   539 MBytes   904 Mbits/sec   42   sender
   [  5]   0.00-5.01   sec   537 MBytes   899 Mbits/sec        receiver
-R（反向，desktop→host）   932 / 928 Mbits/sec
-P 4（四并发，合计）        919 / 910 Mbits/sec
-l 64（64 字节小包）        258 / 241 Mbits/sec   （≈470k pps，瓶颈在 Windows 端）
```

**同一次 537 MB 传输期间，盒子的 CPU 侧端口计数几乎没动**（`grep eth1|eth2|br0 /proc/net/dev`）：

| 计数 | 传输前 | 传输后 | 增量 |
|---|---|---|---|
| eth1 rx_bytes | 17 266 522 | 17 267 311 | **+789 B**（只有 ssh/控制流） |
| eth2 rx_bytes | 107 533 | 107 533 | **0** |
| eth1 rx_packets | 21 624 | 21 634 | +10 |

也就是说 537 MB 的 IPv6 帧**一帧都没进 CPU**，全部由 ZX279128S 的交换芯片按 L2 表转发。
这正是"IPv6 硬件转发"在这块板子上的真实形态（对比我们 OpenWrt 里 `ipv6=1` 那个
把 IPv6 帧从 CPU 陷阱里放出来的参数：不放就是每帧都上交 CPU）。

⚠ 用 v4 访问同一台 desktop 是失败的：`iperf3 -c 192.168.1.243` → `No route to host`
（ARP 不通）。所以本机只能走 fe80 这条 v6 路，v4 对照组拿不到。

---

## 4. 三层（路由）转发：软件，且默认关着

### 4.1 默认配置

```
cat /proc/sys/net/ipv6/conf/all/forwarding      → 1
cat /proc/sys/net/ipv6/conf/br0/forwarding      → 0     ← 网桥口不上转发
cat /proc/sys/net/ipv6/conf/default/forwarding  → 0
ip6tables -t filter -S                          → -P FORWARD ACCEPT，但 FORWARD 上挂了
     IsolateLocalForward / IsolateForwardService / ULAFilter / fwforward … 一串厂商链
     err_filter 里还专门 DROP icmpv6 133/134/135/136/137
```

### 4.2 回环台（主机发、盒子转、再从同口回来）

搭法（都是运行时的，不写 flash）：

```sh
# 主机：给 ens34 加一个全局地址（用 ULA 会被内核按"链路本地源"丢掉，见 4.3）
ip -6 addr add 2408:8459:1630:2fd3:249d:bd31:998b:5599/64 dev ens34 nodad
ip -6 route replace fd88::2/128 via fe80::e660:4dff:fe86:71ef dev ens34
# 盒子：把 fd88::2 指回主机，并打开 br0 转发（测完已还原）
ip -6 route add fd88::2/128 via 2408:8459:1630:2fd3:249d:bd31:998b:5599 dev br0
for f in /proc/sys/net/ipv6/conf/*/forwarding; do echo 1 > $f; done
# 主机侧发包 + 在 ens34 上数回环帧
python3 .scratch/bench6.py blast <n> 2408:…:5599 fd88::2 5201 <gap_ms>
python3 .scratch/bench6.py sniff <秒> fd88
```

结果（`Ip6OutForwDatagrams` 就是盒子的转发计数）：

| 发包 | offer | 盒子 InReceives | 盒子 OutForwDatagrams | 主机侧看到的回环帧 |
|---|---|---|---|---|
| 100 包 / gap 30 ms | 0.4 Mbit/s | +100 | **+99** | 196（含发出那 100） |
| 500 包 / gap 0 | 263 Mbit/s | +500 | **+500** | — |
| 20000 包 / gap 0 | 373 Mbit/s | +15 423 | **+7 672**（≈143 Mbit/s） | — |

20k 包那一轮的 CPU（`/proc/stat` `cpu` 行）：user +189、softirq +68、irq +68 tick，
idle 只 +459 tick —— 转发期间两个核基本被吃完。
同期 `cat /proc/zte/sys/ffe/ffeinfo`：`curr:0`、`ipv6_tcp:0 ipv6_udp:0 ipv6_mc:0`、
`npu:0`、`npuflowcnt:0`。**FFE 没建流、NPU 没参与**，转发全在 `ip6_forward` 里做。

### 4.3 一个必须记住的坑：ULA 源地址会被丢掉

第一轮用 `fdee:70df:d02e::2`（ULA）做源、无论多慢都是 `InReceives` 涨而
`OutForwDatagrams` 不涨（`InNoRoutes`/`InDiscards` 也都不涨）。换成全局地址
（`2408:…`）立刻 100% 转发。原因在 `ip6_forward()` 的源/目的 scope 检查：
ULA 在 4.1 的 `ipv6_addr_scope()` 里与链路本地同档，路由器按规定必须丢弃。
所以"v6 转发不通"先换全局源再判，别急着怪硬件。

---

## 5. ⚠ 本轮发现：slot0 已经不是出厂镜像

> **后续（2026-10-30 18:17，已处理）**：按用户要求把 slot0 写回了 10-27 的出厂备份
> `mtd5_slot0.bin`（V1.0.0.8B8.8000，头在 0x1B40000，serial 52），slot1 原样保留，
> 盒子现在从 slot0 起、跑纯原厂 8000。完整步骤与证据见
> `docs/slot0-restore-factory-2026-10-30.md`；本节以下保留当时的发现记录。

`e2631-fwslot switch` 要先把 slot0 的版本头 `slot_verify()` 一遍（kernel/fs 两段都算
CRC），它通过了；而它读到的是 `header@0x1460000 serial=48 version="V1.0.0.8B8.9900"`，
头里 `body=0x1460000`、`fs_size=0x1120000`、`fs_crc=0xe84708cf`。

`0xe84708cf` 正是 **append 包自己的 fs CRC**（在主机上对包体算出来的，见下），
而已知 10-27 的出厂备份（`03-固件备份/router-flash/mtd5_slot0.bin`，sha256
`4c7a83f6…`）的版本头在 **`0x1440000`**、`body=0x1440000`、`fs_size=0x1100000`、
`fs_crc=0x311c578e`、`serial=4`、版本串 `V1.0.0.8B8.8000`。

进一步对位（`hexdump -C -s <off> -n 32 /dev/mtd0`）：

- slot0 的 fs 起点 `0xa40214` 与 slot1 的 `0x2640214` **字节相同**，且都等于
  append 包里 fs 起点（包内 `0x340428`）那 32 字节；
- 尾部 `0x1b20214` / `0x3720214` 也都等于包内 `0x1420428`；
- `append.pkg` 与 `append-up.pkg` 的 body **逐字节相同**（只有 512 字节版本头里的
  版本串/CRC/serial 不同）。

结论：**slot0 现在装的是同一个"原厂 + root 壳"镜像**，只是它的版本头是
`body 0x1460000 / fs_crc 0xe84708cf / serial 50`（50 是今天 `e2631-fwslot switch` 盖的，
之前是 48）。也就是说 02:30 那次 WebUI 刷机把 **slot0 的固件体也写成了这份镜像**，
"slot0 保持出厂、随时可退"这个前提**已经不成立**。

旁证：12:23 那次（slot0 serial 50 > slot1 serial 49）起来的系统上有
`/sbin/shd`（17268 B）与 uid=0 的 2323 壳，而出厂 rootfs 树
（`03-固件备份/vendor-rootfs-decrypted/rootfs-tree`）里**没有** `sbin/shd` / `sbin/dump6`。

现存的出厂镜像只有 10-27 那份（8000 版）：`03-固件备份/router-flash/mtd5_slot0.bin`
（0x1c00000 B，sha256 `4c7a83f6…`）。要把 slot0 恢复成"出厂"，**必须写 slot0**
（铁律里唯一需要用户明确点头的操作），而且恢复回去的是 8000 版，不是后来的 9900 版。

---

## 6. 本轮对盒子做过的临时改动（全部已还原，未写 flash）

```sh
# 已还原：br0 的 ULA、fd88::2 路由、per-iface forwarding、ip6tables/iptables 的 -I FORWARD ACCEPT
ip -6 addr del fdee:70df:d02e::1/64 dev br0
ip -6 route del fd88::2/128
for f in /proc/sys/net/ipv6/conf/*/forwarding; do case $f in */all/*) ;; *) echo 0 > $f;; esac; done
ip6tables -t filter -D FORWARD -j ACCEPT ; iptables -t filter -D FORWARD -j ACCEPT
# 现在的值：all=1（原样）、br0=0（原样）、default=0（原样），FORWARD 上没有我们加的规则
```

主机侧也清干净了：ens34 只剩 `192.168.1.250/24`、`192.168.5.50/24`、`fe80::20c:29ff:fe4b:592a`。

---

## 7. 还要做的

1. **slot0 怎么处置**（用户决定）：留现状（能起、带壳、但不是出厂）/ 用 10-27 备份写回
   8000 出厂（要写 slot0）/ 先只留档。
2. 要回 OpenWrt 时：现在盒子跑的是原厂、手里没有 `e2631-fwslot`，只能走串口
   `nand erase 0x3760000 0x20000`（顺带 `0x3700000`/`0x3740000`，只碰 slot1）让 cspboot
   回落 slot0，再从 slot0 的 WebUI 把 `e2631-openwrt-v12fix-…-slot1.bin` 写进 slot1。
3. 原厂这套 `br0.forwarding=0` 的做法值得对照我们的 OpenWrt：我们是不是也该在
   `flow_offloading` 关掉时把 v6 转发收在网桥口之外，避免 v6 走 CPU。
4. **（§8 之后新增，优先级最高）** 照原厂 `switch.ko` 的 `ipv6_set` 命令块对齐 v6 的 NPU 表项写法，
   并查清 `hit` 恒 0 的原因：到底是表项没真正落到 ASIC（外部 16 K 槽表缺存储），还是我们的测试
   拓扑（eth1/eth2 拆出 br0、不走交换芯片的 VLAN/PVID）让 ACL 匹配不到。这两种原因的下一步
   完全不同——前者要绕过外部表，后者只要把端口放回交换芯片的 VLAN 里再测一次。
5. 若走"搬 FFE"路线：原厂 FFE 编在内核里（`vendor-vmlinux.bin` 里能查到 `npu_flows`/`retry_left`/
   `activepps` 等格式串，`switch.ko` 只负责把表项写进硬件），要移植的是内核侧那块，不是 `switch.ko`。

---

## 8. 补测（同日晚）：WAN↔LAN 真路由流，NPU 的 v6 表项真的装上了，但数据面没交给硬件

§3/§4 测的是"本机进本机出、出 br0"的形态，`ffeinfo` 全 0。这一轮换成**原厂当路由器、两侧都是真实
以太口**的形态（能力表 `index[1]`：`rxdev: eth*` → `txdev: eth*`，条目里明确带 `ipv6_tcp/udp`）：

```
          (WAN 侧)                                    (LAN 侧)
本机 ens34 ── eth1 [原厂盒子] eth2 ── desktop-a5a2tk2
 fd00:aa::2      fd00:aa::1/64   fd00:1::1/64      fd00:1::b4b4:411a:cbff:1c8
                (br0 里只留 eth3/nbif0/wlan0/apcli5g0，管理走 Wi-Fi 上联 192.168.43.119:2323 + 串口)
```

- `brctl delif br0 eth1/eth2` 把两口拆成三层口；桥 FDB 先验证过归属：主机 MAC 学在 port1=eth1，
  desktop MAC 学在 port2=eth2（所以 WAN 侧=eth1、LAN 侧=eth2 是实测出来的，不是猜的）。
- 两口拆出后原来的链路本地地址会消失（桥端口状态下 addrconf 收掉了），要手工补：
  `ip -6 addr add fe80::e660:4dff:fe86:71ef/64 dev eth1`（eth2 同）。
- 顺手把 `eth1` 的 MAC 改成 `02:11:22:33:44:55`（原来 eth0..eth3 共用 `e4:60:4d:86:71:ef`，
  两口同 L2 会 MAC 抖动），这一步对结果没影响但让邻居表干净。
- 主机侧：`ens34: fd00:aa::2/64` + `ip -6 route add fd00:1::/64 via fe80::e660:4dff:fe86:71ef dev ens34`。
  desktop 的防火墙只放行 `192.168/16`、`fe80::/10`、`fc00::/8`，所以源地址用 ULA（`fd00:aa::2`）。

### 8.1 desktop 的可路由地址怎么来的（Windows 不会自编地址）

原厂 `/bin/dnsmasq` 在这台机器上一执行就卡死（连 `dnsmasq --version` 都不返回，被 ZTE 改过），
所以自己写了一个 33 KB 的静态 ARM 小程序 `ra6`：在指定网卡上每 3 s 发一个 RA（前缀 `fd00:1::/64`、
A=L=1、路由寿命 1800 s），同时用 `AF_PACKET` 抓该口的 IPv6 帧打印收发地址。

- 源码/构建：`.scratch/ra6.c`，工具链复用 `工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-muslgnueabi-gcc -static -Os -s`
- 上机方式：箱子**出不了 TCP**（`wget` 打任何非 127.0.0.1 的地址都 rc=5，`tftp` 超时，没有 `nc`），
  所以用 `printf '\xNN…' >> /tmp/ra6.x` 分 38 块灌进去（`md5sum` 对过），`setsid /tmp/ra6 eth2 fd00:1:: 64 &` 跑起来。
- 结果（抓包原文，`/tmp/ra6.log`）：

```
RX :: -> ff02::1:ff31:3714 icmp6=135 target=fd00:1::c27c:7eb6:c931:3714     <- Windows 对自己新地址做 DAD
RX :: -> ff02::1:ffff:1c8  icmp6=135 target=fd00:1::b4b4:411a:cbff:1c8
RX fd00:1::b4b4:411a:cbff:1c8 -> ff02::1 icmp6=136 target=fd00:1::b4b4:...  <- 无请求 NA，地址生效
RX 2408:8459:1630:2fd3:b4b4:411a:cbff:1c8 -> ff02::1:ff00:c3 ...             <- 它还留着桥接时代办公室路由发的全球地址
```

即 desktop 自动拿到 `fd00:1::b4b4:411a:cbff:1c8`（另有一个临时地址 `fd00:1::c27c:…:3714`），
默认路由指向盒子 eth2 的 LL——**两侧都通了**：`ping -6 -I fd00:aa::2 fd00:1::b4b4:…` 3/3 回。

### 8.2 吞吐（全网都是这条真路由流）

| 协议 | 参数 | 结果 |
|---|---|---|
| TCP | `-t 10` | **928 Mbit/s** sender / 925 receiver，1.08 GB |
| TCP | `-t 45` | 907 / 906 Mbit/s，4.75 GB，237 retr |
| TCP | `-t 60` | 918 / 918 Mbit/s，6.41 GB，297 retr |
| UDP | `-b 900M -t 20` | **716 Mbit/s @ 67.1 kpps**，0/1 336 469 丢 |
| UDP | `-b 900M -t 15` | 682 Mbit/s @ 63.8 kpps |
| UDP | `-b 50M -t 16` | 50 Mbit/s @ 4.66 kpps |

### 8.3 流表原文（这是本轮最关键的一张纸）

```
ipv6_tcp:5 npu:1   curr:5  pps:90260  kbps:979332  npuflowcnt:1  createfail:16
name:ipv6_tcp, enable:2, flows:5, npu_flows:1, slot_size:1024
==flow[   0]: layer:3, npu:1 1, attr:0003, last_used:321026,
   rx:(2779708:3891591200), tx:(2779708:3930507112), pps:86371, bps:977028752,
   lc:0, thresh:0, hit:0, processor:cpu0, rps:cpu0
  src_key:/ eth1/[02:11:22:33:44:55]:[00:0c:29:4b:59:2a]/[fd00:aa::2]:[fd00:1::b4b4:411a:cbff:1c8]:T  0:P  6]/[34637: 5201]
  dst_key:/ eth2/[d8:43:ae:68:18:28]:[e4:60:4d:86:71:ef]/[fd00:aa::2]:[fd00:1::b4b4:411a:cbff:1c8]:T  0:P  6]/[34637: 5201]
  cmd:[tcp_skip+0x0/0x3c]->[ipv6_set+0x0/0x1a0]->[eth_set+0x0/0x6c]->
  passed dev:[eth1]->[eth2]-> mtu[1500], ll_rs[32] gw[fd00:1::b4b4:411a:cbff:1c8],
  npu activepps[20] retry_left[0]
  ct@c55ceee0: l3[ipv6] l4[tcp] timeout[7499] [--REPLIED] [--ASSURED] [ORIGI DIR]
```

UDP 同样（`ipv6_udp:3 npu:2`，`P 17`，`pps:66241`，`src_key …/[34955: 5201]`，
`npu_flows:2`），`cmd` 链同族。

**所以：v6 的 NPU 表项在这块板上装得上**，而且是拿我们逆出来的那套 ACL 命令族
（`ipv6_set` + `eth_set`，`hf_set_l3_entry`/`aclBuildHashAndAclRule`）写进去的，端口/MAC/五元组
都是真实值。**上一版"v6 落不到 NPU / 这块板不可能"的结论错了。**

### 8.4 但数据面没交给硬件（四条独立证据）

1. **CPU 是逐包在处理**：空闲基线 `sirq 0.3%`；流量中 `sirq 30–44%`；而且 CPU 占用随包速率线性缩放
   ——4.66 kpps→3.6%、63.8 kpps→29.5~34.6%、86 kpps→39.8~44%（≈4.7–7.7 µs/包）。固定开销不会这样。
2. **反向没有 ACK 的 UDP 也一样**（63.8 kpps→30% sirq），排除了"CPU 只是在处理回程 ACK"。
3. **CPU 侧端口计数 1:1 跟着流量走**：60 s TCP 期间 `eth2` 的 tx 字节按 1.04 / 1.01 / 0.93 Gbit/s
   每段增长（32 位回绕已折算）；而 §3 的纯交换流量下这些计数几乎不动（537 MB 只增 789 B），
   说明它们是 **CPU 侧计数、不是交换芯片 MIB**——即 CPU 把每一帧都发了一遍。
4. **流表里 `hit:0`**：整条流 3.9 GB / 278 万包走完，硬件命中计数还是 0。

配套的另一个信号：`createfail` 一路涨——`0 → 4 → 10 → 13 → 15 → 16 → 20 → 21 → 25 → 27 → 30 → 31 → 49`，
每建一条流都要失败好几次才成功一次。这跟上一轮 RE 的结论（v6 表项要写外部 16 K 槽表，而那块地址
在这颗芯片上是启动 ROM，写就 imprecise abort）吻合：**驱动侧装上了，硬件侧没生效**。

### 8.5 对项目的直接意义

原厂这套 v6"加速"= **NPU ACL 分类打标（`tcp_skip`+`ipv6_set`+`eth_set`）+ 内核 FFE 五元组软快转**，
CPU 不退出数据面；它把 v6 转发压到 ~4.7 µs/包（900 Mbit/s 用 0.4 核），功劳在 FFE，不在 ASIC。

于是我们要做 v6 加速只有两条路：

1. **把 FFE 这套软快转搬过来**（纯软件，靠 ACL 把流标出来让包走近路）：确定能做，天花板 ≈ 900 Mbit/s / 0.4 核；
2. **把 ASIC 的 v6 L3 转发真正打通**：先照着原厂 `switch.ko` 里 `hf_set_l3_entry`/`aclBuildHashAndAclRule`
   的 `ipv6_set` 命令块逐字节对齐（这是原厂自己写过的、有真实 v6 流的样本），再看它到底把表项放哪、
   为什么 `hit` 恒 0。

> **更正（同晚）**：这里原先写的是"原厂自己都没打通，`createfail` 风暴就是证据"——那句是错的。
> `createfail` 的成因已经追到底，见 §8.7：它是 tm.ko 里"按源 MAC 查子网 ID"查不到时返回的 `-12`，
> 触发条件是我测试时把 eth1 的 MAC 改成 `02:11:22:33:44:55`，跟硬件表项、跟外部哈希表都无关。

### 8.6 本轮临时改动（盒子现在就是这个测试拓扑，全部运行态、没写 flash）

```sh
# 已做：
brctl delif br0 eth1 ; brctl delif br0 eth2
ip link set eth1 address 02:11:22:33:44:55
ip -6 addr add fd00:aa::1/64 dev eth1 ; ip -6 addr add fd00:1::1/64 dev eth2
ip -6 addr add fe80::e660:4dff:fe86:71ef/64 dev eth1  # eth2 同
ip addr add 10.99.0.1/24 dev eth1 ; ip addr add 10.99.1.1/24 dev eth2
ip addr add 10.200.0.1/24 dev pon ; ip -6 addr add fd00:ff::1/64 dev pon ; ifconfig pon up
for f in /proc/sys/net/ipv6/conf/*/forwarding; do echo 1 > $f; done
ip6tables -I FORWARD 1 -j ACCEPT
setsid /tmp/ra6 eth2 fd00:1:: 64 > /tmp/ra6.log 2>&1 &
# 还原：
killall ra6
ip6tables -D FORWARD -j ACCEPT
brctl addif br0 eth1 ; brctl addif br0 eth2
ip addr del 10.99.0.1/24 dev eth1 ; ip addr del 10.99.1.1/24 dev eth2
ip -6 addr del fd00:aa::1/64 dev eth1 ; ip -6 addr del fd00:1::1/64 dev eth2
ip link set eth1 address e4:60:4d:86:71:ef   # 回到原 MAC
# 管理通道：192.168.43.119:2323（走 Wi-Fi 上联，这是把 eth1/eth2 拆出 br0 后唯一的网络入口）或串口
```

主机侧临时：`ens34` 加了 `10.99.0.2/24`、`fd00:aa::2/64`、`ip -6 route add fd00:1::/64 via fe80::e660:4dff:fe86:71ef dev ens34`。

> 状态：`eth1` 的 MAC 已按 §9.4 改回原厂 `e4:60:4d:86:71:ef`（地址、转发开关都还在），其余临时改动仍在。

## 9. createfail 一路上涨到底是怎么回事（把 §8.5 那句"原厂没打通"收回）

**一句话**：`createfail` 数的是 FFE 向 ACL/NPU 装表项的**失败返回次数**；这次的 `-12`(ENOMEM) 来自
tm.ko 里"按源 MAC 查子网 ID"这一步——**这条流的源 MAC 不在那张 16 项的 ONU MAC 表里**。
这是软件在 ACL/哈希表编程**之前**的干净返回，一次硬件写都没发生；而那个查不到的 MAC 正是我测试时手工改的。

### 9.1 计数器 == 日志行数，1:1（同一时刻实测，两次）

`timeout -t 6 cat /dev/logger_main > /tmp/lg.txt`（6485 行，完整落盘后再数，别用 `cat|grep -c` 边读边数，
环形日志读得慢会被 `timeout` 截断成假数）：

| 观测时刻 | `createfail` | `hardfast_session_add failed` 行 | `hf_set_l3_entry FAIL` 行 | `invalid source mac addr` 行 |
|---|---|---|---|---|
| 第一轮（首批失败） | 51 | 51 | 51 | 51 |
| 追查后（A/B 之后） | 57 | 57 | 57 | 57（全部是 `02:11:22:33:44:55`） |

也就是说 `createfail` 没有别的来源：每失败一次，`switch.ko` 打两行、tm.ko 打一行，计数器 +1。

### 9.2 `-12` 的产生位置与调用链（地址均来自 tm.ko 反汇编）

```
FFE(内核 ffe_*)
 └ switch.ko: sw_acl_l3_hardfast_session_add (0x28e0)
    └ tm.ko: zte_api_fast_l3_session_add (0x5ce74)
       └ tm_add_acl_flow_rule (0x51a30)
          ├ 第一步: addFlowOperInfo (0x4fe70, 调用点 0x51a9c)
          │   └ findSubnetIdBySrcMac (0x4fdd8, 调用点 0x50438)
          │      └ 用 pp_pm_get_onu_mac_ram_info 取 0..15 共 16 项 MAC，memcmp 6 字节
          │         └ 找不到: printk("invalid source mac addr %02x:...")(串@0x271e8)
          │                     然后 mvn r0,#11 (=0xFFFFFFF4=-12) 返回 (0x4fe58)
          ├ 失败即解锁返回 r4，不打印 (0x51aa0 subs / 0x51ab0 mov r0,r4)
          │
          ├ 只有 addFlowOperInfo 返回 0 才会走到:
          │   addAclRule (0x4f3a8, 调用点 0x51aec) 或 tm_acl_fast_add (0x51964, 0x51b18)
          │     └ tm_acl_fast_add_v4v6 → cla_set_hash_table / cla_set_external_hash_table
          └ 返回 -12
       └ 0x5d184 `subs sl,r0,#0; bne 0x5d3fc` → 解锁、`mov r0,sl` 原样返回 -12（不打印）
 └ switch.ko: 打印 "[sw_acl_l3_hardfast_session_add] zte_api_fast_l3_session_add failed -12"
 └ hf_set_l3_entry: 打印 "[hf_set_l3_entry] FAIL"，返回 0
 └ FFE: createfail++，按 retry_left 稍后重试
```

关键：失败发生在 `addFlowOperInfo`，**比任何 ACL/哈希表编程都早**（`addAclRule`/`tm_acl_fast_add` 是它成功之后才走的）。

### 9.3 为什么"写外部 16K 槽表 = 写启动 ROM"这个推断是错的

- 失败点在 `addFlowOperInfo` 阶段，`cla_set_hash_table`/`cla_set_external_hash_table` 根本没被调到；
- 日志里没有任何 Oops / data abort / bus error，只有 9.1 那三行正常 printk；
- `cat /sys/devices/platform/tm/ppCla/showaclinfoall` 打出的 ACL 哈希表只有 **12 项**在用（上限 16000，`This rule mode is full`），离满得很远；
- 真是"写启动 ROM"的话，现象应是总线异常/挂死，而不是一个带返回码的 `-12`。

### 9.4 A/B 实测：改 MAC → 涨；改回原厂 MAC → 停

同一拓扑、同一条 10 s 的 v6 路由流（host → desktop iperf3）：

| eth1 MAC | 10 s 吞吐 | createfail 增量 | `invalid source mac` 增量 | NPU 表项 |
|---|---|---|---|---|
| `02:11:22:33:44:55`（我手工改的） | 918 Mbit/s | **+4** | +6（含前一次探测的 2） | 装不上 |
| `e4:60:4d:86:71:ef`（原厂） | 927 Mbit/s | **0** | **0** | `ipv6_tcp:4 npu:2`，`npuflowcnt:2` |

改回原厂 MAC 后 createfail 立刻停涨（57 不动），同时 NPU 表项正常装上——这一条同时否掉了
"v6 表项装不上"和"表项写硬件就异常"两种说法。

### 9.5 "每建一条流要失败好几次才成功一次"要这么读

计数器数的是**尝试次数**，不是流的条数：

- 源 MAC 认不出来的流（就是我改出来的那个口）**每次重试都失败**，失败是确定的，于是同一条流会不断 +1；
- 同一时间源 MAC 能被认出来的流（eth2 / 原厂口）**一次就装上**，不产生 createfail。

所以 0→4→10→13→…→49→51 是这批"坏 MAC"流被反复重试的累计，不是"同一条流试了几次终于成功"。

### 9.6 还剩的真问题（`hit:0`，与 createfail 无关）

B 组里表项装上了（`ipv6_tcp:4 npu:2`）而数据面仍然走 CPU（§8.4 四条证据不变）。下一轮要查的是
ACL 在端口/VLAN 维度上是否真的匹配：测试拓扑把 eth1/eth2 拆出了 br0，端口 PVID/成员关系跟原厂不一样，
这一层不匹配同样会让 `hit` 掉到 0。

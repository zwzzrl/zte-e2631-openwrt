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
| **路由路径**（`ip6_forward`，本机进本机出） | 263 Mbit/s 突发 500/500 全转；373 Mbit/s 突发 20000 包只转 7672（≈**143 Mbit/s**），CPU 打满；`ffeinfo` 里 `curr:0 / ipv6_udp:0 / npu:0` | **纯 CPU 软件转发**，FFE / NPU 都没参与 |
| 出厂默认的路由转发 | `net.ipv6.conf.br0.forwarding = 0`（`all=1`），即**默认不转发** | — |

所以"原厂的 IPv6 转发"在这台机器上真实存在的只有两种：**硬件二层转发（线速）** 与
**CPU 三层转发（百兆级）**。这跟 `npu_cap` 矩阵完全对得上：v6 只支持
`pon*/gpon*/ptm0 ↔ eth*/wlan*` 的 tcp/udp/mc，`eth* → eth0` 只有 `ipv4_raw`，
所以 eth↔eth 的 v6 永远落不到 NPU 上。

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

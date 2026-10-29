# 原厂 IPv6 硬件加速：实测事实（2026-10-30，取自运行中的原厂系统）

> 取证对象：ZXHN E2631（ZX279128S）**原厂系统**，跑在 flash slot1
> （镜像 = 原厂 rootfs 原字节 + 我们追加的 3 个节点，未改任何原厂逻辑），
> `softVersion=V1.0.0.8B8.9900`，`loadtype=3`。
> 本文所有数字与字符串都是**在这台机器的运行态**上读出来的，或从它自己的
> `etc/autokernelconf` 里抄出来的，逐条给了取证命令。
> 凡属我的推断都标了「推断」。

---

## 0. 一句话结论

原厂给 IPv6 开了**两层**加速，且这两层是**可分别开关**的：

| 层 | 名字 | 形态 | 对 IPv6 的覆盖 |
|---|---|---|---|
| 软件快转 | **FFE**（fast forwarding engine，编进内核） | 基于 conntrack 的 5 元组快转，跑在 CPU 上 | ipv4/ipv6 的 raw / tcp / udp / mc **全支持** |
| 硬件卸载 | **NPU**（挂在 switch.ko 的 ACL L3 hardfast 上） | 真正把流下发进交换芯片的 ACL 表 | ipv6 **只支持 tcp / udp / mc**；**raw 不支持**；**NAT66 不支持** |

对外的控制面只有一棵 proc 树 `/proc/zte/sys/ffe/`，按协议给 0/1/2 三档：
`0=全关`、`1=只开 FFE`、`2=FFE + NPU`。`ipv6_tcp`/`ipv6_udp`/`ipv6_mc`/`ipv6_raw`
各有一个这样的旋钮。

**这条对我们的意义**：原厂自己也没把 IPv6 NAT66 交给硬件
（`CONFIG_CSPKERNEL_NPU_SUPPORT_NAT6 is not set`），而我们 OpenWrt 那边
PPE 出问题的正是 IPv6+NAT 这条路径 —— 原厂不是"做对了"，是**绕开了**。

---

## 1. 控制面：`/proc/zte/sys/ffe/` 完整目录

`ls -l /proc/zte/sys/ffe/`（实测）：

```
cmd          ffeinfo      hash/        npu/         scripts/     sharedata/
ipv4_mc ipv4_raw ipv4_tcp ipv4_udp   ipv6_mc ipv6_raw ipv6_tcp ipv6_udp   learnlist
```

* `cmd`：唯一的总命令入口（`echo <命令> <参数> > cmd`）。内容就是帮助文本。
* `ffeinfo`：全局状态与计数（见 §3）。
* `ipv4_* ipv6_*`：各协议当前已加速流表（读出来格式化打印；空表时 0 字节）。
* `npu/`：`npu_cap`（能力矩阵，§4）、`npufilter`（钩子链，§5）、
  8 个 `<协议>_npu`（NPU 侧流表）。
* `hash/`：8 个 `<协议>_hash`（FFE 哈希桶）。
* `scripts/`：23 个可执行 sh 壳子，全部只是 `echo <名字> $1 > /proc/zte/sys/ffe/cmd`，
  方便手敲。**它们不含默认值**，读它们读不出当前档位。
* `sharedata/ffe_path`、`ffe_cmd`：空文件。
* `learnlist`：FFE 学习表。

## 2. `cmd` 的全部命令与语义（原文）

```
npu------npu 0/1 (off/on npu)
npudd------npudd 0/1 (off/on npu drv debug info)
npuretry------npuretry xxx (set npu max retry cnt)
pps------pps cap_index layer type pps
ipv6_tcp------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv6_udp------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv4_tcp------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv4_udp------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv6_mc------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv4_mc------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv6_raw------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
ipv4_raw------proto 0/1/2 (0=all disable, 1=only enable ffe, 2=enable ffe and npu)
mcastdebug------show multicast rtp flow errors
layer_enable------layer enable 0 1 2 3
acc_thresh------acc_thresh 0 n
icmp_acc------icmp_acc 0 1
no_delete------no_delete 0 1
timer------timer interval cpuid
flowcnt------flowcnt xxxx
ffe------ffe 0/1 (off/on ffe)
del------del flows
show------show typemap/devinfo
rps_tx_queue_max------rps_tx_queue_max xxxx (set rps max queue length)
rps_cpu_map------rps_cpu_map xxxx (set rps cpu-mask, 0 mean disable rps)
```

要点：
* 协议清单里**有** `ipv6_raw` 这一档（`0/1/2`），但硬件能力表里没有它（§4），
  所以给它设 2 也只能落到 FFE —— 属"有旋钮、无硬件"。
* `layer_enable 0..3` 与 §4 能力表里的 `layer[2]`/`layer[3]` 是同一套层号。
* `pps <cap_index> <layer> <type> <pps>` 能改 §4 表里的 pps 门限，说明那些
  pps 是**可调的策略值**，不是硬件死值。

取证：`cat /proc/zte/sys/ffe/cmd`；`cat /proc/zte/sys/ffe/scripts/ipv6_tcp`。

## 3. `ffeinfo` 实测全文

```
version:3.6.6
enable:1
max flow:28128
curr:0
high_wm:26370
hint_wm:0
overflow_cnt:0
timeout:45 sec
no_delete:0
icmp_acc:0
acc_thresh:0
layer_enable:3
proto flow cnt:
    ipv4_raw:0 npu:0
    ipv6_raw:0 npu:0
    ipv4_mc:0 npu:0
    ipv6_mc:0 npu:0
    ipv4_udp:0 npu:0
    ipv4_tcp:0 npu:0
    ipv6_udp:0 npu:0
    ipv6_tcp:0 npu:0
statics:
    rx pkts:0 / rx bytes:0 / rx errors:0 / tx pkts:0 / tx bytes:0
    tx stops:0 / tx filters:0 / tx errors:0 / pps:0 / kbps:0 Kbps
    slowpathcnt:546  slowpathpps:0
npuinfo:
    enable:1
    npuflowcnt:0
    createfail:0
    deletefail:0
backlog:
    qlen:0  napi state:0  dropped:0
RPS info:
    cpu_cnt:[2]  cpu_map:[3]  tx_queue_max:[1024]
RPS statics:
    CPU:[0] flows:[0] pkt_dropped:[0] pkt_processed:[0] rcv_rps:[0]
    CPU:[1] flows:[0] ...
```

读得出来的硬数字：
* **FFE 开着（enable:1）、NPU 开着（npuinfo enable:1）、layer_enable:3**（0..3 全开）。
* 流表容量 **28128** 条，高水位 **26370**，闲置老化 **45 秒**。
* RPS 开在 2 个 CPU 上（`cpu_cnt:[2] cpu_map:[3]` = CPU0+CPU1），单队列上限 1024。
* 所有协议流计数都是 0 —— 因为这台机器当时**没有任何在转发的 IPv6 业务**
  （它自己的 WAN 都没起来，见 §8），所以这张表里不会有真实 v6 流。

取证：`cat /proc/zte/sys/ffe/ffeinfo`。

## 4. NPU 能力矩阵 `npu/<npu_cap>`（251 行，完整）

类型号是**全局**的（两张表共用）：
`1=ipv4_mc  2=ipv4_raw  3=ipv4_tcp  4=ipv4_udp  5=dslite_tcp  6=dslite_udp
7=ipv6_mc  8=ipv6_raw  9=ipv6_tcp  10=ipv6_udp`
（1..7 与 9、10 都能在表里点名出现；**8 全表未出现**。）

按 `flow_feature index` 分组，每组先声明"从哪个设备进、到哪个设备出"，再列能力：

**index[0]** —— `rxdev: pon0 pon gpon0 epon0 ptm0` → `txdev: eth*[0] wlan*[0]`（WAN→LAN）
```
layer[2] type3  ipv4_tcp  pps:0  frag:0  cap1:0000001c cap2:00001c1f cap3:00000018 cap5:0000181c cap9:00000018 cap10:00000018
layer[2] type4  ipv4_udp  同上
layer[2] type9  ipv6_tcp  pps:0  frag:0  cap1:0000001c cap2:00001c1f cap3:00000018 cap7:0000181c cap9:00000018 cap10:00000018
layer[2] type10 ipv6_udp  同上
layer[3] type3  ipv4_tcp  pps:20 frag:1  cap1:0000001f cap2:00001c1f cap3:0000001e cap5:00001c1c cap9:0000001c
layer[3] type4  ipv4_udp  pps:0  frag:1  cap1:0000001f cap2:00001c1f cap3:0000001e cap5:00001c1c cap10:0000001c
layer[3] type5  dslite_tcp pps:256 frag:1 cap1:0000001f cap2:00001c1f cap3:0000001e cap5:00001c1c cap7:00001c12 cap9:0000001c cap10:0000001c
layer[3] type6  dslite_udp pps:256 frag:1 同上
layer[3] type9  ipv6_tcp  pps:20 frag:1  cap1:0000001f cap2:00001c1f cap3:0000001e cap7:00001c1c cap9:0000001c
layer[3] type10 ipv6_udp  pps:0  frag:1  cap1:0000001f cap2:00001c1f cap3:0000001e cap7:00001c1c cap10:0000001c
```

**index[1]** —— `rxdev: eth* wlan*` → `txdev: eth*[0] pon[0] ptm0[0]`（LAN→WAN）
协议集合与 index[0] 相同；差别只有：
```
layer[3] dslite_tcp/udp  pps:128（index[0] 是 256），cap3:0000001d，cap7:00000011
layer[3] ipv4/ipv6_tcp/udp cap3:0000001d
```

**index[2]** —— `rxdev: eth*` → `txdev: eth0[0] ptm0[0]`（东西向）
```
layer[2] type2 ipv4_raw pps:0 frag:0  cap1:0000001c cap2:00001c1f cap3:00000018 cap5:0000181c cap9:00000018 cap10:00000018
layer[3] type2 ipv4_raw 同上
```
→ **以太网口之间只有 IPv4 raw 能上 NPU；IPv6 一点都不能。**

**index[3]** —— `rxdev: eth0 pon ptm0` → `txdev: eth*[0]`
```
layer[2]/[3] type2 ipv4_raw，能力同上
```

**index[4]** —— `rxdev: pon0 pon gpon0 epon0 ptm0` → `txdev: eth*[0]`（组播）
```
layer[2]/[3] type1 ipv4_mc pps:0 frag:0 cap1:0000001f cap2:00001c1f cap5:00001c1c
layer[2]/[3] type7 ipv6_mc pps:0 frag:0 cap1:0000001f cap2:00001c1f cap7:00001c1c
```

### 从这张矩阵能确定的三件事

1. **IPv6 只有 mc/tcp/udp 三类能进 NPU**，`ipv6_raw`（ICMPv6、ESP 等）没有能力项。
2. **端口组合是被写死的**：入方向只认 `pon*/gpon*` 或 `eth*`；`eth*→eth0` 只放
   IPv4 raw。原厂设想的 IPv6 业务形态就是 `WAN口 ↔ eth/wlan`。
3. `layer[3]` 的 cap 普遍比 `layer[2]` 宽（`1f` vs `1c`），`frag:1` 只在 layer[3]
   打开 —— 即"分片包的 IPv6 要 layer 3 才卸"。

取证：`cat /proc/zte/sys/ffe/npu/npu_cap`（251 行）。

## 5. NPU 钩子链 `npu/<npufilter>`

```
name:npu_cap_filter                     hookpos:0 hooknum:0 priority:-200
name:npu_cap_filter                     hookpos:0 hooknum:1 priority:-200
name:flow_pps_url_filter                hookpos:0 hooknum:2 priority:400
name:flow_ipv6_nat_filter               hookpos:0 hooknum:2 priority:400
name:mc_front_create                    hookpos:0 hooknum:2 priority:600
name:flow_retry_filter_frontend_create  hookpos:0 hooknum:2 priority:800
name:mc_backend_outdev                  hookpos:1 hooknum:1 priority:0
name:flow_retry_filter_backend_create   hookpos:1 hooknum:2 priority:800
```

* `npu_cap_filter` 优先级最低（-200），先按 §4 的能力表筛一遍。
* **`flow_ipv6_nat_filter`** 单独挂在转发链上 —— 与
  `CONFIG_CSPKERNEL_NPU_SUPPORT_NAT6 is not set`（§6）互相印证：
  IPv6 NAT 流在这里被挡在 NPU 之外，只能走 FFE / 慢路径。

## 6. 内核编译开关（原厂自己的 `etc/autokernelconf`）

这是原厂**固件里带的**内核配置，不是猜的：

```
CONFIG_CSPKERNEL_FFE=y
CONFIG_CSPKERNEL_FFE_NPU=y
CONFIG_CSPKERNEL_FFE_MCAST4=y
CONFIG_CSPKERNEL_FFE_MCAST6=y
CONFIG_CSPKERNEL_FFE_RPS=y
CONFIG_CSPKERNEL_BRIDGE_EBT_FFE_T=y
CONFIG_CSPKERNEL_ND_AGENT=y
CONFIG_CSPKERNEL_IPV6_SLAAC=y
CONFIG_CSPKERNEL_MLD_MODULE=y
CONFIG_CSPKERNEL_MLD_MTOU=y
CONFIG_CSPKERNEL_IPV6ADDR_NOTIFY_ADEV=y
CONFIG_CSPKERNEL_IP_NF_TARGET_URLACCEPT=y
```

关掉的（每一条都是有意为之）：

```
@ CONFIG_CSPKERNEL_NPU_SUPPORT_NAT6   is not set   ← NPU 不做 NAT66
@ CONFIG_CSPKERNEL_IP6_BRIDGE         is not set   ← IPv6 桥接不给加速（必须路由）
@ CONFIG_CSPKERNEL_FFE_8TUPLES        is not set   ← 只做 5 元组
@ CONFIG_CSPKERNEL_FFE_FASTCSUM       is not set
@ CONFIG_CSPKERNEL_FFE_TCP_AUTOACK    is not set
@ CONFIG_CSPKERNEL_FFE_LOCAL          is not set
@ CONFIG_CSPKERNEL_FFE_FILTER         is not set
@ CONFIG_CSPKERNEL_FFE_SKIP_PORT      is not set
@ CONFIG_CSPKERNEL_FFE_BRIDGE_FASTPATH is not set
@ CONFIG_CSPKERNEL_FFE_STATS_BY_MAC   is not set
@ CONFIG_CSPKERNEL_FFE_L2_NETLOOP     is not set
@ CONFIG_CSPKERNEL_IPV6FRAG_MODE      is not set
@ CONFIG_CSPKERNEL_NPU_MC_STATS_MONITOR is not set
@ CONFIG_CSPKERNEL_APP_ACCELERATION   is not set
```

IPv6 NAT 本身是**有**的（走软件）：`CONFIG_IP6_NF_NAT=y`、
`CONFIG_IP6_NF_TARGET_MASQUERADE=y`、`CONFIG_NF_NAT_MASQUERADE_IPV6=y`、
`CONFIG_IPV6_MULTIPLE_TABLES=y`。这与它 iptables 里现成的 `nat66_post`、
`dslite` 链完全对得上。

⚠ 一个**不能误读**的开关：`@ CONFIG_CSPKERNEL_HARDWARE_SPEEDUP is not set`
出现在 `MLDV2_SRC_FILTER`/`MVLAN`/`MLD_MTOU` 之间 —— 它属于
`CSPKERNEL_MULTICAST_MODULE` 那一块（组播加速），**不是**全局加速主开关。
全局加速由 `CSPKERNEL_FFE` + `CSPKERNEL_FFE_NPU` 决定。

取证：`grep -E 'CSPKERNEL' etc/autokernelconf`
（源自 `03-固件备份/vendor-rootfs-decrypted/rootfs-tree/etc/autokernelconf`）。

## 7. 硬件卸载的落地实现（模块符号）

`switch.ko`（224808 B，已在主机上：
`03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/switch.ko`）：

```
D npu_cap_array            0x7b0   ← 就是 §4 打印的那张表
D npu_ipv6_l3tcpup_cap     0x2c    D npu_ipv6_l3tcpdown_cap
D npu_ipv6_l3udpup_cap     0x2c    D npu_ipv6_l3udpdown_cap
D npu_ipv6_l2up_cap        0x2c    D npu_ipv6_l2down_cap
D npu_ipv6_mc_down_cap     0x2c
D npu_dslite_up_cap        0x2c    D npu_dslite_down_cap
T sw_acl_l3_hardfast_session_add / _del / _entry_show / _flowstatus
t hf_set_l3_entry / hf_del_l3_entry / hf_query_l3_status / hf_entry_police2
T l3_hardfastReg / l3_hardfastUnReg
T npu_hff_register
```

`tm.ko`（1270520 B）引用 `zte_api_fast_l3_session_{add,del,stat,clr,use,flow_statis}`
和 `tm_ipv6table_{add,delete,lookup,print}_set` —— 即"**业务层发一条 IPv6 流 →
tm.ko 记账 → switch.ko 下发 ACL L3 hardfast 条目**"这条链。

每条 `npu_*_cap` 都是 0x2c=44 字节，与 §4 每组每类型一行的 44 字节吻合。

取证：`arm-openwrt-linux-nm -S switch.ko | grep -iE 'npu|hardfast'`。

## 8. 当时的运行态（为什么计数全是 0）

原厂系统在这台机器上的实际网络状态：

```
br0  inet 192.168.43.119/24      （br0 上还挂着 192.168.5.1 的 secondary）
default via 192.168.43.1 dev br0
192.168.5.0/24  dev br0 src 192.168.5.1
192.168.43.0/24 dev br0 src 192.168.43.119
```

即：**原厂把 br0 既当 LAN 又当 WAN 用**，从上游 192.168.43.1 拿 DHCP。
它自己还跑着 `dhcp6c`（`DEV.V6DHCP.Client2` 在 `EXCH_STATE_SOLICIT` 反复超时），
说明 IPv6 WAN 是"想连、没连上"。所以 §3 里 v6 流计数全 0 是**环境**造成的，
不是加速没开。

顺带一个会影响所有自动化取证的原厂行为（见 §9.3）：
它的 nat 表里有

```
SNAT all -- 192.168.5.0/24 192.168.5.0/24 to:192.168.43.119
```

这条把**去 192.168.5.0/24 的包源地址改成 192.168.43.119**。后果：
从这台机器往 `192.168.5.50:8099` 连，抓包看到的是
`192.168.43.119 -> 192.168.5.50 [SYN]`，对端若只把 192.168.5.0/24 配在
同一张网卡上，回程会走默认路由 → 永远回不去 → 连接停在 `SYN_SENT`。
**这就是我们那个"自动推流一直没到"的真正原因，与推流工具无关。**

取证：`ip -4 addr; ip route`（原厂 busybox 有 `/bin/ip`）；
`iptables -t nat -S POSTROUTING`。

## 9. 这一轮新学到的取证手段（以后直接用）

### 9.1 原厂 busybox 的 `timeout` 要 `-t`
`timeout -t 3 cat /dev/logger_main`。写成 `timeout 3 cat …` 会报
`timeout: can't execute '3'`。我先前那次"内核日志采空"就是这个原因，
**不是 kmsg 被谁吃掉了**。

### 9.2 内核日志可以从 `/dev/logger_main` 读
`logcat`(pid 601) 持有 `/proc/kmsg`；但它同时把记录写进 `/dev/logger_main`，
而**这个设备节点是可以读的**，且每次打开都会**回放整段开机以来的积压**
（实测一次读出 477691 B）。记录是二进制结构体（模块名/时间/文件名/函数名/消息，
NUL 分隔），搬回主机解析最稳：

```
# 箱子上
timeout -t 3 cat /dev/logger_main > /tmp/lg.bin
hexdump -v -e '16/1 "%02x" "\n"' /tmp/lg.bin     # 只搬 ASCII，避免二进制过终端
# 主机上：把 hex 还原成 bytes，再 re.findall(rb'[\x20-\x7e]{5,}') 取字符串
```

⚠ 不要用 box 上的 `tr -c "[:print:]"`：busybox 1.17 的 `tr` 不认字符类，
会把它当字面量，结果把每个字母都拆成独立行（"npu" 再也 grep 不到）。

### 9.3 `/proc/tm/shell` 支持**按地址**读内核内存
`echo <符号名>` 会 `unknown symbol` —— 它查的是自己的符号表，不是 kallsyms。
但按**地址**可以：

```
echo "-s c07b90e8"       > /proc/tm/shell   → address info: ffe+0x0/0x4
echo "-l -c 128 cdc04a00" > /proc/tm/shell   → 打印 128 个 dword 的 hexdump
```

输出走 printk → `/dev/logger_main`（§9.2）。地址从 `/proc/kallsyms` 取
（`grep -E "(^| )ffe_" /proc/kallsyms`）。

实测有效：`ffe` 是 4 字节符号，值 `0xcdc04a00`；解引用后
`0xcdc04b34 = 0x00006de0 = 28128`（= `max flow`）、
`0xcdc04b18 = 0x00006702 = 26370`（= `high_wm`）、
`0xcdc04a5c..0xcdc04a98` = 16 个指向 `ffe_layer_seq_array`(c0661cc0) 的指针
（16 = 协议槽数）。**三个锚点都与 `ffeinfo` 自报的数字吻合**，说明读的就是那个结构。

### 9.4 `show` 命令的输出没进日志缓冲
`echo show > /proc/zte/sys/ffe/cmd` 返回 0，但随后 3 秒窗口的日志里没有它的输出
（只有 `bindlocal`/`dhcp6c`/`wlan` 这些用户态记录）。判断它只走了 console。
以后要看它的结果，得先解决 console（§10）。

## 10. 还没解出来的（诚实标注）

1. **各协议当前实际档位（0/1/2）没能直接读出来。**
   `scripts/*` 只是写入口，`cat` 读不出值；`ffeinfo` 只报计数不报档位；
   `/proc/zte/sys/ffe/ipv6_*` 是流表不是配置。能确定的是**总开关**
   （`enable:1`、`npuinfo enable:1`、`layer_enable:3`）。
   在 `ffe` 结构里还没定位到那 8 个档位字节（`0xcdc04ab4` 的 `59705970`
   与人眼不可读的 `0x10` 填充无法确定含义）。
2. **console 依然是死的**（原厂内核没有 printk console 输出），所以
   `show`/内核 oops 之类只走 console 的信息还是看不到。

## 11. 对 OpenWrt 移植的直接含义

* 我们 OpenWrt 里 `zx279128s-ppe` 干的活，在原厂对应 **NPU 那一层**；
  原厂的 FFE 是纯软件快转，我们在 OpenWrt 里有 conntrack+flowtable 等价物。
* **原厂也从不让 NPU 碰 IPv6 NAT**（`NPU_SUPPORT_NAT6` 关 + `flow_ipv6_nat_filter`）。
  所以"IPv6 + NAT 场景下硬件卸载"这件事，原厂固件根本不提供 —— 我们先前
  PPE-IPV6-TCP 那个死机，错在**我们比原厂多走了一步**（把 v6 NAT 流塞进 PPE）。
* 原厂对 IPv6 的加速形态是：**只有 routed 的 v6 TCP/UDP/组播能进硬件，
  且必须 WAN↔LAN 方向**；`ipv6_raw` 与 eth↔eth 的 v6 一律软件。
  这给我们的"任意有线口可作 WAN"目标提了个醒：一旦入方向不是 `eth*`
  （例如走 VLAN 子接口、或桥内东西向），原厂那套能力表就没有对应项 ——
  我们的 PPE 若按端口组合白名单过滤，要自己定这张表。
* 分片包的 v6 只在 `layer[3]` 才卸（`frag:1`），`layer[2]` 明确 `frag:0`。
  我们做分片处理时可以照这个分层。

---

## 附：本文所有取证命令速查

```sh
# 控制面
cat /proc/zte/sys/ffe/cmd
cat /proc/zte/sys/ffe/ffeinfo
cat /proc/zte/sys/ffe/npu/npu_cap
cat /proc/zte/sys/ffe/npu/npufilter
ls -l /proc/zte/sys/ffe/{npu,hash,scripts,sharedata}/

# 内核编译开关（离线）
grep -E 'CSPKERNEL|IP6_NF|NF_NAT' 03-固件备份/vendor-rootfs-decrypted/rootfs-tree/etc/autokernelconf

# 模块符号（离线）
NM=工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-nm
$NM -S 03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/switch.ko | grep -iE 'npu|hardfast'

# 内核内存（运行时，只读，安全）
grep -E '(^| )ffe_' /proc/kallsyms
echo "-l -c 128 cdc04a00" > /proc/tm/shell        # 地址从上面拿
timeout -t 3 cat /dev/logger_main > /tmp/lg.bin   # 输出在这里
hexdump -v -e '16/1 "%02x" "\n"' /tmp/lg.bin
```

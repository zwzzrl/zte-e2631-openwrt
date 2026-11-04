---
title: 台架重建与构建刷机 runbook
date: 2026-11-04
status: done
topic: test-bench
summary: 空盒→能打流：台架重建、两条构建路线、刷机与零手工写入验法；路线 B 只对本机成立。
evidence: overlay-removed.txt:20-22, overlay/target/linux/sanechips/base-files/etc/board.d/02_network:13, docs/operations/flashing.md:14-52, .scratch/ffe-fastpath/flash-land/20261104-0430/logs/
supersedes:
---

# 台架重建与构建刷机 runbook

> 读者：要在这台机器上**从一台空盒走到"台架能打流"**的人。
> 只想知道怎么刷 → [flashing.md](flashing.md)；只想调 FFE 参数 → [ffe-tuning-parameters.md](../reference/ffe-tuning-parameters.md)。
> 命令后面的括注是来源（`文件:行` 或历史产物一行）；核不到的写 **【未核】**，别当结论用。

**路径缩写**（本文所有命令都用这几个符号，不再写本机绝对路径）：

| 缩写 | 指代 | 在仓库里？ |
|---|---|---|
| `$R` | 本仓库根 | 是 |
| `$O` | 构建树 `openwrt/`（`build-kernel.sh` 把它挂成 `/openwrt`） | 否 |
| `$L` | 本机工作文件根：`04-构建环境与脚本/`、`05-路由器操作脚本/`、`09-工具脚本/`、`02-固件产物/` | 否，**只对本机成立** |
| `$W` | 本机工作区根（`$L` 的上一级；路线 B 的容器把它挂成 `/work`） | 否，**只对本机成立** |
| `$H` | 过程归档 `.scratch/ffe-fastpath/<workstream>/<YYYYMMDD-HHMM>/` | 否，过程数据 |

## §0 一句话结论

**台架不是"刷完就有"的**：镜像默认拓扑是 `br-lan = lan1 lan2 lan3`、只有 `192.168.1.1`，
`192.168.5.1` / 16 条 nft 放行 / `fd00:5::1/64` 都必须手工重建（§3），而且其中 IPv6 台子**重启即消失**。
构建只有两条路：路线 A（`make check/bootstrap/build`）可移植，路线 B（两遍 `build-kernel.sh` + 容器补编）
**脚本不在仓库里、只对本机成立**（§4）。刷机只走"进 slot0 原厂 → 原厂 WebUI"一条路（§5）。

## §1 三条硬约束（后面每一步都受它约束）

1. **slot0 `[0x700000,0x2300000)` 永不主动写。** 判"slot0 有没有被动过"只看前后两次读的差，
   **不按 `/dev/mtdN` 名字**——两套固件里同一个编号指的不是同一个东西（见 §5.1）。
   **唯一例外**：`e2631-fwslot switch` 造成的 slot0 版本头修改是**授权行为**（用户 2026-11-04 20:50 解禁，
   用前必须 `info` 核对目标，见 §5.2）；授权范围只有该工具，`dd`/`nand write`/`mtd write` 直写仍禁（第 2 条）。
2. **写 flash 只走"进 slot0 原厂 → 原厂 WebUI 上传"一条路。** 禁 `dd` / `nand write` / `mtd write` / TFTP / `bootm` / `saveenv`。
3. **构建不许并发**（同一个 `build_dir` 同时只跑一个 build）、不 `git add -A`/`commit`/`push`；
   盒子上**禁 `sleep`、禁 `pkill -f`**；**同一时刻只许一个串口会话**。

## §2 现状：这一版镜像已经不带台架配置（下一颗起必须手工重建）

| 事实 | 原文证据 |
|---|---|
| 台架专用 uci-defaults 已从仓库移除（52 行删除） | `git -C $R show --stat bbee661` → `.../uci-defaults/63_e2631_mgmt_ra_defaults \| 52 -` |
| 并在 `apply.sh` 的删除表里登记（否则老树里那份还会进镜像） | `overlay-removed.txt:20-22`；机制 `apply.sh:43-55`；为什么需要 `apply.sh:33-36` |
| 备份件与提交前原件**逐字节一致** | `diff <(git show bbee661^:overlay/.../63_...) $H/bench-config/20261104-0400/63_...` 无输出 |
| 上一颗镜像（01:37 出的那颗）**仍带**台架配置 | 构建树里 `.../uci-defaults/63_e2631_mgmt_ra_defaults` 仍在（mtime 11-04 01:30）；`root-sanechips/etc/uci-defaults/` 里也在 |
| 上一颗镜像刷完后 `5201`/`5202` 的 nft 规则数 = 0 | `$H/flash-land/20261104-0430/logs/09-bench-state-initial.txt:103-106`（`--nft-5201` → `0`、`--nft-5202` → `0`） |
| 补规则前 `have_5201=0`；补完 16 条 | `$H/flash-land/20261104-0430/logs/10-bench-uci.txt:7,34`（`have_5201=0`、`Allow-52xx 条数=16`） |

**两条推论（写进流程）**：

* **下一颗镜像必须重跑 `apply.sh` 才会把 63 从树里删掉。** 不重跑 `apply.sh`，镜像会继续自带台架配置
  （这就是 `overlay-removed.txt` 存在的理由，`apply.sh:33-36` 有原文说明）。
* **镜像默认拓扑是 `br-lan = lan1 lan2 lan3`、`wan = wan`**（`overlay/target/linux/sanechips/base-files/etc/board.d/02_network:13`
  `ucidef_set_interfaces_lan_wan "lan1 lan2 lan3" "wan"`），管理地址是 OpenWrt 出厂 LAN 网段的 `.1`
  ⇒ 刚刷完**只有 `192.168.1.1`**，走 `lan1`（它在桥里）。台架要的 `lan1=192.168.5.1` 是**人为改出来的**，不是默认值。

### 2.1 `bench-config/20261104-0400/` 里到底有什么

目录只有 **2 个文件**：

| 文件 | 大小 | 是什么 |
|---|---|---|
| `README.md` | 1408 B | 说明为什么把台架配置移出仓库、怎么手工用，以及一条**已知一致性缺口**（脚本把 `network.wan.device` 设成 `lan1`，但 `e2631_ports.global.manual_port` 仍是 `wan`，`README.md:18-19`） |
| `63_e2631_mgmt_ra_defaults` | 2871 B，0755（`git ls-tree bbee661^` = `100755`） | 一份 uci-defaults：刷机后首次启动重建两件事——(A) `lan1` 从 `br-lan` 摘出、当静态 `192.168.5.1/24` 的 WAN 口，并补 `firewall.mgmt_22` 放行 `192.168.5.0/24 → tcp/22`；(B) `dhcp.lan.ra_default=1`（`63_...:8-13`、`:34-50`） |

**它没有覆盖什么（"从零重建"的真正缺口，必须手工补）**：

1. **没有 IPv6 地址**：`fd00:5::1/64`（lan1）、`br-lan` 的 ULA `/60` 都不在脚本里（全文只有 `uci` 12 条 + `exit 0`）。
2. **没有 proxy NDP / nft `ip6 c1rd`**（`nd-redirect drop`）。
3. **没有任何 iperf3 端口放行**（5201/5202 一条都没有）。
4. **没有 `br-lan` 成员**：它是 `del_list ... ports=lan1`（只摘 lan1），剩下什么取决于镜像默认。
5. **没有宿主侧配置**（`ens34` 地址、路由、`fd00:5::250/251`）。
6. **没有 `wan` zone 打开**：它保留出厂 `input=REJECT`，只补 tcp/22 一条。

⇒ 照着 `63_...` 跑一遍**不等于**台架能打流。完整台架 = `63_...` 的子集 + 下面 §3.4/§3.5 的运行期部分。

## §3 Part A —— 从零重建台架

### 3.1 依赖顺序（为什么必须是这个顺序）

```
§3.2 宿主/服务器就位（ens34 有 192.168.1.x/24）
§3.3 连 192.168.1.1 → 改 uci（lan1 摘出 + wan=lan1 static）→ commit → reboot
§3.4 从 192.168.5.1 连 → 防火墙（zone + forwarding + 16 条放行）+ ra_default → commit
§3.5 运行期 v6（lan1 fd00:5::1/64 + proxy_ndp + c1rd）+ 宿主 v6 地址与路由
§3.6 判据（地址/规则计数/邻居/门禁）
```

顺序的理由（不是风格问题）：

* **§3.3 必须最先、且必须用 reboot 落地**：`uci commit network` 会触发 netifd 重载，把当前 ssh 连接
  （源 `192.168.1.x`、目标 `192.168.1.1`）打断，而新地址 `192.168.5.1` 在重载完成前不存在
  ⇒ 不能"改完再 reload 再验证"。已验证的写法是"只写 uci + commit → 重新找盒子 → 回读 →
  等 uptime ≥95 s → `sync; reboot`"（`$H/tools/iter.sh:1592-1649` 的 `apply_swap_config`；
  `iter.sh:1727-1790` 的 `step_post`：95 s 等待在 `:1769-1774`、`sync; reboot` 在 `:1776`）。
* **§3.4 必须在 §3.3 之后**：`firewall.@zone[1]` 是 `wan` 区；§3.3 之前 `lan1` 还在桥里、`wan` 口是另一个
  netdev，这时写的 `src=wan` 规则指向的不是最终拓扑。
* **§3.5 必须在 §3.3 之后**：`lan1` 还在 `br-lan` 里时给它加 `/64` 没有意义（流量走桥不过它），
  而且 `proxy_ndp` 要开在 `br-lan` 上、proxy 邻居要挂在 `br-lan` 上（`v6-box.sh:13-22`）。

### 3.2 宿主与服务器就位

```sh
# 宿主：把接盒子的那块网卡起好，先给默认拓扑用的 192.168.1.x/24
ip link set ens34 up
ip addr add 192.168.5.50/24 dev ens34
ip addr add 192.168.1.50/24 dev ens34        # 默认拓扑下访问 192.168.1.1 用
ip -4 -o addr show dev ens34                 # 判据：能看到 192.168.5.50/24 与 192.168.1.50/24
```

来源：`$H/flash-land/20261104-0430/logs/18-E-host-nic.txt:10`（本轮实测 `ens34 UP 192.168.5.50/24 192.168.1.50/24 fd00:5::251/64 fd00:5::250/64`）；
`$H/tools/iter.sh:645-652`（`set_host_direct`：`ip addr add 192.168.5.50/24` + `ip addr add 192.168.1.250/24`）。

> ⚠ **不一致（照现场为准）**：`iter.sh:83` 的 `HOST1` 默认是 `192.168.1.250/24`，而本轮日志里宿主是
> `192.168.1.50/24`。两者都能用，但**先用 `ip -4 -o addr show ens34` 看现场**，不要照抄。
> 服务器侧（Windows `192.168.1.243`、MAC `d8:43:ae:68:18:28`、`iperf3 -s -p 5201`/`5202`）由用户维护，
> 本文**不给服务器配置命令**（**【未核】**：Windows 侧静态地址/防火墙怎么设的）。

> ⚠ **宿主 IPv4 源地址坑（不是盒子坏，2026-11-04 实测）**：`ens34` 同时有 `192.168.5.50/24` 与 `192.168.1.50/24`，
> 而 `ip route get 192.168.1.1` 选出的是 `src 192.168.5.50` ⇒ 裸 `ssh`/`ping 192.168.1.1` **100 % 超时**
> （上一轮 90 轮 `ssh root@192.168.1.1` 全空，差点误判"盒子没起来"）。默认拓扑下进盒子一律写
> **`ssh -b 192.168.1.50 root@192.168.1.1`**，`ping` 用 `ping -I 192.168.1.50 192.168.1.1`；
> 判据：加 `-I`/`-b` 后 `2 received, 0% packet loss`。证据：`$H/flash-p1/20261104-0545/logs/06-post-flash-poll.txt:1-10`
> （`ip route get … src 192.168.5.50`；裸 ping `100% packet loss`、加 `-I` 后 `0%`）。本文 §3.3 首连与 §6 第一遍都已按 `-b` 写。

### 3.3 换成台架路由态（lan1 当 WAN）

**在默认拓扑下连 `192.168.1.1`**（必须 `ssh -b 192.168.1.50`，见 §3.2 的 ⚠），只写 uci、不 reload network：

```sh
ssh -b 192.168.1.50 root@192.168.1.1 'sh -s' <<'EOS'
set -u
DI=""; i=0
while [ $i -le 8 ]; do n=$(uci get network.@device[$i].name 2>/dev/null) || break
  [ "$n" = "br-lan" ] && { DI=$i; break; }; i=$((i+1)); done
[ -n "$DI" ] || { echo NO_BRLAN_DEVICE; exit 11; }
ZI=""; i=0
while [ $i -le 8 ]; do n=$(uci get firewall.@zone[$i].name 2>/dev/null) || break
  [ "$n" = "wan" ] && { ZI=$i; break; }; i=$((i+1)); done
[ -n "$ZI" ] || { echo NO_WAN_ZONE; exit 12; }
uci set network.@device[$DI].ports="lan2 lan3"
uci set network.wan.device="lan1"
uci set network.wan.proto="static"
uci set network.wan.ipaddr="192.168.5.1"
uci set network.wan.netmask="255.255.255.0"
uci delete network.wan6
echo "DI=$DI ZI=$ZI PORTS_NOW=$(uci get network.@device[$DI].ports)"
uci show network | grep -E "@device\[$DI\]|network\.wan"

# ⚠ 自锁坑：下一步就要 reboot，而 reboot 后 lan1 归 wan 区、出厂 input=REJECT，
#   §3.4 又要求"从 192.168.5.1 连" ⇒ 不先放行 22 就把自己关在门外。
#   mgmt_22 必须在 reboot 之前提交并 fw4 reload（字段照 63_e2631_mgmt_ra_defaults）。
uci set firewall.mgmt_22=rule
uci set firewall.mgmt_22.name='mgmt-22'
uci set firewall.mgmt_22.src='wan'
uci set firewall.mgmt_22.src_ip='192.168.5.0/24'
uci set firewall.mgmt_22.proto='tcp'
uci set firewall.mgmt_22.dest_port='22'
uci set firewall.mgmt_22.target='ACCEPT'
uci commit firewall
fw4 reload
echo FW_RELOADED
# 判据：grep -c 'mgmt-22' /etc/config/firewall = 1

uci commit network
sync
echo SWAP_COMMITTED
EOS
```

来源：`$H/tools/iter.sh:1592-1649`（`apply_swap_config`，逐行同构）；`$H/flash-land/20261104-0430/logs/bench-rebuild.sh:11-16,54`；
`mgmt_22` 段与"reboot 前提交"的顺序见 `$H/post-flash/20261104-0600/logs/01-a3-swap.txt:17-25`。
判据：输出含 `FW_RELOADED` 与 `SWAP_COMMITTED`，`grep -c 'mgmt-22' /etc/config/firewall` = **1**
（`01-a3-swap.txt:18` 的 `firewall.mgmt_22.name='mgmt-22'`；重启后回读 `--mgmt22` = `1`，`07-a6-gate.txt:25-26`），
且回读里 `network.@device[N].ports='lan2' 'lan3'`、`network.wan.device='lan1'`、`proto='static'`、`ipaddr='192.168.5.1'`。

> ⚠ **为什么 `mgmt_22` 不能省（本轮实测的"自锁"）**：§3.3 的脚本原本不带 `firewall.mgmt_22`；reboot 后
> `192.168.5.1` 落在 `wan` 区，出厂 `input=REJECT`（`04-a4-firewall.txt:4` 回读 `zone1 input=REJECT forward=DROP masq=1`），
> 而 §3.4 要求**从 `192.168.5.1` 连** ⇒ 重启后没有任何管理通路，只能上串口。正确做法：**reboot 之前**提交
> `mgmt_22`（`src=wan`、`src_ip=192.168.5.0/24`、`proto=tcp`、`dest_port=22`、`target=ACCEPT`）并 `fw4 reload`；
> 本轮这样做了，重启后**首连就是 `192.168.5.1`**，一次成功（`02-refind.txt:1` 的 `reached 192.168.1.1 via src 192.168.1.50`
> 是重启前那一次；重启后 `04-a4-firewall.txt:1` 直接落在 `192.168.5.1`）。

然后**重新找盒子 → 回读 → 重启**：

```sh
# 1) 找一个还能连的地址（commit 后 netifd 可能已经重载，地址会从 .1 跳到 .5）
H=""
for h in 192.168.1.1 192.168.5.1; do
  B=""; [ "$h" = "192.168.1.1" ] && B="-b 192.168.1.50"   # 默认拓扑下源地址会选错，见 §3.2 的 ⚠
  ssh $B -o ConnectTimeout=3 root@$h 'echo OK' >/dev/null 2>&1 && { H=$h; break; }
done
[ -n "$H" ] || { echo "两个地址都不通，先看串口"; exit 1; }
echo "reached=$H"

# 2) 配置回读（读的是 /etc/config，不是运行态）
ssh root@$H 'echo "DEV=$(uci get network.wan.device) PROTO=$(uci get network.wan.proto) IP=$(uci get network.wan.ipaddr) PORTS=$(uci get network.@device[0].ports)"'
# 判据：DEV=lan1 PROTO=static IP=192.168.5.1 PORTS=lan2 lan3

# 3) 还没切过去（H 仍是 .1）就重启把它落地；已经到 .5 就只等 uptime
if [ "$H" = "192.168.1.1" ]; then
  UP=$(ssh root@$H 'cut -d" " -f1 /proc/uptime')
  if [ "${UP%%.*}" -lt 95 ]; then
    echo "uptime=${UP}s < 95 s：先别重启，等它到 ≥95 s 再执行本段（两次重启必须 ≥90 s，避免 ppe-pending）"
  else
    ssh root@$H 'sync; reboot'
  fi
fi

# 4) 等盒子重启完（约 60–90 s；别写 while :; do ...; done 空转，人力等一次即可），再验运行态
ssh -o ConnectTimeout=3 root@192.168.5.1 'ip -4 -o addr show dev lan1; ls /sys/class/net/br-lan/brif'
# 判据：lan1 有 192.168.5.1/24；brif 里没有 lan1
# 连不上就先看串口（是不是没起来 / 是不是还停在 .1）
```

> **两次重启必须间隔 ≥90 s**：`iter.sh:1769-1774` 在 uptime < 95 s 时先等到 95 s 再发第二次 `reboot`
> （原文注释："两次重启必须 ≥90 s"，也是为了避开 ppe-pending）。这是**代码里写死的判据**，别省。
> 重启后等 `192.168.5.1` 的 ssh 回来，并且 **uptime ≥108 s 才认为可信**（`iter.sh:1782-1784`）。

### 3.4 防火墙 + RA（16 条放行 + zone + forwarding）

这一份就是历史上机用过、回读 16 条的那份脚本（原文照抄，只把 `set -u` 之后的内容列全）：

```sh
ssh root@192.168.5.1 'sh -s' <<'EOS'
set -u
echo "== before =="
echo "ports=$(uci get network.@device[0].ports 2>/dev/null)"
echo "zone1 input=$(uci get firewall.@zone[1].input 2>/dev/null) forward=$(uci get firewall.@zone[1].forward 2>/dev/null) masq=$(uci get firewall.@zone[1].masq 2>/dev/null)"
echo "have_52xx=$(uci show firewall | grep -c 'Allow-520[12]' || true)"

uci set network.@device[0].ports='lan2 lan3'
uci set network.wan.device='lan1'
uci set network.wan.proto='static'
uci set network.wan.ipaddr='192.168.5.1'
uci set network.wan.netmask='255.255.255.0'
uci delete network.wan6 2>/dev/null
uci set firewall.@zone[1].input='ACCEPT'
uci set firewall.@zone[1].forward='ACCEPT'
uci set firewall.@zone[1].masq='0'
uci set dhcp.lan.ra_default=1
if [ "$(uci show firewall | grep -c 'src=.wan.$+.*dest=.lan.' 2>/dev/null || true)" = "0" ]; then
  F=$(uci add firewall forwarding); uci set firewall.$F.src='wan'; uci set firewall.$F.dest='lan'; echo "ADD_forwarding wan->lan ($F)"
else echo "HAVE_forwarding wan->lan"; fi
for port in 5201 5202; do
  for proto in tcp udp; do
    for fam in ipv4 ipv6; do
      n=$(uci show firewall | grep -c "Allow-$port-$proto-$fam-input" || true)
      if [ "$n" = "0" ]; then
        R=$(uci add firewall rule)
        uci set firewall.$R.name="Allow-$port-$proto-$fam-input"
        uci set firewall.$R.proto=$proto; uci set firewall.$R.dest_port=$port
        uci set firewall.$R.family=$fam;  uci set firewall.$R.target=ACCEPT
        echo "ADD Allow-$port-$proto-$fam-input ($R)"
      else echo "HAVE Allow-$port-$proto-$fam-input"; fi
      n2=$(uci show firewall | grep -c "Allow-$port-$proto-$fam-fwd" || true)
      if [ "$n2" = "0" ]; then
        R2=$(uci add firewall rule)
        uci set firewall.$R2.name="Allow-$port-$proto-$fam-fwd"
        uci set firewall.$R2.proto=$proto; uci set firewall.$R2.dest_port=$port
        uci set firewall.$R2.family=$fam;  uci set firewall.$R2.src=wan
        uci set firewall.$R2.dest=lan;     uci set firewall.$R2.target=ACCEPT
        echo "ADD Allow-$port-$proto-$fam-fwd ($R2)"
      else echo "HAVE Allow-$port-$proto-$fam-fwd"; fi
    done
  done
done
uci commit network; uci commit firewall; uci commit dhcp
# ra_default=1 是本节才提交的：odhcpd 在这之前已随机启动，"已=1 就不重启"的条件式会短路 ⇒ 必须无条件重启才发默认路由 RA
/etc/init.d/odhcpd restart
echo "Allow-52xx 合计=$(uci show firewall | grep -c -E 'Allow-520[12]-(tcp|udp)-(ipv4|ipv6)-(input|fwd)') tcp=$(uci show firewall | grep -c -E 'Allow-520[12]-tcp-(ipv4|ipv6)-(input|fwd)') udp=$(uci show firewall | grep -c -E 'Allow-520[12]-udp-(ipv4|ipv6)-(input|fwd)')"
/etc/init.d/firewall reload
echo "nft 合计=$(nft list ruleset | grep -cE 'dport 520[12]') tcp=$(nft list ruleset | grep -cE 'tcp dport 520[12]') udp=$(nft list ruleset | grep -cE 'udp dport 520[12]')"
echo "nft ipv6 tcp=$(nft list ruleset | grep -cE 'meta nfproto ipv6 tcp dport 520[12]') ipv6 udp=$(nft list ruleset | grep -cE 'meta nfproto ipv6 udp dport 520[12]')"
echo "nft 链归属 input=$(nft list chain inet fw4 input | grep -cE 'dport 520[12]') forward_wan=$(nft list chain inet fw4 forward_wan | grep -cE 'dport 520[12]') output=$(nft list chain inet fw4 output | grep -cE 'dport 520[12]')"
echo BENCH_UCI_DONE
EOS
```

来源：`$H/flash-land/20261104-0430/logs/bench-rebuild.sh:1-75`（本轮实际执行的那份，回读见 `logs/10-bench-uci.txt:25-35`；其中 `:70-74` 的按协议计数/链归属/UDP 冒烟提示是 2026-11-04 20:30 追加的**判据**，当轮没执行过）。
**注意**：本文删掉了原脚本末尾的 `/etc/init.d/network reload`——§3.3 已经把 network 落地过了，这里再 reload
只会把连接打断一次；`firewall reload` 保留。
脚本前半段那 6 行 `network.*` 是**幂等重写**（值已由 §3.3 落地），保留是为了这份脚本单独拿去重跑时也成立；
如果跳过 §3.3 直接跑本段，network 部分不会生效（没有 reload / 没有 reboot）。

**16 条规则的内容与落点**（内容层面的判据）：

| 条数 | 名字模板 | 字段 | fw4 落点 |
|---|---|---|---|
| 4 | `Allow-{5201,5202}-{tcp,udp}-ipv4-input` | `proto/dest_port/family=ipv4/target=ACCEPT`，无 `src/dest` | `chain output`（**不是 `chain input`**） |
| 4 | 同上 `-ipv6-input` | `family=ipv6` | `chain output` |
| 4 | `Allow-{5201,5202}-*-ipv4-fwd` | 再加 `src=wan dest=lan` | `chain forward_wan`，动作 `jump accept_to_lan` |
| 4 | 同上 `-ipv6-fwd` | `family=ipv6` | `chain forward_wan` |

来源：`$H/flash-land/20261104-0430/logs/11-nft-counts.txt:7-22`（原文：`meta nfproto ipv4 tcp dport 5201 … accept comment "!fw4: Allow-5201-tcp-ipv4-input"`
在 32-39 行、`… jump accept_to_lan … "!fw4: Allow-5201-tcp-ipv4-fwd"` 在 105-112 行）；
"input 实际在 chain output"这条：`$H/duplex/20261104-0045/RESULT.md:48`。

> ⚠ **"16 条"只在 nft 侧成立，别写成 "iptables 16 条"**：这一版是 **fw4，没有 iptables 表**——
> `iptables -S | grep -cE '520[12]'` 与 `ip6tables -S | grep -cE '520[12]'` 都是 **0**；
> 16 条全部由 `nft list ruleset` 计到（5201 = 8、5202 = 8；**按协议看是 tcp 8 / udp 8，§3.6 判据按协议分开数，合计 16 只作记录不作判据**）。**判据只认 nft 计数、且必须分协议**（外加 uci 侧 `Allow-52xx` 合计 16 且 tcp/udp 各 8），
> 别拿 `iptables -S` 的 0 当"规则没生效"去重复加规则。
> 证据：`$H/post-flash/20261104-0600/logs/13-iptables-count.txt:3-12`（`--iptables_5201 0` / `--iptables_5202 0` /
> `--ip6tables_5201 0` / `--ip6tables_5202 0` / `--nft_total 16`）。

**另一种写法（原始 nft，备查，不推荐做长期方案）**：`$H/duplex/20261104-0045/logs/add-5202.nft:1-9` 用 `nft -f -` 往
`inet fw4` 的 `forward_wan`/`output` 链上 `insert` 8 条（5202）；`$H/diag-flash/20261103-2230/logs/T11-config.txt:19-80`
是更早那版 5201 的 8 条逐条 `uci set` 原文。**缺点**：直接 `nft insert` 的规则活不过 `/etc/init.d/firewall reload`，
更活不过重启；`uci` 那版是持久化的（`commit firewall` 之后 `fw4` 重生成）。

> **已知坑（2026-11-04 盒上现态核实）：看到 `*-input` 规则存在 ≠ 入向被放行。**
> 这 8 条 `Allow-{5201,5202}-{tcp,udp}-{ipv4,ipv6}-input` 名字里有 input，fw4 却把它们落在 **`chain output`**
> （盒上现取：`$H/bench-udp-onbox/20261104-1955/logs/22-520x-with-chain.txt:1-8`（盒上 `grep -n` 行号 32-39）8 条全在 output；
> `chain input` 里 520x = **0 条**）。对"从 wan 进来、到盒子本机"的路由**无效**——入向真正靠的是
> `uci zone wan input='ACCEPT'`（`$H/flash-land/20261104-0430/logs/bench-rebuild.sh:17`），那是过宽放行、不是这 8 条规则。
> 核对命令（盒上跑，只看落点条数）：
>
> ```sh
> ssh root@192.168.5.1 'for c in input forward_wan output; do printf "%s=%s\n" "$c" "$(nft list chain inet fw4 $c | grep -cE "dport 520[12]")"; done'
> # 现态期望：input=0 forward_wan=8 output=8（合计 16）；input=0 时，别把"规则在"当成"入向通了"
> ```

### 3.5 IPv6 台子（运行期）+ 宿主 v6

盒侧（原文照抄 `v6-box.sh`）：

```sh
ssh root@192.168.5.1 'sh -s' <<'EOS'
set -u
ip -6 addr show dev lan1 2>/dev/null | grep -q 'inet6 fd00:5::1/64' || { ip -6 addr add fd00:5::1/64 dev lan1 || exit 21; echo ADDED_fd005; }
[ "$(sysctl -n net.ipv6.conf.br-lan.proxy_ndp 2>/dev/null || echo 0)" != "1" ] && { sysctl -w net.ipv6.conf.br-lan.proxy_ndp=1 || exit 22; echo SET_proxy_ndp; }
for a in fd00:5::250 fd00:5::251; do
  ip -6 neigh show proxy dev br-lan 2>/dev/null | grep -q "$a" || { ip -6 neigh add proxy "$a" dev br-lan || exit 23; echo "ADDED_proxy_$a"; }
done
nft list tables 2>/dev/null | grep -q '^table ip6 c1rd' || nft add table ip6 c1rd || exit 24
nft list table ip6 c1rd 2>/dev/null | grep -q 'chain out' || nft "add chain ip6 c1rd out { type filter hook output priority raw; }" || exit 25
nft list table ip6 c1rd 2>/dev/null | grep -q 'icmpv6 type nd-redirect' || nft add rule ip6 c1rd out icmpv6 type nd-redirect counter drop || exit 26
uci set dhcp.lan.ra_default=1; uci commit dhcp; echo SET_ra_default; /etc/init.d/odhcpd restart >/dev/null 2>&1 || true   # 无条件 restart：条件式在"§3.4 已提交 ra_default=1"时短路 ⇒ odhcpd 仍跑旧配置、不发默认路由 RA
ps 2>/dev/null | grep -v grep | grep -q odhcpd && echo ODHCPD_running || echo ODHCPD_missing
echo == after ==
ip -6 addr show dev lan1 2>/dev/null | grep 'inet6 fd00:5::1/64' || true
sysctl -n net.ipv6.conf.br-lan.proxy_ndp 2>/dev/null || true
ip -6 neigh show proxy dev br-lan 2>/dev/null || true
nft list table ip6 c1rd 2>/dev/null || true
echo V6_SETUP_DONE
EOS
```

来源：`$H/coal-sweep/20261103-2330/flash/v6-box.sh:1-45`（原文），本轮重放见
`$H/flash-land/20261104-0430/logs/12-v6-box.txt:1-24`（`ADDED_fd005` / `SET_proxy_ndp` / 两条 proxy / `V6_SETUP_DONE` / `rc=0`）。
**与原文的差异（2026-11-04 20:44 实测补的）**：`ra_default` 那行改成**无条件** `odhcpd restart`——原文的
`if !=1` 条件式在"§3.4 已提交 `ra_default=1`"时短路，odhcpd 不重启就不发 IPv6 默认路由 RA ⇒ 宿主→服务器 v6 100 % 丢
（盒子自己 ping6 通、nft 计数正常，极易误判成"转发坏了"）；无条件重启后 `SRV6_PING_OK try=2`
（`$H/flash-rxsplit/20261104-2035/logs/33-odhcpd-fix.txt:1-13`）。

宿主侧（**ULA 前缀每次整槽刷机都会变，必须现查，不要照抄任何一次的值**）：

```sh
# 1) 现查盒子的 ULA 前缀：两条命令都要看（uci 里的 /48 + br-lan 上实际配到的 /60）
ssh root@192.168.5.1 'uci get network.globals.ula_prefix; ip -6 addr show dev br-lan scope global'
#   2026-11-04 这轮新镜像实测 = fd70:44e8:c581::/48、br-lan = fd70:44e8:c581::1/60
#   （上一颗镜像的 /48 已随重装作废；重启/重装后一律现查，本文不复写旧值）
ULA=$(ssh root@192.168.5.1 'uci get network.globals.ula_prefix')                       # 形如 fd70:44e8:c581::/48
BR=$(ssh root@192.168.5.1 "ip -6 addr show dev br-lan scope global | awk '/inet6 fd/{print \$2; exit}'")  # 形如 fd70:44e8:c581::1/60
PFX60=${ULA%::/48}::/60                                                                # 形如 fd70:44e8:c581::/60

# 2) 宿主地址（proxy NDP 的两个源地址）
ip -6 addr add fd00:5::250/64 dev ens34 nodad
ip -6 addr add fd00:5::251/64 dev ens34 nodad
# 3) 到服务器 ULA 的路由（前缀用上面现查的，不要写死）
ip -6 route replace "$PFX60" via fd00:5::1 dev ens34
# 4) 自检（服务器地址也从邻居表按 MAC 现查，不写死）
SRV6=$(ssh root@192.168.5.1 "ip -6 neigh show dev br-lan | awk '/d8:43:ae:68:18:28/ && \$1 ~ /^fd/ {print \$1; exit}'")
ping -6 -c2 -I fd00:5::250 "$SRV6"
ping -6 -c2 "${BR%/*}"                 # br-lan 自己的全局地址
```

来源：`$H/post-flash/20261104-0600/logs/06-host-v6.txt:1-17`（`route_rc=0`、`fd70:44e8:c581::/60 via fd00:5::1`、
两条 `0% packet loss`）；前缀与服务器地址现查：`$H/post-flash/20261104-0600/logs/05-a5-v6-box.txt:25-30`、
`logs/00-conn.txt:11`（`ula_prefix='fd70:44e8:c581::/48'`）。
旧口径（上一轮 `flash-land` 的宿主 v6）见 `$H/flash-land/20261104-0430/logs/13-host-bench.txt:1-14`（`add_rc=0`、`ping6 srv 3/3`）；
服务器 MAC：`$H/flash-land/20261104-0430/logs/16-gate-check.txt:3-4`（`… REACHABLE`，MAC `d8:43:ae:68:18:28`）。

> 这条路由 / 这两个地址 / proxy NDP / `c1rd` 表**都是运行期状态，重启即消失**；`ra_default=1` 和 16 条
> 防火墙规则是 uci、重启仍在（`$H/flash-land/20261104-0430/RESULT.md:227-228`）。

### 3.6 台架判据（全过才算"台架好了"）

```sh
ssh root@192.168.5.1 '
  echo "--addr"; ip -4 -o addr show dev lan1; ip -4 -o addr show dev br-lan
  echo "--brif"; ls /sys/class/net/br-lan/brif
  echo "--uci";  uci get network.wan.device; uci get network.@device[0].ports; uci get dhcp.lan.ra_default
  echo "--nft";  echo "合计=$(nft list ruleset | grep -cE "dport 520[12]") tcp=$(nft list ruleset | grep -cE "tcp dport 520[12]") udp=$(nft list ruleset | grep -cE "udp dport 520[12]")"
  echo "--nft-v6"; echo "ipv6 tcp=$(nft list ruleset | grep -cE "meta nfproto ipv6 tcp dport 520[12]") ipv6 udp=$(nft list ruleset | grep -cE "meta nfproto ipv6 udp dport 520[12]")"
  echo "--chain"; echo "input=$(nft list chain inet fw4 input | grep -cE "dport 520[12]") forward_wan=$(nft list chain inet fw4 forward_wan | grep -cE "dport 520[12]") output=$(nft list chain inet fw4 output | grep -cE "dport 520[12]")"
  echo "--ipt";  iptables -S 2>/dev/null | grep -cE '520[12]'; ip6tables -S 2>/dev/null | grep -cE '520[12]'
  echo "--v6";   ip -6 addr show dev lan1 | grep fd00:5::1; sysctl -n net.ipv6.conf.br-lan.proxy_ndp
'
```

宿主侧（**放行之后必须跑一次；不跑这条就不算"台架建好"**。ULA 每次现查，别写死前缀）：

```sh
SRV6=$(ssh root@192.168.5.1 "ip -6 neigh show dev br-lan | awk '/d8:43:ae:68:18:28/ && \$1 ~ /^fd/ {print \$1; exit}'")
ssh root@192.168.5.1 'uci get network.globals.ula_prefix'   # 服务器 ULA 必须落在这个 /48 内
iperf3 -6 -u -c "$SRV6" -p 5201 -t 3 -b 10M -l 128          # 不许 timeout 早杀
# 判据：出现 `receiver` 行且丢包 0（现态 0/29308 (0%)）⇒ 数据面通、台架才算建好；
#       只有 sender 行 / 丢包 ≠0 / 报 `unable to read from stream socket` ⇒ **台架不算建好**
```

| 判据 | 期望 | 证据出处 |
|---|---|---|
| `lan1` | `192.168.5.1/24` + `fd00:5::1/64` | `12-v6-box.txt:29-30`、`11-nft-counts.txt:27` |
| `br-lan` | `192.168.1.1/24` + **现查的 ULA `::1/60`**，成员只有 `lan2 lan3`（+ 两个 ap） | `07-a6-gate.txt:5`、`05-a5-v6-box.txt:26` |
| `wan.device` / `ports` | `lan1` / `lan2 lan3` | `10-bench-uci.txt:26-27` |
| `dhcp.lan.ra_default` | `1` | `10-bench-uci.txt:33` |
| `nft` **按协议分开数**：`grep -cE 'tcp dport 520[12]'` / `'udp dport 520[12]'` | **tcp = 8 / udp = 8**（合计 16 只作记录，不作判据；只报合计查不出"只放了 tcp"） | `$H/bench-udp-onbox/20261104-1955/logs/02-nft-counts.txt:2,4,6`、`11-nft-counts.txt:2-5` |
| `nft` **IPv6 专项**：`grep -cE 'meta nfproto ipv6 tcp dport 520[12]'` / 同 `udp` | **ipv6 tcp = 4 / ipv6 udp = 4**（v4 同样各 4，四组共 16） | `$H/bench-udp-onbox/20261104-1955/logs/20-counts-by-protofam.txt:4` |
| `nft` **链归属**：`chain input` / `forward_wan` / `output` 各几条 | **input = 0、forward_wan = 8、output = 8**——8 条 `*-input` 落在 `chain output`，**对入向无效**；入向靠 `zone wan input=ACCEPT`（见 §3.4 末的已知坑） | `$H/bench-udp-onbox/20261104-1955/logs/22-520x-with-chain.txt:1-16` |
| 规则计数器（打流前后各取一次） | 目标协议的 `udp dport 520[12]` 计数 **≥1 新增**即算"进了链"，**不看涨多少**：`nft list flowtables` 有 `flags offload`，首包之后整条流 `[HW_OFFLOAD]` 绕过 nft，**每个新流只 +1 包** | `$H/bench-udp-onbox/20261104-1955/RESULT.md:99-113` |
| **UDP 数据面冒烟（宿主侧，必跑）** | 上方命令：**必须出现 `receiver` 行且丢包 0**，否则**台架不算建好** | `$H/bench-udp-onbox/20261104-1955/logs/10-udp-5201.txt:12-13` |
| `iptables -S` / `ip6tables -S` 里 5201/5202 | **0 / 0**（fw4 无 iptables 表，正常，不代表没放行） | `13-iptables-count.txt:3-10` |
| `proxy_ndp` | `1` | `12-v6-box.txt:7,14` |
| 服务器 ULA 邻居 | `REACHABLE` | `16-gate-check.txt:4` |
| **宿主 → 服务器 v6（"转发坏了"常是 RA）** | `ping6` 服务器地址 **0 % 丢**；本台架宿主**没有** v6 默认路由（只有 `fd00:5::/64` 与 ULA `::/48 via fd00:5::1`），**别拿 `ip -6 route \| grep default` 判**（会假 FAIL）；丢了先补 §3.5 的 `odhcpd restart` | `$H/flash-rxsplit/20261104-2035/logs/39-nft-per-chain.txt:6-10`、`33-odhcpd-fix.txt:9-13` |

> **nft 计数的"8"和"4"都对，看 grep 带不带协议**：`ipv4`/`ipv6` **只按族数**各 **8**（`forward_wan` 4 + `output` 4）；
> **带协议**（`ipv4 tcp`）各 **4**（2 端口 × 2 链）。**别把"单链 4"当 ruleset 全量，也别把 ruleset 8 当单链**——
> 按链判据仍是 `input = 0、forward_wan = 8、output = 8`。
> 原文：`$H/flash-rxsplit/20261104-2035/logs/39-nft-per-chain.txt:2-5`（`forward_wan ipv4=4 ipv6=4 tcp=4 udp=4` /
> `output` 同 / `input` 全 0 / `ruleset ipv4=8 ipv6=8`）。

## §4 Part B —— 构建（改驱动 → 出镜像）

### 4.1 先分清两条路

| 路线 | 命令入口 | 可移植性 | 用途 |
|---|---|---|---|
| **A 仓库正规路** | `make check` → `make bootstrap` → `make build`（`BUILD.md:5`、`:123-141`），打包 `node host/release.mjs`（`scripts/build.sh:95`） | ✅ 别人 clone 就能跑（只需 base tree + Node ≥20.15） | 交付/复现 |
| **B 本机既有路** | `apply.sh` → `build-kernel.sh` → 补编 → `build-kernel.sh` → `finish-build.sh` | ❌ 脚本全在仓库外、写死本机绝对路径 | 迭代（~10 min/轮） |

**最大缺口（也是这份 runbook 最该被人知道的一条）**：
`build-kernel.sh`、`docker-build-kernel-inner.sh`、`finish-build.sh`、`check-fix2.sh`、`iter.sh`
**都不在仓库 `$R` 里**，而在本机 `$L/04-构建环境与脚本/` 与 `$H/tools/`，且内部写死本机工作区绝对路径、
依赖本机 docker 镜像 `e2631-builder:22.04`、依赖本机 `$O` 这棵树。
⇒ **别人照做从零重建，只能走路线 A；路线 B 只对这台机器成立，不能照抄。**

### 4.2 路线 A（可移植，推荐给别人）

```sh
cd $R
make check                     # 5+2 道门：bash -n / node --check / busybox ash / slot 工具 / dts / secret-scan
make bootstrap                 # clone 固定 SHA 的 base tree + apply overlay + feeds + seed
make build                     # make world → collect → release.mjs 打包 → verify
```

判据：`make build` 打印 `package: $E2631_OUT/e2631-openwrt-<stamp>.bin`（`scripts/build.sh:93-96`）；
`$E2631_OUT/build-<stamp>/world.log` 末行不是 `Error`；`payload/` 里有 `kernel`/`root`/`e2631.dtb`
（`BUILD.md:135-137`）。

### 4.3 路线 B（本机已验证，含两个已踩过的坑）

**前置核对（不做就别开始）**：

```sh
git -C "$R" rev-parse --short HEAD; git -C "$R" status --porcelain   # 记下 HEAD 与 dirty 清单
docker image inspect e2631-builder:22.04 >/dev/null && echo IMAGE_OK
grep -c . "$R/overlay-removed.txt"                                    # 应含 63_ 那一行
```

**完整序列（2026-11-04 01:26–01:37 真正跑过的那条，含两次 `build-kernel`）**：

```sh
# B-1 apply：把 overlay（含新增的 /etc/config、/etc/init.d）拷进树，并按 overlay-removed.txt 删旧文件
bash "$R/apply.sh" "$O"
# 判据（新树才有的）：
test ! -e "$O/target/linux/sanechips/base-files/etc/uci-defaults/63_e2631_mgmt_ra_defaults" \
  && echo "63 GONE (台架配置不会再进镜像)"
for f in zx279128s-accel.c zx279128s-main.c zx279128s-eth.h; do
  md5sum "$R/overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/$f" \
         "$O/target/linux/sanechips/files/drivers/net/ethernet/zte/$f"; done   # 两两相等

# B-2 第一遍 build-kernel：内核 + ppe.ko + rootfs + 镜像（会给 .fix2-build-start 打时间锚点）
bash "$L/04-构建环境与脚本/build-kernel.sh"

# B-3 补编。必须在容器里跑：宿主没有 make/gcc（build-env.sh:4-5 原文）。
#     a) 先判要不要做：内核树被重抽后 LINUX_DIR 里没有 .ko
find "$O"/build_dir/target-*/linux-*/linux-* -name '*.ko' | wc -l     # = 0 就必须做 a)

docker run --rm --network host \
  -v "$O":/openwrt -v "$W":/work -w /openwrt \
  e2631-builder:22.04 bash -lc '
    set -e
    chown -R builder:builder /openwrt
    asb() { su -s /bin/bash builder -c "cd /openwrt && make -j\$(nproc) $*"; }
    asb target/linux/compile              # a) .ko 与 .modules 戳回来（hmac.ko 就在这一步）
    asb package/kernel/linux/compile      # b) 47 个 kmod apk 重打
    asb package/base-files/clean          # c)
    asb package/base-files/compile V=s    # d) 新 /etc/config、/etc/init.d 打进 base-files 的 apk
  '
#     判据：base-files 的 apk mtime 是刚才；hmac.ko 回到 LINUX_DIR

# B-4 第二遍 build-kernel：这次 package/index → package/install（从零装 rootfs）→ target/install
bash "$L/04-构建环境与脚本/build-kernel.sh"

# B-5 打包 + 自证 + 两道独立校验
bash "$L/04-构建环境与脚本/finish-build.sh"
```

**为什么必须这么排（每条都有原文）**：

1. **`build-kernel.sh` 只做 `target/linux/install` + `target/install`，不重编 `package/base-files`**
   （`build-kernel.sh:5-10` 注释原文："改了 package/、改了 seed 配置、加了新包 —— 那些必须走 make world"）。
   ⇒ 新加的 `/etc/config/zx_ffe`、`/etc/init.d/zx_ffe_coalesce` **进不了 rootfs**。
   实测：第一次 build-kernel 之后 `root-sanechips/etc/config/` 里没有 `zx_ffe`，`base-files-*.apk` 还是 11-01 的旧包
   （`$H/land-config/20261104-0140/RESULT.md:112-116`）。
2. **`package/Makefile` 的 rootfs-cache 以 `packages.adb` 为 key**（`docker-build-kernel-inner.sh:10-12`），
   包没重编时 key 不变 ⇒ `target/install` 会把旧的 `root.orig/root` 整份 `cp` 回来。所以脚本里显式删
   `rootfs-cache-*`、`packages.adb`、`.image_install`（`docker-build-kernel-inner.sh:88-95`）。
3. **内核树重抽后 `LINUX_DIR` 里没有 `.ko`** ⇒ `package/kernel/linux` 打包时会报
   `ERROR: module .../crypto/hmac.ko is missing`（`$H/land-config/20261104-0140/RESULT.md:117-121` 原文）。
   补 `target/linux/compile` 之后 `.modules` 戳与 `hmac.ko` 才回来。
4. **为什么是"两遍 build-kernel"而不是"一遍 + reload"**：B-3 只产出新的 `.apk`，rootfs 要重新装一次
   （`package/install`）+ 镜像要重打（`target/install`），这两步在 `build-kernel.sh` 里（`:96-99`、`:131`）。
   第二遍跑的就是这两步。**这是已验证的形态**；把 B-2/B-3/B-4 合并成"一遍容器内顺序执行"没有实测证据（**【未核】**）。
5. **B-3 那个 docker 包装是"按已验证的零件拼的"**：历史产物里能逐字对上的是
   `$H/land-config/20261104-0140/inner-fix.sh:16-27`（`target/linux/compile` + `base-files` clean/compile，
   用 `su -s /bin/bash builder -c "cd /openwrt && make …"` 和 `chown -R builder:builder /openwrt`）
   以及单独一条 `make package/kernel/linux/compile`（`$H/land-config/.../RESULT.md:108`、`:123`）。
   **把四条连在一个 `docker run` 里没有整体实测过** ⇒ 保守做法：先原样跑 `inner-fix.sh`（把脚本路径换成容器内路径），
   再单独补那条 kmod 的 make。

**B-4 的自动判据**（`build-kernel.sh` 自带，不过就 `exit 1`）：
`docker-build-kernel-inner.sh:101-128` 自证 `rootfs 里的 ppe.ko == 新建 .apk 里的 .ko`，且 md5 不是交付原件
`4ab1c229…`；命中 `172a3fd8…` 时打 NOTE（ABI 恢复后代码未变，属预期）。

### 4.4 怎么确认"新东西真的进了镜像"（七层，逐层收紧）

| 层 | 判据 | 命令/证据 |
|---|---|---|
| 1 树 | overlay 与树里的源文件逐字节相同 | `md5sum` 两两相等（`iter.sh` 的 build 步骤对三个驱动文件就这么做） |
| 2 单编 | 零告警；`CC` 行数 ≥3 | `bash $H/build-p10.5-single.sh`；判据来自 `iter.sh` build 步骤（`w=0`、`cc≥3`） |
| 3 build_dir | `.o` 的 mtime 晚于 build start 锚点 | `ls -l --time-style='+%F %T' "$O"/build_dir/target-*/linux-*/linux-*/drivers/net/ethernet/zte/*.o`；锚点 = `$L/02-固件产物/.fix2-build-start`（`build-kernel.sh:18-21`） |
| 4 rootfs 目录 | 新文件在 `root-sanechips/etc/` 里，且 apk 是刚重打的 | `ls -l "$O"/build_dir/target-*/root-sanechips/etc/config/`、`ls -l "$O"/bin/targets/sanechips/generic/packages/base-files*.apk`（`$H/land-config/.../RESULT.md:148-152`） |
| 5 squashfs（**真正进镜像的那份**） | 解包能看到文件、内容一致、软链在 | `unsquashfs -ll "$O"/build_dir/target-*/linux-*/root.squashfs \| grep <name>`；再解出来 `sed -n`/`grep` 比对（`$H/land-config/.../evidence.log:11-26`） |
| 6 二进制 | 默认值真的编进 `vmlinux` 的 `.data` | `nm vmlinux \| grep -E ' zxcoal_(us\|frames)$'` → `objdump -s -j .data --start-address=<addr>` 读到 `64/16/250`（`$H/land-config/.../evidence.log:28-37`） |
| 7 新鲜度（权威） | `root.squashfs`/`root.orig`/`packages.adb`/`vmlinux`/`System.map` 的 mtime **全部 ≥ build start**；factory 与 sysupgrade 的 kernel/root 成员逐字节一致 | `bash "$L/04-构建环境与脚本/check-fix2.sh" "$O" <builddir> <factory.bin> "$L/02-固件产物/.fix2-build-start"`（`finish-build.sh:39-40` 自动调；判据 `check-fix2.sh:8-19`） |

> **别拿 squashfs 里的文件时间当新鲜度判据**：`mkfs_time` 被 `SOURCE_DATE_EPOCH` 钉死在
> 2026-10-13 01:14:44，所以 `unsquashfs -ll` 里所有文件都显示那个时间（`check-fix2.sh:54-56` 原文）。
> 新鲜度只能看 build start 锚点 + `check-fix2.sh` 的 A/F 段。

### 4.5 构建产物

| 件 | 路径 | 大小 / sha256（上一颗） |
|---|---|---|
| factory（交付、可刷） | `$E2631_OUT/e2631-openwrt-fix2-<stamp>.bin`（`finish-build.sh:18`） | 20,972,052 B / `6a15816176ca5586d18e6ddef4676c8547be0b46fecb5676a90da66b53802e1b`（`$H/land-config/.../RESULT.md:128-136`） |
| sysupgrade（参考，**不可用于刷非活动槽**） | `$O/bin/targets/sanechips/generic/*-squashfs-sysupgrade.bin` | 14,663,939 B / `f405e6c6…`（同上） |
| root.squashfs | `$O/build_dir/target-*/linux-*/root.squashfs` | 11,146,555 B（同上） |

**后缀必须 `.bin`**：`overlay/target/linux/sanechips/base-files/lib/upgrade/platform.sh:31-48` 的
`platform_check_image()` 只认 `*.bin`，其它后缀直接 `return 1` 并打印
`This board upgrades with the factory-format .bin`。

## §5 Part C —— 刷机

### 5.1 写前必须打印并核对的东西

```sh
IMG=<factory.bin 绝对路径>
sha256sum "$IMG"; stat -c 'size=%s mtime=%y' "$IMG"
ssh root@<原厂壳地址> 'cat /proc/zte/verinfo/versionstates'    # 见下：curver/backver 的物理地址
```

| 核对项 | 上一轮实测值（照抄对照） | 出处 |
|---|---|---|
| 镜像 sha256 / 大小 | `6a158161…2e1b` / `20,972,052 B` | `$H/flash-land/20261104-0430/logs/03-slot-target-check.txt:3-5` |
| 当前活动槽 | `curverBootStartPhyAddr: 0x00700000` → **slot0 在跑** | `03-slot-target-check.txt:8`；`02-pre-vendor-shell.txt:11-12` |
| slot1 窗口 | `backverphyaddr: 0x02300000` → `[0x2300000,0x3F00000)` = `0x1c00000` B | `03-slot-target-check.txt:10-12`；`02-pre-vendor-shell.txt:18-20` |
| slot0 窗口 | `curverheader_lowstart 0x00700000` → `curverheader_lowendstart 0x02300000` | `02-pre-vendor-shell.txt:17-18` |
| slot1 版本头 | `backverheaderaddr: 0x03760000`（当次）；OpenWrt 侧 `e2631-fwslot` 报 `header@0x1400000` = `slot1+0x1400000` = **0x3700000** | `02-pre-vendor-shell.txt:27`；`07-fwslot-info.txt:4` |
| 设备名对照（**最容易踩的坑**） | 原厂：`mtd5=defcfg`、`mtd6=kernel1`、`mtd7=kernel2`、`mtd8=rootfs`；OpenWrt：`mtd5=slot0`、`mtd6=slot1`、`mtd9=slot1-header`、`mtd10=userdata` | `02-pre-vendor-shell.txt:40-43`；`20-final-state.txt:3-14` |

> **判槽只按物理地址**（`0x700000` / `0x2300000` / `0x3F00000`），**不按 `/dev/mtdN` 名字**——两套固件里
> 同一个编号指的不是同一个东西。
> 表里这些是 2026-11-04 的读数（含 slot1 sha），**每次写前都要重新打印一遍**，别拿它们当当前状态。

### 5.2 进 slot0（原厂改版）：抹 slot1 的版本头

原理：cspboot 只认 CRC 合法的版本头，抹掉 slot1 那个头，它只剩 slot0 可选
（`docs/operations/flashing.md:37-52` 的反向操作原文；本方向的口令/流程见 `ser-flash-prep.py`）。

```sh
# 0) 串口桥必须先起（本机 127.0.0.1:5555；脚本没有 --yes 时是演练，一个字节都不写）
ss -ltn | grep ':5555 '                      # 判据：有监听
python3 "$L/09-工具脚本/ser-flash-prep.py"            # 演练：只走到 => 就停
python3 "$L/09-工具脚本/ser-flash-prep.py" --yes      # 真做：擦完立刻 reset
```

* 擦除常量**写死在代码里**，且断言在 slot1 窗口内：`ERASES = ((0x3700000,0x20000),(0x3760000,0x20000))`、
  `SLOT1_WINDOW = (0x2300000,0x3F00000)`、越界即 `拒绝执行`（`ser-flash-prep.py:38-44`）。
* 0x3700000 = slot1 + 0x1400000 = 当前几何的**真头**；0x3760000 = 老几何偏移、现落在 `0xff` 空白里，照擦是兜底
  （`ser-flash-prep.py:16-26`）。
* 口令在脚本里：`PW = b'5cE080@fyBD'`（`ser-flash-prep.py:37`；同一口令也写在 `flashing.md:14`）。
* 判据：日志里两条 `Erasing at 0x3700000 -- 100% complete. / OK`、`Erasing at 0x3760000 … / OK`、`已发 reset`
  （历史原文 `$H/coal-sweep/20261103-2330/flash/F4-uboot-erase.txt:119-139`）。
* 之后盒子先起 cspboot → **slot0 原厂**，httpd 要 ~90 s 才应答（`flash-via-webui.py:6-10`）。

> ⚠ **演练会停在 U-Boot `=>`，`--yes` 要重新认一遍 `Press 1`**：不带 `--yes` 的演练走到 `=>` 就停
> （`$H/flash-p1/20261104-0545/logs/02-prep-dryrun.txt:100-116`：`*** Press 1 means entering boot mode**` →
> 口令 → `Hit any key to stop autoboot` → `=>` → `演练模式：停在这里，不发任何命令`），盒子留在 U-Boot。
> 所以随后跑 `--yes` 时脚本要从「`Press 1` → 口令 → autoboot → `=>`」**重新认一遍**。
> 演练后的正确收尾：先 `python3 ser-cmd.py 4 'reset'`（只发 `reset`，**不写 flash**；命令见
> `$H/flash-p1/20261104-0545/RESULT.md:22-24`，串口原文 `03-uboot-reset.txt:1-5`）
> 让盒子起回 OpenWrt，再 `ssh reboot`，然后才跑 `--yes`；真做那次的两条 `nand erase` 见
> `04-prep-erase.txt:112-130`（`0x3700000` / `0x3760000` 都 `100% complete` 后发 `reset`）。
> 与 `recovery-rung2/20261103-1230/RUNBOOK.md:113` 记的"两条都要配一次重启"一致。

> ✅ **`e2631-fwslot switch` 允许使用**（用户 2026-11-04 20:50 解禁）：**由该工具造成的 slot0 版本头修改属授权行为**，
> 是 §1 第 1 条"永不主动写"的**唯一例外**。它会把**另一个槽**的头擦掉重写
> （`overlay/package/firmware/e2631-fwslot/src/e2631-fwslot.c:20-23`、`:681-685`、`:783-784`），
> 从 OpenWrt 跑着时"另一个槽"就是 slot0 ⇒ **用前必须先 `e2631-fwslot info` 打印两槽状态、确认目标槽**
> （`BUILD.md:161-165` 是用法原文）。**授权范围只有这一个工具**：`dd` / `nand write` / `mtd write` 直写任何槽仍然**禁止**（§1 第 2 条）。

### 5.3 原厂 WebUI 上传（只上传一次）

```sh
# 盒子若已停在原厂（httpd 已应答），--seconds 60 够（上一轮就是这么做的）；
# 若是刚抹完版本头、盒子还在重启，用默认 420（脚本 docstring：reset→httpd 约 90 s）
python3 "$L/05-路由器操作脚本/flash-via-webui.py" "$IMG" --seconds 180
```

* 它先只读探测 `GET /?_type=loginsceneData&_tag=login_token_json`（拿到含 `logintoken` 的 JSON 才继续，
  这是原厂 ThinkLua httpd 的特征），探测通过后调 `vendor-web.py upload <pkg> --yes`，**只上传一次、不重试**
  （`flash-via-webui.py:4-20`、`:22`）。
* 判据：`上传响应：HTTP 200` + `{"IF_ERRORID":0}` + `upload_rc=0`（`$H/flash-land/20261104-0430/logs/04-flash-upload.txt:6-11`）。
* **原厂 httpd 只写非活动槽**（`flashing.md:29-33` 实测两次；`$H/flash-land/20261104-0430/RESULT.md:19`）。
* **"上去就刷、别久留"**：原厂在 slot0 跑着时，它会在运行期把自己抄进 slot1——上一轮写前核对就拍到
  "slot1 现有的是原厂运行期自抄副本（serial 0x3d）"（`03-slot-target-check.txt:13`）。
* 上传后盒子自己写 flash 并重启；实测 `02:08:21` 上传受理 → `02:09:47` 新系统 ssh 回来（`05-post-flash-poll.txt`）。

### 5.4 刷后复验（新系统 = slot1）

| 判据 | 期望 | 命令 / 出处 |
|---|---|---|
| 跑在 slot1 | `running slot: 1`，`root=/dev/mtdblock7` | `e2631-fwslot info`（`07-fwslot-info.txt:2`）；`iter.sh` 的 `step_flash` 用 `root=/dev/mtdblock7` 当硬判据 |
| serial 递增 | **原厂写入时的活动槽 serial + 1**（**不是 max+1**）：原厂侧当前槽 slot0=61 ⇒ 刷后 slot1=**62**；刷前 OpenWrt 正跑 slot1/62，若按 `max+1` 会误算成 63 | `$H/flash-p1/20261104-0545/logs/01-pre-baseline.txt:7-8`（slot0 serial=61、slot1 serial=62）；刷后仍 `61/62` 见 `07-post-verify-10.txt:8-9`；语义原文 `RESULT.md:72-75`；理由 `flashing.md:99-100` |
| 版本串/构建号变了 | slot0 `V1.0.0.8B8.9900`/`20260826135759` → slot1 `…8000`/`20261104` | `07-fwslot-info.txt:3-4` |
| slot0 未被写 | `sha256sum /dev/mtd5` 与刷前同值 | `20-final-state.txt:16` = `6adbec13…a681`；对照 `RESULT.md:71-72` |
| 台架地址 | 刚刷完只有 `192.168.1.1`（默认拓扑），**不是** `192.168.5.1` | `09-bench-state-initial.txt:6-8` 是"63 已跑过"的样子；默认态见 `02_network:13` |

> ⚠ **下一颗镜像（不带 63）刷完，`192.168.5.1` 一定不可达**，这不是刷坏——是默认拓扑。
> 走 `192.168.1.1`（宿主 `ens34` 有 `192.168.1.x/24`）进去重建台架（§3）。

> **首启还有两类"计数 / 日志"判据，两条都实测踩过**：① **六计数跨两个文件**——`diag` 里只有
> `rx_nobuf`/`ffe_dup`/`rx_alias`，`bp_double_free`/`rx_poison`/`bp_state_stale` 在 **`dma_state`**，
> **两个文件分别 grep**（只 grep `diag` 只有 3 个）；② 默认态"零新日志"用 `logread -e e2631-rxsplit | wc -l` = **0**——
> 本机 busybox `logread` **不认 `-t`**（`-t` 只被当成短选项解析失败、退化成全量输出 531/578 行），
> 照字面写 `logread -t …` 会把"默认无日志"误判成 FAIL。
> 原文：`$H/flash-rxsplit/20261104-2035/logs/10-firstboot-7checks.txt:22-33`、`11-diag-and-logread.txt:86-107`、`12-sixcounters-locate.txt:1-15`。

## §6 Part D —— 刷后"零手工写入"怎么验

> 口径先钉死：**只有镜像自己负责的默认值**才谈"零手工写入"。台架（`192.168.5.1`/IPv6/5201 放行）
> 从 `bbee661` 起就**必须**手工重建，把它算进"零手工写入"会自欺。
>
> **最干净的验法**：刷完第一次开机、**还没动任何台架配置**时，先走 `192.168.1.1` 抓这一遍
> （此时盒子上只有镜像自己的配置，任何"手工写入"都还没发生）；重建完台架后在 `192.168.5.1` 再抓一遍，
> 两遍应当只有台架相关项不同，镜像默认值那几项逐字相同。

```sh
# 第一遍用 192.168.1.1（刚刷完、未动台架）；第二遍重建完台架后用 192.168.5.1
# 第一遍必须带 -b 192.168.1.50（默认拓扑下源地址会选成 192.168.5.50 ⇒ 100 % 超时，见 §3.2 的 ⚠）
ssh -b 192.168.1.50 root@192.168.1.1 '
  echo "--uptime";   cat /proc/uptime
  echo "--params";   for p in zxcoal_us zxcoal_frames zxnapi_weight; do echo "$p = $(cat /sys/module/zx279128s_eth/parameters/$p)"; done
  echo "--reg";      grep -E "dma\+(030|034)=" /sys/kernel/debug/92000000.ethernet/dma_state
  echo "--logread";  logread | grep zx_ffe_coalesce
  echo "--rc";       ls -l /etc/rc.d/S99zx_ffe_coalesce; ls -l /etc/config/zx_ffe
  echo "--uci";      uci show zx_ffe
  echo "--dmesg";    dmesg | grep -E "zxcoal|ffe_learn"
'
```

| 验收项 | 期望 | 出处 |
|---|---|---|
| 三个参数 | `250 / 16 / 64` | `08-defaults-verify.txt:3-5` |
| 寄存器读回 | `dma+030=000030d4`（=12500 ticks=250 µs）、`dma+034=00000010`（=16） | `20-final-state.txt:29-30`；`08-defaults-verify.txt:47-48` 是驱动自己打的 `raw 12500 -> 12500 (readback 12500)` |
| 开机应用日志 | `zx_ffe_coalesce: us=250 frames=16 weight=64 applied+readback ok` | `20-final-state.txt:39` |
| 软链 | `S99zx_ffe_coalesce -> ../init.d/zx_ffe_coalesce` | `08-defaults-verify.txt:38` |
| 配置文件 | `/etc/config/zx_ffe` 存在且 `uci show` = 250/16/64 | `08-defaults-verify.txt:31-43` |
| dmesg 异常 | `grep -cE 'oops\|panic\|BUG:\|stall'` = 0 | `RESULT.md:93` |
| 加速态 | `accel_ipv4=hw`、`accel_ipv6=ffe`、`ppe_bound=1` | `20-final-state.txt:45-48` |

**"零手工写入"的自证方式**：上面这些在**刷完第一次开机、没执行任何 `echo > /sys/module/...`** 的前提下就成立。
上一轮直接查了 pre/post 参数快照证明"本轮没有任何 `echo > /sys/module/...` 动作"（`RESULT.md:76-80`）。
**下一轮要重复这个证明**：把 `08-defaults-verify.txt` 那一组命令在"刚刷完 / 打流后"各存一份，
两份都等于 250/16/64 且中间没有任何写动作日志。

## §7 Part E —— 测量口径与打流

### 7.1 三个数字的正确含义（混用会得出错结论）

| 数字 | 含义 | 原文 |
|---|---|---|
| **941.2 Mbit/s** | **v6/TCP 载荷上限**：MSS 1440 + 40(IPv6) + 20(TCP) + 14 + 4 + 12(IFG，不计前导) = 1530 B ⇒ `1440/1530` | `$H/duplex-attr/20261104-0130/RESULT.md:85-86` |
| **987.0 Mbit/s** | **L2 线速**：1514+8+12 = 1534 B/帧 ⇒ 81,486 fps ⇒ `81486×1514×8` | 同上 `:78-84` |
| **735 Mbit/s** | **不是硬件上限**：宿主**单进程 iperf3 在 UDP 1452 B 上**只能推到 ~735（`-b 900M` 实发 735、`-b 0` 612、`--pacing-timer 100` 708） | `$H/tm-regdiff/20261104-0335/RESULT.md:169`；`$H/ONBOX-STATE.md:628` |

**口径三条（写死在测量脚本注释里）**：

1. iperf3 的 `Mbit/s` 是 **TCP 载荷口径**（`bits_per_second` 按流内字节算），拿它比 941.2，**不要**比 987.0。
2. **双向每向各自受 1 Gbps 链路限制，合计可以超 941。** 判据不是"总和 ≤941"，而是**每向各自**与单向基线比。
   实测锚点（原厂固件上、2026-11-04 01:49–01:52 **我们自己补测**的，**不是原厂发布记录**）：`iperf3 --bidir -P 1` 三格
   〔**`--bidir` 口径已于 2026-11-04 裁定禁用（"跑不满"）：以下三格数字只留档、不作判据；合法双向锚点改用"方式 A" 1176.7 / 1302.1，见下句**〕
   每向 **586.3/714.3、653.9/478.9、578.5/712.1**，**真实合计 1300.5 / 1133.0 / 1290.3**
   （`$H/vendor-bidir-audit/20261104-1955/RESULT.md:27-29`、`logs/bidir-fields.tsv:2-4`；原厂历史数据只有单向、从无双向，
   `$H/vendor-duplex/20261104-0230/TASKBOOK.md:6`）。方式 A 两次 1176.7/1302.1
   （`$H/vendor-duplex/20261104-0230/RESULT.md:118,119`，两条独立连接、真两向，未改）。
   *合计口径注*：上面三个合计是审计表的**接收侧**列；按发送侧逐字段复算是 **1300.6 / 1132.8 / 1290.6**
   （`logs/V-bidir{1,2,3}.json` 的 `sum_sent` + `sum_sent_bidir_reverse`），两者差 ≤0.3，不影响结论。
   ⚠ **旧值 1172.7 / 1307.7 / 1156.7 已作废**：那是把正向的 `sum_sent + sum_received` 相加（同一方向算了两次），不是两向合计。
   另：原厂这三格是 **`-P 1`**，我方对照是 `--bidir -P 8` 应用层 937–940〔**2026-11-04 起该口径已禁用，仅留档、不作判据**〕（`$H/cpu-headroom/20261104-1935/RESULT.md:10`；
   每向 `lan1 rx 487.9 / tx 496.4`，`$H/wire-rate/20261104-1955/RESULT.md:16-17`）⇒ **并发/方法不同，不能当"同方法对照"用**。
3. **UDP 口径不同**：iperf3 3.18 的 UDP `bits_per_second`/`bytes`/`packets` 是**发送端**口径，
   `lost_percent` 才是接收端；要对账得用 `sum_received.bytes` 或 `(packets−lost)×1452`，
   且 UDP 格**不要带 `-O`**（否则盒侧窗口比 iperf3 窗口多 n 秒）（`$H/duplex-split/20261104-0315/RESULT.md:19-23`）。

> **以下字段口径已于 2026-11-04 禁用，仅用于读历史 JSON**。**已知口径坑（2026-11-04 20:05 补；本仓已踩过两次）**：iperf3 3.18 的 `--bidir` JSON 里，**反向统计在
> `sum_sent_bidir_reverse`，而 `sum_received` 是正向的镜像**。任何逐向统计必须取：正向 `sum_sent` / `sum_received`，
> 反向 `sum_sent_bidir_reverse` / `sum_received_bidir_reverse`。把 `sum_sent + sum_received` 相加 = 同一方向算两次，
> 这正是 1172.7 / 1307.7 / 1156.7 的来历（逐字段复算见 `$H/vendor-bidir-audit/20261104-1955/EVIDENCE.md:77-107`，
> 原始 JSON `$H/vendor-duplex/20261104-0230/logs/V-bidir1.json:1536-1568`）。另外反向 `retransmits` 字段不存在，
> 别引用（`$H/v4-e1e3-stability/20261104-0515/RESULT.md:139`）。

> ⚠ **一处未定论的冲突（本文采用"每向独立"这一支）**：
> `$H/duplex-capacity/20261104-0247/RESULT.md:13-16` 写"盒子的双向不是 2×1 G，而是两向共享约 0.96 Gbit/s，
> 900+900 物理上不可能"。但它测的是**我方镜像**的上限；同一台架**原厂**用唯一合法的双向口径
> ——**方式 A 两进程**（两条独立连接、真两向）——实测合计 **1176.7 / 1302.1**，两者都超过 0.96 Gbit/s 的"共享额度"
> （`$H/vendor-duplex/20261104-0230/RESULT.md:118,119`，补测在 01:49–01:52，早于它）⇒ "共享额度"不能当**链路**结论用，
> 最多是"我方实现的当下上限"。**本裁决以 1176.7 / 1302.1 为准**；`--bidir` 三格合计 1300.5 / 1133.0 / 1290.3
> **仅留档对照、不参与裁决**（该口径已禁，见 §7.1）。这条分歧本身见 §9 的**【未核】**第 6 条。

### 7.2 该用哪个方向（单流 / 双向）

| 场景 | 用哪个 | 理由 |
|---|---|---|
| 单向基线（UDP 1452 口径） | **rev**（服务器发，`-R`），或同向双流 450+450 | 宿主单进程 fwd 打不满（实测 735/900）；同轮 rev 真发满 900.2 Mbit（`$H/tm-regdiff/.../RESULT.md:36,169`） |
| 单向基线（TCP 口径） | fwd 也能到 **913.95 / 916.46** | `$H/flash-land/.../RESULT.md:145-146`（30 s、`-O 2`、IPv6 TCP）⇒ "fwd 只能 735"只对 UDP 1452 成立，别外推 |
| 双向 | ❌ **`--bidir` 已禁用**（"跑不满"，只留档、不作判据）；双向一律走下一行的**方式 A：两进程并发** | 用户 2026-11-04 裁定。`--bidir` 的字段坑仍要记着（`sum_received` 是正向镜像，`$H/duplex-capacity/.../RESULT.md:31`），但只用于读懂旧记录；双向**基线要按方式 A 重测** |
| 方式 A（两条独立连接、方向相反） | `-p 5201` + `-p 5202 -R` 同时起 | `$H/duplex/20261104-0045/logs/measure4.py:6`；两个端口避免抢同一 server 进程（`TASKBOOK.md:43`） |

**⚠ 上一轮的数值被脚本读错了字段，别照抄**（下表是 `--bidir` 口径，**该口径已禁用，只作"字段纠错"示例，不作基线**）：`$H/flash-land/.../logs/measure5.py:254-257` 把 `--bidir` 的反向
当成 `sum_received`。用同目录 JSON 复算：

| 格 | fwd | rev（`sum_sent_bidir_reverse`） | 真实合计 | RESULT 里写的 |
|---|---|---|---|---|
| N-bidir1 | 370.21 | **426.06** | **796.28** | 740.47 |
| N-bidir2 | 390.28 | **468.08** | **858.36** | 780.59 |
| N2-bidir1 | 401.11 | **474.65** | **875.77** | 802.20 |
| N2-bidir2 | 408.55 | **474.57** | **883.12** | 816.98 |

⇒ 我方新镜像的双向合计真实是 **796–883**（中位约 **867**），不是 740–817；"双向只有单向的 0.865×"
这个比值也跟着要重算。**下轮脚本一律不用 `--bidir`（双向走方式 A 两进程）；只有读历史 JSON 时，反向才是 `sum_sent_bidir_reverse`。**

### 7.3 打流命令模板与门禁

```sh
SRV6=$(ssh root@192.168.5.1 "ip -6 neigh show dev br-lan | awk '/d8:43:ae:68:18:28/ && \$1 ~ /^fd/ {print \$1; exit}'")
#   地址不要照抄：**每次重装都要现查**（`uci get network.globals.ula_prefix` + `ip -6 addr show dev br-lan`，
#   见 §3.5 第 1 步）。上面这条命令本身就是"现查"（从 br-lan 邻居表按服务器 MAC 取）；
#   2026-11-04 这轮取到形如 fd70:…:5ace 的值，下一颗镜像大概率又变。

# 单向（rev：服务器→宿主，反向）
iperf3 -6 -c "$SRV6" -p 5201 -R -t 30 -O 2 -J > rev.json
# 单向（fwd：宿主→服务器，TCP 可用；UDP 1452 会被宿主单进程限到 ~735）
iperf3 -6 -c "$SRV6" -p 5201    -t 30 -O 2 -J > fwd.json
# 双向（方式 A：两进程并发；`--bidir` 已禁用，见 §7.2）
iperf3 -6 -c "$SRV6" -p 5201    -t 30 -O 2 -J > bidir-fwd.json &
iperf3 -6 -c "$SRV6" -p 5202 -R -t 30 -O 2 -J > bidir-rev.json &
wait
```

来源：`$H/flash-land/20261104-0430/logs/measure5.py:9,228`（`fwd=-p 5201` / `rev=-p 5201 -R`，
`-t 30 -O 2 -J`）。**双向模板已改按方式 A**（`-p 5201` + `-p 5202 -R` 并发，`$H/duplex/20261104-0045/logs/measure4.py:6`）；
`measure5.py` 里的 `bidir=--bidir` 已按用户 2026-11-04 裁定弃用。实际命令行原文见每格 `meta.json` 的 `iperf_cmd`。
> ⚠ **又一处不一致**：`$H/flash-land/.../RESULT.md:138` 说"每格 `-B` 用 `fd00:5::250`"，
> 但 `measure5.py` 里 `grep -c '"-B"'` = **0**、`meta.json` 的命令里也没有 `-B`。
> 本文按**原始命令**写（不带 `-B`，源地址由路由选）；要复现得完全一致，就照 `meta.json` 抄。

**门禁（不过不许起流）**：

```sh
ssh root@192.168.5.1 'cat /proc/uptime; dmesg | grep ffe_learn'
# 判据 1：/proc/uptime 第一个字段 ≥ 130
# 判据 2：dmesg 里有 "accel: ffe_learn auto-enabled 120000 ms after probe"
```

* 原文：`16-gate-check.txt:1` = `159.75`；`20-final-state.txt:43` = `[ 121.439617] … auto-enabled 120000 ms after probe`。
* 含义：**开机延时自动开学习、只开一次**（不是老化；老化是 `ffe_aging=300` s）。
* ⚠ 上一轮在 uptime **68.83 s** 时抓 `--dmesg-ffe_learn` 是**空的**（`08-defaults-verify.txt:6,49`）——
  不是没开，是还没到 120 s。**先等门禁再打流**。

### 7.4 已被真机否定的旋钮：RX 分环（`rx_split`）——默认 `N`，**永不默认开**

> **结案（2026-11-04 20:47–21:14，运行期；未刷机、未写 flash、未重启，`boot_id` 开工=收尾）**：
> 分核**确实生效**，但**不提升吞吐**；且**"分环 + `napi1`（多一条 threaded NAPI）"这个组合态对单流 `-R` 有害**——
> 只能说组合态有害，**不能说"流量分环本身"有害**（因果未分离，见本节末尾"因果边界"）。本节数字全部取自
> `$H/rx-split-onbox/20261104-2050/RESULT.md`（原始数据同目录 `logs/summary-table.csv` 与每轮 `*-iperf3.json`），
> 并已由对抗复核逐位复现（`$H/rx-split-recheck/20261104-2130/RESULT.md`，2026-11-04 21:30）。

| 场景（接收侧 Mbit/s，60 s × 3 轮取中位） | `rx_split=0`（三轮） | `rx_split=1`（三轮） | 中位变化 |
|---|---|---|---|
| **单流 `-R`（服务器→宿主；本节的唯一判据臂）** | **916.29**（923.17 / 802.28 / 916.29） | **709.21**（721.22 / 705.77 / 709.21） | **−22.60 %** |
| `--bidir -P8`（**只留档，不比较**） | 937.75 | 907.10 | −3.27 %〔该口径已禁用：跑不满，见 §7.2〕 |

- **−22.89 % 是备选口径**：剔除基线异常轮 802.28（真实的两段秒级塌陷，不是慢启动，`RESULT.md:63-65`）后，
  基线中位（=均值）**919.73**，分环仍 709.21 ⇒ **−22.89 %**；结论不变、幅度更大（`rx-split-recheck/20261104-2130/RESULT.md:69-73`）。
- **形态是"停顿变频繁"，不是"容量降到 709"**：60 个 1 s 区间里，分环臂每秒 <700 Mbit/s 的秒数是 **27 / 29 / 32**
  （基线 **2 / 12 / 2**），**峰值仍 933.4 / 933.5 / 933.6，基线 936.7 / 938.1 / 936.8** ⇒ 每秒在 933 ↔ 300–600 之间反复
  （`rx-split-recheck/20261104-2130/RESULT.md:164-177`）⇒ 别把它当"带宽被砍掉"去调容量类参数。
- **判据**：每臂 3 轮、取中位，两组**区间不重叠**。**噪声 3.2 % 的出处**＝`$H/rx-block-map/20261104-2030/` 的
  **单向下行基线 899.25–931.80 的散布**（`RESULT.md:98`）——**不是 `--bidir` 组间距**（那个出处已随口径禁用作废）
  ⇒ 中位比超过 3.2 % 才算信号，单轮不作判据。`-R` 两臂逐轮值见 `rx-split-onbox/20261104-2050/RESULT.md:20-21`；705.77–721.22 vs 802.28–923.17
  不重叠、−22.60 % 是噪声的 **7 倍** ⇒ 判"有害"，不是"没提升"。
- **别用 `--bidir` 复核这件事**：双向判据一律"**方式 A 两进程**"（`-p 5201` + `-p 5202 -R` 并发，见 §7.2/§7.3），
  且**双向基线要按方式 A 重测**；上表 `--bidir` 那行只作历史留档，不得引用为基线。
- **回退后复核**：60 s `-R` = **921.59**（回到基线带内），`RESULT.md:22`、`logs/D9d-recovery-down-60s-iperf3.json`。

**别把"没生效"当结论**——分核确实发生了（`RESULT.md:39,44,45`）：

- `rxq1 +618882` 对 `lan2 rx +618880`（比例 **1.0000032**）；`rxq0` 只吃宿主 ACK；
  `napi/lan3-11` 线程 `psr=0` ⇒ 绑在 **CPU0**（`rx_split_cpu=0`，0-based）。
- CPU1 sirq **62.3 % → 34.1 %**，CPU0 sys **3 % → 36 %**，后者正好等于 `napi1` kthread 的 jiffies：
  threaded NAPI 的时间记在 **`sys`**、不在 `sirq` ⇒ "两核各 30 % sirq"是把列读错了。
- `/proc/interrupts` 的 ethernet 行**始终 1 行**（分环不加中断行、只放大次数）⇒ **行数没变不能当"没拆成"的证据**。

**代价：共享 RX 中断放大 6.4×**。单流 `-R` 从 **4.8k/s**（256 685–286 527 次 / 56.3–57.2 s）涨到 **32.2k/s**
（1 796 015–1 834 117 次 / 56.3–57.2 s）〔原文报 4.6k→30.5k 是**分母错**：Δ 只覆盖 56–57 s 却除以 60 s，
绝对速率低估 5–6 %，倍数不变，`rx-split-recheck/20261104-2130/RESULT.md:125-138`〕；
`--bidir` 那组 2.1×（同口径，仅留档）。结构原因：`napi1` **从不写** `ZX_TM_INT_MASK`，
而 `napi0` 在 `work_done < budget` 时解屏蔽 ⇒ 被 q1 里没取的帧反复触发（`RESULT.md:56,58`）。

**因果边界（引用本节必须照这个口径）**：**基线全在前（20:55–20:59）、分环全在后（21:07–21:11）**，**无 A/B/A 交错**；
回退只有 **2 个点**（10 s 929.55、60 s 921.59）；且**从未测"`rx_split=Y` + trap 全 0"这个对照态**
（`rx-split-recheck/20261104-2130/RESULT.md:14,188,194`）⇒ 只能说"**分环 + `napi1` 组合态**（CLA block1→qid1、q1 真有帧）对 `-R` 有害 −22.6 %"，
**不能说"流量分环本身有害"**——若掉速只由"多一个 threaded NAPI + 共享中断语义"造成，"分环有害"就说过头了。
机制层也未坐实（两核都没饱和；候选：中断放大 / 两核抢 TX 批量 / 共享 RX 归还锁），
按任务书"退化 >10 % 即停手"规则本轮停在这里（`RESULT.md:85`）⇒ 别为了"找回那 22 %"再开分环。
要坐实因果，最小代价是**交错 A/B/A**（分环/回退/分环各 2 轮）或 **`rx_split=Y` + trap 全 0** 对照。

> **10 s 单轮同样不能作判据（同配置差 42 %）**：B6 的 10 s `-R` = **412.46 Mbit/s**，同配置 B6b = **710.94**
> （`rx-split-onbox/20261104-2050/logs/B6-verify-split-10s-iperf3.json:278`、`B6b-...:278`）；B6 的 1 s 序列前 5 秒是
> `194, 52, 49, 52, 48`（几乎停死），随后才回 900+。所以回退侧那个 10 s 929.55 只是弱旁证，**别只挑好的那格报**
> （`rx-split-recheck/20261104-2130/RESULT.md:198-200`）。

**开关（默认 `N`；顺序不能反）**：

```sh
P=/sys/module/zx279128s_eth/parameters
cat $P/rx_split      # 期望 N；镜像默认就是 N，永不默认开
# ——若确要开（只用于验证，不要进镜像默认）——
timeout 10 sh -c 'echo Y > /sys/module/zx279128s_eth/parameters/rx_split'   # kstrtobool：Y/N/1/0 都收
```

> 顺带：只 `echo Y` 而**不写 CLA** 时 q1 永远空，分环等于多建一个什么都轮不到的 NAPI（不丢帧、也没收益）——
> 那是 `known-issues.md` 的 `RXSPLIT-CLA-BOOT-ORDER`，别跟本节"拆了确实生效"混为一谈。

**回退**（先清 CLA、再关 `rx_split`）：

1. CLA：block1（base `0x100` = lan2 入向）trap 0..127 写 **qid=0** —— 下标是 **`0x100 + trap`**（不是 `(block<<7)|trap`）；
   写法是**一次 sysfs 写最多 8 对 `off=val`**（`zx279128s-ppe.c:2064,2099`，第 9 对起被静默丢弃），一行写 4 个 trap（`14=cmd,1c=qid`）。
2. 读回**只能** `echo 4 > $P/dump` 再取 dmesg 的 `cla-reg 010:` 行（一行 4 个字 = `0x10/0x14/0x18/0x1c`）：
   第 2 字 = `0x14`（命令）、第 3 字 = `0x18`（**bit0=1 = idle/完成**，不是 busy）、第 4 字 = `0x1c`（word0 = qid）。
   例：`cla-reg 010: 00000000 09c00100 00000001 00000001` ⇒ 命令 `0x09c00100`、状态完成、word0=`1`（`$H/rx-split-onbox/20261104-2050/logs/B5c-cla-protocol.txt:7`）。
   ⚠ `cla_write` 参数本身**不能**当读回：`.get = param_get_int` 读静态 storage、从不赋值，永远 `0`（`zx279128s-ppe.c:2118-2121`）。
3. 再 `timeout 10 sh -c 'echo N > /sys/module/zx279128s_eth/parameters/rx_split'`；验收四条：
   **rc=0 不挂死**（B1 死锁修复有效）、读回 `rx_split=N`、`napi/lan3-11` 消失、dmesg 无 `oops/BUG/stall`。
4. CLA 全 0 的读回：`0x100`、`0x110`、`0x17f` 都应 `00000000`（手工 128/128 清 0 通过，`RESULT.md:66`）；
   随后 60 s `-R` 回到 **921.59**。

**⚠ 镜像里 `/etc/init.d/e2631-rxsplit` 现在有缺陷（源码已修、镜像未重建），在旧镜像上不要用它**（`RESULT.md:64-66,76`）：

- `verify_trap` **极性写反**：脚本把 `0x18` 的 **bit0=1** 当 busy 拒收（`e2631-rxsplit:145-169`，实际判据是
  `case "${3:-}" in *[02468ace]) ;;`），而驱动语义相反——`zx_pp_wait()` 等的就是 `val & 1`（ready），
  `zx_pp_write()` 下命令后先自旋等 bit0 变 0、再等它回 1（`zx279128s-ppe.c:690-716`）。
- 真机后果：`start` 在 **trap 0** 就判失败并回滚（rc=1，分环其实没打开）；`stop` 同样在 trap 0 失败，
  **只把 trap 0..15 写成 0**（rc=1），`0x110`/`0x17f` 残留 `1` —— 看着"回退成功"其实没退干净。
- 修好之前：进/退一律照上面手工做，或直接用 `$H/rx-split-onbox/20261104-2050/scripts/rxsplit-apply.sh`
  （改过极性、实测 128/128 通过）。❗`logread -e e2631-rxsplit` **不是判据**：128 次读回 dump 会把 syslog 环冲掉
  （收尾时已是 0 行，`RESULT.md:81`）。
- **源码已修（2026-11-04 21:35，离线）**：`verify_trap` 改 `dump=4` + dmesg 逐条比对，`stop`/`start` 回滚
  **无条件清 block1 全 128 项**，加 `RXSPLIT_FORCE=1` 封禁门（`$H/rxsplit-initd-fix/20261104-2130/RESULT.md`）；
  **但盒上跑着的镜像里仍是旧（坏）版，下次构建才生效** ⇒ 在旧镜像上仍按上两条手工做。

## §8 止损 / 回滚

| 现象 | 动作 |
|---|---|
| 刷完 `192.168.5.1` 不通 | 正常（默认拓扑）。走 `192.168.1.1` 或串口，按 §3 重建 |
| 刷完 `192.168.1.1` 也不通 | **先排宿主源地址坑**：`ip route get 192.168.1.1` 看 `src`，不是 `.1.50` 就用 `ssh -b 192.168.1.50` / `ping -I 192.168.1.50` 重试（`$H/flash-p1/20261104-0545/logs/06-post-flash-poll.txt:1-10`）；仍不通再串口看 cspboot：`correct header has been found @…`、`pdt_getimg select = …`（`flashing.md:87-93` 的判读形态） |
| 新系统起不来，要回原厂 | **从金库 `工作文件/03-固件备份/vault-20261103/` 取源**，不许删不许覆盖；路线仍是"进 slot0 → WebUI" |
| 构建出新文件没进镜像 | 按 §4.4 从第 4 层往上查；最常见就是漏了 `package/base-files/{clean,compile}` |

## §9 【未核】清单（本文里凡标这个的都别当结论用）

1. Windows 服务器侧怎么配（静态 `192.168.1.243`、`iperf3 -s -p 5201/5202` 的启动方式、防火墙白名单）——
   只有"用户起的、只连不重启"的记录，没有配置步骤。
2. `make world`（路线 A）会不会自动重编 `package/base-files` 的 apk——没有验证过；本文一律要求显式
   `clean+compile`，不依赖它。
3. 把 B-2/B-3/B-4 合并成"一遍容器内顺序执行"是否等价——没实测。
4. 宿主 `ens32`=`192.168.43.239` 这个背景事实：本轮所有产物里没有出现，只在旧记录里；`ens33`=`192.168.146.128`
   有（`18-E-host-nic.txt:15`）。
5. 宿主 `192.168.1.x` 到底是 `.50` 还是 `.250`：日志是 `.50`，`iter.sh:83` 默认 `.250`，未见谁改过。
6. `$H/duplex-capacity/20261104-0247` 的"两向共享 ~0.96 G"与原厂**方式 A** 实测合计 **1176.7 / 1302.1** 的矛盾
   （`--bidir` 三格只留档、不参与该裁决），没有找到定论文档。
7. 串口桥（`ch343-bridge.py` → `127.0.0.1:5555`）的启动参数与接线——本文只写"必须先起"，没核具体命令。
8. 打流到底有没有用 `-B fd00:5::250`：`RESULT.md:138` 说用了，`measure5.py`/`meta.json` 里没有；
   两者只能信一个，本文信原始命令。
9. `$O` 这棵树当前 `.config`/seed 的具体内容（没读；`BUILD.md:29-42` 只说固定 SHA）。

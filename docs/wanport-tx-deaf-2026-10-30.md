# 「任意口当 WAN 之后只收不发」是一次误判（2026-10-30 晚推翻）

> 这条曾经的"bug"不成立：**驱动没有 bug，VLAN 表写成 0 也不会掐死发送方向。**
> 出事那轮实验里测试机插在 **lan1**，而布局假设它插在 lan2 —— 把端口认错之后，
> 每一个观察到的"症状"都能用"回复从真正的 WAN 口走了"解释干净。
> 本文记录证据链、当初为什么会被骗，以及以后怎么在 30 秒内确认测试机到底插在哪个口。

## 1. 结论

| 曾经的说法 | 现在的事实（都是本轮实测） |
|---|---|
| 端口被移出 br-lan（`members == 0`）后，盒子发不出帧 | **发得出去**。WAN 切到 lan1 并重启后，主机（就在 lan1 上）ping 3/3、ssh 正常、盒子反向 ping 4/4，抓包双向可见 |
| 根因是 `zx_sw_apply()` 在 `members == 0` 时把 VID 1 表项写成 0 | **不成立**。在跑着的独立 WAN 布局上直接把 `vlan[1]` 写成 0，双向收发照旧（§4） |
| 该改法已进驱动 | **已回退**，恢复上游语义（`members ? vlan \| VALID : 0`） |

## 2. 怎么发现端口认错了

在 `br-lan = {lan1, lan3}`、`lan2` 独立这种布局下，给 lan1 发 40 个**广播帧**
（ethertype 0x0806 / 0x0800 / 0x86dd / 0x88b5 各 10 个），然后读四个口的收包计数：

```
发帧前: lan1 321  lan2 302  lan3 0  wan 0
发帧后: lan1 361  lan2 316  lan3 0  wan 0     ← 40 帧全部落在 lan1
```

**lan1 +40，lan2 一动不动。** 测试机 `ens34` 接的是 **lan1**，不是之前记录的 lan2。
用不同 ethertype 分别发，可以排除"某种协议被 ACL 丢掉了"这一层解释——四种类型的都走 lan1。

## 3. 用正确端口复测：独立 WAN 口收发都通

```sh
# 盒子上（串口或 ssh）：把 WAN 切到测试机所在的 lan1，然后重启
e2631-wanport apply lan1 --force && reboot
```

起来后的现场（`uci show network` + `ip addr`）：

| 对象 | 值 |
|---|---|
| `network.wan.device` | `lan1` |
| `lan1` | `192.168.88.1/24`、`fd00:aa::1/64`、`fe80::e660:4dff:fe86:71f0/64`（WAN 固化 MAC `e4:60:4d:86:71:f0`） |
| `br-lan` | `192.168.1.1/24`，成员 `lan2 lan3 wan` |
| `sw_members` | **0x0**（br-lan 里只有 lan2 一个口有链路，算不出硬件交换同伴，符合预期） |

测试机 `192.168.88.2/24` + `fd00:aa::2/64` 侧的结果：

| 方向 | 命令 | 结果 |
|---|---|---|
| 主机 → 盒子 | `ping -c3 192.168.88.1` | **3/3**，`lladdr e4:60:4d:86:71:f0` |
| 主机 → 盒子 | `ssh root@192.168.88.1` | 通，`uname -r` = 6.18.55 |
| 盒子 → 主机 | `ping -c4 -I lan1 192.168.88.2` | **4/4**，`lan1 tx 15 / rx 25` |
| 主机侧抓包 | AF_PACKET 计数 | 盒子发了 4×0x800 + 1×0x806，主机回了 4×0x800 + 1×0x806 |

也就是说：**一个不在任何网桥里的独立口，收发都正常**，`members == 0` 不影响它的发送。

## 4. 直接证伪"VLAN 表写 0 会掐死发送"

在这一布局（已经能正常通信）下，用寄存器工具把 VID 1 表项改写成 0 —— 也就是当初
怀疑的"罪魁祸首"那个值：

```sh
/tmp/zxtool vlan 1 00000000 00000000 00000000 00000000   # 写 0
ping -c3 192.168.88.2          # 盒子 → 主机：3/3
# 主机 → 盒子：ping -c3 192.168.88.1 也是 3/3
```

双向照旧。再把它改回驱动实际写的值（全端口 untag + VALID = `0xaa9`），同样正常。
所以 VLAN 表项内容不是原因，两个方向都通。

（寄存器工具 `/tmp/zxtool` 源码见 `.scratch/ffe-opt/zxtool.c`：`dump` 打印交换块全部
相关寄存器 + VID 0..4 表项，`vlan/wr/rd/trap` 可以单点改写，`raw` 按指定 ethertype
发广播帧。它把 0x92000000 起的 4 MiB 映射进来，只读不写地盘，非常适合这种对照实验。）

## 5. 当初那些"证据"为什么看起来成立

出事那轮的现场是：`e2631-wanport apply lan2 --force` + 重启，配置层面 `wan device = lan2`、
`lan2 = 192.168.88.1/24`；测试机配 `192.168.88.2/24` 却插在 **lan1** 上。于是：

| 当时的观察 | 真实原因 |
|---|---|
| 主机 ping 盒子：ARP 拿不到，`No route to host` | 主机的 ARP 请求进了 br-lan；盒子给 `192.168.88.1` 的 ARP 应答按路由从 **lan2** 发出（那口上是另一台机器），主机永远收不到 |
| `lan2` 的 `rx_packets` 在涨、`tx_packets` 不动 | 涨的那点计数是背景流量；主机帧根本不在 lan2 上（§2 的对照测试能一眼看穿） |
| 盒子 ping 自己的 LAN 对端（桌面）也失败 | 桌面那轮是否在线没有独立验证过，不能算证据 |
| `sw_members = 0x0` | **正常**。br-lan 里只有 lan2 一个有链路的口，算不出"两个口同桥"，硬件交换本来就该关 |

三条教训，写进流程：

1. **每次动网络布局之前，先确认测试机在哪个物理口**（§6 的做法，30 秒）；
2. 端到端不通时，先分清"帧没发出去"和"帧发到别的口去了"——两者的处置完全不同；
3. `sw_members`、MAC MIB 这类计数只能当"有没有动作"的旁证，不能单独当"帧上没上线路"的证据。

## 6. 30 秒确认测试机插在哪个口

```sh
# 主机侧：四种 ethertype 各发 10 个广播帧（避开按 ethertype 抓包的陷阱）
python3 - <<'PY'
import socket, struct
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW); s.bind(('ens34', 0))
for et in (0x0806, 0x0800, 0x86dd, 0x88b5):
    f = b'\xff'*6 + bytes.fromhex('000c294b592a') + struct.pack('!H', et) + b'\x5a'*40
    for _ in range(10): s.send(f)
PY

# 盒子侧：对比前后计数，+40 的那个口就是测试机所在口
ssh root@<盒子> 'for i in lan1 lan2 lan3 wan; do echo -n "$i "; cat /sys/class/net/$i/statistics/rx_packets; done'
```

在 [路由器当前状态与回滚手册.md](路由器当前状态与回滚手册.md) 里记一句：**主机的口是可变项，
历史上被换过线**，任何"哪个口是什么角色"的记录都要现测，不要沿用旧结论。

## 7. 代码状态

- `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-switch.c`
  已恢复上游写法，并在原处留了一段注释说明这次误判，避免下一个人再"修"一遍：
  `writel(members ? vlan | ZX_SBRG_VLAN_VALID : 0, base + ZX_SBRG_IND_DATA(0));`
- 该文件此前只存在于构建树、overlay 里没有的情况也一并修正（overlay 才是版本化的源）。
- 顺带确认的功能事实：`e2631-wanport apply <port>` 把任意口变成 WAN 之后，
  该口的收发、netifd 静态地址、防火墙 wan 区放行都正常工作（本轮实测 lan1 当 WAN）。

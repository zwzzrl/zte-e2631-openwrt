# 原厂「WAN/LAN 口自适应」逆向记录（2026-10-27）

目标：原厂固件支持**任意有线网口当 WAN 口**（WAN/LAN 盲插，自动识别），OpenWrt 侧目前
只认板级固定的 `wan` 口。这一页把原厂实现拆开，作为 OpenWrt 实现的依据。

所有证据都来自解密后的原厂 rootfs：`vendor-rootfs-decrypted/`（原厂 slot0 的 JFFS2 解密后
还原的目录树 + `kmodule.img` 解包结果 + `vmlinux`）。引用格式 `文件:证据`。

## 1. 结论摘要

原厂这条能力分三层，互不耦合：

| 层 | 实现 | 关键证据 |
|---|---|---|
| 界面/配置 | 「WAN 口模式：手动 / 自适应」+ 手动点选 WAN1/WAN2 的端口图 | `home/httpd/public/jquery/static/js/app.4be4c59.js` 的 `wanlanadaptive_001..018`、`0.74824e2.js` 的 `networkPort` 组件 |
| 逻辑 | cspd 内 **NetAdapt**（网络自适应）模块：探测上行 → 定角色 → 重配网桥/DHCP | `bin/cspd` 的 `NetAdapt*` 一整族符号 |
| 底层 | 内核交换/以太驱动提供「端口 WAN/LAN 模式」ioctl | `bin/switchtst -swanlanmode`、`SetWanLanMode DoIoctl Failed!(%d)` |

**对 OpenWrt 的关键结论**：`zx279128s` 的 gmac 驱动在我们这棵树上已经把 4 个物理口各做成
独立 netdev（`lan1 lan2 lan3 wan`），所以**不需要动交换芯片**，也不需要 `switchtst` 那套
`/dev/switch_dev` ioctl。等价实现可以完全在用户态完成：探测出上行口 → 把该 netdev 从
`br-lan` 摘出来绑给 `wan` 接口 → 原 `wan` 口回桥。

## 2. 界面层：原厂的「WAN/LAN 自适应」

`app.4be4c59.js` 里的文案键（原厂 i18n，值是中文）：

```
wanlanadaptive_001  WAN/LAN 自适应
wanlanadaptive_002  WAN 口模式
wanlanadaptive_003  手动
wanlanadaptive_004  自适应
wanlanadaptive_005  手动选择 WAN 口
wanlanadaptive_006  WAN/LAN 口盲插，自动识别 WAN 口
wanlanadaptive_007  2.5GE          （端口速率档位，展示用）
wanlanadaptive_008  GE
wanlanadaptive_009  WAN/LAN       （端口图标：被选中的口）
wanlanadaptive_010  单 WAN 口
wanlanadaptive_011  双 WAN 口
wanlanadaptive_012  手动选择一个 WAN 口
wanlanadaptive_013  手动选择两个 WAN 口
wanlanadaptive_014  WAN1
wanlanadaptive_015  WAN2
wanlanadaptive_016  固定 WAN 口
wanlanadaptive_017  手动选择第二个 WAN 口
wanlanadaptive_018  WAN2 启用后无法使用自适应模式
```

端口点选组件在 `0.74824e2.js`（`networkPort`），它只吃一个 `portinfo` 对象：

```js
computed: { portNumber: portinfo.portorder.length }
selectWanport(i) -> $emit('changselectedWan', portinfo.portorder[i])
isSelected(i)    -> portinfo.selectedport.includes(+portorder[i]) && !banport.includes(...)
```

即 `portorder`（面板从左到右的端口序） / `selectedport`（被选为 WAN 的口） /
`banport`（禁止选的口） / `namearr`（每口名字） / `speedarr`。

双 WAN 页面里 `selectedport` 还带一个自动推导：**端口 `Status == "Up"` 的口默认算作 WAN2 候选**
（`1.bfd8548.js` 的 `initPortInfo`）。也就是说原厂自适应模式下「哪个口插了线、且探测到上行，
哪个口就是 WAN」。

## 3. 数据模型：`OBJ_ETHPORT_INFO_ID`

页面逻辑 `home/httpd/webmodules/modules/internet_eth_port_lua.lua`：

```
GET_TYPE = wanlanadaptive   →  ManagerOBJ / DEV 查询
OBJ_ETHPORT_INFO_ID:  EthPortAliasName, EthPortUpStream, EthPortMaxSpeed, EthPortSpeed, WanType
OBJ_ETHPORT_STATE_ID: EthPortStatus, EthPortRecvRate, EthPortSendRate
OBJ_ETHPORT_RTRATE_ID: 实时速率
```

`EthPortUpStream` 就是「这个口是不是上行口」，`WanType` 是口的角色。同目录的
`internet_eth_portname_json.lua` 还给一份 JSON 版（`ETHPortIFName` + `WanType` + `EthPortStatus`），
前端端口图就是用它渲染的。

## 4. 逻辑层：cspd 的 NetAdapt

`bin/cspd` 里与这件事直接相关的符号（字符串表）：

```
NetAdaptModuleInit / NetAdaptStart / NetAdaptStart mgr
NetAdaptStartEthProbe            ← 开始端口探测
NetAdaptUpLinkProbeStart
NetAdaptUpLinkProbeOK            ← 探测成功，找到上行口
NetAdaptGetUpstreamByPhyID       ← 按 PHY 找上行口
NetAdaptGetWanPhyLinkID
SetWanLanMode  / SetWanLanMode DoIoctl Failed!(%d)   ← 定角色时下发给驱动
NetAdaptSetBrPermitPort          ← 重设 br0 允许端口
NetAdaptSetDHCP4sSwitch
NetAdaptDHCPCStart / NetAdaptDhcp6cStart / NetAdaptSlaacStart
NetAdaptEthPhyUpProc / NetAdaptEthPhyDownProc    ← PHY 上/下电事件
NetAdaptRepeatPhyUpProc / NetAdaptRepeatPhyDownProc
NetAdaptV4AddrDownProc / NetAdaptHandleGUAUp / NetAdaptHandleGUADown
WANLAN_UNCONFIGURED              ← 还没定角色的初始态
EthLink[%s],ifname[%s],WanLan[%s],Low WanLan[%s]
```

流程可以按符号名还原成：

1. 起 `NetAdaptStartEthProbe`：对所有候选口做上行探测（`NetAdaptUpLinkProbeStart`）；
2. 某口 `NetAdaptUpLinkProbeOK` 后，用 `SetWanLanMode` → ioctl 把该口设成 WAN 角色；
3. `NetAdaptSetBrPermitPort` 调整网桥允许端口（把该口从二层桥上摘掉），
   同时把 DHCP 客户端（v4/v6）挪到这个口上；LAN 侧 DHCP 服务器开关随之调整；
4. PHY up/down 事件会重跑这套流程（换线/拔插自动跟随），`NetAdaptRepeatPhyUpProc`
   处理同一口的重复上报。

## 5. 底层：交换/以太驱动的「端口 WAN/LAN 模式」

原厂带了一个寄存器级调试工具 `bin/switchtst`，usage 里直接列了这条能力：

```
switchtst -swanlanmode [auto 0][wan 1][lan1 2][lan2 3][lan3 4][lan4 5]
switchtst -sdwan [port] [mode] [WANIDX]      ← 双 WAN：把某口绑到 WAN1/WAN2
=======================Set Dual WAN ==========================
```

- 它打开的设备是 `/dev/switch_dev`（`Open switch_dev failure!`），走 ioctl；
- `bin/switchtst` 里还有大量 `-spvid/-svlan/-gpsta/-addvlantrans` 等交换机端口/VLAN 调试命令，
  说明 WAN/LAN 角色在原厂是**交换芯片端口属性**（端口被划到 WAN 域或 LAN 域），
  而不是靠 netdev 名字；
- 内核侧：解压后的 `vmlinux` 里有 `switch_dev` 与 `portinfo` 字符串，但**没有** `wanlan` 字样 ——
  `auto/wan/lan1..4` 这套模式名只存在于用户态 CLI，内核侧是数字 ioctl。

顺带：原厂 `cspd`、`libcmapi.so` 里还有 `WanPortVlanEnable/WanPortVlanMode/WanPortVlanID/WanPortPri`，
那是同一套端口概念的 VLAN/优先级参数（IPTV、双 WAN 会用到），与「角色」不是一个东西。

## 6. OpenWrt 侧现状（为什么现在做不到）

- DTS：`gmac_port0..3` 分别绑 `phy10..13`，`label` 依次是 `lan3 lan2 lan1 wan`，
  每口各自从 `macaddr_tag_1e` 取 MAC（LAN 用 base+0，WAN 用 base+1）。
  驱动（`zx279128s-main.c`）按 DTS `label` 建 netdev，**4 个口是 4 个独立 netdev**。
- 板级默认：`overlay/target/linux/sanechips/base-files/etc/board.d/02_network`
  ```sh
  ucidef_set_interfaces_lan_wan "lan1 lan2 lan3" "wan"
  ```
  也就是桥成员与 WAN 口写死在默认配置里，之后没人会去改；LuCI 里也没有「选 WAN 口」的地方。
- 因为驱动给的是独立 netdev，「把 WAN 绑到 lan1」在 uci 层面本来就做得到，
  缺的是：**(a) 自动探测哪个口是上行；(b) 把重绑做成可管理的动作并持久化；(c) 界面。**

## 7. 本仓库的实现（已落地）

不碰交换芯片、不引 `switchtst`，纯用户态：**WAN 接口绑在哪个 netdev、那个 netdev
不在 `br-lan` 里**，它就是 WAN 口。

### 7.1 配置：`/etc/config/e2631_ports`

| 选项 | 默认 | 含义 |
| --- | --- | --- |
| `enabled` | `1` | 总开关；关掉后无论怎么插线都不动作 |
| `mode` | `auto` | `auto` 盲插 / `manual` 固定口 |
| `manual_port` | `wan` | `manual` 模式下用哪个口 |
| `probe_timeout` | `3` | 每个候选口等应答的秒数 |
| `assume_single_link` | `0` | 兜底猜测：只有一根线插着时认它当上行 |
| `candidates` | `lan1 lan2 lan3 wan` | 参与自适应的口 |
| `wan_mac` | 空 | 第一次落地后固化下来的 WAN MAC |

### 7.2 服务：`/sbin/e2631-wanport`

与第 4 节逐个函数对应：

| 原厂（cspd NetAdapt） | 这里 |
| --- | --- |
| `NetAdaptUpLinkProbeStart` / `NetAdaptUpLinkProbeOK` | 候选口上跑一次 DHCP DISCOVER（`udhcpc -f -n -t 1`，用 `/usr/share/e2631-wanport/nop.script` 屏蔽脚本副作用） |
| `NetAdaptGetUpstreamByPhyID` | 有应答的那个 netdev 就是上行口 |
| `SetWanLanMode`（ioctl）| `uci set network.wan.device=<口>`（+ `wan6`） |
| `NetAdaptSetBrPermitPort` | 把该口从 `br-lan` 的 `ports` 里摘掉，其余口放回去 |
| `NetAdaptDhcp6cStart` | `wan6` 跟着同一设备走，无需单独拉起 |
| `NetAdaptEthPhyUpProc/DownProc` | `/etc/hotplug.d/iface/21-e2631-wanport`（带 20 s 去抖） |
| 开机探测 | `/etc/init.d/e2631-wanport`（`START=98`，等 netifd 建好 lan/wan 再跑） |

两条从坑里换来的规矩（实现时写进脚本头部注释）：

1. **探测前必须让口离开网桥**，否则本机 dnsmasq 就在 `br-lan` 上应答这次
   DISCOVER，任何一个 LAN 口都会被判成上行口；探测结束无论选中与否都要放回原来的
   master，否则没被选中的口就永久掉出 `br-lan`。
2. **绝不用 DHCP 探测正在当 WAN 的那个口**：netifd 自己的 udhcpc 正在它上面跑，
   再插一个同 MAC 的客户端会互相抢租约。现用口的死活改问
   `ubus call network.interface.wan status`——它 up 就直接返回，一个字节都不动。
3. **`wan6` 绑的是 `@wan` 别名时一个字都不能写**：别名本来就跟着 `wan` 走，写成
   具体设备名等于把别名换成固定设备，之后换口时 `wan6` 会落到一个不存在的 netdev
   上——IPv6 静默哑掉，没有任何日志。只有显式写了设备名才跟着换。

MAC 策略：WAN 固定用 DTS 给的 `wan` netdev 地址（`macaddr_tag_1e` base+1；
LAN 口都是 base+0），换物理口时上游看到的 MAC 不变，租约、DUID、上游 MAC 绑定
都还能续。LAN 口共用 base+0，所以把某个口从 `br-lan` 里摘掉也不会改变网桥 MAC。

### 7.3 界面：`luci-app-e2631-wanport`

`网络 → WAN 口`（`admin/network/e2631-wanport`）：当前 WAN/网桥/MAC、四个口的
链路状态与角色（每行一个「设为 WAN 口」按钮）、模式与超时设置、「立即探测」。
后端是 rpcd 的 shell 插件（`/usr/libexec/rpcd/e2631-wanport`），`set_port` 等动作
只负责把服务拉起来就返回——探测一轮要十几秒，同步等待只会让页面像卡死。

### 7.4 与原厂的差异（刻意不做）

- **不做双 WAN**：原厂 `WAN2` 是交换芯片上第二个 WAN 角色，这里是单上行；要做
  得给第二个口再建一个 interface + 策略路由，是另一件事。
- **不做面板级 `banport`**：原厂能禁用某些口不让它被选中；这里用 `candidates`
  列表表达同样的意图，够用且更好懂。
- **探测协议只有 DHCP**：原厂 `auto` 下 PPPoE / 静态上行也靠人肉选择，这里同样：
  PPPoE 用 `manual` 模式，或打开 `assume_single_link` 让它按「只插了一根线」猜。

### 7.5 怎么验

- 逻辑层（PC 上，不需要路由器）：`sh scripts/selftest-wanport.sh` —— 假
  uci/ip/udhcpc/ubus + 假 `/sys`，13 个场景（含探测失败复原、MAC 借用与恢复、
  wan6 别名、rpcd 的 JSON 形状）。故意改坏「探测后放回网桥」和「不探现用口」都会报警。
- 真机：`/sbin/e2631-wanport status` 看状态，`detect` 看探测结果，
  `DRY_RUN=1 apply lan2` 看要改哪些配置。

未解的疑问（不影响当前实现）：

- 原厂探测具体打什么包（DHCP DISCOVER / PPPoE PADI / ARP 谁先）——`cspd` 里
  `NetAdaptStartEthProbe` 附近没有明文协议名，只能看到 DHCP 客户端跟着上行口切换；
- 原厂 `auto` 模式下 `banport` 的来源（面板上禁用某些口）——大概率与 `WanPortVlan*`/IPTV 绑定有关；
- `switchtst -sdwan` 的 `WANIDX` 语义（双 WAN 顺序还是优先级），暂不需要。

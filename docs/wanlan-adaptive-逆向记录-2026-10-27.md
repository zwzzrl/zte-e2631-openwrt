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

## 7. 本仓库的实现方向（按此文档实施）

不碰交换芯片、不引 `switchtst`，纯用户态：

1. 新增 uci 配置 `e2631_ports`：
   `wan_mode` ∈ `auto`（默认，盲插）/`manual`；`wan_port`（手动时指定，如 `lan1`）；
2. 新增服务 `e2631-wanport`：
   - **探测**：对候选口（不在 `br-lan` 之外的口都算候选）逐个跑一次 DHCP DISCOVER 试探，
     应答的口即上行口；等价于原厂的 `NetAdaptStartEthProbe`；
   - **落地**：把上行口从 `br-lan` 摘出、`wan` 接口改绑到该 netdev；
     其余口（含原 `wan` 物理口）全部回 `br-lan`；`ifup wan`；
   - **跟随**：监听 hotplug `iface`/`phy` 事件，换线、拔线时重跑；
   - **防抖**：同一结果不重复动配置；探测失败时保持现状（不把用户断网）；
3. 后续阶段（独立提交）：LuCI 页面（对应原厂那张端口图）、双 WAN。

未解的疑问（实施时按需再挖，不阻塞）：

- 原厂探测具体打什么包（DHCP DISCOVER / PPPoE PADI / ARP 谁先）——`cspd` 里
  `NetAdaptStartEthProbe` 附近没有明文协议名，只能看到 DHCP 客户端跟着上行口切换；
- 原厂 `auto` 模式下 `banport` 的来源（面板上禁用某些口）——大概率与 `WanPortVlan*`/IPTV 绑定有关；
- `switchtst -sdwan` 的 `WANIDX` 语义（双 WAN 顺序还是优先级），暂不需要。

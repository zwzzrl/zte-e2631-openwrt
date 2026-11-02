---
title: wanport.md
date: 2026-11-02
status: done
topic: ops-net
summary: WAN 口可手动改绑（lan1/lan2/lan3/wan 四选一）；自动探测已砍，因为探不到 PPPoE/静态上行且能把自己关在门外。
evidence: overlay/target/linux/sanechips/base-files/sbin/e2631-wanport, scripts/selftest-wanport.sh
supersedes:
---

# WAN 口自定义（`e2631-wanport`）

**判定**：这颗板子 4 个物理口在驱动里是 **4 个独立 netdev**（`lan1 lan2 lan3 wan`），
所以"换 WAN 口"在 OpenWrt 里本来只是 uci 的事，不需要动交换芯片。本仓给了一个手动版：
`/sbin/e2631-wanport apply <port>` 把 `wan` 接口绑到指定口、把该口从 `br-lan` 摘出来、
其余口放回桥。**自动探测（盲插）刻意不做**，理由是 2026-10-27 真机上的两条否定结论。

---

## 1. 它保证什么

- 口名白名单写死在脚本里：`lan1 lan2 lan3 wan`。配置里写错一个字符串就可能在
  "不存在的口"上操作，而这一步的代价是断网，所以不做动态解析。
- **WAN MAC 不随物理口变**：始终用 DTS 给 `wan` netdev 的出厂地址（`macaddr_tag_1e` base+1；
  LAN 口都是 base+0）。上游看到的 MAC 不变，租约/DUID/上游 MAC 绑定都能续。
- 换走的口把 MAC 还回去，**只还原那一个口**。不能"遍历四个口，谁带 WAN MAC 就改谁"：
  WAN 换到 `lan2` 之后，空着的 `wan` 口活动地址已是 LAN 基址，遍历版会把它覆盖成 LAN 基址，
  等于把这块板子唯一的 WAN MAC 弄丢（真机上丢过一次）。
- **`wan6` 是 `@wan` 别名时一个字都不写**：别名本来跟着 `wan` 走，写成具体设备名就把别名
  换成固定设备，之后换口时 `wan6` 落在不存在的 netdev 上——IPv6 静默哑掉、没有任何日志。
- 取出厂地址的顺序：`e2631_ports.wan_mac` 固化值 → `ip -o link show dev wan` 里的
  `permaddr` → `wan` 口当前活动地址。注意 `ip` 的脾气：**只有活动地址与出厂地址不同时**
  才会出现 `permaddr` 字段；取不到它不代表出错（说明活动地址就是出厂地址）。

## 2. 怎么用

```sh
/sbin/e2631-wanport status        # 当前绑定 / 网桥成员 / 四口链路与 MAC
DRY_RUN=1 /sbin/e2631-wanport apply lan2   # 先看要改哪些 uci 键
/sbin/e2631-wanport apply lan2    # 真正落地（会 network reload）
/sbin/e2631-wanport selftest      # 不碰网络的自检
```

LuCI：`网络 → WAN 口`（`luci-app-e2631-wanport`），保存后页面先写 uci、再让服务做一次
`apply`；`set_port` 只负责拉起服务就返回（`apply` 会 reload network，同步等待会把 ubus
请求自己掐断），页面每 5 s 轮询状态。

**验证时不要切到管理侧所在的口**：切过去会立刻失联（WAN 区 `input=REJECT`，SSH/HTTP 全拒，
只剩 ping）。真机验证目标口先选**没插线**的口；`scripts/selftest-wanport.sh` 用假
`uci`/`ip`/`ubus` + 假 `/sys` 跑 14 个场景，不需要路由器。

## 3. 为什么不做自动探测（两条真机结论）

第一版是"候选口各发一次 DHCP DISCOVER，谁应答谁当 WAN"，逻辑自检全过，真机上暴露：

1. **探不到真实上行**：PPPoE、静态地址、对端不开 DHCP 的上行都不应答。用户看到的是
   "插上了但它不认"，比手动更容易误判。
2. **自动切换能把自己关在门外**：验证时管理侧网线正好插在 `lan2`，`apply lan2` 之后
   LAN 侧整个消失、WAN 区拒绝 SSH/HTTP；恢复只能到现场换网线。`detect` 的兜底也帮不上忙：
   它第一步"现用 WAN 在 netifd 眼里 up 就什么都不做"，第二步探测又跳过现用 WAN 口。

## 4. 与原厂能力的差距（有意的）

| 原厂（cspd NetAdapt） | 这里 |
|---|---|
| `SetWanLanMode` ioctl 改端口角色 | `uci set network.wan.device=<口>`（+ `wan6`） |
| `NetAdaptSetBrPermitPort` | 从 `br-lan` 的 `ports` 里摘掉/放回 |
| 上行探测、热插拔钩子、开机探测 | **不做**（见 §3） |
| 双 WAN（WAN1/WAN2）、面板级禁用某口 | **不做**：单上行；四个口一律可选 |

原厂那三层的逆向细节（界面、`OBJ_ETHPORT_INFO_ID`、`NetAdapt` 符号、`switchtst -swanlanmode`）
留在 [wanlan-adaptive.md](../reverse/wanlan-adaptive.md)。

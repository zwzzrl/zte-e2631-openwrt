---
title: recovery.md
date: 2026-11-02
status: done
topic: flash-boot
summary: 回不来时怎么救：先分清 CRC 层还是内核层，再按"擦版本头换槽 → 原厂 WebUI 重刷 → U-Boot TFTP"三级阶梯。
evidence: docs/operations/flashing.md, docs/known-issues.md
supersedes:
---

# 起不来怎么救（recovery）

**判定**：这台机器"回不来"只有两种性质，处置完全不同 —— **CRC 层起不来**（槽头/内核不可引导，
链路灯亮但无网络）与**内核起来后挂死**（有串口输出但停住）。前者靠"擦掉版本头让另一个槽接管"，
后者靠"换槽 + 重刷"。**没有串口时不要在开机路径上做实验**，因为选中的槽起不来不会自动换槽。

---

## 1. 先判断是哪一层

| 现象 | 性质 | 下一步 |
|---|---|---|
| 链路灯正常（1000M/Full），裸 ARP 无应答，断电重启无效 | 槽头/内核层（很可能卡在 `bootm`） | §2 |
| 串口有 U-Boot 输出、`Starting kernel ...` 之后就没了 | 内核层（内核跑起来挂死） | §2 或 §3 |
| 串口完全没有输出 | 更早的层（bootloader/供电/串口本身） | 先核对串口接线与波特率（115200 8N1），再查供电 |

**判活只能用管理面证据**（串口 `${...}` 输出、`/proc/uptime`、SSH 会话）。用 v4 ping/吞吐判活会
被判成"死机"，因为 v4 走硬件 offload、软件计数不涨（见
[troubleshooting.md](troubleshooting.md) M1）。

## 2. 换槽（最常用的一招）

原厂 cspboot 每 128 KiB 扫整片 flash 找内层魔数、只校验 CRC1，然后按 `serial` 选槽：
`slotA.serial < slotB.serial` 或 `slotA.serial == 0` ⇒ 起槽 B，否则起槽 A（相等起槽 A）。
**所以只要让当前槽的头失效，它就会去起另一个槽。**

```text
进 U-Boot boot mode：上电后看到 "Press 1 means entering boot mode" 时回 1 → 输口令
  → 在 autoboot 倒计时里按空格 → 拿到 "=>"
=> nand erase <目标槽版本头所在块> 0x20000
=> reset
```

- 擦的必须是**版本头那一个 128 KiB 块**（槽内 `+0x1560000` 附近），不是整个槽；
  该块除 512 B 头之外都是 0xFF，擦掉不损失有效数据。
- 槽几何与版本头位置随固件代变化（6B5 与 7B5/8B8 不同），**先按
  [flash-layout.md](../reference/flash-layout.md) §3 算出当前代的偏移再擦**。
- 真实操作记录（含偏移推导与串口实录）见 [flashing.md](flashing.md) §3.1。

## 3. 重刷（换槽成功之后）

1. 让盒子从**另一个**槽起来（原厂固件或 OpenWrt 都行），并确认 WebUI / SSH 可达。
2. 用**原厂 httpd 的升级接口**上传要恢复的包 —— 它**只写非活动槽**，所以"要写哪个槽，就先让
   另一个槽当活动槽"。压缩/命名规则见 [package-format.md](../reference/package-format.md)。
3. 刷完 `reset`，cspboot 会因为新写的槽 `serial` 更大而选中它。
4. 若要把槽 0 恢复成纯出厂内容（包括"两个槽都乱过"的情况），按 [flashing.md](flashing.md) §3.2
   造一个纯出厂包再上传。

**刷之前必须核对的四件事**（照做，不要凭记忆）：

1. `e2631-fwslot info` 看清 `[running]` 是哪个槽、serial 是多少；
2. 目标槽 = 非活动槽（活动槽只在"另一个槽已确认可引导"时才动）；
3. `mtd5`（defcfg）**任何情况下都不写**，基线 sha256
   `7f849c43e2776dd199e3ffb786abef4174ed3d57c21f7c9b04a6499d11d17b92`，每轮复核；
4. 串口在手（或至少能进 boot mode），否则不做开机路径上的改动。

## 4. 最后手段：U-Boot TFTP

如果两个槽都起不来，只能在 U-Boot 里用 `tftp` 拉一个可引导镜像到内存再 `bootm`，
或用 `nand write` 直接写槽（同样要**先擦后写**：NAND 只能按位与，不擦就写会得到
serial 不变大、CRC2 报废的坏头 —— 原厂 `fw_flashing -v switch` 正是栽在这上面）。

TFTP 服务端要求：与盒子在同一 `/24`，跑 `tftpd` 并把镜像放到根目录；`host` 侧防火墙放行
69/udp 与临时数据端口（本仓踩过"服务在跑但一包都没到"的坑，见
[troubleshooting.md](troubleshooting.md)）。

## 5. 事后的两件必做

1. 把这次的判定过程写进 [troubleshooting.md](troubleshooting.md)（现象 → 根因 → 判据三行就够）；
2. 复核 `mtd5` 基线 sha256，并记录当前 `serial` 与活动槽，避免下一次又靠猜。

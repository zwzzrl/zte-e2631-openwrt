---
title: 2026-10-12-bringup-research.md
date: 2026-10-12
status: historical
topic: hw-datapath
summary: 开工前那批"原厂硬件视图/跨板差异/外围驱动"调研的幸存事实：256 MiB、4 口 1WAN+3LAN、NAND 型号、内存是唯一硬阻塞。
evidence: docs/architecture.md, docs/reference/flash-layout.md
supersedes:
---

# 2026-10-12 开工调研：硬件视图、跨板差异、外围就位

> **历史文件（status: historical）**：这是开工前一天的调研结论汇总，当时还没有本仓的
> overlay。已被实测取代的条目**直接标了"已被取代"**，不要当现状引用。
> 现行口径见 [architecture.md](../architecture.md) 与 [v1.3-progress.md](../v1.3-progress.md)。

## 1. 硬件视图（原厂 `/sys/firmware/fdt` + `/proc/config.gz` 只读取回）

| 项 | 事实 | 判据 |
|---|---|---|
| 内存 | **256 MiB**（`0x40000000`–`0x4FFFFFFF`） | `/proc/device-tree/memory/reg` + `chosen/bootargs` 的 `mem=256M` + `/proc/iomem` 三处一致 |
| CPU | ZX279128S，2×Cortex-A9 1 GHz | U-Boot 打印 `CPU : ZX279128S@A9,1000MHZ` |
| 网口 | **1 WAN + 3 LAN（4 个千兆口）**，不是 5 口 | 原厂 `br0` 成员 = `eth1 eth2 eth3 nbif0 wlan*`；`eth0` 不在桥内且是唯一 PROMISC+UP 的上联口 |
| 启动槽 | 由 U-Boot 经 cmdline 传下来：`… U-Boot V1.0.0 20250325110003 0x700000 0x0 0xaf 0xa3`，**第 4 段 = 启动槽基址** | 真机 `/proc/cmdline` |
| PCIe 复位 | 厂商设备树里**没有** GPIO 复位：pcie 节点只有 `compatible` 与 `interrupts` | `/sys/firmware/fdt` 反解 |
| reserved-memory | **不需要**：真机树的 reservations 段是空的 | `/sys/firmware/fdt` |
| 温度传感器 | 存在（`0x94100034`，crm2 base+0x34，中断 51/90/91），但 compatible 是厂商私有 `zxic,tempsensor`，上游无驱动 | 设备树 |
| USB | 厂商树里两个控制器（`dwc2@0x09000000`、`dwc3@0x09100000`）都是 `okay`；但**碰它会挂**，OpenWrt 侧三处全关 | 厂商 config 里 `CONFIG_USB_SUPPORT=y` 但 `# CONFIG_USB is not set` |

两条容易踩的读法：

- `memory@40000000` 节点里仍写着 `size 0x20000000`（512 MiB），这是**内核没修正的残留**；
  内核按 `mem=` 重写了 `memory` 节点，另一个节点没人动。别拿它当证据，也别把"两个 memory
  节点自相矛盾"当成待解之谜。
- 板级 DTS 照抄 H3600 的 `lan1..lan4 + wan`（5 个 gmac_port）是**错的**——这块板子 4 个口。

## 2. 跨板差异：H3600 / SR1010 vs E2631

**完全一致、可以直接用**（SoC 级，与板卡无关）：UART `0x94404000`（periphid `0x001feffe`，
115200 8N1）、GPIO `0x94407000 + 0x40*n`（n=0..5，每组 16 线）、SPI-FC `0x94406000`、
GMAC `0x92000000`（0x400000）、MDIO `0x9a101000`、PCIe0 DBI `0x0f000000`/ctrl `0x09500000`、
PCIe1 DBI `0x0f100000`/ctrl `0x09600000`、CRM（`0x94000000`/`0x94100000`/`0x94400000`/
`0x9a100000`）、pinmux `0x94200000`、L2C-310/GIC/SCU、NAND 介质（128 MB SPI NAND 同系列）。

**唯一的硬阻塞是内存**：H3600 = 512 MB（`reg = <0x40000000 0x20000000>`，PP 保留区
`0x5da00000` 38 MB，initramfs 入口 `0x48008000`），E2631 = 256 MB。直接刷 H3600 的包必崩，
三条原因都在"DTB 声明了不存在的物理内存 + 保留区越界 + initramfs 入口在不存在的中段"。

**NAND 型号**：E2631 = `HYF1GQ4UTACAE`（manuid `0x01`、chipid `0x15`，128 MiB 3.3 V），
H3600 = `HYF1GQ4UDACAE`（`0xc9`/`0x21`）——同系列同容量，都走 `spifc@94406000`，
所以 NAND **不是**阻塞项；但坏块标志策略不同，要留意。

**S 还是 R**：拆机图（C 级）读到 `ZX279128R`，U-Boot（A 级）自报 `ZX279128S`；官方说法是
同 die 同封装，S 版引出 PON SerDes。对移植无实质影响，外设基址/时钟/PHY 一律按 S 处理。

## 3. 外围分三层（"驱动起来"这件事里没有一项要写驱动）

| 层 | 内容 | 要做的事 |
|---|---|---|
| 1 | SoC 级、驱动内建、板级零依赖的 12 项 | DTS 写 `status = "okay"` |
| 2 | SoC 级、驱动内建、缺板级信息的 4 项（网口/PCIe 等） | 真机实测把数拿回来 |
| 3 | 模块或上游没有的：LED/按键可用，其余不驱动（USB、厂商私有温度传感器等） | 明确"不驱动"并写下来 |

**已被取代的条目**：当时按"外围就位"估计的工作量、成功率、开工路线都被实际执行取代；
`H3600 镜像体积与 E2631 原厂 supgrade 刷入可行性` 的跨板推断已被本仓打包器
（[package-format.md](../reference/package-format.md)）和三次真机刷入取代。

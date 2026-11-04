---
title: 只读寄存器窗口 debugfs/reg_ro：协议、基址口径与安全闸
date: 2026-11-04
status: done
topic: accel-impl
summary: 驱动新增 debugfs 节点 reg_ro，用已映射的 reg base 只读 dump 任意 4 字节对齐的寄存器窗口（限制 0x300000-0x3FFFFF、单次 ≤256 B）；写侧只选范围，不写寄存器。
evidence: 工作文件/08-源码与构建树/zte-e2631-openwrt/overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c
supersedes:
---

# 只读寄存器窗口 `debugfs/reg_ro`

> 读者：要在盒子上读 TM/DMA 寄存器现值、而 `/dev/mem` 又读不了的人。
> 节点路径：`/sys/kernel/debug/92000000.ethernet/reg_ro`。

## §0 为什么要它

`/dev/mem` 在这颗 armv7 上读 `0x92xxxxxx` 返回 `EFAULT`（`dd: /dev/mem: Bad address`）：
内核的 `read_mem()` 走 `__va()`，超过直接映射就拒绝，`CONFIG_STRICT_DEVMEM` 设不设都一样。
寄存器窗口本来就在驱动手里：probe 用 `devm_platform_ioremap_resource(pdev, 0)`
映射了 DTS 的 `reg = <0x92000000 0x400000>`（base 树
`patches-6.18/026-ARM-dts-zte-Add-ZX279128S-SoC-description.patch:220-222`），
所以这个节点直接 `readl(adapter->base + 偏移)`，不另做 `ioremap`。

## §1 协议（写侧只选范围，不是寄存器写）

```sh
D=/sys/kernel/debug/92000000.ethernet

echo 0x354000 > $D/reg_ro          # 从偏移 0x354000 读 64 字节（默认）
echo "0x354000 0x40" > $D/reg_ro   # 偏移 + 字节数；两个数都接受 0x/十进制
cat $D/reg_ro
```

读回来每行一个 32 位字：

```
reg_ro: off=0x354000 len=64 (readl only)
0x354000: 00000000
0x354004: 00000000
...
```

写侧只把 `(偏移, 长度)` 存进两个 `static u32`（文件级，**不是**
`struct zx_eth_adapter` 的字段——那个结构体的 `sizeof` 被 `BUILD_BUG_ON`
钉死为 `0x59c8`，是 ppe.ko 的 ABI 铁律）；读侧每次 `cat` 现场 `readl`。
选择状态在下次写入前一直有效，两次 `cat` 之间不用重写。

## §2 偏移口径（0x354000 是相对谁的）

**相对驱动 reg base `0x92000000`**，也就是整个 gmac 的 4 MiB 窗口。
`zx279128s-eth.h:35-39` 原文："Register offsets from the start of the block
(0x92000000)"；`ZX_TM_BASE = 0x340000`（`eth.h:90`）本身也只是这个窗口里的一个
偏移。因此：

| 写法 | 读到的物理地址 |
|---|---|
| `echo 0x354000` | `0x92354000` |
| `echo 0x340000` | `0x92340000`（TM 控制块起点） |
| `echo 0x350000` | `0x92350000`（DMA 块起点） |

如果手上的寄存器号是**相对 TM 基址**给的（如 `0x14000`），先加 `0x340000`
再写进来；如果它相对的是 `0x92000000` 之外的另一个窗口（例如超过
`0x3FFFFF`），本节点读不到——那需要驱动另外映射，别硬凑。

## §3 安全闸（硬要求，不满足一律 `-EINVAL` + 一行 `dev_info`）

1. 偏移 ∈ `[0x300000, 0x3FFFFF]`，且 4 字节对齐；
2. 字节数 4..256（一条命令最多 64 个字），且是 4 的倍数；
3. 本路径**只** `readl()`，没有任何写寄存器的调用；不开 module_param。

`reg_ro` 的权限是 `0644`（owner 可写=可选范围），不是 `0444`，否则没法选范围。
现有节点（`dma_state`/`diag`/`alias_dump`/`zc`/`zc_ev`/`txtest`/`accel_*`/
`ffe_*`/`soc_temp`）行为不变。

## §4 典型用法：TM/SCH 整形块现值

```sh
echo "0x354000 0x40" > $D/reg_ro
cat $D/reg_ro
# 0x354000 bit0 que_sharp / bit1 dwrr / bit4 tcont_sharp / bit6 secsch_dwrr
# 0x354024[5:0] spend_byte（原厂=20）
# 0x354028[17:0] fill_time（原厂=6249）
```

对照原厂值之前先确认读的是同一颗 dword：偏移按 §2 的口径换算，别把
`0x340000` 加两次。

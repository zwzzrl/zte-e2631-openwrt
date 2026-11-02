---
title: package-format.md
date: 2026-11-02
status: done
topic: build-image
summary: 原厂升级包 = 16B 魔数 + 4B hdr_ofs + 512B verhdr + body；CRC1 覆盖 [0:0xa4]，CRC2 覆盖 [0:0x1fc]。
evidence: docs/history/2026-10-13-intel.md, docs/reference/flash-layout.md
supersedes:
---

# 原厂升级包格式与打包规则

**判定**：原厂 `supgrade` 包是"**16 B 外层魔数 + 4 B `hdr_ofs` + 512 B 版本头（verhdr）+ body**"的
裸容器，body 按槽几何整段写入目标槽；刷写器**只按头行事**（校验两段 extent 的 size/offset/CRC），
不看内容语义。三个官方包（6B5 / 7B5 / 8B8）与两片活槽头逐字节验算通过。

---

## 1. 容器布局

```
0x000  16 B   外层魔数 99999999 44444444 55555555 aaaaaaaa
0x010   4 B   hdr_ofs = 0（= 未签名包；原厂 VerifySign 见到 0 就跳过）
0x014 512 B   版本头 verhdr          ← 本文所有 verhdr 偏移都"相对 verhdr 起点"
0x214         body 起点，整段镜像
              body+0x000000  32 B 槽标记 33333333 cccccccc 88888888 dddddddd + 16×FF
              body+0x000020  kernel extent（明文 legacy uImage，load=entry=0x40008000，comp=0）
              body+0x000214  … 到 0x340000 为 0x00 填充（6B5/7B5/8B8 全零）
              body+0x340000  rootfs extent（AES-128-ECB 密文，见下）
```

## 2. verhdr 关键字段（**文件偏移 = 相对偏移 + 0x14**）

| 相对偏移 | 含义 | 7B5 实测 |
|---|---|---|
| `+0x08` / `+0x0c` | `upgradeKey1` / `Key2` | `0x1351` / `0x5211` |
| `+0x10` | 版本串 32 B | `V1.0.0.7B5.8000` |
| `+0x30` | body 总大小（`package_load()` 会检查它等于 body 长度） | `0x1560000` |
| `+0x34` / `+0x38` / `+0x3c` | kernel size / **包内绝对文件偏移** / CRC32 | `0x33FDCC` / `0x234` / `0x9EF7C246` |
| `+0x40` / `+0x44` / `+0x48` | rootfs size / 文件偏移 / CRC32（**对存放的字节算**） | `0x1220000` / `0x340214` / `0x3DF85EE2` |
| `+0x4c` / `+0x5c` | 槽 A 描述 `{base, span, 0, 0x1000000}` | `0x700000, 0x1C00000` |
| `+0x58` / `+0x68` | 槽 B 描述 `{base, span, 0, 0x1000000}` | `0x2300000, 0x1C00000` |
| `+0x6c` | 产品串 52 B | `ZXHN E2631 V1.0.0` |
| `+0xa4` | **CRC1 = CRC32(verhdr[0x00:0xa4])** | 三包 + 两片活槽头 MATCH |
| `+0xa8` | 构建时间串 16 B | `20250925221124` |
| `+0xb8` | 原厂 switch 要求为 0（"version integrality"） | 0 |
| `+0xc4` / `+0xc8` | flash 容量 / 块对齐 | `0x08000000` / `0x10000` |
| `+0xf4` | 内层魔数 `333333336666666699999999cccccccc` | — |
| `+0x110` | 硬件版本串 | 7B5=`V1.0.0`，8B8=`V1.1.1` |
| `+0x1e0`…`+0x1ec` | 槽表 `{A 起点, B 起点, B 起点, B 终点}` | `0x700000,0x2300000,0x2300000,0x3F00000` |
| `+0x1f0` | **头部所在槽基址**（包里写 0，刷写时盖章） | 槽 A=`0x700000`，槽 B=`0x2300000` |
| `+0x1f4` | **serial**（包里恒为 1，刷写时写 `maxversionum+1`） | 活机两槽都 = `0x10` |
| `+0x1f8` | 新版本标志（刷后 = 1） | 1 |
| `+0x1fc` | **CRC2 = CRC32(verhdr[0x00:0x1fc])**（覆盖 serial） | MATCH |

**决定性一条**：把官方包的 verhdr 与真机 flash 里的槽头逐字节比对，512 B 中**只有
`+0x1f0` / `+0x1f4` / `+0x1f8` / `+0x1fc` 四处不同**，其余（含 CRC1）完全一致。

## 3. 密文归属：kernel 明文、rootfs 密文

- **kernel extent 是明文 legacy uImage**：三包首 4 字节都是 `27 05 19 56`；真机在
  `0x700000+0x20` 与 `0x2300000+0x20` 都读出明文头。
- **rootfs extent 是 AES-128-ECB 密文**（key `E263111559d0dfde`），且 rootfs 的 CRC
  **对密文字节算**。
- 解密发生在**原厂内核按分区的软件读路径**里，不是控制器、也不是 U-Boot：同一器件两个视图 ——
  `/dev/mtdblockN` 给明文 JFFS2，`/dev/mtdblock0 + 0xA40000` 给密文，两者不是同一个东西。
- ⇒ **OpenWrt 侧一律写明文**（kernel 本来就明文，rootfs 也写明文并**对明文算 CRC**），
  不需要也不应该移植任何 AES 层。详见 [flash-layout.md](flash-layout.md) §5。

## 4. 选槽规则（刷完谁先起）

- 当前槽由 U-Boot 经 bootargs 传下来：`/proc/cmdline` 尾部 `... 0x700000 0x0 0xaf 0xa3`，
  **第 4 段 = 当前槽基址**。
- `/proc/zte/verinfo/versionstates` 给 `cur/back` 的 headeraddr、serial、isbad、
  `maxversionum`、槽表、`curUpgradeKey1/2`；它是**开机快照**，写完 flash 不会更新。
- cspboot 选槽：每 128 KiB 扫整片找内层魔数，**只校验 CRC1**；`slot0.serial < slot1.serial`
  或 `slot0.serial == 0` ⇒ 起槽 B，否则起槽 A（**相等起槽 A**）；**起不来不会自动换槽**。
- 所以任何自测都必须留退路：改的是**非活动槽**，或改动默认不生效（pending 标记）。

## 5. 打包器必须满足的判据

1. 外层魔数 16 B 与 `hdr_ofs=0` 正确；verhdr 512 B 落在 `0x14`。
2. `+0x30` body 大小 == 真实 body 长度（原厂四个包同时还等于 `ROUND_UP(len, 0x20000)`，
   在这四个包上两种读法不可区分 —— 别拿"两者恰好相等"当规范）。
3. kernel/rootfs 的 `size`、`offset`、`CRC32` 三者自洽；offset 是**包内绝对偏移**
   （即 `0x214 + body 内偏移`）。
4. CRC1 覆盖 `[0x00,0xa4)`、CRC2 覆盖 `[0x00,0x1fc)`，serial 改动只影响 CRC2。
5. 文件后缀必须是 `.bin`（`platform_check_image` 按后缀 + 头部魔数双重校验，见
   [troubleshooting.md](../operations/troubleshooting.md) D2）。

## 6. 已知的原厂刷写器 bug（别照抄）

原厂 `fw_flashing -v switch` 切槽时**不擦除就地写**：NAND 只能按位与，结果是 serial 不变大、
CRC2 报废；它的错误判断还反了（`CspUserMtdWrite` 失败返回 1，调用方把 `>=0` 当成功），
写完也不回读。正确做法见 [recovery.md](../operations/recovery.md) §切槽。

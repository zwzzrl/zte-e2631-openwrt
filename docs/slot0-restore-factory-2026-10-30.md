# slot0 恢复成纯原厂镜像（2026-10-30 18:1x）

> 对象：ZXHN E2631（ZX279128S），串口 `/dev/ttyACM0`（ch343 网桥 127.0.0.1:5555），
> 原厂 U-Boot 口令 `5cE080@fyBD`。**本次是用户明确点头后写 slot0 的操作**；
> 之前"slot0 保持出厂"的前提已在 §5 作废，这里把它恢复回来。

## 1. 结论

| 槽 | 内容 | 版本头位置 | serial | 状态 |
|---|---|---|---|---|
| slot0（0x700000） | **10-27 出厂备份** `mtd5_slot0.bin`（body 0x1440000） | 0x1B40000（= slot0+0x1440000） | 0x34 = 52 | **活动槽，正在跑** |
| slot1（0x2300000） | 原厂 9900 + root 壳（append 镜像） | 0x3760000（= slot1+0x1460000） | 0x33 = 51 | 非活动，一个字节没动 |

- 盒子现在从 **slot0 起，跑纯原厂 `V1.0.0.8B8.8000`**（WebUI `SoftwareVer` 实测）。
- root 壳随之消失：2323 关闭（出厂 rootfs 里没有 `/sbin/shd`），管理只剩 WebUI
  `http://192.168.43.119:80`（extender/bridge 模式，IP 由上游 DHCP 给）。
- slot1 的 9900+壳 仍是完整的，随时可以切回去（见 §5）。

## 2. 为什么不能"就在 slot0 上刷"

原厂 httpd 的升级接口**只写非活动槽**（2026-10-30 已实测两次）。当时盒子正跑 slot0，
所以直接上传会落到 **slot1**，把 9900+壳 覆盖掉，slot0 反而一个字节不动。
要写 slot0，必须先让盒子**从 slot1 启动**。

## 3. 步骤（可重复）

### 3.1 让盒子从 slot1 起：只抹掉 slot0 的版本头

进 U-Boot boot mode：`Press 1 means entering boot mode` → 回 `1` → 口令 → 停 autoboot
时按空格 → `=>`。然后：

```
=> nand erase 0x1b60000 0x20000
Erasing at 0x1b60000 -- 100% complete.
OK
=> reset
```

- `0x1b60000` = slot0 + 0x1460000，是**当时** slot0 那版（append 几何）的版本头块；
  它正好从 slot0 的 fs 末尾开始（0xA40000 + 0x1120000），除了 512 B 头就是 0xFF，
  擦掉不损失任何有效数据。
- 擦完 cspboot 只找得到一个合法头（slot1 的），于是从 slot1 起。
- 脚本：`09-工具脚本/ser-erase-slot0-header.py`（由 `ser-flash-prep.py` 改常量而来：
  事件驱动、硬超时 240 s、擦除范围写死并断言在 slot0 窗口内）。

### 3.2 造一个纯出厂的 slot0 包

```sh
cd 工作文件/05-路由器操作脚本
python3 make_pkg_from_slot.py ../../03-固件备份/router-flash/mtd5_slot0.bin \
        /tmp/factory.pkg --slot 0 --serial 60 --hdr-ofs 0x1440000 --version V1.0.0.8B8.8000
python3 retitle_pkg.py /tmp/factory.pkg /tmp/e2631-factory-8000-slot0.pkg \
        --slot 0 --serial 60 --version V1.0.0.8B8.8000
```

成品包已存：`02-固件产物/vendor-root-shell/e2631-factory-8000-slot0.pkg`
（21234196 B，md5 `8d490fa0f75e6a2d591a8bc5c6f5f7a4`）。

校验（已做）：

- body 21233664 B，`md5 = 48ee91937584bac5fbdac38b9696a525` **== `mtd5_slot0.bin[:0x1440000]`**；
- 512 B 版本头与出厂头逐字节比，**只差 5 字节**：`+0x1f4` serial 与 `+0x1fc..ff` CRC2；
- 头里 `body=0x1440000 / fs_size=0x1100000 / fs_crc=0x311c578e`，与出厂头一致；
- 内魔数 `33 66 99 cc`、版本串 `V1.0.0.8B8.8000`、槽起点 `0x700000`、CRC1/CRC2 全 OK。

### 3.3 从 slot1 的原厂 WebUI 上传

```sh
ZTE_HOST=192.168.43.119 python3 vendor-web.py upload /tmp/e2631-factory-8000-slot0.pkg --yes
上传响应：HTTP 200  {"IF_ERRORID":0}
```

约 80 s（18:15:50 掉线 → 18:17:12 回来）后自重启。

## 4. 实测证据（串口，刷完那次启动）

```
zteboot_search_firmware:95 correct header has been found @1b40000    ← slot0 新头（出厂几何）
zteboot_search_firmware:95 correct header has been found @3760000    ← slot1 头还在
zteboot_update_bootpara:864 totalImgNum=2 validImgNum=2 bootWhichImg=0 runmode=3
pdt_getimgindex:230 pdt_getimg select = 0, serial0=0x34, serial1=0x33,
 Total found 2 firmwares(valid:2), start from ver 0 :
```

两条推论：

1. **头位置 = slot 起点 + body 长度**这条几何规则再次成立：包 body 0x1440000 →
   头落在 0x1B40000（出厂几何），而不是之前那版的 0x1B60000。
2. **原厂 httpd 写头时 serial = 两槽最大值 + 1**：刷前两槽都是 0x33，刷完 slot0 拿到
   `0x34`。`known-issues.md` 里"httpd 到底怎么算 serial 没有定案"那条，**这次可以定案了**。

## 5. 现在的状态与切回去的办法

- **当前**：slot0 = 出厂 8000（活动，serial 52）；slot1 = 9900+root 壳（非活动，serial 51）。
- **回到 9900 + root 壳**：从当前原厂 WebUI 再把
  `02-固件产物/vendor-root-shell/e2631-vendor-slot1-append-up.pkg` 传一次 ——
  它会写非活动槽 slot1、把 serial 封成 53 > 52，重启就落在 slot1。
- **刷 OpenWrt v12fix 进 slot1**：同理传
  `02-固件产物/e2631-openwrt-v12fix-ppe-20261030-slot1.bin`（写 slot1，serial 53），
  重启进 OpenWrt；slot0 的原厂 8000 保持不变，形成"出厂兜底 + 开发槽"的组合。
- 记住方向：**httpd 写的是"非活动槽"**。要写哪个槽，就先让另一个槽当活动槽。

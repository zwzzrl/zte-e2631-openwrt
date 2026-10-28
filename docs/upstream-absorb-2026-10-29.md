# 从上游 v1.8.0 吸收了什么（2026-10-29）

原则：**我们的分支为主**，不整包合并。上游 `origin/main`（`251ac0c`，v1.8.0）比我们多
45 个提交，但两边在驱动目录上是结构性分叉，直接 merge 会把机器编不过（细节见最后一节）。

## 已吸收

| 上游提交 | 内容 | 落在我们这边的哪里 |
|---|---|---|
| `a6e75bf` P2-1/WiFi | 频段档位：2.4G `HE40 + noscan`、5G `HE80`。不写 `htmode` 时 netifd 退回 HT20，2.4G 协商封顶 ~287 Mbit/s，永远上不了 ~500 | `base-files/etc/config/wireless`（radio0）+ `sbin/e2631-wifi-ap`（按 `$ofs` 分档） |
| `f6effe3` P2-1/PPE 的 **接口部分** | `apply` 目标口有链路时拒绝（`--force` 绕过）；worker 默认不 `network reload`（gmac 半挂死风险），只写 uci + 立刻改端口 MAC | `sbin/e2631-wanport` 新增 `--force` / `--reload`；LuCI 后端改为 `apply <port> --force`；策略与两边证据记在 `docs/known-issues.md` 的 `WANPORT-RELOAD-POLICY` |

## 看了但**不改**的

| 上游内容 | 为什么不吸收 |
|---|---|
| `zx279128s-ppe.c`（内建整引擎，含 IPv6 trap 修复） | 我们已把引擎拆成 `kmod-e2631-ppe` 包。而且那条 IPv6 trap 修复我们**早就有等价实现且更细**：`zx_update_traps()` 里 `v6 = zx_ppe_v6_offload`，默认 0 时 IPv6 ethertype trap 保持打开（IPv6 走 CPU），只有引擎真把 IPv6 表写进硬件才抬 trap（`zx_ppe_set_v6_offload`）。上游写成 `v6 = ppe_active \|\| !sw`，效果在默认路径上一致 |
| `zx279128s-ffe.c` + `zx-ffe-ops.h`（FFE 控制面） | 全新的功能面，与本轮目标（PPE 的 IPv6 真值、刷机自锁善后）无关，且需要内核改动 + 真机验证 |
| `slotpart` 取代 `slotfix` | 上游判定老 `slotfix` 在真机启动路径上从未命中（bootpara 块被 zImage 解压当页表用）。我们的镜像目前从 slot1 启动，不受影响；等换启动路径时再评估 |
| `e2631-upgrade`（三段式版本号、按数字排序） | 我们没有这个工具；发版命名规则以后要跟上，但不影响当前功能 |
| `README.md` / `docs/*` 的结构重写 | 我们的文档结构承载了更多本地证据（串口恢复、事故记录），整份取上游会丢东西 |

## 为什么不能直接 `git merge origin/main`

`files/drivers/net/ethernet/zte/` 这个目录在分叉点 `51d8026` **不存在**，两边各自新增：

- 我们：`zx279128s-main.c` + `zx279128s-ppe.h`（引擎在 kmod 包）
- 上游：`zx279128s-ppe.c`（整引擎）+ `zx279128s-ffe.c` + `zx-ffe-ops.h`

git 把"两边各加不同文件"当成无冲突全保留，于是合并结果会同时出现两套引擎，
`zx_update_traps` / `zx_update_mac_tables` 各被定义两次（我们 `main.c:514/545`，
上游 `ppe.c:599/627`），链接期直接失败。

git 那边另外还有 11 个文件的内容冲突，性质基本是"同一份代码的两种注释写法"
（上游 `1ee19c7` 就是把我们的手动 WAN 口实现原样采纳回去），真要合并时按
「文档/配置取上游、驱动取我方、`overlay-removed.txt` 取并集」处理即可。

## 待真机验证（本轮**没有**验证的部分）

这一轮全部改动都是在离线状态下落地的：板子刷入带 PPE 钩子的镜像后自锁，等 3.3V
USB-TTL 串口恢复，期间 WAN/无线都无法实测。下面这些**必须**在板子回来后补上，
补测之前不许在任何地方写成"已修好"：

1. **无线档位**：`iw dev` 读回 `radio0` 的 `HE40`、`radio1` 的 `HE80`；再看
   `iwinfo phy0-1` 的 `txpower` 与双方协商速率。判据是 2.4G PHY 能到 574
   （原厂 DBDC 档位值），而不是还卡在 ~287。
2. **noscan 的副作用**：`noscan=1` 是强制开 40MHz，2.4G 在拥挤环境里会更容易被
   邻道干扰打到掉速率。要对比"关/开 noscan"两轮打流结果，再决定是否保留。
3. **`e2631-wifi-ap` 分档**：跑一次改 SSID/改密码，确认 `htmode` 没被写回 HT20；
   `$ofs` 到频段的映射是用 phy 路径偏移推的（phy0=Band1=2.4G），需要 `iw phy`
   的实际输出确认。
4. **`--force` 防护**：拿管理网线那一口试 `e2631-wanport apply <管理口>`，应被
   拒绝并打印提示；加 `--force` 才切。
5. **默认不 reload**：`apply` 之后 SSH 不断、`uci show network.wan.device` 已变，
   但 `ip -4 addr show` 里 WAN 地址还是旧的，reboot 后才是新的。

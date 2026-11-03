---
title: docs/ 导航：这套文档怎么读
date: 2026-11-02
status: done
topic: tools
summary: 按"想干什么"分三入口；docs/ 下 5 个主题目录各放什么、常见问题去哪找、状态标签怎么读。
evidence: docs/architecture.md, docs/known-issues.md, docs/v1.3-progress.md
---

# docs/ 导航

**结论**：这套文档按"你要干什么"分三条入口——**想用这台机器**看根目录三篇；
**想改代码/改硬件路径**看 `reference/` 与 `features/`；**想查"当初为什么这么做、后来怎么被推翻"**
看 `reverse/` 与 `history/`。所有日期在文件名里的文档都是**历史**，不是现状。

## 1. 三条入口

| 你要干什么 | 从这里开始 | 说明 |
|---|---|---|
| 把这台机器用起来 | [v1.3-progress.md](v1.3-progress.md) | 当前版本做了什么、还差什么 |
| 它现在哪里坏着 / 哪些是已知坑 | [known-issues.md](known-issues.md) | 机器可读的问题块 + 每条一则"现象/根因/证据" |
| 它整体是怎么搭起来的 | [architecture.md](architecture.md) | 三个仓库、镜像几何、驱动结构、验证栈 |
| 要动驱动/寄存器/刷机 | [reference/](reference/) → [features/](features/) | 先查常量与表，再看功能线为什么这么写 |
| 要调 FFE 运行期参数（合并窗口/NAPI 权重/延迟剂量） | [ffe-tuning-parameters.md](reference/ffe-tuning-parameters.md) | 每个旋钮的作用点、实测影响、代价与未测项 |
| 要复现某个结论 | [reverse/](reverse/) | 每条都给测法与判据，不给"当时日志在哪" |
| 想知道某个结论是怎么被推翻的 | [history/](history/) | 带日期的文档，标 `historical` / `superseded` |

## 2. 五个目录各放什么

**`reference/` —— 可以照着核对的常量与表。**
放槽几何、分区落点、升级包格式、CLA 寄存器位定义、哈希算法、原厂调试面清单。
判断标准：**这里的东西抄错了，代码就会写错**；所以每条都要给偏移、宽度、来源类别。
[ffe-packet-path.md](reference/ffe-packet-path.md) 是例外中的"流程级"参考：它把 FFE 快路
从一个 RX 描述符到一次 TX 门铃的每一跳、每一处 miss/drop 都按 `文件:行` 列全。
[ffe-tuning-parameters.md](reference/ffe-tuning-parameters.md) 是"参数级"参考：每个旋钮的 sysfs/debugfs 路径、寄存器编码、实测影响、代价与未测项。

**`reverse/` —— 硬件逆向的结论与测法。**
放"原厂到底怎么做的、我们试到哪一步"。每条结论必须带：怎么测的、什么数算过、四分类标注
（本板实测 / 代码事实 / 原厂二进制 / 我方推断）。

**`features/` —— 我方实现的功能线。**
放"现在是什么样、为什么这样、边界在哪"。同一功能线的多轮实验合成一篇，不按日期切。

**`operations/` —— 手上要做的动作。**
刷机、换槽、救砖、典型踩坑速查。这里的每一节都应当能照着执行。

**`history/` —— 带日期的历史记录。**
文件名即时间线。`status: historical` = 过程记录、没有当前效力；
`status: superseded` = 结论已被明确推翻，头里有 `superseded_by` 指向接替者。
**引用历史文档的结论之前，先看它的 status。**

## 3. 常见问题去哪找

| 问题 | 去哪 |
|---|---|
| 刷完起不来 / 怎么退回原厂 | [recovery.md](operations/recovery.md) 与 [flashing.md](operations/flashing.md) |
| 某个数字（吞吐/延迟/计数）是多少、怎么量的 | `features/`（功能线）与 `reverse/`（原厂对照） |
| 这个寄存器位是什么意思 | [cla-registers.md](reference/cla-registers.md) |
| 一个包在 FFE 快路里走了哪几跳 / 为什么没走快路 | [ffe-packet-path.md](reference/ffe-packet-path.md) |
| 合并窗口 / NAPI 权重该设多少、怎么量 | [ffe-tuning-parameters.md](reference/ffe-tuning-parameters.md) |
| 升级包怎么组成、自己打的包为什么被拒 | [package-format.md](reference/package-format.md) |
| 换 WAN 口 / 为什么不做盲插 | [wanport.md](features/wanport.md) |
| 我这次踩的坑是不是老坑 | [troubleshooting.md](operations/troubleshooting.md) |
| 某个结论还成立吗 | 先看 `known-issues.md`，再看那篇文档头的 `status` |
| 我改了驱动，怎么验证没退化 | `architecture.md` §verification stack + `scripts/check.sh` |

## 4. 三条写作约定

1. **不写仓库外路径。** 证据要么在仓库里（`overlay/…`、`board/…`），要么改写成
   "结论 + 关键数字 + 测法 + 判据"四段；不写"日志在我机器上的某个目录"。
2. **结论先行。** 每篇正文前 25 行内必须出现结论；过程流水放 `history/`。
3. **推翻不删文。** 结论变了就标 `status: superseded` + 正文顶部横幅 + `superseded_by`，
   把"当初为什么想错"留在仓库里——这是后来人最省时间的东西。

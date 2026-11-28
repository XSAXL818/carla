# 工作日志（CARLA 测试平台）

> 倒序：最新在最上面。每条写清 **时间 / 问题 / 做了什么 / 结果或证据**。
> 由 PM 在关键节点追加；对应的代码提交见 `git log`，验收报告见 `acceptance/`。

---

### 09:14 · 等待第 17 轮验收
- **问题**：上一轮（第 16 轮）判 FAIL——闸门审计行把“离开入口流程的地图”写错（实际从 Town05 离开，却写成入口图）。
- **处理**：修复已提交 `c31607ef4`（新增 `EntryFlowLeftMapId`，日志改为 `<离开地图>, now at <当前图>`；同时修了启动脚本对点号地图路径的识别、预览失败后撤销预览预期）。
- **结果**：修复者按验收原始复现重跑，三行日志现在都写 Town05；正在跑只核三条的小范围验收。

### 08:40 · 第 16 轮验收：FAIL（一条）
- **问题**：入口流程三条路径、零 `LogCarla: Error`、启动脚本、采纳/按钮/芯片全部通过；唯一阻塞是**日志文案与事实不符**。
- **结果**：报告 `acceptance/16-2026-11-28T0039Z.md`，证据 `evidence-16/`（20 张截图 + 日志摘录）。

### 08:08 · 入口流程三项修复：交付 `6458a5525`
- **问题**：闸门单向记忆（PIE 一次入口图后到处弹面板）、入口图每次启动打 `LogCarla: Error`、启动脚本对非入口图必然误报失败。
- **处理**：闸门改为“按本次运行起始图判定 + 离开入口流程后不再重新武装”；给入口关卡补**零道路 OpenDrive**（`L_TestPlatformEntry.xodr`）而不是吞错误；脚本按地图是否在白名单选择就绪标志。
- **结果**：`-game` 启动 `LogCarla: Error` 归零；把 xodr 挪走立刻复现错误（因果关系成立）。

### 07:31 · 第 15 轮验收：FAIL（三条）
- **问题**：闸门 `bEntryMapArmed |= bOnEntryMap` 全文件无复位；入口关卡 Error；`run-editor-lowquality.ps1 -Map 非入口图` 永远等不到 UI 标记。
- **结果**：报告 `acceptance/15-2026-11-27T2325Z.md`。

### 06:56 · 入口关卡交付 `ec9ad949a`
- **做了什么**：`Content/TestPlatform/Maps/L_TestPlatformEntry.umap`（空壳 + PlayerStart）、面板白名单门槛（`TestPlatformSettings.EntryMaps`）、`-TestPlatformForceUI` 强制开关、`GameDefaultMap`/`EditorStartupMap` 指到入口关卡（`ServerDefaultMap` 未动）。
- **过程问题**：空关卡里 CARLA 生成不了 `Map`，后端首次预览直接 `state=error` → 修后端容忍“当前世界无地图”。
- **取舍**：面板按“本次运行起始图”判定，因为后端 `load_world` 在同一进程执行。

### 06:12 · 第 14 轮验收：PASS（看门狗定案）
- **结果**：活客户端暂停 75 s 零帧前进（仅 Warning）；带空闲客户端杀掉打 tick 的进程后 **301 s 按 ceiling 恢复**；`SyncCueCeiling=20` → 20.1 s 恢复；两阈值置 0 → 等满 125 s 不干预。

### 05:41 · 看门狗第三次修复 `128dfcc7b`
- **问题**：上一版“问 TCP 表”的存活判定，被任一空闲客户端解除武装 → 死客户端不再恢复。
- **处理**：加 `carla.SyncCueCeiling`（默认 300 s，无条件下限，0=永不），保留 grace 60 s（仅无连接时生效）。

### 05:17 · 第 13 轮验收：FAIL
- **问题**：任一 ESTABLISHED 连接即解除看门狗，编辑器冻结超过 78 s。

### 04:08 · 第 12 轮验收：FAIL
- **问题**：15 s 盲超时把**活客户端**踢出同步模式（世界偷跑 380 帧、`fixed_delta=None`、会话报错）。
- **教训**：这是我自己让它在引擎里动的那一行，连错两版才定案。

### 03:38 · 面板自动接管 + 启动按钮 `df4d52fc6`
- **问题**：面板自启后端后不认账（卡片仍写“配置后端未运行”、零 `/maps` 请求）。
- **处理**：2 s 轮询 `/health` + 3 s 重试加载目录；离线卡片加「启动后端」按钮，并暴露控制台命令 `TestPlatform.StartBackend` 以便无头验证。
- **结果**：实测后端 `/health` 首次 200 → 目录加载完成 **0.5–0.9 s**，无需点击。

### 02:42 · 第 10 轮验收：FAIL（可见窗口那次）
- **问题**：可见终端窗口本身全通过，但面板不接管自启的后端。

### 02:24 · 范围与方向调整（你定的）
- 测试**只对 Town10HD 负责**，其余 8 张地图不作要求 → “部分地图主车不动”降级为非验收项。
- **撤销 UMG 迁移**：界面迭代由 agent 承担，编译等待不计入人的时间；留在 Slate。
- **录制与导出暂缓**：代码保留，不作验证项。

### 02:00 · 后端改为可见终端窗口 `77a6c1ca6`
- **问题**：后端是隐藏进程，只能事后翻 `startup.log`。
- **处理**：改由 Windows Terminal 打开固定标题窗口（`CARLA TestPlatform Backend`），关窗即停、不自动重启、编辑器退出不杀后端；`wt` 不可用时降级普通控制台。

### 01:00 · CEF 清理 + 插件迁 Source（前半程）
- 删 `WebBrowserWidget`/`WebBrowser` 依赖与两个 HTML 原型；`Plugins/TestPlatform` → `Source/TestPlatform`（代码）+ `Platform/`（后端、工具、图、文档）。文件守恒核对：900 → 836，差额正好是有意删的 64 个。
- 后续补：`requirements.txt`、`architecture.md` 全面对齐代码、`CHANGELOG.md`。

### 00:00 · 起点
- 起点 commit `43871a079`；此后共 **29 个 commit**，全部在 `dev`，未推送。

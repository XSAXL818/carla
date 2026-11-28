# 工作日志（CARLA 测试平台）

> 倒序：最新在最上面。每条写清 **时间 / 问题 / 做了什么 / 结果或证据**。
> 由 PM 在关键节点追加；对应的代码提交见 `git log`，验收报告见 `acceptance/`。

---

### 16:30 · 主车真正按预设路线行驶（不再由 agent 自行规划）
- **问题**：`_build_agent()` 只调 `agent.set_destination(最后一个路点)`，而 `set_destination` 内部是从**主车当前位置**规划到**单点**，预设的中间路点全被丢掉。后果（上一轮实测）：主车出生即背向路线、为到达 94 m 外的终点跑了 494.6 m、画出来的预设折线与实际轨迹只有约 20% 重合；`cross` 预设还有 8 个路点落在 `shoulder`/`sidewalk` 车道上。
- **处理**：
  1. **计划由预设生成**（`session_runner._build_plan()`）：逐个解析预设路点→相邻两点同车道就沿车道 `next()` 走（几何本身），换路才用 `GlobalRoutePlanner.trace_route()`；`trace_route` 用 `map.get_waypoint()` 定位最近车道，而路口连接路在空间上重叠，152 个路点里 11 个会被定位到别的路导致绕行数百米，所以锚点先由 `_relocalize()` 沿目标车道挪到能正确解析的位置。`RoadOption` 从计划几何自身算（前后各 10 m 航向差、叉积定左右、阈值沿用 agent 库的 35°，且只在路口附近给 LEFT/RIGHT），因为 `trace_route` 的标签描述的是它自己那趟 A* 路径并带跨调用状态，拼接后的计划不是那条路径。
  2. **喂计划而非终点**：`_apply_plan()` 调 `set_global_plan(plan)`，`set_destination()` 彻底移出路径；`planner_fallback` 保留，但回退后的 `BasicAgent` 装的是**同一份计划**。
  3. **出生位姿来自计划**：主车直接放在计划第一个路点上、朝向计划首段（4 m 基线方位角），地图出生点只在计划起点被几何挡住时作退路且仍改成计划朝向。
  4. **车道问题在源头修**（`gen_route_presets.py`）：图改为**方向严格**——OpenDRIVE 的 link 是双向拓扑，不代表两向都能开，`drivable_dirs()` 只承认真有 `driving` 车道的那一侧，`prune_graph()` 删掉其余节点/边（546→231 条边）；路口连接方向从 `laneLink` 的 `from` 车道号推出（85 条里有 42 条是从入路末端驶出，旧代码一律当成从末端驶出，这正是绕行的根因）；`lane_at()` 不再退到“任意非零车道”；`LANE_ID` 哨兵从 `-1` 改成 `0`（`-1` 在本图是真实车道，同一个值既是“右车道”又是“任意车道”，是错向的根源之一）。重新生成 `data/routes/Town10HD.json`。
  5. **叠加线画计划**：`_cache_route_points()` 取 `_plan` 的坐标，画面与轨迹同一串点；新增 `GET /sessions/{id}/plan.json` 供外部对照。
- **结果**（Town10HD `loop`，真实 `-game` 会话）：计划 263 点 / 541.8 m，预设 24 个路点 0 未解析、0 非行驶车道、0 逆行；出生在计划起点 0.01 m 处、朝向偏差 **0.01°**；1075 个采样点 **100%** 落在计划 3 m 内（1 m 内 94%，修复前 20.4%）；实驶 514.8 m（计划 541.9 m）；`ego_arrived` 正常报出，无 `ego_stalled`、无 `planner_fallback`。
- **证据**：`acceptance/evidence-19/`（`measure-loop-final.json`、`samples.jsonl`、`shots/` 截图）。
- **顺带修**：`gen_route_presets.py --limit` 原来会用单图结果**覆盖** `index.json`，把另外 14 张图从索引里删掉（界面地图选择器因此只剩 1 张）；改为合并。

### 14:55 · DSH 桌面 profile 注册 CARLA 技能库 MCP（`carla`）
- **问题**：`Util/AgenticTools` 的 MCP 服务只在仓库自带 `.mcp.json` 里登记，DSH 桌面 profile 没接，助手查 CARLA 技能库只能逐个读 `Skills/` 下的文件。
- **处理**：在 `C:\Users\LoongTory\.dsh\profiles\desktop\cordis.patch.yml` 的 MCP 段插入一条 `@deepseek-ai/dsh-mcp-client` 行（`id: carla` / `serverName: "carla"` / `transport: stdio` / `command: node`），`args` 用**绝对路径** `C:/GameProject/CarlaUE5/carla/Util/AgenticTools/bin/carla-agentic-tools.js`（相对路径会按 Host 工作目录解析）；`toolCallTimeoutMs: 180000`，因为 `check_prerequisites` 内部给 `check_env.sh` 留了 120 s，默认 60 s 会截断正常调用。改前备份 `cordis.patch.yml.bak-2026-11-28T06-53-21-carla`（SHA256 一致），改后 PyYAML 解析通过；未重启 Host，HMR 热加载生效。
- **结果**：MCP 里 `mcp__carla__list_skills` / `read_skill` 实测可用（`read_skill` 返回 `Skill directory: C:\GameProject\CarlaUE5\carla\Skills\setup\check-carla-ports`，说明脚本按自身 `__dirname` 定位 `Skills/`，不依赖 cwd）。副作用一条：`get_config` 报告 `CARLA_ROOT=C:\GameProject\CarlaUE5\CarlaUE5` 已失效（该目录不存在，实际仓库在 `C:\GameProject\CarlaUE5\carla`），ue4/ue5/ue58 三组技能因此显示 `available: false`，需用 `set_config` 更正。

### 10:45 · Town10 三项缺陷修复：路线预设 / 背景车 / 停滞误报
- **问题**：① `loop` 预设 18 个航点里 8 个按 `(road, lane_id=-1, s)` 解析不出（735/469 只有 lane 1，不是路不存在），且路线折线从未画出来；② 背景车 `轻度 4` 实际 0 辆；③ 到终点停车 8 s 被判 `ego_stalled`。
- **处理**：`gen_route_presets.py` 读取 laneSection，按 s 取真实存在的行驶车道（右侧优先），重新生成 `data/routes/Town10HD.json`（schema 不变、长度/航点数不变）；`session_runner.py` 把 `draw_polyline`（本分支 DebugHelper 没有这个方法，AttributeError 被 `except: pass` 吞掉）改为逐段 `draw_line`（life 5 s、每 2 s 重画、失败记事件）；背景车的 1.5 km“净空泡”在 500 m 见方的 Town10 上把 155 个出生点全过滤掉，改为 25 m 净空 + 最远优先 + 25 m 间距，逐点记录失败原因并在 start 时把车交给同步 TM；停滞判定改为“到终点/无剩余计划不算、throttle>=0.05 才累计 8 s”。
- **结果**：130/130 航点直连解析通过；背景车 4/4 生成、独立 client 见 3 辆 29–30 km/h 行驶 + 1 辆等红灯；修复后运行到终点停车 60 s 无 `ego_stalled`，用 5 个路障封路仍能在 8 s 后报 `ego_stalled`（throttle 0.75）。里程 494.9/495.9 m，距最后航点 3.7 m。
- **证据**：`acceptance/evidence-town10/fix-*.png` + `fix-measurements.txt`。
- **遗留**：预设折线只保证“贴合地图”，主车仍由 agent 从出生点到终点自行规划，中途并不经过预设中间航点（architecture §14 已列为未验证项），两者只在终点附近重合。

### 09:56 · Town10 真机端到端：主车确实在跑，但暴露 6 个问题
- **做了什么**：起后端 + `-game` → 预览 Town10HD → `POST /start` → 采样 60 s + 独立 CARLA client 做地面真值对照 → 抓 6 帧 → 停止清理。
- **结果（好消息）**：主车真在跑。独立 client 测得 **439.6 m** 真实位移；后端指标首 60 s 345.7 m、全程 494.6 m，速度稳定 25–27 km/h；结束时停在**距规划终点 3.6 m**处。`tick`/`sim_time` 平稳推进，无 planner fallback。
- **问题（按严重度）**：
  1. **路线预设与地图 xodr 不匹配**：`loop` 预设 18 个航点里 **8 个解析不出来**（road 735/469 在 Town10HD_Opt 里不存在），画出来的折线是别处的锯齿，与主车实际轨迹无关；且**画面里看不到任何路线折线**。
  2. **`ego_stalled` 是误报**：主车按规划到达终点后 `throttle=0, brake=1` 停车，8 秒后被判“停滞”——判定只看 `speed<0.5`，不看“是否被要求前进/是否已到终点”。
  3. **面板运行时整块隐藏**（原 UX 设计如此），所以 `运行中`/速度/事件**全看不到**；且通过 HTTP stop 后**面板没有恢复**（bug）。
  4. **背景车 `轻度 4` 没生成**：`traffic=0`、独立 client 全程只有 1 辆车（画面里那两辆是地图静态道具）。
  5. 实测循环 **15.95 Hz**（请求 20 Hz），`sim_time` 以 0.80× 实时推进。
- **证据**：`acceptance/evidence-town10/`（6 张 1600×900）、`%TEMP%\town10\{series,truth}.jsonl`。

### 09:40 · 第 17 轮验收：PASS（入口流程三项定案）
- **问题**：上一轮只剩一条阻塞——闸门审计行写错“离开入口流程的地图”。
- **处理**：`c31607ef4` 用 `EntryFlowLeftMapId` 记住真实的离开地图，日志改为 `<离开地图>, now at <当前图>`；顺带修了启动脚本对点号路径的识别、预览失败即撤销预览预期。
- **结果**：验收自己重做三条——日志两条口径一致；点号 vs 短名分类矩阵 13 个取值全部正确；两种失败路径（请求失败、会话报错）都能撤销预期，正向路径零回归。报告 `acceptance/17-2026-11-28T0137Z.md`。
- **遗留（非阻塞）**：短名 `L_TestPlatformEntry` 仍未按入口图分类（文档却说可以）；`CHANGELOG.md` 缺一条；N2 有一个 ≤0.2 s 的竞态窗口。

### 09:30 · 侧边栏可勾选日志面板（过渡方案）
- **问题**：Markdown 日志看进展要翻对话，且无法勾选已读。
- **处理**：`Docs/worklog.html` 解析同目录 `WORKLOG.md`，提供复选框、未读高亮与计数、只看未读、全部已读、3 秒自动刷新，已读状态存 localStorage；`python -m http.server 8899 --directory Docs` 提供服务，侧边栏以 http 打开。
- **结果**：面板已可用；原生 DSH 插件版本改由另一个会话（插件预设）开发，本会话保留数据源格式稳定。

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

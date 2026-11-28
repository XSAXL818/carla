# CARLA 测试平台 · 架构方案（UE 客户端前端 + Python 后端）

> 状态：已落地；本文按 2026-11-27 的代码与目录校正。
> 本文件只写结构与契约，不含实现细节代码。

## 1. 角色划分

| 角色 | 进程 | 职责 | 不做什么 |
|---|---|---|---|
| **UE 客户端** `CarlaUnreal` | 编辑器或 `-game` 打包版 | 宿主 CARLA 世界（RPC `:2000`）、渲染、**配置界面**、运行中的速度/状态显示（面板内） | 不驱动 ego、不生成交通、不写指标 |
| **Python 后端** | `chat_carla` conda 环境 | 作为唯一 `carla.Client` 控制世界：加载地图、设天气、生成主车与交通、调用智驾算法、录制、采指标、导出、存会话记录 | 不渲染、不拥有窗口 |
| **CARLA 内置算法** | 后端内进程 | `BehaviorAgent` / `BasicAgent` 按路线行驶并输出控制量（`session_runner.py`） | 不直接碰 `world` |

**关键约束**：同步模式下 tick 的所有权必须唯一 —— 只有后端 `world.tick()`。UE 侧只渲染、只读状态。否则两边抢 tick 会互相卡死。

## 2. 数据流

```
                        ┌──────────────────────────────┐
   用户在 UE 面板操作 →  │  UE 客户端（前端 UI）          │
                        │  ├ 面板（Slate C++）        │
                        │  └ CARLA server（RPC :2000）   │ ←── world 在这里
                        └───────────┬──────────────────┘
                                    │ HTTP/JSON（UE 每 0.2 s 轮询）
                        ┌───────────┴──────────────────┐
                        │  Python 后端 FastAPI :8765    │
                        │  ├ session 状态（session_runner）│
                        │  ├ carla.Client ──────────────┼──→ 驱动 world：load_world /
                        │  │                             │    spawn / tick / recorder
                        │  ├ 内置算法（catalog.py 声明）  │
                        │  └ metrics / events 采集        │
                        └───────────┬──────────────────┘
                                    ↓
                      runs/<session_id>/  config.json route.json
                                          metrics.csv events.json rec.log summary.json
```

## 3. 通信协议

主通道 **HTTP + JSON**，默认 `http://127.0.0.1:8765`（仅本机绑定）。UE 侧每 0.2 s 轮询 `GET /sessions/current` 获取状态，不使用 WebSocket。后端监听地址/端口可用 `TESTPLATFORM_HOST` / `TESTPLATFORM_PORT` 覆盖，UE 侧的基址用 `-TestPlatformBackend=` 指向同一地址：该值接受 `http://host:port` 或裸 `host:port`，统一在 `FTestPlatformApi::RootUrl()` 里补齐 `http://`、去掉首尾空白与结尾 `/`（否则拼出的 `//sessions/current` 会被路由成 404）。UE 自动拉起后端时，会把解析出的 host/port 写进子进程的 `TESTPLATFORM_HOST` / `TESTPLATFORM_PORT`，两边因此落在同一端点。

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/` | 后端自述（名称、版本、`/docs` 地址、CARLA 连接状态） |
| GET | `/health` | 后端存活（含它实际绑定的 `backend.host` / `backend.port`）、`carla.Client` 连接状态、地图/路线目录 |
| GET | `/maps` | 界面可选地图 + 各图预设摘要（`ALLOWED_MAPS` 这 9 张各 4 条，别的图不是，见 §6） |
| GET | `/maps/{map}/routes` | 该图完整预设（`road_id/lane_id/s` 路点 + 长度） |
| GET | `/maps/{map}/preview.svg` | 俯视路网 SVG，可叠加预设路线（`preset` / `focus_x` / `focus_y` / `half` / `routes_only`） |
| GET | `/maps/{map}/thumb.png` | 地图卡片位图（Slate 不能画 SVG） |
| GET | `/vehicles` | 车辆蓝图列表（CARLA 在线时按实际蓝图过滤） |
| GET | `/vehicles/{blueprint}/thumb.png` | 车辆卡片位图 |
| GET | `/vehicles/{blueprint}/preview.png` | 占位接口，固定返回 501 |
| GET | `/vehicle_spin/{blueprint}/{NN}.png` | `data/vehicle_spin/` 的静态挂载：车辆卡片按 16 帧（`STestPlatformUI` 的 `SpinFrames`）逐帧取 `${基址}/vehicle_spin/<blueprint>/NN.png` |
| GET | `/weather` | 6 条天气预设 + 自定义参数 schema |
| GET | `/traffic` | 4 档交通预设 |
| GET | `/algorithms` | 算法 + **参数自描述 schema**（见 §5） |
| GET | `/config/schema` | 会话配置 JSON Schema |
| POST | `/sessions` | 提交配置并创建会话（`created`，不动世界） |
| GET | `/sessions` | 所有会话状态 + 当前会话 id |
| GET | `/sessions/current` | 当前会话状态；无会话时返回 `state=none` |
| GET | `/sessions/{id}` | 指定会话状态（阶段、tick、事件计数、相机） |
| POST | `/sessions/preview_map` | 关旧会话、异步加载地图，立即返回 `preview_loading` |
| POST | `/sessions/{id}/preview` | 同步加载地图并停好车，进入 `preview` |
| POST | `/sessions/{id}/start` | 从 `preview` 进入 `running`，起 recorder 与算法 |
| POST | `/sessions/{id}/stop` | 正常收尾、导出，进入 `stopped` |
| POST | `/sessions/{id}/destroy` | 清理演员并停会话 |
| POST | `/sessions/{id}/weather` | 运行中切换天气 |
| POST | `/sessions/{id}/route` | 运行中显示/隐藏路线、切换预设 |
| GET | `/sessions/{id}/summary` | 内联返回 `summary.json` 的内容（JSON 响应，不是文件下载） |
| GET | `/sessions/{id}/metrics.csv` | 下载 `metrics.csv` |
| GET | `/sessions/{id}/events.json` | 事件数组 + 计数 |
| GET | `/sessions/{id}/rec.log` | 下载 CARLA recorder 文件 |
| GET | `/sessions/{id}/presets` | 该会话地图的路线预设 |

## 4. 会话配置对象（后端落盘为 `config.json`）

```json
{
  "schema_version": 1,
  "map": "Town10HD",
  "ego": {
    "blueprint": "vehicle.tesla.model3",
    "spawn": { "mode": "route_start" }
  },
  "route": {
    "source": "preset",
    "preset_id": "loop",
    "waypoints": [
      { "road_id": 8,  "lane_id": -1, "s": 1.0 },
      { "road_id": 11, "lane_id": -1, "s": 14.82 }
    ]
  },
  "weather": {
    "preset": "ClearNoon",
    "custom": null
  },
  "traffic": {
    "preset": "light",
    "vehicles": 4,
    "walkers": 6,
    "autopilot": true,
    "seed": 20261125
  },
  "algorithm": {
    "id": "carla.behavior",
    "params": { "behavior": "normal", "target_speed_kmh": 50,
                "ignore_traffic_lights": false, "ignore_stop_signs": false,
                "ignore_vehicles": false }
  },
  "run": {
    "sync_mode": true,
    "fixed_delta_seconds": 0.05,
    "record": true,
    "export": { "metrics_csv": true, "events_json": true, "config_json": true },
    "max_duration_s": 120
  }
}
```

`route.source` ∈ `preset | custom`。`custom` 时 `waypoints` 至少 2 个点，由后端 `_resolve_route()` 校验；界面上的“自定义 road / lane / s”入口目前是禁用占位，尚未接入表格编辑或导入/导出。`POST /sessions` 只强制要求 `map`，其余字段由 `_payload()` 补默认值。

## 5. 算法参数 schema（后端自描述，前端只取其中一项）

`GET /algorithms` 返回算法列表与每个参数的字段元数据（类型 / 范围 / 单位 / 默认值）。界面**只**消费其中一项：`behavior` 的 `enum` 选项；其余参数在 C++ 里是写死的控件，没有按 `type` 分发的通用渲染逻辑，新增参数或算法都要改 `STestPlatformUI.cpp`：

```json
{
  "schema_version": 1,
  "algorithms": [
    {
      "id": "carla.behavior",
      "name": "BehaviorAgent（CARLA 内置）",
      "builtin": true,
      "desc": "CARLA 自带的规则型驾驶代理，按道路限速、红绿灯与障碍物决策。",
      "params": [
        { "key": "behavior", "label": "驾驶风格", "type": "enum", "group": "驾驶行为",
          "options": [{"value":"cautious","label":"稳健"},
                      {"value":"normal","label":"标准"},
                      {"value":"aggressive","label":"激进"}],
          "default": "normal" },
        { "key": "target_speed_kmh", "label": "目标速度", "type": "number", "group": "驾驶行为",
          "min": 10, "max": 130, "step": 5, "unit": "km/h", "default": 50 },
        { "key": "ignore_traffic_lights", "label": "忽略红绿灯", "type": "bool", "group": "规则", "default": false },
        { "key": "ignore_stop_signs", "label": "忽略停车标志", "type": "bool", "group": "规则", "default": false },
        { "key": "ignore_vehicles", "label": "忽略其他车辆", "type": "bool", "group": "规则", "default": false },
        { "key": "sampling_resolution", "label": "规划采样间隔", "type": "number",
          "group": "规划", "min": 1.0, "max": 10.0, "step": 0.5, "unit": "m", "default": 2.0 }
      ]
    }
  ]
}
```

界面实际怎么用这份 schema（`STestPlatformUI.cpp` 的“智驾算法”区块）：`behavior` 按 `enum` 的 `options` 生成三个风格 chip；`target_speed_kmh` 是写死的 10–130 km/h 滑块（步长 5）；`ignore_traffic_lights` / `ignore_vehicles` / `ignore_stop_signs` 是三个写死的复选框。`sampling_resolution` 只在 schema 里，界面没有控件，后端按默认 2.0 用。`catalog.py` 目前只产出 `enum` / `number` / `bool` 三种 `type`，没有 `string` / `secret` 参数，也就不存在对应的控件映射。

当前只注册 `carla.behavior` 与 `carla.basic` 两个内置算法，定义在 `backend/app/catalog.py`；`session_runner.py::_build_agent()` 按 id 加载 CARLA 的 `BehaviorAgent` / `BasicAgent`，`params` 直接映射到它们的构造参数与开关。后端不扫描第三方算法目录，也没有 `AlgorithmBase` adapter。

## 6. 路线：`(road_id, lane_id, s)` 序列

- **定义**：路点三元组正是 `carla.Waypoint` 的三要素，`map.get_waypoint_xodr(road_id, lane_id, s)` 可直接解析。
- **预设由 `tools/gen_route_presets.py` 从 OpenDRIVE 拓扑离线生成**：产物是 `backend/data/routes/<map>.json`，索引是 `backend/data/routes/index.json`：

  | 预设 id | 名称 | 选取策略 |
  |---|---|---|
  | `loop` | 城市环线 | 走到能回到自身道路的闭环，400–1400 m |
  | `arterial` | 干道直行 | 从最长道路出发沿 link 连续延伸 ≥900 m |
  | `cross` | 跨图长途 | 图上最远点对的最短路，截断到 ≤5 km |
  | `turns` | 路口穿行 | 优先走 junction 连接，≥5 次换路 |

  已生成结果（`index.json` 的 `length_km`，单位 km，列顺序固定 `loop / arterial / cross / turns`）：

  | 地图 | loop | arterial | cross | turns |
  |---|---|---|---|---|
  | Town01 | 0.62 | 1.12 | 1.06 | 1.43 |
  | Town02 | 0.40 | 0.59 | 0.50 | 0.95 |
  | Town03 | 1.17 | 0.92 | 1.69 | 0.91 |
  | Town04 | 0.87 | 1.32 | 2.19 | 1.20 |
  | Town05 | 1.17 | 1.97 | 1.05 | 0.99 |
  | Town06 | 0.63 | 1.25 | 1.23 | 1.66 |
  | Town07 | 0.51 | 1.17 | 0.67 | 1.08 |
  | Town10HD | 0.50 | 0.63 | 0.53 | 1.05 |
  | Town_C | 0.80 | 1.23 | 2.95 | 1.71 |
  | Mine_01 | 0.64 | 0.56 | 3.00 | 0.58 |
  | Town15 | 0.80 | 1.35 | 3.05 | 1.46 |
  | Town11 | 1.33 | 1.58 | 3.38 | 7.58 |
  | Town12 | 1.39 | 1.30 | 4.73 | 2.04 |
  | Town13 | 1.31 | 2.38 | 4.87 | 1.87 |
  | Eixample | 0.59 | — | — | 0.49 |

  条数与覆盖面：`data/routes/` 共 15 张图、58 条预设，**不是每图都 4 条** —— `Eixample` 只生成出
  `loop` 与 `turns` 两条，其余 14 张各 4 条。界面只展示 `ALLOWED_MAPS` 的 9 张（Town01–Town07、
  Town10HD、Town_C，即上表前 9 行），这 9 张都是 4 条；`EXCLUDED_MAPS` 里的 Town11–13 有预设但不进界面，
  `EmptyMap` 没有预设；`Mine_01` 与 `Town15` 各 4 条，也不在界面里。

  每图 JSON 的单条预设除 `length_m` 外还记 `roads`（途经路段数，重复经过同一段路会重复计）与 `waypoints`。
  序列化时每段路记入点、出点两个点，所以现有 58 条预设都满足 `waypoints = 2 × roads`，换路次数都是
  `roads − 1`；`index.json` 只同步 `length_km` 与 `waypoints`，`roads` 要在各图 JSON 里看。例：Town01 四条
  依次途经 9/18/19/24 段路，即 8/17/18/23 次换路（不是 9/18/19/24 次）。

- **路点解析**：`_resolve_route()` 只负责挑预设 / 收自定义路点并校验条数，不碰坐标；坐标在用到时才逐点调
  `map.get_waypoint_xodr(road_id, lane_id, s)`（`_waypoint_at()`，`lane_id` 为 -1 时依次试候选车道并优先
  `LaneType.Driving`）。**只有首尾解析失败会报错**：起点在 `_spawn_ego()`、终点在 `_build_agent()`；
  中间路点解析不到会被静默丢弃 —— `_cache_route_points()` 只把解析成功的点收进画线缓存，不影响运行。
  不做整段连通性校验。
- **自定义**：后端接受 `route.source=custom` + 至少 2 个 waypoints；界面的自定义路线入口是禁用占位，
  未接入表格编辑或导入/导出。

## 7. 交通与天气

**交通预设**（默认轻度，避免 30 辆挤满）：

| 预设 | 背景车 | 行人 | 说明 |
|---|---|---|---|
| `none` | 0 | 0 | 纯主车，最快 |
| `light`（默认） | 4 | 6 | 够触发跟车/让行 |
| `standard` | 10 | 6 | 一般交通 |
| `dense` | 20 | 12 | 拥堵场景 |

配置里的 `traffic.vehicles` / `traffic.walkers` 可覆盖预设数量；`traffic.seed` 只随配置落盘，当前生成未用它固定随机序列。

生成规则：背景车从地图 spawn points 随机生成，ego 周围 15 m 内与前方 40 m 内避开，车型从四轮蓝图里随机，TM autopilot；行人用 `get_random_location_from_navigation()` 取点。

**天气**（常见组合 6 条 + 自定义）：

| 预设 | 用途 |
|---|---|
| `ClearNoon` | 基线，晴天正午 |
| `CloudyNoon` | 无阴影，感知稳定 |
| `WetNoon` | 湿路面、低附着 |
| `HardRainNoon` | 暴雨，能见度与反光 |
| `ClearSunset` | 低太阳角，长阴影 |
| `MidRainyNight` | 夜间雨天，光照极限 |
| `custom` | `sun_altitude_angle / sun_azimuth_angle / cloudiness / precipitation / precipitation_deposits / wetness / fog_density / wind_intensity / dust_storm` |

自定义天气通过 `POST /sessions/{id}/weather` 的 `custom` 对象直接写 `carla.WeatherParameters` 字段；没有命名预设落盘（不存在 `data/weather/`）。

## 8. 录制与导出

单次运行产出 `runs/<session_id>/`：

| 文件 | 内容 |
|---|---|
| `config.json` | §4 的配置快照（含算法参数、种子） |
| `route.json` | 解析后的路线规格 `{"waypoints": [{"road_id","lane_id","s"}]}`，不是稠密世界坐标 |
| `metrics.csv` | 每 `target_hz/10` 采样：`t, frame, x, y, z, yaw, speed_kmh, throttle, brake, steer` |
| `events.json` | 事件数组：`recorder_error / planner_fallback / preview_degraded / tick_error / ego_stalled`（含 `t` 与 `detail`） |
| `rec.log` | CARLA recorder 回放文件（`start_recorder`） |
| `summary.json` | `session_id, map, state, duration_s, ticks, samples, speed_kmh{avg,max}, events, wall_clock_s` |

导出接口分两类：`/metrics.csv` 与 `/rec.log` 用 `FileResponse` 下载原文件；`/summary` 用 `JSONResponse` 内联返回 `summary.json` 的内容，`/events.json` 直接返回事件数组 + 计数，这两个都不是文件下载。目前固定写出上述文件，`run.export` 开关尚未生效。后续可加 Excel/图表。

## 9. UE 侧实现：原生 Slate C++（已定）

结论：界面是**原生 Slate C++，纯代码构建，不使用 UMG 资源**（没有 `.uasset` 控件，也没有 HTML 页面）。

早期考虑过"一个 Widget 内嵌 WebBrowser、复用已有 HTML 原型"的 B 方案。该方案已试并放弃，当时评估记录的原因就是 CEF 依赖 + 内存、首次加载慢。两个 HTML 原型（mockup 与 storyboard）随该方案一并删除。

| | 最终做法 |
|---|---|
| 集成度 | 最高，纯 UE 控件，与视口、相机控制共用同一套 Slate 体系 |
| 参数表单 | 只从 `/algorithms` 的 `params` 里取 `behavior` 的枚举选项；速度滑块与三个开关是 C++ 里写死的控件 |
| 迭代速度 | 慢（改一次要编 C++ / 重开编辑），接受这个代价 |
| 依赖 | 无额外依赖：不引 WebBrowser 模块，不带 CEF |

UE 侧 C++ 负责：把 Slate 面板挂到 game viewport、按后端状态驱动预览/跟车相机、启动并轮询 Python 后端。入口是控制台命令 `TestPlatform.Panel`（0 隐藏），没有编辑器菜单项。后端自动启动由控制台变量 `TestPlatform.BackendAutoStart` 控制（默认 1；设为 0 时只连不拉）；解释器默认取 `C:/SoftWare/MiniConda/envs/chat_carla/python.exe`，不存在时退回 PATH 上的 `python.exe`，也可用命令行 `-TestPlatformPython=` 指定。拉起前先向解析出的基址发一次 `GET /health`（走 HTTP 模块线程，超时 2 s，不占游戏线程）；有应答就不重复拉起，没有应答才启动一次 `run.py`，并且发起过一次拉起后不再重复探测。

面板不要求后端在打开时就位：`/health` 每 2 s 探一次（就绪后回到 5 s），后端一旦应答就每 3 s 重试一次目录（`/maps`、`/vehicles`、`/weather`、`/traffic`、`/algorithms`，同一轮未返回前不叠加），目录就绪后停止重试。因此后端晚于面板启动（自动拉起、手动运行或点「启动后端」）也会在几秒内被接管，卡片自动消失，不需要点「重试」。卡片按 `/health` 的实时结果区分三种状态：探测中（尚无应答）、后端已应答但目录未读完（“正在接管”）、后端不可达（“配置后端未运行”）；后端窗口被关掉后卡片会重新出现，即使目录还缓存着。

卡片上还有一个「启动后端」按钮，只在后端不可达时出现；它与控制台命令 `TestPlatform.StartBackend` 走同一条 `StartBackendNow()`：先探一次 `/health`，有应答就拒绝（不拉起第二个实例），没有应答才用下面的可见终端窗口启动 `run.py`。显式启动可以绕过一次性闩锁和 8 s 限流；按钮在等待应答期间显示“启动中…”并禁用，20 s 无应答就把失败原因留在卡片上。测试时不方便点击按钮，用 `-ExecCmds="TestPlatform.StartBackend,..."` 即可触发同一条代码路径。

自动拉起**不再是隐藏进程**，而是一个可见的 Windows Terminal 窗口（`UnrealEditor -game` 下同理）：

- 标题固定为 `CARLA TestPlatform Backend`：`--suppressApplicationTitle` 让 cmd / python 改不了它，操作员照这个名字找窗口；
- `wt.exe -w -1 nt ... -d "<backend 目录>"` 开的是**独立窗口**，不是塞进操作员已开窗口的标签页——否则“关窗口”会连别人的标签一起关掉；
- 窗口里跑 `cmd /k "chcp 65001>nul && <python> run.py"`：`cmd /k` 让 python 退出后窗口还留着，traceback 不会跟着一起消失；
- stdout/stderr 就是这个窗口的实时输出（面板不再持有管道），`TESTPLATFORM_HOST` / `TESTPLATFORM_PORT` 照旧在拉起前写进环境，后端仍绑面板探测的那个端点；
- 关掉窗口 = 停掉后端：控制台一关，python 随之结束，8765 不再监听，面板下一次 `/health` 探不到就显示“配置后端未运行”（顶部服务器小条此时是 `未知`，因为探测本身没有应答，见 `STestPlatformUI::UpdateChrome` 里 未知/未连接 的区分）；会话条与同步条也随之变中性（`会话 · 未知` / `同步模式 · —`），不再重复最后一次应答；
- 关窗时预览会话正把世界置于同步模式也不会永久卡死：`FCarlaEngine::OnPreTick` 的同步等待有两个上限，都是控制台变量、都能用 `0` 关掉。`carla.SyncCueGrace`（默认 60 s）只在**确实没有连接**、且等待超过它时才回退到异步模式；进程被挂起/调试器暂停的活客户端连接仍是 ESTABLISHED，因此照旧等它，这与同步模式的语义一致。但「还有连接」不等于「发 tick cue 的那个客户端还活着」——监控脚本、第二个工具、调试用的笔记本都会占住 RPC 端口，所以新增 `carla.SyncCueCeiling`（默认 300 s，`0` = 永不）：**不管还有没有连接**，等待超过它就回退到异步模式。这条才是保证：carla 后端被杀后，即使另一个空闲客户端仍连着 2000，编辑器也会在 ceiling 到点时恢复，而不会无限冻结（上一版正是这样：只有所有客户端都断开才恢复）。两个阈值都设 0（或沿用旧名 `carla.SyncCueTimeout=0`，它被保留为 grace 的别名并同时关掉两者）就是上游 CARLA 的永久等待。回退时打 Error，说明原因（已无客户端连接，或 ceiling 到点、连接可能仍在）与后果（客户端会读到 `synchronous_mode=false`、`fixed_delta_seconds=None`）；grace 到点但客户端还连着时只打 Warning，人就能区分「暂停」和「卡死」。后端照旧把这次丢失记成 `sync_mode_lost` 事件并写进指标，面板的 `同步模式` 条也照旧转成 `已失去`。**已知代价**：进程被挂起/调试器暂停的活客户端连接是 ESTABLISHED，不会被 grace 踢；但非 Windows 平台、或拿不到 TCP 表时这个信号不可用（`Unknown`），那时只靠 grace 判定，一个**只是慢**的活客户端也会在 grace 后被切到异步模式——把 `carla.SyncCueGrace` 调大，或依赖 ceiling 兜底；
- 手动关窗之后不会被重拉：一次性闩锁 `bBackendSpawnAttempted` 仍然只在第一次拉发起前置位，没有任何重试路径；
- 编辑器退出**不杀**这个进程（`ShutdownModule` 不碰它，也不放 job object 里），窗口留在操作员手里。

`wt.exe` 找不到、或 `FPlatformProcess::CreateProc` 失败时，退回到普通可见控制台窗口
`cmd /c start "CARLA TestPlatform Backend" "<python>" run.py`，并用 Warning 日志说明走了哪条路径（Windows Terminal / 普通控制台）以及启动进程的 pid、工作目录、端点 —— 绝不静默退回隐藏模式。`wt.exe` 先按 `%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe`（Store 按用户安装的执行别名）找，再退回 PATH 上的 `wt.exe`；命令行开关 `-TestPlatformBackendLauncher=<path>` 覆盖这个查找（指到不存在的文件即可验证兜底路径）。日志里的 pid 是启动器（wt.exe / cmd.exe）的 pid，它很快就退出，真正的 python 在它下面：`python.exe -> cmd.exe -> WindowsTerminal.exe`。

### 9.1 入口地图与面板开关（2026-11-28）

配置界面不再是"打开任意地图就有"的覆盖层，而是**入口地图的前端**。

- **平台资产的家**：`Content/TestPlatform/`。入口地图是 `Content/TestPlatform/Maps/L_TestPlatformEntry.umap`，一个**空壳**：只有 1 个 `PlayerStart`，没有城市几何。配置界面中央的 3D 城市不是这张图的一部分 —— 后端 `POST /sessions/preview_map` 用 `client.load_world` 把选中的城镇加载进**同一个进程**，面板是盖在那张城镇上的。
- **入口图的 OpenDrive**：空壳不等于无图。`Content/TestPlatform/Maps/OpenDrive/L_TestPlatformEntry.xodr` 是一份只有 header、零条 road 的合法 OpenDRIVE（与 `EmptyMap.xodr` 同构），CARLA 的 `ACarlaGameModeBase::ParseOpenDrive` 因此建出一张空 Map：启动日志不再出现 `LogCarla: Error: No OpenDrive file found for map 'L_TestPlatformEntry'` 与 `LogCarla: Error: Invalid Map`，后端 `world.get_map()` 也不再吐出 `ERROR: unable to parse the OpenDRIVE XML string`。真正的坏图照旧报错：把这份 xodr 临时移走（缺失）或换成非法 XML（解析失败）仍会得到同样的两条 `LogCarla: Error`。`session_runner.py` 里对 `get_map()` 的 `RuntimeError` 兜底保留，用于 CARLA 世界里任何其它无 xodr 的关卡。
- **默认地图**：`Config/DefaultEngine.ini` 的 `GameDefaultMap` 与 `EditorStartupMap` 都是 `/Game/TestPlatform/Maps/L_TestPlatformEntry.L_TestPlatformEntry`，所以每次启动都落在配置界面。`ServerDefaultMap` 仍是 Town10HD：它只管 `-server`，与配置界面无关。`GlobalDefaultGameMode`（`CarlaGameMode`）不变 —— 空壳地图同样要由它起 CARLA 的 world/RPC 服务，已用真实 `-game` 运行验证（2000 端口监听、`carla.Client` 能连、`preview_map` 能 load_world）。
- **白名单**：`UTestPlatformSettings::EntryMaps`，落在 `Config/DefaultGame.ini` 的 `[/Script/TestPlatform.TestPlatformSettings]`（编辑器里是 Project Settings > Game > Test Platform）。条目可以写完整包路径（`/Game/TestPlatform/Maps/L_TestPlatformEntry`）或短地图名（`L_TestPlatformEntry`），PIE 前缀忽略；列表为空时回退到同一个入口地图。别的图（Town05 等）不再显示面板，浏览地图是干净的。白名单在模块启动时读一次，改动要重启进程。
- **闸门判定的是"本次运行从哪张图启动"，不是"当前地图"**：每个 game/PIE 会话只在第一个运行时世界判定一次 —— 那张图在白名单里就把面板闩上（`bEntryMapArmed`），不在就永远不开。闩上之后只有一种世界切换保留面板：**面板自己请求的后端预览加载**（面板选图 → `PreviewMap` → 后端 `client.load_world`）。模块在 `STestPlatformUI::OnPreviewRequest` 里记下目标图，只有落到那张图（忽略 `_Opt` 后缀与大小写）的切换才算预览，所以入口图 → 城镇时面板不会被藏掉。其余任何世界切换（别的客户端 `load_world`、手动切图）都算"离开入口流程"：闩锁当场解除、面板移除，本次运行不再重新闩上 —— 从 Town05 启动的进程后来 `load_world` 到入口图也不会弹面板。日志据此核对：判定一次时的 `panel gate: run started on <包路径>: <armed|stays off>`、解除时的 `panel gate: left the entry flow: world changed <旧图> -> <新图> without a panel-requested preview`，以及每次地图变化一行 `map <包路径>: config panel enabled|disabled (<原因>)`，原因写的是真实依据（`entry map` / `backend preview load in a run that started on an entry map` / `this run started on <包路径>, which is not an entry map` 等）。
- **会话结束即重置**：`TickRuntime` 发现 game viewport 不存在（编辑器空闲、PIE 结束）时调用 `ResetGateForNewSession()`，清掉启动图、闩锁、手动覆盖和预览目标，并把面板与 viewport 的绑定复位。因此一次从入口图启动的 PIE 不会把面板闩给同一编辑器进程里的下一次 PIE；下一次 PIE 按它自己的启动图重新判定。（该路径由代码与一次 `-game` 进程内不复用会话的实测支撑；PIE 本身按约束没有运行。）
- **两种显式覆盖**：命令行 `-TestPlatformForceUI` 无视白名单强制显示（自动化用，等价于"永远允许"）；控制台 `TestPlatform.Panel 1` 是手动覆盖（`Panel 0` 把控制权交回闸门）。两者都不写闩锁：`Panel 0` 之后仍按上面的规则判定，自动路径永远不会在非入口地图里显示面板。
- 默认启动仍然没有地图参数：`run-editor-lowquality.ps1` 不带 `-Map` 时用项目默认图（入口地图），带 `-Map /Game/Carla/Maps/Town05_Opt` 时打开指定图且不出面板。脚本按 `Config/DefaultGame.ini` 里的同一份 `EntryMaps` 白名单判断该等哪个就绪标志：入口图（或脚本的 `-ForceUI`）等 `test platform UI added`；非入口图等引擎自己的 `LogWorld: Bringing World ... up for play`，不会再为一个按设计不会出现的面板标志空等到超时。

## 10. 目录结构

```
Content/TestPlatform/          测试平台自己的 UE 资产（不放 CARLA 的内容）
  Maps/L_TestPlatformEntry.umap  入口地图：空壳 + 1 个 PlayerStart，配置界面的宿主
  Maps/OpenDrive/L_TestPlatformEntry.xodr
                                 入口地图的零 road OpenDRIVE：让 CARLA 建出空 Map 而不是报错

Source/TestPlatform/
  Public/           模块、API 客户端、Slate 面板、相机 rig 的头文件
  Private/          模块实现（TestPlatformModule / TestPlatformApi / STestPlatformUI / PreviewCameraRig）
  Platform/
    backend/
      app/            FastAPI 路由（main.py、sessions_api.py）、catalog、session_runner、preview、thumbs、carla_bridge、
                      paths.py（路径与 TESTPLATFORM_HOST/PORT 等环境变量）
      data/routes/    已生成的地图路线预设 + index.json
      data/vehicles.json
      data/vehicle_thumbs/   车辆卡片位图
      data/vehicle_spin/     转台帧
      data/vehicle_art_raw/  原始素材备份（保留）
      data/cache/previews/   预览渲染缓存
      requirements.txt       后端依赖（钉版本，不含 carla，见 §12）
      run.py
    tools/            gen_route_presets.py、render_map_preview.py、render_vehicle_thumbs.py、
                      render_vehicle_turntable.py、postprocess_vehicle_art.py、run-editor-lowquality.ps1
    ui/               截图与地图缩略图（HTML 原型已删）
    runs/             每次运行的产物
    docs/             本文件
```

## 11. 落地顺序

1. `GET /config/schema`、`GET /algorithms`、`GET /maps`、`GET /maps/{map}/routes` 等只读接口（先给假数据也能跑通前端）
2. 缩略图生成（地图俯视、车辆 3/4 视角）与缓存
3. 前端接后端：地图/主车/路线/天气/算法/运行 六块全部可用
4. `POST /sessions/{id}/start`：load_world → 天气 → ego → 算法 → 交通 → recorder → tick 循环
5. 指标与导出：`metrics.csv` / `rec.log` 是 HTTP 文件下载，`summary` / `events.json` 是内联 JSON
6. UE 侧：控制台命令入口 `TestPlatform.Panel` + 原生 Slate 面板（纯 C++，无 UMG 资源）
7. 失败兜底：地图加载失败、生成点被占、路线不可达、算法抛异常、后端与 UE 断连

## 12. 环境与安装（干净检出）

本节命令的工作目录是仓库根 `C:\GameProject\CarlaUE5\carla`（即 `CARLA_ROOT`），文中路径都相对它。

后端跑在本机的 `chat_carla` conda 环境里（`C:\SoftWare\MiniConda\envs\chat_carla`，Python 3.10.21）。干净检出后按四步装：

1. **依赖**：`python -m pip install -r Unreal/CarlaUnreal/Source/TestPlatform/Platform/backend/requirements.txt`。
   里面钉的是这台解释器里实测的版本（`fastapi==0.128.8`、`uvicorn==0.39.0`、`pillow==11.3.0`、
   `numpy==1.26.4`、`networkx==3.4.2`）；`numpy` / `networkx` 不是 `app/` 直接 import 的，是
   `PythonAPI/carla/agents/navigation` 里的导航算法要用的。
2. **CARLA 客户端**：`carla` 不是 PyPI 依赖，`requirements.txt` 因此**故意不钉它**。它的 `pyproject.toml`
   **不在版本库里**：`PythonAPI/.gitignore:5` 忽略它，`PythonAPI/CMakeLists.txt:101-104` 在 CMake configure
   时由 `PythonAPI/carla/pyproject.toml.in` 生成（生成物里 name=`carla`、version=`1.0`，scikit-build-core
   驱动 CMake）。所以干净检出里**没有**这个文件，`python -m pip install -e PythonAPI/carla` 会直接失败；
   **必须先 configure 再安装**，只做 editable 安装是不够的：
   `cmake --preset Release`（`CMakePresets.json`，构建目录 `Build/Release`）生成 `PythonAPI/carla/pyproject.toml`，
   再 `cmake --build Build/Release --target carla-python-api-install` 构建并装进 PATH 上的 `python`（激活的
   `chat_carla`；主 Python 版本走 editable 安装，其它已配置版本装对应 wheel，见 `PythonAPI/CMakeLists.txt:175-251`）。
   装好后是 site-packages 里的 `carla.cp310-win_amd64.pyd`；本机这份的 `direct_url.json` 指向
   `C:\GameProject\CarlaUE5\CarlaUE5\PythonAPI\carla`，与本仓库的 `CARLA_ROOT` 不保证是同一棵树。
3. **导航算法**：`BehaviorAgent` / `BasicAgent` 不在 wheel/pyd 里，后端把 `CARLA_ROOT/PythonAPI/carla` 直接插进
   `sys.path`（`paths.PYTHON_API_CARLA`；`CARLA_ROOT` 默认 `C:\GameProject\CarlaUE5\carla`，可用同名环境变量
   覆盖）。所以 `CARLA_ROOT` 必须指向含 `PythonAPI/carla/agents` 的源码树，否则 `POST /sessions/{id}/start`
   在 `_build_agent()` 里抛 `ImportError`。
4. **启动**：`cd Unreal/CarlaUnreal/Source/TestPlatform/Platform/backend` 后 `python run.py`
   （默认 `127.0.0.1:8765`，可用 `TESTPLATFORM_HOST` / `TESTPLATFORM_PORT` 改；UE 侧用
   `-TestPlatformBackend=` 指同一地址）。模拟器没起时 `/health` 照样返回 200，只是 `carla.connected` 为
   `false` 并带上连接错误；配置类接口照常可用。手动启动是可选做法：不手动启动时 UE 在探测不到后端后会
   自己开一个可见的 Windows Terminal 窗口跑 `run.py`（见 §9），关掉那个窗口就把后端停了；手动的窗口和
   自动的窗口是两条路，同时只该存在一个。

## 13. 已确认的边界

- 只跑单主车；交通车/行人按 §7 预设。
- 主车驾驶由后端算法执行（默认 CARLA `BehaviorAgent`），UE 侧只显示。
- 录制、指标、导出都在后端完成；UE 只轮询状态并显示，不提供回放入口。
- 路线以 `(road_id, lane_id, s)` 为准；预设条数按图而定（界面展示的 9 张图各 4 条，`Eixample` 只有 2 条），并非每图都是 4 条。自定义路线后端已接受 `route.source=custom`，界面入口尚未接入。

## 14. 当前测试范围与未验证项

**范围（2026-11-28 起）**：测试只对 **Town10HD** 负责；界面上另外 8 张图（Town01–07、Town_C）保留可选，但**不作为验收要求**。因此“部分地图主车 0 km/h”这类问题在 Town10 上不出现即视为达标。

**已验证（截至 2026-11-28）**：

- 配置页可加载、默认地图 Town10HD、9 图 × 4 条预设与 18 个车型的缩略图可用；
- 后端在模拟器未启动时仍可服务（`/health` 200，`carla.connected:false`），配置类接口不受影响；
- 模块在 Editor / Game 两个 target 下均可编译并用无头方式验证过；
- 面板状态（已连接 / 未连接 / 未知）、探测地址、加载期行为均有截图与原始日志证据（见 `acceptance/`）。

**未验证（尚未有任何一次真实运行）**：

- **Town10HD 上主车是否真的行驶**：`POST /sessions/{id}/start` 之后 `BehaviorAgent` 是否接管、速度与轨迹是否合理——**从未端到端跑过**；
- 路线预设叠加与主车实际轨迹是否一致；
- `ego_stalled` 是否会在真实运行中触发、以及是否显示到面板；
- `metrics.csv` / `events.json` / `summary` 在一次真实运行后能否导出可用数据（端点存在，但没跑过真实会话）；
- 录制文件 `rec.log` 能否用于回放。

**下一步（建议顺序）**：启动后端（8765）→ 启动编辑器 `-game`（该进程自身即 CARLA RPC 服务，监听 2000）→ `preview_map(Town10HD)` → `start` → 采样 60 s 的速度/位置/会话状态 → 抓俯视跟随与面板状态图 → 检查导出产物 → 关闭并清理。一次运行即可覆盖上面四项。

## 15. UI 迭代策略：迁往 UMG（已决定，待执行）

**问题**：面板当前是纯 Slate C++，改一个 padding 也要编译 15–20 s + 重启约 40 s，一轮 1–2 分钟；结构级改动无法热改。设计决策与实现被压进同一个编译回路。

**决定（2026-11-28）**：把面板迁到 **UMG**，把布局试错放进编辑器，彻底消除“HTML 原型 → Slate”的翻译误差。逻辑（REST、轮询、相机 rig、状态映射）仍是 C++，只是从 `SCompoundWidget` 搬到 `UUserWidget` 基类 + `UGameInstanceSubsystem`；布局与样式进 `.uasset`。

**代价与前提**：

- UMG 资产是二进制，**必须在编辑器里创建/修改**（人工操作或 live MCP），无法像源码一样由无人值守的 agent 夜间完成——迭代循环里需要人在场；
- 逻辑不会变短，只会被拆成“资产 + C++ 数据层”；
- 透明遮罩要重做：根 Widget 全透明、ZOrder 正确、输入模式不能吞掉 3D 视角控制——这是迁移最易翻车处。

**先做 spike（1–2 小时），四项判据全部通过再全量迁移**：

1. 全透明 UMG 面板挂上 viewport：引擎画面完整透出、面板可点击、3D 视角输入不受影响；
2. 用 UMG 重做**一个**真实组件（右栏「5 智驾算法」：chips + 滑块 + 三个复选框 + 提示文字）；
3. 该组件读真实后端数据（`/algorithms`）并把改动回传；
4. 实测**改一个 padding 到运行画面生效的耗时**——这个数字决定全量迁移是否值得。

**通过后的迁移顺序**：右栏 → 车型卡片网格 → 地图列表 → 顶栏状态条 → 相机联动。失败则回退到“Slate + 数据化参数（尺寸/颜色/间距外置）+ 设计原型定稿”的组合。

**更新（2026-11-28，同日撤销）**：不迁 UMG，**继续用 Slate**。理由：界面迭代由 agent 承担（编译等待不计入人的时间），人只验收最终结果；UMG 的收益是“人能即时拖改”，在这个分工下不成立，而代价（资产二进制化、必须有编辑器在场、透明遮罩重做、逻辑拆分）是实打实的。上一段的 spike 取消。

因此 Slate 路线下的提速手段改为：

- 无头编译 + 自动截图回路（启动 `-game`、`PrintWindow(..., 3)`、关闭）——单轮约 1–2 分钟，不需要人在回路；
- 只改单个 TU 时走增量编译（实测 13–20 s）；
- 视觉类改动以 `Platform/ui/` 下的截图作为验收证据，逐轮对比。

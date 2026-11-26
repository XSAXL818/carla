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
| GET | `/maps` | 界面可选地图 + 每图 4 条预设摘要 |
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

## 5. 算法参数自描述（对应"参数可自适应配置"）

`GET /algorithms` 返回字段元数据，界面据此**动态渲染**控件，新增算法不用改前端：

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

控件映射：`enum→分段/下拉`、`number→滑块+数值框`、`bool→开关`、`string→输入框`、`secret→遮罩框`。

当前只注册 `carla.behavior` 与 `carla.basic` 两个内置算法，定义在 `backend/app/catalog.py`；`session_runner.py::_build_agent()` 按 id 加载 CARLA 的 `BehaviorAgent` / `BasicAgent`，`params` 直接映射到它们的构造参数与开关。后端不扫描第三方算法目录，也没有 `AlgorithmBase` adapter。

## 6. 路线：`(road_id, lane_id, s)` 序列

- **定义**：路点三元组正是 `carla.Waypoint` 的三要素，`map.get_waypoint_xodr(road_id, lane_id, s)` 可直接解析。
- **界面展示的 9 张图各 4 条预设**：由 `tools/gen_route_presets.py` 从 OpenDRIVE 拓扑离线生成（已产出 `backend/data/routes/*.json`）：

  | 预设 id | 名称 | 选取策略 |
  |---|---|---|
  | `loop` | 城市环线 | 走到能回到自身道路的闭环，400–1400 m |
  | `arterial` | 干道直行 | 从最长道路出发沿 link 连续延伸 ≥900 m |
  | `cross` | 跨图长途 | 图上最远点对的最短路，截断到 ≤5 km |
  | `turns` | 路口穿行 | 优先走 junction 连接，≥5 次换路 |

  已生成结果（km / 换路数）：Town01 `0.62/9 1.12/18 1.06/19 1.43/24`，Town03 `1.17 0.92 1.69 0.91`，
  Town05 `1.17 1.97 1.05 0.99`，Town10HD `0.50 0.63 0.53 1.05`，Town12 `1.39 1.30 4.73 2.04`，
  Town13 `1.31 2.38 4.87 1.87`（全表见 `backend/data/routes/index.json`）。
  `data/routes/` 现有 15 张图的预设；界面只展示 `ALLOWED_MAPS` 里的 9 张
  （Town01–Town07、Town10HD、Town_C），这 9 张都是 4 条。`EXCLUDED_MAPS` 中的 Town11–13 与 EmptyMap
  不列出；其余 Eixample、Mine_01、Town15 也不在界面里，其中 Eixample 只生成出 `loop` 与 `turns` 两条，
  Mine_01 与 Town15 各 4 条。

- **路点解析**：后端启动前逐个路点调用 `map.get_waypoint_xodr(road_id, lane_id, s)`；`lane_id` 为 -1 时
  依次尝试候选车道并优先 `LaneType.Driving`。解析不到就报错，不做整段连通性校验。
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

导出通过 `GET /sessions/{id}/metrics.csv`、`/events.json`、`/summary`、`/rec.log` 下载；目前固定写出上述文件，`run.export` 开关尚未生效。后续可加 Excel/图表。

## 9. UE 侧实现：原生 Slate C++（已定）

结论：界面是**原生 Slate C++，纯代码构建，不使用 UMG 资源**（没有 `.uasset` 控件，也没有 HTML 页面）。

早期考虑过"一个 Widget 内嵌 WebBrowser、复用已有 HTML 原型"的 B 方案。该方案已试并放弃，当时评估记录的原因就是 CEF 依赖 + 内存、首次加载慢。两个 HTML 原型（mockup 与 storyboard）随该方案一并删除。

| | 最终做法 |
|---|---|
| 集成度 | 最高，纯 UE 控件，与视口、相机控制共用同一套 Slate 体系 |
| 自适应用表单 | C++ 侧按 `/algorithms` 返回的 `params` 自描述 schema 生成控件 |
| 迭代速度 | 慢（改一次要编 C++ / 重开编辑），接受这个代价 |
| 依赖 | 无额外依赖：不引 WebBrowser 模块，不带 CEF |

UE 侧 C++ 负责：把 Slate 面板挂到 game viewport、按后端状态驱动预览/跟车相机、启动并轮询 Python 后端。入口是控制台命令 `TestPlatform.Panel`（0 隐藏），没有编辑器菜单项。后端自动启动由控制台变量 `TestPlatform.BackendAutoStart` 控制（默认 1；设为 0 时只连不拉）；解释器默认取 `C:/SoftWare/MiniConda/envs/chat_carla/python.exe`，不存在时退回 PATH 上的 `python.exe`，也可用命令行 `-TestPlatformPython=` 指定。拉起前先向解析出的基址发一次 `GET /health`（走 HTTP 模块线程，超时 2 s，不占游戏线程）；有应答就不重复拉起，没有应答才启动一次 `run.py`，并且发起过一次拉起后不再重复探测。

## 10. 目录结构

```
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
5. 指标与导出（HTTP 下载）
6. UE 侧：控制台命令入口 `TestPlatform.Panel` + 原生 Slate 面板（纯 C++，无 UMG 资源）
7. 失败兜底：地图加载失败、生成点被占、路线不可达、算法抛异常、后端与 UE 断连

## 12. 已确认的边界

- 只跑单主车；交通车/行人按 §7 预设。
- 主车驾驶由后端算法执行（默认 CARLA `BehaviorAgent`），UE 侧只显示。
- 录制、指标、导出都在后端完成；UE 只轮询状态并显示，不提供回放入口。
- 路线以 `(road_id, lane_id, s)` 为准，预设 4 条/图；自定义路线后端已接受 `route.source=custom`，界面入口尚未接入。

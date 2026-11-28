# 在另一台电脑上搭起来

本文档写给「已经能在这台机器上跑、要复制到另一台机器」的情况。按顺序做，每一步都有验证方法。

> 适用提交：`133d2164e`（2026-11-28）之后的 `dev` 分支。

---

## 0. 先搞清楚什么在 git 里、什么不在

| 内容 | 在 git 里？ | 说明 |
|---|---|---|
| 源码（`Source/`、`Plugins/*/Source`） | ✅ | 含 TestPlatform 模块全部代码 |
| 后端 Python（`Platform/backend`） | ✅ | 含 `requirements.txt` |
| 路线预设、车型图、工具脚本 | ✅ | `Platform/data`、`Platform/tools` |
| 验收证据、文档 | ✅ | `acceptance/`、`Docs/` |
| 入口关卡 `Content/TestPlatform/` | ✅ | 唯一入库的 Content 内容 |
| **`Content/Carla/`（51 GB）** | ❌ | `.gitignore:20` 排除 |
| **引擎 `UE_5.8_Carla`（几十 GB）** | ❌ | 独立安装 |
| **构建产物（`Build/`、`Intermediate/`、`Binaries/`）** | ❌ | 跨机器无效，必须重生成 |

所以「clone 下来」只拿到**代码**，跑不起来。下面补齐另外三样。

---

## 1. clone 代码

```powershell
git clone -b dev https://github.com/XSAXL818/carla.git C:\GameProject\CarlaUE5\carla
cd C:\GameProject\CarlaUE5\carla
git log --oneline -3
```

**验证**：HEAD 应是 `133d2164e` 或更新；`git status` 干净。

> ⚠️ 分支必须是 **`dev`**。默认分支里没有测试平台代码。

---

## 2. 拷贝 `Content/Carla`（51 GB）

从原机器复制整个目录到新机器的同一相对位置：

```
<clone 根>/Unreal/CarlaUnreal/Content/Carla/
```

按必需度分档：

| 子目录 | 体量 | 必需？ |
|---|---|---|
| `Static/` | 34.4 GB | **必需**——建筑/道路/车辆网格，地图引用它们 |
| `Maps/` | 10.9 GB | **必需**——Town10HD 等地图 |
| `__ExternalActors__/` | 3.8 GB | **必需**——World Partition 外部 Actor，缺了地图能开但内容缺失 |
| `Blueprints/` | 75 MB | **必需**——`CarlaGameMode` 在这，没有它 RPC 服务起不来 |
| `OpenDrive/` | ~0 MB | **必需**——很小但关键 |
| `road_xodr/` | 4 MB | **必需** |
| `Config/` | ~0 MB | **必需** |
| `HDMaps/` | 1.6 GB | 可选（高清地图） |
| `HoudiniEngine/` | 379 MB | 可选 |
| `Animations/` | 43 MB | 可选（行人动画） |

**验证**：`Content/Carla/Maps/Town10HD_Opt.umap` 存在且大小与原机器一致。

---

## 3. 引擎

需要 CARLA 定制的 Unreal Engine 5.8 fork（本机在 `G:\Epic\EpicApps\UE_5.8_Carla`）。

- 从原机器整目录拷贝，或
- 按 CARLA 官方 `build-carla-ue58` 流程自行构建

**验证**：`<engine>/Engine/Binaries/Win64/UnrealEditor.exe` 存在。

> ⚠️ 用 Epic 官方启动器装的 UE 5.8 **不能替代**——CARLA fork 有定制插件。

---

## 4. Python 环境

```powershell
conda create -n chat_carla python=3.10 -y
conda activate chat_carla
pip install -r Unreal/CarlaUnreal/Source/TestPlatform/Platform/backend/requirements.txt
```

`carla` 客户端是**本地 editable 包**，不能 `pip install carla`：

```powershell
# 必须先 configure 生成 pyproject.toml（它不在版本库里，由 CMake 从 .in 模板生成）
cmake --preset Release
cmake --build Build/Release --target carla-python-api-install
```

详细说明见 `Source/TestPlatform/Platform/docs/architecture.md` §12。

**验证**：`python -c "import carla; print(carla.__version__)"`。

---

## 5. 删掉旧的构建产物并重编

```powershell
Remove-Item -Recurse -Force Build -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force Unreal\CarlaUnreal\Intermediate -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force Unreal\CarlaUnreal\Binaries -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force Unreal\CarlaUnreal\Saved -ErrorAction SilentlyContinue
Get-ChildItem Unreal\CarlaUnreal\Plugins -Directory | ForEach-Object {
  Remove-Item -Recurse -Force "$($_.FullName)\Intermediate" -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force "$($_.FullName)\Binaries" -ErrorAction SilentlyContinue
}
```

然后：

```powershell
& "<engine>\Engine\Build\BatchFiles\Build.bat" CarlaUnrealEditor Win64 Development `
    -Project="C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\CarlaUnreal.uproject" `
    -WaitMutex -MaxParallelActions=6
```

**验证**：`Result: Succeeded`，且 `Unreal/CarlaUnreal/Binaries/Win64/UnrealEditor-TestPlatform.dll` 存在。

> ⚠️ 不删 `Intermediate` 会编译失败，且报错信息很难懂（里面写死了原机器的绝对路径）。

---

## 6. 配置 `.uproject` 的引擎关联

`CarlaUnreal.uproject` 里的 `EngineAssociation` 是本机的引擎 GUID/路径。新机器上要么：

- 让引擎注册表指向新位置的 `UE_5.8_Carla`，或
- 直接编辑 `.uproject` 的 `EngineAssociation` 为新引擎路径

**验证**：双击 `.uproject` 能用正确引擎打开，且 CARLA 插件被识别（不会弹「Missing Plugin」）。

---

## 7. DSH 侧

| 项 | 做法 |
|---|---|
| 技能 | `~/.agents/skills/` 是**用户级、按机器存**的。新机器要重新装（含 `game-ui-support`） |
| `carla` MCP | 路径变了，需重设：`set_config({"CARLA_ROOT": "<新 clone 路径>"})` |
| `unreal-mcp` | 端口 6256，只有编辑器开着才有 |

`.mcp.json` 里用的是相对路径（`Util/AgenticTools/bin/...`），clone 后可直接用；但 DSH profile 里注册的 `carla` 用的是**绝对路径**，新机器要改。

---

## 8. 跑起来验证

```powershell
# 后端
cd Unreal\CarlaUnreal\Source\TestPlatform\Platform\backend
python run.py

# 另一个终端：编辑器（最低画质，参数见 Docs/LOW-QUALITY-LAUNCH.md）
& "<engine>\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\CarlaUnreal.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -nosound -nologtimes `
  -TestPlatformBackend=http://127.0.0.1:8765 `
  -ABSLOG="C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Saved\Logs\verify.log" `
  -ExecCmds="sg.ViewDistanceQuality 0,sg.ShadowQuality 0,sg.PostProcessQuality 0,r.DynamicGlobalIlluminationMethod 0,r.ReflectionMethod 0,r.Shadow.Virtual.Enable 0,r.ScreenPercentage 70"
```

**成功的标志**：配置界面出现 → 顶栏 `已连接` → 中间显示 Town10HD 俯瞰 → 地图列表有 9 张。

---

## 常见故障对照

| 现象 | 原因 |
|---|---|
| 双击 `.uproject` 提示 Missing Plugin | 用的是 Epic 官方 UE 5.8，不是 CARLA fork |
| 编译失败，报找不到 `Definitions.def` | `Build/CMakeCache.txt` 没生成，需先 `cmake --preset Release` |
| 编辑器起来但地图是空的 | `Content/Carla` 拷漏了 `__ExternalActors__` 或 `Static` |
| 面板不出现 | 当前地图不是入口关卡；或 `Config/DefaultEngine.ini` 的 `GameDefaultMap` 没指向 `L_TestPlatformEntry` |
| 面板显示「配置后端未运行」 | 后端没起，或端口不是 8765 |
| `import carla` 失败 | 没跑 `carla-python-api-install`，或 `CARLA_ROOT` 指错 |
| 编译报绝对路径错误 | 没删 `Intermediate` |

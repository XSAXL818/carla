# CARLA 低画质启动参数（权威定义）

> 本文件是**唯一权威**的低画质启动参数清单。任何 agent / 脚本 / 人手敲命令行启动
> `UnrealEditor.exe` 跑本项目时，都应照抄这里的内容，不要各自发明。
> 脚本 `run-editor-lowquality.ps1` 用的是同一组值；改这里就要同步改那里。

## 为什么需要

这台机器跑 CARLA 的编辑器二进制很重：`-game` 模式常驻约 13 GB、默认画质下 15–16 FPS，
仿真时钟只能跑到实时的 0.14–0.8×。降到最低画质后能明显回收帧率，也让真机验证变得可承受。

## 基准参数（1280×720，ScreenPercentage 70）

```
ResX=1280  ResY=720  r.ScreenPercentage 70
```

需要看清界面文字时提到 `1600x900 / 50`；只是跑行为验证时用基准值即可。

## 画质 cvar（全部置最低/关闭）

```
sg.ViewDistanceQuality 0
sg.ShadowQuality 0
sg.PostProcessQuality 0
sg.TextureQuality 0
sg.EffectsQuality 0
sg.FoliageQuality 0
sg.ShadingQuality 0
sg.AntiAliasingQuality 0
r.DynamicGlobalIlluminationMethod 0     # 关 Lumen GI
r.ReflectionMethod 0                    # 关 Lumen 反射
r.Shadow.Virtual.Enable 0               # 关虚拟阴影贴图
r.AntiAliasingMethod 0
r.SSR.Quality 0
r.AmbientOcclusionLevels 0
r.BloomQuality 0
r.MotionBlurQuality 0
r.DepthOfFieldQuality 0
```

## 完整命令行（`-game`，跑行为/视觉验证）

```
& "G:\Epic\EpicApps\UE_5.8_Carla\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\CarlaUnreal.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -nosound -nologtimes `
  -TestPlatformBackend=http://127.0.0.1:8765 `
  -ABSLOG="C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Saved\Logs\verify-<case>.log" `
  -ExecCmds="sg.ViewDistanceQuality 0,sg.ShadowQuality 0,sg.PostProcessQuality 0,sg.TextureQuality 0,sg.EffectsQuality 0,sg.FoliageQuality 0,sg.ShadingQuality 0,sg.AntiAliasingQuality 0,r.DynamicGlobalIlluminationMethod 0,r.ReflectionMethod 0,r.Shadow.Virtual.Enable 0,r.AntiAliasingMethod 0,r.SSR.Quality 0,r.AmbientOcclusionLevels 0,r.BloomQuality 0,r.MotionBlurQuality 0,r.DepthOfFieldQuality 0,r.ScreenPercentage 70"
```

## 三条纪律（比参数本身更重要）

1. **必须带 `-ABSLOG=<独立路径>`**。多个进程共用 `Saved/Logs/CarlaUnreal.log` 会互相轮转覆盖，
   导致别的 agent 的证据被冲掉。每个验证用例一个日志文件。
2. **不要加 `stat fps` / `stat unit`**，除非明确要看帧率——那个覆盖层会污染截图取证。
   需要帧率时单独跑一次。
3. **同一时刻只允许一个编辑器进程**。它是单例：两个 agent 同时要就会互相阻塞（已经发生过一次，
   一个 agent 因此卡了 45 分钟）。需要抢资源时先确认端口 2000/2001/2002 没有监听。

## 已知的启动异常（不是参数问题）

- 编辑器偶发在启动阶段崩在引擎 `DevHttp`（`InheritedContext.cpp:130` 断言），**原样重试即可**，与本次改动无关。
- 运行中偶发 `WM_CLOSE` 无响应（编辑器卡死），只能强杀；杀完确认端口释放再继续。
- `PrintWindow(hwnd, hdc, 2)` 对 UE 的 D3D 交换链常返回全黑客户区，**用 `3`（`PW_CLIENTONLY`）**。

# CarlaUnreal 项目经验库（项目级）

## CarlaUnreal.exe 一启动就不开窗口、约 2 秒内自行退出（Game target 只吃 cooked 内容）

- 日期：2026-11-26
- 引擎 / 项目：UE 5.8 CARLA fork（`G:/Epic/EpicApps/UE_5.8_Carla`）/ `CarlaUnreal`
- 现象：`& "C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Binaries\Win64\CarlaUnreal.exe" -carla-rpc-port=2000 -windowed -ResX=1280 -ResY=720` 运行后始终没有窗口，日志里只有这一组致命错误：`LogShaders: Error: The global shader cache file 'G:/Epic/EpicApps/UE_5.8_Carla/Engine/GlobalShaderCache-PCD3D_SM6.bin' is missing.` 与 `Your application is built to load COOKED content. No COOKED content was found; This usually means you did not cook content for this build.` 与 `Alternatively build and run the UNCOOKED version instead.`；紧接着 `LogWindows: FPlatformMisc::RequestExit(0, CompileGlobalShaderMap)`。日志从 `Log file open`（21:12:28）到 `Log file closed`（21:12:30）共约 2 秒，引擎时间戳只从 21:12:29.124 走到 21:12:30.061（约 0.94 秒），以 `LogExit: Exiting.` 收尾。
- 根因：`CarlaUnreal` 是 `TargetType.Game`（cooked-only）：Game 二进制不含 shader 编译器，源码版引擎目录也没有 `Engine/GlobalShaderCache-PCD3D_SM6.bin`，两者缺一都无法启动未 cook 的内容。编辑器二进制能跑是因为它走未 cook 路径。
- 做法：不 cook 时用编辑器二进制以游戏模式启动：`G:\Epic\EpicApps\UE_5.8_Carla\Engine\Binaries\Win64\UnrealEditor.exe C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\CarlaUnreal.uproject -game -windowed`（走 `UGameEngine`）。**本机本项目未验证**，先验证再当结论用。
- 别再试：不要为了让它跑起来去做 cook/打包——用户在 2026-11-26 明确决定砍掉打包这条线；也不要因为 exe 秒退就判断引擎没装或构建失败（构建产物是好的）。
- 证据：`Saved/Logs/CarlaUnreal-backup-2026.11.26-13.12.30.log`（2026-11-26 21:12:30 收尾的那次 `CarlaUnreal.log`，之后被新的启动轮转成这个名字；同内容另有一份 `CarlaUnreal-backup-2026.11.26-13.11.50.log`）；同类通用条目已存在于 `~/.dsh/ue-playbook/GENERAL.md` 第 39–41 行，本条只记项目侧的增量（本项目哪个 exe、哪条命令、怎么绕）。

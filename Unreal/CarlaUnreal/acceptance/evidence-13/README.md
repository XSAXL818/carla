# evidence-13 — the synchronous tick-cue watchdog, made safe

Round 12's review (`../12-2026-11-27T200657Z.md`, D1) measured two things: the
engine's 15 s "no tick cue" watchdog ejected a **live** client from synchronous
mode, and the backend then died on `sim_time += None`. This directory is the
evidence for the fix.

Everything here was produced on this machine, 2026-11-28 (Asia/Shanghai), with
the editor built from this commit (`CarlaUnrealEditor Win64 Development`) and the
backend run from the same tree. No PIE, no cook, no package, no asset or level
saved. Editor launches used `-game -windowed -ResX=1600 -ResY=900 -nosound
-nologtimes -TestPlatformBackend=http://127.0.0.1:8765`.

## 01 — the build really compiled the changed translation units

* `01-build-1-FALSE-identifier.txt` — first attempt: `error C2065: "FALSE": 未声明的标识符`
  in the new `GetExtendedTcpTable` calls (only that; the IPHlpApi include and the
  TestPlatform changes compiled). Extracted from the 560 KB UBT log.
* `01-build-2-succeeded.txt` — after `FALSE` → `0`: `[1/4] Compile CarlaEngine.cpp`,
  `[2/4] Link UnrealEditor-Carla.lib`, `[3/4] Link UnrealEditor-Carla.dll`,
  `Result: Succeeded`, 46.82 s. The TestPlatform TUs compiled in attempt 1
  (`[1/90] TestPlatformModule.cpp`, `[2/90] TestPlatformApi.cpp`,
  `[3/90] STestPlatformUI.cpp`, `[4/90] Link UnrealEditor-TestPlatform.dll`), and
  the stale `.obj`s were deleted beforehand, so neither "up to date" is being
  counted as evidence.

## 02 — live-but-paused client (pause 70 s > default timeout 60 s)

* `02-live-paused-probe.log` — the reviewer's probe repeated with a longer pause:
  `settings after apply: True 0.05`, `ticked once, frame = 446`, then 35 samples
  over 70 s: **every one `frame=446, sync=true, fixed_delta=0.05`**;
  `frame delta over the 70 s pause: 0`; `settings at end of pause: True 0.05`.
  The client was never ejected, nothing advanced behind its back.
* `02-editor-log-during-pause.txt` — the new log line, at 60 s:
  `LogCarla: Warning: No tick cue for 60 s in synchronous mode, but a client is still connected to RPC port 2000: waiting for it (synchronous mode lets a live client set the pace, so the world is paused, not wedged). carla.SyncCueTimeout=60, set it to 0 to wait forever.`
  So the timeout *did* elapse, and the liveness check deliberately suppressed the
  ejection.
* `02-editor-responsiveness-during-pause.csv` — the editor is `Responding=False`
  for the whole pause and `True` from t≈70 s: a live client pausing the world is
  the synchronous contract, not a wedge.

## 03 — dead client: the editor still recovers (with the backend, not a probe)

* `03-session-before-kill.json` — the panel's preview session: `state=preview`,
  `sync_mode=true`, `fixed_delta=0.33333`, `tick=1064`.
* The backend's `python run.py` (pid 47572) was then killed with `Stop-Process
  -Force` at 04:46:35 (no graceful settings reset), the backend window/port stayed
  down (`8765 unreachable`).
* `03-responsiveness-after-kill.csv` — `Responding=False` at t=2…56 s,
  **`True` from t=63 s**: the editor recovers.
* `03-editor-log-after-kill.txt` — the consequence, at Error level:
  `LogCarla: Error: Synchronous mode lost: no tick cue for 60 s and no client is connected to the RPC port any more. Returning to asynchronous mode so the game thread keeps running: clients now read synchronous_mode=false and fixed_delta_seconds=None, …`
* `03-panel-after-backend-killed.png` — the panel after the recovery
  (`PrintWindow(hwnd, hdc, 3)`, client area 1600×900): chips read
  `服务器 127.0.0.1 : 2000 · 未知`, `会话 · 未知`, `同步模式 · —`, plus the
  `配置后端未运行` card. No stale `会话 · 预览中` / `同步模式 · 20 Hz`.

## 04 — backend resilience: suspended python, longer than the timeout

* `04-session-before-suspend.json` — `state=preview tick=22 sim=7.333
  sync=True delta=0.33333`.
* `04-suspender.log` — `NtSuspendProcess` on pid 47572 at 04:39:33,
  `NtResumeProcess` at 04:40:43 (70 s; the TCP connection stays ESTABLISHED).
* `04-responsiveness-during-suspend.csv` — editor `Responding=False` throughout,
  `/health` timing out, `True` and `200` again the moment the resume lands.
* `04-editor-log-suspend-resume.txt` — the same "still connected … waiting"
  Warning; **no** "Synchronous mode lost" Error, because no client was gone.
* `04-session-after-resume.json` — `state=preview` (not `error`), `tick=189`,
  `sim_time=63`, `sync_mode=true`, `sync_lost=false`, `events=0`; a second poll
  6 s later: `tick=207 sim=69`. The loop kept running; nothing was recorded as
  lost because the engine had waited for the still-connected client.
* `04b-backend-lost-sync-unit.log` — the isolated proof of the `+=` fix, running
  the production `Session._loop` against a fake world that reports
  `fixed_delta_seconds = None` after 6 ticks: the old expression raises
  `TypeError: unsupported operand type(s) for +=: 'float' and 'NoneType'` (the
  review's `state=error`), while the loop stays `preview`, advances `sim_time`,
  records `sync_mode_lost`, re-asserts the settings once and records
  `sync_mode_restored`; all six CHECKs PASS.

## 05 — a real sync-mode loss, the chips, and the recovery

* `05-before-sync-loss.png` — before: `会话 · 预览中`, `同步模式 · 3 Hz` (the
  backend's real preview delta; the old constant was a hard-coded `20 Hz`).
* `05-thief.log` — a second CARLA client applying asynchronous settings every
  0.4 s for 40 s: a genuine way for the session to lose its clock.
* `05-sync-lost-*.png` / `05-watcher.log` / `05-sync-lost-polls.csv` — captured
  at the moment `GET /sessions/current` reported `sync_lost=true`:
  `会话 · 预览中` + `同步模式 · 已失去` (warn colour), `state=preview` — the
  session survived the loss, it did not become `error`.
* `05-session-after-sync-restored.json` — after the thief stops: `sync=True`,
  `fixed_delta=0.33333`, `sync_lost=false`, `sync_reasserts=44`, i.e. the backend
  re-asserted the intended settings.
* `05-after-sync-restored.png` — the chip back to `同步模式 · 3 Hz`.

## scripts/

The probes, so the run is repeatable: `probe_live_pause.py` (tick once, then
pause while sampling the server's own settings), `steal_sync.py` (apply
asynchronous settings from a second client), `suspend_python.ps1`
(`NtSuspendProcess`), `watch_sync.ps1` + `capture_panel.ps1` (poll and capture
the client area with `PrintWindow`), `unit_lost_sync.py` (the isolated
`fixed_delta_seconds = None` test).

## Not covered here

* The full editor log for the suspend test was ~215 KB of CARLA content noise and
  is trimmed to the relevant lines; the raw file stays in `Saved/Logs/`.
* Only `CarlaUnrealEditor Win64 Development` was built and run. The game target
  (`CarlaUnreal.exe`) and the CMake/packaged builds were not rebuilt; the
  behaviour is in the same `CarlaEngine.cpp`, but that is unverified here.
* Non-Windows builds: the client-liveness query is Windows-only by design and
  returns `Unknown` there, so only the timeout decides (the documented trade-off).

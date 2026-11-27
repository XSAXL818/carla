# F1 (first load window loses its forced probe) — verification evidence

Date: 2026-11-28 00:23–01:20 (+08:00)
Project: `C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal` (UE 5.8 CARLA fork)
Subject: the uncommitted `bCloseConnection` change in
`Source/TestPlatform/Private/TestPlatformApi.cpp` / `Public/TestPlatformApi.h`.

**Verdict: NOT COMMITTED.** The change is harmless but its stated root cause is
falsified by measurement, and the defect it claims to fix does not reproduce with
the header removed. See the parent report for the full reasoning.

## Build under test

`Build.bat CarlaUnrealEditor Win64 Development ...` — real compile of
`TestPlatformApi.cpp` (`[1/4] Compile [x64] TestPlatformApi.cpp`, then link).
`Binaries/Win64/UnrealEditor-TestPlatform.dll` 687,616 B, 2026-11-28 01:11:32.
The same target was also built once with the call-site argument flipped to
`false` (control build, DLL 00:59:18) and then restored and rebuilt (01:11:32).

## Primary run: one panel session, two load windows (`stub-requests-two-windows.log`)

```
01:18:41.612 cport=57011 GET /health
01:18:41.613 cport=57011 GET /health -> 200 connected=True port=2005 delay=0.0
01:18:42.427 cport=57278 POST /sessions/preview_map <- body 575B read conn=close
01:18:42.433 cport=57278 POST /sessions/preview_map -> window OPEN state=preview_loading hold=6.0s
01:18:42.611 cport=57283 GET /sessions/current
...   (only /sessions/current and vehicle images until the close)
01:18:47.937 cport=57278 POST /sessions/preview_map state->preview (flip), response in 0.5s
01:18:48.449 cport=57278 POST /sessions/preview_map -> 200 state=preview (window CLOSED)
01:18:48.492 cport=57272 GET /health?force=true
01:18:48.493 cport=57272 GET /health?force=true -> 200 connected=True port=2005 delay=0.0
...
01:18:50.546 cport=57273 POST /sessions/preview_map <- body 575B read conn=close
01:18:50.548 cport=57273 POST /sessions/preview_map -> window OPEN state=preview_loading hold=6.0s
...   (only /sessions/current until the close)
01:18:56.052 cport=57273 POST /sessions/preview_map state->preview (flip), response in 0.5s
01:18:56.565 cport=57273 POST /sessions/preview_map -> 200 state=preview (window CLOSED)
01:18:56.599 cport=57272 GET /health?force=true
01:18:56.601 cport=57272 GET /health?force=true -> 200 connected=True port=2005 delay=0.0
```

- Window 1 = the session's **first** window (startup auto-load), 42.433 → 48.449.
- Zero `/health` requests inside either window (gap 42.433–48.449 and 50.548–56.565).
- `?force=true` after **each** close: 43 ms (first window) and 34 ms (second).
- Both forced probes went out on client port 57272, not on the POST's port
  (57278 / 57273) — `Connection: close` was sent on both POSTs (`conn=close`).

## Files

| File | What it is |
|---|---|
| `stub-requests-two-windows.log` | the run above (two windows, one session) |
| `two-01-window1-open-topbar.png` | during window 1: `服务器 127.0.0.1 : 2005 · 未知`, `会话 · 未开始`, button `处理中…` |
| `two-02-window1-closed-forced-topbar.png` | after window 1: `服务器 127.0.0.1 : 2005 · 已连接`, `会话 · 预览中` |
| `two-03-window2-open-topbar.png` | during window 2: `服务器 127.0.0.1 : 2005 · 未知` |
| `two-04-window2-closed-forced-topbar.png` | after window 2: `已连接` |
| `01-window1-open-noanswer-topbar.png` | earlier session, first `/health` still in flight: `服务器 — · 未知` |
| `03-stale-answer-dropped-topbar.png` | a stale `connected:false` answer landed: still `已连接` (dropped by epoch) |
| `04-window2-open-topbar.png`, `05-window2-closed-forced-topbar.png` | earlier session, clicked window, before/after |
| `06-disconnected-topbar.png` | genuine disconnect: `未连接` |
| `panel-desync-*.png` | control build (header off) against the desync stub |
| `real-backend-raw-socket.txt` | raw HTTP/1.1 to the shipped backend: body POST then GET on the same socket |
| `desync-stub-same-connection.txt` | raw HTTP/1.1 to a stub that answers the POST without draining it |
| `editor-log-http-two-windows.txt` | editor `LogHttp`/`LogTestPlatform` lines of the primary run |
| `stub-requests.log`, `final-*` | an earlier fixed-build run (one window); kept for comparison |

The top bar was captured with `PrintWindow(hwnd, hdc, 3)` (client-only,
1600x900); the top-bar crops are 2x nearest-neighbour.

## Control build (header off) against the desync stub (01:07–01:08)

```
01:08:39.166 cport=65152 POST /sessions/preview_map <- body NOT read (575 bytes left in socket) conn=keep-alive
01:08:39.172 cport=65152 POST /sessions/preview_map -> window OPEN state=preview_loading hold=4.0s
01:08:42.683 cport=65152 POST /sessions/preview_map state->preview (flip), response in 0.5s
01:08:43.193 cport=65152 POST /sessions/preview_map -> 200 state=preview (window CLOSED)
01:08:43.219 cport=65147 GET /health?force=true
01:08:43.222 cport=65147 GET /health?force=true -> 200 connected=True port=2005 delay=0.0
```

With the header absent and the server leaving the POST body unread, the forced
probe was still issued (26 ms after the close) and answered 200 — libcurl sent it
on the polling connection (65147), not on the POST connection (65152). The 501
was not reproducible on the client path.

## Shipped backend, raw sockets

`real-backend-raw-socket.txt`: a body-carrying `POST /sessions/preview_map`
answered in 6 ms, and the immediately following `GET /health?force=true` on the
same keep-alive socket answered `200 OK`. The shipped backend drains the body.

`desync-stub-same-connection.txt`: with a server that answers the POST without
reading its body, the next request on that socket is parsed as
`{"map": "Town01"}GET /health?force=true HTTP/1.1` → `400 Bad request syntax`.
The desync failure mode is real, but only against such a server, and the client
does not put the forced probe on that connection in these runs.

## Environment note

The panel's startup includes an engine stall longer than 12 s on this machine;
the five catalog GETs issued at UI creation then time out
(`LogHttp: Warning: HTTP request timed out after 12.00 seconds URL=.../maps`),
so the auto-load never starts. A click on `载入预设` (top bar, client
1455,25) re-issues `LoadCatalog` after the stall and the panel then loads
normally; the primary run above was driven that way. One panel launch crashed
during that stall with an engine assert in
`UE::FInheritedContextExtensionData::DestroyAndFree` reached from
`FCurlHttpResponse::Release` / `FCurlHttpConnectionPool::ThreadLoop`
(DevHttp), i.e. a pre-existing engine/HTTP-thread issue, unrelated to this diff.

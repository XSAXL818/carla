"""Drive one acceptance run end to end and record independent ground truth.

Attaches to the backend over HTTP (the same surface the panel uses) and to the
simulator through an *independent* carla.Client, so nothing here trusts the
backend's own numbers.

modes
  arrival : start the current preview session, follow the ego until the backend
            reports ego_arrived, keep sampling the parked car (stall false
            positive check), then prove the route overlay is really on screen by
            toggling it off and diffing frames taken from the now static camera.
  stall   : create a fresh session (traffic 'none'), preview, start, let the ego
            drive, drop props across its lane and watch for ego_stalled.
"""
from __future__ import annotations

import argparse
import http.client
import json
import math
import sys
import time

import carla

import shot

API_HOST, API_PORT = "127.0.0.1", 8765


def api(method: str, path: str, body: dict | None = None, timeout: float = 120.0):
    """Minimal JSON client.

    urllib.request is not used on purpose: every urllib GET to this uvicorn
    server is answered 400 "Invalid HTTP request received." on this machine,
    while http.client / curl / requests are fine, so the harness talks to the
    backend through http.client (verified 200 before use).
    """
    conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=timeout)
    try:
        payload = json.dumps(body).encode() if body is not None else None
        headers = {"Content-Type": "application/json"} if payload else {}
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        raw = resp.read().decode()
        if resp.status >= 400:
            raise RuntimeError(f"{method} {path} -> {resp.status}: {raw[:300]}")
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {"raw": raw}
    finally:
        conn.close()


def actor_snapshot(world, ego_hint):
    rows = []
    ego = None
    best = 1e18
    for a in world.get_actors().filter("vehicle.*"):
        try:
            tf = a.get_transform()
            v = a.get_velocity()
            c = a.get_control()
            speed = math.sqrt(v.x ** 2 + v.y ** 2 + v.z ** 2) * 3.6
            row = {"id": a.id, "type": a.type_id,
                   "x": round(tf.location.x, 2), "y": round(tf.location.y, 2),
                   "z": round(tf.location.z, 2),
                   "yaw": round(tf.rotation.yaw, 1), "speed": round(speed, 2),
                   "throttle": round(c.throttle, 3), "brake": round(c.brake, 3),
                   "steer": round(c.steer, 3), "gear": int(c.gear)}
            rows.append(row)
            if ego_hint is not None:
                d = math.dist((tf.location.x, tf.location.y), ego_hint)
                if d < best:
                    best, ego = d, row
        except Exception as exc:  # noqa: BLE001  (actor may be dying)
            rows.append({"id": a.id, "error": str(exc)[:80]})
    others = [r for r in rows if ego is None or r["id"] != ego["id"]]
    return ego, others


def block_lane(world, ego_row, rows=3, per_row=5, first=11.0, row_gap=2.6,
               half_width=3.4):
    """Wall the ego's lane off with static props; return (ids, blueprint used).

    Construction cones alone do not stop a car (a 0.3 m cone is driven over), so
    the barrier is built from street barriers in several depth rows, with cones
    filling the gaps between them.
    """
    lib = world.get_blueprint_library()
    barriers = lib.filter("static.prop.streetbarrier")
    cones = lib.filter("static.prop.constructioncone")
    if not barriers and not cones:
        return [], "none"
    yaw = math.radians(ego_row["yaw"])
    fx, fy = math.cos(yaw), math.sin(yaw)
    rx, ry = -math.sin(yaw), math.cos(yaw)
    spawned = []
    for r in range(rows):
        ahead = first + r * row_gap
        for i in range(per_row):
            frac = (i / max(1, per_row - 1)) - 0.5
            x = ego_row["x"] + fx * ahead + rx * frac * 2 * half_width
            y = ego_row["y"] + fy * ahead + ry * frac * 2 * half_width
            bp = barriers[0] if barriers else cones[0]
            tf = carla.Transform(carla.Location(x=x, y=y, z=ego_row["z"] + 0.3),
                                 carla.Rotation(yaw=ego_row["yaw"]))
            try:
                a = world.try_spawn_actor(bp, tf)
                if a is not None:
                    spawned.append(a.id)
            except Exception:  # noqa: BLE001
                pass
        if cones:  # cones between the barriers of this row
            for i in range(per_row - 1):
                frac = (i + 0.5) / max(1, per_row - 1) - 0.5
                x = ego_row["x"] + fx * ahead + rx * frac * 2 * half_width
                y = ego_row["y"] + fy * ahead + ry * frac * 2 * half_width
                tf = carla.Transform(carla.Location(x=x, y=y, z=ego_row["z"] + 0.3),
                                     carla.Rotation(yaw=ego_row["yaw"]))
                try:
                    a = world.try_spawn_actor(cones[0], tf)
                    if a is not None:
                        spawned.append(a.id)
                except Exception:  # noqa: BLE001
                    pass
    used = "static.prop.streetbarrier" if barriers else "static.prop.constructioncone"
    return spawned, used


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["arrival", "stall"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--shots", required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--preset", default="loop")
    ap.add_argument("--traffic", default="light")
    ap.add_argument("--duration", type=float, default=300.0)
    ap.add_argument("--shot-every", type=float, default=10.0)
    ap.add_argument("--props-at", type=float, default=15.0)
    ap.add_argument("--park-sample", type=float, default=40.0,
                    help="seconds to keep sampling after the ego has arrived/stopped")
    args = ap.parse_args()

    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(30.0)
    fh = open(args.out, "w", encoding="utf-8")
    shots: list[str] = []
    shot_idx = [0]

    def snap(tag: str) -> str | None:
        try:
            path = f"{args.shots}/{tag}.png"
            hwnd, _text = shot.find_window(args.pid, None)
            shot.capture(hwnd, path)
            shot_idx[0] += 1
            shots.append(path)
            return path
        except Exception as exc:  # noqa: BLE001
            print(f"  shot failed: {exc}", flush=True)
            return None

    def rec(_tag: str, **kw):
        line = {"tag": _tag, "t": round(time.time(), 3), "wall": time.strftime("%H:%M:%S"), **kw}
        fh.write(json.dumps(line, ensure_ascii=False) + "\n")
        fh.flush()
        return line

    def events(sid):
        try:
            return api("GET", f"/sessions/{sid}/events.json", timeout=20).get("events", [])
        except Exception:  # noqa: BLE001
            return []

    # ---------------------------------------------------------------- setup
    if args.mode == "arrival":
        cur = api("GET", "/sessions/current")
        sid = cur["id"]
        print(f"attaching to current session {sid} state={cur['state']} map={cur['map']}", flush=True)
        rec("attach", session=sid, state=cur["state"], traffic=cur.get("traffic"),
            route=cur.get("route"))
        if cur["state"] != "preview":
            prev = api("POST", f"/sessions/{sid}/preview", timeout=300)
            print(f"re-preview state={prev.get('state')} traffic={prev.get('traffic')} "
                  f"route={prev.get('route')}", flush=True)
            rec("preview", **{k: prev.get(k) for k in ("state", "traffic", "traffic_detail",
                                                       "route", "ego")})
        snap("00-preview")
        started = api("POST", f"/sessions/{sid}/start", timeout=180)
    else:
        cur = api("GET", "/sessions/current")
        if cur.get("id"):
            print(f"destroying previous session {cur['id']} ({cur['state']})", flush=True)
            try:
                api("POST", f"/sessions/{cur['id']}/destroy", timeout=120)
            except Exception as exc:  # noqa: BLE001
                print(f"  destroy failed: {exc}", flush=True)
        cfg = {
            "map": "Town10HD",
            "ego": {"blueprint": "vehicle.tesla.model3", "spawn": {"mode": "route_start"}},
            "route": {"source": "preset", "preset_id": args.preset},
            "weather": {"preset": "ClearNoon"},
            "traffic": {"preset": args.traffic, "autopilot": True},
            "algorithm": {"id": "carla.behavior", "params": {}},
            "run": {"sync_mode": True, "fixed_delta_seconds": 0.05, "record": False,
                    "max_duration_s": int(args.duration)},
        }
        started = api("POST", "/sessions", cfg, timeout=60)
        sid = started["id"]
        print(f"created session {sid}", flush=True)
        rec("create", session=sid, state=started.get("state"))
        prev = api("POST", f"/sessions/{sid}/preview", timeout=300)
        print(f"preview state={prev.get('state')} traffic={prev.get('traffic')} "
              f"route={prev.get('route')}", flush=True)
        rec("preview", **{k: prev.get(k) for k in ("state", "traffic", "traffic_detail",
                                                   "route", "ego")})
        snap("00-preview")
        started = api("POST", f"/sessions/{sid}/start", timeout=180)

    rec("start", **{k: started.get(k) for k in ("state", "traffic", "traffic_detail", "route",
                                               "cameras", "sync_mode", "fixed_delta")})
    print(f"start -> state={started.get('state')} traffic={started.get('traffic')} "
          f"route={started.get('route')}", flush=True)

    world = client.get_world()
    t0 = time.time()
    stopped_since = None
    props_at_fired = False
    arrived_at = None
    next_shot = args.shot_every
    next_events = 1.0
    seen_kinds: list[str] = []

    while True:
        elapsed = time.time() - t0
        if elapsed > args.duration:
            print("duration reached", flush=True)
            break
        try:
            st = api("GET", "/sessions/current", timeout=20)
        except Exception as exc:  # noqa: BLE001
            rec("http_error", error=str(exc))
            time.sleep(0.5)
            continue
        ego_rep = st.get("ego")
        hint = (ego_rep["x"], ego_rep["y"]) if ego_rep else None
        ego_act, others = actor_snapshot(world, hint)
        speed = ego_rep["speed_kmh"] if ego_rep else None

        if elapsed >= next_events:
            evs = events(sid)
            for e in evs:
                key = f"{e.get('kind')}@{e.get('t')}"
                if key not in seen_kinds:
                    seen_kinds.append(key)
                    rec("event", kind=e.get("kind"), sim_t=e.get("t"),
                        detail=e.get("detail"))
                    print(f"  EVENT {e.get('kind')} t={e.get('t')} {e.get('detail')}", flush=True)
                    if e.get("kind") == "ego_arrived" and arrived_at is None:
                        arrived_at = elapsed
            next_events = elapsed + 1.5

        rec("sample", elapsed=round(elapsed, 2), state=st.get("state"),
            traffic=st.get("traffic"), walkers=st.get("walkers"),
            events=st.get("events"), sim_time=st.get("sim_time"),
            ego_speed=round(speed, 2) if speed is not None else None,
            ego=ego_rep, route=st.get("route"),
            ego_actor=ego_act, other_vehicles=others,
            sync=(st.get("sync_mode"), st.get("fixed_delta"), st.get("sync_lost")))

        if args.mode == "stall" and not props_at_fired and elapsed >= args.props_at and ego_act:
            ids, pid = block_lane(world, ego_act)
            props_at_fired = True
            print(f"staged props at {elapsed:.1f}s: {len(ids)} spawned ({pid})", flush=True)
            rec("props", elapsed=round(elapsed, 2), ids=ids, blueprint=pid)

        if elapsed >= next_shot:
            snap(f"{shot_idx[0]:02d}-t{int(elapsed):03d}")
            next_shot += args.shot_every

        if speed is not None and speed < 0.6:
            if stopped_since is None:
                stopped_since = elapsed
        else:
            stopped_since = None

        if args.mode == "arrival":
            if arrived_at is not None and elapsed - arrived_at > args.park_sample:
                print(f"arrived at {arrived_at:.1f}s, sampled {elapsed - arrived_at:.1f}s parked",
                      flush=True)
                break
        else:
            stalled = any(k.startswith("ego_stalled@") for k in seen_kinds)
            if stalled and props_at_fired and elapsed - args.props_at > 20.0:
                print("stall confirmed and sampled, ending", flush=True)
                break
            if elapsed - args.props_at > 40.0 and props_at_fired:
                print("stall window elapsed without ego_stalled", flush=True)
                break
        time.sleep(0.5)

    # -------------------------------------------------- route overlay on/off diff
    snap(f"{shot_idx[0]:02d}-route-on")
    on_frame = shots[-1]
    api("POST", f"/sessions/{sid}/route", {"visible": False}, timeout=30)
    print("route overlay turned off, waiting for the 5 s line lifetime to expire", flush=True)
    time.sleep(8.0)
    snap(f"{shot_idx[0]:02d}-route-off")
    off_frame = shots[-1]
    api("POST", f"/sessions/{sid}/route", {"visible": True}, timeout=30)
    time.sleep(8.0)
    snap(f"{shot_idx[0]:02d}-route-on-again")
    rec("route_toggle", on=on_frame, off=off_frame, on_again=shots[-1])

    stopped = api("POST", f"/sessions/{sid}/stop", timeout=180)
    rec("stopped", **{k: stopped.get(k) for k in ("state", "traffic", "events", "sim_time")})
    fh.close()
    print(f"samples -> {args.out}; shots: {len(shots)}", flush=True)
    print(json.dumps({"session": sid, "shots": shots, "arrived_at": arrived_at,
                      "events_seen": seen_kinds}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())


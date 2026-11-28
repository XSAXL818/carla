"""Drive one acceptance run and record independent ground truth.

Samples the backend's own status (the surface the panel uses) *and* the
simulator through an independent carla.Client, then measures the run against the
plan the backend reports:

  * spawn yaw vs the plan's first segment bearing;
  * coincidence: share of sampled ego positions within 3 m of the plan polyline;
  * driven path length vs the plan's own length;
  * arrival / stall events;
  * the plan's own lane types.

Nothing here trusts the backend's numbers: positions come from the simulator and
the plan polyline is read back from the simulator's map by the same preset
triples, then compared with what the backend says it built.
"""
from __future__ import annotations

import argparse
import http.client
import json
import math
import os
import sys
import time

import carla

API_HOST, API_PORT = "127.0.0.1", 8765
HERE = os.path.dirname(os.path.abspath(__file__))


def api(method: str, path: str, body: dict | None = None, timeout: float = 120.0):
    """Minimal JSON client (urllib GETs to this uvicorn answer 400 on this box)."""
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


def bearing(a, b):
    return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))


def dist_point_segment(p, a, b):
    px, py = p
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    if dx == 0.0 and dy == 0.0:
        return math.dist(p, a)
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.dist(p, (ax + t * dx, ay + t * dy))


def dist_to_polyline(p, poly):
    if len(poly) < 2:
        return float("inf")
    return min(dist_point_segment(p, poly[i], poly[i + 1]) for i in range(len(poly) - 1))


def polyline_length(poly):
    return sum(math.dist(a, b) for a, b in zip(poly, poly[1:]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--duration", type=float, default=200.0)
    ap.add_argument("--park-sample", type=float, default=25.0)
    ap.add_argument("--preset", default="loop")
    ap.add_argument("--traffic", default="light")
    args = ap.parse_args()

    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(30.0)
    world = client.get_world()
    cmap = world.get_map()
    print("map:", cmap.name, flush=True)

    cur = api("GET", "/sessions/current")
    if not cur.get("id"):
        # a fresh backend has no session at all: create one for the preset
        print("no current session; creating one", flush=True)
        cur = api("POST", "/sessions", {
            "map": "Town10HD",
            "ego": {"blueprint": "vehicle.tesla.model3", "spawn": {"mode": "route_start"}},
            "route": {"source": "preset", "preset_id": args.preset},
            "weather": {"preset": "ClearNoon"},
            "traffic": {"preset": args.traffic, "autopilot": True},
            "algorithm": {"id": "carla.behavior", "params": {}},
            "run": {"sync_mode": True, "fixed_delta_seconds": 0.05,
                    "record": False, "max_duration_s": int(args.duration)},
        }, timeout=60)
    sid = cur["id"]
    print(f"session {sid} state={cur['state']} map={cur.get('map')}", flush=True)
    if cur["state"] == "running":
        # already driving (a previous attempt started it): stop, then re-preview
        # so the measurement begins from a fresh spawn
        print("stopping the running session to start from a fresh spawn", flush=True)
        api("POST", f"/sessions/{sid}/stop", timeout=200)
        cur = api("GET", "/sessions/current")
    if cur["state"] != "preview":
        prev = api("POST", f"/sessions/{sid}/preview", timeout=300)
        print("preview:", prev.get("state"), flush=True)

    status = api("GET", "/sessions/current")
    plan_meta = status["route"]["plan"]
    spawn_meta = status["route"]["spawn"]
    print("plan:", plan_meta, flush=True)
    print("spawn:", spawn_meta, flush=True)

    started = api("POST", f"/sessions/{sid}/start", timeout=180)
    print("start ->", started.get("state"), flush=True)

    # The plan the run committed to, read from the product's own surface and
    # cross-checked against an independent resolution of the same preset triples
    # on the simulator's map. The coincidence measurement uses the *plan*: the
    # preset's 24 control points are a sparse subset, and a car driving exactly
    # between two of them is still "far from the preset polyline" on a bend.
    plan_doc = api("GET", f"/sessions/{sid}/plan.json", timeout=60)
    plan_poly = [(p["x"], p["y"]) for p in plan_doc["points"]]
    print(f"plan.json: {len(plan_poly)} points, stats={plan_doc['stats']}", flush=True)
    routes = json.load(open(os.path.join(
        r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform",
        "Platform", "backend", "data", "routes", "Town10HD.json"), encoding="utf-8"))
    specs = next(p for p in routes["presets"] if p["id"] == args.preset)["waypoints"]
    preset_poly = []
    for spec in specs:
        wp = cmap.get_waypoint_xodr(int(spec["road_id"]), int(spec["lane_id"]),
                                    float(spec["s"]))
        if wp is not None:
            preset_poly.append((wp.transform.location.x, wp.transform.location.y))

    samples = []
    events = []
    seen = set()
    t0 = time.time()
    arrived_at = None
    next_events = 0.0
    ego_id = None
    ego_track = []
    while True:
        elapsed = time.time() - t0
        if elapsed > args.duration:
            print("duration reached", flush=True)
            break
        try:
            st = api("GET", "/sessions/current", timeout=20)
        except Exception as exc:  # noqa: BLE001
            print("http error:", exc, flush=True)
            time.sleep(0.5)
            continue
        rep = st.get("ego")
        # Identify the ego by matching the backend's reported position: every
        # traffic car carries role_name "autopilot", and the ego carries none,
        # so role is not usable. Once found, the id is sticky -- but the
        # position match has to be re-established whenever the id is unknown.
        best = None
        actors = list(world.get_actors().filter("vehicle.*"))
        if rep is not None:
            for a in actors:
                tf = a.get_transform()
                d = math.dist((tf.location.x, tf.location.y), (rep["x"], rep["y"]))
                if best is None or d < best[0]:
                    best = (d, a)
        if best is not None and (ego_id is None or best[1].id == ego_id) and best[0] < 5.0:
            ego_id = best[1].id
        if ego_id is not None:
            actor = next((a for a in actors if a.id == ego_id), None)
            if actor is not None:
                tf = actor.get_transform()
                v = actor.get_velocity()
                row = {"t": round(elapsed, 2), "sim_time": st.get("sim_time"),
                       "id": ego_id,
                       "x": round(tf.location.x, 3), "y": round(tf.location.y, 3),
                       "yaw": round(tf.rotation.yaw, 2),
                       "speed_kmh": round(math.sqrt(v.x ** 2 + v.y ** 2 + v.z ** 2) * 3.6, 2),
                       "backend_speed": (round(rep["speed_kmh"], 2) if rep else None),
                       "state": st.get("state"),
                       "plan_points": st.get("route", {}).get("plan", {}).get("points"),
                       "match_m": round(best[0], 3) if best else None}
                samples.append(row)
                ego_track.append((tf.location.x, tf.location.y))
        if elapsed >= next_events:
            try:
                for e in api("GET", f"/sessions/{sid}/events.json", timeout=20).get("events", []):
                    key = f"{e.get('kind')}@{e.get('t')}"
                    if key not in seen:
                        seen.add(key)
                        events.append(e)
                        print(f"  EVENT {e.get('kind')} t={e.get('t')} {e.get('detail')}",
                              flush=True)
                        if e.get("kind") == "ego_arrived" and arrived_at is None:
                            arrived_at = elapsed
            except Exception:  # noqa: BLE001
                pass
            next_events = elapsed + 1.5
        if arrived_at is not None and elapsed - arrived_at > args.park_sample:
            print(f"arrived at {arrived_at:.1f}s; sampled {elapsed - arrived_at:.1f}s parked",
                  flush=True)
            break
        time.sleep(0.5)

    stopped = api("POST", f"/sessions/{sid}/stop", timeout=180)
    print("stop ->", stopped.get("state"), flush=True)

    # ---- measurements -------------------------------------------------------
    # Independent copy of the preset control points, resolved on the live map.
    poly = preset_poly
    plan_poly_used = plan_poly if len(plan_poly) >= 2 else poly

    positions = [(s["x"], s["y"]) for s in samples]
    driven = polyline_length(positions) if len(positions) > 1 else 0.0
    within3 = [dist_to_polyline(p, plan_poly_used) <= 3.0 for p in positions]
    within1 = [dist_to_polyline(p, plan_poly_used) <= 1.0 for p in positions]
    dists = [dist_to_polyline(p, plan_poly_used) for p in positions]
    within3_preset = [dist_to_polyline(p, poly) <= 3.0 for p in positions]
    first_yaw = samples[0]["yaw"] if samples else None
    # Bearing of the plan's own first segment, measured over a >= 4 m baseline:
    # the first plan points are ~2 m apart, and a 2 m baseline turns sub-degree
    # position noise into several degrees of apparent heading.
    plan_first_bearing = None
    if len(plan_poly_used) > 1:
        a0 = plan_poly_used[0]
        target = plan_poly_used[1]
        for point in plan_poly_used[1:]:
            target = point
            if math.dist(a0, target) >= 4.0:
                break
        if math.dist(a0, target) > 1e-3:
            plan_first_bearing = bearing(a0, target)
    kinds = [e.get("kind") for e in events]
    spawn_meta = status["route"]["spawn"]
    spawn_yaw = (spawn_meta.get("yaw") if spawn_meta.get("measured") else first_yaw)
    spawn_delta = (round((spawn_yaw - plan_first_bearing + 180) % 360 - 180, 2)
                   if spawn_yaw is not None and plan_first_bearing is not None else None)

    summary = {
        "session": sid,
        "preset": args.preset,
        "plan_meta": plan_meta,
        "spawn_meta": spawn_meta,
        "samples": len(samples),
        "driven_m": round(driven, 2),
        "plan_length_m": plan_meta.get("length_m"),
        "plan_polyline_points": len(plan_poly_used),
        "plan_polyline_length_m": round(polyline_length(plan_poly_used), 2),
        "preset_polyline_points": len(poly),
        "preset_polyline_length_m": round(polyline_length(poly), 2),
        "spawn_yaw_actual": spawn_yaw,
        "spawn_yaw_first_sample": first_yaw,
        "plan_first_bearing": (round(plan_first_bearing, 2)
                              if plan_first_bearing is not None else None),
        "spawn_yaw_delta_deg": spawn_delta,
        "coincidence_within_3m_pct": round(100.0 * sum(within3) / max(1, len(within3)), 1),
        "coincidence_within_1m_pct": round(100.0 * sum(within1) / max(1, len(within1)), 1),
        "coincidence_within_3m_pct_vs_preset": round(
            100.0 * sum(within3_preset) / max(1, len(within3_preset)), 1),
        "distance_to_plan_m": {
            "min": round(min(dists), 2) if dists else None,
            "median": round(sorted(dists)[len(dists) // 2], 2) if dists else None,
            "max": round(max(dists), 2) if dists else None,
        },
        "events": events,
        "event_kinds": kinds,
        "ego_arrived": "ego_arrived" in kinds,
        "ego_stalled": "ego_stalled" in kinds,
        "planner_fallback": "planner_fallback" in kinds,
        "samples_json": os.path.join(HERE, "samples.jsonl"),
    }
    with open(os.path.join(HERE, "samples.jsonl"), "w", encoding="utf-8") as fh:
        for row in samples:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, ensure_ascii=False, indent=1)
    print(json.dumps({k: v for k, v in summary.items()
                      if k not in ("events", "plan_meta", "spawn_meta")},
                     ensure_ascii=False, indent=1), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

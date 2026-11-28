"""Per-segment diagnosis of trace_route between consecutive preset waypoints."""
from __future__ import annotations

import json
import math
import sys

import carla

sys.path.insert(0, r"C:\GameProject\CarlaUE5\carla\PythonAPI\carla")
from agents.navigation.global_route_planner import GlobalRoutePlanner  # noqa: E402

ROUTES = (sys.argv[2] if len(sys.argv) > 2 else
          r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
          r"\Platform\backend\data\routes\Town10HD.json")


def resolve(cmap, spec, prev=None, nxt=None):
    road_id, lane_id, s = int(spec["road_id"]), int(spec["lane_id"]), float(spec["s"])
    if lane_id not in (None, 0, -1):
        wp = cmap.get_waypoint_xodr(road_id, lane_id, s)
        if wp is not None and wp.lane_type == carla.LaneType.Driving:
            return wp
    candidates = []
    for lane in (1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6):
        wp = cmap.get_waypoint_xodr(road_id, lane, s)
        if wp is not None and wp.lane_type == carla.LaneType.Driving:
            candidates.append(wp)
    if not candidates:
        return None
    ref = None
    for other in (nxt, prev):
        if other is not None and int(other["road_id"]) == road_id:
            ref = (float(other["s"]) - s) if other is nxt else (s - float(other["s"]))
            break
    if ref is None:
        return candidates[0]
    want_neg = ref > 0
    for wp in candidates:
        if (wp.lane_id < 0) == want_neg:
            return wp
    return candidates[0]


def length(plan):
    return sum(a[0].transform.location.distance(b[0].transform.location)
               for a, b in zip(plan, plan[1:]))


def main():
    preset_id = sys.argv[1] if len(sys.argv) > 1 else "loop"
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    grp = GlobalRoutePlanner(cmap, 2.0)
    data = json.load(open(ROUTES, encoding="utf-8"))
    specs = next(p for p in data["presets"] if p["id"] == preset_id)["waypoints"]
    wps = [resolve(cmap, s, specs[i - 1] if i else None,
                   specs[i + 1] if i + 1 < len(specs) else None)
           for i, s in enumerate(specs)]
    print(f"{preset_id}: {len(wps)} waypoints")
    total = 0.0
    for i, (a, b) in enumerate(zip(wps, wps[1:])):
        if a is None or b is None:
            print(f"  seg {i:>2}: unresolved")
            continue
        straight = a.transform.location.distance(b.transform.location)
        trace = grp.trace_route(a.transform.location, b.transform.location)
        seg_len = length(trace)
        total += seg_len
        flag = ""
        if seg_len > straight * 2.5 + 30:
            flag = "  <-- DETOUR"
        print(f"  seg {i:>2}: {a.road_id}/{a.lane_id}@{a.s:.1f} -> "
              f"{b.road_id}/{b.lane_id}@{b.s:.1f} | straight {straight:7.1f} m | "
              f"trace {seg_len:8.1f} m | {len(trace):4d} pts{flag}")
    print(f"total traced: {total:.1f} m")


if __name__ == "__main__":
    main()

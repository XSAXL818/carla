"""Prototype: build a [(Waypoint, RoadOption)] plan from a preset, live map.

Not part of the product -- this is the experiment that decides the algorithm the
backend will use, so the backend can be written against measured behaviour
instead of hope.

Algorithm under test
  1. resolve every preset waypoint (road_id, lane_id, s) onto a *driving* lane
     whose side matches the direction of travel implied by the preset;
  2. trace_route between consecutive preset waypoints, not start->end, so the
     plan stays on the preset's roads;
  3. concatenate, drop consecutive duplicates and lane-change noise;
  4. derive RoadOption from the geometry of the plan itself.

Reports: plan length, gap sizes (non-continuous hops), how close the plan passes
to each preset waypoint, and the distribution of RoadOption values.
"""
from __future__ import annotations

import json
import math
import sys

import carla

sys.path.insert(
    0, r"C:\GameProject\CarlaUE5\carla\PythonAPI\carla")
from agents.navigation.global_route_planner import GlobalRoutePlanner  # noqa: E402
from agents.navigation.local_planner import RoadOption  # noqa: E402

ROUTES = (r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
          r"\Platform\backend\data\routes\Town10HD.json")


def bearing(a, b):
    return math.degrees(math.atan2(b.y - a.y, b.x - a.x)) % 360.0


def resolve(cmap, spec, prev=None, nxt=None):
    """(road, lane, s) -> Waypoint on a Driving lane, direction-aware.

    ``lane_id`` in the preset is authoritative when it is a driving lane. The
    -1 sentinel means "any driving lane": pick the one whose side matches the
    direction the preset travels (previous/next waypoint), which is what stops
    the ego from being spawned on the opposite carriageway.
    """
    road_id, lane_id, s = int(spec["road_id"]), int(spec["lane_id"]), float(spec["s"])
    if lane_id not in (None, 0, -1):
        wp = cmap.get_waypoint_xodr(road_id, lane_id, s)
        if wp is not None and wp.lane_type == carla.LaneType.Driving:
            return wp
    # probe both sides, keep Driving ones, prefer the side matching travel dir
    candidates = []
    for lane in (1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6):
        wp = cmap.get_waypoint_xodr(road_id, lane, s)
        if wp is not None and wp.lane_type == carla.LaneType.Driving:
            candidates.append(wp)
    if not candidates:
        return None
    if prev is None and nxt is None:
        return candidates[0]
    # travel direction along the road: sign of (s_next - s_prev)
    ref = None
    for other in (nxt, prev):
        if other is not None and int(other["road_id"]) == road_id:
            ref = float(other["s"]) - s if other is nxt else s - float(other["s"])
            break
    if ref is None:
        return candidates[0]
    want_neg_lane = ref > 0  # right-hand traffic: id < 0 travels +s
    for wp in candidates:
        if (wp.lane_id < 0) == want_neg_lane:
            return wp
    return candidates[0]


def build_plan(cmap, grp, specs, sampling=2.0):
    waypoints = []
    for i, spec in enumerate(specs):
        prev = specs[i - 1] if i else None
        nxt = specs[i + 1] if i + 1 < len(specs) else None
        wp = resolve(cmap, spec, prev, nxt)
        waypoints.append(wp)
        if wp is None:
            print(f"  !! preset waypoint {i} {spec} did not resolve")
    raw = []
    for a, b in zip(waypoints, waypoints[1:]):
        if a is None or b is None:
            continue
        trace = grp.trace_route(a.transform.location, b.transform.location)
        if not trace:
            print(f"  !! trace_route empty {a.road_id}/{a.lane_id} -> {b.road_id}/{b.lane_id}")
            continue
        raw.extend(trace)
    # dedup consecutive identical waypoints
    plan = []
    for wp, opt in raw:
        if plan:
            last = plan[-1][0]
            if (last.road_id == wp.road_id and last.section_id == wp.section_id
                    and last.lane_id == wp.lane_id
                    and last.transform.location.distance(wp.transform.location) < 0.5):
                continue
        plan.append((wp, opt))
    return waypoints, plan


def plan_length(plan):
    return sum(a[0].transform.location.distance(b[0].transform.location)
               for a, b in zip(plan, plan[1:]))


def main():
    preset_id = sys.argv[1] if len(sys.argv) > 1 else "loop"
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    data = json.load(open(ROUTES, encoding="utf-8"))
    preset = next(p for p in data["presets"] if p["id"] == preset_id)
    specs = preset["waypoints"]
    print(f"preset {preset_id}: {len(specs)} waypoints, declared {preset['length_m']} m")

    grp = GlobalRoutePlanner(cmap, 2.0)
    waypoints, plan = build_plan(cmap, grp, specs)
    print(f"plan: {len(plan)} points, length {plan_length(plan):.1f} m")

    # continuity: every hop should be a small step or a lane change
    gaps = []
    for a, b in zip(plan, plan[1:]):
        d = a[0].transform.location.distance(b[0].transform.location)
        if d > 6.0:
            gaps.append((round(d, 1), a[0].road_id, a[0].lane_id, b[0].road_id, b[0].lane_id))
    print(f"hops > 6 m: {len(gaps)} {gaps[:6]}")

    # how close does the plan come to each preset waypoint
    worst = []
    for i, wp in enumerate(waypoints):
        if wp is None:
            continue
        best = min(w.transform.location.distance(wp.transform.location) for w, _ in plan)
        worst.append((round(best, 2), i, wp.road_id, wp.lane_id))
    worst.sort(reverse=True)
    print("worst preset-waypoint distances to plan:", worst[:6])

    # first segment bearing vs the plan's own second point
    if len(plan) > 3:
        b0 = bearing(plan[0][0].transform.location, plan[2][0].transform.location)
        print(f"plan[0] road/lane {plan[0][0].road_id}/{plan[0][0].lane_id} "
              f"yaw {plan[0][0].transform.rotation.yaw:.1f} "
              f"bearing to plan[2] {b0:.1f} "
              f"delta {(b0 - plan[0][0].transform.rotation.yaw + 180) % 360 - 180:+.1f}")

    opts = {}
    for _wp, opt in plan:
        opts[str(opt)] = opts.get(str(opt), 0) + 1
    print("road options:", opts)

    lanes = {}
    for wp, _opt in plan:
        lanes[wp.lane_type] = lanes.get(str(wp.lane_type), 0) + 1
    print("lane types:", lanes)

    json.dump(
        {"preset": preset_id,
         "plan": [{"x": round(w.transform.location.x, 2),
                   "y": round(w.transform.location.y, 2),
                   "z": round(w.transform.location.z, 2),
                   "road_id": w.road_id, "lane_id": w.lane_id,
                   "yaw": round(w.transform.rotation.yaw, 2),
                   "option": str(o)} for w, o in plan],
         "preset_points": [{"x": round(w.transform.location.x, 2),
                            "y": round(w.transform.location.y, 2),
                            "road_id": w.road_id, "lane_id": w.lane_id}
                           for w in waypoints if w is not None]},
        open(f"plan-{preset_id}.json", "w", encoding="utf-8"), indent=1)
    print(f"wrote plan-{preset_id}.json")


if __name__ == "__main__":
    main()

"""A/B the two plan builders on every preset, and report the numbers.

A  pure trace_route between consecutive preset waypoints, endpoints relocated
   so get_waypoint() lands back on the intended lane;
B  lane-walk within one lane (exact geometry), trace_route only across a change.
"""
from __future__ import annotations

import json
import sys

import carla

sys.path.insert(0, r"C:\GameProject\CarlaUE5\carla\PythonAPI\carla")
from agents.navigation.global_route_planner import GlobalRoutePlanner  # noqa: E402
from agents.navigation.local_planner import RoadOption  # noqa: E402

ROUTES = sys.argv[1] if len(sys.argv) > 1 else (
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
    r"\Platform\backend\data\routes\Town10HD.json")
SAMPLING = 2.0


def relocalize(cmap, wp):
    for direction in (1, -1):
        cur = wp
        for _ in range(14):
            snapped = cmap.get_waypoint(cur.transform.location)
            if snapped.road_id == wp.road_id and snapped.lane_id == wp.lane_id:
                return cur.transform.location
            nxt = cur.next(SAMPLING) if direction > 0 else cur.previous(SAMPLING)
            if not nxt:
                break
            cur = nxt[0]
            if cur.road_id != wp.road_id or cur.lane_id != wp.lane_id:
                break
    return None


def dedup(raw):
    plan = []
    for wp, opt in raw:
        if plan:
            last = plan[-1][0]
            if (last.road_id == wp.road_id and last.section_id == wp.section_id
                    and last.lane_id == wp.lane_id
                    and last.transform.location.distance(wp.transform.location) < 0.5):
                continue
        plan.append((wp, opt))
    return plan


def plan_a(cmap, grp, resolved):
    raw = []
    for a, b in zip(resolved, resolved[1:]):
        loc_a = relocalize(cmap, a) or a.transform.location
        loc_b = relocalize(cmap, b) or b.transform.location
        trace = grp.trace_route(loc_a, loc_b)
        if not trace:
            continue
        raw.append((a, RoadOption.LANEFOLLOW))
        raw.extend(trace[1:])
        raw.append((b, RoadOption.LANEFOLLOW))
    return dedup(raw)


def walk_same_lane(wp_a, wp_b):
    if (wp_a.road_id != wp_b.road_id or wp_a.lane_id != wp_b.lane_id
            or wp_a.section_id != wp_b.section_id):
        return None
    if (wp_b.s - wp_a.s) * (1 if wp_a.lane_id < 0 else -1) <= 0:
        return None
    out, cur = [wp_a], wp_a
    for _ in range(4000):
        if cur.transform.location.distance(wp_b.transform.location) <= SAMPLING * 1.5:
            break
        nxt = cur.next(SAMPLING)
        if not nxt:
            return None
        nxt = nxt[0]
        if nxt.road_id != wp_a.road_id or nxt.lane_id != wp_a.lane_id:
            return None
        cur = nxt
        out.append(cur)
    out.append(wp_b)
    return out


def plan_b(cmap, grp, resolved):
    raw = []
    for a, b in zip(resolved, resolved[1:]):
        walked = walk_same_lane(a, b)
        if walked is not None:
            raw.extend((w, RoadOption.LANEFOLLOW) for w in walked)
            continue
        loc_a = relocalize(cmap, a) or a.transform.location
        loc_b = relocalize(cmap, b) or b.transform.location
        trace = grp.trace_route(loc_a, loc_b)
        if not trace:
            continue
        raw.append((a, RoadOption.LANEFOLLOW))
        raw.extend(trace[1:])
        raw.append((b, RoadOption.LANEFOLLOW))
    return dedup(raw)


def length(plan):
    return sum(a[0].transform.location.distance(b[0].transform.location)
               for a, b in zip(plan, plan[1:]))


def describe(plan, resolved, label, declared):
    gaps = [a[0].transform.location.distance(b[0].transform.location)
            for a, b in zip(plan, plan[1:])]
    worst = max((min(w.transform.location.distance(p.transform.location)
                     for p, _ in plan), i)
                for i, w in enumerate(resolved))
    off_lane = sum(1 for w, _ in plan if w.lane_type != carla.LaneType.Driving)
    print(f"   {label}: {len(plan):>4} pts | {length(plan):7.1f} m "
          f"(declared {declared:6.1f}) | max hop {max(gaps):5.1f} m "
          f"| hops>6m {sum(1 for g in gaps if g > 6.0):>3} "
          f"| worst preset wp {worst[0]:5.2f} m @{worst[1]} "
          f"| non-driving pts {off_lane}")


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    data = json.load(open(ROUTES, encoding="utf-8"))
    grp = GlobalRoutePlanner(cmap, SAMPLING)
    for preset in data["presets"]:
        resolved = [cmap.get_waypoint_xodr(int(s["road_id"]), int(s["lane_id"]), float(s["s"]))
                    for s in preset["waypoints"]]
        missing = sum(1 for w in resolved if w is None)
        print(f"\n{preset['id']}: {len(resolved)} waypoints, unresolved {missing}")
        describe(plan_a(cmap, grp, resolved), resolved, "A trace-only", preset["length_m"])
        describe(plan_b(cmap, grp, resolved), resolved, "B walk+trace", preset["length_m"])


if __name__ == "__main__":
    main()

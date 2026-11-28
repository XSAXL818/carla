"""Is the plan length explained by geometry, or is a detour left in it?

For every hop of the built plan: the straight-line distance between its two
preset endpoints, versus the plan length between them. A hop that is much longer
than the road distance between its ends is a detour.
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


def seglen(pts):
    return sum(a.transform.location.distance(b.transform.location)
               for a, b in zip(pts, pts[1:]))


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    grp = GlobalRoutePlanner(cmap, SAMPLING)
    data = json.load(open(ROUTES, encoding="utf-8"))
    for preset in data["presets"]:
        resolved = [cmap.get_waypoint_xodr(int(s["road_id"]), int(s["lane_id"]), float(s["s"]))
                    for s in preset["waypoints"]]
        print(f"\n{preset['id']} ({len(resolved)} wps, declared {preset['length_m']} m)")
        total_plan = total_straight = 0.0
        detours = []
        for i, (a, b) in enumerate(zip(resolved, resolved[1:])):
            walked = walk_same_lane(a, b)
            if walked is not None:
                pts, how = walked, "walk"
            else:
                loc_a = relocalize(cmap, a) or a.transform.location
                loc_b = relocalize(cmap, b) or b.transform.location
                trace = grp.trace_route(loc_a, loc_b)
                pts, how = ([a] + [t[0] for t in trace[1:]] + [b]), "trace"
            seg = seglen(pts)
            straight = a.transform.location.distance(b.transform.location)
            total_plan += seg
            total_straight += straight
            if seg > straight * 2.0 + 25.0:
                detours.append((i, how, a.road_id, a.lane_id, b.road_id, b.lane_id,
                                round(straight, 1), round(seg, 1)))
        print(f"   plan {total_plan:.1f} m vs endpoint-to-endpoint {total_straight:.1f} m")
        print(f"   detours: {len(detours)}")
        for d in detours:
            print(f"      hop {d[0]:>2} {d[1]:<5} {d[2]}/{d[3]} -> {d[4]}/{d[5]} "
                  f"straight {d[6]:6.1f} m  plan {d[7]:7.1f} m")


if __name__ == "__main__":
    main()

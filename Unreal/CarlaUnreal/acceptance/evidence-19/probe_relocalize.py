"""Does nudging the endpoint location fix trace_route's localization?

trace_route takes *locations* and localizes them with map.get_waypoint(), which
returns the nearest lane. Junction connector roads overlap each other in space,
so the nearest lane to a connector's endpoint is often a different road -- the
trace then starts or ends somewhere else and detours around the block.

This checks, for the preset waypoints whose location localizes wrongly, whether
walking a little way along the *intended* lane produces a location that
localizes back to that lane.
"""
from __future__ import annotations

import json
import sys

import carla

ROUTES = sys.argv[1] if len(sys.argv) > 1 else (
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
    r"\Platform\backend\data\routes\Town10HD.json")


def relocalize(cmap, wp):
    """A location on `wp`'s own lane that get_waypoint() maps back to it."""
    candidates = [wp]
    fwd = wp
    for _ in range(12):
        nxt = fwd.next(2.0)
        if not nxt:
            break
        fwd = nxt[0]
        if fwd.road_id != wp.road_id or fwd.lane_id != wp.lane_id:
            break
        candidates.append(fwd)
    back = wp
    for _ in range(12):
        prv = back.previous(2.0)
        if not prv:
            break
        back = prv[0]
        if back.road_id != wp.road_id or back.lane_id != wp.lane_id:
            break
        candidates.append(back)
    for cand in candidates:
        snapped = cmap.get_waypoint(cand.transform.location)
        if snapped.road_id == wp.road_id and snapped.lane_id == wp.lane_id:
            return cand, cand.transform.location.distance(wp.transform.location)
    return None, None


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    data = json.load(open(ROUTES, encoding="utf-8"))
    bad = fixed = unfixable = 0
    for preset in data["presets"]:
        for i, spec in enumerate(preset["waypoints"]):
            wp = cmap.get_waypoint_xodr(int(spec["road_id"]), int(spec["lane_id"]),
                                        float(spec["s"]))
            if wp is None:
                continue
            snapped = cmap.get_waypoint(wp.transform.location)
            if snapped.road_id == wp.road_id and snapped.lane_id == wp.lane_id:
                continue
            bad += 1
            cand, shift = relocalize(cmap, wp)
            if cand is None:
                unfixable += 1
                print(f"  {preset['id']}[{i}] {wp.road_id}/{wp.lane_id}@{wp.s:.2f} "
                      f"-> snaps to {snapped.road_id}/{snapped.lane_id}  UNFIXABLE")
            else:
                fixed += 1
                print(f"  {preset['id']}[{i}] {wp.road_id}/{wp.lane_id}@{wp.s:.2f} "
                      f"-> snaps to {snapped.road_id}/{snapped.lane_id}; "
                      f"relocalized at {cand.road_id}/{cand.lane_id}@{cand.s:.2f} "
                      f"(shifted {shift:.1f} m)")
    print(f"\nmislocalized={bad} fixable={fixed} unfixable={unfixable}")


if __name__ == "__main__":
    main()

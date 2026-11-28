"""Why does trace_route detour on a same-road hop?

trace_route(origin_location, destination_location) starts from
`map.get_waypoint(location)` -- the *nearest* waypoint, which on a two-way road
may be the opposite carriageway. If the preset waypoint is on lane -1 and
get_waypoint() snaps to lane 1, the whole trace runs the wrong way round.

This compares, for every preset waypoint, the resolved waypoint against what
get_waypoint() returns for the same location.
"""
from __future__ import annotations

import json
import sys

import carla

ROUTES = sys.argv[1] if len(sys.argv) > 1 else (
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
    r"\Platform\backend\data\routes\Town10HD.json")


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    data = json.load(open(ROUTES, encoding="utf-8"))
    mismatched = total = 0
    for preset in data["presets"]:
        rows = []
        for i, spec in enumerate(preset["waypoints"]):
            total += 1
            wp = cmap.get_waypoint_xodr(int(spec["road_id"]), int(spec["lane_id"]),
                                        float(spec["s"]))
            if wp is None:
                rows.append((i, spec, "unresolved", None))
                continue
            snapped = cmap.get_waypoint(wp.transform.location)
            same = (snapped.road_id == wp.road_id and snapped.lane_id == wp.lane_id)
            if not same:
                mismatched += 1
            rows.append((i, spec, f"{wp.road_id}/{wp.lane_id}",
                         f"{snapped.road_id}/{snapped.lane_id}", same))
        print(f"\n{preset['id']}:")
        for row in rows:
            mark = "" if len(row) < 5 or row[4] else "   <-- get_waypoint() snaps elsewhere"
            print(f"   {row[0]:>2} preset {row[1]['road_id']}/{row[1]['lane_id']}"
                  f"@{row[1]['s']}  resolved {row[2]}  snapped {row[3]}{mark}")
    print(f"\nTOTAL {total} waypoints, get_waypoint() disagrees on {mismatched}")


if __name__ == "__main__":
    main()

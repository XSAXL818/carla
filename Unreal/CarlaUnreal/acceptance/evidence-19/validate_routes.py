"""Validate a generated routes JSON against the live map, strictly.

For every preset waypoint:
  * it must resolve with get_waypoint_xodr(road_id, lane_id, s) verbatim;
  * its lane must be LaneType.Driving;
  * the lane's own heading must agree with the direction the route travels
    through that waypoint (no opposite-carriageway waypoints).
"""
from __future__ import annotations

import json
import math
import sys

import carla


def bearing(a, b):
    return math.degrees(math.atan2(b.y - a.y, b.x - a.x)) % 360.0


def delta(a, b):
    return abs((a - b + 180.0) % 360.0 - 180.0)


def main():
    path = sys.argv[1]
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map on server:", cmap.name)
    data = json.load(open(path, encoding="utf-8"))
    total = bad_lane = unresolved = against = 0
    for preset in data["presets"]:
        wps = preset["waypoints"]
        rows = []
        for i, spec in enumerate(wps):
            total += 1
            road_id, lane_id, s = int(spec["road_id"]), int(spec["lane_id"]), float(spec["s"])
            try:
                wp = cmap.get_waypoint_xodr(road_id, lane_id, s)
            except Exception as exc:  # noqa: BLE001
                wp = None
                print(f"  EXC {road_id}/{lane_id}@{s}: {exc}")
            if wp is None:
                unresolved += 1
                rows.append((i, road_id, lane_id, s, "UNRESOLVED", None))
                continue
            lane_ok = wp.lane_type == carla.LaneType.Driving
            if not lane_ok:
                bad_lane += 1
            # travel direction through this waypoint, from its neighbours
            ref = None
            if i + 1 < len(wps) and int(wps[i + 1]["road_id"]) == road_id:
                ref = float(wps[i + 1]["s"]) - s
            elif i and int(wps[i - 1]["road_id"]) == road_id:
                ref = s - float(wps[i - 1]["s"])
            direction_ok = None
            if ref is not None and abs(ref) > 1e-6:
                # lane < 0 travels +s, lane > 0 travels -s
                direction_ok = (wp.lane_id < 0) == (ref > 0)
                if not direction_ok:
                    against += 1
            rows.append((i, road_id, wp.lane_id, s, str(wp.lane_type), direction_ok))
        print(f"\n{preset['id']}: {len(wps)} waypoints, length_m={preset['length_m']}")
        for row in rows:
            flag = ""
            if row[4] != "Driving":
                flag += "  <-- NON-DRIVING"
            if row[5] is False:
                flag += "  <-- WRONG DIRECTION"
            print(f"   {row[0]:>2} road={row[1]:<4} lane={row[2]:>3} s={row[3]:<8} "
                  f"{row[4]:<12} dir_ok={row[5]}{flag}")
    print(f"\nTOTAL {total} waypoints: unresolved={unresolved} "
          f"non-driving={bad_lane} wrong-direction={against}")


if __name__ == "__main__":
    main()

"""Is each preset point on the lane that actually heads towards the next point?

For every consecutive pair, compare the bearing from waypoint i to waypoint i+1
with the yaw of the lane the point resolved to (and with the lane the backend
would probe). A pair whose bearing differs from the lane yaw by ~180 deg is a
point sitting on the opposite carriageway: the ego spawned there faces away from
the route and has to drive around the block.
"""
from __future__ import annotations

import json
import math

data = json.load(open(r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
                      r"\Platform\backend\data\routes\Town10HD.json", encoding="utf-8"))
drawn = json.load(open("drawn-polyline.json", encoding="utf-8"))


def bearing(a, b):
    return math.degrees(math.atan2(b[0] - a[0], b[1] - a[1]))  # carla yaw convention: x=east, y=south


def wrap(d):
    return (d + 180.0) % 360.0 - 180.0


for preset in data["presets"]:
    pts = drawn[preset["id"]]
    print(f"\n=== {preset['id']} ===")
    bad = 0
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        brg = bearing((a["x"], a["y"]), (b["x"], b["y"]))
        delta = wrap(brg - a["yaw"]) if "yaw" in a else None
        seg = math.dist((a["x"], a["y"]), (b["x"], b["y"]))
        flag = ""
        if seg > 12.0 and abs(wrap(brg - a["leg_yaw"])) > 100.0:
            flag = "  <-- leg runs against the stored lane direction"
            bad += 1
        if seg > 12.0:
            print(f"  {i:02d}->{i+1:02d} stored_lane={a['stored_lane']:2d} used={a['lane_used']:2d} "
                  f"leg={seg:6.1f} m bearing={brg:7.1f} lane_yaw={a['leg_yaw']:7.1f} "
                  f"delta={wrap(brg - a['leg_yaw']):7.1f}{flag}")
    print(f"  long legs against their lane: {bad}")

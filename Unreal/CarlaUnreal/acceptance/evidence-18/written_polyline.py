"""Reproduce the polyline the backend really paints, byte for byte.

Mirrors Session._waypoint_at(): a stored lane id other than (None, 0, -1) is
probed alone; the -1 sentinel is expanded to [1..6, -1..-6] and the first
Driving waypoint wins, with the first non-None result kept as the fallback.
Writes drawn-polyline.json so the overlap measurement uses the drawn geometry,
not a naive re-resolution.
"""
from __future__ import annotations

import json

import carla

LANES = [1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6]


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(30.0)
    cmap = client.get_world().get_map()
    data = json.load(open(r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal"
                          r"\Source\TestPlatform\Platform\backend\data\routes\Town10HD.json",
                          encoding="utf-8"))
    out = {}
    for preset in data["presets"]:
        pts = []
        for wp in preset["waypoints"]:
            lane_id = int(wp["lane_id"])
            lanes = [lane_id] if lane_id not in (None, 0, -1) else LANES
            fallback = None
            chosen = None
            for lane in lanes:
                try:
                    got = cmap.get_waypoint_xodr(int(wp["road_id"]), lane, float(wp["s"]))
                except Exception:  # noqa: BLE001
                    got = None
                if got is None:
                    continue
                if got.lane_type == carla.LaneType.Driving:
                    chosen = (got, lane)
                    break
                if fallback is None:
                    fallback = (got, lane)
            pick = chosen or fallback
            if pick is not None:
                pts.append({"x": round(pick[0].transform.location.x, 2),
                            "y": round(pick[0].transform.location.y, 2),
                            "lane_used": pick[1], "stored_lane": lane_id,
                            "lane_type": str(pick[0].lane_type)})
        out[preset["id"]] = pts
        print(f"{preset['id']:<9} {len(pts)}/{len(preset['waypoints'])} points drawn")
        lanes_changed = [(p["stored_lane"], p["lane_used"]) for p in pts
                         if p["stored_lane"] != p["lane_used"]]
        if lanes_changed:
            print(f"   stored lane replaced with a probed lane: {len(lanes_changed)} "
                  f"samples {lanes_changed[:6]}")
    with open("drawn-polyline.json", "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    print("wrote drawn-polyline.json")


if __name__ == "__main__":
    main()

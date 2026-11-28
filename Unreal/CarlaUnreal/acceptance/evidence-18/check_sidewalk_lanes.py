"""What lanes does the .xodr really declare at (road, s) for the preset waypoints?

Confirms whether the emitted lane_id is a driving lane in the source data or a
fallback to whatever non-zero lane exists (e.g. a sidewalk lane).
"""
from __future__ import annotations

import json
import xml.etree.ElementTree as ET

XODR = (r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Content\Carla\Maps"
        r"\OpenDrive\Town10HD_Opt.xodr")

want = {}
res = json.load(open("route-resolve.json", encoding="utf-8"))
for p in res["presets"]:
    for r in p["rows"]:
        if r.get("lane_type") != "Driving":
            want.setdefault(r["road_id"], set()).add((p["id"], r["lane_id"], r["s"],
                                                     r.get("lane_type")))

root = ET.parse(XODR).getroot()
for road in root.iter("road"):
    rid = int(road.get("id"))
    if rid not in want:
        continue
    print(f"\nroad {rid} length={road.get('length')} junction={road.get('junction')}")
    for sec in road.findall("lanes/laneSection"):
        s0 = sec.get("s")
        lanes = []
        for side in ("left", "right"):
            el = sec.find(side)
            if el is None:
                continue
            for lane in el.findall("lane"):
                lanes.append((int(lane.get("id")), lane.get("type"), side))
        print(f"   laneSection s={s0}: {lanes}")
    for row in sorted(want[rid], key=lambda x: x[2]):
        print(f"   preset {row[0]} lane={row[1]} s={row[2]} -> resolved as {row[3]}")

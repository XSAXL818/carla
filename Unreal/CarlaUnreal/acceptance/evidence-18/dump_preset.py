"""Dump one preset's waypoints next to the resolved coordinates and segment lengths."""
import json
import math
import sys

preset_id = sys.argv[1] if len(sys.argv) > 1 else "loop"
res = json.load(open("route-resolve.json", encoding="utf-8"))
p = next(x for x in res["presets"] if x["id"] == preset_id)
print(f"preset {preset_id}")
prev = None
for r in p["rows"]:
    seg = "" if prev is None else f"seg={math.dist((prev['x'], prev['y']), (r['x'], r['y'])):7.1f}"
    print(f"  i={r['i']:2d} road={r['road_id']:4d} lane={r['lane_id']:2d} s={r['s']:8.2f} "
          f"({r['x']:9.2f},{r['y']:9.2f}) {r.get('lane_type',''):<9} {seg}")
    prev = r

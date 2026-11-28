"""Geometry sanity of the resolved Town10HD presets (continuity, closure)."""
import json
import math

d = json.load(open("route-resolve.json", encoding="utf-8"))
for p in d["presets"]:
    pts = [(r["x"], r["y"]) for r in p["rows"] if r["ok"]]
    segs = [math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    closed = math.dist(pts[0], pts[-1])
    print("%-9s n=%d segmin=%.1f segmax=%.1f segavg=%.1f sum=%.1f closure=%.1f"
          % (p["id"], len(pts), min(segs), max(segs), sum(segs) / len(segs), sum(segs), closed))
    big = [(i, round(s, 1)) for i, s in enumerate(segs) if s > 120]
    if big:
        print("   segments >120 m:", big)
    # sidewalk-lane waypoints
    sd = [r["i"] for r in p["rows"] if r["ok"] and r.get("lane_type") != "Driving"]
    if sd:
        print("   non-Driving lane waypoint indices:", sd)

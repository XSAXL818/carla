"""Does the drawn route polyline coincide with the path the ego actually drives?

Inputs
  samples-*.jsonl      independent samples (ego actor x/y per tick from a separate
                       carla.Client)
  route-resolve.json   the preset waypoints resolved on the live map (same
                       (road, lane, s) -> location lookup the backend uses for
                       its debug overlay)

Reports, per preset-like polyline:
  * how far each drawn vertex is from the driven path (min / median / max)
  * what fraction of the driven path lies within 3 m of the drawn polyline
  * which drawn vertices are "never approached" (> 20 m from the whole driven path)
"""
from __future__ import annotations

import json
import math
import sys


def seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return math.dist(p, a)
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def poly_dist(p, poly):
    if len(poly) == 1:
        return math.dist(p, poly[0])
    return min(seg_dist(p, poly[i], poly[i + 1]) for i in range(len(poly) - 1))


def main():
    samples_path, resolve_path, preset_id = sys.argv[1], sys.argv[2], sys.argv[3]
    path = []
    for line in open(samples_path, encoding="utf-8"):
        row = json.loads(line)
        if row.get("tag") != "sample":
            continue
        act = row.get("ego_actor")
        if act and "x" in act:
            path.append((act["x"], act["y"]))
    # de-duplicate consecutive identical points
    dedup = [path[0]] if path else []
    for p in path[1:]:
        if math.dist(p, dedup[-1]) > 0.05:
            dedup.append(p)

    res = json.load(open(resolve_path, encoding="utf-8"))
    preset = next(p for p in res["presets"] if p["id"] == preset_id)
    poly = [(r["x"], r["y"]) for r in preset["rows"] if r["ok"]]

    print(f"driven path samples : {len(dedup)} points, "
          f"length {sum(math.dist(dedup[i], dedup[i+1]) for i in range(len(dedup)-1)):.1f} m")
    print(f"drawn polyline      : {len(poly)} vertices, "
          f"length {sum(math.dist(poly[i], poly[i+1]) for i in range(len(poly)-1)):.1f} m")

    vertex_d = [poly_dist(v, dedup) for v in poly]
    vertex_d_sorted = sorted(vertex_d)
    med = vertex_d_sorted[len(vertex_d_sorted) // 2]
    print(f"drawn vertex -> driven path : min {min(vertex_d):.1f} m  median {med:.1f} m  "
          f"max {max(vertex_d):.1f} m")
    print(f"  vertices within 3 m  : {sum(d <= 3 for d in vertex_d)}/{len(vertex_d)}")
    print(f"  vertices within 10 m : {sum(d <= 10 for d in vertex_d)}/{len(vertex_d)}")
    print(f"  vertices farther than 20 m : {sum(d > 20 for d in vertex_d)}/{len(vertex_d)}")
    for i, d in enumerate(vertex_d):
        flag = "  <-- far" if d > 20 else ""
        print(f"    v{i:02d} ({poly[i][0]:9.2f},{poly[i][1]:9.2f}) d={d:7.1f} m{flag}")

    covered = [poly_dist(p, poly) <= 3.0 for p in dedup]
    print(f"driven path within 3 m of the drawn line: {sum(covered)}/{len(covered)} "
          f"({100.0 * sum(covered) / max(1, len(covered)):.1f} %)")
    for frac in (3.0, 10.0, 20.0):
        c = sum(poly_dist(p, poly) <= frac for p in dedup)
        print(f"  driven path within {frac:4.1f} m: {100.0 * c / max(1, len(dedup)):5.1f} %")
    # where does the ego start relative to the drawn polyline?
    print(f"ego start distance to polyline: {poly_dist(dedup[0], poly):.1f} m; "
          f"ego end distance to polyline: {poly_dist(dedup[-1], poly):.1f} m")


if __name__ == "__main__":
    main()

"""Offline experiment: direction-aware + driving-lane-aware preset graph.

Reuses gen_route_presets' parser, then answers two questions before touching the
generator:

  1. which (road, dir) pairs actually carry a driving lane in that direction
     (right-hand traffic: lane id < 0 travels +s, lane id > 0 travels -s);
  2. what presets come out when the graph only contains traversable pairs.
"""
from __future__ import annotations

import sys
from collections import defaultdict

sys.path.insert(
    0,
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform\Platform\tools",
)
import gen_route_presets as g  # noqa: E402

XODR = (r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Content\Carla\Maps"
        r"\OpenDrive\Town10HD.xodr")


def driving_dirs(lane_sections, rid):
    """{+1, -1} for the s-directions that have at least one driving lane."""
    dirs = set()
    for _s0, table in lane_sections.get(rid, []):
        for lid, kind in table.items():
            if kind != "driving":
                continue
            dirs.add(1 if lid < 0 else -1)
    return dirs


def main():
    roads, links, junctions, lane_sections = g.parse_xodr(XODR)
    dirs = {rid: driving_dirs(lane_sections, rid) for rid in roads}
    no_lane = sorted(r for r in roads if not dirs[r])
    print(f"roads={len(roads)} with no driving lane at all: {no_lane}")
    wrong = sorted(r for r in roads if dirs[r] and dirs[r] != {1, -1})
    print(f"roads with only one drivable direction: {len(wrong)} -> {wrong}")

    # build the graph, then drop every node whose (road, dir) is not drivable
    adj = g.build_graph(roads, links, junctions)
    pruned = defaultdict(list)
    dropped = 0
    for src, edges in adj.items():
        if src[1] not in dirs.get(src[0], set()):
            dropped += len(edges)
            continue
        for dst, kind, jid in edges:
            if dst[1] in dirs.get(dst[0], set()):
                pruned[src].append((dst, kind, jid))
            else:
                dropped += 1
    print(f"graph: {sum(len(v) for v in adj.values())} edges -> "
          f"{sum(len(v) for v in pruned.values())} kept, {dropped} dropped")

    presets = g.build_presets(roads, pruned, lane_sections)
    for p in presets:
        print(f"\n{p['id']:<9} length_m={p['length_m']} roads={p['roads']} "
              f"waypoints={len(p['waypoints'])}")
        bad = []
        for wp in p["waypoints"]:
            rid, lane, s = wp["road_id"], wp["lane_id"], wp["s"]
            table = None
            for s0, cand in lane_sections.get(rid, []):
                if s0 <= s:
                    table = cand
            if table is None:
                bad.append((rid, lane, s, "no-section"))
                continue
            kind = table.get(lane)
            if kind != "driving":
                bad.append((rid, lane, s, kind))
        print(f"   non-driving/sentinel waypoints: {len(bad)} {bad[:6]}")
        print("   roads:", [w["road_id"] for w in p["waypoints"]])

    # direction consistency: consecutive waypoints on the same road must step in
    # the road's drivable direction for the lane that was chosen
    for p in presets:
        wps = p["waypoints"]
        bad = []
        for a, b in zip(wps, wps[1:]):
            if a["road_id"] != b["road_id"]:
                continue
            ds = b["s"] - a["s"]
            lane = a["lane_id"]
            if lane == g.LANE_ID:
                continue
            want = 1 if lane < 0 else -1
            if ds * want < 0:
                bad.append((a["road_id"], lane, a["s"], b["s"]))
        print(f"{p['id']:<9} same-road steps against lane direction: {len(bad)} {bad[:4]}")


if __name__ == "__main__":
    main()

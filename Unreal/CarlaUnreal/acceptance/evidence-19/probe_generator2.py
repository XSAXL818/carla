"""Offline experiment 2: direction-strict lane_at + pruned graph.

Mirrors the intended generator change, without touching it yet:
  * a lane is only usable when it is `driving` AND on the side that travels the
    traversed s-direction (right-hand traffic: id < 0 -> +s, id > 0 -> -s);
  * the graph only keeps (road, dir) nodes that have such a lane.

Reports every preset waypoint that still resolves to a sentinel / non-driving
lane, plus the same-road direction consistency.
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


def lane_at_strict(lane_sections, rid, s, direction):
    sections = lane_sections.get(rid) or []
    if not sections:
        return None
    table = sections[0][1]
    for s0, candidate in sections:
        if s0 <= s:
            table = candidate
        else:
            break
    driving = [lid for lid, kind in table.items()
               if kind == "driving" and (lid < 0) == (direction > 0)]
    if not driving:
        return None
    return min(driving, key=lambda lid: (abs(lid), lid))


def table_at(lane_sections, rid, s):
    table = None
    for s0, cand in lane_sections.get(rid, []):
        if s0 <= s:
            table = cand
    return table or {}


def drivable_dirs(lane_sections):
    out = {}
    for rid, sections in lane_sections.items():
        dirs = set()
        for _s0, table in sections:
            for lid, kind in table.items():
                if kind == "driving":
                    dirs.add(1 if lid < 0 else -1)
        out[rid] = dirs
    return out


def prune(adj, dirs):
    out = defaultdict(list)
    for src, edges in adj.items():
        if src[1] not in dirs.get(src[0], set()):
            continue
        for dst, kind, jid in edges:
            if dst[1] in dirs.get(dst[0], set()):
                out[src].append((dst, kind, jid))
    return out


def to_waypoints(roads, path, lane_sections):
    wps = []

    def push(rid, s, length, direction):
        s = round(max(0.0, min(length, s)), 2)
        if wps and wps[-1]["road_id"] == rid and abs(wps[-1]["s"] - s) < 1e-6:
            return
        lane = lane_at_strict(lane_sections, rid, s, direction)
        wps.append({"road_id": rid, "lane_id": g.LANE_ID if lane is None else lane, "s": s})

    for rid, direction in path:
        length = roads.get(rid, 0.0)
        if direction > 0:
            entry, exit_ = g.S_MARGIN, length - g.S_MARGIN
        else:
            entry, exit_ = length - g.S_MARGIN, g.S_MARGIN
        push(rid, entry, length, direction)
        push(rid, exit_, length, direction)
    return wps


def main():
    roads, links, junctions, lane_sections = g.parse_xodr(XODR)
    dirs = drivable_dirs(lane_sections)
    adj = prune(g.build_graph(roads, links, junctions), dirs)
    print(f"edges kept: {sum(len(v) for v in adj.values())}")

    attempts = {
        "loop": [
            lambda: g.preset_loop(adj, roads),
            lambda: g.preset_loop(adj, roads, min_m=400.0, max_m=1400.0,
                                  max_hops=12, max_tries=80),
            lambda: g.preset_loop_relaxed(adj, roads),
        ],
        "arterial": [
            lambda: g.preset_arterial(adj, roads, 1200.0, 900.0),
            lambda: g.preset_arterial(adj, roads, 1000.0, 600.0),
            lambda: g.preset_arterial(adj, roads, 700.0, 400.0),
            lambda: g.preset_arterial(adj, roads, 600.0, 250.0),
        ],
        "cross": [
            lambda: g.preset_cross(adj, roads, 5000.0, 400.0),
            lambda: g.preset_cross(adj, roads, 8000.0, 300.0),
            lambda: g.preset_cross(adj, roads, 3000.0, 150.0),
        ],
        "turns": [
            lambda: g.preset_turns(adj, roads, 1800.0, 5, 900.0),
            lambda: g.preset_turns(adj, roads, 1400.0, 3, 600.0),
            lambda: g.preset_turns(adj, roads, 1000.0, 2, 400.0),
        ],
    }
    found = {}
    for kind, tries in attempts.items():
        for i, fn in enumerate(tries):
            hit = fn()
            if hit and len(hit[0]) >= 2 and hit[1] > 0:
                found[kind] = (hit, i)
                break
    for kind in ("loop", "arterial", "cross", "turns"):
        hit, idx = found[kind]
        path, length = hit
        wps = to_waypoints(roads, path, lane_sections)
        sentinels, nondriving = [], []
        for wp in wps:
            if wp["lane_id"] == g.LANE_ID:
                sentinels.append(wp)
                continue
            kind_at = table_at(lane_sections, wp["road_id"], wp["s"]).get(wp["lane_id"])
            if kind_at != "driving":
                nondriving.append((wp, kind_at))
        same_road_bad = []
        for a, b in zip(wps, wps[1:]):
            if a["road_id"] != b["road_id"] or a["lane_id"] == g.LANE_ID:
                continue
            want = 1 if a["lane_id"] < 0 else -1
            if (b["s"] - a["s"]) * want < 0:
                same_road_bad.append((a["road_id"], a["lane_id"], a["s"], b["s"]))
        print(f"\n{kind:<9} try#{idx} length_m={round(length, 1)} roads={len(path)} "
              f"waypoints={len(wps)}")
        print(f"   sentinel lane_id: {len(sentinels)} {sentinels[:4]}")
        print(f"   non-driving lane: {len(nondriving)} {nondriving[:4]}")
        print(f"   same-road direction violations: {len(same_road_bad)} {same_road_bad[:4]}")
        print(f"   first={wps[0]} last={wps[-1]}")
        print(f"   path={path}")


if __name__ == "__main__":
    main()

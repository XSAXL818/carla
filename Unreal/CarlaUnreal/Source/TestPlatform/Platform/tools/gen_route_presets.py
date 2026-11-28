#!/usr/bin/env python3
"""Derive four route presets per CARLA map from its OpenDRIVE (.xodr) file.

A route is a list of waypoint control points ``(road_id, lane_id, s)`` -- the
same triple a CARLA ``carla.Waypoint(road_id, lane_id, s)`` takes, so a preset
can be replayed by the backend without re-planning:

    from carla.agents.navigation.global_route_planner import GlobalRoutePlanner
    grp = GlobalRoutePlanner(world.get_map(), 2.0)
    plan = grp.trace_route(wp0, wpN)          # validated at runtime

The graph is **direction-strict**: OpenDRIVE links peer roads in both directions,
but a road can only be driven the way its lanes point.  Right-hand traffic puts
the lanes travelling along +s on the right of the reference line (negative lane
ids) and the lanes travelling along -s on the left (positive ids), so a
``(road, dir)`` node only exists when the road really has a ``driving`` lane on
that side.  Without this the presets asked the ego to drive junction connectors
and whole roads the wrong way -- the map has no driving lane there at all, only
``shoulder``/``sidewalk``, and the ego was spawned facing backwards.

The backend turns a preset into a ``[(carla.Waypoint, RoadOption)]`` plan on the
live map (``session_runner._build_plan()``) and the agent follows exactly that
plan, so what the generator emits here is what the car drives.

Preset kinds, one of each per map:

    loop      城市环线   closes back on itself, 400-1400 m
    arterial  干道直行   follows the longest arterial, >= 900 m
    cross     跨图长途   farthest-to-farthest pair in the graph
    turns     路口穿行   junction-dense, >= 5 road changes

Usage:
    python gen_route_presets.py --maps-root <Content/Carla/Maps> --out <data dir>
"""

from __future__ import annotations

import argparse
import heapq
import json
import os
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict, deque

# ``lane_id = 0`` is the "any driving lane" sentinel: OpenDRIVE lane 0 is the
# reference line and is never a drivable lane, so it cannot collide with a real
# id. It used to be -1, which in this map is a perfectly ordinary lane (the
# right-hand lane travelling along +s) -- a stored -1 therefore meant "right
# lane" to the generator and "probe from the left lane first" to the backend,
# which is how the ego ended up spawned on the opposite carriageway. Every
# waypoint now carries a lane id that the .xodr really has at that ``s``, is
# ``driving``, and points the way the route travels; 0 only survives for roads
# whose lane sections cannot be read at all.
LANE_ID = 0
S_MARGIN = 1.0  # keep waypoints off the exact road ends

# --------------------------------------------------------------------------- #
# OpenDRIVE parsing
# --------------------------------------------------------------------------- #
def parse_xodr(path: str):
    """Iteratively parse roads, links, junctions and per-road lane sections.

    Returns (roads, links, junctions, lane_sections). ``lane_sections`` maps a
    road id onto ``[(s_start, {lane_id: lane_type})]`` sorted by ``s_start``; the
    presets need it to emit a ``lane_id`` that exists at the waypoint's ``s``
    (junction connecting roads often have no lane -1 at all, only lane 1).
    """
    roads: dict[int, float] = {}
    links: dict[int, dict[str, tuple[str, str, str]]] = {}
    junctions: dict[int, list[tuple[int, str, int, int]]] = defaultdict(list)
    lane_sections: dict[int, list[tuple[float, dict[int, str]]]] = {}

    for _event, el in ET.iterparse(path, events=("end",)):
        if el.tag == "road":
            rid = int(el.get("id"))
            roads[rid] = float(el.get("length") or 0.0)
            entry = {}
            link_el = el.find("link")
            if link_el is not None:
                for key in ("predecessor", "successor"):
                    sub = link_el.find(key)
                    if sub is not None:
                        entry[key] = (
                            sub.get("elementType") or "",
                            sub.get("elementId") or "-1",
                            sub.get("contactPoint") or "start",
                        )
            links[rid] = entry
            sections = []
            lanes_el = el.find("lanes")
            if lanes_el is not None:
                for sec in lanes_el.findall("laneSection"):
                    try:
                        s0 = float(sec.get("s") or 0.0)
                    except (TypeError, ValueError):
                        s0 = 0.0
                    table: dict[int, str] = {}
                    for side in ("left", "right"):
                        side_el = sec.find(side)
                        if side_el is None:
                            continue
                        for lane in side_el.findall("lane"):
                            try:
                                lid = int(lane.get("id"))
                            except (TypeError, ValueError):
                                continue
                            if lid != 0:
                                table[lid] = lane.get("type") or "driving"
                    sections.append((s0, table))
            sections.sort(key=lambda item: item[0])
            lane_sections[rid] = sections
            el.clear()
        elif el.tag == "junction":
            for conn in el.findall("connection"):
                try:
                    inc = int(conn.get("incomingRoad"))
                    con = int(conn.get("connectingRoad"))
                except (TypeError, ValueError):
                    continue
                cp = conn.get("contactPoint") or "start"
                # Which end of the *incoming* road feeds this connection is not
                # in the connection attributes -- it is in its laneLinks. A
                # from-lane id < 0 travels along +s, so the connection leaves the
                # incoming road at its end; from-lane > 0 leaves at its start.
                # Assuming "always leaves at the end" made 42 of this map's 85
                # connections traverse the incoming road the wrong way, which is
                # why the backend's trace_route had to detour hundreds of metres
                # around junctions that are actually one-way.
                inc_dirs = {1 if int(link.get("from") or 0) < 0 else -1
                            for link in conn.findall("laneLink")
                            if (link.get("from") or "").lstrip("-").isdigit()
                            and int(link.get("from") or 0) != 0}
                for inc_dir in (sorted(inc_dirs) or [1]):
                    junctions[inc].append((con, cp, int(conn.get("id") or -1), inc_dir))
            el.clear()

    return roads, links, junctions, lane_sections


# --------------------------------------------------------------------------- #
# Traversal graph
# --------------------------------------------------------------------------- #
def entry_dir(contact_point: str) -> int:
    """+1 when the peer road is entered at its start (traverse s: 0 -> L)."""
    return 1 if contact_point == "start" else -1


def build_graph(roads, links, junctions):
    """(road, dir) graph.  dir = +1 traverses s from 0 to length, -1 the reverse.

    Edge payload: (target_node, kind, junction_id).
    """
    adj = defaultdict(list)

    def add(src, dst, kind, jid=-1):
        if dst[0] in roads:
            adj[src].append((dst, kind, jid))

    for rid, ln in links.items():
        if rid not in roads:
            continue
        for key, out_dir in (("successor", 1), ("predecessor", -1)):
            item = ln.get(key)
            if not item:
                continue
            etype, eid, cp = item
            if etype != "road":
                continue
            try:
                nid = int(eid)
            except ValueError:
                continue
            if nid not in roads:
                continue
            nd = entry_dir(cp)
            add((rid, out_dir), (nid, nd), "link")
            add((nid, -nd), (rid, -out_dir), "link")

    for inc, conns in junctions.items():
        for con, cp, jid, inc_dir in conns:
            nd = entry_dir(cp)
            # The connection is only valid when the incoming road is being
            # traversed *towards* the end that feeds it, and when the connecting
            # road can be entered in the direction it is joined.
            add((inc, inc_dir), (con, nd), "junction", jid)
            add((con, -nd), (inc, -inc_dir), "junction", jid)

    return adj


def drivable_dirs(lane_sections) -> dict[int, set[int]]:
    """{road_id: {+1, -1}} -- the s-directions that road really has lanes for.

    OpenDRIVE links two roads to each other in *both* directions, which says the
    topology is connected, not that either direction is legal. Right-hand
    traffic: lane id < 0 lies right of the reference line and travels +s, lane
    id > 0 lies left and travels -s. A direction with no ``driving`` lane on its
    side has no drivable lane at all (junction connectors often carry only
    ``shoulder``/``sidewalk``), so asking the ego to traverse it is asking it to
    drive the wrong way down a one-way stub.
    """
    out: dict[int, set[int]] = {}
    for rid, sections in lane_sections.items():
        dirs: set[int] = set()
        for _s0, table in sections:
            for lid, kind in table.items():
                if kind == "driving":
                    dirs.add(1 if lid < 0 else -1)
        out[rid] = dirs
    return out


def prune_graph(adj, dirs):
    """Drop every (road, dir) node and edge the road cannot legally be driven."""
    pruned = defaultdict(list)
    for src, edges in adj.items():
        if src[1] not in dirs.get(src[0], ()):
            continue
        for dst, kind, jid in edges:
            if dst[1] in dirs.get(dst[0], ()):
                pruned[src].append((dst, kind, jid))
    return pruned


def neighbours(adj, node):
    return adj.get(node, ())


def walk_cost(roads, node):
    return roads.get(node[0], 0.0)


# --------------------------------------------------------------------------- #
# Shortest path / sweeps
# --------------------------------------------------------------------------- #
def dijkstra(adj, roads, src):
    dist = {src: 0.0}
    prev: dict = {}
    pq = [(0.0, src)]
    while pq:
        d, node = heapq.heappop(pq)
        if d > dist.get(node, float("inf")):
            continue
        for dst, _kind, _jid in neighbours(adj, node):
            nd = d + max(1.0, walk_cost(roads, dst))
            if nd < dist.get(dst, float("inf")):
                dist[dst] = nd
                prev[dst] = node
                heapq.heappush(pq, (nd, dst))
    return dist, prev


def rebuild(prev, src, dst):
    path, cur = [dst], dst
    while cur != src:
        cur = prev.get(cur)
        if cur is None:
            return []
        path.append(cur)
    path.reverse()
    return path


# --------------------------------------------------------------------------- #
# Presets
# --------------------------------------------------------------------------- #
def preset_loop(adj, roads, min_m=400.0, max_m=1400.0, max_hops=9, max_tries=80):
    """A cycle that returns to its own road."""
    for rid in sorted(roads, key=lambda r: -roads[r])[:max_tries]:
        start = (rid, 1)
        q = deque([(start, [start], 0.0)])
        seen = {start}
        while q:
            node, path, dist = q.popleft()
            if len(path) > max_hops:
                continue
            for dst, kind, _jid in neighbours(adj, node):
                if dst[0] == rid and len(path) >= 3:
                    total = dist + walk_cost(roads, dst)
                    if min_m <= total <= max_m:
                        return path + [dst], total
                if dst in seen:
                    continue
                seen.add(dst)
                q.append((dst, path + [dst], dist + walk_cost(roads, dst)))
    return None


def preset_loop_relaxed(adj, roads):
    return preset_loop(adj, roads, min_m=250.0, max_m=2600.0, max_hops=14, max_tries=200)


def preset_arterial(adj, roads, target_m=1200.0, min_m=900.0):
    if not roads:
        return None
    rid = max(roads, key=lambda r: roads[r])
    node = (rid, 1)
    path, dist, used = [node], 0.0, {node}
    while dist < target_m:
        cand = [
            (dst, kind)
            for dst, kind, _jid in neighbours(adj, node)
            if dst not in used and kind == "link"
        ]
        if not cand:
            cand = [
                (dst, kind)
                for dst, kind, _jid in neighbours(adj, node)
                if dst not in used
            ]
        if not cand:
            break
        dst = max(cand, key=lambda c: walk_cost(roads, c[0]))[0]
        path.append(dst)
        used.add(dst)
        dist += walk_cost(roads, dst)
        node = dst
    return (path, dist) if len(path) >= 2 and dist >= min_m else None


def truncate_to(path, roads, max_m):
    """Keep the prefix of a path while it stays under max_m (>= 2 roads)."""
    total, keep = 0.0, [path[0]]
    for node in path[1:]:
        step = walk_cost(roads, node)
        if total + step > max_m and len(keep) >= 2:
            break
        total += step
        keep.append(node)
    return keep, total


def preset_cross(adj, roads, max_m=5000.0, min_m=400.0):
    if not roads:
        return None
    start = (max(roads, key=lambda r: roads[r]), 1)
    d1, _ = dijkstra(adj, roads, start)
    if not d1:
        return None
    far1 = max(d1, key=d1.get)
    d2, prev = dijkstra(adj, roads, far1)
    if not d2:
        return None
    far2 = max(d2, key=d2.get)
    path = rebuild(prev, far1, far2)
    if len(path) < 2:
        return None
    path, length = truncate_to(path, roads, max_m)
    if length < min_m:
        return None
    return path, length


def preset_turns(adj, roads, target_m=1800.0, min_turns=5, min_m=900.0):
    if not roads:
        return None
    best = None
    starts = sorted(roads, key=lambda r: -roads[r])[:40]
    for rid in starts:
        node = (rid, 1)
        path, used, dist, turns = [node], {node}, 0.0, 0
        while dist < target_m and len(path) < 24:
            jumps = [
                (dst, jid)
                for dst, kind, jid in neighbours(adj, node)
                if kind == "junction" and dst not in used
            ]
            cand = jumps or [
                (dst, jid)
                for dst, kind, jid in neighbours(adj, node)
                if dst not in used
            ]
            if not cand:
                break
            dst = max(cand, key=lambda c: walk_cost(roads, c[0]))[0]
            turns += 1 if jumps else 0
            path.append(dst)
            used.add(dst)
            dist += walk_cost(roads, dst)
            node = dst
            if dist > target_m * 1.8:  # a single road can be kilometres long
                break
        if turns < min_turns or dist < min_m:
            continue
        if best is None or turns > best[2]:
            best = (path, dist, turns)
    return (best[0], best[1]) if best else None


# --------------------------------------------------------------------------- #
# Serialisation
# --------------------------------------------------------------------------- #
def lane_at(lane_sections, rid, s, direction):
    """The driving lane id that travels ``direction`` on ``rid`` at ``s``.

    Right-hand traffic: travelling with +s uses the right side of the reference
    line (negative OpenDRIVE ids), travelling with -s the left side (positive
    ids); the lane closest to the centre line wins. Returns ``None`` when the
    road has no ``driving`` lane on that side at that ``s`` -- the caller then
    stores the sentinel, and the backend resolves it against the live map.

    The previous version fell back to "any non-zero lane" when no driving lane
    was found, which is how the ``cross`` preset came to carry ``lane_id = 1``
    on junction connectors whose only lanes are ``shoulder`` and ``sidewalk``:
    the overlay dipped onto the kerb, and a car told to follow that plan would
    have driven on the pavement. No driving lane on the correct side means the
    traversal is not drivable, and the graph pruner keeps such roads out of the
    route in the first place.
    """
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


def to_waypoints(roads, path, lane_sections):
    """Entry *and* exit control point for every road, so the route is unambiguous.

    Entry-only would leave the traversal of each road implicit; with both ends
    the renderer can draw the exact driven geometry and the backend can hand the
    list straight to GlobalRoutePlanner.
    """
    wps = []

    def push(rid, s, length, direction):
        s = round(max(0.0, min(length, s)), 2)
        if wps and wps[-1]["road_id"] == rid and abs(wps[-1]["s"] - s) < 1e-6:
            return
        lane = lane_at(lane_sections, rid, s, direction)
        wps.append({"road_id": rid, "lane_id": LANE_ID if lane is None else lane, "s": s})

    for rid, direction in path:
        length = roads.get(rid, 0.0)
        if direction > 0:
            entry, exit_ = S_MARGIN, length - S_MARGIN
        else:
            entry, exit_ = length - S_MARGIN, S_MARGIN
        push(rid, entry, length, direction)
        push(rid, exit_, length, direction)
    return wps


PRESET_META = {
    "loop": ("城市环线", "回到起点的闭环，适合反复起停与信号灯工况"),
    "arterial": ("干道直行", "沿最长的连续干道直行，适合跟车与车速控制"),
    "cross": ("跨图长途", "拓扑上最远的两点，适合长距离导航与能耗统计"),
    "turns": ("路口穿行", "高频路口转向，适合验证转向与让行逻辑"),
}


def build_presets(roads, adj, lane_sections):
    """Each kind is tried at its preferred size, then at relaxed sizes."""
    attempts = {
        "loop": [
            lambda: preset_loop(adj, roads),
            # A direction-strict graph is much smaller than the permissive one
            # (Town10HD: 231 of 546 edges), so the 9-hop cycle search that used
            # to find the 498 m ring finds nothing; one more hop still lands
            # inside the 400-1400 m band with a real, drivable cycle.
            lambda: preset_loop(adj, roads, max_hops=12),
            lambda: preset_loop_relaxed(adj, roads),
        ],
        "arterial": [
            lambda: preset_arterial(adj, roads, 1200.0, 900.0),
            lambda: preset_arterial(adj, roads, 1000.0, 600.0),
            lambda: preset_arterial(adj, roads, 700.0, 400.0),
            lambda: preset_arterial(adj, roads, 600.0, 250.0),
        ],
        "cross": [
            lambda: preset_cross(adj, roads, 5000.0, 400.0),
            lambda: preset_cross(adj, roads, 8000.0, 300.0),
            lambda: preset_cross(adj, roads, 3000.0, 150.0),
        ],
        "turns": [
            lambda: preset_turns(adj, roads, 1800.0, 5, 900.0),
            lambda: preset_turns(adj, roads, 1400.0, 3, 600.0),
            lambda: preset_turns(adj, roads, 1000.0, 2, 400.0),
        ],
    }
    found = {}
    for kind, tries in attempts.items():
        for fn in tries:
            hit = fn()
            if hit and len(hit[0]) >= 2 and hit[1] > 0:
                found[kind] = hit
                break

    # cross may still collapse to a stub on tiny maps: fall back to arterial
    if "cross" not in found and "arterial" in found:
        found["cross"] = found["arterial"]

    out, seen = [], set()
    for kind in ("loop", "arterial", "cross", "turns"):
        hit = found.get(kind)
        if not hit:
            continue
        path, length = hit
        key = tuple(path)
        if key in seen:
            continue
        seen.add(key)
        name, note = PRESET_META[kind]
        out.append(
            {
                "id": kind,
                "name": name,
                "kind": kind,
                "note": note,
                "length_m": round(float(length), 1),
                "roads": len(path),
                "waypoints": to_waypoints(roads, path, lane_sections),
            }
        )
    return out


# --------------------------------------------------------------------------- #
def main(argv=None):
    default_root = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "_xodr_cache",
    )
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--maps-root", required=True, help="Content/Carla/Maps")
    ap.add_argument("--out", required=True, help="output directory for the JSON")
    ap.add_argument("--limit", default=None, help="only these maps, comma separated")
    args = ap.parse_args(argv)

    wanted = None
    if args.limit:
        wanted = {m.strip() for m in args.limit.split(",") if m.strip()}

    xodrs = []
    for dirpath, _dirnames, filenames in os.walk(args.maps_root):
        for fn in filenames:
            if not fn.lower().endswith(".xodr"):
                continue
            if fn.lower().endswith("_opt.xodr"):
                continue
            map_name = os.path.splitext(fn)[0]
            if wanted and map_name not in wanted:
                continue
            xodrs.append((map_name, os.path.join(dirpath, fn)))
    xodrs.sort()

    os.makedirs(args.out, exist_ok=True)
    # Merge into the existing index instead of replacing it: --limit is for
    # regenerating one map, and starting from an empty index made a single-map
    # run delete the other fourteen maps from index.json (the map picker reads
    # it, so the UI silently dropped to one map).
    index_path = os.path.join(args.out, "index.json")
    index = {"source": "OpenDRIVE", "maps": {}}
    if os.path.isfile(index_path):
        try:
            with open(index_path, encoding="utf-8") as fh:
                existing = json.load(fh)
            if isinstance(existing.get("maps"), dict):
                index["source"] = existing.get("source", index["source"])
                index["maps"] = existing["maps"]
        except (OSError, ValueError):
            pass  # unreadable index: rebuild it from what this run produces
    print(f"{'map':<12}{'roads':>7}{'junc':>6}  presets")
    print("-" * 78)

    for map_name, path in xodrs:
        try:
            roads, links, junctions, lane_sections = parse_xodr(path)
        except ET.ParseError as exc:
            print(f"{map_name:<12}  parse error: {exc}")
            continue
        if not roads:
            print(f"{map_name:<12}  no roads")
            continue
        adj = prune_graph(build_graph(roads, links, junctions),
                          drivable_dirs(lane_sections))
        presets = build_presets(roads, adj, lane_sections)
        entry = {
            "map": map_name,
            "xodr": os.path.relpath(path, args.maps_root).replace("\\", "/"),
            "roads": len(roads),
            "junctions": len(junctions),
            "presets": presets,
        }
        with open(os.path.join(args.out, f"{map_name}.json"), "w", encoding="utf-8") as fh:
            json.dump(entry, fh, ensure_ascii=False, indent=1)
        index["maps"][map_name] = {
            "roads": len(roads),
            "junctions": len(junctions),
            "presets": [
                {
                    "id": p["id"],
                    "name": p["name"],
                    "length_km": round(p["length_m"] / 1000.0, 2),
                    "waypoints": len(p["waypoints"]),
                }
                for p in presets
            ],
        }
        summary = "  ".join(
            f"{p['id']}={p['length_m'] / 1000.0:.2f}km/{p['roads']}rd" for p in presets
        )
        print(f"{map_name:<12}{len(roads):>7}{len(junctions):>6}  {summary or 'none'}")

    with open(index_path, "w", encoding="utf-8") as fh:
        json.dump(index, fh, ensure_ascii=False, indent=1)
    print("-" * 78)
    print(f"written to {args.out} ({len(index['maps'])} maps in index.json)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

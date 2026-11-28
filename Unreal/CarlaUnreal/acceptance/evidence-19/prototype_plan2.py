"""Prototype v2: lane-walk within a road, anchored trace_route between roads.

Why not plain trace_route per hop:
  * it takes *locations* and localizes them with map.get_waypoint(), i.e. the
    nearest lane; junction connectors overlap in space, so 11 of the 152 preset
    waypoints localize onto a different road and the trace detours up to 600 m;
  * for a hop that stays on one lane it re-derives geometry the lane already
    has, and can wander off it.

So: walk the lane with waypoint.next() when the hop stays on the same road and
lane (that IS the geometry), and use trace_route only across a road change,
anchored on locations that localize back onto the intended lane.
"""
from __future__ import annotations

import json
import math
import sys

import carla

sys.path.insert(0, r"C:\GameProject\CarlaUE5\carla\PythonAPI\carla")
from agents.navigation.global_route_planner import GlobalRoutePlanner  # noqa: E402
from agents.navigation.local_planner import RoadOption  # noqa: E402

ROUTES = sys.argv[2] if len(sys.argv) > 2 else (
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform"
    r"\Platform\backend\data\routes\Town10HD.json")
SAMPLING = 2.0


def relocalize(cmap, wp):
    """A location on wp's own lane that get_waypoint() maps back to that lane."""
    for direction in (1, -1):
        cur = wp
        for _ in range(14):
            snapped = cmap.get_waypoint(cur.transform.location)
            if snapped.road_id == wp.road_id and snapped.lane_id == wp.lane_id:
                return cur.transform.location
            nxt = cur.next(SAMPLING) if direction > 0 else cur.previous(SAMPLING)
            if not nxt:
                break
            cur = nxt[0]
            if cur.road_id != wp.road_id or cur.lane_id != wp.lane_id:
                break
    return None


def walk_same_lane(wp_a, wp_b):
    """Points from wp_a to wp_b along one lane; None if they are not co-lane."""
    if (wp_a.road_id != wp_b.road_id or wp_a.lane_id != wp_b.lane_id
            or wp_a.section_id != wp_b.section_id):
        return None
    # lane id < 0 travels along +s, lane id > 0 along -s
    forward = (wp_b.s - wp_a.s) * (1 if wp_a.lane_id < 0 else -1) > 0
    if not forward:
        return None  # against the lane: not a legal traversal
    out = [wp_a]
    cur = wp_a
    for _ in range(4000):
        if cur.transform.location.distance(wp_b.transform.location) <= SAMPLING * 1.5:
            break
        nxt = cur.next(SAMPLING)
        if not nxt:
            return None
        nxt = nxt[0]
        if nxt.road_id != wp_a.road_id or nxt.lane_id != wp_a.lane_id:
            return None
        cur = nxt
        out.append(cur)
    out.append(wp_b)
    return out


def build_plan(cmap, grp, specs):
    resolved = []
    for spec in specs:
        wp = cmap.get_waypoint_xodr(int(spec["road_id"]), int(spec["lane_id"]),
                                    float(spec["s"]))
        if wp is None:
            print(f"  !! {spec} unresolved")
        resolved.append(wp)

    raw, stats = [], {"walk": 0, "trace": 0, "walk_failed": 0}
    for a, b in zip(resolved, resolved[1:]):
        if a is None or b is None:
            continue
        walked = walk_same_lane(a, b)
        if walked is not None:
            raw.extend((w, RoadOption.LANEFOLLOW) for w in walked)
            stats["walk"] += 1
            continue
        if (a.road_id == b.road_id and a.lane_id == b.lane_id):
            stats["walk_failed"] += 1
        loc_a = relocalize(cmap, a) or a.transform.location
        loc_b = relocalize(cmap, b) or b.transform.location
        trace = grp.trace_route(loc_a, loc_b)
        if not trace:
            print(f"  !! empty trace {a.road_id}/{a.lane_id} -> {b.road_id}/{b.lane_id}")
            continue
        # the trace starts at the nearest waypoint to loc_a; replace its head
        # with the preset waypoint itself so the plan really visits it
        trace = [(a, RoadOption.LANEFOLLOW)] + list(trace[1:])
        raw.extend(trace)
        stats["trace"] += 1

    # de-duplicate consecutive identical waypoints
    plan = []
    for wp, opt in raw:
        if plan:
            last = plan[-1][0]
            if (last.road_id == wp.road_id and last.section_id == wp.section_id
                    and last.lane_id == wp.lane_id
                    and last.transform.location.distance(wp.transform.location) < 0.5):
                continue
        plan.append((wp, opt))
    return resolved, plan, stats


def plan_length(plan):
    return sum(a[0].transform.location.distance(b[0].transform.location)
               for a, b in zip(plan, plan[1:]))


def _heading(wp):
    return math.radians(wp.transform.rotation.yaw)


def _point_at_distance(plan, start, metres, step=1):
    """Index of the plan point roughly `metres` along from `start`."""
    base = plan[start][0].transform.location
    i = start
    while i + step < len(plan):
        i += step
        if plan[i][0].transform.location.distance(base) >= metres:
            return i
    return len(plan) - 1


def derive_options(plan, lookahead_m=10.0):
    """RoadOption from the plan's own geometry (what the car will actually do).

    trace_route's labels describe its own A* route, computed per call with
    internal state (`_previous_decision`); a plan spliced from many hops plus
    lane walks is not that route, so its labels are not trustworthy here.

    The turn is read off the geometry instead: compare the heading ~10 m before
    the point with the heading ~10 m after it, and take the sign of the cross
    product (CARLA's yaw is positive towards +Y, so a positive cross product is
    a left turn). 35 degrees is the agent library's own threshold
    (local_planner._compute_connection), so the labels mean what the agent's
    consumers already expect. Only points at or approaching a junction get
    LEFT/RIGHT -- a curving lane is not an instruction to turn, and labelling
    every arc would make BehaviorAgent brake at every bend (behavior_agent
    run_step case 3).
    """
    out = []
    n = len(plan)
    for i, (wp, _opt) in enumerate(plan):
        before = plan[max(0, i - 1)][0]
        j = i
        while j > 0 and plan[j][0].transform.location.distance(
                wp.transform.location) < lookahead_m:
            j -= 1
        before = plan[j][0]
        after = plan[_point_at_distance(plan, i, lookahead_m)][0]
        v1x, v1y = (wp.transform.location.x - before.transform.location.x,
                    wp.transform.location.y - before.transform.location.y)
        v2x, v2y = (after.transform.location.x - wp.transform.location.x,
                    after.transform.location.y - wp.transform.location.y)
        cross = v1x * v2y - v1y * v2x
        turn_deg = math.degrees(math.atan2(cross, v1x * v2x + v1y * v2y))
        near_junction = wp.is_junction or after.is_junction or before.is_junction
        if not near_junction or abs(turn_deg) < 35.0:
            out.append((wp, RoadOption.LANEFOLLOW))
        elif turn_deg > 0:
            out.append((wp, RoadOption.LEFT))
        else:
            out.append((wp, RoadOption.RIGHT))
    return out


def main():
    preset_id = sys.argv[1] if len(sys.argv) > 1 else "loop"
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    data = json.load(open(ROUTES, encoding="utf-8"))
    preset = next(p for p in data["presets"] if p["id"] == preset_id)
    specs = preset["waypoints"]
    print(f"preset {preset_id}: {len(specs)} waypoints, declared {preset['length_m']} m")

    grp = GlobalRoutePlanner(cmap, SAMPLING)
    resolved, plan, stats = build_plan(cmap, grp, specs)
    print(f"hops: walked {stats['walk']}, traced {stats['trace']}, "
          f"same-road walk failures {stats['walk_failed']}")
    print(f"plan: {len(plan)} points, length {plan_length(plan):.1f} m")

    gaps = [(round(a[0].transform.location.distance(b[0].transform.location), 1),
             a[0].road_id, a[0].lane_id, b[0].road_id, b[0].lane_id)
            for a, b in zip(plan, plan[1:])
            if a[0].transform.location.distance(b[0].transform.location) > 6.0]
    print(f"hops > 6 m: {len(gaps)} {gaps[:6]}")

    worst = sorted(((round(min(w.transform.location.distance(p.transform.location)
                               for p, _ in plan), 2), i, w.road_id, w.lane_id)
                    for i, w in enumerate(resolved) if w is not None), reverse=True)
    print("worst preset-waypoint distance to plan:", worst[:6])

    opts = {}
    for _wp, o in derive_options(plan):
        opts[str(o)] = opts.get(str(o), 0) + 1
    print("road options:", opts)

    lanes = {}
    for wp, _o in plan:
        lanes[str(wp.lane_type)] = lanes.get(str(wp.lane_type), 0) + 1
    print("lane types in plan:", lanes)

    if len(plan) > 3:
        p0, p2 = plan[0][0], plan[2][0]
        brg = math.degrees(math.atan2(p2.transform.location.y - p0.transform.location.y,
                                      p2.transform.location.x - p0.transform.location.x)) % 360
        print(f"plan[0] {p0.road_id}/{p0.lane_id} yaw={p0.transform.rotation.yaw:.1f} "
              f"bearing(plan[0]->plan[2])={brg:.1f} "
              f"delta={(brg - p0.transform.rotation.yaw + 180) % 360 - 180:+.1f}")

    json.dump({"preset": preset_id, "stats": stats,
               "length_m": round(plan_length(plan), 2),
               "plan": [{"x": round(w.transform.location.x, 2),
                         "y": round(w.transform.location.y, 2),
                         "z": round(w.transform.location.z, 2),
                         "yaw": round(w.transform.rotation.yaw, 2),
                         "road_id": w.road_id, "lane_id": w.lane_id,
                         "lane_type": str(w.lane_type),
                         "option": str(o)} for w, o in derive_options(plan)],
               "preset_points": [{"x": round(w.transform.location.x, 2),
                                  "y": round(w.transform.location.y, 2),
                                  "road_id": w.road_id, "lane_id": w.lane_id}
                                 for w in resolved if w is not None]},
              open(f"plan2-{preset_id}.json", "w", encoding="utf-8"), indent=1)
    print(f"wrote plan2-{preset_id}.json")


if __name__ == "__main__":
    main()

"""Trace one hop and print the roads it walks, to explain a detour."""
from __future__ import annotations

import sys

import carla

sys.path.insert(0, r"C:\GameProject\CarlaUE5\carla\PythonAPI\carla")
from agents.navigation.global_route_planner import GlobalRoutePlanner  # noqa: E402


def main():
    road_a, lane_a, s_a = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
    road_b, lane_b, s_b = int(sys.argv[4]), int(sys.argv[5]), float(sys.argv[6])
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    a = cmap.get_waypoint_xodr(road_a, lane_a, s_a)
    b = cmap.get_waypoint_xodr(road_b, lane_b, s_b)
    print(f"a={a.road_id}/{a.lane_id}@{a.s:.2f} junction={a.is_junction}")
    print(f"b={b.road_id}/{b.lane_id}@{b.s:.2f} junction={b.is_junction}")
    snapped_a = cmap.get_waypoint(a.transform.location)
    snapped_b = cmap.get_waypoint(b.transform.location)
    print(f"get_waypoint(a)={snapped_a.road_id}/{snapped_a.lane_id}@{snapped_a.s:.2f}")
    print(f"get_waypoint(b)={snapped_b.road_id}/{snapped_b.lane_id}@{snapped_b.s:.2f}")

    grp = GlobalRoutePlanner(cmap, 2.0)
    trace = grp.trace_route(a.transform.location, b.transform.location)
    print(f"trace: {len(trace)} points")
    prev = None
    for wp, opt in trace:
        key = (wp.road_id, wp.lane_id)
        if key != prev:
            print(f"   road {wp.road_id:>4} lane {wp.lane_id:>3} s={wp.s:7.2f} "
                  f"opt={opt} junction={wp.is_junction}")
            prev = key
    total = sum(p[0].transform.location.distance(q[0].transform.location)
                for p, q in zip(trace, trace[1:]))
    print(f"trace length: {total:.1f} m; straight: "
          f"{a.transform.location.distance(b.transform.location):.1f} m")


if __name__ == "__main__":
    main()

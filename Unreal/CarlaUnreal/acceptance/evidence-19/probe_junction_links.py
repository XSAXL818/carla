"""Can a car really drive these junctions? Ask the map's own lane links.

The preset generator links roads from the .xodr <link>/<junction> elements, which
says two roads are topologically joined. CARLA's lane-level topology is what the
ego can actually follow, so a preset edge only exists if waypoint.next() walks
it. This prints the junction hops that trace_route had to detour around.
"""
from __future__ import annotations

import carla


def dump(cmap, road, lane, s, label):
    wp = cmap.get_waypoint_xodr(road, lane, s)
    print(f"\n{label}: road {road} lane {lane} s {s} -> "
          f"{None if wp is None else (wp.road_id, wp.lane_id, round(wp.s,1), str(wp.lane_type), 'junction' if wp.is_junction else 'road')}")
    if wp is None:
        return
    nxt = wp.next(2.0)
    for cand in nxt:
        print(f"    next -> road {cand.road_id} lane {cand.lane_id} "
              f"s {cand.s:.1f} type {cand.lane_type} "
              f"{'JUNCTION' if cand.is_junction else ''}")


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    dump(cmap, 2, -1, 10.08, "end of road 2 (+s lane)")
    dump(cmap, 676, 1, 28.67, "start of road 676 (-s lane)")
    dump(cmap, 5, -1, 105.75, "end of road 5 (+s lane)")
    dump(cmap, 516, 1, 42.19, "start of road 516 (-s lane)")


if __name__ == "__main__":
    main()

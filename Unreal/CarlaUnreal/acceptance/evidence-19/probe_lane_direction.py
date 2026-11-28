"""Which way does a lane travel? Empirical check on the live map.

CARLA's convention is negative lane id = right side of the reference line =
travel along +s, positive = left side = travel along -s. The preset generator
must not emit a waypoint whose s-direction contradicts its lane, so this pins
the convention down on the real Town10HD map instead of trusting the docs.
"""
from __future__ import annotations

import carla


def bearing(a, b):
    import math
    return math.degrees(math.atan2(b.y - a.y, b.x - a.x)) % 360.0


def main():
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(60.0)
    cmap = client.get_world().get_map()
    print("map:", cmap.name)
    for road in (5, 469, 735, 12, 676):
        for lane in (1, 2, -1, -2):
            a = cmap.get_waypoint_xodr(road, lane, 2.0)
            b = cmap.get_waypoint_xodr(road, lane, 8.0)
            if a is None or b is None:
                print(f"road {road} lane {lane}: absent")
                continue
            brg = bearing(a.transform.location, b.transform.location)
            print(f"road {road} lane {lane:>2} type={str(a.lane_type):<22} "
                  f"yaw={a.transform.rotation.yaw:7.1f} "
                  f"bearing(s 2->8)={brg:7.1f} "
                  f"{'s+ direction' if abs((brg - a.transform.rotation.yaw + 180) % 360 - 180) < 45 else 'AGAINST yaw'}")


if __name__ == "__main__":
    main()

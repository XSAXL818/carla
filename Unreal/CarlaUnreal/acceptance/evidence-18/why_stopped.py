"""Why did the ego stop where it stopped? Ground truth from the live map.

Reads a run's metrics.csv (backend's own 10 Hz samples), takes the final pose and
asks the map what is there: junction or not, nearby traffic lights and their
state, nearest waypoint lane, and the distance to the preset's last waypoint.
Also prints the throttle/brake profile so a "held" stop is distinguishable from a
"commanded to move but stuck" stop.
"""
from __future__ import annotations

import csv
import json
import math
import sys

import carla

csv_path = sys.argv[1]
routes_json = sys.argv[2]
preset_id = sys.argv[3] if len(sys.argv) > 3 else "loop"

rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))
print(f"{len(rows)} metric rows, t={rows[0]['t']}..{rows[-1]['t']} s")
print("tail:")
for r in rows[-6:]:
    print(f"  t={float(r['t']):7.2f} x={float(r['x']):8.2f} y={float(r['y']):8.2f} "
          f"yaw={float(r['yaw']):7.1f} v={float(r['speed_kmh']):6.2f} "
          f"thr={float(r['throttle']):.3f} brk={float(r['brake']):.3f}")
last = rows[-1]
px, py = float(last["x"]), float(last["y"])

# when did it stop and what was it commanded to do while stopped?
stopped = [r for r in rows if float(r["speed_kmh"]) < 0.6]
if stopped:
    print(f"\nfirst near-zero-speed row: t={stopped[0]['t']} "
          f"({len(stopped)} such rows, {100*len(stopped)/len(rows):.0f}%)")
    thr = [float(r["throttle"]) for r in stopped]
    brk = [float(r["brake"]) for r in stopped]
    print(f"while stopped: throttle min/max {min(thr):.3f}/{max(thr):.3f}, "
          f"brake min/max {min(brk):.3f}/{max(brk):.3f}")
    print(f"rows with throttle>=0.05 while stopped: {sum(1 for t in thr if t >= 0.05)}")

client = carla.Client("127.0.0.1", 2000)
client.set_timeout(30.0)
world = client.get_world()
cmap = world.get_map()
wp = cmap.get_waypoint(carla.Location(x=px, y=py, z=float(last["z"]) + 0.5))
print(f"\nmap on server: {cmap.name}")
print(f"final waypoint: road={wp.road_id} lane={wp.lane_id} s={wp.s:.1f} "
      f"lane_type={wp.lane_type} is_junction={wp.is_junction}")

data = json.load(open(routes_json, encoding="utf-8"))
preset = next(p for p in data["presets"] if p["id"] == preset_id)
tw = preset["waypoints"][-1]
end = cmap.get_waypoint_xodr(int(tw["road_id"]), int(tw["lane_id"]), float(tw["s"]))
if end is not None:
    d = math.dist((px, py), (end.transform.location.x, end.transform.location.y))
    print(f"preset last waypoint: road={tw['road_id']} lane={tw['lane_id']} s={tw['s']} "
          f"at ({end.transform.location.x:.2f},{end.transform.location.y:.2f}) -> "
          f"ego stopped {d:.2f} m from it")
first = preset["waypoints"][0]
fw = cmap.get_waypoint_xodr(int(first["road_id"]), int(first["lane_id"]), float(first["s"]))
if fw is not None:
    d0 = math.dist((px, py), (fw.transform.location.x, fw.transform.location.y))
    print(f"preset start waypoint at ({fw.transform.location.x:.2f},"
          f"{fw.transform.location.y:.2f}) -> {d0:.1f} m away")

print("\ntraffic lights within 40 m of the final pose:")
for a in world.get_actors().filter("traffic.traffic_light*"):
    loc = a.get_location()
    d = math.dist((px, py), (loc.x, loc.y))
    if d < 40:
        print(f"  id={a.id} {a.type_id} pole={a.get_pole_index()} state={a.state} "
              f"({loc.x:.1f},{loc.y:.1f}) {d:.1f} m")

print("\nvehicles around the final pose (radius 25 m):")
for a in world.get_actors().filter("vehicle.*"):
    loc = a.get_location()
    d = math.dist((px, py), (loc.x, loc.y))
    if d < 25:
        v = a.get_velocity()
        print(f"  id={a.id} {a.type_id} {d:.1f} m speed="
              f"{math.sqrt(v.x**2+v.y**2+v.z**2)*3.6:.1f} km/h")

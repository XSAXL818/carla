"""Independent probe: resolve every Town10HD preset waypoint on the LIVE map.

Does not import the backend. Reads data/routes/Town10HD.json verbatim and asks
carla.Map.get_waypoint_xodr(road_id, lane_id, s) for each waypoint with exactly
the lane_id stored in the file (no fallback probing), which is the strict reading
of "the preset resolves on the map".

Usage: python probe_route_resolve.py <routes.json> [--json out.json]
"""
from __future__ import annotations

import json
import sys

import carla


def main() -> int:
    routes_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None
    data = json.load(open(routes_path, encoding="utf-8"))

    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(30.0)
    world = client.get_world()
    cmap = world.get_map()
    print(f"map on server : {cmap.name}")
    print(f"map file      : {routes_path}")
    print(f"roads in xodr : {data.get('roads')} junctions={data.get('junctions')}")

    result = {"map_on_server": cmap.name, "routes_json": routes_path, "presets": []}
    grand_ok = grand_bad = 0
    for preset in data["presets"]:
        rows = []
        ok = bad = 0
        non_driving = []
        for i, wp in enumerate(preset["waypoints"]):
            road_id, lane_id, s = int(wp["road_id"]), int(wp["lane_id"]), float(wp["s"])
            try:
                got = cmap.get_waypoint_xodr(road_id, lane_id, s)
            except Exception as exc:  # noqa: BLE001
                rows.append({"i": i, "road_id": road_id, "lane_id": lane_id, "s": s,
                             "ok": False, "error": f"{type(exc).__name__}: {exc}"})
                bad += 1
                continue
            if got is None:
                rows.append({"i": i, "road_id": road_id, "lane_id": lane_id, "s": s,
                             "ok": False})
                bad += 1
                continue
            ok += 1
            if str(got.lane_type) != "Driving":
                non_driving.append({"i": i, "road_id": road_id, "lane_id": lane_id,
                                    "s": s, "lane_type": str(got.lane_type)})
            rows.append({"i": i, "road_id": road_id, "lane_id": lane_id, "s": s, "ok": True,
                         "lane_type": str(got.lane_type),
                         "x": round(got.transform.location.x, 2),
                         "y": round(got.transform.location.y, 2)})
        fails = [r for r in rows if not r["ok"]]
        print(f"\npreset {preset['id']:<9} declared_len={preset.get('length_m')} m "
              f"waypoints={len(preset['waypoints'])} resolved={ok} failed={bad}")
        for f in fails:
            print(f"   FAIL idx={f['i']} road={f['road_id']} lane={f['lane_id']} s={f['s']}"
                  + (f" err={f.get('error')}" if f.get("error") else ""))
        if non_driving:
            print(f"   non-driving lanes: {len(non_driving)} -> {non_driving[:6]}")
        grand_ok += ok
        grand_bad += bad
        result["presets"].append({"id": preset["id"], "waypoints": len(preset["waypoints"]),
                                  "resolved": ok, "failed": bad,
                                  "non_driving": non_driving, "rows": rows})

    print(f"\nTOTAL resolved={grand_ok} failed={grand_bad} "
          f"of {grand_ok + grand_bad}")
    result["total_resolved"] = grand_ok
    result["total_failed"] = grand_bad
    if out_path:
        with open(out_path, "w", encoding="utf-8") as fh:
            json.dump(result, fh, ensure_ascii=False, indent=1)
        print(f"wrote {out_path}")
    return 0 if grand_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

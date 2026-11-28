"""Compare Town10HD.json between HEAD~1 and HEAD: schema, lengths, waypoint ids."""
import json
import subprocess

REL = "Unreal/CarlaUnreal/Source/TestPlatform/Platform/backend/data/routes/Town10HD.json"


def load(ref):
    out = subprocess.run(["git", "show", f"{ref}:{REL}"], capture_output=True, text=True,
                         encoding="utf-8", cwd=r"C:\GameProject\CarlaUE5\carla")
    if out.returncode != 0:
        raise SystemExit(out.stderr)
    return json.loads(out.stdout)


def main():
    old, new = load("HEAD~1"), load("HEAD")
    print("top-level keys identical:", list(old.keys()) == list(new.keys()), list(new.keys()))
    print("map/xodr/roads/junctions unchanged:",
          all(old[k] == new[k] for k in ("map", "xodr", "roads", "junctions")))
    total_lane_changes = 0
    for po, pn in zip(old["presets"], new["presets"]):
        print(f"\npreset {pn['id']} keys_same={list(po.keys()) == list(pn.keys())} "
              f"name_same={po['name'] == pn['name']} "
              f"length={po['length_m']}->{pn['length_m']} "
              f"nwp={len(po['waypoints'])}->{len(pn['waypoints'])}")
        diffs = [(i, o["lane_id"], n["lane_id"])
                 for i, (o, n) in enumerate(zip(po["waypoints"], pn["waypoints"])) if o != n]
        total_lane_changes += len(diffs)
        print(f"   waypoint dict diffs: {len(diffs)} sample={diffs[:8]}")
        print(f"   road_id list same: {[w['road_id'] for w in po['waypoints']] == [w['road_id'] for w in pn['waypoints']]}")
        print(f"   s list same:       {[w['s'] for w in po['waypoints']] == [w['s'] for w in pn['waypoints']]}")
        print(f"   roads field same:  {po.get('roads') == pn.get('roads')}  note same: {po.get('note') == pn.get('note')}")
    print(f"\ntotal waypoint changes across presets: {total_lane_changes}")


if __name__ == "__main__":
    main()

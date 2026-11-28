"""Summarise an independent sample log: traffic actor identity + motion."""
from __future__ import annotations

import json
import sys

path = sys.argv[1]
rows = [json.loads(line) for line in open(path, encoding="utf-8")]
samples = [r for r in rows if r.get("tag") == "sample"]
print(f"{len(samples)} samples; wall {samples[0]['wall']} .. {samples[-1]['wall']}")
ids: dict[int, dict] = {}
for s in samples:
    for v in s.get("other_vehicles") or []:
        if "id" not in v:
            continue
        rec = ids.setdefault(v["id"], {"type": v.get("type"), "first": v, "last": v,
                                       "max_speed": 0.0, "speeds": []})
        rec["last"] = v
        rec["max_speed"] = max(rec["max_speed"], v.get("speed") or 0.0)
        rec["speeds"].append(v.get("speed") or 0.0)
print(f"distinct non-ego vehicle actors seen: {len(ids)}")
for aid, rec in sorted(ids.items()):
    moved = ((rec["last"]["x"] - rec["first"]["x"]) ** 2 +
             (rec["last"]["y"] - rec["first"]["y"]) ** 2) ** 0.5
    sp = rec["speeds"]
    nonzero = sum(1 for x in sp if x > 0.5)
    print(f"  id={aid:5d} {rec['type']:<28} moved {moved:7.1f} m  max {rec['max_speed']:6.2f} km/h "
          f" moving_samples {nonzero}/{len(sp)}  start({rec['first']['x']:.1f},{rec['first']['y']:.1f}) "
          f" end({rec['last']['x']:.1f},{rec['last']['y']:.1f})")

ego = [s for s in samples if s.get("ego_actor")]
if ego:
    p0 = (ego[0]["ego_actor"]["x"], ego[0]["ego_actor"]["y"])
    p1 = (ego[-1]["ego_actor"]["x"], ego[-1]["ego_actor"]["y"])
    print(f"\nego: id={ego[0]['ego_actor']['id']} {ego[0]['ego_actor']['type']} "
          f"path length "
          f"{sum(((ego[i+1]['ego_actor']['x']-ego[i]['ego_actor']['x'])**2 + (ego[i+1]['ego_actor']['y']-ego[i]['ego_actor']['y'])**2)**0.5 for i in range(len(ego)-1)):.1f} m")
    print(f"ego speed first/last: {ego[0]['ego_speed']} / {ego[-1]['ego_speed']} km/h")

print("\nbackend self-report vs independent count:")
for s in samples[::max(1, len(samples) // 12)]:
    print(f"  t={s['elapsed']:6.1f} backend.traffic={s['traffic']} "
          f"independent_others={len(s.get('other_vehicles') or [])} state={s['state']} "
          f"ego_speed={s['ego_speed']}")

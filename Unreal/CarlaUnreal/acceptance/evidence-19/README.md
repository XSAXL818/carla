# Round 19 — the ego drives the configured route preset

Commit `8ef3a03cb` · Town10HD only · branch `dev`

## What was wrong (measured by round 18)

`_build_agent()` called `agent.set_destination(<last waypoint>)`. `set_destination()`
plans **from the ego's current position to that single point**, so the preset's
intermediate waypoints were never used. Consequences, all reproduced here before the fix:

* `loop` waypoint 0 stored `lane_id = -1`; the backend probed lane 1 first, which on
  road 5 points the *other* way (lane 1 yaw 15.6°, lane -1 yaw -164.4°), so the ego
  spawned facing backwards.
* 494.6 m driven to reach a destination 94 m away.
* The painted preset polyline and the driven path coincided ~20 % of the time.
* `cross` carried 8 waypoints on `shoulder`/`sidewalk` lanes.

## Root causes found while fixing it

1. **`lane_id = -1` meant two different things.** It is the "any driving lane" sentinel
   in the old preset files, and it is *also* a real lane on this map (the right lane
   travelling +s). A stored `-1` therefore read as "right lane" to the generator and
   "probe from the left lane first" to the backend. The sentinel is now `0` (OpenDRIVE
   lane 0 is the reference line, never drivable), and the backend validates a stored
   concrete id against the travel direction instead of trusting it.
2. **The generator assumed every junction connection leaves the incoming road at its
   `end`.** The direction is in the connection's `laneLink` lanes, not its attributes:
   `from` lane id < 0 travels +s (leaves at the end), > 0 leaves at the start. **42 of
   this map's 85 connections** feed from the other end, so 42 junction hops asked the
   car to drive the incoming road the wrong way.
3. **The generator's graph was bidirectional.** OpenDRIVE `<link>` is a two-way
   topological statement; a road can only be driven the way its lanes point. On
   Town10HD, **231 of 546 edges** survive a direction-strict prune. The 20 roads with
   no `driving` lane at all (258, 718, 769, 942, …) are the `shoulder`/`sidewalk`
   connectors the `cross` preset was driving on.
4. **`trace_route()` takes locations, not waypoints.** It localizes each end with
   `map.get_waypoint()` = the *nearest* lane, and junction connectors overlap in space,
   so **11 of the 152** preset waypoints localized onto a different road and the trace
   detoured up to 600 m. `_relocalize()` walks along the intended lane to a location
   that localizes back onto it; all 11 are fixable, shifting 1.8–6.2 m.
5. **`--limit` overwrote `index.json`.** Regenerating one map replaced the whole index
   with that single map, so the map picker silently dropped from 9 maps to 1. Now merged.

## The fix

* `session_runner._build_plan()` — plan from the preset, not from a destination:
  resolve every waypoint (`_waypoint_at()` + `_waypoint_spec_travel_sign()`), walk the
  lane with `next()` when two consecutive waypoints share it, `trace_route()` only
  across a road change, de-duplicate.
* `_label_road_options()` — `RoadOption` measured on the plan's own geometry
  (heading delta over ~10 m either side, cross product for left/right, the agent
  library's own 35° threshold, LEFT/RIGHT only at junctions).
* `_apply_plan()` — `set_global_plan()`; `set_destination()` is gone from the path.
  `planner_fallback` still fires and gives `BasicAgent` the **same** plan.
* `_spawn_ego()` — spawn on the plan's first waypoint, facing its first segment
  (4 m-baseline bearing). Map spawn points only as a fallback, still re-oriented.
* `_cache_route_points()` — the overlay paints the plan, so drawn == driven.
* `GET /sessions/{id}/plan.json` — the plan as data, for outside measurement.
* `gen_route_presets.py` — `drivable_dirs()` + `prune_graph()`, `laneLink`-aware
  junction directions, direction-strict `lane_at()`, sentinel `-1 → 0`, merging index.

## Measured result (Town10HD, `loop`, real `-game` session)

`measure-loop-final.json`, 1075 samples, session `20261128-162320-189196`.

| quantity | before (round 18) | after |
|---|---|---|
| preset waypoints unresolved | 0 | 0 |
| preset waypoints on non-driving lanes | 8 (`cross`) | **0** |
| preset waypoints against the lane direction | 64 sentinel / 8 wrong-way | **0** |
| spawn yaw vs plan first segment | −171.6° (facing backwards) | **+0.01°** |
| spawn distance to plan start | n/a (nearest map spawn point) | **0.01 m** |
| samples within 3 m of the route | **20.4 %** | **100.0 %** |
| samples within 1 m of the route | 20.6 % (5 m) | **94.0 %** |
| median distance to the route | 17.0 m | **0.11 m** |
| driven path length | 494.6 m (destination 94 m away) | **514.8 m** |
| plan length | 667.7 m (preset polyline) | **541.9 m** (263 points) |
| `ego_arrived` | yes | **yes** (t=105.9 s, 0 waypoints left, 2.2 m from the end) |
| `ego_stalled` (false) | no | **no** |
| `planner_fallback` | no | **no** |

`cross` (the preset with the sidewalk waypoints): 48 preset waypoints, 0 unresolved,
plan 407 points / 842.3 m, **every plan point on a `Driving` lane**, spawn on the plan
start (0.1 m, 0.0° delta), and the car drove the route to the far side of the map
(`cross-events.json`).

## Files

| file | what |
|---|---|
| `app/session_runner.py` | plan builder, direction-aware resolution, plan-based spawn, plan overlay, plan in `/sessions/current` |
| `app/sessions_api.py` | `GET /sessions/{id}/plan.json` |
| `tools/gen_route_presets.py` | direction-strict graph, `laneLink` junction directions, `lane_at()`, sentinel, index merge |
| `data/routes/Town10HD.json` | regenerated (schema unchanged: same keys, same waypoint keys) |
| `data/routes/index.json` | Town10HD entry only; the other 14 maps byte-identical |
| `docs/architecture.md` | §6 route resolution/planning/spawn/overlay, §3 new endpoint, §14 verified/unverified |
| `Docs/WORKLOG.md` | 16:30 entry |
| `CHANGELOG.md` | Test platform section entry |

## Files in this directory

`probe_generator*.py` (offline generator experiments), `probe_lane_direction.py`
(empirical lane-direction convention), `probe_segments.py` / `probe_one_hop.py`
(per-hop trace diagnosis), `probe_localize.py` / `probe_relocalize.py` (the 11
mislocalized waypoints), `probe_ab.py` / `probe_detours.py` (algorithm choice, residual
detour check), `prototype_plan.py` / `prototype_plan2.py` (the two builders),
`validate_routes.py` (strict per-waypoint validation), `run_measure.py` (the measured
run), `probe_tickrate.py`, `diag_ego.py`, `shot.py` / `capture_static_overlay.py` /
`count_route_pixels.py` / `diff_overlay.py` / `overlay_isolate.py` (screenshots),
`measure-loop-final.json`, `samples.jsonl`, `cross-events.json`, `shots/`.

## Not verified

* The other 8 UI maps (out of scope by the project's own statement).
* `arterial` / `cross` / `turns` plan building is verified (0 failures, all Driving
  lanes, 0 wrong-way); only `loop` and `cross` were driven end to end.
* `rec.log` replay.

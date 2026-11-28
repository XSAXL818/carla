"""Session runtime: preview state, run loop, metrics, recorder, export.

Ownership rules (docs/architecture.md §1):
  * the backend is the only ``carla.Client`` that ticks the world;
  * preview keeps the world alive at a low tick rate with traffic parked, so
    selecting a map feels immediate and "entering" the world loads nothing;
  * if the machine cannot keep up with the preview tick, the runner falls back
    to a static world instead of stalling the editor.

Everything CARLA touching is imported lazily so this module loads with the
simulator closed and the API stays usable.
"""
from __future__ import annotations

import csv
import json
import math
import random
import threading
import time
import traceback
import uuid
from pathlib import Path
from typing import Any

from . import carla_bridge, catalog, paths

PREVIEW_HZ = 3.0          # slow tick while configuring
PREVIEW_FALLBACK_HZ = 0.5  # used when the preview tick cannot keep its budget
# Steps the clock with this delta when the simulator will not say what the tick
# advanced (its fixed delta is gone: synchronous mode was lost). The loop keeps
# running and the loss is recorded in the session status and events; it is not a
# reason to end the session.
DEFAULT_FIXED_DELTA = 1.0 / 20.0
_STOPPED = {"stopped", "finished", "error", "created"}

# Route overlay: drawn as one line per segment (this CARLA build's DebugHelper
# has no draw_polyline), refreshed long before its lifetime runs out.
ROUTE_DRAW_LIFE_S = 5.0
ROUTE_DRAW_EVERY_S = 2.0
# "Home" radius for the stall detector: the agent stops a few metres short of
# the destination waypoint, so a stop inside this radius is an arrival.
ARRIVAL_RADIUS_M = 12.0
# Background traffic: keep the ego's spawn clear and spread the cars out.
TRAFFIC_EGO_CLEARANCE_M = 25.0
TRAFFIC_MIN_GAP_M = 25.0
# Plan building: step used to walk a lane, and the lookahead over which the turn
# at a waypoint is measured (both metres). TURN_THRESHOLD_DEG is the agent
# library's own threshold (local_planner._compute_connection), so the RoadOption
# values mean what BasicAgent/BehaviorAgent already expect.
PLAN_SAMPLING_M = 2.0
PLAN_TURN_LOOKAHEAD_M = 10.0
TURN_THRESHOLD_DEG = 35.0


def _carla():
    import carla  # noqa: PLC0415  (lazy on purpose)

    return carla


class SessionError(RuntimeError):
    pass


class Session:
    def __init__(self, session_id: str, config: dict):
        self.id = session_id
        self.config = config
        self.state = "created"
        self.error: str | None = None
        self.created_at = time.time()
        self.started_at: float | None = None
        self.stopped_at: float | None = None

        self.output_dir = paths.RUNS_DIR / session_id
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.client = None
        self.world = None
        self.map = None
        self.ego = None
        self.agent = None
        self.traffic: list[Any] = []
        self.walkers: list[Any] = []
        self.spectator = None

        self.tick = 0
        self.sim_time = 0.0
        self.route_waypoints: list[dict] = []
        self.route_length_m = 0.0
        self.travelled_m = 0.0
        self.events: list[dict] = []
        self.metrics: list[dict] = []
        self.preview_camera: dict | None = None
        self.preview_degraded = False
        self.last_error: str | None = None

        self._thread: threading.Thread | None = None
        self._stop_evt = threading.Event()
        self._lock = threading.Lock()
        self._target_hz = PREVIEW_HZ
        self._running_metrics = False
        self._recording = False
        self._stalled_ticks = 0
        self._stall_reported = False
        self._destination = None
        self._arrived_reported = False
        self._route_draw_error: str | None = None
        self._traffic_report: dict = {}
        self.show_route = bool(config.get("run", {}).get("show_route", True))
        self._route_points: list = []
        self._route_color = None
        # The plan the agent actually follows, and the polyline drawn for it:
        # the overlay has to show what the car drives, not the preset's control
        # points (those are a subset of the plan now, so the drawn line is the
        # plan itself).
        self._plan: list = []
        self._plan_stats: dict = {}
        self.spawn_pose: dict | None = None
        self._spawn_request_location = None
        self._spawn_source = None
        self._grp = None
        # What the simulator confirmed about the clock. `_fixed_delta` is the
        # last delta it really ran at, so losing synchronous mode degrades the
        # metrics to "last known delta" instead of killing the loop with a
        # None.
        self._fixed_delta: float | None = None
        self._requested_delta: float | None = None
        self.sync_lost = False
        self.sync_lost_detail: str | None = None
        self.sync_lost_at: float | None = None
        self.sync_reasserts = 0
        self._last_reassert_at = 0.0

    # ------------------------------------------------------------------ #
    # config helpers
    # ------------------------------------------------------------------ #
    @property
    def map_name(self) -> str:
        return self.config["map"]

    def _traffic_counts(self) -> tuple[int, int]:
        traffic = self.config.get("traffic", {})
        preset_id = traffic.get("preset", "light")
        preset = next((p for p in catalog.TRAFFIC_PRESETS if p["id"] == preset_id), None)
        vehicles = traffic.get("vehicles", preset["vehicles"] if preset else 4)
        walkers = traffic.get("walkers", preset["walkers"] if preset else 6)
        return int(vehicles), int(walkers)

    def _resolve_route(self) -> list[dict]:
        """Preset id or explicit list -> validated [(road_id, lane_id, s)]."""
        route = self.config.get("route", {})
        if route.get("source") == "custom":
            wps = route.get("waypoints") or []
            if len(wps) < 2:
                raise SessionError("自定义路线至少需要 2 个路点")
            return wps
        preset_id = route.get("preset_id")
        data = catalog.routes_for(self.map_name)
        if data is None:
            raise SessionError(f"没有 {self.map_name} 的路线预设")
        preset = next((p for p in data.get("presets", []) if p["id"] == preset_id), None)
        if preset is None:
            raise SessionError(f"{self.map_name} 没有预设 {preset_id}")
        return preset["waypoints"]

    # ------------------------------------------------------------------ #
    # CARLA plumbing
    # ------------------------------------------------------------------ #
    def _connect(self):
        carla = _carla()
        if self.client is None:
            self.client = carla.Client(paths.CARLA_HOST, paths.CARLA_PORT)
            self.client.set_timeout(30.0)
        if self.world is None:
            try:
                self.world = self.client.get_world()
            except Exception as exc:  # noqa: BLE001
                raise SessionError(
                    f"连接 CARLA 失败（{paths.CARLA_HOST}:{paths.CARLA_PORT}）："
                    f"{exc or '请先启动 CarlaUnreal'}") from exc
        try:
            self.map = self.world.get_map()
        except RuntimeError:
            # A world can genuinely have no OpenDrive at all. The config-screen
            # entry level is an empty host *with* an empty OpenDrive (zero roads,
            # Content/TestPlatform/Maps/OpenDrive/L_TestPlatformEntry.xodr), so it
            # maps fine; this is the fallback for any other mapless level. preview()
            # loads the town before anything needs the map, and
            # _compute_preview_camera already handles None.
            self.map = None

    def _current_world_name(self) -> str:
        """Name of the map the simulator is showing, "" when it has none.

        The entry level's empty OpenDrive yields an empty Map whose name never
        matches the requested town, so preview() goes on and loads it. A world
        that cannot build a Map at all raises ``RuntimeError`` here; that is
        still "no map", not an error.
        """
        try:
            return self.world.get_map().name
        except RuntimeError:
            return ""

    def _set_settings(self, hz: float):
        carla = _carla()
        settings = self.world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = 1.0 / max(0.1, hz)
        self.world.apply_settings(settings)
        self._requested_delta = 1.0 / max(0.1, hz)
        self._fixed_delta = self._requested_delta

    def _tick_delta(self) -> float:
        """Seconds this tick advanced; never ``None``.

        ``world.get_settings().fixed_delta_seconds`` is the delta the world is
        running at, but it is not guaranteed to exist: the simulator drops out of
        synchronous mode on its own (``carla.SyncCueTimeout``, or another client
        applying asynchronous settings) and then reports
        ``fixed_delta_seconds = None``. Doing ``self.sim_time += None`` killed the
        tick loop and turned a lost sync mode into ``state=error``, so the loop
        keeps running on the last delta it actually saw and records the loss.
        """
        delta = None
        try:
            delta = self.world.get_settings().fixed_delta_seconds
        except Exception:  # noqa: BLE001  (a lost world must not kill the loop either)
            delta = None
        if delta is not None and delta > 0.0:
            delta = float(delta)
            self._fixed_delta = delta
            if self.sync_lost:
                self.sync_lost = False
                self.sync_lost_detail = None
                self.events.append({
                    "t": self.sim_time,
                    "kind": "sync_mode_restored",
                    "detail": f"同步模式已恢复，固定步长 {delta:.4f} s",
                })
            return delta

        # Synchronous mode is gone: record it once, ask for it back, keep going.
        if not self.sync_lost:
            self.sync_lost = True
            self.sync_lost_at = time.time()
            self.sync_lost_detail = (
                "模拟器不再处于同步模式（fixed_delta_seconds 为空）：仿真继续运行，"
                f"指标按最后已知步长 {self._fallback_delta():.4f} s 计时")
            self.events.append({
                "t": self.sim_time,
                "kind": "sync_mode_lost",
                "detail": self.sync_lost_detail,
            })
            print(f"[session {self.id}] {self.sync_lost_detail}", flush=True)
        self._reassert_sync()
        return self._fallback_delta()

    def _fallback_delta(self) -> float:
        if self._fixed_delta and self._fixed_delta > 0.0:
            return float(self._fixed_delta)
        if self._requested_delta and self._requested_delta > 0.0:
            return float(self._requested_delta)
        return DEFAULT_FIXED_DELTA

    def _reassert_sync(self) -> None:
        """Ask for the intended settings back, at most every 2 s.

        The engine drops synchronous mode when it gives up on a client; a client
        that is alive again (resumed, reconnected) re-applies what it wanted
        instead of silently running free for the rest of the session.
        """
        now = time.time()
        if now - self._last_reassert_at < 2.0:
            return
        self._last_reassert_at = now
        self.sync_reasserts += 1
        try:
            self._set_settings(self._target_hz)
        except Exception:  # noqa: BLE001  (the next tick retries)
            pass

    def _run_async(self) -> None:
        """Spawn requests complete on a server tick; in synchronous mode nobody
        ticks yet, so actor creation must happen while the world runs freely."""
        if self.world is None:
            return
        settings = self.world.get_settings()
        if settings.synchronous_mode:
            settings.synchronous_mode = False
            settings.fixed_delta_seconds = 0.0
            self.world.apply_settings(settings)

    def _apply_weather(self):
        carla = _carla()
        weather_cfg = self.config.get("weather", {})
        preset = weather_cfg.get("preset", "ClearNoon")
        if preset == "custom" and weather_cfg.get("custom"):
            params = carla.WeatherParameters()
            for key, value in weather_cfg["custom"].items():
                if hasattr(params, key):
                    setattr(params, key, value)
            self.world.set_weather(params)
            return
        preset_obj = getattr(carla.WeatherParameters, preset, None)
        if preset_obj is None:
            raise SessionError(f"未知天气预设 {preset}")
        self.world.set_weather(preset_obj)

    def _spawn_point_from_waypoint(self, wp):
        carla = _carla()
        transform = wp.transform
        transform.location.z += 0.3
        return transform

    def _driving_waypoint_near(self, waypoint, max_steps: int = 80):
        """Walk along the lane until a real driving lane is found."""
        carla = _carla()
        current = waypoint
        for _ in range(max_steps):
            if current is None:
                break
            if current.lane_type == carla.LaneType.Driving:
                return current
            following = current.next(2.0)
            current = following[0] if following else None
        return waypoint

    @staticmethod
    def _lane_travel_sign(lane_id: int) -> int:
        """+1 when this lane travels along increasing ``s``, -1 when decreasing.

        Right-hand traffic in OpenDRIVE: lanes with a negative id lie right of
        the reference line and travel +s, positive ids lie left and travel -s.
        Verified on the live Town10HD map: road 5 lane -1 at s 2->8 bears 199.7
        deg, which matches its own yaw (-164.4); lane 1 of the same road bears
        the same 199.7 but its yaw is 15.6, i.e. it faces the other way.
        """
        return 1 if lane_id < 0 else -1

    def _waypoint_at(self, road_id: int, lane_id: int, s: float,
                     travel_sign: int | None = None):
        """Resolve a (road, lane, s) tuple onto a *driving* lane.

        ``travel_sign`` (+1/-1) says which way along the road the route goes
        through this point. A concrete lane id is used verbatim when it really is
        a driving lane travelling that way -- the generator already picked one.
        It is *not* trusted blindly: ``-1`` is a legitimate lane here (the right
        lane travelling +s) and it is also the "any driving lane" sentinel older
        preset files still carry, so the direction check is what tells the two
        apart. Anything that fails it -- the sentinel, a non-driving lane, or a
        lane pointing the wrong way -- falls back to a direction-aware probe.
        Returns None when nothing drivable matches, so the caller can decide
        whether that is fatal (start/end) or skippable.

        The old version took the first Driving lane in a fixed probe order and
        fell back to *any* lane, which is how the ``loop`` preset's first
        waypoint resolved to the opposite carriageway (spawn facing backwards)
        and the ``cross`` preset resolved onto ``shoulder``/``sidewalk`` lanes.
        """
        carla = _carla()
        if lane_id not in (None, 0):
            try:
                waypoint = self.map.get_waypoint_xodr(road_id, lane_id, s)
            except Exception:  # noqa: BLE001
                waypoint = None
            if waypoint is not None and waypoint.lane_type == carla.LaneType.Driving:
                if travel_sign is None or self._lane_travel_sign(
                        waypoint.lane_id) == travel_sign:
                    return waypoint

        found = []
        for lane in (1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6):
            try:
                waypoint = self.map.get_waypoint_xodr(road_id, lane, s)
            except Exception:  # noqa: BLE001
                waypoint = None
            if waypoint is None or waypoint.lane_type != carla.LaneType.Driving:
                continue
            if travel_sign is not None and self._lane_travel_sign(
                    waypoint.lane_id) != travel_sign:
                continue
            found.append(waypoint)
        if found:
            return found[0]
        # No driving lane on the requested side. Rather than hand the car a lane
        # that points the wrong way (the original defect), report "unresolvable"
        # and let the caller skip the point.
        return None

    def _waypoint_spec_travel_sign(self, index: int) -> int | None:
        """Which way the preset travels through ``self.route_waypoints[index]``.

        Taken from the neighbouring waypoint on the same road: a positive delta
        in ``s`` towards that neighbour means the route goes along +s there, and
        the neighbour's own ``s`` has to be subtracted in the right order (an
        entry point is followed by the exit, an exit point follows the entry).
        Without such a neighbour (a road visited only once, one point) the
        direction is unknown and the caller passes None.
        """
        specs = self.route_waypoints
        if not 0 <= index < len(specs):
            return None
        spec = specs[index]
        road_id = int(spec["road_id"])
        s = float(spec["s"])
        for other, towards_next in (
                (specs[index + 1] if index + 1 < len(specs) else None, True),
                (specs[index - 1] if index else None, False)):
            if other is None or int(other["road_id"]) != road_id:
                continue
            # towards the next waypoint: s grows (or shrinks) as we leave here
            delta = (float(other["s"]) - s) if towards_next else (s - float(other["s"]))
            if abs(delta) > 1e-6:
                return 1 if delta > 0 else -1
        return None

    def _resolve_route_waypoints(self) -> list:
        """Every preset waypoint as a carla.Waypoint on a driving lane, in order."""
        resolved = []
        for i, spec in enumerate(self.route_waypoints):
            waypoint = self._waypoint_at(int(spec["road_id"]), int(spec["lane_id"]),
                                         float(spec["s"]),
                                         self._waypoint_spec_travel_sign(i))
            resolved.append(waypoint)
        return resolved

    # ------------------------------------------------------------------ #
    # plan building
    # ------------------------------------------------------------------ #
    def _import_agents(self, module: str, name: str):
        """Import a navigation agent module from the CARLA checkout.

        The pip wheel's ``carla`` is a bare extension module (``carla.agents``
        does not exist), and this machine's site-packages has an *unrelated*
        ``agents`` package that imports tensorflow and dies. So the checkout's
        ``PythonAPI/carla`` has to be ahead of site-packages on sys.path *before*
        the first ``agents`` import anywhere in the process -- the path insert
        used to live only in ``_build_agent()``, which is late enough that an
        earlier import could poison ``sys.modules['agents']``.
        """
        import sys  # noqa: PLC0415

        checkout = str(paths.PYTHON_API_CARLA)
        if paths.PYTHON_API_CARLA.is_dir() and sys.path[:1] != [checkout]:
            sys.path.insert(0, checkout)
        import importlib  # noqa: PLC0415

        return getattr(importlib.import_module(module), name)

    def _road_option_enum(self):
        return self._import_agents("agents.navigation.local_planner", "RoadOption")

    def _global_planner(self):
        """A GlobalRoutePlanner over this session's map, built once."""
        if getattr(self, "_grp", None) is None:
            GlobalRoutePlanner = self._import_agents(
                "agents.navigation.global_route_planner", "GlobalRoutePlanner")
            self._grp = GlobalRoutePlanner(self.map, PLAN_SAMPLING_M)
        return self._grp

    def _relocalize(self, waypoint):
        """A location on ``waypoint``'s own lane that ``map.get_waypoint()`` maps back to it.

        ``trace_route`` takes *locations* and localizes them with
        ``map.get_waypoint()``, which returns the nearest lane -- and junction
        connectors overlap in space, so 11 of the 152 Town10HD preset waypoints
        localize onto a different road. The trace then starts or ends somewhere
        else and detours up to 600 m around the block. Walking a little way
        along the intended lane finds a point that localizes correctly.
        """
        for direction in (1, -1):
            current = waypoint
            for _ in range(14):
                snapped = self.map.get_waypoint(current.transform.location)
                if (snapped.road_id == waypoint.road_id
                        and snapped.lane_id == waypoint.lane_id):
                    return current.transform.location
                nxt = (current.next(PLAN_SAMPLING_M) if direction > 0
                       else current.previous(PLAN_SAMPLING_M))
                if not nxt:
                    break
                current = nxt[0]
                if (current.road_id != waypoint.road_id
                        or current.lane_id != waypoint.lane_id):
                    break
        return None

    @staticmethod
    def _walk_same_lane(wp_a, wp_b):
        """Waypoints from ``wp_a`` to ``wp_b`` along one lane, or None.

        Same road, same section, same lane, and ``wp_b`` ahead of ``wp_a`` *in
        that lane's travel direction*: then the lane geometry is the plan, and
        asking trace_route for it would only re-derive (and sometimes
        mis-derive) the same points.
        """
        if (wp_a.road_id != wp_b.road_id or wp_a.lane_id != wp_b.lane_id
                or wp_a.section_id != wp_b.section_id):
            return None
        travel = 1 if wp_a.lane_id < 0 else -1
        if (wp_b.s - wp_a.s) * travel <= 0:
            return None
        out = [wp_a]
        current = wp_a
        for _ in range(4000):
            if current.transform.location.distance(
                    wp_b.transform.location) <= PLAN_SAMPLING_M * 1.5:
                break
            nxt = current.next(PLAN_SAMPLING_M)
            if not nxt:
                return None
            nxt = nxt[0]
            if nxt.road_id != wp_a.road_id or nxt.lane_id != wp_a.lane_id:
                return None
            current = nxt
            out.append(current)
        out.append(wp_b)
        return out

    def _trace_hop(self, wp_a, wp_b):
        """Plan points from ``wp_a`` to ``wp_b`` across a road change."""
        grp = self._global_planner()
        start = self._relocalize(wp_a) or wp_a.transform.location
        end = self._relocalize(wp_b) or wp_b.transform.location
        trace = grp.trace_route(start, end)
        if not trace:
            return None
        # trace_route's own endpoints are the *nearest* waypoints to the
        # locations it was handed, so its head and tail are replaced with the
        # preset waypoints: the plan has to visit them, not something nearby.
        return [wp_a] + [wp for wp, _opt in trace[1:-1]] + [wp_b]

    def _build_plan(self) -> list:
        """The configured route as ``[(carla.Waypoint, RoadOption)]``.

        Built from the preset, not from a destination point: every preset
        waypoint is resolved onto a driving lane travelling the right way, then
        the gaps between consecutive waypoints are filled -- by walking the lane
        when both ends share one, and by ``trace_route`` (which keeps the plan on
        the intended roads) when the road changes. ``set_destination()`` is never
        called: it re-plans from the ego's current position to a single point,
        which is exactly how the preset's intermediate waypoints used to be
        thrown away.
        """
        resolved = self._resolve_route_waypoints()
        if len(resolved) < 2:
            return []
        raw: list = []
        stats = {"hops": 0, "walked": 0, "traced": 0, "failed": 0}
        for wp_a, wp_b in zip(resolved, resolved[1:]):
            if wp_a is None or wp_b is None:
                stats["failed"] += 1
                continue
            stats["hops"] += 1
            walked = self._walk_same_lane(wp_a, wp_b)
            if walked is not None:
                raw.extend(walked)
                stats["walked"] += 1
                continue
            hopped = self._trace_hop(wp_a, wp_b)
            if hopped is None:
                stats["failed"] += 1
                self.events.append({
                    "t": self.sim_time,
                    "kind": "plan_hop_failed",
                    "detail": (f"road {wp_a.road_id}/{wp_a.lane_id} -> "
                               f"{wp_b.road_id}/{wp_b.lane_id} 无法规划，跳过该段"),
                })
                continue
            raw.extend(hopped)
            stats["traced"] += 1

        plan = []
        for waypoint in raw:
            if plan and self._same_plan_point(plan[-1][0], waypoint):
                continue
            plan.append((waypoint, None))

        labelled = self._label_road_options(plan)
        self._plan_stats = {**stats, "points": len(labelled),
                            "length_m": round(self._plan_length(labelled), 1),
                            "unresolved_waypoints": sum(1 for w in resolved if w is None)}
        detail = (f"路线计划：{stats['hops']} 段（沿车道 {stats['walked']}，"
                  f"跨路 {stats['traced']}，失败 {stats['failed']}），"
                  f"{len(labelled)} 点，{self._plan_stats['length_m']} m，"
                  f"预设航点 {len(resolved)} 个")
        self.events.append({"t": self.sim_time, "kind": "route_plan", "detail": detail})
        print(f"[session {self.id}] {detail}", flush=True)
        return labelled

    @staticmethod
    def _same_plan_point(a, b) -> bool:
        return (a.road_id == b.road_id and a.section_id == b.section_id
                and a.lane_id == b.lane_id
                and a.transform.location.distance(b.transform.location) < 0.5)

    @staticmethod
    def _plan_length(plan) -> float:
        return sum(a[0].transform.location.distance(b[0].transform.location)
                   for a, b in zip(plan, plan[1:]))

    @staticmethod
    def _point_along(plan, start: int, metres: float):
        base = plan[start][0].transform.location
        index = start
        while index + 1 < len(plan):
            index += 1
            if plan[index][0].transform.location.distance(base) >= metres:
                break
        return plan[index][0]

    def _label_road_options(self, plan: list) -> list:
        """Attach a RoadOption to every plan point, from the plan's own geometry.

        ``trace_route`` does return labels, but they describe *its* A* route and
        it carries turn state between calls (``_previous_decision``); a plan
        spliced from many hops plus lane walks is not that route, so its labels
        would be wrong exactly where it matters. The turn is therefore measured
        on the plan: the heading change between the point ~10 m behind and the
        point ~10 m ahead, taking the sign of the cross product (CARLA's yaw
        grows towards +Y, so a positive cross product is a left turn). The 35
        degree threshold is the agent library's own
        (``local_planner._compute_connection``), so LANEFOLLOW/LEFT/RIGHT mean
        what BasicAgent and BehaviorAgent already interpret.

        Only points at or next to a junction get LEFT/RIGHT: a lane that merely
        curves is not a turn instruction, and labelling every arc would make
        BehaviorAgent slow down at every bend (``behavior_agent.run_step``
        case 3).
        """
        RoadOption = self._road_option_enum()

        labelled = []
        for i, (waypoint, _unused) in enumerate(plan):
            j = i
            while j > 0 and plan[j][0].transform.location.distance(
                    waypoint.transform.location) < PLAN_TURN_LOOKAHEAD_M:
                j -= 1
            before = plan[j][0]
            after = self._point_along(plan, i, PLAN_TURN_LOOKAHEAD_M)
            v1x = waypoint.transform.location.x - before.transform.location.x
            v1y = waypoint.transform.location.y - before.transform.location.y
            v2x = after.transform.location.x - waypoint.transform.location.x
            v2y = after.transform.location.y - waypoint.transform.location.y
            cross = v1x * v2y - v1y * v2x
            turn = math.degrees(math.atan2(cross, v1x * v2x + v1y * v2y))
            near_junction = (waypoint.is_junction or before.is_junction
                             or after.is_junction)
            if not near_junction or abs(turn) < TURN_THRESHOLD_DEG:
                labelled.append((waypoint, RoadOption.LANEFOLLOW))
            elif turn > 0.0:
                labelled.append((waypoint, RoadOption.LEFT))
            else:
                labelled.append((waypoint, RoadOption.RIGHT))
        return labelled

    def _plan_heading_at_start(self) -> float:
        """Bearing of the plan's first segment, in CARLA yaw degrees.

        The ego has to face *the direction the plan travels*, which is not the
        same as the first waypoint's own lane yaw when the route leaves the road
        immediately (a junction connector, or a waypoint at a road's end): the
        lane's yaw is the road's tangent, the plan's heading is where the car
        will actually go. Before this, the ego was spawned on the lane's yaw of
        a waypoint that had resolved to the opposite carriageway, so it started
        facing backwards and drove 494 m to reach a point 94 m away.
        """
        if len(self._plan) >= 2:
            a = self._plan[0][0].transform.location
            # a few metres in, so a 2 m step does not amplify the noise
            target = a
            for waypoint, _opt in self._plan[1:]:
                target = waypoint.transform.location
                if a.distance(target) >= 4.0:
                    break
            if a.distance(target) > 1e-3:
                return math.degrees(math.atan2(target.y - a.y, target.x - a.x))
        if self._plan:
            return self._plan[0][0].transform.rotation.yaw
        return 0.0

    def _spawn_ego(self) -> None:
        """Spawn the ego on the plan's first waypoint, facing the plan.

        The pose comes from the plan, not from the nearest map spawn point: the
        car has to start *on* the configured route's first waypoint and pointing
        along its first segment, otherwise the plan's opening metres are driven
        backwards or sideways. The map's spawn points are only a fallback for the
        case where the plan's own start is inside static geometry (kerbs, bridge
        decks, junction furniture) and the actor cannot be created there.
        """
        blueprint_id = self.config["ego"]["blueprint"]
        blueprints = self.world.get_blueprint_library()
        bp = blueprints.find(blueprint_id)
        if bp is None:
            raise SessionError(f"车型不存在：{blueprint_id}")

        if not self._plan:
            raise SessionError("路线计划为空：无法确定主车出生位姿")
        start_wp = self._plan[0][0]
        start_loc = start_wp.transform.location
        route_yaw = self._plan_heading_at_start()
        carla = _carla()

        def attempt(location, yaw):
            transform = carla.Transform(
                carla.Location(x=location.x, y=location.y, z=location.z + 0.3),
                carla.Rotation(pitch=0.0, yaw=yaw, roll=0.0))
            actor = self.world.try_spawn_actor(bp, transform)
            return actor, transform

        self.ego = None
        self._spawn_request_location = None
        self._spawn_source = None
        # 1st choice: exactly on the plan's first waypoint, facing its first segment
        self.ego, transform = attempt(start_loc, route_yaw)
        if self.ego is not None:
            self._spawn_request_location = transform.location
            self._spawn_source = "plan_start"
        else:
            # 2nd choice: the nearest map spawn point, still rotated to the plan
            # heading, so the car is clear of geometry but points the right way
            candidates = sorted(self.map.get_spawn_points(),
                                key=lambda sp: sp.location.distance(start_loc))
            aligned = []
            for spawn_point in candidates[:16]:
                yaw_delta = abs((spawn_point.rotation.yaw - route_yaw + 180.0) % 360.0 - 180.0)
                aligned.append((yaw_delta, spawn_point))
            aligned.sort(key=lambda item: (item[0] > 60.0, item[0],
                                           item[1].location.distance(start_loc)))
            for _delta, spawn_point in aligned[:12]:
                self.ego, transform = attempt(spawn_point.location, route_yaw)
                if self.ego is not None:
                    self._spawn_request_location = transform.location
                    self._spawn_source = "map_spawn_point"
                    break
        if self.ego is None:
            raise SessionError("主车生成失败：路线起点与地图出生点都无法生成")

        self.ego.set_autopilot(False)
        self.ego.apply_control(carla.VehicleControl())

        # Record the pose the simulator really gave us, so the run can prove the
        # spawn faced the route instead of assuming it did. The transform is not
        # readable in the same call that created the actor (it comes back as the
        # default zero pose), so it is captured on the first tick instead --
        # see _capture_spawn_pose().
        self.spawn_pose = {
            "x": None, "y": None, "z": None, "yaw": None,
            "plan_yaw": round(route_yaw, 2),
            "plan_first_waypoint": {
                "road_id": start_wp.road_id,
                "lane_id": start_wp.lane_id,
                "s": round(start_wp.s, 2),
            },
            "source": self._spawn_source,
            "requested_distance_to_plan_start_m": round(
                self._spawn_request_location.distance(start_loc), 2)
            if self._spawn_request_location is not None else None,
            "measured": False,
        }
        self._capture_spawn_pose()

    def _capture_spawn_pose(self) -> None:
        """Fill in the ego's real spawn pose once the simulator reports it.

        ``try_spawn_actor`` returns an actor whose transform is not populated
        yet; reading it immediately yields (0, 0, 0) with yaw 0, which is worse
        than no number at all because it looks like a measurement. Retried on
        every tick until the simulator answers, and left as ``measured: False``
        if it never does.
        """
        if self.ego is None or self.spawn_pose is None or self.spawn_pose.get("measured"):
            return
        try:
            actual = self.ego.get_transform()
        except Exception:  # noqa: BLE001
            return
        if (abs(actual.location.x) < 1e-6 and abs(actual.location.y) < 1e-6
                and abs(actual.rotation.yaw) < 1e-6):
            return  # the simulator has not published the spawn yet
        plan_start = self._plan[0][0].transform.location if self._plan else None
        self.spawn_pose.update({
            "x": round(actual.location.x, 2),
            "y": round(actual.location.y, 2),
            "z": round(actual.location.z, 2),
            "yaw": round(actual.rotation.yaw, 2),
            "distance_to_plan_start_m": (
                round(actual.location.distance(plan_start), 2)
                if plan_start is not None else None),
            "yaw_delta_deg": round(
                (actual.rotation.yaw - self.spawn_pose["plan_yaw"] + 180.0) % 360.0 - 180.0, 2),
            "measured": True,
        })
        detail = (f"主车出生：road {self.spawn_pose['plan_first_waypoint']['road_id']} "
                  f"lane {self.spawn_pose['plan_first_waypoint']['lane_id']} "
                  f"（{self.spawn_pose.get('source')}）"
                  f"yaw {actual.rotation.yaw:.1f}（计划首段 "
                  f"{self.spawn_pose['plan_yaw']:.1f}，偏差 "
                  f"{self.spawn_pose['yaw_delta_deg']:+.1f}°），"
                  f"距计划起点 {self.spawn_pose['distance_to_plan_start_m']} m")
        self.events.append({"t": round(self.sim_time, 2), "kind": "ego_spawn",
                            "detail": detail})
        print(f"[session {self.id}] {detail}", flush=True)

    def _spawn_traffic(self, vehicles: int, autopilot: bool) -> None:
        """Spawn the configured background traffic and record why it did or did not.

        The old version kept a 1.5 km "clear bubble" ahead of the ego; Town10HD
        is ~500 m across (its farthest spawn point is 245 m from the route
        start), so *every* spawn point was skipped and the preset silently
        produced zero cars. The bubble is now 25 m, the cars are picked farthest
        from the ego first and spread at least 25 m apart, and each failed
        attempt is attributed instead of dropped on the floor.
        """
        report = {
            "requested": int(vehicles),
            "spawn_points": 0,
            "blueprints": 0,
            "skipped_clearance": 0,
            "skipped_gap": 0,
            "attempted": 0,
            "spawned": 0,
            "failed": 0,
            "reasons": [],
            "ego_clearance_m": TRAFFIC_EGO_CLEARANCE_M,
            "min_gap_m": TRAFFIC_MIN_GAP_M,
        }
        self._traffic_report = report
        if vehicles <= 0:
            return
        library = self.world.get_blueprint_library()
        candidates = list(library.filter("vehicle.*"))
        blueprints = [bp for bp in candidates
                      if bp.has_attribute("number_of_wheels")
                      and bp.get_attribute("number_of_wheels").as_int() == 4]
        if not blueprints:  # never refuse to spawn over wheels we cannot read
            blueprints = candidates
        report["blueprints"] = len(blueprints)
        if not blueprints:
            report["reasons"].append("no vehicle blueprint in this build")
            self._record_traffic()
            return

        spawn_points = list(self.world.get_map().get_spawn_points())
        report["spawn_points"] = len(spawn_points)
        if not spawn_points:
            report["reasons"].append("map has no spawn points")
            self._record_traffic()
            return

        traffic_cfg = self.config.get("traffic", {})
        try:
            seed = int(traffic_cfg.get("seed") or 0)
        except (TypeError, ValueError):
            seed = 0
        rng = random.Random(seed) if seed else random
        ego_loc = self.ego.get_location() if self.ego is not None else None
        # farthest first: the traffic ends up spread over the map, not queued
        # behind the ego, and everything within the clearance is simply left out
        ordered = sorted(
            spawn_points,
            key=lambda sp: -(sp.location.distance(ego_loc) if ego_loc else 0.0))
        for spawn_point in ordered:
            if len(self.traffic) >= vehicles:
                break
            if ego_loc is not None and spawn_point.location.distance(ego_loc) < TRAFFIC_EGO_CLEARANCE_M:
                report["skipped_clearance"] += 1
                continue
            if any(spawn_point.location.distance(actor.get_location()) < TRAFFIC_MIN_GAP_M
                   for actor in self.traffic):
                report["skipped_gap"] += 1
                continue
            bp = rng.choice(blueprints)
            report["attempted"] += 1
            try:
                # spawn_actor (not try_spawn_actor) so the collision/blueprint
                # reason survives as the exception text instead of a bare None
                actor = self.world.spawn_actor(bp, spawn_point)
            except Exception as exc:  # noqa: BLE001
                report["failed"] += 1
                reason = f"{type(exc).__name__}: {str(exc).splitlines()[0][:140]}"
                if reason not in report["reasons"]:
                    report["reasons"].append(reason)
                continue
            actor.set_autopilot(autopilot)
            if not autopilot:
                # parked until /start hands it to the Traffic Manager
                actor.apply_control(_carla().VehicleControl())
            self.traffic.append(actor)
            report["spawned"] += 1
        self._record_traffic()

    def _record_traffic(self) -> None:
        """Put the traffic spawn report in the log, the events and the status."""
        report = self._traffic_report
        detail = (f"背景车：请求 {report.get('requested', 0)}，生成 {report.get('spawned', 0)}，"
                  f"候选点 {report.get('spawn_points', 0)}，蓝图 {report.get('blueprints', 0)}，"
                  f"间距跳过 {report.get('skipped_clearance', 0) + report.get('skipped_gap', 0)}，"
                  f"失败 {report.get('failed', 0)}")
        if report.get("reasons"):
            detail += "（" + "; ".join(report["reasons"][:3]) + "）"
        print(f"[session {self.id}] {detail}", flush=True)
        if report.get("requested", 0) > 0 and report.get("spawned", 0) == 0:
            self.events.append({"t": self.sim_time, "kind": "traffic_spawn_failed",
                                "detail": detail})
        else:
            self.events.append({"t": self.sim_time, "kind": "traffic_spawn",
                                "detail": detail})

    def _spawn_walkers(self, walkers: int) -> None:
        if walkers <= 0:
            return
        carla = _carla()
        bp = self.world.get_blueprint_library().filter("walker.pedestrian.*")
        controller_bp = self.world.get_blueprint_library().find("controller.ai.walker")
        if not bp or controller_bp is None:
            return
        spawn_points = []
        for _ in range(walkers):
            loc = self.world.get_random_location_from_navigation()
            if loc is not None:
                spawn_points.append(carla.Transform(loc))
        for transform in spawn_points:
            actor = self.world.try_spawn_actor(random.choice(bp), transform)
            if actor is None:
                continue
            controller = self.world.try_spawn_actor(controller_bp, carla.Transform(), actor)
            if controller is None:
                actor.destroy()
                continue
            controller.start()
            controller.go_to_location(self.world.get_random_location_from_navigation())
            self.walkers.append((actor, controller))

    def _compute_preview_camera(self) -> None:
        """Camera over the whole map.

        CARLA's Python API reports metres; the UE camera actor wants centimetres,
        so everything below is converted on the way out.
        """
        if self.map is None:
            self.preview_camera = None
            return
        # Frame the whole road network, not just the spawn points (those cover a
        # fraction of the map and produced a close-up of the downtown block).
        try:
            samples = self.map.generate_waypoints(8.0)
        except Exception:  # noqa: BLE001
            samples = []
        if samples:
            xs = [w.transform.location.x for w in samples]
            ys = [w.transform.location.y for w in samples]
        else:
            spawn_points = self.map.get_spawn_points()
            if not spawn_points:
                self.preview_camera = None
                return
            xs = [sp.location.x for sp in spawn_points]
            ys = [sp.location.y for sp in spawn_points]
        cx, cy = (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0
        span = math.hypot(max(xs) - min(xs), max(ys) - min(ys))  # metres
        # FOV 90 => vertical coverage is 2*height: 0.6*span frames the map
        # with a little margin while staying as close as possible
        height_m = max(60.0, span * 2.2)
        self.preview_camera = {
            "x": cx * 100.0,
            "y": cy * 100.0,
            "z": height_m * 100.0,
            "pitch": -89.5,
            "yaw": 0.0,
            "roll": 0.0,
        }

    def _cleanup_actors(self) -> None:
        for actor in self.traffic:
            try:
                actor.destroy()
            except Exception:  # noqa: BLE001
                pass
        self.traffic.clear()
        for walker, controller in self.walkers:
            try:
                controller.stop()
                controller.destroy()
            except Exception:  # noqa: BLE001
                pass
            try:
                walker.destroy()
            except Exception:  # noqa: BLE001
                pass
        self.walkers.clear()
        if self.ego is not None:
            try:
                self.ego.destroy()
            except Exception:  # noqa: BLE001
                pass
            self.ego = None
        self.agent = None

    # ------------------------------------------------------------------ #
    # lifecycle
    # ------------------------------------------------------------------ #
    def _resolve_world_name(self) -> str:
        """Map our catalog id (Town03) onto what this simulator actually ships (Town03_Opt)."""
        try:
            available = [m.split("/")[-1] for m in self.client.get_available_maps()]
        except Exception:  # noqa: BLE001
            return self.map_name
        wanted = self.map_name.lower()
        for candidate in available:
            low = candidate.lower()
            if low == wanted or low.replace("_opt", "") == wanted or low == wanted + "_opt":
                return candidate
        raise SessionError(
            f"地图 {self.map_name} 在当前模拟器中不存在"
            f"（可用：{', '.join(available[:8])}…）")

    def preview_async(self) -> dict:
        """Start loading in a worker thread and answer at once.

        ``client.load_world`` blocks the simulator for tens of seconds, so keeping
        it off the request thread lets the UI show progress instead of a dead
        button (and lets the user click another map, which supersedes this one).
        """
        if self.state in ("preview", "running"):
            return self.status()
        self.state = "preview_loading"
        self.error = None

        def worker() -> None:
            try:
                self.preview()
                if not self._is_current():
                    return
            except Exception:  # noqa: BLE001  (preview() already recorded state)
                pass

        threading.Thread(target=worker, name=f"preview-{self.id}", daemon=True).start()
        return self.status()

    def _is_current(self) -> bool:
        from . import session_runner as runner  # noqa: PLC0415

        return runner.manager.current() is self

    def preview(self) -> dict:
        """Load the world for the selected map and park everything (no loading later)."""
        if self.state not in ("created", "stopped", "finished", "error", "preview_loading"):
            return self.status()
        self.state = "preview_loading"
        self.error = None
        try:
            self.route_waypoints = self._resolve_route()
            self._connect()
            carla = _carla()
            raw_name = self._current_world_name()
            # e.g. "Carla/Maps/Town10HD_Opt" -> "town10hd"; the entry level's
            # empty map name never matches, so the town is loaded.
            current = raw_name.split("/")[-1].lower().replace("_opt", "")
            if current != self.map_name.lower():
                world_name = self._resolve_world_name()
                # loading a map takes tens of seconds; the RPC call must not time out
                self.client.set_timeout(300.0)
                try:
                    self.world = self.client.load_world(world_name)
                finally:
                    self.client.set_timeout(30.0)
                self.map = self.world.get_map()
            else:
                self.world = self.client.get_world()
                self.map = self.world.get_map()

            self._run_async()
            # reset before spawning, so the spawn/traffic reports survive
            self.tick = 0
            self.sim_time = 0.0
            self.travelled_m = 0.0
            self.metrics = []
            self.events = []
            self._apply_weather()
            # The plan comes first: the ego is spawned on its first waypoint
            # facing its first segment, and the overlay paints it.
            self._plan = self._build_plan()
            self._cache_route_points()
            self._spawn_ego()
            vehicles, _walkers = self._traffic_counts()
            self._spawn_traffic(vehicles, autopilot=False)
            self._set_settings(PREVIEW_HZ)
            self._compute_preview_camera()
            self._stalled_ticks = 0
            self._stall_reported = False
            self._arrived_reported = False
            self._destination = None
            self._route_draw_error = None
            self._stop_evt.clear()
            self._target_hz = PREVIEW_HZ
            self.state = "preview"
            self._start_loop()
        except Exception as exc:  # noqa: BLE001
            self.state = "error"
            self.error = str(exc)
            self.last_error = traceback.format_exc()
            raise SessionError(str(exc)) from exc
        return self.status()

    def plan_polyline(self) -> list[dict]:
        """The plan as plain data, so a checker can measure against it.

        Exposed instead of the agent's live queue because the queue is consumed
        as the car drives; this is the whole route the run committed to.
        """
        return [
            {
                "x": round(waypoint.transform.location.x, 2),
                "y": round(waypoint.transform.location.y, 2),
                "z": round(waypoint.transform.location.z, 2),
                "yaw": round(waypoint.transform.rotation.yaw, 2),
                "road_id": waypoint.road_id,
                "section_id": waypoint.section_id,
                "lane_id": waypoint.lane_id,
                "s": round(waypoint.s, 2),
                "lane_type": str(waypoint.lane_type),
                "is_junction": bool(waypoint.is_junction),
                "option": str(option),
            }
            for waypoint, option in self._plan
        ]

    def _cache_route_points(self) -> None:
        """Cache the polyline the overlay paints: the plan the agent follows.

        The overlay used to paint the preset's control points -- 18 vertices for
        a 498 m preset -- and the car, planning on its own, drove somewhere else
        (20 % of the drive was within 3 m of the line). It now paints the plan,
        so what is drawn and what is driven are the same points.
        """
        self._route_points = [waypoint.transform.location for waypoint, _opt in self._plan]
        self._route_color = _carla().Color(255, 180, 84)

    def set_weather(self, preset: str, custom: dict | None = None) -> dict:
        """Apply weather to the live world (preview keeps running, so it is visible)."""
        weather = self.config.setdefault("weather", {})
        weather["preset"] = preset
        if custom is not None:
            weather["custom"] = custom
        if self.world is not None:
            self._apply_weather()
        return self.status()

    def set_route(self, visible: bool | None = None,
                  preset_id: str | None = None) -> dict:
        route_cfg = self.config.setdefault("route", {})
        if preset_id:
            route_cfg["preset_id"] = preset_id
            self.route_waypoints = self._resolve_route()
            if self.map is not None:
                self._plan = self._build_plan()
                self._cache_route_points()
        if visible is not None:
            self.show_route = bool(visible)
            self.config.setdefault("run", {})["show_route"] = self.show_route
        return self.status()

    def start(self) -> dict:
        if self.state == "created":
            self.preview()
        if self.state != "preview":
            raise SessionError(f"当前状态 {self.state} 无法启动，请先进入预览")
        try:
            run = self.config.get("run", {})
            hz = 1.0 / max(0.01, float(run.get("fixed_delta_seconds", 0.05)))
            self._target_hz = max(1.0, min(hz, 100.0))
            self._run_async()
            self._activate_traffic()
            _vehicles, walkers = self._traffic_counts()
            self._spawn_walkers(walkers)

            self.agent = self._build_agent()
            self._set_settings(self._target_hz)

            if bool(run.get("record", True)):
                log_path = str(self.output_dir / "rec.log")
                try:
                    self.client.start_recorder(log_path, False)
                    self._recording = True
                except Exception as exc:  # noqa: BLE001
                    self.events.append({"t": self.sim_time, "kind": "recorder_error",
                                        "detail": str(exc)})

            self.state = "running"
            self.started_at = time.time()
            self._running_metrics = True
        except Exception as exc:  # noqa: BLE001
            self.state = "error"
            self.error = f"启动失败：{exc}"
            self.last_error = traceback.format_exc()
            raise SessionError(self.error) from exc
        return self.status()

    def _activate_traffic(self) -> None:
        """Hand the parked preview traffic to the Traffic Manager at /start.

        The TM lives in this process (it steps when *we* tick the world), and a
        synchronous world wants a synchronous TM -- a mixed pair is the classic
        source of jittering or frozen traffic (control-traffic skill).
        """
        if not self.traffic:
            return
        traffic_cfg = self.config.get("traffic", {})
        autopilot = bool(traffic_cfg.get("autopilot", True))
        for actor in self.traffic:
            try:
                actor.set_autopilot(autopilot)
            except Exception as exc:  # noqa: BLE001
                self.events.append({"t": self.sim_time, "kind": "traffic_autopilot_error",
                                    "detail": f"{actor.type_id}: {exc}"})
        if not autopilot:
            return
        detail = f"交通管理器：{len(self.traffic)} 辆车已交给 TM 自动驾驶"
        try:
            tm = self.client.get_trafficmanager()
            tm.set_synchronous_mode(True)  # the world runs synchronous
            detail += "，TM 同步模式开启"
            try:
                seed = traffic_cfg.get("seed")
                if seed is not None:
                    tm.set_random_device_seed(int(seed))
                    detail += f"，随机种子 {seed}"
            except Exception:  # noqa: BLE001  (seed is optional)
                pass
            self.events.append({"t": self.sim_time, "kind": "traffic_manager",
                                "detail": detail})
            print(f"[session {self.id}] {detail}", flush=True)
        except Exception as exc:  # noqa: BLE001
            self.events.append({"t": self.sim_time, "kind": "traffic_manager_error",
                                "detail": str(exc)})

    def _build_agent(self):
        """Instantiate the configured agent and point it at the route end."""
        algo = self.config.get("algorithm", {})
        algo_id = algo.get("id", "carla.behavior")
        params = algo.get("params", {}) or {}

        # The wheel has no `carla.agents`; the agents live in PythonAPI/carla,
        # and the path insert happens in _import_agents() before any import.
        def load(module: str, name: str):
            return self._import_agents(module, name)

        try:
            if "basic" in algo_id:
                BasicAgent = load("agents.navigation.basic_agent", "BasicAgent")
                agent = BasicAgent(
                    self.ego,
                    target_speed=float(params.get("target_speed_kmh", 50.0)),
                    opt_dict={"sampling_radius": float(params.get("sampling_resolution", 2.0))},
                )
            else:
                BehaviorAgent = load("agents.navigation.behavior_agent", "BehaviorAgent")
                agent = BehaviorAgent(self.ego, behavior=params.get("behavior", "normal"))
                for flag, setter in (
                    ("ignore_traffic_lights", "ignore_traffic_lights"),
                    ("ignore_stop_signs", "ignore_stop_signs"),
                    ("ignore_vehicles", "ignore_vehicles"),
                ):
                    if params.get(flag):
                        getattr(agent, setter)(True)
                if "target_speed_kmh" in params:
                    try:
                        agent.set_target_speed(float(params["target_speed_kmh"]))
                    except Exception:  # noqa: BLE001
                        pass
        except Exception as exc:  # noqa: BLE001
            raise SessionError(f"算法 {algo_id} 初始化失败：{exc}") from exc

        # The agent follows the *configured* route. set_destination() is never
        # called: it re-plans from the ego's current position to a single point,
        # so the preset's intermediate waypoints were never visited (the ego
        # drove 494.6 m to reach a destination 94 m away).
        if not self._plan:
            raise SessionError("路线计划为空：无法交给算法执行")
        self._destination = self._plan[-1][0].transform.location
        self._apply_plan(agent)

        # A BehaviorAgent with an empty plan just sits there braking, so prove
        # the plan is really queued and otherwise fall back to the simpler lane
        # follower -- which is given the *same* plan, not a fresh destination.
        try:
            queued = agent.get_local_planner().get_plan()
        except Exception:  # noqa: BLE001
            queued = None
        if queued is not None and len(queued) == 0:
            self.events.append({
                "t": self.sim_time,
                "kind": "planner_fallback",
                "detail": (f"{algo_id} 未能装载路线计划（0 个航点），回退 BasicAgent，"
                           f"沿用同一计划"),
            })
            print(f"[session {self.id}] planner_fallback: {algo_id} -> BasicAgent", flush=True)
            agent = self._make_basic_agent()
            self._apply_plan(agent)
        return agent

    def _apply_plan(self, agent) -> None:
        """Hand the plan to ``agent`` via set_global_plan and verify it took it."""
        plan = [(waypoint, option) for waypoint, option in self._plan]
        try:
            agent.set_global_plan(plan)
        except TypeError:
            # older signature without the keyword arguments
            agent.get_local_planner().set_global_plan(plan)
        queued = agent.get_local_planner().get_plan()
        detail = (f"路线计划已装载：{len(queued)} 个航点，"
                  f"{self._plan_length(self._plan):.1f} m，"
                  f"终点 road {self._plan[-1][0].road_id} lane {self._plan[-1][0].lane_id}")
        self.events.append({"t": self.sim_time, "kind": "agent_plan", "detail": detail})
        print(f"[session {self.id}] {detail}", flush=True)

    def _make_basic_agent(self):
        params = (self.config.get("algorithm", {}).get("params") or {})
        try:
            BasicAgent = self._import_agents("agents.navigation.basic_agent", "BasicAgent")
        except Exception as exc:  # noqa: BLE001
            raise SessionError(f"BasicAgent 不可用：{exc}") from exc
        return BasicAgent(
            self.ego,
            target_speed=float(params.get("target_speed_kmh", 50.0)),
            opt_dict={"sampling_radius": 2.0},
        )

    def stop(self) -> dict:
        if self.state in ("stopped", "finished"):
            return self.status()
        self.state = "stopping"
        self._stop_evt.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None
        if self._recording and self.client is not None:
            try:
                self.client.stop_recorder()
            except Exception:  # noqa: BLE001
                pass
            self._recording = False
        self._running_metrics = False
        try:
            if self.world is not None:
                settings = self.world.get_settings()
                settings.synchronous_mode = False
                settings.fixed_delta_seconds = 0.0
                self.world.apply_settings(settings)
        except Exception:  # noqa: BLE001
            pass
        self._cleanup_actors()
        self.stopped_at = time.time()
        self.state = "stopped"
        self._export()
        return self.status()

    def destroy(self) -> dict:
        if self.state == "running":
            self.stop()
        if self.state == "preview":
            self._stop_evt.set()
            if self._thread is not None:
                self._thread.join(timeout=5.0)
                self._thread = None
            self._cleanup_actors()
            try:
                if self.world is not None:
                    settings = self.world.get_settings()
                    settings.synchronous_mode = False
                    settings.fixed_delta_seconds = 0.0
                    self.world.apply_settings(settings)
            except Exception:  # noqa: BLE001
                pass
            self.state = "stopped"
        return self.status()

    # ------------------------------------------------------------------ #
    # tick loop
    # ------------------------------------------------------------------ #
    def _start_loop(self) -> None:
        self._thread = threading.Thread(target=self._loop, name=f"session-{self.id}", daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        budget = 1.0 / self._target_hz
        overruns = 0
        while not self._stop_evt.is_set():
            t0 = time.perf_counter()
            try:
                self.world.tick()
                self.tick += 1
                # Never `+= None`: a lost synchronous mode must degrade the run,
                # not end it (see _tick_delta).
                self.sim_time += self._tick_delta()
                self._capture_spawn_pose()
                if self.state == "running":
                    self._step_agent()
                if self._running_metrics and self.tick % max(1, int(self._target_hz / 10)) == 0:
                    self._sample_metrics()
                # paint the planned route into the world; it expires if we stop.
                # This CARLA build's DebugHelper has draw_line but no
                # draw_polyline, and the old call raised AttributeError into a
                # bare `except: pass` -- the route was never drawn at all.
                if (self.show_route and len(self._route_points) >= 2
                        and self._route_color is not None
                        and self.tick % max(1, int(self._target_hz * ROUTE_DRAW_EVERY_S)) == 0):
                    try:
                        for begin, end in zip(self._route_points, self._route_points[1:]):
                            self.world.debug.draw_line(
                                begin, end, 0.25, self._route_color,
                                ROUTE_DRAW_LIFE_S, True)
                    except Exception as exc:  # noqa: BLE001
                        if self._route_draw_error is None:
                            self._route_draw_error = str(exc)
                            self.events.append({
                                "t": round(self.sim_time, 2),
                                "kind": "route_draw_error",
                                "detail": f"路线折线绘制失败：{exc}",
                            })
                            print(f"[session {self.id}] 路线绘制失败：{exc}", flush=True)
            except Exception as exc:  # noqa: BLE001
                self.last_error = traceback.format_exc()
                self.events.append({"t": self.sim_time, "kind": "tick_error", "detail": str(exc)})
                self.state = "error"
                self.error = f"tick 失败：{exc}"
                break

            elapsed = time.perf_counter() - t0
            if self.state == "preview":
                # degrade to a near-static world instead of stalling the editor
                if elapsed > budget * 2.5:
                    overruns += 1
                    if overruns >= 5 and not self.preview_degraded:
                        self.preview_degraded = True
                        self.events.append({"t": self.sim_time, "kind": "preview_degraded",
                                            "detail": "预览 tick 超时，降级为慢速"})
                        self._target_hz = PREVIEW_FALLBACK_HZ
                else:
                    overruns = 0
                budget = 1.0 / self._target_hz
            elif self._target_hz > 0:
                budget = 1.0 / self._target_hz

            sleep_for = budget - elapsed
            if sleep_for > 0:
                self._stop_evt.wait(sleep_for)

    def _remaining_plan_points(self) -> int | None:
        """How many plan points the agent still has queued, or None if unknown."""
        if self.agent is None:
            return None
        try:
            return len(self.agent.get_local_planner().get_plan())
        except Exception:  # noqa: BLE001
            return None

    def _at_destination(self) -> bool:
        """True when the ego is parked *because the route ended*.

        The decisive signal is the agent's own plan: the local planner pops a
        waypoint as it is reached, so an (almost) empty queue means the route is
        driven. Distance to the destination alone is not enough -- a ``loop``
        preset ends where it began, ~3 m from the spawn point on the opposite
        lane, so a radius test declared "arrived" on the first tick of every
        loop run. Distance is therefore only used as a corroborating signal
        once the queue is nearly exhausted.
        """
        if self.ego is None:
            return False
        remaining = self._remaining_plan_points()
        if remaining is not None:
            if remaining == 0:
                return True
            # a couple of points left is the planner's tail, not a detour
            if remaining <= 2 and self._destination is not None:
                try:
                    return self.ego.get_location().distance(
                        self._destination) <= ARRIVAL_RADIUS_M
                except Exception:  # noqa: BLE001
                    return False
            return False
        # No plan introspection (an agent without get_local_planner): fall back
        # to the old radius test.
        if self._destination is None:
            return False
        try:
            return self.ego.get_location().distance(self._destination) <= ARRIVAL_RADIUS_M
        except Exception:  # noqa: BLE001
            return False

    def _step_agent(self) -> None:
        if self.agent is None or self.ego is None:
            return
        try:
            control = self.agent.run_step()
            control.manual_gear_shift = False
            self.ego.apply_control(control)

            # A standing car is only a stall when something asked it to move and
            # it did not: parked at the planned end it is an arrival, and with
            # throttle 0 / brake on it is being held (red light, queue, the
            # agent's own stop). Both used to be reported as "8 s 未移动".
            velocity = self.ego.get_velocity()
            speed = math.sqrt(velocity.x ** 2 + velocity.y ** 2 + velocity.z ** 2) * 3.6
            if speed >= 0.5:
                self._stalled_ticks = 0
            elif self._at_destination():
                self._stalled_ticks = 0
                if not self._arrived_reported:
                    self._arrived_reported = True
                    distance = (self.ego.get_location().distance(self._destination)
                                if self._destination is not None else float("nan"))
                    self.events.append({
                        "t": round(self.sim_time, 2),
                        "kind": "ego_arrived",
                        "detail": (f"主车走完路线计划（剩余航点 "
                                   f"{self._remaining_plan_points()}，距终点 {distance:.1f} m），"
                                   f"停车不算停滞"),
                    })
            elif control.throttle >= 0.05:
                self._stalled_ticks += 1
            else:
                # commanded to hold, not to move
                self._stalled_ticks = 0

            stall_seconds = self._stalled_ticks / max(1.0, self._target_hz)
            if (stall_seconds > 8.0 and not self._stall_reported):
                self._stall_reported = True
                remaining = (self.ego.get_location().distance(self._destination)
                             if self._destination is not None else float("nan"))
                self.events.append({
                    "t": round(self.sim_time, 2),
                    "kind": "ego_stalled",
                    "detail": f"主车 8 s 未移动（throttle={control.throttle:.2f}, "
                              f"brake={control.brake:.2f}, 距终点 {remaining:.1f} m）",
                })
        except Exception as exc:  # noqa: BLE001
            self.state = "error"
            self.error = f"算法运行失败：{exc}"
            self.last_error = traceback.format_exc()
            self._stop_evt.set()

    # ------------------------------------------------------------------ #
    # metrics
    # ------------------------------------------------------------------ #
    def _sample_metrics(self) -> None:
        if self.ego is None:
            return
        carla = _carla()
        try:
            tf = self.ego.get_transform()
            vel = self.ego.get_velocity()
            ctrl = self.ego.get_control()
            speed = math.sqrt(vel.x ** 2 + vel.y ** 2 + vel.z ** 2) * 3.6
            row = {
                "t": round(self.sim_time, 3),
                "frame": self.tick,
                "x": round(tf.location.x, 2),
                "y": round(tf.location.y, 2),
                "z": round(tf.location.z, 2),
                "yaw": round(tf.rotation.yaw, 2),
                "speed_kmh": round(speed, 2),
                "throttle": round(ctrl.throttle, 3),
                "brake": round(ctrl.brake, 3),
                "steer": round(ctrl.steer, 3),
                # A row timed at a fallback delta because the simulator dropped
                # synchronous mode says so, instead of looking like a normal one.
                "sync_lost": int(self.sync_lost),
            }
            self.metrics.append(row)
        except Exception:  # noqa: BLE001
            pass

    def _export(self) -> None:
        out = self.output_dir
        (out / "config.json").write_text(
            json.dumps(self.config, ensure_ascii=False, indent=1), encoding="utf-8")
        (out / "route.json").write_text(
            json.dumps({"waypoints": self.route_waypoints}, ensure_ascii=False, indent=1),
            encoding="utf-8")
        if self.metrics:
            keys = list(self.metrics[0].keys())
            with (out / "metrics.csv").open("w", newline="", encoding="utf-8") as fh:
                writer = csv.DictWriter(fh, fieldnames=keys)
                writer.writeheader()
                writer.writerows(self.metrics)
        (out / "events.json").write_text(
            json.dumps(self.events, ensure_ascii=False, indent=1), encoding="utf-8")
        speeds = [m["speed_kmh"] for m in self.metrics]
        summary = {
            "session_id": self.id,
            "map": self.map_name,
            "state": self.state,
            "duration_s": round(self.sim_time, 2),
            "ticks": self.tick,
            "samples": len(self.metrics),
            "speed_kmh": {
                "avg": round(sum(speeds) / len(speeds), 2) if speeds else 0.0,
                "max": round(max(speeds), 2) if speeds else 0.0,
            },
            "events": len(self.events),
            "sync_lost": self.sync_lost,
            "sync_lost_at_s": (round(self.sim_time, 2) if self.sync_lost else None),
            "wall_clock_s": round((self.stopped_at or time.time()) -
                                  (self.started_at or self.created_at), 2),
        }
        (out / "summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")

    # ------------------------------------------------------------------ #
    # status
    # ------------------------------------------------------------------ #
    def _ego_pose(self) -> dict | None:
        if self.ego is None:
            return None
        try:
            tf = self.ego.get_transform()
            vel = self.ego.get_velocity()
            ctrl = self.ego.get_control()
            return {
                "x": tf.location.x,
                "y": tf.location.y,
                "z": tf.location.z,
                "yaw": tf.rotation.yaw,
                "speed_kmh": math.sqrt(vel.x ** 2 + vel.y ** 2 + vel.z ** 2) * 3.6,
                "control": {
                    "throttle": round(ctrl.throttle, 3),
                    "brake": round(ctrl.brake, 3),
                    "steer": round(ctrl.steer, 3),
                    "hand_brake": bool(ctrl.hand_brake),
                    "gear": int(ctrl.gear),
                },
            }
        except Exception:  # noqa: BLE001
            return None

    def follow_camera(self) -> dict | None:
        ego = self._ego_pose()
        if ego is None:
            return None
        # metres -> centimetres, then a chase offset behind and above the ego
        yaw = math.radians(ego["yaw"])
        back_cm, up_cm = 1200.0, 1800.0
        return {
            "x": ego["x"] * 100.0 - math.cos(yaw) * back_cm,
            "y": ego["y"] * 100.0 - math.sin(yaw) * back_cm,
            "z": ego["z"] * 100.0 + up_cm,
            "pitch": -62.0,
            "yaw": ego["yaw"],
            "roll": 0.0,
        }

    def status(self) -> dict:
        # Only a preview/running session with a live synchronous mode has a
        # fixed delta to report; anything else (loading, stopped, lost) has none,
        # so the panel's sync chip has something true to print.
        delta = None
        if not self.sync_lost and self.state in ("preview", "running"):
            delta = self._fixed_delta
        return {
            "id": self.id,
            "state": self.state,
            "error": self.error,
            "map": self.map_name,
            "tick": self.tick,
            "sim_time": round(self.sim_time, 3),
            "traffic": len(self.traffic),
            "traffic_detail": self._traffic_report,
            "walkers": len(self.walkers),
            "route": {
                "visible": bool(self.show_route and len(self._route_points) >= 2),
                "points": len(self._route_points),
                "draw_error": self._route_draw_error,
                # What the agent is following, so the run can be checked against
                # the route that was configured rather than against a guess.
                "plan": {
                    "points": len(self._plan),
                    "length_m": self._plan_stats.get("length_m"),
                    "preset_waypoints": len(self.route_waypoints),
                    "unresolved_waypoints": self._plan_stats.get("unresolved_waypoints"),
                    "hops": self._plan_stats.get("hops"),
                    "walked": self._plan_stats.get("walked"),
                    "traced": self._plan_stats.get("traced"),
                    "failed": self._plan_stats.get("failed"),
                },
                "spawn": self.spawn_pose,
            },
            "preview_degraded": self.preview_degraded,
            # The clock the session is *really* running at, so the panel can show
            # a live sync chip and stop showing one once the simulator has
            # dropped synchronous mode.
            "sync_mode": bool(delta is not None and delta > 0.0),
            "fixed_delta": round(delta, 5) if delta else None,
            "sync_lost": self.sync_lost,
            "sync_lost_detail": self.sync_lost_detail,
            "sync_reasserts": self.sync_reasserts,
            "ego": self._ego_pose(),
            "cameras": {
                "preview": self.preview_camera,
                "follow": self.follow_camera(),
            },
            "events": len(self.events),
            "output_dir": str(self.output_dir),
        }


class SessionManager:
    def __init__(self):
        self._sessions: dict[str, Session] = {}
        self._lock = threading.Lock()
        self.current_id: str | None = None

    def create(self, config: dict) -> Session:
        if config.get("map") in paths.EXCLUDED_MAPS:
            raise SessionError(f"{config['map']} 不在可选地图列表里")
        session_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
        session = Session(session_id, config)
        with self._lock:
            self._sessions[session_id] = session
            self.current_id = session_id
        return session

    def get(self, session_id: str | None = None) -> Session:
        with self._lock:
            key = session_id or self.current_id
            if not key or key not in self._sessions:
                raise SessionError("会话不存在")
            return self._sessions[key]

    def current(self) -> Session | None:
        with self._lock:
            return self._sessions.get(self.current_id) if self.current_id else None

    def all(self) -> list[dict]:
        with self._lock:
            return [s.status() for s in self._sessions.values()]


manager = SessionManager()








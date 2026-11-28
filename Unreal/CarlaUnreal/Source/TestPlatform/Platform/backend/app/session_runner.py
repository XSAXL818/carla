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
        self.show_route = bool(config.get("run", {}).get("show_route", True))
        self._route_points: list = []
        self._route_color = None
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

    def _waypoint_at(self, road_id: int, lane_id: int, s: float):
        """Resolve a (road, lane, s) tuple onto a *drivable* lane.

        Our presets emit ``lane_id = -1`` meaning "any driving lane", and CARLA's
        ``get_waypoint_xodr`` rejects that. Probing in order can also land on a
        shoulder or parking lane, where the ego gets stuck against the kerb, so
        prefer ``LaneType.Driving`` and only fall back to whatever exists.
        """
        carla = _carla()
        lanes = [lane_id] if lane_id not in (None, 0, -1) else [1, 2, 3, 4, 5, 6,
                                                               -1, -2, -3, -4, -5, -6]
        fallback = None
        for lane in lanes:
            try:
                waypoint = self.map.get_waypoint_xodr(road_id, lane, s)
            except Exception:  # noqa: BLE001
                waypoint = None
            if waypoint is None:
                continue
            if waypoint.lane_type == carla.LaneType.Driving:
                return waypoint
            if fallback is None:
                fallback = waypoint
        return fallback

    def _spawn_ego(self) -> None:
        """Spawn the ego on a clear spot near the route start.

        A bare waypoint transform can sit inside static geometry (kerbs, bridge
        decks, junction furniture): the car spawns "fine" and then cannot move,
        even at full throttle. The map's own spawn points are guaranteed clear,
        so prefer the nearest one to the route start.
        """
        blueprint_id = self.config["ego"]["blueprint"]
        blueprints = self.world.get_blueprint_library()
        bp = blueprints.find(blueprint_id)
        if bp is None:
            raise SessionError(f"车型不存在：{blueprint_id}")

        first = self.route_waypoints[0]
        start_wp = self._waypoint_at(first["road_id"], first["lane_id"], first["s"])
        if start_wp is None:
            raise SessionError(
                f"路线起点无法解析：road {first['road_id']} lane {first['lane_id']} s {first['s']}")
        start_loc = start_wp.transform.location

        candidates = sorted(
            self.map.get_spawn_points(),
            key=lambda sp: sp.location.distance(start_loc))
        carla = _carla()

        # Face the ego along the route: a spawn point has its own (often
        # opposite) heading, and a BehaviorAgent asked to drive "the other way"
        # just stands on the brake. Prefer points already aligned, then rewrite
        # the rotation to the route heading.
        route_yaw = start_wp.transform.rotation.yaw
        aligned = []
        for spawn_point in candidates[:16]:
            yaw_delta = abs((spawn_point.rotation.yaw - route_yaw + 180.0) % 360.0 - 180.0)
            aligned.append((yaw_delta, spawn_point))
        aligned.sort(key=lambda item: (item[0] > 60.0, item[0], item[1].location.distance(start_loc)))

        self.ego = None
        for _delta, spawn_point in aligned[:12]:
            transform = carla.Transform(spawn_point.location,
                                        carla.Rotation(pitch=0.0, yaw=route_yaw, roll=0.0))
            self.ego = self.world.try_spawn_actor(bp, transform)
            if self.ego is not None:
                break
        if self.ego is None:
            # last resort: the waypoint transform itself, nudged upwards
            transform = start_wp.transform
            transform.location.z += 0.3
            self.ego = self.world.try_spawn_actor(bp, transform)
        if self.ego is None:
            raise SessionError("主车生成失败：路线起点附近没有可用位置")

        self.ego.set_autopilot(False)
        self.ego.apply_control(carla.VehicleControl())

    def _spawn_traffic(self, vehicles: int, autopilot: bool) -> None:
        if vehicles <= 0:
            return
        blueprints = [bp for bp in self.world.get_blueprint_library().filter("vehicle")
                      if bp.has_attribute("number_of_wheels")
                      and bp.get_attribute("number_of_wheels").as_int() == 4]
        if not blueprints:
            return
        spawn_points = self.world.get_map().get_spawn_points()
        ego_loc = self.ego.get_location() if self.ego else None
        random.shuffle(spawn_points)
        ego_tf = self.ego.get_transform() if self.ego else None
        ego_fwd = ego_tf.get_forward_vector() if ego_tf else None
        for sp in spawn_points:
            if len(self.traffic) >= vehicles:
                break
            if ego_tf is not None:
                # keep a clear bubble ahead of the ego
                d = sp.location - ego_tf.location
                if d.length() < 1500.0:
                    continue
                if ego_fwd is not None and d.dot(ego_fwd) > 0 and d.length() < 4000.0:
                    continue
            bp = random.choice(blueprints)
            actor = self.world.try_spawn_actor(bp, sp)
            if actor is None:
                continue
            actor.set_autopilot(autopilot)
            self.traffic.append(actor)

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
            self._apply_weather()
            self._spawn_ego()
            vehicles, _walkers = self._traffic_counts()
            self._spawn_traffic(vehicles, autopilot=False)
            self._cache_route_points()
            self._set_settings(PREVIEW_HZ)
            self._compute_preview_camera()
            self.tick = 0
            self.sim_time = 0.0
            self.travelled_m = 0.0
            self.metrics = []
            self.events = []
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

    def _cache_route_points(self) -> None:
        """Resolve the route once so it can be painted into the world each tick."""
        points = []
        for spec in self.route_waypoints:
            waypoint = self._waypoint_at(spec["road_id"], spec["lane_id"], spec["s"])
            if waypoint is not None:
                points.append(waypoint.transform.location)
        self._route_points = points
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
            for actor in self.traffic:
                actor.set_autopilot(bool(self.config.get("traffic", {}).get("autopilot", True)))
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

    def _build_agent(self):
        """Instantiate the configured agent and point it at the route end."""
        algo = self.config.get("algorithm", {})
        algo_id = algo.get("id", "carla.behavior")
        params = algo.get("params", {}) or {}

        # The wheel has no `carla.agents`; the agents live in PythonAPI/carla.
        import sys  # noqa: PLC0415

        if str(paths.PYTHON_API_CARLA) not in sys.path and paths.PYTHON_API_CARLA.is_dir():
            sys.path.insert(0, str(paths.PYTHON_API_CARLA))

        def load(module: str, name: str):
            for candidate in (module, "carla." + module):
                try:
                    mod = __import__(candidate, fromlist=[name])
                    return getattr(mod, name)
                except Exception:  # noqa: BLE001
                    continue
            raise ImportError(
                f"无法导入 {name}（已尝试 {module} 与 carla.{module}，"
                f"PYTHONPATH 需包含 {paths.PYTHON_API_CARLA}）")

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

        last = self.route_waypoints[-1]
        destination_wp = self._waypoint_at(last["road_id"], last["lane_id"], last["s"])
        if destination_wp is None:
            raise SessionError("路线终点无法解析")
        destination_wp = self._driving_waypoint_near(destination_wp)
        try:
            agent.set_destination(destination_wp.transform.location)
        except TypeError:
            agent.set_destination(destination_wp.transform.location,
                                  self.ego.get_transform().location)

        # A BehaviorAgent with an empty plan just sits there braking, so prove
        # the plan exists and otherwise fall back to the simpler lane follower.
        try:
            plan = agent.get_local_planner().get_plan()
        except Exception:  # noqa: BLE001
            plan = None
        if plan is not None and len(plan) == 0:
            self.events.append({
                "t": self.sim_time,
                "kind": "planner_fallback",
                "detail": "BehaviorAgent 未能规划到终点，回退 BasicAgent",
            })
            agent = self._make_basic_agent()
            agent.set_destination(destination_wp.transform.location)
        return agent

    def _make_basic_agent(self):
        import sys  # noqa: PLC0415

        if str(paths.PYTHON_API_CARLA) not in sys.path and paths.PYTHON_API_CARLA.is_dir():
            sys.path.insert(0, str(paths.PYTHON_API_CARLA))
        params = (self.config.get("algorithm", {}).get("params") or {})
        try:
            from agents.navigation.basic_agent import BasicAgent  # noqa: PLC0415
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
                if self.state == "running":
                    self._step_agent()
                if self._running_metrics and self.tick % max(1, int(self._target_hz / 10)) == 0:
                    self._sample_metrics()
                # paint the planned route into the world; it expires if we stop
                if (self.show_route and self._route_points
                        and self._route_color is not None
                        and self.tick % max(1, int(self._target_hz / 2)) == 0):
                    try:
                        self.world.debug.draw_polyline(
                            self._route_points, self._route_color, 0.3, 0.9)
                    except Exception:  # noqa: BLE001
                        pass
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

    def _step_agent(self) -> None:
        if self.agent is None or self.ego is None:
            return
        try:
            control = self.agent.run_step()
            control.manual_gear_shift = False
            self.ego.apply_control(control)

            # "throttle but no movement" is the failure mode that looks like a
            # broken simulator, so surface it as an event instead of silently
            # idling forever.
            velocity = self.ego.get_velocity()
            speed = math.sqrt(velocity.x ** 2 + velocity.y ** 2 + velocity.z ** 2) * 3.6
            if speed < 0.5:
                self._stalled_ticks += 1
            else:
                self._stalled_ticks = 0
            stall_seconds = self._stalled_ticks / max(1.0, self._target_hz)
            if (stall_seconds > 8.0 and not self._stall_reported):
                self._stall_reported = True
                self.events.append({
                    "t": round(self.sim_time, 2),
                    "kind": "ego_stalled",
                    "detail": f"主车 8 s 未移动（throttle={control.throttle:.2f}, "
                              f"brake={control.brake:.2f}）",
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
            "walkers": len(self.walkers),
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








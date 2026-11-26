"""Static catalogues the configuration UI renders: maps, routes, vehicles,
weather, traffic, algorithms and the session config schema.

Nothing here starts a simulation; when the CARLA server is up the vehicle list
is narrowed to the blueprints this build really ships.
"""
from __future__ import annotations

import json
from functools import lru_cache

from . import carla_bridge, paths

# --------------------------------------------------------------------------- #
# maps + routes (generated offline by tools/gen_route_presets.py)
# --------------------------------------------------------------------------- #
# Friendly names/classes for the blueprint ids this CARLA build really ships.
_EXTRA_VEHICLES = {
    "vehicle.ue4.audi.tt": ("Audi TT", "跑车"),
    "vehicle.ue4.mercedes.ccc": ("Mercedes Coupé", "轿跑"),
    "vehicle.ue4.ford.mustang": ("Ford Mustang", "跑车"),
    "vehicle.ue4.chevrolet.impala": ("Chevrolet Impala", "轿车"),
    "vehicle.ue4.ford.crown": ("Ford Crown Victoria", "轿车"),
    "vehicle.nissan.patrol": ("Nissan Patrol", "SUV"),
    "vehicle.dodge.charger": ("Dodge Charger", "轿车"),
    "vehicle.dodgecop.charger": ("Dodge Charger 警车", "警车"),
    "vehicle.mini.cooper": ("MINI Cooper", "小型"),
    "vehicle.kia.niro": ("Kia Niro", "SUV"),
    "vehicle.fuso.mitsubishi": ("Fuso Mitsubishi", "卡车"),
    "vehicle.sprinter.mercedes": ("Mercedes Sprinter", "货车"),
    "vehicle.taxi.ford": ("Ford Taxi", "出租车"),
    "vehicle.ambulance.ford": ("Ford Ambulance", "救护车"),
    "vehicle.carlacola.actors": ("Carla Cola 卡车", "货车"),
    "vehicle.firetruck.actors": ("消防车", "卡车"),
}


@lru_cache(maxsize=1)
def _routes_index() -> dict:
    index_path = paths.ROUTES_DIR / "index.json"
    if not index_path.is_file():
        return {"maps": {}}
    return json.loads(index_path.read_text(encoding="utf-8"))


def routes_for(map_name: str) -> dict | None:
    path = paths.ROUTES_DIR / f"{map_name}.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def maps() -> list[dict]:
    index = _routes_index().get("maps", {})
    out = []
    # Only the maps the selection UI offers, in the requested order.
    for name in paths.ALLOWED_MAPS:
        info = index.get(name)
        if info is None:
            continue
        if name in paths.EXCLUDED_MAPS:
            continue
        full = routes_for(name) or {}
        presets = [
            {
                "id": p["id"],
                "name": p["name"],
                "kind": p.get("kind", p["id"]),
                "length_km": round(p["length_m"] / 1000.0, 2),
                "waypoints": len(p["waypoints"]),
            }
            for p in full.get("presets", [])
        ]
        if not presets:
            continue
        out.append(
            {
                "id": name,
                "name": name,
                "roads": info.get("roads"),
                "junctions": info.get("junctions"),
                "presets": presets,
                "preview": f"/maps/{name}/preview.svg",
            }
        )
    return out


# --------------------------------------------------------------------------- #
# vehicles
# --------------------------------------------------------------------------- #
@lru_cache(maxsize=1)
def _seed_vehicles() -> list[dict]:
    path = paths.DATA_DIR / "vehicles.json"
    return json.loads(path.read_text(encoding="utf-8"))["vehicles"]


def _apply_extra_names(entry: dict) -> dict:
    known = _EXTRA_VEHICLES.get(entry.get("id", ""))
    if known:
        entry["name"], entry["cls"] = known
    return entry


def vehicles() -> dict:
    seed = [dict(v) for v in _seed_vehicles()]
    live = carla_bridge.blueprint_ids("vehicle")
    if live is not None:
        known = {v["id"] for v in seed}
        seed = [v for v in seed if v["id"] in live]
        seed += [
            {"id": bid, "name": bid.split(".", 1)[-1], "cls": "其他", "wheels": 4}
            for bid in live
            if bid not in known and not bid.endswith("_bp")
        ]
    for v in seed:
        _apply_extra_names(v)
        if v.get("id", "").endswith(("_bp",)):
            continue
        v["preview"] = f"/vehicles/{v['id']}/thumb.png"
        v["verified"] = live is not None
    # four-wheelers first: the cards show cars, not bikes
    bikes = ("kawasaki", "harley", "crossbike", "diamondback", "gazelle", "vespa", "yamaha")
    cars = [v for v in seed if not any(b in v["id"] for b in bikes)]
    others = [v for v in seed if any(b in v["id"] for b in bikes)]
    return {"vehicles": cars + others, "source": "live" if live is not None else "seed"}


# --------------------------------------------------------------------------- #
# weather
# --------------------------------------------------------------------------- #
WEATHER_PRESETS = [
    {
        "id": "ClearNoon", "name": "晴 · 正午", "desc": "基线光照，阴影清晰",
        "css": "none",
    },
    {
        "id": "CloudyNoon", "name": "多云 · 正午", "desc": "无直射阴影，感知稳定",
        "css": "brightness(.92) saturate(.85)",
    },
    {
        "id": "WetNoon", "name": "湿路面 · 正午", "desc": "低附着，制动距离变长",
        "css": "brightness(.9) saturate(.8) contrast(1.05)",
    },
    {
        "id": "HardRainNoon", "name": "暴雨 · 正午", "desc": "能见度与反光干扰",
        "css": "brightness(.78) saturate(.7) contrast(1.1)",
    },
    {
        "id": "ClearSunset", "name": "晴 · 黄昏", "desc": "低太阳角，长阴影",
        "css": "sepia(.35) brightness(.85) saturate(1.15)",
    },
    {
        "id": "MidRainyNight", "name": "夜 · 中雨", "desc": "光照极限场景",
        "css": "invert(.06) brightness(.45) saturate(.6) hue-rotate(180deg)",
    },
]

WEATHER_CUSTOM_KEYS = [
    {"key": "sun_altitude_angle", "label": "太阳高度角", "type": "number",
     "min": -90, "max": 90, "step": 1, "unit": "°", "default": 45},
    {"key": "sun_azimuth_angle", "label": "太阳方位角", "type": "number",
     "min": -180, "max": 180, "step": 1, "unit": "°", "default": 0},
    {"key": "cloudiness", "label": "云量", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 10},
    {"key": "precipitation", "label": "降水", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 0},
    {"key": "precipitation_deposits", "label": "路面积水", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 0},
    {"key": "wetness", "label": "湿滑", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 0},
    {"key": "fog_density", "label": "雾浓度", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 0},
    {"key": "wind_intensity", "label": "风速", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 10},
    {"key": "dust_storm", "label": "沙尘", "type": "number",
     "min": 0, "max": 100, "step": 1, "unit": "%", "default": 0},
]


def weather() -> dict:
    return {
        "presets": WEATHER_PRESETS,
        "custom": {"id": "custom", "name": "自定义", "params": WEATHER_CUSTOM_KEYS},
    }


# --------------------------------------------------------------------------- #
# traffic
# --------------------------------------------------------------------------- #
TRAFFIC_PRESETS = [
    {"id": "none", "name": "无", "vehicles": 0, "walkers": 0, "desc": "纯主车，最快"},
    {"id": "light", "name": "轻度", "vehicles": 4, "walkers": 6,
     "desc": "够触发跟车与让行（默认）"},
    {"id": "standard", "name": "标准", "vehicles": 10, "walkers": 6, "desc": "一般交通"},
    {"id": "dense", "name": "拥堵", "vehicles": 20, "walkers": 12, "desc": "密集车流"},
]


def traffic() -> dict:
    return {"presets": TRAFFIC_PRESETS}


# --------------------------------------------------------------------------- #
# algorithms (self-describing parameters)
# --------------------------------------------------------------------------- #
ALGORITHMS = [
    {
        "id": "carla.behavior",
        "name": "BehaviorAgent（CARLA 内置）",
        "builtin": True,
        "entry": "carla.agents.navigation.behavior_agent.BehaviorAgent",
        "desc": "CARLA 自带的规则型驾驶代理，按道路限速、红绿灯与障碍物决策。",
        "params": [
            {"key": "behavior", "label": "驾驶风格", "type": "enum", "group": "驾驶行为",
             "options": [
                 {"value": "cautious", "label": "稳健"},
                 {"value": "normal", "label": "标准"},
                 {"value": "aggressive", "label": "激进"},
             ],
             "default": "normal"},
            {"key": "target_speed_kmh", "label": "目标速度", "type": "number", "group": "驾驶行为",
             "min": 10, "max": 130, "step": 5, "unit": "km/h", "default": 50},
            {"key": "ignore_traffic_lights", "label": "忽略红绿灯", "type": "bool",
             "group": "规则", "default": False},
            {"key": "ignore_stop_signs", "label": "忽略停车标志", "type": "bool",
             "group": "规则", "default": False},
            {"key": "ignore_vehicles", "label": "忽略其他车辆", "type": "bool",
             "group": "规则", "default": False},
            {"key": "sampling_resolution", "label": "规划采样间隔", "type": "number",
             "group": "规划", "min": 1.0, "max": 10.0, "step": 0.5, "unit": "m", "default": 2.0},
        ],
    },
    {
        "id": "carla.basic",
        "name": "BasicAgent（CARLA 内置）",
        "builtin": True,
        "entry": "carla.agents.navigation.basic_agent.BasicAgent",
        "desc": "只沿全局路径行驶、按目标速度限速，不看信号灯的最简代理（适合做基线）。",
        "params": [
            {"key": "target_speed_kmh", "label": "目标速度", "type": "number", "group": "驾驶行为",
             "min": 10, "max": 130, "step": 5, "unit": "km/h", "default": 50},
            {"key": "sampling_resolution", "label": "规划采样间隔", "type": "number",
             "group": "规划", "min": 1.0, "max": 10.0, "step": 0.5, "unit": "m", "default": 2.0},
        ],
    },
]


def algorithms() -> dict:
    return {"schema_version": 1, "algorithms": ALGORITHMS}


# --------------------------------------------------------------------------- #
# session config schema (§4 of docs/architecture.md)
# --------------------------------------------------------------------------- #
def session_schema() -> dict:
    waypoint = {
        "type": "object",
        "required": ["road_id", "lane_id", "s"],
        "properties": {
            "road_id": {"type": "integer", "minimum": 0},
            "lane_id": {"type": "integer"},
            "s": {"type": "number", "minimum": 0},
        },
        "additionalProperties": False,
    }
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "CARLA 测试平台 · 单次运行配置",
        "version": 1,
        "type": "object",
        "required": ["schema_version", "map", "ego", "route", "weather", "traffic",
                     "algorithm", "run"],
        "properties": {
            "schema_version": {"type": "integer", "const": 1},
            "map": {"type": "string", "description": "地图 id，例如 Town10HD"},
            "ego": {
                "type": "object",
                "required": ["blueprint"],
                "properties": {
                    "blueprint": {"type": "string"},
                    "spawn": {
                        "type": "object",
                        "properties": {
                            "mode": {"type": "string",
                                     "enum": ["route_start", "explicit", "auto"]},
                            "index": {"type": "integer", "minimum": 0},
                        },
                        "additionalProperties": False,
                    },
                },
                "additionalProperties": False,
            },
            "route": {
                "type": "object",
                "required": ["source"],
                "properties": {
                    "source": {"type": "string", "enum": ["preset", "custom"]},
                    "preset_id": {"type": "string"},
                    "waypoints": {"type": "array", "items": waypoint, "minItems": 2},
                },
                "additionalProperties": False,
            },
            "weather": {
                "type": "object",
                "required": ["preset"],
                "properties": {
                    "preset": {"type": "string"},
                    "custom": {"type": "object"},
                },
                "additionalProperties": False,
            },
            "traffic": {
                "type": "object",
                "properties": {
                    "preset": {"type": "string",
                               "enum": [p["id"] for p in TRAFFIC_PRESETS]},
                    "vehicles": {"type": "integer", "minimum": 0, "maximum": 60},
                    "walkers": {"type": "integer", "minimum": 0, "maximum": 60},
                    "autopilot": {"type": "boolean"},
                    "seed": {"type": "integer"},
                },
                "additionalProperties": False,
            },
            "algorithm": {
                "type": "object",
                "required": ["id"],
                "properties": {
                    "id": {"type": "string"},
                    "params": {"type": "object"},
                },
                "additionalProperties": False,
            },
            "run": {
                "type": "object",
                "properties": {
                    "sync_mode": {"type": "boolean", "default": True},
                    "fixed_delta_seconds": {"type": "number", "default": 0.05,
                                            "minimum": 0.01, "maximum": 0.5},
                    "record": {"type": "boolean", "default": True},
                    "export": {
                        "type": "object",
                        "properties": {
                            "metrics_csv": {"type": "boolean", "default": True},
                            "events_json": {"type": "boolean", "default": True},
                            "config_json": {"type": "boolean", "default": True},
                        },
                        "additionalProperties": False,
                    },
                    "max_duration_s": {"type": "number", "default": 120},
                },
                "additionalProperties": False,
            },
        },
        "additionalProperties": False,
    }


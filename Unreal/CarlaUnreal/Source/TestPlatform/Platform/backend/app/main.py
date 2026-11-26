"""FastAPI entry point: read-only catalog + vector previews.

Phase 1 endpoints (no simulation):
    GET /health
    GET /maps                       maps with their 4 route presets
    GET /maps/{map}/routes          full preset list incl. (road_id, lane_id, s)
    GET /maps/{map}/preview.svg     real road network (+ optional route overlay)
    GET /vehicles
    GET /weather
    GET /traffic
    GET /algorithms
    GET /config/schema

The session lifecycle (POST /sessions, /start, /stop, metrics, websocket) lands
next; this module already owns the paths and the app object it will register on.
"""
from __future__ import annotations

from fastapi import FastAPI, HTTPException, Query, Response
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from . import carla_bridge, catalog, paths, preview, thumbs
from . import sessions_api

app = FastAPI(
    title="CARLA 测试平台后端",
    version="0.1.0",
    description="配置界面与仿真会话的后端；配置类接口在模拟器未启动时也可用。",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # permissive for local dev tools (Swagger UI, direct browser hits)
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.on_event("startup")
def unfreeze_world() -> None:
    """A crashed backend can leave the world in synchronous mode with nobody
    ticking it, which makes every later client time out. Reset that on boot -
    in a worker thread, because a dead simulator must never block startup."""
    import threading  # noqa: PLC0415

    threading.Thread(target=carla_bridge.reset_settings, daemon=True).start()

_spin_dir = paths.DATA_DIR / "vehicle_spin"
if _spin_dir.is_dir():
    app.mount("/vehicle_spin", StaticFiles(directory=str(_spin_dir)), name="vehicle_spin")

app.include_router(sessions_api.router)


@app.get("/")
def root():
    return {
        "name": "CARLA test platform backend",
        "version": "0.1.0",
        "docs": "/docs",
        "carla": carla_bridge.status(),
    }


@app.get("/health")
def health(force: bool = Query(False, description="re-probe CARLA instead of using the cache")):
    return {
        "backend": {"ok": True, "host": paths.API_HOST, "port": paths.API_PORT},
        "carla": carla_bridge.status(force=force),
        "maps_dir": str(paths.MAPS_DIR),
        "routes_dir": str(paths.ROUTES_DIR),
    }


@app.get("/maps")
def get_maps():
    items = catalog.maps()
    return {"count": len(items), "maps": items}


@app.get("/maps/{map_name}/routes")
def get_routes(map_name: str):
    if map_name in paths.EXCLUDED_MAPS:
        raise HTTPException(status_code=404, detail=f"{map_name} 不在可选地图列表中")
    data = catalog.routes_for(map_name)
    if data is None:
        raise HTTPException(status_code=404,
                            detail=f"没有 {map_name} 的路线预设，请先运行 tools/gen_route_presets.py")
    return data


@app.get("/maps/{map_name}/thumb.png")
def map_thumb(
    map_name: str,
    w: int = Query(208, ge=64, le=512),
    h: int = Query(96, ge=48, le=320),
    preset: str | None = Query(None),
):
    """Rasterised map card art for the native UI (Slate cannot draw SVG)."""
    if map_name in paths.EXCLUDED_MAPS:
        raise HTTPException(status_code=404, detail=f"{map_name} 不在可选地图里")
    try:
        data = thumbs.thumb_png(map_name, w, h, preset)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return Response(content=data, media_type="image/png",
                    headers={"Cache-Control": "public, max-age=86400"})


@app.get("/vehicles/{blueprint}/thumb.png")
def vehicle_thumb(blueprint: str, cls: str = Query("")):
    try:
        data = thumbs.vehicle_png(blueprint, cls)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return Response(content=data, media_type="image/png",
                    headers={"Cache-Control": "public, max-age=86400"})

@app.get("/maps/{map_name}/preview.svg")
def get_map_preview(
    map_name: str,
    preset: str | None = Query(None, description="叠加该预设路线，例如 loop"),
    focus_x: float | None = Query(None, description="裁剪中心世界坐标 X"),
    focus_y: float | None = Query(None, description="裁剪中心世界坐标 Y"),
    half: float | None = Query(None, description="裁剪半径（米）"),
    width: int = Query(900, ge=200, le=2400),
    height: int = Query(520, ge=120, le=1600),
    routes_only: bool = Query(False, description="透明背景，只画路线与起终点（叠加到 3D 画面上）"),
):
    routes = catalog.routes_for(map_name) if preset else None
    if preset and routes is None:
        raise HTTPException(status_code=404, detail=f"没有 {map_name} 的路线预设")
    chosen = None
    if preset:
        chosen = next((p for p in routes.get("presets", []) if p["id"] == preset), None)
        if chosen is None:
            raise HTTPException(status_code=404,
                                detail=f"{map_name} 没有名为 {preset} 的预设")
    focus = (focus_x, focus_y) if focus_x is not None and focus_y is not None else None
    try:
        svg = preview.preview_svg(map_name, chosen, focus, half, width, height,
                                  routes_only=routes_only)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return Response(
        content=svg,
        media_type="image/svg+xml",
        headers={"Cache-Control": "public, max-age=86400"},
    )


@app.get("/vehicles")
def get_vehicles():
    return catalog.vehicles()


@app.get("/vehicles/{blueprint}/preview.png")
def get_vehicle_preview(blueprint: str):
    # thumbnails need a live world to render; reported as "not yet generated"
    raise HTTPException(status_code=501,
                        detail="车辆缩略图需要已连接的 CARLA 世界，尚未实现")


@app.get("/weather")
def get_weather():
    return catalog.weather()


@app.get("/traffic")
def get_traffic():
    return catalog.traffic()


@app.get("/algorithms")
def get_algorithms():
    return catalog.algorithms()


@app.get("/config/schema")
def get_config_schema():
    return JSONResponse(catalog.session_schema())




"""Session lifecycle endpoints (preview -> running -> stopped + export)."""
from __future__ import annotations

import json

from fastapi import APIRouter, Body, HTTPException
from fastapi.responses import FileResponse, JSONResponse

from . import catalog, session_runner, paths

router = APIRouter(prefix="/sessions", tags=["sessions"])


def _payload(body: dict) -> dict:
    """Fill defaults so the UI can post a partial config."""
    cfg = dict(body or {})
    cfg.setdefault("schema_version", 1)
    cfg.setdefault("ego", {"blueprint": "vehicle.tesla.model3",
                           "spawn": {"mode": "route_start"}})
    cfg.setdefault("route", {"source": "preset", "preset_id": "loop"})
    cfg.setdefault("weather", {"preset": "ClearNoon"})
    cfg.setdefault("traffic", {"preset": "light", "autopilot": True})
    cfg.setdefault("algorithm", {"id": "carla.behavior", "params": {}})
    cfg.setdefault("run", {"sync_mode": True, "fixed_delta_seconds": 0.05,
                           "record": True, "max_duration_s": 120})
    if "map" not in cfg:
        raise HTTPException(status_code=422, detail="缺少 map 字段")
    return cfg


@router.post("")
def create_session(config: dict = Body(...)):
    try:
        cfg = _payload(config)
        session = session_runner.manager.create(cfg)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return session.status()


@router.get("")
def list_sessions():
    return {"sessions": session_runner.manager.all(),
            "current": session_runner.manager.current_id}


@router.get("/current")
def current_session():
    session = session_runner.manager.current()
    if session is None:
        return JSONResponse({"state": "none", "cameras": {"preview": None, "follow": None},
                             "ego": None, "id": None})
    return session.status()


@router.get("/{session_id}")
def get_session(session_id: str):
    try:
        return session_runner.manager.get(session_id).status()
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.post("/preview_map")
def preview_map(config: dict = Body(...)):
    """One call for "user picked a map": tear the old session down, load the map.

    The load itself runs in the backend's worker thread and this answers
    immediately with ``state=preview_loading`` so the UI stays alive; the panel
    polls ``/sessions/current`` and re-points the camera once the world is up.
    """
    try:
        cfg = _payload(config)
        current = session_runner.manager.current()
        if current is not None and current.state in (
                "preview", "preview_loading", "running", "error", "created"):
            try:
                current.destroy()
            except Exception:  # noqa: BLE001  (never block the new map on cleanup)
                pass
        session = session_runner.manager.create(cfg)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return session.preview_async()


@router.post("/{session_id}/weather")
def set_weather(session_id: str, body: dict = Body(default={})):
    """Apply weather to the live world: the preview repaints immediately."""
    try:
        session = session_runner.manager.get(session_id)
        preset = body.get("preset") or "ClearNoon"
        return session.set_weather(preset, body.get("custom"))
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{session_id}/route")
def set_route(session_id: str, body: dict = Body(default={})):
    """Show/hide the planned route in the world, optionally switching preset."""
    try:
        session = session_runner.manager.get(session_id)
        visible = body.get("visible")
        preset_id = body.get("preset_id")
        return session.set_route(None if visible is None else bool(visible), preset_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{session_id}/preview")
def preview_session(session_id: str):
    """Load the map and park the car: the 'select a map' step."""
    try:
        return session_runner.manager.get(session_id).preview()
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{session_id}/start")
def start_session(session_id: str):
    try:
        return session_runner.manager.get(session_id).start()
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{session_id}/stop")
def stop_session(session_id: str):
    try:
        return session_runner.manager.get(session_id).stop()
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{session_id}/destroy")
def destroy_session(session_id: str):
    try:
        return session_runner.manager.get(session_id).destroy()
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("/{session_id}/summary")
def session_summary(session_id: str):
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    summary = session.output_dir / "summary.json"
    if not summary.is_file():
        raise HTTPException(status_code=404, detail="还没有导出结果")
    return JSONResponse(json.loads(summary.read_text(encoding="utf-8")))


@router.get("/{session_id}/metrics.csv")
def session_metrics(session_id: str):
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    path = session.output_dir / "metrics.csv"
    if not path.is_file():
        raise HTTPException(status_code=404, detail="还没有指标数据")
    return FileResponse(path, media_type="text/csv", filename=f"{session_id}-metrics.csv")


@router.get("/{session_id}/events.json")
def session_events(session_id: str):
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return {"events": session.events, "count": len(session.events)}


@router.get("/{session_id}/plan.json")
def session_plan(session_id: str):
    """The route plan the agent is following, as plain waypoint data.

    Same surface as the events: what the run committed to, readable without
    touching the simulator, so a check can measure the drive against the plan
    instead of against the preset's control points.
    """
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return {"session_id": session.id, "stats": session._plan_stats,
            "points": session.plan_polyline()}


@router.get("/{session_id}/rec.log")
def session_recorder(session_id: str):
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    path = session.output_dir / "rec.log"
    if not path.is_file():
        raise HTTPException(status_code=404, detail="本次运行没有录制文件")
    return FileResponse(path, media_type="application/octet-stream",
                        filename=f"{session_id}-rec.log")


@router.get("/{session_id}/presets")
def session_presets(session_id: str):
    """Route presets for the session's map, so the UI can render its chips."""
    try:
        session = session_runner.manager.get(session_id)
    except session_runner.SessionError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return catalog.routes_for(session.map_name) or {}

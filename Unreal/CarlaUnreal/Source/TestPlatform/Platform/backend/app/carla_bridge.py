"""Lazy CARLA client probe.

The configuration endpoints must work with the simulator closed (the UI is often
opened before the world is running), so every CARLA touch is guarded and the
result is cached briefly instead of blocking a request.
"""
from __future__ import annotations

from . import paths

import threading
import time

from . import paths

try:  # the carla wheel is installed in the chat_carla env
    import carla  # type: ignore
except Exception as exc:  # pragma: no cover - reported through status()
    carla = None
    _import_error = exc
else:
    _import_error = None

_lock = threading.Lock()
_client = None
_status: dict | None = None
_status_at = 0.0
_TTL = 5.0


def _probe() -> dict:
    global _client
    if carla is None:
        return {
            "connected": False,
            "host": paths.CARLA_HOST,
            "port": paths.CARLA_PORT,
            "error": f"carla package unavailable: {_import_error}",
        }
    try:
        client = carla.Client(paths.CARLA_HOST, paths.CARLA_PORT)
        client.set_timeout(1.0)
        version = client.get_server_version()
        _client = client
        return {
            "connected": True,
            "host": paths.CARLA_HOST,
            "port": paths.CARLA_PORT,
            "server_version": version,
            "client_version": client.get_client_version(),
        }
    except Exception as exc:
        _client = None
        return {
            "connected": False,
            "host": paths.CARLA_HOST,
            "port": paths.CARLA_PORT,
            "error": str(exc),
        }


def status(force: bool = False) -> dict:
    global _status, _status_at
    with _lock:
        if not force and _status is not None and (time.time() - _status_at) < _TTL:
            return _status
        _status = _probe()
        _status_at = time.time()
        return _status


def client():
    """Connected client, or None when no server answers."""
    if status()["connected"]:
        return _client
    return None


def blueprint_ids(kind: str = "vehicle") -> list[str] | None:
    """Vehicle/walker blueprint ids from the live world, or None if offline."""
    cli = client()
    if cli is None:
        return None
    try:
        return sorted(bp.id for bp in cli.get_world().get_blueprint_library().filter(kind))
    except Exception:
        return None


def reset() -> None:
    global _status, _status_at, _client
    with _lock:
        _status = None
        _status_at = 0.0
        _client = None

def reset_settings() -> bool:
    """Force the simulator back into asynchronous mode.

    Used at backend startup: if a previous run crashed while the world was in
    synchronous mode, no one ticks it any more and every client call times out
    until the settings are cleared.
    """
    try:
        import carla  # noqa: PLC0415

        client = carla.Client(paths.CARLA_HOST, paths.CARLA_PORT)
        client.set_timeout(5.0)
        world = client.get_world()
        settings = world.get_settings()
        if settings.synchronous_mode or settings.fixed_delta_seconds:
            settings.synchronous_mode = False
            settings.fixed_delta_seconds = 0.0
            world.apply_settings(settings)
            return True
    except Exception:  # noqa: BLE001
        return False
    return False


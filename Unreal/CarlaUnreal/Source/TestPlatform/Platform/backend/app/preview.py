"""Vector map previews rendered from OpenDRIVE, cached on disk.

The preview needs no running CARLA server: the .xodr shipped next to the maps
already carries every road's planView geometry.  The renderer is the offline
tool in `tools/render_map_preview.py`, imported here so the CLI and the API
produce byte-identical output.
"""
from __future__ import annotations

import hashlib
import sys
import threading
from pathlib import Path

from . import paths

if str(paths.TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(paths.TOOLS_DIR))

import render_map_preview as renderer  # noqa: E402  (path set up above)

_roads_lock = threading.Lock()
_roads_cache: dict[str, dict] = {}


def xodr_path(map_name: str) -> Path | None:
    """Locate <Maps>/.../<map>.xodr, ignoring the *_Opt.xodr variants."""
    if not paths.MAPS_DIR.is_dir():
        return None
    direct = paths.MAPS_DIR / "OpenDrive" / f"{map_name}.xodr"
    if direct.is_file():
        return direct
    for candidate in paths.MAPS_DIR.rglob(f"{map_name}.xodr"):
        return candidate
    return None


def roads_for(map_name: str) -> dict:
    with _roads_lock:
        cached = _roads_cache.get(map_name)
        if cached is not None:
            return cached
    path = xodr_path(map_name)
    if path is None:
        raise FileNotFoundError(f"OpenDRIVE for {map_name} not found under {paths.MAPS_DIR}")
    roads = renderer.parse_roads(str(path), step=1.5)
    with _roads_lock:
        _roads_cache[map_name] = roads
    return roads


def preview_svg(
    map_name: str,
    preset: dict | None = None,
    focus: tuple[float, float] | None = None,
    half_size: float | None = None,
    width: int = 900,
    height: int = 520,
    routes_only: bool = False,
) -> str:
    """Return the SVG for a map (optionally cropped, route-only or with overlay)."""
    roads = roads_for(map_name)
    key = hashlib.sha1(
        "|".join(
            [
                map_name,
                preset["id"] if preset else "-",
                f"{focus[0]:.1f},{focus[1]:.1f}" if focus else "-",
                f"{half_size:.1f}" if half_size else "-",
                f"{width}x{height}",
                "routes" if routes_only else "full",
            ]
        ).encode()
    ).hexdigest()[:12]
    cached = paths.PREVIEW_CACHE / f"{map_name}-{key}.svg"
    if cached.is_file():
        return cached.read_text(encoding="utf-8")

    svg = renderer.build_svg(
        roads,
        preset,
        width=width,
        height=height,
        focus=focus,
        half_size=half_size,
        routes_only=routes_only,
    )
    cached.write_text(svg, encoding="utf-8")
    return svg


def clear_memory_cache() -> None:
    with _roads_lock:
        _roads_cache.clear()

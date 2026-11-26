"""Rasterised map thumbnails for the native (Slate) UI.

Slate cannot draw SVG, so the map cards get a small PNG rendered here from the
same OpenDRIVE geometry the SVG previews use. Cached on disk like the SVGs.
"""
from __future__ import annotations

import hashlib
import io
import json

from PIL import Image, ImageDraw

from . import catalog, paths, preview as preview_mod

BG = (10, 16, 21)
ROAD = (47, 68, 83)
ROUTE = (255, 180, 84)
START = (34, 211, 166)
END = (255, 180, 84)


def thumb_png(map_name: str, width: int = 208, height: int = 96,
              preset_id: str | None = None) -> bytes:
    key = hashlib.sha1(
        f"{map_name}|{preset_id or '-'}|{width}x{height}".encode()
    ).hexdigest()[:12]
    cached = paths.PREVIEW_CACHE / f"{map_name}-thumb-{key}.png"
    if cached.is_file():
        return cached.read_bytes()

    roads = preview_mod.roads_for(map_name)
    points = [p for parts in roads.values() for _s, pts in parts for p in pts]
    if not points:
        raise ValueError(f"no geometry for {map_name}")

    preset = None
    if preset_id:
        data = catalog.routes_for(map_name)
        if data:
            preset = next((p for p in data.get("presets", [])
                           if p["id"] == preset_id), None)

    # route geometry in the same metres the roads use
    route_parts: list[list[tuple[float, float]]] = []
    if preset:
        try:
            route_parts = [preview_mod.renderer.route_polyline(roads, preset)]
        except Exception:  # noqa: BLE001  (a bad preset must not break the card)
            route_parts = []

    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    min_x, max_x, min_y, max_y = min(xs), max(xs), min(ys), max(ys)
    span_x = max(1e-3, max_x - min_x)
    span_y = max(1e-3, max_y - min_y)

    pad = max(4, min(width, height) // 16)
    scale = min((width - 2 * pad) / span_x, (height - 2 * pad) / span_y)
    off_x = (width - span_x * scale) / 2.0
    off_y = (height - span_y * scale) / 2.0

    def to_px(point: tuple[float, float]) -> tuple[float, float]:
        # OpenDRIVE Y grows north, image Y grows down: flip it
        return (off_x + (point[0] - min_x) * scale,
                height - (off_y + (point[1] - min_y) * scale))

    image = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(image)

    for _road_id, parts in roads.items():
        for _s0, pts in parts:
            if len(pts) < 2:
                continue
            draw.line([to_px(p) for p in pts], fill=ROAD, width=1)

    if route_parts:
        for pts in route_parts:
            if len(pts) < 2:
                continue
            draw.line([to_px(p) for p in pts], fill=ROUTE, width=2)
        start = to_px(route_parts[0][0])
        draw.ellipse([start[0] - 3, start[1] - 3, start[0] + 3, start[1] + 3],
                     outline=START, width=2)
        end = to_px(route_parts[-1][-1])
        draw.ellipse([end[0] - 2, end[1] - 2, end[0] + 2, end[1] + 2], fill=END)

    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    payload = buffer.getvalue()
    cached.write_bytes(payload)
    return payload


def vehicle_png(vehicle_id: str, cls: str, width: int = 132, height: int = 64) -> bytes:
    """Real in-simulator render when available, otherwise simple placeholder art.

    The renders come from ``tools/render_vehicle_thumbs.py`` (an RGB camera sensor
    in the live world) and live in ``backend/data/vehicle_thumbs/<id>.png``.
    """
    rendered = paths.DATA_DIR / "vehicle_thumbs" / f"{vehicle_id}.png"
    if rendered.is_file():
        return rendered.read_bytes()

    key = hashlib.sha1(f"{vehicle_id}|{cls}|{width}x{height}".encode()).hexdigest()[:12]
    cached = paths.PREVIEW_CACHE / f"{vehicle_id.replace('/', '_')}-{key}.png"
    if cached.is_file():
        return cached.read_bytes()

    tint = {
        "轿车": (32, 90, 120),
        "跑车": (120, 60, 90),
        "SUV": (60, 110, 80),
        "卡车": (110, 90, 50),
        "摩托车": (90, 70, 110),
        "自行车": (70, 100, 110),
    }.get(cls, (60, 70, 85))

    image = Image.new("RGB", (width, height), (18, 26, 33))
    draw = ImageDraw.Draw(image)
    # a very schematic car silhouette: body + cabin + two wheels
    body_top = int(height * 0.45)
    body_bottom = int(height * 0.75)
    draw.rounded_rectangle([10, body_top, width - 10, body_bottom], radius=6, fill=tint)
    draw.rounded_rectangle([int(width * 0.28), int(height * 0.26),
                            int(width * 0.72), body_top + 2], radius=5,
                           fill=tuple(min(255, c + 28) for c in tint))
    for cx in (int(width * 0.28), int(width * 0.72)):
        r = int(height * 0.13)
        draw.ellipse([cx - r, body_bottom - r, cx + r, body_bottom + r], fill=(28, 34, 40))

    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    payload = buffer.getvalue()
    cached.write_bytes(payload)
    return payload


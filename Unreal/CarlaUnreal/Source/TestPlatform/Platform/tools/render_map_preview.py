#!/usr/bin/env python3
"""Render a real, vector map preview straight from a CARLA map's OpenDRIVE.

Nothing here needs a running CARLA server: the .xodr shipped with the maps
already contains every road's planView geometry (line / arc / spiral / poly3),
so the preview can be produced offline and served as SVG by the backend:

    GET /maps/{map}/preview.svg            真实路网
    GET /maps/{map}/preview.svg?preset=loop  + 该预设的路线叠加

SVG is the point: it stays sharp at any zoom, the frontend can recolour or
highlight it with CSS, and the route polyline can be injected as-is.

Usage:
    python render_map_preview.py --xodr <Town01.xodr> --out <dir> [--preset-json ...]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import xml.etree.ElementTree as ET


# --------------------------------------------------------------------------- #
# geometry sampling
# --------------------------------------------------------------------------- #
def sample_geometry(g, step: float = 1.5):
    """Sample one <geometry> element into world-space points."""
    x0 = float(g.get("x"))
    y0 = float(g.get("y"))
    hdg0 = float(g.get("hdg"))
    length = float(g.get("length") or 0.0)
    if length <= 0.0:
        return [(x0, y0)]

    kind, curvature = "line", 0.0
    for child in g:
        kind = child.tag
        if kind == "arc":
            curvature = float(child.get("curvature") or 0.0)
        elif kind == "spiral":
            c0 = float(child.get("curvStart") or 0.0)
            c1 = float(child.get("curvEnd") or 0.0)
            curvature = 0.5 * (c0 + c1)
        break

    n = max(1, int(length / step))
    pts = []
    for i in range(n + 1):
        t = length * i / n
        if abs(curvature) < 1e-9:
            dx, dy = t * math.cos(hdg0), t * math.sin(hdg0)
        else:
            dx = (math.sin(hdg0 + curvature * t) - math.sin(hdg0)) / curvature
            dy = -(math.cos(hdg0 + curvature * t) - math.cos(hdg0)) / curvature
        pts.append((x0 + dx, y0 + dy))
    return pts


def parse_roads(path: str, step: float = 1.5):
    """road_id -> list of (s_start, points) so we can cut a piece at any s."""
    roads: dict[int, list[tuple[float, list[tuple[float, float]]]]] = {}
    current = None
    pending: list[tuple[float, list[tuple[float, float]]]] = []

    for _ev, el in ET.iterparse(path, events=("end",)):
        if el.tag == "road":
            rid = int(el.get("id"))
            roads[rid] = pending
            pending = []
            el.clear()
        elif el.tag == "geometry":
            # geometry lives under road/planView; roads are emitted in file order
            pending.append((float(el.get("s") or 0.0), sample_geometry(el, step)))
            el.clear()
    return roads


def point_at(road_parts, s: float):
    if not road_parts:
        return None
    spans = [(s0, geometry_length(pts), pts) for s0, pts in road_parts if len(pts) > 1]
    if not spans:
        return road_parts[0][1][0] if road_parts[0][1] else None
    total = max(s0 + span for s0, span, _pts in spans)
    s = max(0.0, min(float(s), total))
    for s0, span, pts in spans:
        if s0 - 1e-6 <= s <= s0 + span + 1e-6:
            frac = (s - s0) / span if span > 0 else 0.0
            idx = max(0.0, min(1.0, frac)) * (len(pts) - 1)
            i0 = int(idx)
            i1 = min(i0 + 1, len(pts) - 1)
            t = idx - i0
            return (
                pts[i0][0] + (pts[i1][0] - pts[i0][0]) * t,
                pts[i0][1] + (pts[i1][1] - pts[i0][1]) * t,
            )
    s0, span, pts = spans[-1] if s > spans[-1][0] else spans[0]
    return pts[-1] if s > s0 else pts[0]


def geometry_length(pts):
    total = 0.0
    for a, b in zip(pts, pts[1:]):
        total += math.hypot(b[0] - a[0], b[1] - a[1])
    return total


def slice_polyline(road_parts, s_from: float, s_to: float, step: float = 2.0):
    """World points along the road between two s values (either direction)."""
    if s_from > s_to:
        s_from, s_to = s_to, s_from
    pts = []
    for s0, geom_pts in road_parts:
        if len(geom_pts) < 2:
            continue
        span = geometry_length(geom_pts)
        s1, s2 = s0, s0 + span
        if s2 < s_from or s1 > s_to:
            continue
        lo, hi = max(s1, s_from), min(s2, s_to)
        n = max(2, int((hi - lo) / step) + 1)
        for i in range(n + 1):
            s = lo + (hi - lo) * i / n
            p = point_at([(s0, geom_pts)], s)
            if p:
                pts.append(p)
    return pts


# --------------------------------------------------------------------------- #
# svg
# --------------------------------------------------------------------------- #
def route_polyline(roads, wps):
    """Continuous world-space polyline through a preset's (road, s) waypoints."""
    out = []
    for a, b in zip(wps, wps[1:]):
        if a["road_id"] == b["road_id"]:
            seg = slice_polyline(roads.get(a["road_id"], []), a["s"], b["s"])
            if len(seg) >= 2 and out:
                # a reversed traversal must be emitted from the end we are standing on
                if (math.hypot(seg[-1][0] - out[-1][0], seg[-1][1] - out[-1][1]) <
                        math.hypot(seg[0][0] - out[-1][0], seg[0][1] - out[-1][1])):
                    seg.reverse()
            if seg:
                out.extend(seg if not out else seg[1:])
        else:
            # across a junction: bridge the gap between the two road ends
            pa = point_at(roads.get(a["road_id"], []), a["s"])
            pb = point_at(roads.get(b["road_id"], []), b["s"])
            for p in (pa, pb):
                if p and not (out and math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) < 1.0):
                    out.append(p)
    return out


def build_svg(roads, preset=None, width=900, height=520, pad=None,
              focus=None, half_size=None, routes_only=False):
    all_pts = [p for parts in roads.values() for _s, pts in parts for p in pts]
    if not all_pts:
        raise SystemExit("no geometry")
    if pad is None:  # keep small thumbnails from being eaten by the margin
        pad = max(6, min(width, height) // 40)
    xs = [p[0] for p in all_pts]
    ys = [p[1] for p in all_pts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    if focus and half_size:
        minx, maxx = focus[0] - half_size, focus[0] + half_size
        miny, maxy = focus[1] - half_size, focus[1] + half_size
    scale = min((width - 2 * pad) / max(1e-6, maxx - minx),
                (height - 2 * pad) / max(1e-6, maxy - miny))

    def tf(p):
        u = pad + (p[0] - minx) * scale
        v = height - pad - (p[1] - miny) * scale
        return u, v

    parts = []
    if not routes_only:
        for rid, road_parts in roads.items():
            for _s0, pts in road_parts:
                if len(pts) < 2:
                    continue
                d = "M" + " L".join(f"{u:.1f},{v:.1f}" for u, v in (tf(p) for p in pts))
                parts.append(
                    f'<path d="{d}" fill="none" stroke="#2f4453" stroke-width="2.1" '
                    f'stroke-linecap="round" stroke-linejoin="round"/>'
                )

    overlay = ""
    if preset and preset.get("waypoints"):
        line = route_polyline(roads, preset["waypoints"])
        if line:
            d = "M" + " L".join(f"{u:.1f},{v:.1f}" for u, v in (tf(p) for p in line))
            overlay = (
                f'<path d="{d}" fill="none" stroke="#000" stroke-opacity=".55" stroke-width="7" '
                f'stroke-linecap="round" stroke-linejoin="round"/>'
                f'<path d="{d}" fill="none" stroke="#ffb454" stroke-width="3.2" '
                f'stroke-linecap="round" stroke-linejoin="round"/>'
            )
            start = tf(line[0])
            end = tf(line[-1])
            overlay += (
                f'<circle cx="{start[0]:.1f}" cy="{start[1]:.1f}" r="9" fill="#0b1016" '
                f'stroke="#22d3a6" stroke-width="2.4"/>'
                f'<circle cx="{end[0]:.1f}" cy="{end[1]:.1f}" r="7" fill="#0b1016" '
                f'stroke="#ffb454" stroke-width="2.4"/>'
            )

    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}">'
        + (""
           if routes_only
           else f'<rect width="{width}" height="{height}" fill="#0a1015"/>')
        + "".join(parts) + overlay + "</svg>"
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xodr", required=True)
    ap.add_argument("--out", required=True, help="output svg path")
    ap.add_argument("--preset-json", default=None,
                    help="route preset file (backend/data/routes/<Map>.json)")
    ap.add_argument("--preset-id", default=None)
    ap.add_argument("--routes-only", action="store_true",
                    help="transparent background, only the route + endpoints")
    ap.add_argument("--step", type=float, default=1.5)
    ap.add_argument("--focus-x", type=float, default=None, help="crop centre, world X")
    ap.add_argument("--focus-y", type=float, default=None, help="crop centre, world Y")
    ap.add_argument("--half-size", type=float, default=None, help="crop half width in metres")
    args = ap.parse_args(argv)

    roads = parse_roads(args.xodr, args.step)
    preset = None
    if args.preset_json:
        data = json.load(open(args.preset_json, encoding="utf-8"))
        want = args.preset_id or (data["presets"][0]["id"] if data.get("presets") else None)
        preset = next((p for p in data.get("presets", []) if p["id"] == want), None)

    focus = None
    if args.focus_x is not None and args.focus_y is not None and args.half_size:
        focus = (args.focus_x, args.focus_y)
    svg = build_svg(roads, preset, focus=focus, half_size=args.half_size, routes_only=args.routes_only)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(svg)
    n_pts = sum(len(pts) for parts in roads.values() for _s, pts in parts)
    print(f"{os.path.basename(args.out)}: {len(roads)} roads, {n_pts} points"
          + (f", preset={preset['id']} {preset['length_m']:.0f} m" if preset else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


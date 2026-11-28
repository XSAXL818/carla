"""Isolate the overlay from camera drift with a three-frame comparison.

Frames: A = overlay ON, B = overlay OFF (7 s later), C = overlay ON again (7 s
later). The camera keeps easing between frames, so A-vs-B and A-vs-C both contain
camera motion; the *difference between those two diffs* is the overlay.

Reports, for each pair, how many pixels changed and how many of the changed
pixels in the ON frame carry the overlay colour.
"""
from __future__ import annotations

import sys

from PIL import Image

TARGET = (255, 180, 84)


def load(path):
    return Image.open(path).convert("RGB")


def diff(a, b, tol=24):
    pa, pb = a.load(), b.load()
    w, h = a.size
    changed = []
    for y in range(h):
        for x in range(w):
            ca, cb = pa[x, y], pb[x, y]
            if abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2]) > tol:
                changed.append((x, y, ca))
    return changed


def route_like(rgb, tol=(45, 45, 55)):
    return (abs(rgb[0] - TARGET[0]) <= tol[0] and abs(rgb[1] - TARGET[1]) <= tol[1]
            and abs(rgb[2] - TARGET[2]) <= tol[2])


def report(name, a, b):
    changed = diff(a, b)
    route = sum(1 for _x, _y, c in changed if route_like(c))
    print(f"{name:<26} changed {len(changed):>7} | route-coloured {route:>6} "
          f"({100.0 * route / max(1, len(changed)):5.1f} %)")
    return changed, route


def main():
    on1, off, on2 = sys.argv[1], sys.argv[2], sys.argv[3]
    a, b, c = load(on1), load(off), load(on2)
    print(f"sizes {a.size} {b.size} {c.size}")
    _ch_ab, route_ab = report("A(on) vs B(off)", a, b)
    _ch_ac, route_ac = report("A(on) vs C(on again)", a, c)
    print(f"\noverlay-attributable route pixels: {route_ab - route_ac:+d} "
          f"(A-vs-B minus the camera-drift baseline A-vs-C)")


if __name__ == "__main__":
    main()

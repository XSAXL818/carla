"""Prove the orange pixels are the route overlay, not road paint.

Diffs the route-on and route-off frames taken from the same camera pose and
reports how many changed pixels carry the overlay colour (255, 180, 84).
"""
from __future__ import annotations

import sys

from PIL import Image

ON = sys.argv[1]
OFF = sys.argv[2]


def main():
    a = Image.open(ON).convert("RGB")
    b = Image.open(OFF).convert("RGB")
    if a.size != b.size:
        print(f"size mismatch {a.size} vs {b.size}")
        return 1
    w, h = a.size
    pa, pb = a.load(), b.load()
    changed = 0
    route_like = 0
    bands = [0] * 9
    for y in range(h):
        for x in range(w):
            ca, cb = pa[x, y], pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])) <= 24:
                continue
            changed += 1
            bands[min(8, y * 9 // h)] += 1
            if (abs(ca[0] - 255) <= 40 and abs(ca[1] - 180) <= 40
                    and abs(ca[2] - 84) <= 50):
                route_like += 1
    print(f"size {w}x{h}")
    print(f"changed pixels (>24/255): {changed}")
    if changed:
        print(f"route-coloured on the ON frame: {route_like} "
              f"({100.0 * route_like / changed:.1f} %)")
    print(f"changed pixels per 100-row band: {bands}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

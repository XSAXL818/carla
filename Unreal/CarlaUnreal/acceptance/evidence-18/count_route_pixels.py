"""Is the route overlay colour actually on screen?

The backend draws the route with carla.Color(255, 180, 84). Debug lines are
unlit, so those pixels should survive rendering almost unchanged. This counts
them per frame (with a tolerance) plus a looser "orange-ish" band, and reports
the bounding box, so "the route line is visible" becomes a number instead of an
impression.
"""
from __future__ import annotations

import glob
import os
import sys

from PIL import Image

TARGET = (255, 180, 84)
TOL = 26


def near(px, target, tol):
    return all(abs(p - t) <= tol for p, t in zip(px, target))


def main():
    paths = sorted(glob.glob(sys.argv[1]))
    for path in paths:
        img = Image.open(path).convert("RGB")
        w, h = img.size
        px = img.load()
        exact = []
        orange = 0
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                p = px[x, y]
                if near(p, TARGET, TOL):
                    exact.append((x, y))
                elif p[0] > 170 and 90 < p[1] < 210 and p[2] < 120:
                    orange += 1
        if exact:
            xs = [p[0] for p in exact]
            ys = [p[1] for p in exact]
            bbox = (min(xs), min(ys), max(xs), max(ys))
        else:
            bbox = None
        print(f"{os.path.basename(path):<18} exact_route_px={len(exact):6d} (sampled 1/4) "
              f"orange_px={orange:6d} bbox={bbox}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

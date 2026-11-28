"""Diff an overlay-ON frame against an overlay-OFF frame from the same camera.

With the ego parked the follow camera does not move, so anything that disappears
between the two frames is the route overlay (the backend stops redrawing it and
its 5 s lifetime expires). Reports the changed-pixel count, bounding box, the
mean colour of the changed pixels, and writes a visual diff.
"""
from __future__ import annotations

import sys

from PIL import Image, ImageChops

on_path, off_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
on = Image.open(on_path).convert("RGB")
off = Image.open(off_path).convert("RGB")
diff = ImageChops.difference(on, off)
gray = diff.convert("L")
w, h = gray.size
px = gray.load()
onpx = on.load()
changed = [(x, y) for y in range(h) for x in range(w) if px[x, y] > 24]
print(f"frame size {w}x{h}; changed pixels (>24/255): {len(changed)}")
if changed:
    xs = [p[0] for p in changed]
    ys = [p[1] for p in changed]
    cols = [onpx[p[0], p[1]] for p in changed[:: max(1, len(changed) // 400)]]
    avg = tuple(sum(c[i] for c in cols) // len(cols) for i in range(3))
    print(f"bbox=({min(xs)},{min(ys)})-({max(xs)},{max(ys)})  "
          f"on-frame mean colour of changed pixels={avg}")
    # how many changed pixels were route-coloured (255,180,84) on the ON frame?
    def near(px_):
        return all(abs(a - b) <= 30 for a, b in zip(px_, (255, 180, 84)))
    n = sum(1 for p in changed if near(onpx[p[0], p[1]]))
    print(f"of those, route-coloured (255,180,84 +-30) on the ON frame: {n} "
          f"({100.0 * n / len(changed):.1f} %)")
    # mass per 100-row band, so a long thin line is distinguishable from a car
    bands: dict[int, int] = {}
    for x, y in changed:
        bands[y // 100] = bands.get(y // 100, 0) + 1
    print("changed pixels per 100-row band:", dict(sorted(bands.items())))
else:
    print("NO difference: the overlay is not drawn at all (or the camera moved)")
diff.save(out_path)
print(f"diff written to {out_path}")

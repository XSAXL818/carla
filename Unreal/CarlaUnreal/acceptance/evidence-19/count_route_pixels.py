"""Count route-coloured pixels in the on / off / on-again frames.

Camera-independent: the overlay colour is (255, 180, 84), so if the overlay is
drawn there are thousands of such pixels and when it is off there are almost
none. That is a stronger statement than a pixel diff, which is dominated by the
chase camera still interpolating between frames.
"""
from __future__ import annotations

import sys

from PIL import Image

TARGET = (255, 180, 84)


def count(path, tol=(38, 38, 48)):
    img = Image.open(path).convert("RGB")
    px = img.load()
    w, h = img.size
    hits = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if (abs(r - TARGET[0]) <= tol[0] and abs(g - TARGET[1]) <= tol[1]
                    and abs(b - TARGET[2]) <= tol[2]):
                hits += 1
    return hits, w * h


def main():
    for path in sys.argv[1:]:
        hits, total = count(path)
        print(f"{path:<34} route-coloured pixels {hits:>7}  ({100.0 * hits / total:.3f} % of frame)")


if __name__ == "__main__":
    main()

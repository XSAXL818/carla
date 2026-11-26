"""Finish the vehicle card art: punchy colour + soft cut-out.

The frames come from a real camera in the simulator, but a rectangular photo of
a car sitting on asphalt reads as "grey box" in a card grid. Two cheap, reliable
steps turn it into hero-card art:

* a soft elliptical alpha mask around the car (same framing for every render, so
  one mask fits all of them), which lets the car sit on the card's dark plate;
* a mild saturation/contrast lift.

Originals are copied to data/vehicle_art_raw/ before being overwritten.

    python tools/postprocess_vehicle_art.py
"""
from __future__ import annotations

import argparse
import math
import shutil
from pathlib import Path

from PIL import Image, ImageEnhance

DATA = Path(__file__).resolve().parents[1] / "backend" / "data"
STILLS = DATA / "vehicle_thumbs"
SPIN = DATA / "vehicle_spin"
RAW = DATA / "vehicle_art_raw"

# car placement is identical in every render: the same camera, only the car spins
ELLIPSE_CENTER = (0.47, 0.62)   # normalised
ELLIPSE_RADII = (0.42, 0.40)    # normalised
FEATHER = 0.14                  # softness of the mask edge


def ellipse_mask(size: tuple[int, int]) -> Image.Image:
    width, height = size
    cx, cy = ELLIPSE_CENTER[0] * width, ELLIPSE_CENTER[1] * height
    rx, ry = ELLIPSE_RADII[0] * width, ELLIPSE_RADII[1] * height
    mask = Image.new("L", (width, height), 0)
    pixels = mask.load()
    inner = 1.0 - FEATHER
    for y in range(height):
        dy = (y - cy) / ry if ry else 0.0
        for x in range(width):
            dx = (x - cx) / rx if rx else 0.0
            distance = math.sqrt(dx * dx + dy * dy)
            if distance <= inner:
                pixels[x, y] = 255
            elif distance < 1.0:
                # smooth falloff so the edge does not look like a sticker
                t = (1.0 - distance) / FEATHER
                pixels[x, y] = int(255 * (t * t * (3 - 2 * t)))
    return mask


def process(path: Path, saturation: float, contrast: float) -> bool:
    try:
        image = Image.open(path).convert("RGB")
    except Exception:
        return False
    relative = path.relative_to(DATA)
    backup = RAW / relative
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup)

    image = ImageEnhance.Color(image).enhance(saturation)
    image = ImageEnhance.Contrast(image).enhance(contrast)
    image = ImageEnhance.Brightness(image).enhance(1.05)
    out = image.convert("RGBA")
    out.putalpha(ellipse_mask(out.size))
    out.save(path, format="PNG", optimize=True)
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--saturation", type=float, default=1.22)
    parser.add_argument("--contrast", type=float, default=1.06)
    args = parser.parse_args(argv)

    done = 0
    for path in sorted(STILLS.glob("*.png")):
        done += 1 if process(path, args.saturation, args.contrast) else 0
    for path in sorted(SPIN.glob("*/*.png")):
        done += 1 if process(path, args.saturation, args.contrast) else 0
    print(f"{done} images processed (originals in {RAW})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

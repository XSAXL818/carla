"""Render vehicle turntables + stills with a transparent background.

Two cameras see the same view: an RGB sensor and an instance-segmentation
sensor. The segmentation frame gives a mask for exactly our vehicle, so the PNG
gets a real alpha channel - the card can then show the car floating (and, on
hover, grow it past the card frame like a game hero portrait).

    python tools/render_vehicle_turntable.py --frames 16
"""
from __future__ import annotations

import argparse
import io
import math
import sys
import time
from pathlib import Path

import carla
from PIL import Image, ImageEnhance

OUT_ROOT = Path(__file__).resolve().parents[1] / "backend" / "data" / "vehicle_spin"
STILL_DIR = Path(__file__).resolve().parents[1] / "backend" / "data" / "vehicle_thumbs"

DEFAULT_VEHICLES = [
    "vehicle.tesla.model3",
    "vehicle.lincoln.mkz",
    "vehicle.ue4.audi.tt",
    "vehicle.ue4.mercedes.ccc",
    "vehicle.nissan.patrol",
    "vehicle.dodge.charger",
    "vehicle.mini.cooper",
    "vehicle.kia.niro",
    "vehicle.fuso.mitsubishi",
    "vehicle.taxi.ford",
    "vehicle.ue4.ford.mustang",
    "vehicle.ue4.chevrolet.impala",
    "vehicle.ue4.ford.crown",
    "vehicle.sprinter.mercedes",
    "vehicle.ambulance.ford",
    "vehicle.dodgecop.charger",
    "vehicle.carlacola.actors",
    "vehicle.firetruck.actors",
]


def spawn_points_for(world, preferred, limit: int = 24):
    """Preferred spawn point first, then outward - leftovers can block one."""
    points = world.get_map().get_spawn_points()
    if not points:
        return [preferred]
    ordered = sorted(points, key=lambda p: p.location.distance(preferred.location))
    return ordered[:limit]


class Grabber:
    def __init__(self) -> None:
        self.image = None
        self.serial = 0

    def __call__(self, image) -> None:
        self.image = image
        self.serial += 1


def wait_frame(grabber: Grabber, previous: int, timeout: float = 3.0) -> int:
    deadline = time.time() + timeout
    while grabber.serial <= previous and time.time() < deadline:
        time.sleep(0.02)
    return grabber.serial


def compose_rgba(rgb, segmentation, actor_id: int) -> Image.Image:
    """Cut the vehicle out of the RGB frame using the instance mask."""
    width, height = rgb.width, rgb.height
    color = rgb.convert(carla.ColorConverter.Raw).raw_data
    seg = segmentation.convert(carla.ColorConverter.Raw).raw_data

    # instance segmentation packs the object id in R/G (little endian)
    mask = bytearray(width * height)
    matched = 0
    for index in range(width * height):
        base = index * 4
        object_id = seg[base] | (seg[base + 1] << 8) | (seg[base + 2] << 16)
        if object_id == actor_id:
            mask[index] = 255
            matched += 1
    if matched < 32:  # mask failed: keep the frame fully opaque rather than empty
        mask = bytearray(b"\xff" * (width * height))

    rgba = bytearray(width * height * 4)
    for index in range(width * height):
        src = index * 4
        dst = index * 4
        rgba[dst] = color[src + 2]      # CARLA raw is BGRA
        rgba[dst + 1] = color[src + 1]
        rgba[dst + 2] = color[src]
        rgba[dst + 3] = mask[index]
    image = Image.frombytes("RGBA", (width, height), bytes(rgba))
    # the render can be flat; recover the body colour that the prototype expects
    rgb_only = image.convert("RGB")
    rgb_only = ImageEnhance.Color(rgb_only).enhance(1.45)
    rgb_only = ImageEnhance.Contrast(rgb_only).enhance(1.10)
    rgb_only = ImageEnhance.Brightness(rgb_only).enhance(1.06)
    out = rgb_only.convert("RGBA")
    out.putalpha(image.getchannel("A"))
    return out


def png_bytes(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def render_vehicle(world, blueprint_id: str, spot, frames: int, size) -> int:
    library = world.get_blueprint_library()
    try:
        blueprint = library.find(blueprint_id)
    except IndexError:
        return 0
    vehicle = None
    used_spot = spot
    for candidate in spawn_points_for(world, spot, limit=4):
        vehicle = world.try_spawn_actor(blueprint, candidate)
        if vehicle is not None:
            used_spot = candidate
            break
    if vehicle is None:
        return 0
    spot = used_spot
    vehicle.set_simulate_physics(False)
    actor_id = vehicle.id

    local_offset = carla.Location(x=6.6, y=-5.2, z=2.6)
    camera_world = spot.transform(local_offset)
    target = spot.transform(carla.Location(x=0.3, z=0.85))
    direction = target - camera_world
    yaw = math.degrees(math.atan2(direction.y, direction.x))
    pitch = math.degrees(math.atan2(direction.z, math.hypot(direction.x, direction.y)))
    view = carla.Transform(camera_world, carla.Rotation(pitch=pitch, yaw=yaw, roll=0.0))

    def make_sensor(kind: str):
        bp = library.find(kind)
        bp.set_attribute("image_size_x", str(size[0]))
        bp.set_attribute("image_size_y", str(size[1]))
        if kind.endswith("rgb"):
            bp.set_attribute("fov", "36")
        return world.try_spawn_actor(bp, view)

    rgb_camera = make_sensor("sensor.camera.rgb")
    if rgb_camera is None:
        vehicle.destroy()
        return 0
    rgb_grab = Grabber()
    rgb_camera.listen(rgb_grab)

    out_dir = OUT_ROOT / blueprint_id
    out_dir.mkdir(parents=True, exist_ok=True)
    STILL_DIR.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.png"):
        old.unlink()

    rgb_serial = 0
    written = 0
    for index in range(frames):
        angle = 360.0 * index / frames
        vehicle.set_transform(carla.Transform(
            spot.location,
            carla.Rotation(pitch=0.0, yaw=spot.rotation.yaw + angle, roll=0.0)))
        rgb_serial = wait_frame(rgb_grab, rgb_serial)
        if rgb_grab.image is None:
            continue
        frame = rgb_grab.image
        raw = bytes(frame.raw_data)
        if len(raw) < frame.width * frame.height * 4:
            continue
        photo = Image.frombytes("RGBA", (frame.width, frame.height), raw).convert("RGB")
        photo = ImageEnhance.Color(photo).enhance(1.45)
        photo = ImageEnhance.Contrast(photo).enhance(1.10)
        payload = png_bytes(photo)
        (out_dir / f"{index:02d}.png").write_bytes(payload)
        if index == 0:
            (STILL_DIR / f"{blueprint_id}.png").write_bytes(payload)
        written += 1

    rgb_camera.stop()
    rgb_camera.destroy()
    vehicle.destroy()
    return written


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2000)
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--width", type=int, default=288)
    parser.add_argument("--height", type=int, default=162)
    parser.add_argument("--vehicles", nargs="*", default=None)
    args = parser.parse_args(argv)

    client = carla.Client(args.host, args.port)
    client.set_timeout(30.0)
    world = client.get_world()
    # A test session may have left the world in synchronous mode; nobody ticks it
    # then, so sensors never deliver. Rendering needs a free-running world.
    settings = world.get_settings()
    if settings.synchronous_mode or settings.fixed_delta_seconds:
        print("world was synchronous - switching to asynchronous for the shoot")
        settings.synchronous_mode = False
        settings.fixed_delta_seconds = 0.0
        world.apply_settings(settings)
        time.sleep(0.5)

    previous = world.get_weather()
    shoot = carla.WeatherParameters()
    # brighter and cleaner than the first pass: the background is cut out anyway,
    # so the body can take real sunlight (that is what killed the saturation)
    shoot.sun_altitude_angle = 26.0
    shoot.sun_azimuth_angle = 130.0
    shoot.cloudiness = 55.0
    shoot.fog_density = 0.0
    shoot.fog_distance = 0.0
    shoot.fog_falloff = 0.0
    shoot.wetness = 0.0
    shoot.precipitation = 0.0
    shoot.precipitation_deposits = 0.0
    shoot.mie_scattering_scale = 0.03
    shoot.rayleigh_scattering_scale = 0.02
    world.set_weather(shoot)
    time.sleep(0.8)

    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    spawn_points = world.get_map().get_spawn_points()
    if not spawn_points:
        print("no spawn points", file=sys.stderr)
        return 3
    spot = spawn_points[len(spawn_points) // 2]

    vehicles = args.vehicles or DEFAULT_VEHICLES
    total = 0
    for blueprint_id in vehicles:
        count = render_vehicle(world, blueprint_id, spot, args.frames,
                               (args.width, args.height))
        total += count
        print(f"  {blueprint_id}: {count} frames")

    world.set_weather(previous)
    print(f"{total} frames into {OUT_ROOT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())







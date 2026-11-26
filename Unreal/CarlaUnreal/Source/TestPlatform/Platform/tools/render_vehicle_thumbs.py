"""Render real vehicle thumbnails with an in-simulator camera.

Painted placeholders are not convincing, so every car card gets a genuine render:
spawn the blueprint at a free spawn point, park an RGB camera at a 3/4 front
angle, grab one frame, save a PNG and destroy the actors.

Runs against a live CARLA server in *asynchronous* mode (i.e. when no test
session owns the tick loop). Output goes to backend/data/vehicle_thumbs/, which
the backend serves at /vehicles/<blueprint>/thumb.png.

    python tools/render_vehicle_thumbs.py --host 127.0.0.1 --port 2000
"""
from __future__ import annotations

import argparse
import io
import math
import sys
import time
from pathlib import Path

import carla
from PIL import Image

OUT_DIR = Path(__file__).resolve().parents[1] / "backend" / "data" / "vehicle_thumbs"

# the nine cards the UI shows first
# the ids this CARLA build actually ships (ue58 uses the *ue4.* naming for some)
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
    "vehicle.carlacola.actors",
    "vehicle.sprinter.mercedes",
    "vehicle.ambulance.ford",
    "vehicle.dodgecop.charger",
    "vehicle.firetruck.actors",
]


class FrameGrabber:
    """Collects the newest frame from a camera sensor."""

    def __init__(self) -> None:
        self.image = None
        self.bytes_per_pixel = 4

    def __call__(self, image: "carla.Image") -> None:
        self.image = image
        self.bytes_per_pixel = len(image.raw_data) // (image.width * image.height)


def render_one(world, blueprint_id: str, spawn_transform, size=(384, 216)) -> bytes | None:
    library = world.get_blueprint_library()
    try:
        blueprint = library.find(blueprint_id)
    except IndexError:
        return None
    if blueprint is None:
        return None

    vehicle = world.try_spawn_actor(blueprint, spawn_transform)
    if vehicle is None:
        return None

    # camera in the vehicle's own frame (3/4 front-left), aimed at the body
    local_offset = carla.Location(x=6.6, y=-5.2, z=2.6)
    camera_world = spawn_transform.transform(local_offset)
    target = spawn_transform.transform(carla.Location(x=0.3, z=0.85))
    direction = target - camera_world
    yaw = math.degrees(math.atan2(direction.y, direction.x))
    pitch = math.degrees(math.atan2(direction.z, math.hypot(direction.x, direction.y)))
    camera_transform = carla.Transform(camera_world, carla.Rotation(pitch=pitch, yaw=yaw, roll=0.0))

    grabber = FrameGrabber()
    camera_bp = library.find("sensor.camera.rgb")
    camera_bp.set_attribute("image_size_x", str(size[0]))
    camera_bp.set_attribute("image_size_y", str(size[1]))
    camera_bp.set_attribute("fov", "36")
    if camera_bp.has_attribute("exposure_compensation"):
        camera_bp.set_attribute("exposure_compensation", "-1.6")
    if camera_bp.has_attribute("exposure_max_bright"):
        camera_bp.set_attribute("exposure_max_bright", "1.0")
    if camera_bp.has_attribute("exposure_min_bright"):
        camera_bp.set_attribute("exposure_min_bright", "0.15")
    camera = world.try_spawn_actor(camera_bp, camera_transform)
    if camera is None:
        vehicle.destroy()
        return None
    camera.listen(grabber)  # async mode: the server keeps ticking on its own

    deadline = time.time() + 3.0
    while grabber.image is None and time.time() < deadline:
        time.sleep(0.05)
    image = grabber.image

    camera.stop()
    camera.destroy()
    vehicle.destroy()

    if image is None:
        return None
    if grabber.bytes_per_pixel == 4:  # BGRA
        raw = bytes(image.raw_data)
        img = Image.frombytes("RGBA", (image.width, image.height), raw)
        img = img.convert("RGB")
    else:
        raw = bytes(image.raw_data)
        img = Image.frombytes("RGB", (image.width, image.height), raw)

    buffer = io.BytesIO()
    img.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2000)
    parser.add_argument("--vehicles", nargs="*", default=None)
    args = parser.parse_args(argv)

    client = carla.Client(args.host, args.port)
    client.set_timeout(30.0)
    world = client.get_world()

    settings = world.get_settings()
    if settings.synchronous_mode:
        print("world is in synchronous mode (a session owns the tick loop); "
              "stop the session first", file=sys.stderr)
        return 2

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    previous_weather = world.get_weather()
    # soft, fog-free light for the shoot: no hard sun clipping white bodywork
    shoot_weather = carla.WeatherParameters()
    shoot_weather.sun_altitude_angle = 24.0
    shoot_weather.sun_azimuth_angle = 120.0
    shoot_weather.cloudiness = 60.0
    shoot_weather.fog_density = 0.0
    shoot_weather.fog_distance = 0.0
    shoot_weather.fog_falloff = 0.0
    shoot_weather.wetness = 0.0
    shoot_weather.precipitation = 0.0
    shoot_weather.precipitation_deposits = 0.0
    shoot_weather.scattering_intensity = 0.4
    shoot_weather.mie_scattering_scale = 0.1
    world.set_weather(shoot_weather)
    time.sleep(0.6)
    vehicles = args.vehicles or DEFAULT_VEHICLES
    spawn_points = world.get_map().get_spawn_points()
    if not spawn_points:
        print("no spawn points on this map", file=sys.stderr)
        return 3
    spot = spawn_points[len(spawn_points) // 2]

    written = 0
    for blueprint_id in vehicles:
        payload = render_one(world, blueprint_id, spot)
        if payload is None:
            print(f"  {blueprint_id}: FAILED")
            continue
        target = OUT_DIR / f"{blueprint_id}.png"
        target.write_bytes(payload)
        written += 1
        print(f"  {blueprint_id}: {len(payload) // 1024} KB -> {target.name}")
    world.set_weather(previous_weather)
    print(f"{written}/{len(vehicles)} rendered into {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())






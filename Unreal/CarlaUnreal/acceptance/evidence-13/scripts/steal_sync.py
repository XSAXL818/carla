"""Take synchronous mode away from whoever holds it, for --seconds seconds.

A second CARLA client applying asynchronous settings is a real way for a
synchronous session to lose its clock (another tool, a second panel, a script
that resets the world). This probe does it on purpose, repeatedly, so the
backend's reaction to a *sustained* loss can be observed instead of a one-frame
race.

Usage: python steal_sync.py --seconds 20 --every 0.4
"""
from __future__ import annotations

import argparse
import time

import carla


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2000)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--every", type=float, default=0.4)
    args = parser.parse_args()

    client = carla.Client(args.host, args.port)
    client.set_timeout(30.0)
    world = client.get_world()
    print(f"thief connected, map = {world.get_map().name}", flush=True)

    started = time.time()
    n = 0
    while time.time() - started < args.seconds:
        try:
            settings = world.get_settings()
            settings.synchronous_mode = False
            settings.fixed_delta_seconds = 0.0
            world.apply_settings(settings)
            n += 1
            print(f"t={time.time() - started:5.1f}s applied async (#{n})", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"t={time.time() - started:5.1f}s apply failed: {exc}", flush=True)
        time.sleep(args.every)
    print(f"done, {n} async applies", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

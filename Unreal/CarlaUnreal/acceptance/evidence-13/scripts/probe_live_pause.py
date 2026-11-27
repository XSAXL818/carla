"""Live-but-paused synchronous client probe.

Connects to a running CARLA server, switches it to synchronous mode at 20 Hz,
ticks exactly once, then pauses for --pause seconds WITHOUT sending any further
tick cue. Every --sample seconds it asks the server for its settings and the
current frame, so the transcript shows whether the simulator kept waiting for
this (alive) client or silently ejected it from synchronous mode.

Exit code 0 always; the transcript is the evidence.

Usage:
  python probe_live_pause.py --pause 70 --sample 2
"""
from __future__ import annotations

import argparse
import json
import time

import carla


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2000)
    parser.add_argument("--hz", type=float, default=20.0)
    parser.add_argument("--pause", type=float, default=70.0)
    parser.add_argument("--sample", type=float, default=2.0)
    args = parser.parse_args()

    client = carla.Client(args.host, args.port)
    client.set_timeout(30.0)
    world = client.get_world()
    print(f"connected map = {world.get_map().name}", flush=True)

    settings = world.get_settings()
    print(f"settings before: {settings.synchronous_mode} {settings.fixed_delta_seconds}", flush=True)
    settings.synchronous_mode = True
    settings.fixed_delta_seconds = 1.0 / args.hz
    world.apply_settings(settings)
    settings = world.get_settings()
    print(f"settings after apply: {settings.synchronous_mode} {settings.fixed_delta_seconds}", flush=True)

    frame0 = world.tick()
    print(f"ticked once, frame = {frame0}", flush=True)

    rows = []
    started = time.time()
    while True:
        elapsed = time.time() - started
        if elapsed > args.pause:
            break
        current = world.get_settings()
        try:
            frame = world.get_snapshot().frame
        except Exception:  # noqa: BLE001
            frame = -1
        row = {
            "t": round(elapsed, 1),
            "frame": int(frame),
            "sync": bool(current.synchronous_mode),
            "fixed_delta": (round(current.fixed_delta_seconds, 4)
                            if current.fixed_delta_seconds else None),
        }
        rows.append(row)
        print(json.dumps(row), flush=True)
        # No tick() here on purpose: this is the "live client is busy" pause.
        time.sleep(args.sample)

    last = rows[-1] if rows else {"frame": frame0}
    print(f"frame delta over the {args.pause:.0f} s pause: {int(last['frame']) - int(frame0)}", flush=True)

    final = world.get_settings()
    print(f"settings at end of pause: {final.synchronous_mode} {final.fixed_delta_seconds}", flush=True)

    # Leave the server as we found it (asynchronous), so a later run is clean.
    final.synchronous_mode = False
    final.fixed_delta_seconds = 0.0
    world.apply_settings(final)
    print("restored async", flush=True)
    print(json.dumps(rows), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

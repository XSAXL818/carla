"""Isolated proof of the lost-sync-mode handling in session_runner.

No simulator: a fake world hands out fixed_delta_seconds = 0.33333 for a few
ticks, then reports None (exactly what the engine returns after it drops
synchronous mode), and accepts apply_settings() the way the engine does. The
session's real _loop runs against it, so this exercises the production code
path, not a copy of it.

Expected:
  * the old expression `sim_time += get_settings().fixed_delta_seconds` raises
    TypeError on None  (the review's `state=error`),
  * the new loop keeps running, records sync_mode_lost once, re-asserts the
    settings, records sync_mode_restored, and never leaves `preview`.
"""
from __future__ import annotations

import sys
import time
import types
from pathlib import Path

BACKEND_DIR = Path(r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform\Platform\backend")
sys.path.insert(0, str(BACKEND_DIR))

from app import session_runner as sr  # noqa: E402


class FakeSettings:
    def __init__(self, fixed_delta):
        self.synchronous_mode = fixed_delta is not None
        self.fixed_delta_seconds = fixed_delta


class FakeWorld:
    """Loses synchronous mode after `lose_after` ticks; re-asserting restores it."""

    def __init__(self, requested_delta, lose_after):
        self.requested_delta = requested_delta
        self.delta = requested_delta
        self.lose_after = lose_after
        self.frame = 0
        self.applies = 0
        self.debug = types.SimpleNamespace(draw_polyline=lambda *a, **k: None)

    def tick(self):
        self.frame += 1
        if self.frame == self.lose_after:
            # what the engine does when it gives up on a client:
            # synchronous_mode = false, fixed_delta_seconds = None
            self.delta = None
        return self.frame

    def get_settings(self):
        return FakeSettings(self.delta)

    def apply_settings(self, settings):
        self.applies += 1
        self.delta = (settings.fixed_delta_seconds
                      if getattr(settings, "synchronous_mode", False) else None)


def main() -> int:
    print("== old code: the expression the loop used to run ==", flush=True)
    try:
        value = 1.0
        value += None  # noqa: PLW2901  (sim_time += world.get_settings().fixed_delta_seconds)
    except TypeError as exc:
        print(f"  1.0 + None -> TypeError: {exc}   (this set state=error and broke the loop)", flush=True)

    print("== new code: Session._loop against a world that loses sync mode ==", flush=True)
    session = sr.manager.create({"map": "Town01", "run": {}})
    try:
        session.world = FakeWorld(requested_delta=1.0 / 3.0, lose_after=6)
        session.state = "preview"
        session._target_hz = 3.0
        session._fixed_delta = 1.0 / 3.0
        session._start_loop()
        time.sleep(3.0)
        session._stop_evt.set()
        if session._thread is not None:
            session._thread.join(timeout=2.0)
        status = session.status()
        print(f"state={status['state']} error={status['error']!r} tick={status['tick']} "
              f"sim_time={status['sim_time']} sync_mode={status['sync_mode']} "
              f"fixed_delta={status['fixed_delta']} sync_lost={status['sync_lost']} "
              f"reasserts={status['sync_reasserts']}", flush=True)
        print(f"world.applies={session.world.applies} (re-asserted settings)", flush=True)
        print(f"events={len(session.events)}", flush=True)
        for event in session.events:
            print(f"  t={event['t']:.3f} kind={event['kind']} detail={event['detail']}", flush=True)
        lost = [e for e in session.events if e["kind"] == "sync_mode_lost"]
        restored = [e for e in session.events if e["kind"] == "sync_mode_restored"]
        checks = {
            "loop stayed out of error": status["state"] != "error",
            "sim_time advanced": status["sim_time"] > 0.0,
            "ticks advanced": status["tick"] > 0,
            "loss recorded": len(lost) >= 1,
            "settings re-asserted": session.world.applies >= 1,
            "sync restored after re-assert": len(restored) >= 1 and not status["sync_lost"],
        }
        for name, ok in checks.items():
            print(f"CHECK {'PASS' if ok else 'FAIL'}: {name}", flush=True)
        return 0 if all(checks.values()) else 1
    finally:
        import shutil
        shutil.rmtree(session.output_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

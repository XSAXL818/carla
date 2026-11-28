"""Measure the session's real tick rate, and whether the route overlay costs it.

The overlay draws one draw_line per plan point every ROUTE_DRAW_EVERY_S. The plan
has 263 points on Town10HD's loop preset where the old preset polyline had 18,
so this checks whether the denser overlay is what slows the clock down.
"""
from __future__ import annotations

import http.client
import json
import time

API_HOST, API_PORT = "127.0.0.1", 8765


def api(method, path, body=None, timeout=120.0):
    conn = http.client.HTTPConnection(API_HOST, API_PORT, timeout=timeout)
    try:
        payload = json.dumps(body).encode() if body is not None else None
        headers = {"Content-Type": "application/json"} if payload else {}
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        raw = resp.read().decode()
        if resp.status >= 400:
            raise RuntimeError(f"{method} {path} -> {resp.status}: {raw[:300]}")
        return json.loads(raw)
    finally:
        conn.close()


def rate(sid, seconds, label):
    a = api("GET", "/sessions/current")
    t0, s0, k0 = time.time(), a["sim_time"], a["tick"]
    time.sleep(seconds)
    b = api("GET", "/sessions/current")
    t1, s1, k1 = time.time(), b["sim_time"], b["tick"]
    wall = t1 - t0
    sim = s1 - s0
    ticks = k1 - k0
    print(f"{label:<28} wall {wall:5.1f} s | sim {sim:6.2f} s | ticks {ticks:5d} "
          f"| {ticks / wall:5.2f} tick/s | sim/wall {sim / wall:5.2f} "
          f"| speed {b['ego']['speed_kmh'] if b['ego'] else None} km/h")
    return ticks / wall


def main():
    cur = api("GET", "/sessions/current")
    sid = cur["id"]
    print("session", sid, cur["state"], "route:", cur["route"]["visible"],
          cur["route"]["points"], "plan:", cur["route"]["plan"])
    if cur["state"] != "running":
        print("not running; start it first")
        return

    rate(sid, 15.0, "overlay ON")
    api("POST", f"/sessions/{sid}/route", {"visible": False}, timeout=30)
    time.sleep(6.0)  # let the 5 s line lifetime expire
    rate(sid, 15.0, "overlay OFF")
    api("POST", f"/sessions/{sid}/route", {"visible": True}, timeout=30)
    time.sleep(6.0)
    rate(sid, 15.0, "overlay ON again")


if __name__ == "__main__":
    main()

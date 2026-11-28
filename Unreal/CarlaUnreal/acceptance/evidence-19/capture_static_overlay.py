"""Capture the overlay on/off/on triple while the ego is stationary.

The diff only means anything if the camera does not move between the frames, so
this waits until the ego has been at a standstill for a few seconds (red light or
the parked end of the route), then toggles the overlay and captures.
"""
from __future__ import annotations

import http.client
import json
import subprocess
import sys
import time

import shot

API_HOST, API_PORT = "127.0.0.1", 8765
PY = sys.executable


def api(method, path, body=None, timeout=60.0):
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


def main():
    pid = int(sys.argv[1])
    outdir = sys.argv[2]
    max_wait = float(sys.argv[3]) if len(sys.argv) > 3 else 600.0
    cur = api("GET", "/sessions/current")
    sid = cur["id"]
    print(f"session {sid} state={cur['state']}", flush=True)

    # wait for a standstill of >= 4 s (two consecutive polls)
    stationary = 0
    t0 = time.time()
    while time.time() - t0 < max_wait:
        st = api("GET", "/sessions/current")
        ego = st.get("ego")
        if ego is None:
            time.sleep(1.0)
            continue
        if ego["speed_kmh"] < 0.3:
            stationary += 1
            if stationary >= 2:
                print(f"ego stationary at ({ego['x']:.1f}, {ego['y']:.1f}) "
                      f"sim={st['sim_time']}", flush=True)
                break
        else:
            stationary = 0
        time.sleep(1.2)
    else:
        print("ego never came to a standstill", flush=True)
        return 1

    def snap(tag):
        path = f"{outdir}/{tag}.png"
        hwnd, _t = shot.find_window(pid, None)
        shot.capture(hwnd, path)
        return path

    on = snap("60-route-on")
    api("POST", f"/sessions/{sid}/route", {"visible": False}, timeout=30)
    time.sleep(7.0)  # the drawn lines live 5 s
    off = snap("61-route-off")
    api("POST", f"/sessions/{sid}/route", {"visible": True}, timeout=30)
    time.sleep(7.0)
    again = snap("62-route-on-again")
    print(json.dumps({"on": on, "off": off, "on_again": again}))
    r = subprocess.run([PY, "diff_overlay.py", on, off], capture_output=True, text=True)
    print(r.stdout)
    if r.stderr:
        print(r.stderr, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

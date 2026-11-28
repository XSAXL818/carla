"""Diagnose: which actor is the ego, and does a passive client see it move?

The backend owns the tick (synchronous world), so this client must not tick. It
identifies the ego by matching the backend's own reported position each poll,
and prints every vehicle so a frozen read is obvious.
"""
from __future__ import annotations

import http.client
import json
import math
import time

import carla

API_HOST, API_PORT = "127.0.0.1", 8765


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
    client = carla.Client("127.0.0.1", 2000)
    client.set_timeout(30.0)
    world = client.get_world()
    print("settings:", world.get_settings().synchronous_mode,
          world.get_settings().fixed_delta_seconds)

    cur = api("GET", "/sessions/current")
    sid = cur["id"]
    print("session", sid, cur["state"])
    if cur["state"] != "preview":
        api("POST", f"/sessions/{sid}/preview", timeout=300)
        for _ in range(60):
            time.sleep(2)
            st = api("GET", "/sessions/current")
            if st["state"] in ("preview", "error"):
                break
    st = api("GET", "/sessions/current")
    print("preview ego report:", st.get("ego"))
    print("spawn:", st["route"]["spawn"])

    print("\n--- before start, every vehicle (independent client) ---")
    for a in world.get_actors().filter("vehicle.*"):
        tf = a.get_transform()
        print(f"  id={a.id} {a.type_id:<32} pos=({tf.location.x:8.2f},{tf.location.y:8.2f}) "
              f"yaw={tf.rotation.yaw:7.1f}")

    api("POST", f"/sessions/{sid}/start", timeout=180)
    print("\nstarted; sampling 20 s")
    t0 = time.time()
    while time.time() - t0 < 20.0:
        st = api("GET", "/sessions/current")
        rep = st.get("ego")
        hint = (rep["x"], rep["y"]) if rep else None
        rows = []
        best = None
        for a in world.get_actors().filter("vehicle.*"):
            tf = a.get_transform()
            v = a.get_velocity()
            speed = math.sqrt(v.x ** 2 + v.y ** 2 + v.z ** 2) * 3.6
            rows.append((a.id, tf.location.x, tf.location.y, tf.rotation.yaw, speed))
            if hint is not None:
                d = math.dist((tf.location.x, tf.location.y), hint)
                if best is None or d < best[0]:
                    best = (d, a.id)
        print(f"\n t={time.time() - t0:5.1f} backend ego=({rep['x']:.2f},{rep['y']:.2f}) "
              f"yaw={rep['yaw']:.1f} speed={rep['speed_kmh']:.1f} "
              f"| nearest actor id={best[1] if best else None} d={best[0] if best else None:.2f}")
        for row in rows:
            mark = "  <-- ego?" if best and row[0] == best[1] else ""
            print(f"    id={row[0]} pos=({row[1]:8.2f},{row[2]:8.2f}) yaw={row[3]:7.1f} "
                  f"speed={row[4]:6.2f}{mark}")
        time.sleep(2.0)


if __name__ == "__main__":
    main()

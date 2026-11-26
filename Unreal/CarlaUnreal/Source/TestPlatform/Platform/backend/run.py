"""Start the configuration backend.

    C:\\SoftWare\\MiniConda\\envs\\chat_carla\\python.exe run.py

Environment:
    TESTPLATFORM_PORT   listen port (default 8765)
    TESTPLATFORM_HOST   bind address (default 127.0.0.1)
    CARLA_HOST/PORT     simulator to probe (default 127.0.0.1:2000)
    CARLA_MAPS_DIR      Content/Carla/Maps of the UE project
"""
from __future__ import annotations

import sys
from pathlib import Path

import uvicorn

sys.path.insert(0, str(Path(__file__).resolve().parent))

from app import paths  # noqa: E402

if __name__ == "__main__":
    print(f"backend     http://{paths.API_HOST}:{paths.API_PORT}")
    print(f"carla probe {paths.CARLA_HOST}:{paths.CARLA_PORT}")
    print(f"maps        {paths.MAPS_DIR}")
    uvicorn.run(
        "app.main:app",
        host=paths.API_HOST,
        port=paths.API_PORT,
        reload=False,
        log_level="info",
    )

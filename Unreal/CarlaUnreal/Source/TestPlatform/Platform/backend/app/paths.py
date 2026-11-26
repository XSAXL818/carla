"""Path and environment configuration for the test platform backend."""
from __future__ import annotations

import os
from pathlib import Path

# Source/TestPlatform/Platform/
PROJECT_ROOT = Path(__file__).resolve().parents[2]
BACKEND_DIR = PROJECT_ROOT / "backend"
DATA_DIR = BACKEND_DIR / "data"
ROUTES_DIR = DATA_DIR / "routes"
CACHE_DIR = DATA_DIR / "cache"
PREVIEW_CACHE = CACHE_DIR / "previews"
TOOLS_DIR = PROJECT_ROOT / "tools"
RUNS_DIR = PROJECT_ROOT / "runs"

# CARLA content shipped with the UE project; override with CARLA_MAPS_DIR.
_DEFAULT_MAPS = Path(
    r"C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Content\Carla\Maps"
)
MAPS_DIR = Path(os.environ.get("CARLA_MAPS_DIR", str(_DEFAULT_MAPS)))

# CARLA source tree: the pip wheel ships only the compiled `carla` module, so
# the navigation agents (BehaviorAgent/BasicAgent) are imported from here.
CARLA_ROOT = Path(os.environ.get("CARLA_ROOT", r"C:\GameProject\CarlaUE5\carla"))
PYTHON_API_CARLA = CARLA_ROOT / "PythonAPI" / "carla"

# Large maps stay out of the picker: they take 10-30 s to stream in and break
# the "select a map, see it immediately" experience (confirmed with the user).
EXCLUDED_MAPS = {"Town11", "Town12", "Town13", "EmptyMap"}

# The selection UI offers exactly these (the simulator ships them as *_Opt).
ALLOWED_MAPS = [
    "Town01", "Town02", "Town03", "Town04", "Town05",
    "Town06", "Town07", "Town10HD", "Town_C",
]

CARLA_HOST = os.environ.get("CARLA_HOST", "127.0.0.1")
CARLA_PORT = int(os.environ.get("CARLA_PORT", "2000"))
API_HOST = os.environ.get("TESTPLATFORM_HOST", "127.0.0.1")
API_PORT = int(os.environ.get("TESTPLATFORM_PORT", "8765"))

for _d in (DATA_DIR, ROUTES_DIR, CACHE_DIR, PREVIEW_CACHE, RUNS_DIR):
    _d.mkdir(parents=True, exist_ok=True)


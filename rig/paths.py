"""Where things are, for the rig scripts.

The defaults fit this layout, with an Archipelago source checkout and its venv next to the repo:

    <folder>/
      dusklight-archipelago/   this repo
      ap/                      Archipelago (github.com/ArchipelagoMW/Archipelago), any 0.6.x
      venv/                    a Python environment that can run that checkout

Override any of them with environment variables:

    AP_DIR      the Archipelago checkout
    AP_PYTHON   the Python that runs it (default: venv/ above if it exists, else this Python)
    RIG_WORK    scratch space for generated seeds (default: build/rig in this repo)

Install the apworld into the checkout before running anything that generates seeds:
`python tools/build_apworld.py --install <AP_DIR>/custom_worlds`.
"""
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
AP = Path(os.environ.get("AP_DIR", REPO.parent / "ap"))
_VENV = REPO.parent / "venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
PY = os.environ.get("AP_PYTHON", str(_VENV if _VENV.exists() else sys.executable))
WORK = Path(os.environ.get("RIG_WORK", REPO / "build" / "rig"))

_EXE = ".exe" if os.name == "nt" else ""
GEN_TEST = REPO / "build" / f"ap_gen_test{_EXE}"  # built with the mod (see README, Building)
TLS_TEST = REPO / "build" / f"tls_test{_EXE}"

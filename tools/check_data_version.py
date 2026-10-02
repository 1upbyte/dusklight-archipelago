"""Check that the mod and the apworld fingerprint the logic data identically.

The mod refuses a seed whose data_version differs from its own, so if the two
implementations ever drift apart it would refuse every seed. Run in CI.

    python tools/check_data_version.py <path to ap_gen_test executable>
"""
from __future__ import annotations

import ast
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def world_files() -> tuple[str, ...]:
    """WORLD_FILES out of data.py, read rather than imported: importing the apworld needs
    Archipelago on the path, and reading it compares against the real list either way."""
    tree = ast.parse((ROOT / "apworld" / "tp_dusklight" / "data.py").read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "WORLD_FILES" for t in node.targets):
            return tuple(ast.literal_eval(node.value))
    raise SystemExit("WORLD_FILES not found in data.py")


def python_version() -> int:
    crc = 0
    for name in ("items.yaml", "locations.yaml", "macros.yaml", "settings_list.yaml",
                 *world_files()):
        raw = (ROOT / "generator" / "data" / name).read_bytes()
        crc = zlib.crc32(raw.replace(b"\r", b""), crc)
    return crc


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    expected = python_version()
    # Resolved, and with .exe added on Windows, so `build/ap_gen_test` works everywhere.
    exe = Path(sys.argv[1]).resolve()
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    out = subprocess.run([str(exe), "--data-version"], capture_output=True, text=True,
                         check=True).stdout.strip()
    actual = int(out.split()[-1])
    if actual != expected:
        print(f"MISMATCH: apworld says {expected}, mod says {actual}")
        print("The file list in src/ap/data_version.cpp and data.py::WORLD_FILES disagree.")
        return 1
    print(f"data_version matches: {expected}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

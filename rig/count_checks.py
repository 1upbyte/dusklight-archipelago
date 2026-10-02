"""Generate single-player seeds from option overrides and report how many checks each has.

    python rig/count_checks.py              # the built-in probes
    python rig/count_checks.py presets/X.yaml ...

A "check" is a real Archipelago location for the Dusklight slot (slot_data location_ids).
Uses the apworld installed in ap/custom_worlds, so install the current one first.
"""
import glob
import io
import os
import pickle
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib

import yaml

import paths

AP = str(paths.AP)
PY = paths.PY
BASE = str(paths.REPO / "presets" / "Easy.yaml")
# Separate work dirs let a sweep and a preset measurement run side by side.
WORK = os.environ.get("COUNT_WORK", str(paths.WORK / "count"))
GAME = "Twilight Princess (Dusklight)"
sys.path.insert(0, AP)
os.environ["SKIP_REQUIREMENTS_UPDATE"] = "1"

EXTRAS_OFF = dict(golden_bugs=False, sky_characters=False, gifts_from_npcs=False,
                  shop_items=False, hidden_skills=False, hidden_rupees=False,
                  freestanding_rupees=False, poe_souls="vanilla")
EXTRAS_ON = dict(golden_bugs=True, sky_characters=True, gifts_from_npcs=True,
                 shop_items=True, hidden_skills=True, hidden_rupees=True,
                 freestanding_rupees=True, poe_souls="all")
NO_SKIPS = dict(skip_prologue=False, skip_midnas_desperate_hour=False,
                faron_twilight_cleared=False, eldin_twilight_cleared=False,
                lanayru_twilight_cleared=False)

PROBES = {
    "floor (all vanilla, skips on)": dict(EXTRAS_OFF, small_keys="vanilla", big_keys="vanilla",
                                          maps_and_compasses="vanilla"),
    "floor, no skips": {**EXTRAS_OFF, **NO_SKIPS, "small_keys": "vanilla",
                        "big_keys": "vanilla", "maps_and_compasses": "vanilla"},
    "current Easy": {},
    "ceiling (everything on)": {**EXTRAS_ON, **NO_SKIPS, "small_keys": "anywhere",
                                "big_keys": "anywhere", "maps_and_compasses": "anywhere"},
}


def count(options: dict, label: str, seed: int = 1) -> int | str:
    doc = yaml.safe_load(io.open(BASE, encoding="utf-8"))
    doc[GAME].update(options)
    return count_doc(doc, seed)


def count_doc(doc: dict, seed: int = 1) -> int | str:
    """Checks for a whole YAML document, exactly as written.

    Every run gets its own fresh directory: clearing a shared one with rmtree fails quietly on
    Windows whenever something still holds the last zip open (a handle, antivirus), and the
    next run then read the previous run's output. The spoiler's seed is checked as well, so a
    stale result can't slip through again. The zip's path is left in count_doc.last_zip.
    """
    os.makedirs(WORK, exist_ok=True)
    run_dir = tempfile.mkdtemp(prefix=f"seed{seed}_", dir=WORK)
    players, out = os.path.join(run_dir, "players"), os.path.join(run_dir, "out")
    os.makedirs(players)
    doc = dict(doc, name="Probe")
    with io.open(os.path.join(players, "p.yaml"), "w", encoding="utf-8") as f:
        yaml.safe_dump(doc, f, sort_keys=False)
    p = subprocess.run([PY, "Generate.py", "--player_files_path", players,
                        "--outputpath", out, "--seed", str(seed)],
                       cwd=AP, capture_output=True, text=True, input=chr(10), timeout=1800)
    if "Done. Enjoy" not in p.stdout:
        return "FAILED: " + (p.stdout + p.stderr).strip().splitlines()[-1][:120]
    zips = glob.glob(os.path.join(out, "*.zip"))
    if len(zips) != 1:
        return f"FAILED: expected one zip in {out}, found {len(zips)}"
    with zipfile.ZipFile(zips[0]) as z:
        spoiler = z.read([n for n in z.namelist() if n.endswith("Spoiler.txt")][0]).decode("utf-8-sig")
        raw = z.read([n for n in z.namelist() if n.endswith(".archipelago")][0])
    if f"Seed: {seed}" not in spoiler.splitlines()[0]:
        return f"FAILED: output is not from seed {seed}: {spoiler.splitlines()[0]}"
    names = list(pickle.loads(zlib.decompress(raw[1:]))["slot_data"][1]["location_ids"])
    count.last_names = names
    count_doc.last_zip = zips[0]
    count_doc.last_spoiler = spoiler
    return len(names)


def main() -> None:
    if len(sys.argv) > 1:
        # Which dungeons stay vanilla is random per seed, so report a spread, not one number.
        for path in sys.argv[1:]:
            doc = yaml.safe_load(io.open(path, encoding="utf-8"))
            results = [count_doc(doc, seed) for seed in range(101, 109)]
            nums = [r for r in results if isinstance(r, int)]
            errors = [r for r in results if not isinstance(r, int)]
            summary = (f"avg {sum(nums) / len(nums):.0f}  min {min(nums)}  max {max(nums)}"
                       if nums else "")
            print(f"{os.path.basename(path):40} {summary}  {errors[0] if errors else ''}", flush=True)
        return
    for label, opts in PROBES.items():
        print(f"{label:40} {count(opts, label)}", flush=True)


if __name__ == "__main__":
    main()

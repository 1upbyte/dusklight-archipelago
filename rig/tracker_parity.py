"""Does the in-game tracker agree with the apworld about what's in logic?

Generates a preset in-process, replays its playthrough sphere by sphere, and at several points
asks both sides which checks are reachable with what's been collected so far: the apworld's
own rules (CollectionState), and the mod's tracker logic (src/ap/ap_tracker.cpp, through
ap_gen_test --tracker). The two sets must match.

    python rig/tracker_parity.py presets/X.yaml [seed ...] [--players N]

With --players N the preset is generated N times into one multiworld, and player 1's items
found in other worlds reach the tracker as received items, the way the server delivers them.
"""
import io
import json
import os
import subprocess
import sys
import tempfile

import yaml

import paths

AP = str(paths.AP)
GEN_TEST = str(paths.GEN_TEST)
WORK = os.environ.get("PARITY_WORK", str(paths.WORK / "parity"))
GAME = "Twilight Princess (Dusklight)"
ITEM_ID_BASE = 0x54500000

START = os.getcwd()  # preset paths are relative to where we were run from
os.chdir(AP)
sys.path.insert(0, AP)
os.environ["SKIP_REQUIREMENTS_UPDATE"] = "1"
import ModuleUpdate  # noqa: E402

ModuleUpdate.update_ran = True
import Generate  # noqa: E402
import Main  # noqa: E402
from BaseClasses import CollectionState  # noqa: E402


def generate(preset: str, seed: int, run_dir: str, n_players: int = 1):
    players = os.path.join(run_dir, "players")
    os.makedirs(players)
    for n in range(1, n_players + 1):
        doc = yaml.safe_load(io.open(preset, encoding="utf-8"))
        doc["name"] = f"Probe{n}"
        with io.open(os.path.join(players, f"p{n}.yaml"), "w", encoding="utf-8") as f:
            yaml.safe_dump(doc, f, sort_keys=False)
    args = Generate.mystery_argparse(["--player_files_path", players, "--outputpath", run_dir,
                                      "--seed", str(seed), "--skip_output"])
    erargs, s = Generate.main(args)
    return Main.main(erargs, s)


def stage_indices(n: int) -> list[int]:
    return sorted({0, 1, 2, n // 4, n // 2, (3 * n) // 4, max(n - 1, 0)})


def run(preset: str, seed: int, n_players: int = 1) -> int:
    os.makedirs(WORK, exist_ok=True)
    run_dir = tempfile.mkdtemp(prefix=f"parity{seed}_", dir=WORK)
    mw = generate(preset, seed, run_dir, n_players)
    world = mw.worlds[1]
    slot_data = world.fill_slot_data()
    sd_path = os.path.join(run_dir, "slot_data.json")
    json.dump(slot_data, open(sd_path, "w"))

    spheres = list(mw.get_spheres())
    events = [loc for loc in mw.get_locations(1) if loc.address is None]
    real = [loc for loc in mw.get_locations(1) if loc.address is not None]
    start = [it.code - ITEM_ID_BASE for it in mw.precollected_items[1] if it.code is not None]

    stages, expected = [], []
    for k in stage_indices(len(spheres)):
        state = CollectionState(mw)
        checked, received = set(), list(start)
        for sphere in spheres[:k]:
            for loc in sphere:
                # Checks only: events come from the sweep below, once (collecting them here too
                # would count King Bulblin's key twice).
                if loc.address is None:
                    continue
                if loc.item is not None:
                    state.collect(loc.item, True, loc)
                if loc.player == 1:
                    checked.add(loc.name)
                elif loc.item is not None and loc.item.player == 1:
                    received.append(loc.item.code - ITEM_ID_BASE)
        state.sweep_for_advancements(locations=events)
        expected.append({loc.name for loc in real if loc.name not in checked and loc.can_reach(state)})
        stages.append({"k": k, "received": received, "checked": sorted(checked)})

    q_path, out_path = os.path.join(run_dir, "stages.json"), os.path.join(run_dir, "out.json")
    json.dump({"stages": stages}, open(q_path, "w"))
    p = subprocess.run([GEN_TEST, sd_path, os.path.join(run_dir, "gen"), "--tracker", q_path, out_path],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(f"FAIL  {os.path.basename(preset):22} seed={seed}  ap_gen_test: {p.stdout.strip()[-300:]}")
        return 1
    out = json.load(open(out_path))
    bad = 0
    for st, exp, res in zip(stages, expected, out["stages"]):
        got = set(res["reachable"]) - set(st["checked"])
        extra, missing = sorted(got - exp), sorted(exp - got)
        ok = not extra and not missing
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {os.path.basename(preset):22} x{n_players} seed={seed} sphere {st['k']:>2}/"
              f"{len(spheres)}  checked {len(st['checked']):>3}  received {len(st['received']):>3}  "
              f"in logic {len(exp):>3}  "
              f"tracker {len(got):>3}  {res['ms']:.0f} ms", flush=True)
        for name in extra[:5]:
            print(f"        tracker only: {name}")
        for name in missing[:5]:
            print(f"        apworld only: {name}")
    print(f"      tracker build {out['build_ms']:.0f} ms")
    return bad


def main() -> int:
    args = sys.argv[1:]
    n_players = 1
    if "--players" in args:
        i = args.index("--players")
        n_players = int(args[i + 1])
        del args[i:i + 2]
    preset = os.path.join(START, args[0])
    seeds = [int(a) for a in args[1:]] or [1]
    return 1 if sum(run(preset, s, n_players) for s in seeds) else 0


if __name__ == "__main__":
    sys.exit(main())

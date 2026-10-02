"""Generate seeds and rebuild each one with ap_gen_test exactly as the mod does, checking that
nothing differs.

    python rig/sweep_dungeons.py                  # dungeon sweep
    python rig/sweep_dungeons.py presets/*.yaml   # each preset, several seeds

A pass is "placements ok N bad 0 empty locations 0" from ap_gen_test: every placement AP made
(checks and unshuffled-dungeon contents) came out the same in the in-game generator.
"""
import io
import itertools
import json
import os
import pickle
import sys
import subprocess
import zipfile
import zlib

import yaml

sys.path.insert(0, os.path.dirname(__file__))
import count_checks as cc  # noqa: E402

GEN = str(cc.paths.GEN_TEST)

COUNTS = (0, 1, 3, 6, 9)
KEYS = ("own_dungeon", "anywhere", "vanilla", "keysy")
REWARDS = (False, True)
# Plentiful adds a spare of every key, which is what broke own-dungeon placement once.
SCARCITY = ("plentiful", "minimal")
PRESET_SEEDS = (201, 202, 203, 204)


def rebuild(doc: dict, seed: int) -> tuple[str, int | None]:
    checks = cc.count_doc(doc, seed)
    if isinstance(checks, str):
        return checks, None
    with zipfile.ZipFile(cc.count_doc.last_zip) as z:
        raw = z.read([n for n in z.namelist() if n.endswith(".archipelago")][0])
    sd = pickle.loads(zlib.decompress(raw[1:]))["slot_data"][1]
    run_dir = os.path.dirname(os.path.dirname(cc.count_doc.last_zip))
    sd_path = os.path.join(run_dir, "slot.json")
    io.open(sd_path, "w", encoding="utf-8").write(json.dumps(sd))
    r = subprocess.run([GEN, sd_path, os.path.join(run_dir, "rebuild")], capture_output=True,
                       text=True, timeout=900)
    tail = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:]
    return tail, checks


def passed(tail: str, checks) -> bool:
    return checks is not None and " bad 0 " in f" {tail} " and "empty locations 0" in tail


def sweep() -> int:
    failures = 0
    base = yaml.safe_load(io.open(cc.BASE, encoding="utf-8"))
    combos = itertools.product(COUNTS, KEYS, REWARDS, SCARCITY)
    for i, (n, keys, rewards, scarcity) in enumerate(combos):
        seed = 1000 + i  # a different dungeon pick for every combination
        doc = json.loads(json.dumps(base))
        doc[cc.GAME].update({**cc.EXTRAS_OFF, "shuffled_dungeons": n, "small_keys": keys,
                             "big_keys": keys,
                             "maps_and_compasses": "own_dungeon" if keys == "keysy" else keys,
                             "dungeon_rewards_can_be_anywhere": rewards,
                             "item_scarcity": scarcity})
        tail, checks = rebuild(doc, seed)
        ok = passed(tail, checks)
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  dungeons={n} keys={keys:<11} rewards_anywhere={rewards!s:<5} "
              f"{scarcity:<9} seed={seed}  checks={checks}  | {tail}", flush=True)
    return failures


def presets(paths: list[str]) -> int:
    failures = 0
    for path in paths:
        doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        for seed in PRESET_SEEDS:
            tail, checks = rebuild(doc, seed)
            ok = passed(tail, checks)
            failures += not ok
            print(f"{'PASS' if ok else 'FAIL'}  {os.path.basename(path):24} seed={seed}  "
                  f"checks={checks}  | {tail}", flush=True)
    return failures


def main() -> int:
    failures = presets(sys.argv[1:]) if len(sys.argv) > 1 else sweep()
    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

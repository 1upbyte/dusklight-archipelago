"""Does excluding Hyrule Castle keep important items out of it? Generates a preset (and the same
preset with exclude_locations removed, as a control: since 0.6.0 the default excludes nothing, so
the control should put important items there), then reads every Hyrule Castle check's item
classification straight from the multidata.

    python rig/check_exclusion.py presets/X.yaml ...
"""
import io
import os
import pickle
import sys
import zipfile
import zlib

import yaml

sys.path.insert(0, os.path.dirname(__file__))
import count_checks as cc  # noqa: E402

PROGRESSION, USEFUL = 0b001, 0b010


def hc_items(doc: dict, seed: int) -> tuple[int, int, str]:
    """(Hyrule Castle checks in the seed, how many hold an important item, unshuffled list)."""
    result = cc.count_doc(doc, seed)
    if isinstance(result, str):
        raise SystemExit(result)
    with zipfile.ZipFile(cc.count_doc.last_zip) as z:
        raw = z.read([n for n in z.namelist() if n.endswith(".archipelago")][0])
    md = pickle.loads(zlib.decompress(raw[1:]))
    ids = md["slot_data"][1]["location_ids"]
    hc = {i for name, i in ids.items() if name.startswith("Hyrule Castle")}
    important = sum(1 for loc_id, (_item, _owner, flags) in md["locations"][1].items()
                    if loc_id in hc and flags & (PROGRESSION | USEFUL))
    unshuffled = next((l for l in cc.count_doc.last_spoiler.splitlines()
                       if l.startswith("Unshuffled dungeons")), "all shuffled")
    return len(hc), important, unshuffled


def main() -> int:
    bad = 0
    for path in sys.argv[1:]:
        doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        default_doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        default_doc[cc.GAME].pop("exclude_locations", None)
        for label, d in (("explicit", doc), ("control", default_doc)):
            for seed in (301, 302):
                n, important, unshuffled = hc_items(d, seed)
                ok = important == 0 if label == "explicit" else True
                bad += not ok
                note = "HC unshuffled" if "Hyrule Castle" in unshuffled else f"{n} HC checks"
                verdict = ("PASS" if ok else "FAIL") if label == "explicit" else "info"
                print(f"{verdict:4}  {os.path.basename(path):22} {label:8} seed={seed}  "
                      f"{note:16} important items in HC: {important}", flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

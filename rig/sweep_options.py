"""Generate a seed for every value of every exposed option; report failures.

usage: python sweep_options.py [out.txt]
"""
import io, os, subprocess, sys, time

import paths

AP = str(paths.AP)
PY = paths.PY
PLAYERS = str(paths.WORK / "sweep_players")
OUT = str(paths.WORK / "sweep_out")
sys.path.insert(0, AP)
os.environ["SKIP_REQUIREMENTS_UPDATE"] = "1"

from worlds.tp_dusklight import data  # noqa: E402
from worlds.tp_dusklight.options import FORCED, option_key  # noqa: E402

cases = []
for info in data.settings().values():
    if info.name in FORCED:
        continue
    key = option_key(info.name)
    if info.numeric:
        values = [info.options[0], info.default, info.options[-1]]
    elif info.options == ["Off", "On"]:
        values = ["false", "true"]
    else:
        values = [option_key(o) for o in info.options]
    for v in dict.fromkeys(values):
        cases.append((key, v))

os.makedirs(PLAYERS, exist_ok=True)
results = []
start = time.time()
for i, (key, value) in enumerate(cases, 1):
    for f in os.listdir(PLAYERS):
        os.remove(os.path.join(PLAYERS, f))
    yaml = ("name: Sweep\ngame: Twilight Princess (Dusklight)\n"
            "Twilight Princess (Dusklight):\n  %s: %s\n" % (key, value))
    io.open(os.path.join(PLAYERS, "sweep.yaml"), "w", encoding="utf-8").write(yaml)
    p = subprocess.run([PY, "Generate.py", "--player_files_path", PLAYERS, "--outputpath", OUT,
                        "--seed", "1234"], cwd=AP, capture_output=True, text=True, input="\n",
                       timeout=900)
    ok = p.returncode == 0 and "Done. Enjoy." in p.stdout
    tail = ""
    if not ok:
        lines = [l for l in (p.stdout + p.stderr).splitlines() if l.strip()]
        tail = " | ".join(lines[-4:])[:500]
    results.append((key, value, ok, tail))
    print(f"[{i}/{len(cases)}] {key}={value}: {'OK' if ok else 'FAIL ' + tail}", flush=True)

fails = [r for r in results if not r[2]]
print(f"\n{len(results) - len(fails)}/{len(results)} passed in {time.time() - start:.0f}s")
for key, value, _, tail in fails:
    print(f"FAIL {key}={value}: {tail}")

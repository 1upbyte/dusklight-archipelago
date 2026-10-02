"""Why can't pre_fill place a group? For a preset + seed, report which goal locations are
reachable with every item in the game, which key progression items survived into the pool,
and how many regions are unreachable.

    python rig/debug_prefill.py <preset.yaml> <seed>
"""
import io
import os
import sys
import tempfile
from collections import Counter

import yaml

import paths

AP = str(paths.AP)
sys.path.insert(0, AP)
os.chdir(AP)
os.environ["SKIP_REQUIREMENTS_UPDATE"] = "1"

import Utils  # noqa: E402

Utils.init_logging("DebugPrefill", exception_logger="Generator")
import worlds  # noqa: E402,F401
from BaseClasses import CollectionState  # noqa: E402
from worlds.AutoWorld import AutoWorldRegister  # noqa: E402

GAME = "Twilight Princess (Dusklight)"
TP = AutoWorldRegister.world_types[GAME]
original = TP.pre_fill

KEY_ITEMS = ("Shadow Crystal", "Progressive Clawshot", "Progressive Sword", "Lantern",
             "Iron Boots", "Gale Boomerang", "Zora Armor", "Progressive Bow", "Bomb Bag",
             "Spinner", "Ball and Chain", "Progressive Dominion Rod", "Progressive Sky Book",
             "Progressive Fishing Rod", "Slingshot", "Aurus Memo", "Asheis Sketch",
             "Progressive Mirror Shard", "Progressive Fused Shadow")


def patched(self):
    mw, p = self.multiworld, self.player
    print("unshuffled:", sorted(self._vanilla_dungeons))
    full = CollectionState(mw)
    for item in mw.itempool:
        if item.player == p:
            full.collect(item, True)
    full.sweep_for_advancements(locations=self.get_locations())

    pool = Counter(i.name for i in mw.itempool if i.player == p)
    locked = Counter(l.item.name for l in self.get_locations() if l.item and l.address is None)
    print(f"real locations {len(self._real_location_names)}, pool items {sum(pool.values())}")
    for key in KEY_ITEMS:
        print(f"  {key:26} pool {pool.get(key, 0)}  locked {locked.get(key, 0)}")

    goals = [l for l in self.get_locations() if l.loc_data and l.loc_data.goal]
    print("goal locations reachable with every item:")
    for l in sorted(goals, key=lambda l: l.name):
        state = f"locked: {l.item.name}" if l.item else "open"
        print(f"  {'yes' if l.can_reach(full) else 'NO '}  {l.name:45} {state}")
    unreachable = [r.name for r in self.get_regions() if not r.can_reach(full)]
    print(f"regions unreachable with every item: {len(unreachable)} of {len(self.get_regions())}")
    return original(self)


TP.pre_fill = patched

original_basic = TP.generate_basic


def patched_basic(self):
    mw, p = self.multiworld, self.player
    swept = CollectionState(mw)
    for item in mw.itempool:
        if item.player == p:
            swept.collect(item, True)
    swept.sweep_for_advancements(locations=self.get_locations())
    given = swept.copy()
    for l in self.get_locations():
        if l.item and l.item.player == p and l.address is None:
            given.collect(l.item, True)
    n = len(self.get_regions())
    ur_swept = [r for r in self.get_regions() if not r.can_reach(swept)]
    ur_given = [r for r in self.get_regions() if not r.can_reach(given)]
    print(f"before pruning: unreachable when swept {len(ur_swept)}/{n}, "
          f"when every locked item is simply given {len(ur_given)}/{n}")
    # Which locked items the sweep never reached, that giving outright would fix:
    missing = sorted({l.item.name for l in self.get_locations()
                      if l.item and l.address is None and l.item.player == p
                      and l.item.advancement and not l.can_reach(swept)})
    # Hand the sweep one missing item at a time: the one that unlocks the most is the lock.
    base = n - len(ur_swept)
    gains = []
    seen = set()
    for l in self.get_locations():
        it = l.item
        if not (it and l.address is None and it.player == p and it.advancement
                and not l.can_reach(swept)) or it.name in seen:
            continue
        seen.add(it.name)
        trial = swept.copy()
        trial.collect(it, True)
        trial.sweep_for_advancements(locations=self.get_locations())
        gains.append((sum(1 for r in self.get_regions() if r.can_reach(trial)) - base, it.name, l.name))
    gains.sort(reverse=True)
    ft = [l for l in self.get_locations() if "Forest Temple" in l.name or "Faron Woods" in l.name
          or "Coro" in l.name]
    print("Forest Temple / Faron locked locations, reachable when swept?")
    for l in sorted(ft, key=lambda l: l.name):
        if l.item and l.address is None and l.item.player == p and "Can " not in l.name:
            print(f"  {'yes' if l.can_reach(swept) else 'NO '}  {l.name:50} -> {l.item.name}")
    print("single items that unlock the most, and where each is locked:")
    for g, item, where in gains[:8]:
        print(f"  +{g:5} regions  {item:40} at {where}")
    before = {l.name: l.item.name for l in self.get_locations()
              if l.address is None and l.item and l.item.name in self._vanilla_locked.values()
              and l.name in self._vanilla_locked}
    original_basic(self)
    after = {l.name for l in self.get_locations()}
    gone = {name: item for name, item in before.items() if name not in after}
    print(f"generate_basic removed {len(gone)} vanilla-locked locations carrying real items:")
    for name, item in sorted(gone.items())[:25]:
        tag = "dungeon" if name in self._dungeon_locked else "setting"
        print(f"  [{tag}] {name}  ->  {item}")


TP.generate_basic = patched_basic

if "--no-held-keys" in sys.argv:
    # Recreate the unshuffled-dungeon deadlock, to check generate_basic reports it.
    original_regions = TP.create_regions

    def without_held_keys(self):
        original_regions(self)
        menu = self.multiworld.get_region("Menu", self.player)
        menu.locations = [l for l in menu.locations if not l.name.startswith("Unshuffled: ")]

    TP.create_regions = without_held_keys
    sys.argv.remove("--no-held-keys")

if __name__ == "__main__":
    preset, seed = sys.argv[1], sys.argv[2]
    doc = yaml.safe_load(io.open(preset, encoding="utf-8"))
    run = tempfile.mkdtemp(prefix="prefill_")
    os.makedirs(os.path.join(run, "players"))
    with open(os.path.join(run, "players", "p.yaml"), "w", encoding="utf-8") as f:
        yaml.safe_dump(dict(doc, name="Probe"), f, sort_keys=False)
    sys.argv = ["Generate.py", "--player_files_path", os.path.join(run, "players"),
                "--outputpath", os.path.join(run, "out"), "--seed", seed]
    # Imported as a module, not run as __main__: its "worlds loaded before logging" guard
    # only applies to __main__, and this script had to load worlds to patch them.
    import Generate
    from Main import main as generate_multiworld
    try:
        erargs, s = Generate.main()
        generate_multiworld(erargs, s)
    except BaseException as e:  # noqa: BLE001 - we want the report, then the failure
        print("generation failed:", type(e).__name__, str(e).splitlines()[0][:200])

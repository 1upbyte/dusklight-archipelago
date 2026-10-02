import sys, os, time
import paths
os.chdir(paths.AP); sys.path.insert(0, ".")
os.environ["SKIP_REQUIREMENTS_UPDATE"]="1"
import ModuleUpdate; ModuleUpdate.update_ran=True
from worlds.AutoWorld import call_all, AutoWorldRegister
from BaseClasses import MultiWorld, CollectionState
from Generate import get_seed_name
import Options
from argparse import Namespace
W = AutoWorldRegister.world_types["Twilight Princess (Dusklight)"]
def build(opts=None):
    mw = MultiWorld(1); mw.game[1]=W.game; mw.player_name={1:"P"}; mw.set_seed(0)
    args = Namespace()
    for name, option in W.options_dataclass.type_hints.items():
        v = (opts or {}).get(name, option.default)
        setattr(args, name, {1: option.from_any(v)})
    mw.set_options(args)
    mw.state = CollectionState(mw)
    t=time.time()
    for step in ("generate_early","create_regions","create_items","set_rules"):
        call_all(mw, step)
    print("gen steps", round(time.time()-t,2), "regions", len(mw.regions), "entrances", sum(len(r.exits) for r in mw.regions))
    return mw
if __name__ == "__main__":
    mw = build()
    t=time.time()
    st = mw.get_all_state(False)
    print("all_state", round(time.time()-t,2))
    locs=[l for l in mw.get_locations() if l.address is not None]
    un=[l.name for l in locs if not l.can_reach(st)]
    print(len(locs), "real locs; unreachable with all items:", len(un)); print(un[:40])
    evs=[l for l in mw.get_locations() if l.address is None]
    print("events unreachable:", [l.name for l in evs if not l.can_reach(st)][:40])
    print("beatable:", st.has("Game Beatable",1))

def frontier(mw, st):
    from worlds.tp_dusklight import data
    reach = {r.name for r in mw.regions if r.can_reach(st)}
    hubs = [a for a in data.areas() if a in reach]
    print("reachable hubs", len(hubs), hubs[:60])
    for r in mw.regions:
        if r.name in reach:
            for e in r.exits:
                if e.connected_region.name not in reach and "(" in e.connected_region.name:
                    print("BLOCKED", e.name)

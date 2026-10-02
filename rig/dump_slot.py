import sys, os, zipfile, zlib, pickle, json, glob
import paths
os.chdir(paths.AP); sys.path.insert(0, ".")
import NetUtils
z = sorted(glob.glob("../test_out/*.zip"), key=os.path.getmtime)[-1]
with zipfile.ZipFile(z) as zf:
    name = [n for n in zf.namelist() if n.endswith(".archipelago")][0]
    raw = zf.read(name)
d = pickle.loads(zlib.decompress(raw[1:]))
sd = d["slot_data"][int(sys.argv[1]) if len(sys.argv)>1 else 1]
json.dump(sd, open("../slot_data.json","w"), indent=1)
print(z, len(sd["placements"]), sd["seed"], sum(1 for v in sd["placements"].values() if isinstance(v,dict)))

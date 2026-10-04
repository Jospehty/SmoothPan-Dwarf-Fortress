#!/usr/bin/env python3
"""Detail of joined intended-vs-shown timing: offset of the shown timestamp
from the intended refresh (us) for hits, and when misses happen.
Usage: joindetail.py <pace_label> <vblmsc_output_file>"""
import sys, statistics
F = "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/smoothpan_pace.txt"
label, vf = sys.argv[1], sys.argv[2]
rows, on = [], False
for line in open(F, errors="replace"):
    if line.startswith("# block label="):
        on = line.split("label=")[1].split()[0] == label
        if on: rows = []
    elif on and line.startswith("start=") and line.rstrip().split()[-1].startswith("scale="):
        rows.append({k: float(v) for k, v in (kv.split("=") for kv in line.split())})
seen = {}
for l in open(vf):
    d = dict(kv.split("=") for kv in l.split())
    seen[int(d["msc"])] = int(d["ust"])
shown = [seen[k] for k in sorted(seen)]
P = rows[-1]["P"]
hits, misses = [], []
j = 0
for u in shown:
    while j + 1 < len(rows) and rows[j + 1]["done"] < u: j += 1
    r = rows[j]
    if r["done"] >= u or not r["target"]: continue
    off = u - r["target"]
    k = round(off / P)
    (hits if k == 0 else misses).append((u, off - k * P, r))
ho = [h[1] for h in hits]
print(f"hits {len(hits)}: shown-target offset us  min {min(ho):.0f}  median {statistics.median(ho):.0f}  max {max(ho):.0f}")
mo = [m[1] for m in misses]
if mo: print(f"misses {len(misses)}: residual after +1T  min {min(mo):.0f} median {statistics.median(mo):.0f} max {max(mo):.0f}")
t0 = shown[0]
print("miss times (ms from start):", [round((m[0] - t0) / 1000) for m in misses])
print("miss render ms (start->done):", [round((m[2]['done'] - m[2]['start']) / 1000, 2) for m in misses])
print("miss present phase vs grid (us before target):", [round(m[2]['target'] - m[2]['done']) for m in misses])

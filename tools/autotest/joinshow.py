#!/usr/bin/env python3
"""Join what SmoothPan intended (smoothpan_pace.txt: target refresh, present
done time per frame) with what the compositor actually did (XWayland Present
UST per displayed frame, from vblmsc output), on the shared CLOCK_MONOTONIC.

For each displayed frame, find the newest SmoothPan frame presented before it:
  shown - target  = 0      -> shown on the intended refresh
                  = +1 T   -> missed its refresh (compositor did not take it)
  done  - target  = how early we presented it (our latch margin in practice)
Usage: joinshow.py <pace_label> <vblmsc_output_file>"""
import sys
from collections import Counter
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
res = Counter(); margins_ok, margins_miss = [], []
j = 0
detail = []
for u in shown:
    while j + 1 < len(rows) and rows[j + 1]["done"] < u: j += 1
    r = rows[j]
    if r["done"] >= u or not r["target"]:
        continue
    k = round((u - r["target"]) / P)
    res[k] += 1
    m = (r["target"] - r["done"]) / 1000.0
    (margins_ok if k == 0 else margins_miss).append(m)
    detail.append((k, m, (r["done"] - r["start"]) / 1000.0))
tot = sum(res.values())
print(f"displayed frames matched: {tot}")
print("shown vs intended refresh: " + "  ".join(f"{k:+d}T:{res[k]}" for k in sorted(res)))
def st(v): return f"min {min(v):.2f} med {sorted(v)[len(v)//2]:.2f} max {max(v):.2f} ms" if v else "-"
print(f"margin (target - present done) when ON time : {st(margins_ok)}")
print(f"margin (target - present done) when MISSED  : {st(margins_miss)}")

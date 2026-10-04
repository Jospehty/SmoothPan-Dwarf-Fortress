#!/usr/bin/env python3
"""Left-edge behaviour during a pan: where the map's first visible pixel sits
on screen (vp.left + black run), DF's viewport offset, and the pan offset, per
frame.  Jumps in 'edge' between consecutive frames are judder at the edge.
Usage: leftedge.py <watch_label> [--frames]"""
import sys
from collections import Counter
W = "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/smoothpan_watch.txt"
label = sys.argv[1]
rows, on = [], False
for line in open(W, errors="replace"):
    if line.startswith("# block label="):
        on = line.split("label=")[1].split()[0] == label
        if on: rows = []
    elif on and line.startswith("f=") and "vpx=" in line:
        rows.append(dict(kv.split("=", 1) for kv in line.split()))
edges = []
for r in rows:
    vpx = int(r["vpx"]); L = min(int(x) for x in r["L"].split(","))
    edges.append((max(0, vpx) + L, vpx, L, float(r["cx"]), r))
mov = [i for i in range(1, len(edges)) if abs(edges[i][3] - edges[i-1][3]) > 1e-4]
jumps = [abs(edges[i][0] - edges[i-1][0]) for i in mov]
print(f"{len(rows)} frames, {len(mov)} moving")
print("left edge x (map start) values while moving: " + " ".join(f"{k}:{v}" for k, v in sorted(Counter(edges[i][0] for i in mov).items())))
print("frame-to-frame edge jumps while moving: " + " ".join(f"{k}px:{v}" for k, v in sorted(Counter(jumps).items())))
print("vpx values while moving: " + " ".join(f"{k}:{v}" for k, v in sorted(Counter(edges[i][1] for i in mov).items())))
if "--frames" in sys.argv:
    for i in mov[:60]:
        e = edges[i]
        print(f"f={e[4]['f']} edge={e[0]} vpx={e[1]} L={e[2]} cx={e[3]:.4f} frac={e[3]%1:.3f}")

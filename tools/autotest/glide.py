#!/usr/bin/env python3
"""Per-frame glide trace for gesture frames of a watch block: displayed tile
size (scale x cell), its change, and markers for holds / jumps / reversals /
bake changes.  Usage: glide.py <label>"""
import subprocess, sys
import os as _os
HERE = _os.path.dirname(_os.path.abspath(__file__))
label = sys.argv[1]
out = subprocess.run(["python3", HERE + "/watchstats.py", label, "--frames"],
                     capture_output=True, text=True).stdout
rows = []
for line in out.splitlines():
    if line.startswith("f="):
        rows.append(dict(kv.split("=", 1) for kv in line.split()))
prev = None
prev_d = 0.0
for r in rows:
    vis = float(r["scale"]) * float(r["cell"])
    if prev is not None and r["gest"] == "1" and prev[1]["gest"] == "1":
        d = vis - prev[0]
        tags = []
        if abs(d) < 1e-4: tags.append("HOLD")
        if prev_d and d and (d > 0) != (prev_d > 0): tags.append("REV")
        if r["cell"] != prev[1]["cell"]: tags.append(f"BAKE {prev[1]['cell']}->{r['cell']}")
        print(f"f={r['f']:>5} ms={r['ms']:>5} cell={r['cell']:>2} scale={r['scale']} vis={vis:7.3f} d={d:+7.3f} v={r['v']:>6} {' '.join(tags)}")
        if abs(d) >= 1e-4: prev_d = d
    prev = (vis, r)

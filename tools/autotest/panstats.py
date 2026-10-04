#!/usr/bin/env python3
"""Pan smoothness + edge gaps for a watch block.
Displayed camera position cx,cy (tiles) per frame -> screen speed in px/ms.
  reversals  direction flips while moving (judder)
  stalls     frames with no movement between moving frames (stutter)
  spikes     frames moving > 2.5x the local median speed (pops)
  edges      max black run in from each viewport edge while moving (px)
Usage: panstats.py <label> [--frames]"""
import sys
W = "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/smoothpan_watch.txt"
label = sys.argv[1]; show = "--frames" in sys.argv
blocks, cur = [], None
for line in open(W, errors="replace"):
    if line.startswith("# block label="):
        cur = {"label": line.split("label=")[1].split()[0], "rows": []}
        blocks.append(cur)
    elif cur is not None and line.startswith("f=") and "cx=" in line:
        cur["rows"].append(dict(kv.split("=", 1) for kv in line.split()))
b = [x for x in blocks if x["label"] == label]
if not b or not b[-1]["rows"]:
    print(f"no pan data for {label}"); sys.exit(1)
rows = b[-1]["rows"]
f = lambda r, k: float(r[k])
print(f"== {label}: {len(rows)} frames ==")
blk = [f(r, "black") for r in rows if f(r, "black") >= 0]
if blk:
    print(f"black   mean={sum(blk)/len(blk):.4f} max={max(blk):.4f}")
spd = []
for i in range(1, len(rows)):
    cell = f(rows[i], "cell") * max(1.0, f(rows[i], "scale"))
    dx = (f(rows[i], "cx") - f(rows[i - 1], "cx")) * cell
    dy = (f(rows[i], "cy") - f(rows[i - 1], "cy")) * cell
    ms = max(f(rows[i], "ms"), 0.5)
    spd.append((i, dx, dy, ms))
moving = [(i, dx, dy, ms) for i, dx, dy, ms in spd if abs(dx) + abs(dy) > 0.05]
if not moving:
    print("no camera motion recorded"); sys.exit(0)
rev = stalls = spikes = 0
prev = None
for j in range(len(spd)):
    i, dx, dy, ms = spd[j]
    mv = abs(dx) + abs(dy) > 0.05
    if prev is not None and mv and prev[0] and (dx * prev[1] < -0.01 or dy * prev[2] < -0.01):
        rev += 1
    if not mv and 0 < j < len(spd) - 1:
        a, c = spd[j - 1], spd[j + 1]
        if abs(a[1]) + abs(a[2]) > 0.05 and abs(c[1]) + abs(c[2]) > 0.05:
            stalls += 1
    prev = (mv, dx, dy) if mv else prev
vel = [((abs(dx) + abs(dy)) / ms, i) for i, dx, dy, ms in moving]
for j, (v, i) in enumerate(vel):
    nb = sorted(x for x, _ in vel[max(0, j - 6):j] + vel[j + 1:j + 7])
    loc = nb[len(nb) // 2] if nb else 0
    if loc > 0 and v > 2.5 * loc and v * f(rows[i], "ms") > 4:
        spikes += 1
vv = sorted(v for v, _ in vel)
print(f"pan     moving frames={len(moving)} speed median={vv[len(vv)//2]:.3f}px/ms max={vv[-1]:.3f}px/ms "
      f"reversals={rev} stalls={stalls} spikes={spikes}")
# Pixel truth: a frame identical to the previous one (d1 == 0) sandwiched
# between frames that did change is a duplicated frame while moving.
d1 = [f(r, "d1") for r in rows]
dups = sum(1 for i in range(1, len(rows) - 1)
           if d1[i] == 0.0 and d1[i - 1] > 1.0 and d1[i + 1] > 1.0)
changing = sum(1 for x in d1 if x > 1.0)
print(f"pixels  changing frames={changing} duplicated-while-moving={dups}")
mi = set(i for i, *_ in moving)
edge = {"L": 0, "R": 0, "T": 0, "B": 0}
for i in mi:
    for k in edge:
        edge[k] = max(edge[k], min(int(x) for x in rows[i][k].split(",")))
print("edges   (min over 3 lines, max over moving frames) " + " ".join(f"{k}={v}px" for k, v in edge.items()))
if show:
    for i, dx, dy, ms in spd:
        r = rows[i]
        print(f"f={r['f']} ms={r['ms']} cx={r['cx']} cy={r['cy']} dx={dx:+.2f}px L={r['L']} R={r['R']} T={r['T']} B={r['B']}")

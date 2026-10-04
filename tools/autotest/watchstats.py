#!/usr/bin/env python3
"""Summarise the most recent block with a given label in smoothpan_watch.txt.

Per-frame fields come from frame_probe (pixel truth) plus the compositor's and
zoom camera's own view of that frame.  Reports what matters for the three
player-visible symptoms:
  black    near-black fraction of the sampled map (black areas / strobe)
  flip     frame differs from previous but matches the one before (shimmer)
  map      map blits the compositor captured (full vs partial redraws)
Usage: watchstats.py <label> [--frames]
"""
import sys
from collections import Counter

W = "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/smoothpan_watch.txt"
label = sys.argv[1]
show = "--frames" in sys.argv

blocks, cur = [], None
for line in open(W, errors="replace"):
    if line.startswith("# block label="):
        lab = line.split("label=")[1].split()[0]
        cur = {"label": lab, "rows": [], "notes": []}
        blocks.append(cur)
    elif cur is not None and line.startswith("f="):
        if "nomap" in line:
            continue
        cur["rows"].append(dict(kv.split("=", 1) for kv in line.split()))
    elif cur is not None and line.startswith("# WATCHDOG"):
        cur["notes"].append(line.strip())

mine = [b for b in blocks if b["label"] == label]
if not mine:
    print(f"no block labelled {label}")
    sys.exit(1)
b = mine[-1]
rows = b["rows"]
if not rows:
    print(f"{label}: 0 frames")
    sys.exit(0)

f = lambda r, k: float(r[k])
black = [f(r, "black") for r in rows]
d1 = [f(r, "d1") for r in rows if f(r, "d1") >= 0]
flips = sum(int(r["flip"]) for r in rows)
maps = Counter(int(r["map"]) for r in rows)
ms = sorted(f(r, "ms") for r in rows if f(r, "ms") > 0)
swings = [abs(black[i] - black[i - 1]) for i in range(1, len(black))]

print(f"== {label}: {len(rows)} frames ==")
print(f"black   mean={sum(black)/len(black):.4f} max={max(black):.4f} "
      f"frames>5%={sum(x > 0.05 for x in black)} frames>25%={sum(x > 0.25 for x in black)}")
print(f"swing   max frame-to-frame black change={max(swings) if swings else 0:.4f}")
print(f"motion  d1 mean={sum(d1)/len(d1) if d1 else 0:.1f} max={max(d1) if d1 else 0:.1f}  flips={flips}")
if ms:
    import statistics as _st
    core = [x for x in ms if x < 3 * ms[len(ms) // 2]]
    cv = (_st.pstdev(core) / _st.mean(core)) if len(core) > 2 else 0.0
    print(f"frame   ms median={ms[len(ms)//2]:.1f} p95={ms[int(len(ms)*0.95)]:.1f} max={ms[-1]:.1f} "
          f"pacing CV={cv*100:.1f}% (std/mean of present interval, outliers excluded; low = steady cadence)")
print("map blits per frame (count x frames): " + ", ".join(f"{k}x{v}" for k, v in maps.most_common(8)))
print("choice: " + ", ".join(f"{k}x{v}" for k, v in Counter(r['choice'] for r in rows).most_common()))
# Displayed tile size = composite scale x baked cell: what the eye actually sees
# zooming.  During a gesture it should move monotonically toward the target.
#   reversals  frame-to-frame direction flips (shimmer: A-A-B / A-B-A patterns)
#   holds      frames with no change while the gesture is still moving (stutter)
#   jumps      frames whose change is > 3x the median change (pops)
vis = [f(r, "scale") * f(r, "cell") for r in rows]
gest = [r["gest"] == "1" for r in rows]
deltas = []
for i in range(1, len(rows)):
    if gest[i] and gest[i - 1]:
        deltas.append((i, vis[i] - vis[i - 1]))
rev = holds = jumps = 0
prev_sign = 0
mags = sorted(abs(d) for _, d in deltas if abs(d) > 1e-4)
med = mags[len(mags) // 2] if mags else 0.0
for i, d in deltas:
    sign = (d > 1e-4) - (d < -1e-4)
    if sign == 0:
        holds += 1
        continue
    if prev_sign and sign != prev_sign:
        rev += 1
    prev_sign = sign
    if med and abs(d) > 3 * med:
        jumps += 1
print(f"glide   gesture frames={len(deltas)} reversals={rev} holds={holds} jumps={jumps} "
      f"median_step={med:.3f}px")
# What the eye sees: how far the farthest point of the map moves in one frame.
# A tile-size change of d px/tile at size v moves a point R px from the anchor
# by R*d/v px.  R ~ 2000 px from a third-in anchor to the far corner at 1440p.
R = 2000.0
disp = []
for i, d in deltas:
    base = vis[i - 1]
    if base > 0:
        disp.append((abs(d) / base * R, int(rows[i]["f"])))
if disp:
    dv = sorted(x for x, _ in disp if x > 0.01)
    dmed = dv[len(dv) // 2] if dv else 0.0
    mx = max(disp)
    # Pop = the map moved faster THIS frame than around it.  Velocity is corner
    # px per ms of this frame, compared with the median of the +-6 frames
    # around it, so a long frame that moves proportionally is not a pop.
    msmap = {int(r["f"]): float(r["ms"]) for r in rows}
    vel = [(x / max(msmap.get(fr, 1.0), 0.5), x, fr) for x, fr in disp]
    pops = []
    for j, (vv, x, fr) in enumerate(vel):
        nb = sorted(v for v, _, _ in vel[max(0, j - 6):j] + vel[j + 1:j + 7])
        local = nb[len(nb) // 2] if nb else 0.0
        if x > 4.0 and local > 0 and vv > 2.5 * local:
            pops.append((x, fr))
    print(f"corner  per-frame move median={dmed:.2f}px p95={dv[int(len(dv)*0.95)] if dv else 0:.2f}px "
          f"max={mx[0]:.1f}px@f={mx[1]}  pops={len(pops)}"
          + (" at f=" + ",".join(f"{fr}({x:.0f}px)" for x, fr in pops[:10]) if pops else ""))
for n in b["notes"]:
    print(n)
if show:
    for r in rows:
        print(" ".join(f"{k}={r[k]}" for k in ("f", "ms", "black", "d1", "d2", "flip", "choice", "map", "cell", "scale", "v", "gest", "trans")))

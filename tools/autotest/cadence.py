#!/usr/bin/env python3
"""On-screen cadence of DF's window, measured: XWayland's Present UST is the
time the compositor presented each new DF frame.  Samples every ~1 ms with
vblmsc, keeps each distinct (msc, ust), and reports the gaps between presented
frames in refresh periods.  Perfect pacing = every gap the same integer.
Usage: cadence.py <seconds> [refresh_hz]  (run while something is moving)"""
import subprocess, sys, statistics
import os as _os
HERE = _os.path.dirname(_os.path.abspath(__file__))
from collections import Counter
secs = float(sys.argv[1]); hz = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
T = 1e6 / hz
n = int(secs * 1000 / 1.1)
out = subprocess.run([HERE + "/vblmsc", "0x320000c", str(n), "900"],
                     capture_output=True, text=True, timeout=secs + 20).stdout
seen = {}
for l in out.splitlines():
    d = dict(kv.split("=") for kv in l.split())
    seen[int(d["msc"])] = int(d["ust"])
ust = [seen[k] for k in sorted(seen)]
gaps = [(b - a) / T for a, b in zip(ust, ust[1:])]
if len(gaps) < 10:
    print("too few frames"); sys.exit(1)
rounded = Counter(round(g) for g in gaps)
off = sum(1 for g in gaps if abs(g - round(g)) > 0.15)
mode_gap, mode_n = rounded.most_common(1)[0]
print(f"presented frames: {len(ust)} in {(ust[-1]-ust[0])/1e6:.2f}s  ({len(ust)/((ust[-1]-ust[0])/1e6):.1f} fps on screen)")
print("gap (refreshes) histogram: " + "  ".join(f"{k}:{rounded[k]}" for k in sorted(rounded)))
print(f"cadence regularity: {mode_n*100/len(gaps):.1f}% of gaps == {mode_gap} refresh(es); "
      f"non-integer (timestamp noise) {off*100/len(gaps):.1f}%")
# pattern irregularity: how often the gap changes from one frame to the next
chg = sum(1 for a, b in zip(gaps, gaps[1:]) if round(a) != round(b))
print(f"gap changes frame-to-frame: {chg*100/max(1,len(gaps)-1):.1f}%  (0% = perfectly steady cadence)")

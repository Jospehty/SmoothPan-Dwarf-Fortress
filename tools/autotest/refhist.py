#!/usr/bin/env python3
"""Present-interval histogram in 180 Hz refreshes, per recording label.
Usage: refhist.py <label>..."""
import collections, subprocess, sys
import os as _os
HERE = _os.path.dirname(_os.path.abspath(__file__))
for lab in sys.argv[1:]:
    out = subprocess.run(["python3", HERE + "/watchstats.py", lab, "--frames"],
                         capture_output=True, text=True).stdout
    c = collections.Counter()
    for l in out.splitlines():
        if not l.startswith("f="):
            continue
        d = dict(kv.split("=", 1) for kv in l.split())
        c[round(float(d["ms"]) / 5.5555)] += 1
    print(lab, sorted(c.items()))

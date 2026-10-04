#!/usr/bin/env python3
"""Horizontal content displacement between two shots (search dx in 0..100):
B(x+dx) ~= A(x) over an open-map band.  Usage: offset2.py <shotA> <shotB>"""
import sys
D = "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/"
def load(n):
    data = open(D + f"shot_{n}.ppm", "rb").read()
    parts = data.split(b"\n", 3); w, h = map(int, parts[1].split()); return w, parts[3]
wa, A = load(sys.argv[1]); wb, B = load(sys.argv[2])
def lum(buf, w, x, y):
    i = (y * w + x) * 3; return buf[i] * 2 + buf[i + 1] * 5 + buf[i + 2]
ys = range(300, 1100, 9); xs = range(600, 1800, 5)
best = None
for dx in range(0, 101):
    s = sum(abs(lum(A, wa, x, y) - lum(B, wb, x + dx, y)) for y in ys for x in xs)
    if best is None or s < best[0]: best = (s, dx)
print(f"{sys.argv[1]} -> {sys.argv[2]}: content moved right by {best[1]} px (mean diff {best[0]/(len(ys)*len(xs)):.1f})")

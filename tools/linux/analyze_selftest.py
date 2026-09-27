#!/usr/bin/env python3
"""Turn a smoothpan_selftest.txt FRAMES table into objective judder metrics.

The selftest already records, per frame: the composite scale, the baked cell,
the continuous visual cell v, the black-gap sizes at each viewport edge, and
the frame time.  Judder is not a single number, so we report the four things
that actually make a zoom look bad, per step:

  scale_max   how far the bake was stretched before a new one landed.  The map
              is a magnified (soft) texture at scale>1 and snaps crisp when the
              commit lands, so a big excursion reads as a "pop".  1.0 is ideal.
  bake_pops   how many times the baked cell changed during the step.  Each one
              is a potential pop.
  v_jerk      max |second difference| of the visual cell v, normalised by cell.
              v is what the player's eye tracks; a smooth ease has near-zero
              jerk, a stutter spikes.
  gap_max     largest black edge band seen (px).  Should be 0.
  ms_p95/max  frame time.  Screenshot frames are excluded (they cost ~0.5-1s of
              PNG encoding in the harness and are not gameplay cost).

Usage: analyze.py <selftest.txt> [<selftest.txt> ...]
"""
import re
import sys
import statistics

FRAME_RE = re.compile(r'^(\S+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\S)\s+(\d+)\s+(\d+)\s+'
                      r'(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)/(\d+)\s+([\d.]+)\s+'
                      r'(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+'
                      r'(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)')


def parse(path):
    steps = {}
    order = []
    in_frames = False
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('== FRAMES =='):
            in_frames = True
            continue
        if not in_frames:
            continue
        m = FRAME_RE.match(line)
        if not m:
            continue
        g = m.groups()
        step = g[0]
        rec = dict(
            f=int(g[1]), ms=float(g[2]), choice=int(g[4]), scale=float(g[9]),
            cell=int(g[10]), v=float(g[14]),
            gaps=[int(g[19]), int(g[20]), int(g[21]), int(g[22])],
        )
        if step not in steps:
            steps[step] = []
            order.append(step)
        steps[step].append(rec)
    return order, steps


def jerk(vs):
    """Max |v[i+1] - 2v[i] + v[i-1]| — spikes when the ease stutters."""
    if len(vs) < 3:
        return 0.0
    return max(abs(vs[i + 1] - 2 * vs[i] + vs[i - 1]) for i in range(1, len(vs) - 1))


def analyze(path):
    order, steps = parse(path)
    print(f"\n=== {path.split('/')[-1]} ===")
    print(f"{'step':<14}{'n':>4}{'scale_max':>10}{'bake_pops':>10}{'v_jerk':>9}"
          f"{'gap_max':>8}{'ms_p95':>8}{'ms_max*':>8}{'bridged':>8}")
    tot_jerk, tot_scale = [], []
    for step in order:
        r = steps[step]
        vs = [x['v'] for x in r]
        cells = [x['cell'] for x in r]
        pops = sum(1 for i in range(1, len(cells)) if cells[i] != cells[i - 1])
        scale_max = max(x['scale'] for x in r)
        gap_max = max(max(x['gaps']) for x in r)
        # Drop the top 2 frames as screenshot-encode outliers.
        ms = sorted(x['ms'] for x in r)
        ms_clean = ms[:-2] if len(ms) > 4 else ms
        p95 = ms_clean[int(len(ms_clean) * 0.95)] if ms_clean else 0.0
        bridged = sum(1 for x in r if x['choice'] == 2)
        j = jerk(vs)
        cellref = max(1, max(cells))
        jn = j / cellref
        print(f"{step:<14}{len(r):>4}{scale_max:>10.3f}{pops:>10}{jn:>9.4f}"
              f"{gap_max:>8}{p95:>8.1f}{max(ms_clean) if ms_clean else 0:>8.1f}{bridged:>8}")
        if 'zoom' in step or 'vanilla' in step:
            tot_jerk.append(jn)
            tot_scale.append(scale_max)
    if tot_jerk:
        print(f"{'ZOOM SUMMARY':<14}{'':>4}{max(tot_scale):>10.3f}{'':>10}{max(tot_jerk):>9.4f}"
              "   <- worst scale excursion / worst normalised jerk")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for p in sys.argv[1:]:
        try:
            analyze(p)
        except Exception as e:  # keep going across a batch
            print(f"{p}: {e}")

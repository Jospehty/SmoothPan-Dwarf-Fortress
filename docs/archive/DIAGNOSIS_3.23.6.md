# DIAGNOSIS 3.23.6 — Per-Blit Telemetry Verifies the Cell-Mismatch Mechanism

**Date:** 2026-06-04
**Build:** 3.23.6
**Status:** SHIPPED & USER-CONFIRMED STABLE

## Summary

3.23.6 added per-blit telemetry in `sdl_hook.cpp` to finally answer
"what is the blitter actually doing during the dim-lag NARROW frames?"
The answer: **the blitter IS rendering in the dim-lag region**, but at
the OLD cell size. The dim-lag is a cell-size mismatch strip, not a
missing-blits region. User confirmed the current state is "pretty
solid" with only minor artifacts at z=224 NARROW. No further fix
attempt needed unless user requests it.

## Test captures

User ran `smoothpan zoomcap v3236b blit on` then F9 twice:
- `smoothpan_telemetry_v3236b_1.txt` — 63.9 MB, 30 frames, 424,941 Blit: lines (stand-still)
- `smoothpan_telemetry_v3236b_2.txt` — 87.9 MB, 30 frames, 571,968 Blit: lines (pan + zoom)
- 30 BMPs per capture (2560x1440 32bpp)

## Per-blit log format (3.23.6 NEW)

`Blit: frame=%d dst=(x,y,w,h) src=(x,y,w,h) vpDim=(W,H) pass=%d vpass=%d vmap=%d vpscr=(x,y) shifted=%d in_ui=%d tile=%d align=%d suspect=%d`

Float version: `BlitF:` with same fields in float.

Old format still matches (regex backcompat).

## Cross-capture analysis (capture 2 pan+zoom)

**6 NARROW frames in capture 2** (z=256→128 zoom-in transition):
frames 27, 25, 23, 20, 18, 16.

**Each NARROW frame:** `main_viewport dim=(OLD) cell=(NEW)`. Dim hasn't
updated yet. E.g., frame 27 capture 2: `dim=(40,23) cell=(56,56) zf=224`
(z=256 OLD dim, z=224 NEW cell).

| SMOOTHPAN fr | z (NEW) | cell (NEW) | dim (OLD) | vpW | blitter cell | dim-lag right | dim-lag bottom |
|--------------|---------|------------|-----------|-----|--------------|---------------|----------------|
| 27           | 224     | 56         | 40×23     | 2240| 64 (OLD z=256)| 320px (12.5%) | 184px |
| 25           | 192     | 48         | 46×26     | 2208| 64 (OLD z=256)| 352px (13.8%) | 240px |
| 23           | 160     | 40         | 54×30     | 2160| 64 (OLD z=256)| 400px (15.6%) | 360px |
| 20           | 128     | 32         | 64×36     | 2048| 64 (OLD z=256)| 512px (20.0%) | 512px |
| 18           |  96     | 24         | 80×45     | 1920| 32 (OLD z=128)| 640px (25.0%) | 331px |
| 16           |  64     | 16         | 107×60    | 1712| 24 (OLD z=96) | 848px (33.1%) | 443px |

**Per-frame blit summary parsed successfully.** Each NARROW frame: 99.9% of
map blits shifted; only 1 unshifted map blit. Blitter covers full screen
in every NARROW frame (blitter cell=64 fills 40×64=2560 wide for early
transitions, 80×32=2560 for z=96, 107×24=2568 for z=64).

## Pixel analysis (frame 16 z=64 NARROW, the worst case)

`tools/check_dim_lag_region.py` + manual sampling. Main vp at frame 16:
`dim=(107,60) cell=(16,16) zf=64 screen=(33,37)`, ends at (1745, 997).
Screen is 2560×1440.

**Right dim-lag region (1712..2560, 37..997):**
- mean RGB = (89.5, 109.8, 58.8) — **grass colors**
- unique colors: 4773
- black%: 0.9
- first 10 pixels: (28, 28, 28) × 8 (DF GUI bg at very top), then variation
- **NOT pure black, NOT unrendered**

**Blit verification (DF frame 1913 = SMOOTHPAN f16):**
- 13043 total Blit: lines
- **11260 vmap=1 blits at dst=(x,y,24,24)** (blitter using OLD z=96 cell)
- dst x range: 6..2550 (covers nearly full screen)
- **3536 of those 11260 blits are in the dim-lag region (x >= 1745)**
- Blitter IS rendering there with cell=24 content

**Pixel sample at (1900, 2200, 2400) y=200..1400:** all grass/foliage
colors. The dim-lag is rendered world content at cell=24, not GUI bg.

## Mechanism (verified)

The dim-lag is a **tile-size discontinuity at the viewport edge**, NOT
missing-blits:

- Blitter uses `dispx_z` (OLD from previous z) → tiles at OLD cell
- Main vp uses `zf/4` (NEW from current z) → tiles at NEW cell
- Blitter renders `dim*dispx_z` pixels covering nearly full screen
- Main vp covers `dim*zf/4` pixels (smaller area)
- Area inside blitter coverage but outside main vp = dim-lag strip
- This strip is rendered at OLD cell, main vp is rendered at NEW cell
- User sees tiles of two different sizes side by side at x=vpW edge

At z=64 NARROW: OLD cell=24, NEW cell=16. Tiles in the dim-lag region
are 50% bigger than tiles in the main vp. User's eye reads this as
"the view is broken / unrendered" because the tile size mismatch is
visually jarring.

## User confirmation (2026-06-04)

> "wait a minute, I'm playing on the latest version and I'm not seeing
> that big unrendered region, now it's just a few little unrendered
> artifact lines that lines up closer with what you're describing. It
> might be my fault giving you bad information about the latest version.
> Even after panning around it's actually pretty solid (not smooth, nor
> perfect with these slight presumably dim lag related artifacts that
> you've been able to spot in the f9, but it's better than it was before)."

The "few little unrendered artifact lines" matches the z=224 NARROW
analysis (320px right + 184px bottom = 12% of screen, "a few little
lines" at the right edge and bottom). User typically uses mid-zoom
(z=224, z=192) and doesn't usually hit z=64 NARROW where the artifact
would be 52.8% of the screen.

## Why earlier "30%+ unrendered" report was different

The user originally reported "30%+ of the frame is unrendered for a
bit while zooming out" — this was likely from an older test build with
a different fix state. Current 3.23.6 build (with all fix attempts
kill-switched) shows the smaller z=224 artifact. The 52.8% analysis
at z=64 is the worst-case worst-zoom extrapolation; the user doesn't
typically hit that case.

## What 3.23.6 ships

- `sdl_hook.cpp`: per-blit `Blit:`/`BlitF:` log with new fields
  (`frame=`, `src=`, `vpDim=`). All four SDL hook variants pass
  `srcrect` with NULL guard.
- `tools/parse_f9_zoom.py`: parses new fields, adds per-frame blit
  summary parsing, adds main-viewport coverage analysis with
  `[OVERFLOW]` and `[COV]` sections, new columns `MCovR`, `MCovB`,
  `shft%`, `map%`, `tile%`.
- `tools/check_dim_lag_region.py` (NEW): BMP pixel sampler. Loads
  uncompressed 24/32-bit BMP, samples right-of-vp and below-vp
  regions, reports color stats, optional cross-frame region_diff.

## Design for 3.24.0 (deferred, ready if user requests)

**Goal:** align blitter cell with main vp cell for the main vp blits
only, without affecting UI/radar.

**Approach:** intercept main vp blits only. Filter:
`vmap=1 && vpscr == main_vp->screen && !in_ui`. Override the blit's
`dst.w` and `dst.h` from `dispx_z` to `zf/4` (the new cell). The blit
position stays the same; only the size changes.

**Why this would work:**
- The 3.23.0/3.23.2 approach modified `dispx_z` globally → broke UI
- The 3.23.5 approach grew `vp->dim` → didn't address cell mismatch
- This approach changes the blit rectangle per-blit, only for the
  main vp blits, leaving UI/radar untouched
- The blitter renders at the new cell, matching the main vp cell
- No realloc, no dim change, no dispx_z change, no UI impact

**Risk:** blit count is `vp->dim_x * vp->dim_y` at the OLD cell.
If we change the cell, the total coverage changes from
`dim*OLD_cell` to `dim*NEW_cell`, which is smaller (zooming IN).
The blitter would under-fill the dim-lag region. Need to also
re-tile the dim-lag region with the new cell to cover the same
area. This is non-trivial — defer until user requests.

**Alternative (simpler but invasive):** modify the blit's `dst.x`
and `dst.w` so the new-cell tiles map to the same world coordinates
as the old-cell tiles. This is essentially "re-stretch the blits to
new cell, anchored at the same world position". Complex but
mechanically clean.

## Files referenced

- `src/sdl_hook.cpp` (3.23.6 changes)
- `src/sdl_hook.h` (3.23.6 changes)
- `tools/parse_f9_zoom.py` (3.23.6 changes)
- `tools/check_dim_lag_region.py` (NEW)
- `C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\dfhack-config\smoothpan\smoothpan_telemetry_v3236b_1.txt` (63.9 MB)
- `C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\dfhack-config\smoothpan\smoothpan_telemetry_v3236b_2.txt` (87.9 MB)
- `C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\dfhack-config\smoothpan\smoothpan_frame_v3236b_c*_*.bmp` (60 files)
- `C:\Users\Joseph\AppData\Local\Temp\opencode\dim_lag_visual_c2.png` (7-frame side-by-side crop with yellow viewport edge line)
- `C:\Users\Joseph\AppData\Local\Temp\opencode\check_f16.py`, `check_f16_v2.py` (one-off pixel samplers)

## Related diagnoses

- `docs/DIAGNOSIS_3.23.4.md` — initial cross-capture F9 analysis
- `docs/DIAGNOSIS_3.23.5.md` — 3.23.5 fix crash root-cause TBD
- `docs/DIAGNOSIS_3.23.2.md` — why modifying `dispx_z` broke UI
- `docs/DIAGNOSIS_3.23.1.md` — why SMOOTHPAN header reads wrong cell
- `docs/ZOOM_POSTMORTEM.md` — anti-patterns authoritative

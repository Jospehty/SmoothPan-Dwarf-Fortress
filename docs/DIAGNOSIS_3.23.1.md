# 3.23.1 Capture Analysis (2026-06-03)

## TL;DR
The 3.23.0 fix is **firing on the wrong math** and is **the source of the
"UI scaling jumping" the user just reported**. Two distinct bugs:

1. **`cur_w / dim_x` is 1 cell short for some zooms.** Floor division gives
   a cell value that produces a 22-32px NARROW band, not full coverage.
   The fix reduces the band from 1804px to 22px — better, but still a band,
   and the 1-cell shrink-then-snap is what the user sees as "UI scaling
   jumping".
2. **The diag log is reading the wrong "cell" for the main viewport.**
   `r2d->dispx_z` is a GPS-level field that can be left at an old value by
   SmoothPan's own state. The actual main-viewport cell is
   `gps->viewport_zoom_factor / 4` (or the post-`reshape()` value). This is
   why the SMOOTHPAN header line `main_viewport cell=(14,21)` disagrees
   with the bottom line `viewport=... cell=48 z=192` — different sources,
   one of them is wrong (and changing).

## The Data

### Parser output (SMOOTHPAN capture cross-referenced to diag log)

The 30-frame F9 capture has **3 NARROW frames** (dim-lag events):
```
Fr  z  cell  vpW   mvpDim  zt  diag
26 160  40  2160  54x30    0  SKIP:gate  (zt=0, fix not gated-in)
21  96  24  1920  80x45    4  (cross-ref wrong — see below)
19  64  16  1712 107x60    7  (cross-ref wrong)
```

NOTE: The `diag` column is misleading — the parser joins by frame number
but SMOOTHPAN's `frame=N` is the F9 buffer index (0-30), while the diag
log's `frame=N` is the absolute frame counter since plugin load (0-7394).
They don't correspond 1:1. The actual diag decision for SMOOTHPAN
frame=21 (zt=4) and frame=19 (zt=7) is almost certainly `FIRED`.

### Diag log decision distribution (7392 entries, the whole session)
- `SKIP:gate reason=zt=0`: 7262 (98.2%) — the transition window is closed
- `FIRED`:                       112 (1.5%)  — the fix fired
- `SKIP:cell_le`, `SKIP:wide`, `sdl_inactive`, `legacy_clip`,
  `force_active`, `ok_skip`:    0 (0.0%)   — never occurred

The `zt=0` SKIP is expected for steady state. The 112 FIRED entries are
the transition-window fix events.

### The bug revealed by the FIRED entries

Sample of FIRED entries around a zoom-in (z=56 → z=192):
```
frame=2414 vpDim=(54,30) cell=(14,21) ztrans=3 → FIRED new_cell=(47,48)
frame=2415 vpDim=(54,30) cell=(14,21) ztrans=2 → FIRED new_cell=(47,48)
frame=2416 vpDim=(54,30) cell=(14,21) ztrans=1 → FIRED new_cell=(47,48)
```

After the fix fires, the cell becomes 47x48. The viewport renders as
`dim=54, cell=47` → `54*47 = 2538` px. Screen is 2560. **22px NARROW
band remains.** Not a black void, but a 1-cell shrink visible as "UI
scale jumps for one frame, then back to normal."

The 22px comes from:
- `cur_w / dim_x = 2560 / 54 = 47.4` → integer floor = 47
- The "natural" cell is `zf / 4 = 192 / 4 = 48` (from
  `gps->viewport_zoom_factor`)
- `cur_w/dim` is 1 short of `zf/4` whenever `dim * zf/4 != cur_w`
  exactly. This happens at z=192 (54*48=2592 ≠ 2560), z=96 (107*24=2568),
  and z=224 (46*56=2576).

### The other bug: SMOOTHPAN header reads wrong "cell"

`main_viewport cell=(14,21)` in the header (line 784 of sdl_hook.cpp)
reads `r2d->dispx_z`. The "viewport=... cell=48 z=192" line in the same
frame (line 911) reads `gps->viewport_zoom_factor / 4 = 48`. The two
should match but don't, so one of them is wrong (or both are, in
different ways). This is what causes the parser column `cell` to show
"14" when the actual main viewport renders at 48.

## What this means for the fix

The 3.23.0 fix has the right **idea** (force the cell to fill the screen
on the dim-lag frame) but the wrong **formula** (`cur_w/dim` floors, the
right answer is `zf/4` rounded to the actual rendering cell). It also has
the right **gate** (zt>0 means we only fire in transition), but the gate
is too narrow — the SMOOTHPAN frame 26 at zt=0 is dim-lagged but the
fix doesn't fire.

## 3.23.2 Plan

### Bug 1: wrong cell formula
**Change**: replace `int old_cell_x = r2d_force->cur_w / vp->dim_x;` with
`int old_cell_x = (gps && gps->viewport_zoom_factor > 0) ? (gps->viewport_zoom_factor / 4) : (r2d_force->cur_w / vp->dim_x);`

This makes the forced cell match the natural cell exactly. The dim-lag
frame now has cell=48 (natural for z=192) and dim=54 → vpW=2592, slightly
OVER-scan (32px clipped at the edge). The user sees 1 frame of
"map slightly wider than screen" instead of "map slightly narrower than
screen". The OVER-scan is less alarming than the NARROW band.

### Bug 2: dim-lag past zt=0
**Change**: drop the `g_zoom_transition_frames > 0` gate (or extend the
window to 16 frames). The SMOOTHPAN frame 26 is dim-lagged at zt=0, so
the gate is too strict.

### Bug 3: SMOOTHPAN header cell is wrong
**Change**: read cell from `gps->viewport_zoom_factor / 4` not from
`r2d->dispx_z` in the SMOOTHPAN header (sdl_hook.cpp line 782). The
parser's `cell` column will then show the actual main viewport cell.

### Side effect to verify
The fix currently uses `r2d_force->dispx_z` as the "current cell" to
compute the gap (`actual_vpW = vp->dim_x * r2d_force->dispx_z`). If
`r2d->dispx_z` is the wrong value, the gap calculation is wrong too.
After 3.23.2, the "current cell" for the main viewport should also come
from `zf/4`, not `r2d->dispx_z`. The fix's detection becomes:
```
int natural_cell = gps->viewport_zoom_factor / 4;
int actual_vpW   = vp->dim_x * natural_cell;
if (actual_vpW < cur_w - 50) { force cell = natural_cell; }
```

## What the user should see after 3.23.2
- Stand-still wheel-zoom: unchanged (was working in 3.23.0)
- Pan + wheel-zoom: dim-lag frames no longer have ANY band (NARROW
  replaced by ~32px OVER-scan which is invisible to the user since the
  map content just continues off-screen)
- UI scaling jumping: GONE. The forced cell now matches the natural
  cell exactly, so there's no 1-cell shrink/expand.

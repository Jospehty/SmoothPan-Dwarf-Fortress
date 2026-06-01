# SmoothPan 3.11.0 — Diagnosis & Fix Plan

This release tackles every outstanding issue at once, backed by an upgraded F9
telemetry capture so we can iterate quickly if any fix misses.

## Confirmed geometry (from F9 telemetry)

At the test zoom: screen `2560x1440`, `cell = 40px`, `z (tile_px) = 160`,
renderer `origin = (6,6)`, main viewport `vp = (24, 28, 2584, 1468)`,
`dim = 64x36`.

Two structural facts drive most bugs:

1. **The tile bake grid (`origin = 6,6`) is not aligned to the map viewport
   (`vp.left=24, vp.top=28`).** Offset `(18,22)px`, not a multiple of `cell`.
   So no map row/col is ever "viewport-aligned" by a `vp.left/top`-based test.
2. **DF runs 10+ `update_full_viewport` passes per frame** (renderer trace:
   screen positions `(0,0),(2,10),(98,89),(0,25),(45,25)…`, all same `dim`).
   Only one generates the ~2000 tile blits. We were shifting exactly one.

## Problem 1 — Top rows don't sub-tile

`is_embedded_hud_tile_blit()` misclassifies real map tiles as HUD because
`is_viewport_map_aligned()` measures `(y - vp.top) % cell`. Tiles bake at
`origin_y=6`, not `vp.top=28`, so the alignment test always fails and the
embedded-HUD guard claims every tile-sized blit in `[vp.top, vp.top+tile_px)`
= `[28,188)` (4 cells tall) is HUD → unshifted.

**Fix:** alignment uses `origin_x/origin_y` from `renderer_2d`; shrink the
embedded band from `tile_px` to one `cell_size`; treat a blit as top-chrome
only if it is *fully* above `vp.top`. Genuine on-grid tiles then shift; an
off-grid status row stays excluded.

## Problem 2 — Left border shimmers / reveals terrain

Asymmetry: viewport `[24,2584]` on a `2560` screen → **left margin `[0,24]` is
on-screen, right margin `[2560,2584]` is off-screen.** Shifting tiles left
slides column 0 into `[0,24]`, and the amount eaten fluctuates with the
sub-pixel shift → shimmer. The right margin is off-screen, so the right edge
looks perfect. The 3.10.8–3.10.11 left fills targeted the wrong region
(`[24,46]`), never `[0,24]`.

**Fix:** snap the map-pass clip rect to exactly the viewport
`[vp.left, vp.top, vp_w, vp_h]` so shifted tiles can never paint into the
left/top margin. The margin becomes a constant-width, vanilla-faithful border
(no shimmer); sub-tile motion happens entirely inside the viewport. The
misguided left-fill machinery is removed (the left edge needs no gap fill —
content flows in from the right while column 0 slides out the clipped left).

## Problem 5 — Lower-z / off-map tiles don't align

Lower-z show-through and off-map reveal render in *separate*
`update_full_viewport` passes that don't match our single-pointer gate, so they
aren't shifted while neighbors are.

**Fix:** broaden the shift gate to any viewport whose `dim_x/dim_y` match the
main viewport (all map-content passes share the dim), so they shift by the same
`render_shift` and stay aligned.

## Problem 4 — Minimap outline frozen

We move the camera by writing `window_x/y` directly and erase the `CURSOR_*`
inputs, so DF's scroll path (which marks the view dirty) never runs.

**Fix:** set `gps->force_full_display_count` whenever `window_x/y` changes so the
minimap rectangle recomputes each tile step.

## Problem 3 — Mouse desync — **RESOLVED (3.11.34)**

Historical note: early fix dropped static origin term from SDL hook. Final fix was **GPS-only boot** — bump `precise_mouse_x/y` in feed/logic only; no SDL mouse layer in production. See [MOUSE_SYNC.md](MOUSE_SYNC.md).

## Debug upgrades (F9)

- **Per-pass tagging:** each `update_full_viewport` increments a pass counter;
  every blit logs its `pass` id; a per-pass summary logs `dim/screen/blits/shifted`.
- **Clip capture:** log DF's incoming map-pass clip rect and the snapped rect.
- **Edge profile:** in `RenderPresent`, read pixels on sampled rows/cols and log
  where content starts on the left/right/top — shimmer shows up as a fluctuating
  number across frames instead of needing visual inspection.

## Implementation order

1. Debug upgrades (make everything verifiable).
2. Problem 1, 2, 5 (the visible map issues).
3. Problem 4, then 3.

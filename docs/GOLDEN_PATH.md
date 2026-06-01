# SmoothPan Golden Path

This document defines the **target visual state** for SmoothPan and lists patterns that must not be reintroduced.

## Target state (user-verified baseline)

| Working | Known follow-ups (Phase B) |
|---------|---------------------------|
| Smooth sub-tile map pan | Tooltip offset vs cursor on map |
| HUD stable (toolbar, panels) | UI clicks occasionally mis-register while panning |
| No tile-scale ghosting / judder | |

Phase A (3.3.0) restores the visual baseline. Phase B fixes mouse/tooltip/UI without changing render shift rules.

## Render shift rule

Shift a blit when **all** are true:

1. Intersects the strict map viewport
2. NOT toolbar bands (top/bottom)
3. NOT interface glyph / large panel blits (14×21, ≥200×200)
4. NOT Premium info panel
5. NOT intersecting a **leaf** widget (overlay panels; container bounds ignored to avoid fullscreen-root false positives)

**Do not gate on SDL clip rect.** Premium DF never sets a viewport-matching clip in F9 telemetry (`pass=0` on every blit). Clip logging is kept for pipeline RE only.

Mouse compensation uses **`IsMouseInUI`** (viewport bounds + widget hit-test), not `classify_blit` — toolbar buttons are not map blits but must not receive shifted coordinates.

## Overscan (edge fill)

Black/juddering borders appear when map blits shift without extra tiles drawn at the leading edge. Paired mechanism:

1. **`begin_render_overscan`**: if `render_frac > 0`, temporarily `window_x/y -= 1` so DF draws one extra tile row/column
2. **`render_shift`**: `render_frac * z - overscan_tiles * z` (compensates for the temporary window pull-back)
3. **Clip expand**: when overscan is active and clip matches viewport, expand by ±`tile_px` (same as pre-3.3.0 architecture)

Do not use clip expand or edge masks without overscan.

## Sub-pixel shift

- `frac_x/y` updated in `SmoothCamera.update()` each logic tick
- `freeze_render_frac()` once at render start (`begin_render_overscan`) — frozen `render_frac_x/y` for the whole frame
- Integer blits: subtract `lround(render_shift)`
- Float blits (`RenderCopyF` / `ExF`): subtract **float** `render_shift` directly (no int truncation of destination)

## Mouse compensation (Phase B)

- Hook `SDL_GetMouseState` and `SDL_GetGlobalMouseState` only
- Compensate when **not** `IsMouseInUI` (viewport bounds, widget tree, Premium info panel)
- Use the same frozen `render_shift` as render hooks
- Do **not** use widget-tree filtering for render classification (false positives / Z-order fights)

## Anti-patterns (from historical review + regressions)

| Pattern | Why it fails |
|---------|----------------|
| Shift only `tile_sized` blits | Entities on `RenderCopyF` stay on integer grid → judder |
| Widget-tree render filter **without map-pass gate** | Unit panel 40×40 borders shifted → orange ghosting |
| Widget-tree render filter **as sole gate** | Map false positives / Z-order fights (Gemini) |
| Clip rect expand without overscan | Edge ghosting when overscan window is disabled |
| Overscan **scaling** of dst rects | Distorts sprites; breaks alignment |
| `tile_aligned` / `fully_inside` gates | Pinned edge tiles; alternating ghost frames |
| Present-time frac resnapshot | Shift can drift mid-frame vs camera update |
| Present-time black edge masks with overscan | Double-compensation artifacts |

## Pass criteria

**Phase A (visual):**

- Pan NE at 60 FPS: smooth, no tile-scale alternating ghosts
- HUD toolbar/panels: static
- F9 telemetry: non-UI viewport blits shifted > 99% for both `tile=1` and `tile=0`

**Phase B (input):**

- Map clicks accurate while panning
- UI buttons click without spurious panel dismiss
- Tooltips track cursor on map (UI tooltip drift acceptable as follow-up)

## Test checklist

After 3.3.0:

1. Console: `SmoothPan 3.3.0 enabled`
2. Pan NE — smooth? HUD static?
3. F9 — `sprite=…/… (100%)` and `tile=…/… (100%)`?

After Phase B mouse fixes:

4. Map clicks while panning
5. UI panel buttons (no spurious dismiss)
6. Tooltip vs cursor on map

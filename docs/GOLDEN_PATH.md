# SmoothPan Golden Path

Target visual and input state for SmoothPan. Patterns listed under **Anti-patterns** must not be reintroduced.

See also [MOUSE_SYNC.md](MOUSE_SYNC.md) for the resolved mouse + designation model (3.11.46).

## Target state (user-verified baseline — 3.11.46)

| Working | Remaining minor follow-ups |
|---------|---------------------------|
| Smooth sub-tile map pan (all edges, zoom levels) | Optional cleanup of debug `both`/SDL toggle |
| HUD stable (toolbar, panels) | |
| Mouse: world + UI clicks while panned | |
| Mining designation drag while panned | |
| Tooltips, designation preview, cursor box on map | |
| Minimap outline | Catches up on pan stop; may lag during pan (lazy mode OK) |

Phase A (visual), Phase B (mouse), and Phase C (designation drag) are **complete** for normal play.

## Render shift rule

Shift a blit when **all** are true:

1. Intersects the strict map viewport
2. NOT toolbar bands (top/bottom)
3. NOT interface glyph / large panel blits (14×21, ≥200×200)
4. NOT Premium info panel
5. NOT intersecting a **leaf** widget (overlay panels; container bounds ignored to avoid fullscreen-root false positives)

**Do not gate on SDL clip rect.** Premium DF rarely sets a viewport-matching clip in F9 telemetry. Clip logging is kept for pipeline RE only.

Mouse compensation uses the **unified UI gate** in `viewport.cpp` (`map_pick_screen_for_gate`, `mouse_gate_should_compensate`), not raw SDL coords alone.

## Overscan (edge fill)

Black/juddering borders appear when map blits shift without extra tiles drawn at the leading edge. In current SDL mode, overscan window offset is **disabled**; sub-tile motion is SDL blit shift only (`render_shift = render_frac * cell`).

Do not use clip expand or edge masks without understanding overscan interaction — see [PIPELINE.md](PIPELINE.md) for history.

## Sub-pixel shift

- `frac_x/y` updated in `SmoothCamera.update()` each logic tick
- `freeze_render_frac()` once at render start (`begin_render_overscan`)
- Integer blits: subtract `lround(render_shift)`
- Float blits (`RenderCopyF` / `ExF`): subtract **float** `render_shift` directly

## Mouse compensation (production — 3.11.46)

- **Boot to GPS-only** (`mouse_comp_boot_gps()` on enable)
- Bump **`gps->precise_mouse_x/y`** in `feed` and `logic` only
- **Do not** bump `mouse_x/y` or `window_x/y` globally
- **Exception:** `designation_sync.cpp` patches `mouse_x/y` only during **live rectangle drags** over map (saved/restored around vanilla)
- SDL `GetMouseState` hook is **off** in production (`mouse=gps`)
- Same frozen `render_shift` as render hooks; skip when UI gate says in-UI

Debug: F8 toggles `gps` ↔ `both`; F9 dumps telemetry. Not required for normal play.

## Anti-patterns (from historical review + regressions)

| Pattern | Why it fails |
|---------|----------------|
| Shift only `tile_sized` blits | Entities on `RenderCopyF` stay on integer grid → judder |
| Widget-tree render filter **without map-pass gate** | Unit panel borders shifted → ghosting |
| Widget-tree render filter **as sole gate** | Map false positives / Z-order fights |
| Bump `mouse_x/y` globally in `apply_mouse_compensation` | Breaks UI tabs / text grid (3.11.42) |
| Bump `mouse_x/y` during stale `doing_rectangle` without live `selection_rect` | Closes dwarf info / poisons UI clicks (3.11.45) |
| Bump `window_x/y` for mouse | Breaks `getMousePos()` paths |
| SDL hook + GPS bump both on at boot | Double-comp / wrong boot state; use GPS-only |
| Auto mode warmup cycles | Never matched manual F8 timing; polluted SDL state |
| Constant `(vp.left - origin)` in mouse comp | Gap not multiple of cell → multi-tile error |
| Present-time frac resnapshot | Shift drifts mid-frame vs camera update |

## Pass criteria

**Visual:**

- Pan NE at 60 FPS: smooth, no tile-scale alternating ghosts
- HUD toolbar/panels: static
- F9: non-UI viewport blits shifted > 99% for tile and sprite classes

**Input (3.11.34+):**

- Enable → pan → map designate: 0 tile error at frac 0, 0.25, 0.5, 0.75
- UI toolbar clicks: correct, no spurious dismiss
- Multiple zoom levels: same
- F9 shows `mouse=gps` when correct

## Test checklist

1. Console: `SmoothPan 3.11.41 enabled` (or current version)
2. Pan NE — smooth? HUD static?
3. Click map feature mid-pan — correct tile?
4. Click UI tab / panel — correct hit?
5. F9 — `mouse=gps`, sensible `shift=` while panned

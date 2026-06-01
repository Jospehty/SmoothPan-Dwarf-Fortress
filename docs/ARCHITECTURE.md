# SmoothPan Architecture

SmoothPan adds sub-tile camera panning to Dwarf Fortress Premium (DF 50.x) via a DFHack plugin. The game logic continues to use integer tile coordinates; sub-tile motion is applied only at SDL render time.

See also [GOLDEN_PATH.md](GOLDEN_PATH.md) for the target visual baseline and anti-patterns.

## Split-brain camera

| Layer | Responsibility |
|-------|----------------|
| **Logic** | `window_x`, `window_y` — integer tile center; updated only when crossing tile boundaries |
| **Visual** | `frac_x`, `frac_y` — fractional remainder in [0, 1); frozen once per frame as `render_frac_x/y` |

```text
feed (WASD) → panning flags → SmoothCamera.update() → window_x/y + frac_x/y
                                                      ↓
render start → freeze_render_frac() → render_shift = render_frac * z
                                                      ↓
                              SDL_RenderCopy* hooks shift map blits by -render_shift
```

## Coordinate systems

| Symbol | Meaning |
|--------|---------|
| `viewport_zoom_factor` (`z`) | Pixels per **graphical tile** |
| `main_viewport->dim_x/y` | Viewport size in **4×4 text cells** (4 cells = 1 graphical tile) |
| `cell_size` | `z / 4` pixels per cell |
| `window_x/y` | Camera center in **tile** coordinates |

Viewport pixel bounds:

```text
left   = main_viewport->screen_x
top    = main_viewport->screen_y
right  = left + dim_x * cell_size
bottom = top  + dim_y * cell_size
```

Camera edge clamp half-extent in tiles: `dim_x / 8` (cells ÷ 4 cells/tile ÷ 2).

## Hook points

### DFHack vtable (`smoothpan.cpp`)

- **`viewscreen_dwarfmodest::feed`** — intercepts cursor keys, sets pan direction, erases keys so vanilla tile-step pan does not run.
- **`viewscreen_dwarfmodest::render`** — runs `SmoothCamera.update()`, freezes render frac via `begin_render_overscan()`, calls vanilla render, restores window.

### MinHook on SDL2 (`sdl_hook.cpp`)

| API | Purpose |
|-----|---------|
| `SDL_RenderCopy` / `Ex` | Shift classified map blits by `-lround(render_shift)` |
| `SDL_RenderCopyF` / `ExF` | Shift by float `-render_shift` (sub-pixel) |
| `SDL_RenderPresent` | Debug dumps (F9/F10), no mid-frame frac resnapshot |
| `SDL_GetMouseState` / `SDL_GetGlobalMouseState` | Compensate mouse over map (not UI) |
| `SDL_RenderSetClipRect` | Track map render pass (viewport clip match) |

All SDL hooks no-op when the plugin is disabled (`is_enabled == false`).

## Blit classification (`viewport.cpp`)

A blit is a map candidate when it intersects the viewport and is **not** excluded by toolbar bands, interface glyph sizes, Premium info panel, or widget-tree overlap.

**Shift only during the map render pass:** `SDL_RenderSetClipRect` matching the strict viewport sets `g_in_map_pass`. UI overlays drawn afterward (unit window, etc.) are not shifted even if their 40×40 border tiles overlap the viewport.

Widget-tree intersection is used for **overlay exclusion** and **mouse hit-testing**, not as the only render gate.

Entity sprites use `SDL_RenderCopyF`; classify by position only (FRect w/h are not pixel sizes).

## Overscan

Overscan window offset is **disabled** in 3.3.x. Gap fill via temporary `window_x/y` decrement is not active; clip expansion was removed to match.

## Debug commands

```text
smoothpan dump <frames> [delay]      — BMP frames + blit telemetry to dfhack-config/smoothpan/
smoothpan classify <frames> [delay]  — log suspected HUD misclassification only
F9 / F10                             — in-game hotkeys for dump / classify (while enabled)
```

F9 header includes:

- `map=shifted/total (%)` — non-UI viewport blits
- `tile=…` — tile-sized map cells
- `sprite=…` — non-tile blits (entities/effects)

Logs are written under `{dfhack-config}/smoothpan/`.

## Known limitations

- Dwarf Fortress mode only (`viewscreen_dwarfmodest`)
- Windows: physical key release via `GetAsyncKeyState`
- Premium DF first; widget tree used for **mouse** UI detection only
- Map-anchored overlays (cursor box, designations) may not shift with terrain until DF-side hooks are added

## File map

| File | Role |
|------|------|
| `smoothpan.cpp` | Plugin entry, vtable hooks, commands |
| `camera.cpp` | Camera physics, frozen render frac, zoom resync |
| `viewport.cpp` | Viewport math, UI hit-testing, blit classification |
| `sdl_hook.cpp` | SDL MinHook implementation |
| `debug_paths.cpp` | Portable log path under dfhack-config |

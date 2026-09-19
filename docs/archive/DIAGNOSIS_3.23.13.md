# Diagnosis 3.23.13 — Workshop placement ghost sync fix

## Symptom
User reported that in `BUILDING_PLACEMENT` mode (and `ZONE_PAINT`,
`STOCKPILE_PAINT`, `BURROW_PAINT`), the building/zone ghost cursor was drawn
out of sync with the actual mouse position. The click landed at the correct
(compensated) world tile, but the ghost was rendered at the un-compensated
position. Difference: 1 world tile (~48–64 pixels depending on zoom).

## Versions
- 3.23.6: shipped, stable (visual panning)
- 3.23.7: first attempt at ghost sync — **did not work**
- 3.23.8: added `world_cursor` F9 telemetry
- 3.23.9: added `world_cursor_rend_start/end` capture
- 3.23.10: added `mode bottom/desig` logging
- 3.23.11: added per-sub-check `wants_render` trace
- 3.23.12: added per-sub-check `overmap` trace
- 3.23.13: **FIX** — works, user confirmed

## Root cause
`designation_over_map()` (in `src/designation_sync.cpp`) gated render-time
comp on `IsMouseInUI_reason() != 0`. In placement modes, the
building-placement panel is a DF widget whose CONTAINER rect covers the
entire viewport. `IsMouseInUI_reason()` calls `widgets_contain_point()`,
which matches the container's rect (`check_widget_visible_at` —
container-aware). So it returned **5** (`widgets_contain_point`) even
though the cursor was over the map, not over a real leaf UI widget.

The leaf-only `mouse_over_ui_widget()` (which uses `leaf_widget_contains_point`
and never matches a container's own rect) correctly returned `false` in
this case, but `designation_over_map()` bailed out at the `IsMouseInUI_reason`
check before reaching it.

## Fix
`src/designation_sync.cpp:designation_over_map()` — for placement modes,
skip the `IsMouseInUI_reason()` container-rect check. Rely on the
leaf-only `mouse_over_ui_widget()` check + the `mouse_gate_should_compensate()`
compensable-map gate. Rectangle drag still uses the strict container-rect
check (avoids patching while dragging over sidebar panels).

## F9 evidence chain
1. **3.23.8**: `world_cursor_f9=(-30000,-30000,-30000)` at SDL_RenderPresent
   (DF resets world cursor to sentinel at end of frame — not the bug)
2. **3.23.9**: `precise_start=precise_end=(1226,729)` (un-bumped) — comp
   not applied at render
3. **3.23.10**: `mode bottom=1` (BUILDING_PLACEMENT) — mode check correct
4. **3.23.11**: `wants_render last=0 drag=0 placement=1 overmap=0` —
   `designation_over_map()` returning false
5. **3.23.12**: `overmap inui=5 widget=-1 gate=-1 raw=(1317,693)` —
   `IsMouseInUI_reason()` returning 5 (container rect match)
6. **3.23.13**: `wants_render last=1 overmap=1` and `precise_end` bumped
   (1311+8=1319) — comp applied, ghost in sync ✓

## Key code locations
- `src/designation_sync.cpp:109` `designation_over_map()` — split gate
- `src/designation_sync.cpp:79` `placement_mode_active()` — placement mode check
- `src/designation_sync.cpp:99` `designation_sync_wants_render()` — entry
- `src/viewport.cpp:131` `widgets_contain_point()` — container-rect check
- `src/viewport.cpp:169` `mouse_over_ui_widget()` — leaf-only check
- `src/smoothpan.cpp:154` `apply_mouse_compensation('r')` — render comp gate
- `src/smoothpan.cpp:449` render interpose — comp_rend decision

## Notes
- The world cursor `df::global::cursor` is at the sentinel `(-30000)` at
  F9 time (end of frame) and also at the sentinel during the entire
  render. The building ghost is NOT drawn from `df::global::cursor` —
  it uses `precise_mouse` (or a derived tile) directly. This is why the
  ghost desync could be fixed purely by bumping `precise_mouse` at
  render time, without touching the world cursor.
- `mouse_x/y` (text-grid index) is NOT affected by the comp (int division
  by cell size rounds to 0 for sub-tile shifts). This is correct and
  intentional — UI hit-testing uses `mouse_x/y`, not the comp.
- All 3.23.8–3.23.12 diagnostic fields remain in place (gated by F9
  capture frames), harmless overhead outside F9 dumps.

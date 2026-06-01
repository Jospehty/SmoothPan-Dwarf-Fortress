# SmoothPan — Smooth Zoom Plan

**Status:** Phase 2 **parked at 3.13.7** — SDL zoom scale disabled (viewport buffer constraint); see [INVESTIGATION_ZOOM_AND_EDGES.md](INVESTIGATION_ZOOM_AND_EDGES.md)  
**Ladder (verified):** z = 64, 96, 128, 160, 192, 224, 256 (Δ32)  
**Input path:** wheel → `feed_ZOOM_IN` / `feed_ZOOM_OUT` (not `renderer::zoom`)  
**Next:** Trace vanilla rebake allocation → safe margin ring → re-enable visual zoom

---

## Goal

Add **smooth sub-step zoom** (mouse wheel / zoom keys) on top of existing smooth pan, using the same **split-brain camera** model: vanilla keeps discrete `viewport_zoom_factor`; motion is visual-only until commit.

Target feel: modern map navigation — cursor-anchored zoom, no HUD wobble, designation and WASD/MMB unchanged.

---

## Design principle (same as pan)

| Layer | State | Who owns it |
|-------|-------|-------------|
| **Logic / sim** | `window_x/y`, integer `viewport_zoom_factor` | Vanilla + commit on anim end |
| **Visual** | `frac_x/y` (pan), `zoom_anim_t` + `render_zoom_scale` (zoom) | SmoothPan render only |
| **Input** | Intercept before vanilla | `feed` / wheel (TBD) |

```text
true_x/y + frac          → render_shift = frac * cell_visual
zoom_from_z → zoom_to_z  → render_zoom_scale = lerp(z_from, z_to, ease(t)) / z_baked
SDL map blits            → pan shift, then scale about anchor
```

**Do not** change `viewport_zoom_factor` every animation frame — one rebake per completed step.

---

## Phased delivery

| Phase | Build | Deliverable | Must not break |
|-------|-------|-------------|----------------|
| **0 — Discovery** | 3.12.0 | Zoom ladder, input path, renderer hook telemetry | Everything (observe only) |
| **1 — Intercept** | 3.12.1 | Block vanilla snap-zoom; queue target level | WASD, MMB, designation |
| **2 — Visual anim** | 3.12.2 | SDL scale on map blits (center anchor MVP) | HUD static |
| **3 — Cursor anchor** | 3.12.3 | Preserve world point under cursor; pan frac on commit | Golden-path mouse |
| **4 — Wheel + perf** | 3.12.4 | Debounce, F7 tags, minimap on zoom end | F7 budget |
| **5 — Release** | **v1.2.0** | Default-on smooth zoom | Full regression matrix |

Each phase uses the same checklist as [GOLDEN_PATH.md](GOLDEN_PATH.md) plus zoom-specific rows.

---

## Phase 0 — Discovery (current)

**Questions to answer before writing animation code:**

1. **Discrete zoom ladder** — all `viewport_zoom_factor` values Premium uses
2. **Input path** — `ZOOM_IN`/`ZOOM_OUT` in feed vs `renderer::zoom()` vs both
3. **Call order** — feed → logic → `set_viewport_zoom_factor` → rebake timing
4. **Rebake cost** — F7 frame time on zoom step vs pan tile step
5. **Anchor fields** — fortress mode has no embark `zoom_cent_*`; we own anchor math

**Instrumentation (3.12.0):**

- `zoom_probe.cpp` — event ring, unique level list
- Renderer interpose: `zoom()`, `set_viewport_zoom_factor()` (log only)
- Feed: note `ZOOM_IN` / `ZOOM_OUT` (no erase yet)
- F9: `zoom probe:` block + `zoom_evt` ring
- Append flush: `dfhack-config/smoothpan/smoothpan_zoom_discovery.txt` on F9

**How to collect data:** see [ZOOM_DISCOVERY.md](ZOOM_DISCOVERY.md).

---

## Phase 1 — Input intercept + state machine

- Erase `ZOOM_IN`/`ZOOM_OUT` in feed (like `CURSOR_*` / MMB)
- `SmoothCamera` zoom state: `zoom_from_z`, `zoom_to_z`, `zoom_anim_t`
- `smoothpan_zoom_update(dt)` in logic + render (mirror MMB lifecycle)
- Replace blunt `last_zoom` reset in `camera.update()` with anchor-aware resync (stub until Phase 3)

---

## Phase 2 — Visual zoom (SDL)

- Extend `freeze_render_frac()` → `freeze_render_transform()`
- Map blits only: scale dst rect about anchor after pan shift
- Handle `RenderCopyF` entities same as pan
- `render_cell_visual` for pan shift during anim

---

## Phase 3 — Cursor-anchored zoom

- Anchor: raw SDL on map, else viewport center
- On commit: recompute `true_x/y/frac` so world under anchor is fixed
- Extend GPS mouse comp for combined pan + zoom transform (M3 in MOUSE_PLANS)

---

## Phase 4 — Polish

- Wheel debounce, queue one pending step
- `smoothpan zoom on|off`, `smoothpan zoom duration N`
- Minimap `mustmake` on zoom end; ffd full on commit

---

## Anti-patterns

| Do not | Why |
|--------|-----|
| Animate `viewport_zoom_factor` per frame | Rebake storm, layer desync |
| Scale HUD / toolbar blits | HUD wobble |
| Zoom without anchor correction | Map runs away from cursor |
| Resnapshot zoom at Present | Mid-frame drift |
| Reset `true_x/y` to integer on zoom | Kills sub-tile pan (current code) |
| Use compensated mouse for anchor | MMB lesson — use raw SDL |
| Bump `mouse_x/y` globally | Breaks UI (3.11.42) |

---

## Files (planned)

| File | Role |
|------|------|
| `zoom_probe.cpp` | Phase 0 telemetry |
| `zoom_camera.cpp` (or extend `camera.cpp`) | Anim state + commit |
| `sdl_hook.cpp` | Scale transform on map blits |
| `smoothpan.cpp` | Feed intercept |
| `renderer_hook.cpp` | Optional bake hooks during zoom |
| `mouse_comp` / `viewport` | Inverse transform for picks |

---

## Regression checklist (every phase)

- [ ] WASD smooth pan mid-sub-tile
- [ ] Middle-mouse grab-pan all directions
- [ ] Release MMB → WASD works; re-pan no jump
- [ ] Mining designation drag after pan
- [ ] UI tabs / dwarf info while panned
- [ ] Zoom in/out full ladder (phase 1+)
- [ ] Zoom during sub-tile pan (phase 2+)
- [ ] Designate / click at mid-zoom anim (phase 3+)
- [ ] F9: `zoom ladder` populated; events show feed vs renderer order

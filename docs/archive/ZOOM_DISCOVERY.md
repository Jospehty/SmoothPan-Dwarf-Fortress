# Smooth Zoom — Phase 0 Discovery Guide

**Build:** 3.12.0+  
**Behavior change:** none — observation only.

---

## What to do in-game

1. Load SmoothPan 3.12.0 (`plugin load smoothpan` / `enable smoothpan`).
2. Enter a fortress at default zoom.
3. Perform each action **2–3 times**, pausing briefly between:
   - Mouse wheel **zoom in** (over the map)
   - Mouse wheel **zoom out**
   - Keyboard zoom keys (if bound — check DF keybindings)
   - Zoom from **minimum to maximum** and back once
4. While zooming, press **F9** once (starts 30-frame telemetry dump).
5. Optional: repeat while **mid sub-tile pan** (WASD) to see interaction order.

---

## Log files

All under `{DF folder}/dfhack-config/smoothpan/`:

| File | Contents |
|------|----------|
| `smoothpan_telemetry.txt` | F9 frame dump; includes `zoom probe:` and `zoom_evt` lines |
| `smoothpan_zoom_discovery.txt` | Append-only copy of zoom probe section on each F9 |

---

## F9 lines to paste / review

```
  zoom probe: gps_z=192 levels=5 feed_in=2 feed_out=1 renderer_zoom=2 set_viewport=2
  zoom ladder: 128 160 192 224 256 (cell= z/4)
    zoom_evt frame=1234 feed_ZOOM_IN a=192 b=0
    zoom_evt frame=1234 zoom_in a=192 b=0
    zoom_evt frame=1235 set_viewport_z a=192 b=224
    zoom_evt frame=1235 gps_z_change a=192 b=224
```

### How to read events

| Tag | Meaning |
|-----|---------|
| `feed_ZOOM_IN` / `feed_ZOOM_OUT` | Key reached `viewscreen_dwarfmodest::feed` |
| `zoom_in` / `zoom_out` / … | `renderer::zoom()` called |
| `set_viewport_z` | `set_viewport_zoom_factor(nfactor)` — **a=prev, b=new** |
| `gps_z_change` | `gps->viewport_zoom_factor` changed between logic frames |

**Input path:** if wheel only shows `zoom_in` without `feed_ZOOM_*`, wheel bypasses feed keys. If both appear same frame, both paths fire.

**Ladder:** sorted unique `z` values seen — this is the discrete set smooth zoom must step through.

---

## Success criteria for Phase 0

- [ ] Ladder has **≥ 3** distinct levels documented
- [ ] Event order feed → renderer → set_viewport understood
- [ ] F7 optional: note frame ms spike on zoom step vs idle (manual)
- [ ] No regressions to pan / MMB / designation during testing

When complete, fill in the **Results** section below and proceed to Phase 1.

---

## Results (fill after testing)

| Item | Value |
|------|-------|
| Date / DF version | 2026-05-31 / Premium DF 50.x, SmoothPan 3.12.0 |
| Zoom ladder (z values) | **64, 96, 128, 160, 192, 224, 256** (7 steps, Δ32 each) |
| Cell sizes (z/4) | 16, 24, 32, 40, 48, 56, 64 px/tile |
| feed vs wheel path | **Wheel → `feed_ZOOM_IN` / `feed_ZOOM_OUT` only** (`renderer_zoom=0`) |
| Typical event order | `feed_ZOOM_*` → `set_viewport_z` (same frame) → `gps_z_change` |
| Rebake frame cost (F7, ms) | TBD (optional follow-up) |
| Notes | `set_viewport_z a=N b=N` no-ops seen before real step; intercept in **feed** for Phase 1 |

**Phase 0 status:** complete — proceed to Phase 1.

**Phase 1 status:** intercept disabled in 3.12.5 — vanilla wheel zoom; probe telemetry only.

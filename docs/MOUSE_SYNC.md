# SmoothPan — Mouse Sync (Resolved)

**Status: fixed in 3.11.34** (world + UI clicks). **Mining designation drag: fixed in 3.11.46.**

Mouse sync was the last major blocker. Visual panning (edges, HUD, zoom levels) was solid from 3.11.0 onward; input lagged behind because we were fighting the wrong problem.

---

## Production model (3.11.34+)

| Setting | Value |
|---------|--------|
| Boot mode | **`gps`** (`MouseCompMode::GpsOnly`) |
| Compensation | Bump **`gps->precise_mouse_x/y`** only during `feed` and `logic` |
| SDL mouse hook | **Off** in normal play (`mouse_comp_sdl_enabled()` false) |
| Fields never touched globally | `mouse_x`, `mouse_y`, `window_x`, `window_y` |

On `enable smoothpan`, the plugin calls `mouse_comp_boot_gps()` and stays in GPS mode. No warmup cycle, no auto `sdl → gps → both` frame dance.

**User test (3.11.46):** Pan → drag-mine designate → click dwarf sprite → switch info tabs — all correct without F8.

---

## Designation drag (3.11.44–3.11.46)

Rectangle mining/stockpile/zone paint uses **`mouse_x/y`** (text-grid index) for commit, not just `precise_mouse`. GPS-only compensation fixed single clicks and preview overlay, but drag-paint designated the wrong tiles.

**Production fix (`designation_sync.cpp`):**

| When | What |
|------|------|
| Live rectangle drag (`doing_rectangle` + valid `selection_rect.start`) | Briefly set `mouse_x/y = precise/cell` (compensated) and sync `selection_rect` end from `getMousePos()` |
| Designate paint mouse-down | Rewrite fresh `selection_rect.start` in `after_vanilla` only (no global `mouse_x` touch) |
| Render during live drag | Same sync while `map_port` fractional shift is applied |
| UI / stale drag flag | **No patch** — require live `selection_rect`; block any `IsMouseInUI_reason != 0` |

**Do not** bump `mouse_x/y` globally in `apply_mouse_compensation` (3.11.42 broke UI tabs).

---

## Why it works

DF reads the cursor in two ways:

| Field | Meaning | Used for |
|-------|---------|----------|
| `precise_mouse_x/y` | Pixel offset from renderer `origin_x/y` | World picks via `getMousePos()` = `window + precise/cell` |
| `mouse_x/y` | Text-grid index = `precise / cell` | Some UI paths |

SmoothPan shifts **map blits** by `-render_shift` at SDL time. Vanilla DF has no concept of that shift when it samples the mouse. We add `+round(render_shift)` to `precise_mouse_*` in `feed`/`logic` so designation and hover see the tile drawn under the cursor.

With the 3.11 viewport fixes (origin-based alignment, unified UI gate in `viewport.cpp`), **GPS-only compensation is sufficient for both world and UI**. The second layer (SDL `GetMouseState` hook) was a workaround for bad boot state and misaligned gates — not a permanent requirement.

---

## What was *not* the fix

Several approaches looked plausible but failed or misled diagnosis:

| Approach | Why it failed / misled |
|----------|------------------------|
| Bump `mouse_x/y` | Text-grid index; breaks UI at zoomed-out cell sizes |
| Bump `window_x/y` | Poisons paths that call `getMousePos()` when shift ≠ 0 |
| Render-phase precise bump | Corrupts UI hover every frame |
| Use `tile_pixel_x` for cell | ~14 px ≠ map cell `z/4` (~32–64) → multi-tile error |
| SDL hook **only** | Shifts both SDL and derived fields inconsistently |
| **`both` mode at boot** | SDL reinit without stable GPS state → worse than GPS alone |
| Auto warmup `sdl → gps → both` | Frame-count transitions ≠ manual F8 timing; never matched user “perfect” |
| Constant `(vp.left - origin)` term | Gap is not a multiple of cell; double-compensates |

---

## The real barrier: boot mode, not “GPS vs both”

Manual testing during 3.11.27–3.11.33 suggested a ritual:

1. Boot → broken (`sdl` or partial state)
2. **F8×1 → `gps`** → map OK, UI wrong
3. **F8×2 → `both`** → everything perfect

That pattern made it look like two layers were always required. **3.11.34 proved otherwise:** boot directly to `gps` and leave it there — world and UI both work.

Interpretation:

- The bug was **starting in the wrong mode** and cycling through modes that polluted SDL hook state.
- F8×2 “worked” partly because **`gps → both`** triggered `CleanupSDLHooks()` + `InitSDLHooks()` after the user had already been playing in GPS.
- F9 logs from “perfect” sessions often still showed `mouse=gps` — telemetry was accurate; subjective “both” did not always match the enum at capture time.

**Lesson:** For production, **one layer, correct boot state.** Debug toggles (`F8` gps↔both, `smoothpan mouse …`) remain for diagnosis only.

---

## Code locations

| File | Role |
|------|------|
| `mouse_comp.cpp` / `mouse_comp.h` | Mode enum; `mouse_comp_boot_gps()` sets `GpsOnly` |
| `smoothpan.cpp` | `apply_mouse_compensation` / `restore_mouse_compensation` in `feed`/`logic` |
| `designation_sync.cpp` | Designation-only `mouse_x/y` + `selection_rect` during live drags |
| `viewport.cpp` | `map_pick_screen_for_gate`, `mouse_gate_should_compensate` — unified UI gate |
| `sdl_hook.cpp` | SDL mouse hook (disabled when `mouse=gps`); F9 telemetry `mouse=` field |

Compensation apply rule (unchanged since 3.11.19):

```cpp
gps->precise_mouse_x += lround(render_shift_x());
gps->precise_mouse_y += lround(render_shift_y());
// save/restore around INTERPOSE_NEXT; skip when UI gate says in-UI
```

---

## Diagnostics

**F9** → `{dfhack-config}/smoothpan/smoothpan_telemetry.txt`

When mouse feels correct, expect:

```
mouse=gps
```

`mouse=both` is not required for normal play. If something regresses, compare `shift=`, `comp feed=`, and click ring `tile=(tx,ty)` against expected designation.

**Enable message (3.11.34):**

```
SmoothPan 3.11.34 enabled
Mouse: gps (map + UI). F8 only if debugging mouse layers.
```

---

## Regression checklist (golden path)

After any mouse or viewport change, re-run:

1. Enable plugin — no F8
2. Pan mid sub-tile — designate a rectangle on map → **matches preview**
3. Pan — click map sprite and dwarf info tabs — **no spurious close**
4. Repeat at **max zoom** and **one step zoomed out**
5. F9 — confirm `mouse=gps`, `desig drag=0` when not dragging

Archive one good F9 capture per release under `dfhack-config/smoothpan/`.

---

## Debug commands (optional)

| Input | Effect |
|-------|--------|
| **F8** | Toggle `gps` ↔ `both` (arms SDL hook + reinit on → both) |
| **F9** | Telemetry dump |
| `smoothpan mouse sync` / `ui` | Force `both` + SDL reinit |
| `smoothpan mouse gps` | Force GPS-only |

Do not use these in normal play unless investigating a regression.

---

## Related docs

- [STATUS_3.11.md](STATUS_3.11.md) — full project handoff and remaining minor bugs
- [MOUSE_PLANS.md](MOUSE_PLANS.md) — investigation plans (historical; superseded by this doc)
- [GOLDEN_PATH.md](GOLDEN_PATH.md) — render rules and anti-patterns
- [ARCHITECTURE.md](ARCHITECTURE.md) — hook overview

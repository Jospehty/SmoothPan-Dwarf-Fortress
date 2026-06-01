# SmoothPan — Mouse Sync: Diagnosis & Fix Plans

> **RESOLVED — see [MOUSE_SYNC.md](MOUSE_SYNC.md)** (3.11.34). Production uses **GPS-only** boot; world + UI verified at all zoom levels. The plans below are **historical** — kept for context if mouse regresses.

UI and map rendering were stable as of **3.11.26**. Mouse sync was the open problem through **3.11.33**; fixed in **3.11.34** by booting directly to `gps` and stopping mode-cycling / SDL-layer workarounds.

## Resolution (3.11.34)

| Layer | Production |
|-------|------------|
| GPS bump (`feed`/`logic`) | **On** — boot default |
| SDL hook (`GetMouseState`) | **Off** — debug only via F8 → `both` |

**Root cause of the “barrier”:** wrong boot mode and auto mode cycling, not missing SDL layer. User confirmed GPS-only works for world **and** UI at multiple zoom levels with no F8 ritual.

Full write-up: [MOUSE_SYNC.md](MOUSE_SYNC.md).

---

## What we knew during investigation (historical)

### Two compensation layers (possible interaction bug)

| Layer | Where | What it does |
|-------|--------|--------------|
| **A. SDL hook** | `sdl_hook.cpp` → `Hook_SDL_GetMouseState` | Adds `+round(render_shift)` to SDL coords when `!IsMouseInUI(x,y)` |
| **B. GPS bump** | `smoothpan.cpp` → `apply_mouse_compensation` in `feed` / `logic` | Adds `+round(render_shift)` to `gps->precise_mouse_x/y` when not in UI |

Visual pan shifts **tiles** by `-render_shift` via SDL blit offset. Picking is supposed to add `+render_shift` so the cursor still points at the tile drawn under it.

**Open question:** Does DF populate `precise_mouse_*` from hooked SDL reads (A then B = double)? Or from an internal path that bypasses A (only B matters)? We have never proven this.

### DF’s actual world pick formula (from DFHack `Gui.cpp`)

```
world_x = window_x + precise_mouse_x / cell_px   // integer division (trunc toward zero)
world_y = window_y + precise_mouse_y / cell_px
cell_px = viewport_zoom_factor / 4
```

- `window_x/y` = integer map origin (SmoothPan updates these on tile steps).
- `precise_mouse_*` = pixel offset from renderer **origin** (`origin_x/y`), not from `vp.left/top`.
- `mouse_x/y` = UI text grid only — we intentionally do **not** touch these.

### Coordinate spaces (easy to mix up)

| Space | Example | Used for |
|-------|---------|----------|
| SDL window pixels | F9 `raw_sdl=(837,1010)` | SDL queries |
| Bake origin pixels | `precise + origin` → screen pixel | UI margin tests in comp |
| Viewport rect | `vp.left/top/right/bottom` | `IsMouseInUI_reason` margins |
| Map tile index | `window + precise/cell` | Designation, hover, scroll |

The gap `(vp.left - origin_x, vp.top - origin_y)` is **not** a multiple of `cell` at Premium zoom. Any fix that adds this gap as a constant (removed in 3.11.x) caused multi-tile error.

### Existing telemetry (F9)

Already logged per frame / click:

- `raw_sdl`, `df_precise`, `df_tile`, `shift`, `origin`, `screen`, `cell`
- `comp feed=reason/calls`, `click#N` ring with `raw`, `precise`, `gap`, `fshift`, computed `tile=(tx,ty)`
- `inui` reason, widget hit rect

**Gap:** F9 does not yet log **expected world tile from first principles**, **error in tiles vs what DF actually selected**, or **which code path read the mouse**.

---

## Symptom taxonomy (split before fixing)

Mouse “feels wrong” may be several bugs. Tag each repro:

| ID | Symptom | Likely path |
|----|---------|-------------|
| M1 | Click designates wrong **map tile** (constant offset) | `precise_mouse` / `window_x` formula |
| M2 | Error **scales with pan** (worse mid-glide) | `render_shift` timing vs frozen frac |
| M3 | Error **scales with zoom** | wrong `cell_px` in one path |
| M4 | **Jumps by whole tiles** at sub-tile boundaries | integer `/ cell` vs visual sub-tile |
| M5 | Wrong only near **viewport edges** | `IsMouseInUI` margin false positive/negative |
| M6 | **Hover/tooltip** wrong but clicks OK (or vice versa) | `logic` vs `feed` vs other hooks |
| M7 | **Scroll-drag** map wrong | `mouse_scrolling_map` path bypasses comp |
| M8 | OK when still, wrong while **velocity ≠ 0** | frac frozen at `begin_render_overscan` vs read at feed |

Fix plans should target one symptom class at a time.

---

## Plan 1 — Consumer matrix (“who reads the mouse?”)

**Hypothesis:** Compensation runs in the wrong place(s) because some DF code reads mouse **before** `feed`/`logic`, or via APIs we do not hook.

**Method:**

1. Temporarily hook/log (ring buffer, not every call):
   - `SDL_GetMouseState`, `SDL_GetGlobalMouseState`, `SDL_WarpMouseInWindow` (if used)
   - `df::renderer_2d::get_precise_mouse_coords` (vtable interpose)
   - Any write to `gps->precise_mouse_x/y` (breakpoint-style log in plugin on frame tick diff)
2. On each **click** (feed with keys), dump last N hook hits with frame seq + `render_shift`.
3. Classify consumers into: **SDL-direct**, **precise field**, **mouse_x/y UI**.

**Pass:** Document ordered timeline per click:  
`[event poll] → [precise updated?] → [our feed comp] → [getMousePos used?]`

**Fail / action:** If `precise_mouse` is updated **after** our restore, or from SDL already shifted by hook A, we know to disable one layer.

**Effort:** ~1 session instrumentation. Low risk.

---

## Plan 2 — A/B compensation toggles (isolate A vs B)

**Hypothesis:** Double compensation (SDL hook + GPS bump) or wrong single layer causes multi-tile drift.

**Method:** Debug hotkey or `smoothpan mouse-mode` command with four modes:

| Mode | SDL hook | GPS bump (`feed`/`logic`) |
|------|----------|---------------------------|
| 0 | off | off (baseline — broken visually) |
| 1 | on | off |
| 2 | off | on |
| 3 | on | on (current) |

**Test protocol (same save, same zoom):**

1. Pause mid sub-tile pan (`frac ≈ 0.5`).
2. Click center of a **unique feature** (single tree, statue).
3. Record: designated tile vs expected tile under cursor.
4. Repeat at `frac ≈ 0.1`, `0.9`, and after tile step (`frac ≈ 0`).

**Pass:** One mode is clearly best (error ≤ 0 tiles at all fractions).  
**Fail:** All modes wrong → bug is not compensation magnitude but **formula / timing / cell size**.

**Effort:** Small code change + structured manual test (~30 min).

---

## Plan 3 — Ground-truth error model (make F9 conclusive)

**Hypothesis:** We can compute **expected** world tile from geometry and compare to DF’s outcome automatically.

**Model (graphics mode):**

```
// Screen pixel under cursor (compensated)
screen_x = origin_x + precise_mouse_x + comp_px
screen_y = origin_y + precise_mouse_y + comp_py

// Which map tile is drawn at that screen pixel?
// Tiles visually shifted by -render_shift on blit:
pick_px = screen_x + render_shift_x   // undo visual shift
pick_py = screen_y + render_shift_y

// Tile index relative to viewport content (approx — validate with trace):
local_x = (pick_px - origin_x) / cell_px   // integer div, match DF
local_y = (pick_py - origin_y) / cell_px

expected_world = (window_x + local_x, window_y + local_y)
```

**Method:**

1. Add to F9 / click ring: `expected=(ex,ey)`, `delta=(dx,dy)` vs tile DF actually designated (read back from `cursor` or last designation event if hookable).
2. Log **trunc vs round** variants — DF uses trunc division; compensation uses `lround(render_shift)`.

**Pass:** `delta` stable and explains symptom (e.g. always `(0,1)` near top edge).  
**Fail:** `delta` chaotic → timing or multiple readers (Plan 1).

**Effort:** Medium — mostly logging + one designation readback hook.

---

## Plan 4 — Frame timeline / temporal skew

**Hypothesis:** `render_shift` at **click time** ≠ `render_shift` used for **blits** in the same frame (frac frozen in `begin_render_overscan`, mouse read in `feed` before/after render).

**Method:**

1. Log per frame (single line):  
   `frame_seq`, `frac`, `render_shift`, `window_x/y`, time of `freeze_render_frac`, `feed`, `logic`, `RenderPresent`.
2. On click, assert: `shift_at_feed` vs `shift_at_last_present` vs `shift_in_snapshot`.
3. Test M8 specifically: pan with held key vs tap-step.

**Pass:** Identified skew > 1px correlates with M2/M8. Fix by reading shift from same snapshot used for blits (`last_snapshot` at feed time).  
**Fail:** Shifts match → not a timing bug.

**Effort:** Low — extend existing `frame_seq` / snapshot.

---

## Plan 5 — UI gate audit (edge false positives)

**Hypothesis:** `IsMouseInUI` / widget tests use **different coordinates** in SDL hook vs GPS bump, so one path compensates on map and the other skips (or reverse).

**Known asymmetry today:**

- SDL hook: `IsMouseInUI(*x,*y)` on **raw SDL coords**
- GPS bump: `IsMouseInUI_reason(precise + origin_x, precise + origin_y)`

These are only equivalent if `raw_sdl == precise + origin`. F9 line `raw_sdl` vs `df_precise` + `origin` should prove equality; if not, unify before any shift math.

**Method:**

1. F9 assert: `raw_sdl == origin + precise` (±0).
2. Log mismatches where SDL hook and feed disagree on `inui`.
3. Re-test M5 along `vp.left`, `vp.top`, especially upper-left gap `(0..vp.left)`.

**Pass:** Single unified `map_pick_pixel(raw_x, raw_y)` used by both layers.  
**Fail:** Gates correct → look elsewhere.

**Effort:** Low.

---

## Plan 6 — DF source trace (static truth)

**Hypothesis:** We compensate the wrong field because DF’s Premium renderer uses a different pick path (widgets, `getDepthAt`, `screentexpos`).

**Method:**

1. Trace in DF Premium / DFHack headers:
   - `get_precise_mouse_coords` implementation (`renderer_2d`)
   - Enabler event loop: order of SDL poll → `precise_mouse` update
   - `viewscreen_dwarfmodest::feed` / designation handler: which mouse API
2. Build call graph for: **left click designate**, **mouse look hover**, **middle drag scroll**.

**Deliverable:** One-page “pick pipeline” diagram tied to real function names.

**Pass:** Identifies a reader we never compensate.  
**Effort:** Medium — reading only, no binary patching.

---

## Plan 7 — Visual calibration overlay (human ground truth)

**Hypothesis:** Numeric logs lie or use wrong space; eyes + grid don’t.

**Method:**

1. Debug overlay (one frame, semi-transparent):
   - Crosshair at compensated pick pixel
   - Grid of **expected** tile boundaries using `origin`, `cell`, `render_shift`
   - Text: `world tile`, `frac`, `shift`, `comp mode`
2. Toggle overlay with hotkey during live pan.
3. Compare crosshair to tile under cursor feature.

**Pass:** Overlay aligns → bug is in DF downstream; misaligns → bug in our model (Plan 3).  
**Effort:** Medium-high — SDL draw or blit debug rects.

---

## Plan 8 — Architectural fix candidates (after diagnosis)

Do **not** implement until Plans 2–3 identify root cause. Options on the shelf:

### 8a — Single compensation point (preferred if double-comp confirmed)

- Disable SDL mouse hook **or** GPS bump, not both.
- Rule: compensate where DF **actually** reads for world pick (`precise_mouse` only, per `Gui::getMousePos`).

### 8b — Match DF integer semantics

- Use `floor`/`trunc` consistently with DF’s `/ cell_px` (may require compensating in **tile space** then converting back to pixels).

### 8c — Pick in unshifted space (inverse of render)

- Stop shifting pick; instead derive tile from `(precise + shift) / cell` with shift defined as exact negative of blit shift (including overscan term if active).

### 8d — Viewport bake shift mode for mouse

- If mouse error tracks SDL blit shift but not viewport `screen_x/y` shift, unify on one shift mode for both render and pick (ShiftMode coupling).

### 8e — Scroll / drag separate path

- M7 fix: hook the specific scroll handler rather than global mouse (avoids breaking UI).

---

## Recommended order of execution

```
Phase 0 (1 hour)     Symptom tagging M1–M8 on current 3.11.26 build
Phase 1 (2 hours)    Plan 5 — prove raw_sdl vs origin+precise; unify UI gate
Phase 2 (2 hours)    Plan 2 — A/B toggles; identify best single layer
Phase 3 (3 hours)    Plan 3 — expected tile + delta in F9 click ring
Phase 4 (parallel)   Plan 1 or 6 — consumer matrix / DF source trace
Phase 5 (if needed)  Plan 4 (timing) or Plan 7 (overlay)
Phase 6              Implement one of 8a–8e with proof from Phase 3
```

**Stop rule:** Do not merge another “try ±N pixels” change unless F9 shows `delta` improving toward `(0,0)` on a fixed test case (same save, zoom, click coords).

---

## Minimal regression test (golden path)

When any mouse fix lands, require:

1. **Still pan:** designate center-map feature at `frac=0, 0.25, 0.5, 0.75` — error 0 tiles.
2. **Edge:** click at `vp.left + 2*cell`, `vp.top + 2*cell` — error 0 tiles.
3. **UI:** click toolbar — no compensation (`inui≠0`, no drift).
4. **Scroll:** middle-drag pan — cursor stays under grabbed point (if M7 in scope).
5. **Zoom:** repeat at default and one step zoomed in/out — error still 0 (M3).

Record F9 for one full pass; archive in `dfhack-config/smoothpan/`.

---

## Relation to rendering work

Mouse sync is **orthogonal** to blit classification / HUD pinning. Rendering fixes changed `render_shift`, `origin`, and post-viewport compositing — any of those can invalidate an old mouse tweak without the mouse code changing.

After rendering changes, always re-baseline Plan 3 `delta` before new mouse patches.

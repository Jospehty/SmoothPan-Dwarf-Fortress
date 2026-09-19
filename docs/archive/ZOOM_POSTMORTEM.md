# Smooth Zoom Postmortem (3.12.0 → 3.19.3)

**Status:** Smooth wheel zoom **removed in 3.20.0**. SmoothPan is **pan-only** again; mouse wheel uses vanilla DF discrete zoom.

This document captures everything learned from discovery, implementation attempts, and the final F9 session on **3.19.3** so a future retry does not repeat the same loops.

---

## Executive summary

We tried to add visual-only SDL scale on map blits while vanilla commits discrete `viewport_zoom_factor` steps (Δ32: 64…256). Commits eventually worked (`miss=0`), but:

1. **No visible easing** — telemetry showed `stretch=1 scale=114` while **`frozen=100`** during anim frames; blits still drew at 1.0 scale.
2. **Large edge voids** — zoom-out anim frames had `void_r` 300–631 px; zoom-in had `void_px` 667–1058.
3. **One-frame anim at fast scroll** — `t=0` only; too brief to perceive even if scale had applied.
4. **Net feel** — vanilla snap zoom plus extra seams/jagged bands; not worth shipping.

**Root constraint:** DF bakes the map into a fixed viewport buffer. Scaling blits without a **margin ring / overscan bake** leaves missing texels at edges (black bands). Pan solved this with tile overscan + edge stretch; zoom needs an analogous **Phase R** path (see [PLAN_SMOOTH_ZOOM.md](PLAN_SMOOTH_ZOOM.md), [INVESTIGATION_ZOOM_AND_EDGES.md](INVESTIGATION_ZOOM_AND_EDGES.md)).

**Do not retry:** texture freeze via `ReadPixels` on zoom-out (crashes in 3.16.x experiments).

---

## F9 evidence — 3.19.3 (user session, May 2026)

Log: `{DF}/dfhack-config/smoothpan/smoothpan_telemetry.txt`  
Build: `SMOOTHPAN_3.19.3`, 30-frame F9 dump while panning + wheel zoom.

### At rest (between zoom steps)

| Field | Typical value | Meaning |
|-------|---------------|---------|
| `anim=0` | — | No active ease |
| `gps_z` = `baked` = `target` | e.g. 256 | Logic and bake aligned |
| `void_l/r` | ~20 px | Baseline pan edge gaps (acceptable) |
| `stretch=0` | — | Pan-only edge extend |

### Zoom-out commit frame (example: 256 → 224)

```
anim=1 dir=-1 gps_z=224 baked=224 prev=256 dgps=-32
t=0 scale=114 frozen=100 s0=114 s1=100 stretch=1 hitch=2 miss=0
VoidScan: void_left=14 void_right=300
```

- **Commit succeeded:** `dgps=-32`, `miss=0`, gps matches baked level.
- **Anim state existed:** `scale=114` (114% of baked = visual bridge from old size).
- **SDL blits did not use scale:** `frozen=100` — the value passed to `map_blit_apply_pan_and_zoom` stayed 1.0.
- **Seam:** 300 px void on the right during the anim frame.

Similar zoom-out steps: `void_r` 329, 511, 631 at z=192, 128, 96.

### Zoom-in commit frame (example: 96 → 128)

```
anim=1 dir=1 gps_z=128 baked=128 prev=96 dgps=32
t=0 scale=75 frozen=100 s0=75 s1=100 stretch=1 miss=0
VoidScan: void_px=1058 void_left=16 void_right=871
```

Zoom-in produced even larger total void pixel counts (818–1058 px bands).

### Pattern across all wheel steps

| Observation | Detail |
|-------------|--------|
| Commits | Clean after 3.19.2 fix (`miss=0`, `dgps=±32`) |
| Anim duration | Often **one frame** (`t=0` only) at 60 FPS + fast scroll |
| Scale application | **`frozen=100` always** during `stretch=1` frames |
| Visual result | Vanilla discrete zoom + extra edge gaps |
| Pan during session | Unaffected (`shift` stable, overscan=0 in SDL mode) |

---

## Attempted approaches (chronological)

### Phase 0 — Discovery (3.12.0+)

- **What:** `zoom_probe.cpp`, renderer hooks on `set_viewport_zoom_factor` / `zoom()`, feed logging for `ZOOM_IN`/`ZOOM_OUT`.
- **Result:** Ladder confirmed: z ∈ {64,96,128,160,192,224,256}, Δ32. Input path is **feed keys**, not `renderer::zoom()` alone.

### 3.18.0–3.18.1 — Deferred commit + scale on old bake

- **What:** Hold gps commit; SDL scale old bake toward new level.
- **Result:** Commits failed or wheel events swallowed; rebake timing fought feed order.

### 3.18.2 — Vanilla feed inject commit

- **What:** Pass wheel to vanilla; inject commit after feed.
- **Result:** Commits landed but frame timing wrong; hitch and snap-back.

### 3.18.3 — Same-frame `renderer::zoom()` commit

- **What:** Call vanilla zoom from render hook.
- **Result:** **Broken** — `zoom()` no-op in fortress mode path we hit.

### 3.18.4–3.18.5 — Feed inject + pending bridge scale

- **What:** Queue step; bridge scale between baked and target z.
- **Result:** Better; still hitch; occasional snap-back frames.

### 3.19.0 — Commit-first (ease on **new** bake)

- **What:** Let vanilla commit first; animate scale on freshly baked texture.
- **Result:** Felt like vanilla + seams; animation one frame late relative to rebake.

### 3.19.1 — Pre-arm on feed

- **What:** Start anim **before** vanilla runs, anticipating commit.
- **Result:** **Bounce-back bug** — anim ran without gps change (`miss` implicit). User stuck at same z while visual eased; 200+ `feed_ZOOM_OUT` with no `set_viewport` after first commit. **Do not pre-arm without confirmed commit.**

### 3.19.2 — Confirm commit after vanilla feed/render

- **What:** Removed pre-arm; detect gps change after vanilla.
- **Result:** Commits reliable (`miss=0`). Still no visible smoothing.

### 3.19.3 — Sync scale at first map blit (`smoothpan_zoom_prepare_map_blit`)

- **What:** Call prepare hook before blit classification to copy anim scale into frozen render state.
- **Result:** **Incomplete** — F9 still showed `frozen=100` during `stretch=1`. Scale sync did not reach actual `RenderCopyF` dst rects effectively.

### Abandoned earlier (3.13.x — see INVESTIGATION)

- SDL scale without margin → viewport buffer underrun → black edges.
- `dim` / viewport resize hacks → crashes.
- ReadPixels texture hold on zoom-out → instability/crash.

---

## Why pan works but zoom did not

| Mechanism | Pan | Zoom |
|-----------|-----|------|
| Logic change | Integer tile step rare | Every wheel notch changes `viewport_zoom_factor` + full rebake |
| Visual trick | Fractional shift within same bake | Needs scale ≠ 1 on same bake |
| Edge fill | Overscan −1 tile + last-col stretch | Scale shrinks/grows blits → gaps unless bake has margin |
| Timing | `begin_render_overscan` before render; shift frozen for frame | Rebake mid-frame; anim starts at render entry, often after gps already changed |
| Clip | Origin-grid clip stable | Scale needed clip bypass (null clip) → bleed risk |

Pan golden path (3.14.x / v1.2.0): float `RenderCopyF` shift, edge stretch, origin-grid clip, `restore_window_overscan()`, GPS mouse sync — **unchanged in 3.20.0**.

---

## Appendix — 3.20.0 pan-only: black regions on vanilla zoom (F9, May 2026)

**Report:** With smooth zoom removed (wheel = vanilla), stepped zoom still shows large black unrendered bands at viewport edges. Not present with SmoothPan disabled. F9 captured while zooming out.

**Not fixed in 3.20.0** — documented for future work. Likely pan infrastructure, not zoom anim code.

### F9 facts (shift=0, no pan active)

| Frame | gps_z | cell | vp.right | rightContentEnd | rightGap | Notes |
|-------|-------|------|----------|-----------------|----------|-------|
| 30 | 192 | 48 | 2619 | 2559 | −59 | Steady; ~20–60 px baseline edge slack |
| 28 | 192→160 | 40 | 2187 | 2553 | **+367** | Commit frame: viewport shrunk, blits still wide |
| 25 | 128 | 32 | 2049 | 2553 | **+505** | Same pattern |
| 23 | 96 | 24 | 1929 | 2553 | **+625** | Same pattern |
| 20 | 64 | 16 | 1734 | 2553 | **+820** | Same pattern |
| 9 | 160 | 40 | 3209 | 2559 | **−649** | Viewport wider than drawn content |
| 11 | (wide) | 24 | 3889 | 2559 | **−1329** | Huge interior void vs theoretical vp.right |

`rightGap = rightContentEnd − (vp.right − 1)`. Negative = content ends before viewport right edge (black band inside viewport). Positive = content extends past new viewport (stale bake width).

`origin_x` stayed **6** while `vp.left` jumped **27 → 1 → 9 → 22 → 49** during the wheel sequence. `shift=0`, `overscan=0`, `mode=sdl` throughout.

### Likely mechanism (strong hypothesis, not proven in isolation)

Three pan-only systems interact badly with vanilla zoom rebakes:

1. **Origin-grid SDL clip** (`smoothpan_apply_map_clip`) — clip is `(origin_x, origin_y, vp.right−vp.left, vp.bottom−vp.top)`, **not** anchored at `(vp.left, vp.top)`. Designed so sub-tile pan shifts do not bleed into the top-left HUD margin. When vanilla zoom moves `screen_x/screen_y` and cell size in the same frame, the clip box and on-screen viewport rect **slide relative to each other** for 1+ frames.

2. **Viewport vs blit extent desync on commit frames** — `get_strict_viewport_rect()` updates immediately from new `viewport_zoom_factor`, but map `RenderCopyF` destinations on the commit frame still cover the **previous** pixel width (~2553 px in this capture). Telemetry shows `rightContentEnd` flat while `vp.right` steps down → exactly the void/seam pattern seen in 3.19.x smooth zoom, but **without any zoom anim or SDL scale**.

3. **Edge extend is pan-gated** — last-column/row stretch only runs when `|render_shift| ≥ 0.5`. At `shift=0` (wheel only), nothing fills the trailing gap when rebake geometry changes. Pan hides this because overscan + edge stretch run during motion; zoom at rest does not.

This explains why vanilla DF alone is fine (no origin clip, no shift hook) and why the bands scale with zoom step size, not with smooth-zoom `scale`/`frozen`.

### What this implies for a future smooth zoom

- Any zoom solution must treat **zoom commit frames** like a pan edge case: rebake width, clip rect, and `origin_x`/`vp.left` delta must be consistent **on the first blit** after `set_viewport_zoom_factor`.
- Phase R margin ring alone may not be enough if origin-grid clip still uses pre-zoom alignment during the commit frame.
- Useful regression test: **3.20.0 + F9 while wheel zoom only (no pan)** — track `rightGap`/`leftGap`; target same ±20 px baseline as idle pan at each z.
- Confirm with `plugin unload smoothpan` + same F9: if gaps vanish, hypothesis confirmed.

### Quick repro checklist

1. Load 3.20.0, fortress view, **do not pan** (`fx=fy=0`).
2. Wheel zoom out several notches; F9 during motion.
3. In telemetry: `EdgeProfile` rows — look for `|rightGap| > 60` or `|leftGap| > 30` on commit frames.
4. Compare unload smoothpan → repeat; gaps should disappear if pan hooks are the cause.

---

## Appendix B — Deep investigation (3.20.0 F9 parse, May 2026)

Automated parse: `tools/parse_f9_zoom.py` on the user’s zoom-out F9 capture.

### The alternating frame pattern

Every vanilla zoom step produces **two viewport geometries** for the same `gps_z`:

| Type | vp width | Typical `#map` blits | max map x | User-visible |
|------|----------|----------------------|-----------|--------------|
| **NARROW** | `dim_x × cell` (logical rebake size) | 0–85 | ≪ vp.right | **Black bands** |
| **WIDE** | ≈2560 px (screen-fill layout) | 4500–22000 | ≈ vp.right | Mostly OK (~20–60 px edge slack) |

Example sequence zooming 192→160→128 (F9 frames 30→26):

```
 Fr   z  cell   vpW  dimX  #map   type    rGap   maxMx
 30  192    48  2592  54.0  3140   WIDE    -59    2598   ← steady
 28  160    40  2160  54.0    85  NARROW  +367     534   ← broken commit frame
 27  160    40  2560  64.0  4566   WIDE     -7    2566   ← recovery
 26  128    32  2048  64.0    40  NARROW  +505     646   ← broken
 25  128    32  2560  80.0  6893   WIDE    -15    2566   ← recovery
```

**NARROW frames are the smoking gun:** frame 28 has only **85 map blits** (vs 3140 on a good frame). Map tiles stop around x≈534 while `vp.right=2187` — ~1650 px of viewport never receives map texels.

Frame 21 (z=64 NARROW): **0 map blits**, `rightGap=+820` — effectively a black map pass.

### Root cause (high confidence)

Vanilla zoom is **not atomic**. Across 2+ frames per wheel notch:

1. `viewport_zoom_factor` and `cell_size` update.
2. `main_viewport->screen_x/y` and effective `dim_x` oscillate between **logical rebake** (NARROW) and **screen-fill** (WIDE) layouts.
3. SmoothPan reads `get_strict_viewport_rect()` **every frame** and immediately applies:

```157:159:src/sdl_hook.cpp
    SDL_Rect clip = { vp.origin_x, vp.origin_y,
                      vp.right - vp.left, vp.bottom - vp.top };
    True_SDL_RenderSetClipRect(r, &clip);
```

4. On NARROW commit frames, clip width shrinks **before** the renderer finishes blitting the full tile grid. The SDL clip hook also **re-snaps** any DF clip to the same origin rect during `update_full_viewport`:

```287:302:src/sdl_hook.cpp
        if (g_in_main_viewport_update.load(std::memory_order_relaxed) && sdl_shift_mode_active()) {
            ViewportRect vp;
            if (get_strict_viewport_rect(&vp)) {
                SDL_Rect snapped = { vp.origin_x, vp.origin_y,
                                     vp.right - vp.left, vp.bottom - vp.top };
                ...
                return True_SDL_RenderSetClipRect(renderer, &snapped);
```

5. Thousands of would-be map blits are **clipped away** or never issued → black unrendered regions on the right/bottom.

Vanilla DF does not install this clip. Rebake completes on the next WIDE frame, so vanilla looks like a single crisp step; with SmoothPan the NARROW frame is visibly broken.

This is **orthogonal to smooth zoom anim** — it is the pan-era **origin-grid clip** interacting with vanilla’s multi-frame rebake.

### Geometry detail: origin vs screen viewport

On stable WIDE frames at z=192:

- `vp.left=27`, `origin_x=6` → **21 px left margin** (matches baseline `leftGap=21`)
- Clip ends at `origin_x + width = 6 + 2592 = 2598` while `vp.right=2619` → **21 px right margin** (`rightGap≈−59` includes UI chrome in EdgeProfile scan)

The clip is deliberately **not** aligned to `(vp.left, vp.top)` — correct for idle pan, wrong when `vp.left` jumps during zoom.

### What does *not* explain it

| Ruled out | Evidence |
|-----------|----------|
| Zoom anim / SDL scale | 3.20.0 has none; `shift=0`, `frozen=100` N/A |
| Pan overscan | `overscan=0`, `mode_sdl` all frames |
| Edge extend | Requires `\|shift\| ≥ 0.5`; never triggers at rest |
| Wrong mouse comp | `sdl_match=1` throughout |

### WIDE frames with huge negative rightGap

Some WIDE frames have `vp.right` **larger than the monitor** (e.g. 3889 px at z=96, frame 12) with **0 map blits** and `rightGap=−1329`. Here `get_strict_viewport_rect()` overshoots while rebake has not started — same clip starvation class, opposite direction (viewport rect too large, nothing drawn into it).

### Likely fix directions (future, not implemented)

1. **Zoom transition gate** — on `gps_z` change, disable `smoothpan_apply_map_clip` and the clip re-snap hook for 1–2 frames (or until `dim_x×cell` stabilizes).
2. **Clip to union** — during zoom, clip = bounding box of previous + current viewport (prevents premature narrowing).
3. **Clip in screen space** — use `(vp.left, vp.top, width, height)` instead of origin anchor when `|vp.left − origin_x|` changes frame-to-frame (detect zoom vs pan).
4. **Validation** — re-run `parse_f9_zoom.py`; success = no NARROW frame with `#map < 1000` or `map_shortfall > 100`.

### Confirmation experiment

```
plugin unload smoothpan
# F9 while zooming out — parse should show no NARROW/WIDE alternation with #map≈0
plugin load smoothpan
# same — NARROW frames should reappear
```

---

## Code removed in 3.20.0

- `zoom_camera.cpp/h` — state machine, feed intercept, SDL scale, void metrics
- Feed wheel erase / queue / after_vanilla hooks
- `map_blit_apply_pan_and_zoom` scale branch → **`map_blit_apply_pan_shift`** (pan only)
- Zoom clip bypass, zoom edge-extend branches, VoidScan F9 block
- `smoothpan zoom on|off` — command prints removal notice

**Kept for future discovery:**

- `zoom_probe.cpp` — renderer/feed telemetry, F9 `zoom_probe:` block, `smoothpan_zoom_discovery.txt` flush

---

## Recommended retry direction (when revisiting)

1. **Phase R — margin ring:** Trace vanilla rebake allocation; bake N pixels/cells extra; scale within margin so edges never sample outside texture (see PLAN Phase R).
2. **Same-frame scale:** Start anim **before or during** first map blit of commit frame, not at render hook entry after gps change. Possibly hook first `update_full_viewport` blit, not `camera.update()`.
3. **Do not pre-arm** without confirmed `dgps ≠ 0` on that frame.
4. **Consider zoom-out-only ease** first (scale ≤ 1 on old bake) or accept vanilla timing until margin path exists.
5. **Multi-frame anim:** Minimum display time or dt-based t independent of rebake cadence.
6. **Validation:** F9 must show `frozen != 100` during anim **and** `void_r/l < pan baseline (~20px)** before shipping.

---

## References

- [PLAN_SMOOTH_ZOOM.md](PLAN_SMOOTH_ZOOM.md) — original phased plan (parked)
- [INVESTIGATION_ZOOM_AND_EDGES.md](INVESTIGATION_ZOOM_AND_EDGES.md) — buffer constraint write-up
- [ZOOM_DISCOVERY.md](ZOOM_DISCOVERY.md) — how to collect probe data
- [GOLDEN_PATH.md](GOLDEN_PATH.md) — pan regression checklist

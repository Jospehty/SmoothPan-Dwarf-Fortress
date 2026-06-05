# Smooth Zoom — Master Plan & Handoff

**Audience:** the engineer/model implementing smooth zoom on top of the stable
SmoothPan pan foundation. This is the single source of truth for the strategy.
Read this top-to-bottom before touching code. It supersedes the older parked
plan in `PLAN_SMOOTH_ZOOM.md`; read `ZOOM_POSTMORTEM.md` for what already failed.

**Author of plan:** opus-4.8 session, build 3.20.0 → 3.21.0+.
**Status at time of writing:** Stage 1 SHIPPED & TESTED. **Theory 1 (clip
starvation) was REFUTED by the 3.21.0 F9 capture.** Read §0.5 FIRST — it changes
everything and contains the exact next-step checklist.

---

## 0.5. ⭐ CURRENT STATE & THE PERFECT CHECKLIST (read this first) ⭐

### **3.23.6 SHIPPED & USER-CONFIRMED STABLE (2026-06-04).**

User report: *"even after panning around it's actually pretty solid (not
smooth, nor perfect with these slight presumably dim lag related artifacts
that you've been able to spot in the f9, but it's better than it was before)."*

**Live build:** 3.23.6 with both 3.23.0/3.23.2 and 3.23.5 fix attempts
kill-switched behind `if (false && ...)`. Per-blit instrumentation in
`sdl_hook.cpp` is active. Parser is at 3.23.6 with main-viewport coverage
analysis. New `tools/check_dim_lag_region.py` BMP pixel sampler shipped.

**Remaining artifact (user-acceptable):** the dim-lag is a **cell-size
mismatch strip** at the viewport edge. Blitter uses `dispx_z` (OLD from
previous z) → tiles at OLD cell. Main vp uses `zf/4` (NEW) → tiles at NEW
cell. At z=224 NARROW (the worst case the user typically hits), the strip
is 320px right + 184px bottom = ~12% of screen. "A few little unrendered
artifact lines" — user-visible but not blocking. At deeper zoom (z=96,
z=64) the strip grows to 815px + 443px (52.8% screen), but user doesn't
typically zoom that deep.

**Dim-lag mechanism (verified by 3.23.6 per-blit log, see §9 3.23.6 entry):**
- blitter blits 24x24 (OLD z=96) tiles in dim-lag region next to main
  vp's 16x16 (NEW z=64) tiles
- 11260 vmap=1 24x24 blits in z=64 NARROW, 3536 in the right dim-lag strip
- blitter IS rendering — the dim-lag is a cell mismatch, not unrendered
- user's "unrendered" perception = tiles look 50% bigger in the dim-lag

**Option 3.24.0 (deferred, design ready but not implemented):** intercept
main vp blits (vmap=1 + vpscr matches main_vp), override their `dst.w/h`
to use NEW cell (`zf/4`). Skips UI/radar blits. Made possible by 3.23.6
per-blit log giving us the exact filter. Not currently needed because
user accepts the current state.

### What we now KNOW (hard data, 3.21.0 zoom-out F9 capture)

The Stage-1 clip fix is built, shipped, and confirmed harmless — but **it does
NOT fix the black bands**, because the bands were never caused by our clip.

Parser output from the user's zoom-out sweep (256→64), gated clip:
```
 Fr   z cell   vpW  dimX  rGap  #map zt cstr clipOut   type
 30 224   56  2576  46.0   -38  2135  0    1       0   WIDE   <- steady, clean
 27 192   48  2208  46.0   330   163  0    1       0 NARROW   <- BLACK BAND (BMP confirms)
 26 160   40  2560  64.0    -6  4296  8    0       0   WIDE   <- clean
 25 128   32  2048  64.0   512   122  7    0       0 NARROW   <- BLACK BAND
 24 128   32  2560  80.0   -14  6784  8    0       0   WIDE
 23  96   24  1920  80.0   632    13  7    0       0 NARROW   <- BLACK BAND
 21  64   16  1712 107.0   827     0  7    0       0 NARROW   <- 0 map blits!
 20  64   16  2560 160.0   -54 22641  8    0       0   WIDE
```

**Proven facts:**
1. `clipOut=0` and `clip_set=0` on EVERY frame, including the banded NARROW ones.
   Our origin clip discards nothing. **Theory 1 (clip starvation) = DEAD.**
2. The band correlates perfectly with `vpW < screen_width` (NARROW). On a NARROW
   frame the map content genuinely ends at `vpW` (e.g. 2208) and the rest of the
   2560-px screen is **unbaked black**. `#map` collapses (163, 122, 13, **0**).
3. **Root cause = `dim_x` lag.** On a zoom step `cell` shrinks immediately
   (224→192 ⇒ 56→48) but `dim_x` stays at the OLD value for ONE present
   (stays 46), so `vpW = dim_x*cell` shrinks below the screen width. The NEXT
   present vanilla regrows `dim_x` (46→64) to refill the screen ⇒ WIDE/clean.
   So every zoom commit emits one NARROW (banded) present then one WIDE present.
4. frame 27 BMP: clear black band on the **right and bottom** ~13% of the map
   area; HUD/minimap fine; map content correct but doesn't reach the screen edge.

**Caveats about this capture (don't over-trust beyond the above):**
- It was taken mid-pan (`fx=0.731 shift≈40px`), not the clean stand-still C1, so
  `cstr=1` on steady frames. Irrelevant to the conclusion (clipOut=0 regardless).
- **Only ONE of the four requested captures survived.** F9 does `remove()` on the
  telemetry each press and the user didn't rename between runs, so legacy/zoom-in/
  pan-zoom all overwrote each other. We never got the legacy-vs-gated A/B nor the
  plugin-UNLOADED baseline. **Getting those is step 1 below.**
- The 30-frame file was 64 MB because of per-blit `Blit:` lines. See step 0.

### Was Stage 1 wasted? No.
Keep the gated clip + transition window — it is correct hygiene (the clip should
not be forced when not panning) and it is provably harmless (`clipOut=0`,
pan unaffected). It also gave us the `ZoomDiag`/`ClipStarve` telemetry that
refuted Theory 1. We just learned the bands are a **different** problem:
**vanilla's one-frame `dim_x` regrow lag on zoom commits.**

### Why does vanilla-ALONE (plugin unloaded) reportedly NOT show bands?
The postmortem claims unloaded DF has no bands. We have NOT re-confirmed this on
this machine. Two possibilities; **the unloaded baseline (step 1) decides which:**
- **P-lag-is-ours:** something SmoothPan does (writes `window_x/y` +
  `force_full_display_count` in `camera.update()` before vanilla render; or the
  `SDL_RenderPresent` hook’s timing) makes the NARROW frame get presented when
  vanilla would have coalesced it. If so, the fix is to stop perturbing the
  commit frame (e.g. skip our `window_x/y`/ffd writes during `ztrans>0`, or don’t
  freeze/shift on the commit present).
- **P-lag-is-vanilla:** vanilla also emits the NARROW present but clears to the
  map background or re-presents, so it’s invisible; our pipeline makes it black.
  Then the fix is to force `dim_x` to refill on the commit frame, or to hide the
  NARROW present.

### THE PERFECT CHECKLIST — from here to working smooth zoom

Work top to bottom. Do not skip the data steps; every past failure came from
coding ahead of evidence. After each build run `deploy.ps1` and have the user
test; you cannot run DF yourself.

**STEP 0 — make captures small & non-overwriting (do this FIRST, ~30 min).**
- [ ] In `sdl_hook.cpp` `Hook_SDL_RenderPresent`, gate the per-blit `Blit:` lines
      behind a flag that is OFF for zoom captures (they bloat the file 1000×; the
      parser can use `ClipStarve map_total` instead of counting `Blit:` lines).
- [ ] Add a console command `smoothpan zoomcap <label>` (or make F9 append a
      counter/timestamp) so successive captures write to DISTINCT files
      (`smoothpan_zoom_<label>.txt`) and never overwrite. Update `parse_f9_zoom.py`
      `path` or make it take argv[1].
- [ ] Teach `parse_f9_zoom.py` to fall back to `map_total` (from ClipStarve) when
      no `Blit:` lines are present.

**STEP 1 — get the three MISSING baselines (decisive data).**
For each, stand STILL (no WASD, `fx≈0`), hover map center, capture, then wheel
out ~5 notches; save to a distinct file:
- [ ] **B-unloaded:** `plugin unload smoothpan`; zoom out; record a screen capture
      / phone photo of any banded frame (no F9 possible unloaded). Question to
      answer: *does vanilla alone show bands?* → picks P-lag-is-ours vs -vanilla.
- [ ] **B-legacy:** `enable smoothpan; smoothpan clip legacy`; F9; zoom out. Save.
- [ ] **B-gated:** `smoothpan clip gated`; F9; zoom out. Save.
  Compare `clipOut` legacy vs gated (expect both ≈0 → confirms clip irrelevant)
  and confirm bands appear in both (→ confirms dim-lag, not clip).

**STEP 2 — instrument the dim-lag precisely (cheap, high value).**
- [ ] In the renderer hook (`update_full_map_port` / `update_full_viewport`) log,
      per call during `ztrans>0`: `gps->viewport_zoom_factor`, `main_viewport->
      dim_x/dim_y/screen_x/screen_y`, `main_map_port->dim_x/dim_y`, and whether
      `update_full_map_port` was even called this frame. Add these to the F9 dump
      as a `DimTrace:` line.
- [ ] Goal: pin down WHICH field lags and WHICH vanilla call regrows it. Look in
      the DFHack-generated headers for `df::renderer_2d` and `df::graphic_map_portst`
      / `df::graphic_viewportst` (in `D:\dfhack\build\...\df\` or the codegen): find
      the method that sets `dim_x = floor(screen_pixels / cell)` (a "reshape" /
      "compute_dim" / resize on zoom). `set_viewport_zoom_factor` is interposed in
      `renderer_hook.cpp` — check what it does vs. what recomputes dim.

**STEP 3 — fix the commit-frame band (pick based on STEP 1 result).**
- [ ] If **P-lag-is-ours**: during `ztrans>0` (the transition window already
      exists), suppress the perturbation — e.g. skip `freeze`/shift and skip
      `window_x/y`+`force_full_display_count` writes on the commit present, or
      re-present the previous frame. Re-capture; bands should vanish.
- [ ] If **P-lag-is-vanilla**: force the dim regrow on the commit frame. Safest is
      to find and call vanilla’s own reshape (it reallocs `screentexpos_*`); do NOT
      hand-mutate `dim_x` without the realloc (that crashed in 3.13.6 — see
      ZOOM_POSTMORTEM). Alternative stopgap: detect NARROW
      (`vpW < screen_w - cell` while `ztrans>0`) and skip presenting that frame.
- [ ] **PASS criteria:** parser shows NO NARROW frame with `#map` collapse /
      `rGap > 60`; banded BMP gone; pan + mouse regression (GOLDEN_PATH) intact.
- [ ] Ship as 3.22.0. This is "Stage 1 done for real."

**STEP 4 — smooth zoom-OUT easing (Stage 2). See §4 Stage 2 for full detail.**
- [ ] Plumb `render_zoom_scale` into `freeze_render_frac()` (it is hard-pinned to
      1.0 today — THE `frozen=100` bug). Add zoom-anim state.
- [ ] On `gps_z` decrease, ease scale from `old_cell/new_cell (>1)` → `1.0`,
      center-anchored, on the NEW bake. Scale stays ≥1 ⇒ subset of baked texels ⇒
      no margin ring (the core insight, §3). Disable edge-extend while scaling;
      use screen-space clip during anim.
- [ ] PASS: F9 shows `frozen != 100` during anim; `rGap` ≤ ~20px every anim frame;
      no bands; pan/mouse intact. Ship 3.23.0.

**STEP 5 — smooth zoom-IN easing (Stage 3). See §4 Stage 3.**
- [ ] De-risk the programmatic commit first via a `smoothpan zoomtest in` command
      that calls `set_viewport_zoom_factor(next)` and confirm a clean step.
- [ ] Erase `ZOOM_IN` in feed; ease scale `1.0 → new/old (>1)` on the OLD bake;
      commit at anim end; collapse rapid notches by retargeting. PASS as in Stage 2.

**STEP 6 — polish (Stage 4):** mouse-comp inverse transform during anim (freeze
comp meanwhile), optional cursor anchor, `smoothpan zoom on|off|duration`,
minimap/ffd on commit, full regression matrix. Ship v1.3.0.

### One-line summary for whoever picks this up
*Black bands = vanilla’s `dim_x` lags the cell-size change by one present on every
zoom commit (NARROW frame). Our clip is innocent (proven, clipOut=0). Fix the
NARROW present (STEP 3), then layer the scale-≥1 easing (STEPS 4–5).*

---

## 0. TL;DR for a new contributor

1. Pan is rock solid. Do **not** regress it (see `GOLDEN_PATH.md`).
2. There are **two** independent zoom problems. They were historically conflated.
   - **Problem A — clip starvation:** vanilla *stepped* zoom shows black edge
     bands *because* of SmoothPan's origin-grid clip being applied on every
     frame, even when not panning. This happens with **zero** smooth-zoom code.
     **This is Stage 1 and a hard prerequisite for everything else.**
   - **Problem B — easing:** the actual visual smoothing between zoom steps.
     Every past attempt failed because (a) the scale never reached the blits
     (`frozen=100` bug) and (b) they used "commit-first" which forces scale < 1
     and creates the dreaded missing-texel "margin ring".
3. **The key insight that unlocks Problem B:** *scaling a bake UP (scale ≥ 1.0)
   shows a centered subset of texels that already exist → no margin ring ever.*
   Drive both zoom directions so the SDL scale is **always ≥ 1.0**:
   - **Zoom-OUT** (tiles shrink): let vanilla commit the new smaller-cell bake
     first, then ease scale **down from (old_cell/new_cell)>1 to 1.0** on the new
     bake. No feed suppression needed; just detect `gps_z` decreased.
   - **Zoom-IN** (tiles grow): **suppress** vanilla (erase `ZOOM_IN` in feed),
     ease scale **up from 1.0 to (new_cell/old_cell)** on the *old* bake, then
     commit `set_viewport_zoom_factor(target)` at the end and reset scale to 1.0.
4. Anchor: vanilla fortress zoom is **center-anchored** (no `zoom_cent_*` in
   fortress mode). MVP uses center anchor → matches vanilla exactly. Cursor
   anchor is optional polish (Stage 4).
5. Build: edit files in `src/`, run `deploy.ps1` (it syncs `src/` →
   `D:\dfhack\plugins\smoothpan`, builds via cmake, verifies the version string,
   copies the DLL to the Steam DF install). User tests in-game and returns F9
   telemetry. **You cannot run DF; rely on hard telemetry, not vibes.**

---

## 1. System recap (how pan works — the invariants you must preserve)

Split-brain camera (`camera.cpp` / `camera.h`):

| Layer | State | Notes |
|-------|-------|-------|
| Logic/sim | `df::global::window_x/y` (int tiles) | integer; only changes on tile-boundary cross |
| Visual | `frac_x/y` (atomic float [0,1)) | sub-tile remainder, updated each logic tick |
| Render | `render_frac_x/y`, `render_zoom_scale` | snapshot frozen once per frame in `freeze_render_frac()` |

Per-frame pixel shift:
```
cell            = viewport_zoom_factor / 4            (px per map tile = "cell_size")
render_shift_x  = render_frac_x * cell * render_zoom_scale  - overscan_x*cell
```
`render_zoom_scale` already multiplies the shift (it is currently hard-pinned to
1.0 in `freeze_render_frac()` — that pin is the `frozen=100` bug and Stage 2 fixes it).

Render pipeline:
- `smoothpan_dwarfmode_hook::render` (smoothpan.cpp): `g_camera.update()` →
  `begin_render_overscan()` (calls `freeze_render_frac()`) → vanilla render →
  `end_render_overscan()`.
- `smoothpan_renderer_2d_hook::update_full_viewport` (renderer_hook.cpp): marks
  map passes (`g_in_main_viewport_update`), applies the **origin-grid clip** via
  `smoothpan_apply_map_clip()`.
- SDL hooks (sdl_hook.cpp): `Hook_SDL_RenderCopy[F][Ex][ExF]` shift map-blit dst
  rects by `-render_shift` (float path via `RenderCopyF`). `map_blit_extend_
  viewport_edges()` stretches the last column/row to the viewport edge so pan
  leaves no trailing gap (only fires when `|shift| ≥ 0.5`).
- Mouse: GPS-only. `apply_mouse_compensation()` bumps `gps->precise_mouse_x/y`
  by `lround(render_shift)` in feed/logic. Never touch `mouse_x/y` or `window_x/y`.

Zoom ladder (verified): `z ∈ {64,96,128,160,192,224,256}` Δ32, `cell=z/4 ∈
{16,24,32,40,48,56,64}`. Input path: wheel → `feed_ZOOM_IN/ZOOM_OUT` →
`set_viewport_zoom_factor(n)` same frame → `gps->viewport_zoom_factor` changes.
`renderer::zoom()` is **not** the fortress path. `set_viewport_zoom_factor` and
`zoom()` are both interposed in `renderer_hook.cpp` (currently observe-only).

---

## 2. The two problems in detail

### Problem A — clip starvation (Stage 1)

`smoothpan_apply_map_clip()` (sdl_hook.cpp) sets the SDL clip to
`(origin_x, origin_y, vp.right-vp.left, vp.bottom-vp.top)` during every map bake
pass. A second "re-snap" in `Hook_SDL_RenderSetClipRect` forces any DF clip to
the same rect while `g_in_main_viewport_update`. This clip is anchored at the
bake-grid **origin** (~6,6), *not* at the screen viewport (`vp.left=screen_x`,
fluctuates 1..49 during zoom). It exists to stop sub-tile-shifted tiles bleeding
into the top/left HUD margin **while panning**.

Vanilla zoom rebake is **not atomic**: each wheel notch produces, across 2+
presents for the same `gps_z`:
- **NARROW** present: viewport = logical rebake size (`dim_x*cell`), very few map
  blits issued (0–85), tiles stop far short of the screen right edge.
- **WIDE** present: screen-fill (~2560 px), thousands of map blits, looks fine.

On NARROW presents the origin clip width desyncs from where tiles actually need
to draw and discards/short-circuits the map → **black bands**. Confirmed in
`ZOOM_POSTMORTEM.md` Appendix B (`parse_f9_zoom.py` on a real 3.20.0 capture):
NARROW frames had `#map` 0–85 and `rightGap` +300…+820 px.

Important caveat (the postmortem hedges): "strong hypothesis, not proven in
isolation." Two theories:
- **T1 (clip starvation):** blits are *issued* but clipped → removing the clip
  when not panning fixes it.
- **T2 (vanilla issues few blits on the NARROW present):** the band is vanilla's
  own intermediate frame; our clip is irrelevant → need a different fix
  (suppress our compositing / skip presenting transition frames).

Stage 1 does the T1 fix **and** adds telemetry that distinguishes T1 vs T2, so
the next capture is decisive.

### Problem B — easing never applied + margin ring (Stages 2–3)

From `ZOOM_POSTMORTEM.md`: every attempt showed `scale=NNN` in anim state but
`frozen=100` at blit time → the scale never reached `RenderCopyF` dst rects. Root
cause: `freeze_render_frac()` hard-sets `render_zoom_scale = 1.0f` every frame
and the anim scale lived elsewhere. Fix: set `render_zoom_scale` from the zoom
anim state inside `freeze_render_frac()`.

The "margin ring" (missing texels at edges when scaling) **only** happens for
scale < 1.0. The scale-≥-1.0 model (§0.3) sidesteps it entirely. The postmortem
never actually tested scale ≥ 1.0 because the scale never applied at all.

---

## 3. The scale-≥-1.0 model (core strategy for Problem B)

A baked map texture fills the viewport at scale 1.0. Geometry facts:
- **scale > 1.0**: the image grows; the viewport shows a **centered subset** of
  the bake. Every displayed pixel exists in the texture. **No voids.**
- **scale < 1.0**: the image shrinks; a border appears needing texels the bake
  never produced. **Voids / margin ring.** *Avoid this.*

Therefore:

| Direction | Cells | Bake used | Scale path | Commit timing |
|-----------|-------|-----------|------------|---------------|
| **Zoom-OUT** | shrink | the **new** (committed) bake | `old/new (>1)` → `1.0` | vanilla commits first; we react |
| **Zoom-IN** | grow | the **old** bake | `1.0` → `new/old (>1)` | we suppress, then commit at anim end |

Both keep `scale ≥ 1.0` for the whole animation. `render_shift = frac*cell*scale`
already composes correctly (cell is the *baked* cell in both cases: new-cell for
zoom-out since gps_z already committed; old-cell for zoom-in since gps_z is held).

Blit transform (center-anchored), reduces to current pan path when scale==1:
```
shifted.x = anchor_x + (orig.x - anchor_x) * scale - render_shift_x
shifted.w = orig.w * scale
```
`anchor` = viewport center in screen px for MVP. During anim, **disable
edge-extend** (scale≥1 already overfills the viewport) and clip in **screen
space** `(vp.left, vp.top, w, h)` so the overscaled image cannot bleed into HUD.

Why zoom-in must suppress (not commit-first): commit-first would force scale < 1
on zoom-in (void) or an unnatural overshoot-then-settle if forced ≥ 1. Suppress +
ease-up-old-bake is the only void-free, natural-feeling zoom-in.

---

## 4. Staged delivery

Each stage is independently shippable and **must** pass the pan regression
checklist in `GOLDEN_PATH.md` plus its own zoom checks.

### Stage 1 — Kill the black bands (vanilla stepped zoom). PREREQUISITE.
Make the origin clip pan-gated and zoom-transition-aware; add discriminating
telemetry. Detailed below in §5. **Status: in progress (this session).**

### Stage 2 — Smooth zoom-OUT (commit-first, scale-down, no feed suppression).
- Add zoom anim state to `SmoothCamera` (or a small `zoom_anim` struct):
  `active`, `from_cell`, `to_cell`, `t`, `duration_s`, `anchor_x/y`.
- Plumb scale into `freeze_render_frac()`: while anim active,
  `render_zoom_scale = lerp(from_cell/to_cell, 1.0, ease(t))` (starts >1, ends 1).
  (For zoom-out, `to_cell` = current committed cell, `from_cell` = previous.)
- Detect zoom-out in `camera.update()`: when `gps_z < last_zoom`, start anim with
  `from_cell = last_zoom/4`, `to_cell = gps_z/4`, `t=0`, anchor = viewport center.
- Blit transform: extend `map_blit_apply_pan_shift` → apply center-anchored scale
  (only when `render_zoom_scale != 1`). Disable `map_blit_extend_viewport_edges`
  while scaling. Use screen-space clip during anim (reuse the Stage-1 transition
  gate, generalized to "anim active").
- Advance `t` by `dt/duration` each render; clamp; clear anim at t≥1. Enforce a
  **minimum display time / dt-based t** so fast wheel scrolling still eases
  (postmortem: one-frame anims were imperceptible).
- Retarget on rapid notches: if another zoom-out arrives mid-anim, update
  `to_cell` to the newest committed cell and keep `from_cell` = current visual.
- **Validate:** F9 shows `frozen != 100` during anim; `rightGap/leftGap` ≤ pan
  baseline (~20px) on every anim frame; no black bands; pan unaffected.

### Stage 3 — Smooth zoom-IN (suppress + programmatic commit). HIGHER RISK.
- **De-risk first:** add a hidden console command `smoothpan zoomtest in|out` that
  calls `set_viewport_zoom_factor(next_ladder_z)` directly and confirm via F9
  that `gps_z` changes and the rebake is clean (no bands, given Stage 1). This
  proves the programmatic-commit mechanism in isolation before wiring it to anim.
- In `feed`, when `ZOOM_IN` present and not in UI: erase it (same mechanism as
  `CURSOR_*`), set anim `from_cell=cur`, `to_cell=next-up`, hold `gps_z`.
- `freeze_render_frac`: `render_zoom_scale = lerp(1.0, to_cell/from_cell, ease(t))`
  (starts 1, ends >1) — old bake (gps_z held at `from`) grows.
- On `t≥1`: call `set_viewport_zoom_factor(to_z)`, reset scale to 1, clear anim.
  Set the Stage-1 transition window so the commit frame's rebake is clean.
- Collapse rapid notches: keep `to_z` = latest requested; keep animating toward it.
- **Pitfalls from postmortem:** do NOT pre-arm without a confirmed commit
  (3.19.1 bounce-back: 200+ feed_ZOOM with no commit). Do NOT call
  `renderer::zoom()` (3.18.3 no-op in fortress). Use `set_viewport_zoom_factor`.
- **Validate:** zoom-in eases up monotonically; commit lands once (`gps_z` steps
  exactly once per notch); no bounce-back; no bands; pan/mouse unaffected.

### Stage 4 — Polish.
- Mouse comp during anim: until done, **freeze/skip** GPS comp while anim active
  (clicks during the ~120ms ease are rare). Then implement the inverse transform:
  world tile under cursor = `window + ((cursor - anchor)/scale + anchor - origin)/cell`.
- Optional cursor-anchored zoom (recompute `window_x/y`+frac on commit so the
  world point under the cursor is fixed; extend GPS comp transform).
- Commands: `smoothpan zoom on|off`, `smoothpan zoom duration <ms>`.
- Minimap `mustmake` + ffd full on zoom commit (like a tile step).
- Full regression matrix (GOLDEN_PATH + zoom rows in PLAN_SMOOTH_ZOOM §regression).

---

## 5. Stage 1 implementation detail (this session)

### Files touched
- `version.h`, `deploy.ps1` — bump to 3.21.0; **fix deploy to sync ALL src files**.
- `camera.cpp` / `camera.h` — `g_zoom_transition_frames` (atomic int),
  `g_zoom_prev_z`; set both when `gps_z` changes in `update()`.
- `sdl_hook.cpp` — clip gating helper; track active clip; clip-out counter;
  decrement transition frames at present; new F9 lines.
- `renderer_hook.cpp` — no logic change needed (gating lives inside
  `smoothpan_apply_map_clip`), but verify call sites.

### Clip gating logic (sdl_hook.cpp)
```
static bool map_clip_should_constrain() {
    if (g_zoom_transition_frames > 0) return false;          // any zoom commit window
    return fabs(render_shift_x()) >= 0.5f || fabs(render_shift_y()) >= 0.5f;  // real pan
}
```
- `smoothpan_apply_map_clip(enable=true)`: only set origin clip if
  `sdl_shift_mode_active() && map_clip_should_constrain()`. Track with
  `g_sp_clip_applied`. On `enable=false`: only null if we applied.
- Re-snap block in `Hook_SDL_RenderSetClipRect`: gate the forced origin snap on
  `map_clip_should_constrain()`. Otherwise pass DF's clip through unchanged.
- This is byte-identical to current behavior while panning (shift≥0.5, no zoom);
  it only relaxes the clip at rest (shift<0.5px) and during the 8-frame post-zoom
  window — exactly when the black bands occur.

### Transition window (camera.cpp update())
When `df::global::gps->viewport_zoom_factor != last_zoom`:
`g_zoom_prev_z = last_zoom; g_zoom_transition_frames = 8;` (then existing
`last_zoom = zoom`). Decrement `g_zoom_transition_frames` once per present in
`Hook_SDL_RenderPresent`.

### Telemetry (sdl_hook.cpp) — added to the F9 dump block
- Track the live clip: `g_active_clip_rect`, `g_active_clip_set`, updated wherever
  we call `True_SDL_RenderSetClipRect` (helper `track_clip`).
- Count `g_frame_map_clip_out` = map-class blits whose dst is **fully outside**
  the active clip (would be discarded). Checked in `process_map_blit[_f]`.
- New per-frame lines:
  - `ZoomDiag: gps_z=%d prev_z=%d ztrans=%d constrain=%d clip_set=%d shift=(%.2f,%.2f)`
  - `ClipStarve: clip=(%d,%d,%d,%d) set=%d map_total=%d map_clipped_out=%d`
- Reset `g_frame_map_clip_out` with the other per-frame counters.

### Parser (tools/parse_f9_zoom.py)
- Frame-header regex → version-agnostic `SMOOTHPAN_[\d.]+`.
- Parse `ClipStarve` → add `clip_out` and `clip_set` columns.

### Why this can't regress pan
The clip only relaxes when `|shift| < 0.5px` (centered on a tile) or during the
8-frame zoom-transition window. During active panning with no zoom, behavior is
unchanged. Edge-extend, mouse comp, ffd, minimap untouched.

### Decision branch after Stage 1 capture
- **RESOLVED (3.21.0 capture):** `clip_out=0` in gated mode AND bands persist on
  NARROW frames → **Theory 1 REFUTED.** Root cause is vanilla's one-present
  `dim_x` regrow lag (see §0.5). Proceed via §0.5 STEP 1–3, not the old branch.
- (Historical) The pre-capture plan expected `clip_out` high→0 to confirm T1.
  That did not happen; clip was innocent all along.

---

## 6. Telemetry & test harness reference

- **F9** (hotkey) → 30-frame dump to `dfhack-config/smoothpan/smoothpan_telemetry.txt`
  + per-present BMP `smoothpan_frame_N.bmp` (direct visual proof of bands).
  Implemented in `smoothpan_poll_debug_hotkeys` (sets `g_test_dump_frames=30`).
- **`smoothpan dump <frames> [delay]`** — same with a delay (lets you set up).
- **`tools/parse_f9_zoom.py`** — parses the telemetry into a per-frame table
  (NARROW/WIDE, #map, rightGap, and now clip_out). Edit the hardcoded `path` at
  top if the DF install differs.
- **EdgeProfile** lines (RenderReadPixels) give `leftGap/rightGap` = on-screen
  content border vs viewport edge. Negative rightGap = black band inside viewport.
- Note: F9 logging lowers FPS; per-present geometry (bands) still captured, but
  shimmer magnitude is understated. Bands are geometry, not FPS, so F9 is valid
  for Stage 1.

### Stage 1 in-game test protocol (give to the user verbatim)
After deploying 3.21.0 (`plugin unload smoothpan; plugin load smoothpan; enable
smoothpan` → expect `SmoothPan 3.21.0 enabled`):

The build has an **A/B toggle** so before/after use identical telemetry:
- `smoothpan clip legacy` → reproduces the 3.20.0 always-clip behavior (banded).
- `smoothpan clip gated` → the 3.21.0 fix (default on enable).

- **A0 (banded baseline):** `smoothpan clip legacy`. Stand still (no WASD), F9,
  immediately wheel-OUT ~5 notches over ~2s over map center. Save telemetry as
  `tele_legacy.txt`. Expect NARROW frames with high `clip_out`.
- **C1 (zoom-out, fixed):** `smoothpan clip gated`. Same capture. Save as
  `tele_gated_out.txt`. Expect `clip_out≈0` on every frame.
- **C2 (zoom-in, fixed):** `smoothpan clip gated`, F9, wheel-IN ~5 notches.
- **C3 (pan-then-zoom):** `smoothpan clip gated`, pan WASD briefly, stop mid-tile,
  F9, wheel-OUT ~5 notches.
- Send each `smoothpan_telemetry.txt` (renamed) + 2–3 `smoothpan_frame_*.bmp`
  (especially any still-banded). Run `tools/parse_f9_zoom.py` per file; it prints
  a `[WARN] clip_out>0` / `[OK]` summary.

### Stage 1 success criteria (hard numbers)
- Every present: `map_clipped_out ≈ 0`.
- `rightGap`/`leftGap` within ±60 px at every z (matches idle-pan baseline ~20px).
- No banded BMP among the 30 frames.
- Pan regression (GOLDEN_PATH) unaffected.

---

## 7. Anti-patterns (do NOT reintroduce — from history)

- Animate `viewport_zoom_factor` every frame → rebake storm, layer desync.
- Scale HUD/toolbar blits → HUD wobble. Only scale map-class blits.
- Scale < 1.0 on any bake → missing-texel black ring (the whole reason zoom failed).
- Mutate `dim_x/y` or `window_x/y` mid-`update_full_viewport` to "bake more" →
  buffer overrun, diagonal garbage, crash (3.13.6).
- `ReadPixels` texture-freeze on zoom-out → crash (3.16.x).
- Pre-arm zoom anim without a confirmed `dgps≠0` commit → bounce-back (3.19.1).
- `renderer::zoom()` for commit → no-op in fortress (3.18.3). Use
  `set_viewport_zoom_factor`.
- Present-time resnapshot of frac/scale → mid-frame drift.
- Origin clip applied when not panning → **Problem A** (this is what Stage 1 fixes).
- Bump `mouse_x/y` or `window_x/y` for mouse → breaks UI / getMousePos.

---

## 8. Build & deploy quick reference

```powershell
# from repo root
powershell -ExecutionPolicy Bypass -File deploy.ps1
```
- Canonical build tree: `D:\dfhack\plugins\smoothpan`; build dir
  `D:\dfhack\build\VC2022`; output
  `...\build\VC2022\plugins\smoothpan\Release\smoothpan.plug.dll`; copied to
  `C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\hack\plugins\`.
- `deploy.ps1` syncs `src/` → plugin tree, builds, verifies the version string is
  embedded in the DLL, copies. **Keep `$version` in deploy.ps1 == version.h.**
- After editing any `src/*.cpp/*.h`, just run deploy.ps1 (it now syncs all files).

---

## 9. Status log (update as you go)

- **3.20.0** — pan-only baseline; smooth zoom removed. Black bands on vanilla
  zoom documented (ZOOM_POSTMORTEM Appendix). `render_zoom_scale` plumbing exists
  but pinned to 1.0.
- **3.21.0 — SHIPPED & TESTED.** Stage 1 clip gating + zoom-transition window +
  `ZoomDiag`/`ClipStarve` telemetry + `smoothpan clip legacy|gated` A/B toggle +
  parser update + `deploy.ps1` sync-all. **Result: Theory 1 (clip starvation)
  REFUTED** — `clip_out=0` everywhere yet black bands persist on NARROW commit
  frames. Real cause = vanilla `dim_x` one-present regrow lag (§0.5). The 3.21.0
  code is correct/harmless and stays in. **Next work = §0.5 checklist STEP 0→3.**
  - Implementation landed in: `version.h` (3.21.0), `deploy.ps1` (sync-all +
    version), `camera.h`/`camera.cpp` (`g_zoom_transition_frames`,
    `g_zoom_prev_z`, set on gps_z change, decremented per present in sdl_hook),
    `sdl_hook.h`/`sdl_hook.cpp` (`map_clip_should_constrain()`, `track_clip()`,
    `note_clip_starve()`, `g_force_legacy_clip`, `smoothpan_set_legacy_clip()`,
    `ZoomDiag`/`ClipStarve:` F9 lines), `smoothpan.cpp` (`clip` command + help),
    `tools/parse_f9_zoom.py` (version-agnostic, ClipStarve cols, OK/WARN verdict).
  - Telemetry caveat for next session: F9 `remove()`s the telemetry each press and
    per-blit `Blit:` lines make it ~64MB/30frames; captures overwrite each other.
    Fix in §0.5 STEP 0 before gathering more data.

- **3.22.0 — SHIPPED.** STEP 0 capture hygiene. Per-capture file naming +
  opt-in blit log + parser argv + #mapT column + NARROW summary. **No gameplay
  behavior change** (3.21.0 clip gating still in place). All work is telemetry-
  side so the next STEP 1 capture session is decisive instead of wasted.
  - Implementation landed in: `version.h` (3.22.0), `deploy.ps1` (3.22.0),
    `sdl_hook.h` (added `<string>` include + externs for `g_capture_label`,
    `g_capture_seq`, `g_blit_log_enabled` + setters/getters + `arm_capture`
    decl), `sdl_hook.cpp` (Stage 2 block defining the new globals, `telemetry_
    file_name()` and `telemetry_bmp_prefix()` helpers, `smoothpan_arm_capture()`,
    `g_blit_log_enabled` gate on the two `log_blit_telemetry[_f]` functions, BMP
    file name uses the per-capture prefix), `smoothpan.cpp` (F9 path and `dump`
    command now call `smoothpan_arm_capture`, new `zoomcap` subcommand for
    `label` + `blit on|off`, help text + status footer updated),
    `tools/parse_f9_zoom.py` (takes argv[1] for path, prints source path,
    prints `[NOTE]` when no `Blit:` lines, adds `#mapT` column from ClipStarve,
    adds `[INFO]` NARROW-frame summary).
  - User protocol: see §0.5 STEP 1 (below).  Run `enable smoothpan`, expect
    "SmoothPan 3.22.0 enabled."  Then `smoothpan zoomcap <label>` before each
    F9, parser invocation is `python tools/parse_f9_zoom.py
    "...smoothpan_telemetry_<label>_<N>.txt"`.

- **3.23.0 — SHIPPED & TESTED (2026-06-02).** STEP 3 P-lag-is-OURS fix.
  Vanilla DF's wheel zoom updates `dispx_z` (cell) one frame BEFORE
  `main_viewport->dim_x`.  On that 1-frame NARROW commit, the blitter iterates
  OLD dim at NEW cell and only covers `dim*cell` pixels of the screen; the rest
  is unrendered (the user's "big black unrendered section" — see attached
  screenshot, ~510×210 px black rectangle in the bottom-right).  Our
  `gps.viewport_zoom_factor` arrives from the binary's `set_viewport_zoom_factor`
  call, which sets the gps-side `zf` that `shape_viewport_according_by_pixel_size`
  uses to compute `dim_x = px/dim` where `dim = zf*32/128`.  So the dim DOES
  update — just one present after the cell, because vanilla's wheel path
  (`feed_ZOOM_IN` → binary `set_viewport_zoom_factor` → main thread) and the
  reshape path (`renderer->zoom()` → `reshape()` → `gps_allocate()` →
  `reshape_viewports()`) interleave in a way that the gps zf reaches the
  `shape_*` call one frame late.
  - **Fix (in `renderer_hook.cpp` interpose on `update_full_viewport`):** on
    the first map pass of a frame where `dim_x*cell < cur_w - 50` (NARROW),
    force `dispx_z`/`dispy_z` to the OLD cell (derived as
    `cur_w/vp->dim_x` and `cur_h/vp->dim_y`) for the rest of the frame, then
    restore in the `render` interpose.  Result: blits cover OLD dim * OLD
    cell ≈ full screen for that one frame.  User sees 1-frame snap-back to
    old-zoom (vanilla's intrinsic 1-frame lag, which they say is fine), then
    new-zoom on frame 2.
  - **Gating:** only fires when `sdl_shift_mode_active() && !g_force_legacy_clip
    && g_zoom_transition_frames > 0 && is_map_vp`.  Steady state, panning,
    and MapPort mode are all unaffected.  Legacy clip mode unchanged (user
    can still A/B compare with `smoothpan clip legacy`).
  - **Safety:** static `g_narrow_force_active` is checked at the start of
    `update_full_viewport` and forces a restore if a previous frame ended
    without going through `render` (crash/exit edge case).
  - **Telemetry:** the renderer log (`dfhack-config/smoothpan/
    smoothpan_renderer.txt`) gets a `NARROW_FORCE` line each time the fix
    fires, with from/to cell, vpDim, vpW, cur, ztrans.  Main F9 telemetry
    parser output (vpW column) will show the NARROW frames converted to
    WIDE — that is the user-visible proof.
  - **Implementation:** `version.h` (3.23.0), `deploy.ps1` (3.23.0),
    `sdl_hook.h` (added `extern bool g_force_legacy_clip` and decl for
    `sdl_shift_mode_active()`), `sdl_hook.cpp` (removed file-static
    qualifiers, dropped stale forward decl), `renderer_hook.cpp` (3.23.0
    block: 3 file-scope statics + detection in `update_full_viewport`
    interpose + restore in `render` interpose + safety net at start of
    `update_full_viewport`).
  - **User test (2026-06-02, panzoom2 capture):**
    - Stand-still zoom: **PASS** (no bands, smoothpan intact).
    - Pan+zoom (WAS held during wheel-out 3 notches): **PARTIAL FAIL.**
      Captures `smoothpan_telemetry_panzoom2_*.txt` show the fix FIRES on
      zt=8 (fresh) frames (full coverage, BMPs clean) but NOT on zt<=7
      frames (large black L-shaped band visible on frames 16, 19, 21).
    - The fix's gate `g_zoom_transition_frames > 0` is satisfied (zt=7,6
      on those frames).  Suspect: dim regrows within the wheel event
      window to a value where the fix's `actual_vpW < cur_w-50` check
      no longer triggers (e.g., dim=107*16=1712 → vpW=2560, but only
      partially, leaving a band not detected by the +50 threshold).
  - **DO NOT do these (landmines):** do not enable `smoothpan clip legacy`
    to test this — legacy skips the fix by design.  Do not press F9 mid-pan;
    the user is stand-still so clip=0 anyway.  Do not change
    `g_force_legacy_clip` to true.
  - **What this fixes vs what it doesn't:** eliminates the visible 1-frame
    black band on stand-still wheel-zoom.  **Does NOT eliminate the band
    during pan+zoom on zt<=7 frames** — 3.23.1 diagnostic needed.

- **3.23.1 — SHIPPED (2026-06-02, diagnostic build).** Always-on NARROW_DIAG
  logging to figure out the pan+zoom residual.
  - `version.h` (3.23.1), `deploy.ps1` (3.23.1).
  - `sdl_hook.cpp` (3.23.1): per-frame telemetry header now also writes
    `main_viewport dim=(W,H) cell=(X,Y) screen=(X,Y)` so we can see how
    `main_viewport->dim_x` regrows between frames (read from
    `df::renderer_2d` via enabler cast — `gps` itself is `df::graphic`
    which doesn't have `dispx_z`).
  - `renderer_hook.cpp` (3.23.1): new file-scope `g_narrow_diag_log` and
    `g_narrow_diag_logged_this_frame`.  `narrow_diag_open_if_needed()`
    appends to `dfhack-config/smoothpan/smoothpan_narrow_diag.txt`
    (no gate, always-on).  On the first map_vp pass of each frame, logs
    one line with: frame, vpDim, cell, vpW, vpH, cur, ztrans, force
    state, result (FIRED / SKIP:wide / SKIP:cell_le / SKIP:gate
    reason=...).  The per-frame gate is reset to 0 in the `render`
    interpose so the next frame logs again.
  - `tools/parse_f9_zoom.py` (3.23.1): takes optional argv[2] for the
    diag path, adds `mvpDim` and `diag` columns to the per-frame table,
    adds `[DIAG]` summary at end (counts of FIRED / SKIP:wide / etc.,
    and a pattern callout when FIRED frames coexist with SKIP:wide
    frames — which is the pan+zoom regression signature).
  - **Test protocol (same as 3.23.0 pan+zoom, just let it run longer):**
    ```
    1. enable smoothpan           (expect: SmoothPan 3.23.1 enabled)
    2. smoothpan zoomcap panzoom3
    3. F9                         (start rolling buffer)
    4. While panning (WASD), wheel zoom out 3 notches
    5. F9 again                   (dump buffer)
    6. python tools\parse_f9_zoom.py "...smoothpan_telemetry_panzoom3_*.txt"
    7. type "...smoothpan_narrow_diag.txt" | more
    ```
    The parser will show `mvpDim` per frame (the actual `main_viewport->
    dim_x/dim_y`) and `diag` column (FIRED or SKIP reason).  The
    narrow_diag.txt shows the full per-frame decision log.
  - **3.23.1 result (2026-06-03):** diag log captured 7392 frames, 112
    FIRED, 7262 SKIP:gate reason=zt=0.  Cross-referenced SMOOTHPAN
    capture had 3 NARROW frames (dim-lag events).  Diagnosis in
    `docs/DIAGNOSIS_3.23.1.md`:
    1. **UI scaling jumping = 3.23.0 fix itself.** The fix's
       `cur_w / dim_x` formula floors, giving 47 instead of the
       natural 48 at z=192.  Forced cell 47 leaves 22px NARROW band;
       user sees viewport shrink one tile for 1 frame, then snap back.
    2. **SMOOTHPAN header reads wrong "cell".** `r2d->dispx_z` can
       be a stale value (e.g. 14 when the main viewport actually
       renders at 48).  The actual main-viewport cell is
       `gps->viewport_zoom_factor / 4`.  Parser's `cell` column
       was wrong as a result.
    3. **Dim-lag can persist past zt=0.** SMOOTHPAN frame 26 is
       NARROW at zt=0, so the `zt>0` gate blocks the fix on those
       post-transition dim-lag frames.

- **3.23.2 — SHIPPED & TESTED (2026-06-04), DISABLED in 3.23.3.**
  - `version.h` (3.23.2), `deploy.ps1` (3.23.2).
  - `renderer_hook.cpp` (3.23.2): 3 changes from 3.23.1
    (round-up cell formula, dropped zt>0 gate, updated gate reasons).
  - `sdl_hook.cpp` (3.23.2): SMOOTHPAN header reads cell from zf/4.
  - `tools/parse_f9_zoom.py` (3.23.2): accepts optional zf= in regex.
  - **3.23.2 result (2026-06-04):** BROKEN.  User reported:
    - "On enabling smoothpan all the UI gets super big" — NEW regression
    - "Scales as I zoom in and out" — UI is the same value as dispx_z
    - "If I stand still and zoom there's no black border issues"
    - "If I pan and then zoom I get unrendered regions and black lines"
  - **Diagnosis (in `docs/DIAGNOSIS_3.23.2.md`):**
    1. **`dispx_z` is shared between main viewport blits AND UI/radar
       text rendering** (g_src `renderer_2d.hpp:281, 337, 393, 426,
       458, 493, 557, 4132` all use `dispx_z * x + origin_x` with
       width `dispx_z`).  The fix made the radar/UI "super big" for
       1 frame on every fire.
    2. **The 3.23.2 fix dropped the zt>0 gate**, so it fires on EVERY
       frame (not just 8-frame transition window).  Combined with
       `dispx_z` permanently stuck at 14 (the saved value from prior
       fix runs), every frame's detection sees `dim*14 < cur_w-50` and
       fires.  Diag log confirms: 99% FIRED in 3.23.2 session (vs.
       1.5% in 3.23.1 session).
    3. **The main viewport renders at `zf/4` not `dispx_z`** (the
       SMOOTHPAN `viewport=... cell=...` line uses
       `gps->viewport_zoom_factor / 4`, not `r2d->dispx_z`).  So
       the fix's force of `dispx_z = 48` has NO effect on the
       main viewport's dim-lag band.  The fix was a no-op for
       the dim-lag, but a regression for the radar/UI.
  - **Conclusion:** the dim-lag cannot be fixed by modifying `dispx_z`
    because that's not what the main viewport uses.  Need a different
    approach: change `dim` instead (Option C in DIAGNOSIS_3.23.2.md),
    or trigger a reshape to sync zf and dim.

- **3.23.3 — SHIPPED (2026-06-04, fix disabled, diag log kept).**
  - `version.h` (3.23.3), `deploy.ps1` (3.23.3).
  - `renderer_hook.cpp` (3.23.3): 3.23.2 fix force wrapped in
    `if (false && ...)` so it never executes.  Diag log still
    logs SKIP:wide / SKIP:gate for every frame.  Per-frame
    detection logic still active so the diag log shows what the
    fix WOULD have done.
  - **User-visible behavior:** identical to 3.22.0 (no fix at all).
    - Stand-still zoom: 1-frame dim-lag band (vanilla bug, accepted)
    - Pan+zoom: same 1-frame dim-lag band
    - **No UI scaling regression** (the "super big" radar is gone)
    - **No UI scale jumps** (the dim-lag is unchanged from vanilla)
  - **User protocol:**
    ```
    1. enable smoothpan           (expect: SmoothPan 3.23.3 enabled)
    2. Test pan+zoom
    ```
    **PASS:** UI is normal size, no "super big" radar, no
    scale jumps.  1-frame dim-lag band may still be visible
    during zoom — this is a vanilla DF bug, accepted.  **FAIL:**
    any UI scaling issues or other regressions.
  - **Next steps:** redesign the dim-lag fix.  Options:
    - **Option A (recommended):** change `dim` on the dim-lag
      frame to `cur_w / dispx_z` (or `cur_h / dispy_z`).  The
      main viewport then renders with the OLD cell and the NEW
      dim, covering the full screen.  Doesn't affect dispx_z
      (so UI/radar unchanged).  Requires safe dim allocation
      (3.13.6 landmine concerns; need to verify current
      smoothpan has proper bounds checks).
    - **Option B:** trigger a `reshape()` immediately after
      `set_viewport_zoom_factor` to sync dim and cell.  More
      invasive (touches the binary's update path).
    - **Option C:** delay the cell update by 1 frame so dim
      can catch up.  Requires hooking `reshape()` or
      `set_viewport_zoom_factor`.
    - **Option D:** accept the 1-frame dim-lag band as a
      vanilla bug.  Document it.  Move on to other things.

- **3.23.4 — SHIPPED (2026-06-04, diagnostic).** Per-capture `seq`
  preservation + two-capture test (stand-still + pan+zoom with same
  `g_capture_seq` for clean A/B).
  - `version.h` (3.23.4), `deploy.ps1` (3.23.4).
  - `sdl_hook.cpp` (3.23.4): `telemetry_file_name()` always includes
    `g_capture_seq`; `telemetry_bmp_prefix()` uses `c<seq>_` no-label
    format; `smoothpan_arm_capture()` always bumps seq.
  - **3.23.4 result (2026-06-04):** parser showed F9 metadata
    IDENTICAL between stand-still and pan+zoom (same dim=40x23,
    cell=56, vpW=2240, rGap≈306) — only the camera `shift` differed.
    Visual BMPs differed: dim-lag region in pan+zoom frames looked
    black; in stand-still frames matched previous frame (so the
    F9-detected dim-lag was invisible in stand-still).  Initial
    hypothesis: "previous-frame content in 320px dim-lag region".
    Diagnosis in `docs/DIAGNOSIS_3.23.4.md`.

- **3.23.5 — SHIPPED then CRASHED (2026-06-04), DISABLED in 3.23.5b.**
  Redesigned Option A fix (change `vp->dim` to grow on dim-lag
  frame so blitter covers the full screen).
  - `version.h` (3.23.5), `deploy.ps1` (3.23.5).
  - `renderer_hook.cpp` (3.23.5): on dim-lag detection in
    `update_full_viewport` interpose, set
    `vp->dim_x = (cur_w + dispx_z - 1) / dispx_z` (round up) and
    `vp->dim_y = (cur_h + dispy_z - 1) / dispy_z`.  Doesn't touch
    `dispx_z`, doesn't touch UI/radar.  Self-corrects next frame
    via vanilla's `reshape()`.
  - **3.23.5 result (2026-06-04):** **HARD CRASH on plugin enable.**
    Disabled.  Root cause not yet diagnosed — most likely a
    `screentexpos_*` array out-of-bounds write (the array is sized
    for the OLD dim, but we changed the dim without reallocating).
    Lesson: don't ship a fix that modifies a shared field without
    first instrumenting the actual blit pipeline to understand
    state transitions.  Diagnosis in `docs/DIAGNOSIS_3.23.5.md`.

- **3.23.5b — SHIPPED (2026-06-04, fix disabled).** Entire 3.23.5
  fix block wrapped in `if (false && ...)` kill-switch.  Plugin
  re-enables safely.  All other code unchanged.

- **3.23.6 — SHIPPED & USER-CONFIRMED STABLE (2026-06-04).**
  Per-blit instrumentation in `sdl_hook.cpp` + parser updates +
  NEW `tools/check_dim_lag_region.py` BMP pixel sampler.
  - `version.h` (3.23.6), `deploy.ps1` (3.23.6).
  - `sdl_hook.cpp` (3.23.6): `log_blit_telemetry`/`log_blit_telemetry_f`
    take `src_x, src_y, src_w, src_h` and emit `frame=`, `src=`,
    `vpDim=` fields in `Blit:`/`BlitF:` lines (backward-compatible
    regex).  `process_map_blit(_f)` signatures updated; all four
    SDL hook variants pass `srcrect` (with NULL guard).
  - `tools/parse_f9_zoom.py` (3.23.6): parses new Blit: fields,
    per-frame blit summary line, main-viewport coverage analysis
    with `[OVERFLOW]` and `[COV]` sections, columns `MCovR`,
    `MCovB`, `shft%`, `map%`, `tile%`.
  - `tools/check_dim_lag_region.py` (NEW): BMP pixel sampler.
    Loads uncompressed 24/32-bit BMP, samples right-of-vp and
    below-vp regions, reports mean/stddev/unique/black% color
    stats, optional cross-frame region_diff, prints horizontal
    color strip at y=mid.
  - **3.23.6 result (2026-06-04):** two-capture test (`v3236b_1`
    stand-still, `v3236b_2` pan+zoom, 87.9MB / 571,968 Blit: lines
    capture 2).  Confirmed: blitter IS rendering in the dim-lag
    region (3536 24x24 blits in right strip at z=64 NARROW) —
    dim-lag is a cell-size mismatch strip, not a missing-blits
    region.  At z=224 NARROW, the artifact is 320px right + 184px
    bottom (12% screen, "a few little unrendered artifact lines"
    in user description).  At z=64 NARROW, the artifact would be
    815px right + 443px bottom (52.8% screen), but user doesn't
    typically zoom that deep.
  - **User confirmation (2026-06-04):** "playing on the latest
    version and I'm not seeing that big unrendered region, now
    it's just a few little unrendered artifact lines.  Even after
    panning around it's actually pretty solid (not smooth, nor
    perfect with these slight presumably dim lag related
    artifacts that you've been able to spot in the f9, but it's
    better than it was before)."  **SHIPPED & STABLE.**

- **3.23.7 — SHIPPED (2026-06-04, fix attempt 1).**  Attempted to
  extend render-time mouse comp to placement modes
  (BUILDING_PLACEMENT / ZONE_PAINT / STOCKPILE_PAINT / BURROW_PAINT /
  main_designation_selected != NONE) so the building/zone ghost
  tracks the compensated mouse.  Comp was applied at render but
  the ghost was still out of sync.  See DIAGNOSIS_3.23.13.md for
  the full F9-driven root-cause analysis.

- **3.23.8–3.23.12** — diagnostic intermediates (each adds one
  more level of trace to F9 output, no gameplay change).
  3.23.8: `world_cursor` at F9 time.  3.23.9:
  `world_cursor_rend_start/end` capture.  3.23.10: `mode bottom/desig`
  logging.  3.23.11: per-sub-check `wants_render` trace.
  3.23.12: per-sub-check `overmap` trace.

- **3.23.13 — SHIPPED & USER-CONFIRMED (2026-06-05).**  Ghost sync
  fix landed.  `designation_over_map()` now skips the
  `IsMouseInUI_reason()` container-rect check for placement modes
  (was returning 5 because the building-placement panel's CONTAINER
  rect covers the map area).  Relies on the leaf-only
  `mouse_over_ui_widget()` check + the `mouse_gate_should_compensate()`
  compensable-map gate.  Rectangle drag still uses the strict
  container-rect check.
  - `version.h` (3.23.13), `deploy.ps1` (3.23.13).
  - `src/designation_sync.cpp` `designation_over_map()`: split gate
    by `real_rectangle_drag()` — strict for drag, leaf-only for
    placement.  `placement_mode_active()` new helper, `wants_render()`
    now takes uncomp precise coords.  Diagnostic globals added for
    F9 trace.
  - `src/designation_sync.h`: signature change, new externs.
  - `src/smoothpan.cpp` render interpose: passes uncomp precise to
    `wants_render`, captures `cursor->x,y,z` + `precise` at
    start/end of render for F9.
  - `src/sdl_hook.cpp` F9: emits `world_cursor_f9`,
    `world_cursor_rend_start/end`, `mode bottom/desig`,
    `wants_render`, `overmap` lines.
  - **User confirmation (2026-06-05):** "workshop placement is
    perfect now".  **SHIPPED & STABLE.**
  - **Next steps (deferred — current state acceptable):**
    - **3.24.0 (if desired):** per-blit cell alignment — intercept
      main vp blits (vmap=1 + vpscr matches main_vp), override
      their `dst.w/h` to use NEW cell (`zf/4`).  Skips UI/radar
      blits.  Made possible by 3.23.6 per-blit log giving us the
      exact filter.  The 3.23.0/3.23.2 approach modified
      `dispx_z` globally and broke UI.  The 3.23.5 approach (grow
      `vp->dim`) didn't address the cell mismatch.  3.24.0
      directly rewrites the blit rectangle for main vp blits only.
    - **Future diagnostic:** pixel-sample the right-side dim-lag
      region in motion (not just stills) to characterize the
      in-game appearance.  Test the deep-zoom-out NARROW frames
      (z=96, z=64) where the dim-lag is largest.  Test the
      zoom-OUT direction (z increasing) to see if the artifact
      behaves the same way.  My test was zoom-IN; user said they
      were zoom-OUT.

# Investigation: Smooth Zoom + Edge Shimmer

**Date:** 2026-05-31  
**Current safe build:** 3.13.7 (vanilla stepped zoom both directions; pan/MMB/GPS mouse unchanged)  
**Status:** Research only — no further zoom experiments until margin path is validated

---

## 1. Redeploy

3.13.7 is copied to `hack/plugins/smoothpan.plug.dll`. Reload in DF:

```
plugin unload smoothpan
plugin load smoothpan
```

Enable line must say **SmoothPan 3.13.7 enabled**.

---

## 2. F9 analysis — 3.13.6 zoom-out (no crash session)

Your capture shows what the broken build was doing before the hang/crash on the earlier run.

### Zoom-out anim (160→128, frame 25)

| Field | Value | Meaning |
|-------|-------|---------|
| `dir=-1` | zoom out | SDL scale anim active |
| `s0=107` | capped start | 107% (not full 125% ladder ratio) |
| `scale=106` | frame 1 of ease | shrinking toward 100% |
| `overscan=(0,0)` | Sdl mode | no window pull — gaps must come from bake or fill |
| `map=12631/12881 (98.1%)` | 250 map blits unshifted | likely non-tile sprites during scaled pass |
| `sprite=0/250` | counted in map bucket | classification edge case during zoom |

### EdgeProfile during zoom anim

```
vp.right=2048  rightContentEnd=2553  (content past nominal vp width)
row y=1163  leftContentStart=196      (hole — no tiles in left region of row)
```

**Interpretation:** At 106% center-scale, tiles extend ~6% beyond the vanilla bake grid on all sides. Those pixels have **no texpos data** → black/unrendered bands. As scale eases to 100%, bands shrink — matches “big empty region that fills back in.”

**Not a clip bug** (clip was disabled during anim in 3.13.5+). **Missing baked texels** in the margin ring.

### Diagonal lines (3.13.6 dim hack — crash run)

When `renderer_hook` temporarily did:

```cpp
vp->dim_x += margin * 2 * dim_px;
*window_x -= margin;
```

DF still indexed `screentexpos_*` arrays allocated for the **original** `dim_x × dim_y`. Writing “extra” tiles = buffer overrun → garbage tex indices → diagonal smears → hang/crash.

This matches your intuition: **you cannot paint outside the allocated viewport buffer.**

---

## 3. DF viewport memory model (confirmed)

From `df/graphic_viewportst.h` (DFHack codegen):

- `dim_x`, `dim_y` — logical viewport size
- `screentexpos_*` — dozens of **heap arrays**, one slot per viewport cell per layer
- `update_full_viewport` fills these arrays, then SDL blits from them

Same pattern on `graphic_map_portst` for world map.

**Implication:** Any “bake more tiles” strategy must either:

1. Let vanilla resize/reallocate through its normal zoom/rebake path (observe with trace), or
2. Stay within the **existing** dim and only use SDL transforms that don **not** require extra texels (scale ≤ 1.0, or shift-only pan).

**Anti-pattern (proven):** Mutating `dim_x/y` or `window_x/y` ad-hoc during `update_full_viewport` without matching allocation.

---

## 4. Why pan works but zoom-out scale doesn’t

| Technique | Pan (working) | Zoom-out SDL scale > 1 (failed) |
|-----------|---------------|----------------------------------|
| Sub-tile shift | Moves existing texels inside fixed grid | Same — OK |
| Trailing overscan | `window_x/y ± 1` pulls **one** extra tile on **one** edge (disabled in Sdl mode) | Needs margin on **all four** edges simultaneously |
| Edge fill | `fill_edge_gap()` stretches **last row/col** into 3–20 px gap | Needs source texels at scaled margin; stretch from 1 tile ≠ full ring |
| Clip snap | Origin-grid clip stops left/top bleed | Disabling clip during zoom removed one symptom, not missing texels |

Pan gaps are **one-sided and ~frac×cell pixels** (typically &lt; 40 px). Zoom-out at 107% needs **~3.5% of half-width** on every side (~45 px at 2560 wide) — comparable, but on **all sides at once**, and the center-scale transform moves the gap continuously during the anim.

---

## 5. Option 1 — Proper overscan ring (recommended research track)

Goal: bake a margin ring through **vanilla’s own resize path**, then SDL-scale inside that ring.

### Phase R0 — Observe legitimate resize (trace only)

Use existing `smoothpan trace` / renderer log on **vanilla wheel zoom** (SmoothPan zoom anim off):

- Log `dim_x/y`, `screen_x/y`, `window_x/y` at `update_full_map_port` and each `update_full_viewport`
- Count texpos array usage vs dim (if accessible via debugger / future hook)
- Record whether dim changes **only** on `set_viewport_zoom_factor` or also on rebake passes

**Success criteria:** Document the exact call sequence when DF safely goes 160→128 z.

### Phase R1 — Margin without touching dim mid-pass

Candidates to evaluate (in order):

1. **Vanilla already bakes slightly outside visible rect** — measure `origin_x` vs `screen_x` slack; may be enough for scale ≤ 1.05 only
2. **`pixel_perc_x/y` on map_port** (used for sub-tile pan in MapPort shift mode) — might shift bake origin without realloc; unknown effect on fortress viewport
3. **Directed multi-pass overscan** — four rebake frames each pulling window one tile N/S/E/W (too slow; probably unacceptable)
4. **Intercept + defer zoom commit** — hold `gps_z` at old value while baking margin at new cell size (Phase 1 in PLAN_SMOOTH_ZOOM; previously risky)

### Phase R2 — Re-enable SDL zoom only with proven margin

Constraints:

- `render_zoom_scale` never exceeds what margin texels support (dynamic cap from baked bounds)
- No hold-then-jump timing (caused stutter in 3.13.4–3.13.6)
- Clip: origin snap when idle; expanded clip only if texpos data exists in margin

---

## 6. Edge shimmer (right / bottom) — separate from zoom

### What “worked perfectly” referred to

From session history: the **visual baseline** around 3.3.x had smooth map + static HUD; remaining issues were **mouse/tooltip only** ([MOUSE_SYNC.md](MOUSE_SYNC.md)). Edge shimmer was a **later** regression tracked through 3.11–3.12 (MMB judder, edge fill iterations).

Do **not** revert GPS mouse compensation or renderer shift gates to chase edges.

### Current mechanism (3.13.7)

- **Left/top:** `smoothpan_apply_map_clip()` snaps to origin grid — stable
- **Right/bottom:** `fill_edge_gap()` in `sdl_hook.cpp` — when last col/row detected, **stretch full tile** into gap via `RenderCopyF`

F9 on stable pan (3.13.6 file, idle pan) shows fills **do fire**:

```
RightFill: (2553,45,7,40)    — 7 px wide gap at vp.right=2560
BottomFill: (673,1445,40,22) — 22 px tall gap at vp.bottom=1467
```

`tile_right = origin_x + (vp.right - vp.left) = 6 + 2560 = 2566` but actual last blits end ~2553 (6 px origin slack). Fill targets `[shifted_right, vp.right)` — gap width **changes every frame** with `frac` → stretch ratio oscillates → **shimmer**.

### Why MMB worse than WASD

- MMB sets `true_x/y` directly; continuous shift without velocity-aligned overscan
- `overscan=(0,0)` in Sdl mode by design ([ARCHITECTURE.md](ARCHITECTURE.md) — single-sided overscan hurt Sdl mode)
- 3.12.7 float `RenderCopyF` helped integer judder; did not fix stretch shimmer

### Edge fix research (no code yet)

| Approach | Pros | Risks |
|----------|------|-------|
| **A. 1:1 blit slice** (3.12.8 tried) | No stretch shimmer | Atlas slice smaller than gap → unfilled band (failed before) |
| **B. Smarter gap cap** | Only fill if gap ≤ N px; else clip | Possible thin black line |
| **C. Trailing-edge overscan in Sdl mode** | Real texels, pan-proven | Must not reintroduce 3.4.0 asymmetric black bars; only trailing edge |
| **D. Right/bottom clip to vp.right/bottom** | No margin shimmer | Loses sub-tile motion at those edges (acceptable?) |
| **E. F9 EdgeProfile diff** | Quantify `rightMargin` variance frame-to-frame during MMB | Diagnostic for next fix |

**3.13.8 failed:** Sdl overscan +1 every frame without `restore_window_overscan()` → `window_x/y` drifted +1 per frame (camera pulled SE). Negative `render_shift` also inverted pan feel. **Reverted in 3.13.9**; added `restore_window_overscan()` inside `end_render_overscan()` for any future overscan experiments.

**FPS / F9 confound (user finding):** Shimmer is worst at high FPS + fast MMB; at ~10 FPS cap it largely vanishes. F9 logging drops FPS during capture, so F9 dumps understate shimmer. Root cause: gap width changes every frame; old fill **stretched** the full tile to `gap_w`, rescaling texels each frame. **3.14.0:** 1:1 edge-strip fill (no horizontal/vertical stretch).

---

## 7. What we must not revert

- GPS-only mouse compensation (`mouse_comp.cpp`, production boot)
- Renderer shift gate on same-dim map passes (`renderer_hook.cpp`)
- Float `render_shift` + `RenderCopyF` map path (`3.12.7` pan fix)
- Origin-grid clip during idle pan (`smoothpan_apply_map_clip`)
- Widget/toolbar classification exclusions

---

## 8. Proposed delivery order

1. **3.13.7** — stable baseline (vanilla zoom) ← **now**
2. **Trace capture doc** — user runs vanilla zoom + MMB F9 on 3.13.7; append to this file
3. **Edge shimmer** — directional overscan or fill policy (3.14.x pan-only)
4. **Zoom margin research** — trace-driven; no dim hacking
5. **Zoom SDL anim** — only after margin proven (3.15.x+)

---

## 9. User capture checklist (next session)

On **3.13.7**:

1. **Vanilla zoom** — one wheel tick in/out; F9; confirm `anim=0 scale=100` always
2. **MMB drag** — 2 s along right edge; F9; send telemetry
3. **WASD pan** — same; compare EdgeProfile `rightMargin` variance
4. Optional: `smoothpan trace 5` during vanilla zoom step for dim/alloc sequence

Log path: `{DF}/dfhack-config/smoothpan/smoothpan_telemetry.txt`

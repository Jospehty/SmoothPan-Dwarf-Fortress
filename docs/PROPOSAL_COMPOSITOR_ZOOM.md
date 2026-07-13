# Proposal: Smooth Zoom via a Retained-Frame Map Compositor

**Date:** 2026-07-13
**Baseline:** 3.23.13 (pan rock-solid; vanilla stepped zoom with accepted 1-frame
dim-lag strip)
**Status:** Design proposal — no code yet. Supersedes the per-blit easing plan in
`SMOOTH_ZOOM_MASTER_PLAN.md` §4 Stages 2–3 (the scale-≥-1.0 *model* from that plan
is kept; the *application mechanism* changes).

---

## 0. TL;DR

Stop trying to scale thousands of individual map blits in place. Instead,
**redirect the map-layer blits into an offscreen SDL render target we own**, then
draw that one texture to the screen with a **single** `RenderCopyF` carrying the
whole camera transform (pan shift + zoom scale about the cursor anchor).

This one architectural move:

1. **Makes the scale actually apply** — one dst rect instead of ~5000. The
   `frozen=100` class of bug (scale never reaching blits, 3.19.x) becomes
   structurally impossible.
2. **Gives us a retained copy of the last complete map frame.** During vanilla's
   non-atomic zoom rebake (the NARROW/WIDE dim-lag presents), we composite the
   *previous* good texture instead of the broken partial bake. This erases the
   remaining dim-lag artifact **even for vanilla stepped zoom** — the fix that
   3.23.0/3.23.2/3.23.5 tried and failed to do by mutating DF state
   (`dispx_z`, `vp->dim`), done instead with **zero writes to DF memory**.
3. **Keeps scale ≥ 1.0 at all times** (the master-plan insight), so no missing
   texels ever, including **cursor-anchored** zoom (proof in §4.3).
4. **Makes mouse compensation exact** during the animation — the map transform is
   a single invertible affine map, not an emergent property of per-blit edits.

The pan pipeline inside the texture is **byte-identical to today's golden path**
(same shift, same edge extend, same clip). At `scale == 1.0` the composite blit
is 1:1, so Phase A ships as a pure refactor with pixel-identical output, behind a
`smoothpan compositor on|off` toggle with the current direct path kept as instant
fallback.

---

## 1. Constraint inventory (what the history proves)

Everything below is evidenced in `ZOOM_POSTMORTEM.md`, `SMOOTH_ZOOM_MASTER_PLAN.md`,
`INVESTIGATION_ZOOM_AND_EDGES.md`, and the DIAGNOSIS_3.23.x series. Any zoom design
must satisfy all of it:

| # | Constraint | Evidence |
|---|-----------|----------|
| C1 | The viewport bake is a fixed `dim_x × dim_y` buffer; you cannot paint outside it or resize it without vanilla's realloc | 3.13.6 diagonal-garbage crash; 3.23.5 hard crash |
| C2 | SDL scale < 1.0 on the bake exposes texels that don't exist (margin ring / black bands) | 3.13.4–3.13.6; postmortem §"Why pan works" |
| C3 | Scale ≥ 1.0 shows a subset of baked texels → void-free | Master plan §3 (never actually tested — scale never applied) |
| C4 | Vanilla zoom commit is non-atomic: cell updates one present before `dim_x` regrows → NARROW frame with partial coverage | 3.21.0 capture, §0.5 of master plan |
| C5 | `dispx_z` is shared with UI/radar rendering — mutating it breaks the HUD | 3.23.2 "super big UI" regression |
| C6 | The main viewport renders at `zf/4`, not `dispx_z` | DIAGNOSIS_3.23.2 |
| C7 | Zoom input path is `feed_ZOOM_IN/OUT` → `set_viewport_zoom_factor`; `renderer::zoom()` is a fortress-mode no-op | Phase 0 discovery; 3.18.3 |
| C8 | Never pre-arm an anim without a confirmed `dgps ≠ 0` commit | 3.19.1 bounce-back |
| C9 | One-frame animations are imperceptible; `t` must be dt-based with a minimum duration | 3.19.3 F9 (`t=0` only) |
| C10 | `ReadPixels` texture freezing crashes | 3.16.x |
| C11 | Mouse comp must stay GPS-only; never bump `mouse_x/y` / `window_x/y` | MOUSE_SYNC, GOLDEN_PATH |
| C12 | Pan (shift, edge extend, origin clip, GPS comp) must not regress | GOLDEN_PATH |

The per-blit easing plan (master plan Stages 2–3) satisfies C1–C11 on paper but
has never demonstrated the scale reaching the screen. It also carries a risk the
postmortem never got far enough to hit:

**C13 (new observation) — per-blit scaling cracks.** Scaling each tile's dst rect
independently about a common anchor (`x' = a + (x-a)*s`) puts adjacent tiles at
float coordinates whose rasterized edges don't necessarily abut: at `s=1.07`, a
64-px tile becomes 68.48 px, and SDL rounds each blit's edges independently.
Sub-pixel seams (background bleed lines) between every tile pair are likely,
shimmering as `s` eases. Pan never sees this because a pure translation moves all
rects rigidly. A single composite blit cannot crack by construction.

---

## 2. The core idea

### 2.1 Today's pipeline (per frame)

```text
update_full_viewport (map passes, N×)      → tile blits → backbuffer (shifted per blit)
post-viewport map compositing              → aligned tile blits → backbuffer (shifted)
HUD / toolbar / panels                     → blits → backbuffer (untouched)
SDL_RenderPresent
```

The map layer is irrecoverable the instant it hits the backbuffer — we can never
re-present it, scale it, or bridge over a broken bake.

### 2.2 Proposed pipeline

```text
first map pass BEGIN:   SetRenderTarget(map_tex[cur])     ← our screen-sized target texture
map passes + post-viewport map blits:
    tile blits → map_tex (pan shift + edge extend applied per blit, EXACTLY as today)
map layer END:          SetRenderTarget(backbuffer)
                        composite: RenderCopyF(map_tex, src=full, dst=zoom transform)
HUD / toolbar / panels: → backbuffer (untouched, drawn after composite, always live)
SDL_RenderPresent
```

- `map_tex` is `SDL_TEXTUREACCESS_TARGET`, sized to the renderer output, recreated
  on resize. Two of them (`cur`/`prev`) — see §5.
- The blits DF issues are untouched in *content*; only their destination surface
  changes. Their dst coordinates are already screen-space, so the texture is a
  1:1 screen-space snapshot of the map layer.
- The composite transform (center `a`, scale `s`, applied to the whole texture):

```text
dst.x = a.x - a.x * s          dst.w = tex_w * s
dst.y = a.y - a.y * s          dst.h = tex_h * s
```

  At `s = 1.0` this is the identity — pixel-identical to the current pipeline.
- Composite is clipped to the strict viewport rect in screen space so the
  overscaled image cannot bleed into HUD margins (replaces the zoom-time clip
  concerns of past attempts; the origin-grid pan clip still applies to blits
  *inside* the texture, unchanged).

### 2.3 Why the pan shift stays per-blit (inside the texture)

Moving the pan shift onto the composite blit is tempting (one transform for
everything) but would break the edge-extend trick: the texture only contains what
was baked, so a shifted composite would expose an unbaked strip with no
last-column tile to stretch. Keeping pan per-blit means:

- GOLDEN_PATH pan behavior is preserved verbatim (C12) — same code, same rects,
  just a different render target.
- The composite adds *only* the zoom scale. When no zoom anim is active, it's a
  1:1 copy.
- Transform composition stays clean: `screen = a + (tex_px - a)·s`, and `tex_px`
  space is exactly today's screen space (shift already inside). The mouse inverse
  is a single divide (§6).

---

## 3. What each historical failure looks like under the compositor

| Past failure | Under the compositor |
|---|---|
| `frozen=100` — scale never reached blits (3.19.x) | Scale is applied to exactly one blit that we issue ourselves. Nothing to plumb through per-blit freeze state. |
| Margin ring on scale < 1 (3.13.x) | Scale ≥ 1.0 policy kept (§4); geometry proof includes cursor anchor (§4.3). |
| Edge voids during anim (`void_r` 300–1058 px) | The texture always holds a complete viewport bake; composite at s ≥ 1 samples a subset of it. |
| NARROW dim-lag band (3.20.0 → 3.23.6 residual) | On a detected-incomplete frame, composite `map_tex[prev]` instead. No DF state touched — the 3.23.0/3.23.2/3.23.5 crash/regression mechanisms don't exist here. |
| Per-blit `dim`/`dispx_z` mutation crashes (C1, C5) | Zero writes to DF memory. We only choose what to draw. |
| ReadPixels freeze crash (C10) | GPU-side render target; no readback anywhere. |
| One-frame anim imperceptible (C9) | Anim `t` advances on wall-clock dt with a min duration (~120–180 ms); the bake cadence no longer gates the animation at all, because we can keep compositing a retained texture while vanilla is mid-rebake. |
| Per-blit scale cracks (C13) | Impossible — one blit. |

---

## 4. Zoom model: continuous visual cell + ladder commits

### 4.1 State

Keep the split-brain principle. Add one continuous value:

| Layer | State | Owner |
|---|---|---|
| Logic | `gps->viewport_zoom_factor` ∈ ladder {64…256} | Vanilla + our programmatic commits |
| Visual | `v` = visual cell size in px (float, continuous) | SmoothPan |
| Render | `s = v / baked_cell`, anchor `a` | frozen once per frame |

Invariant: **`baked_cell ≤ v`**, i.e. the committed ladder step is always the one
at-or-below the visual zoom, so `s = v / baked_cell ≥ 1.0` at all times (C3).

Wheel input drives `v` through a velocity model in log-space, mirroring the pan
physics (impulse per notch + friction), so fast scrolling produces one continuous
glide instead of queued discrete steps. Retargeting mid-anim is free — notches
just add velocity.

### 4.2 Ladder crossings

Cells ladder: 16, 24, 32, 40, 48, 56, 64 (`z/4`).

- **Zoom-in (v rising):** while `v < next_cell`, just ease `s` up on the current
  bake (`s ∈ [1, next/cur)`, worst 1.5 at the bottom of the ladder, 1.14 at the
  top). When `v` crosses `next_cell`: erase `ZOOM_IN` from feed is *not* needed —
  we own the wheel entirely in this model (wheel notches never reach vanilla;
  they only move `v`). Commit `set_viewport_zoom_factor(next_z)` ourselves,
  bridge the rebake frames with `map_tex[prev]` still composited at the old-bake
  scale (§5), then continue on the new bake at `s = v/next_cell ≈ 1.0+`.
- **Zoom-out (v falling):** the moment `v < cur_cell`, `s` would dip below 1 —
  commit down to `prev_cell` immediately, bridge the rebake, resume with
  `s = v/prev_cell ∈ (1, ratio]` easing down toward 1. The ≤ 2 bridge frames at
  the boundary are covered by the retained texture (composited at its own scale,
  clamped ≥ 1), so there is no visible under-scale window.
- **At rest:** `v` glides to the nearest ladder cell and settles at `s = 1.0`
  exactly — crisp 1:1 texels whenever the camera is idle. Zero rendering
  compromise at rest. (Optional later: a `smoothpan zoom free` mode that lets `v`
  rest between steps for players who prefer RimWorld-style arbitrary zoom over
  crispness.)

Commit safety: commits are *our own* `set_viewport_zoom_factor` calls (proven
path, C7) and the anim state is driven by `v`, not by anticipating gps — there is
no pre-arm (C8). If a commit unexpectedly doesn't land (gps unchanged next
frame), clamp `v` back to the baked cell and drop to `s = 1` — worst case is
vanilla behavior, never bounce-back.

### 4.3 Cursor anchoring is safe at s ≥ 1 (why no margin ring, ever)

Let the viewport rect be `V`, anchor `a ∈ V`, transform `T(p) = a + (p−a)·s` with
`s ≥ 1`. The screen pixels we must fill are `V`; they sample texture points
`T⁻¹(V) = a + (V−a)/s`. Since `a ∈ V` and `1/s ≤ 1`, that preimage is `V` shrunk
toward an interior point — a subset of `V`. Every sampled texel exists in the
bake. This holds for *any* anchor inside the viewport, so **cursor-anchored zoom
costs nothing extra** — it's the same one blit with a different `a`. (Vanilla
fortress zoom is center-anchored; cursor anchor is the RimWorld feel the project
wants, and here it's free rather than a Stage-4 stretch goal.)

On commit, recompute `true_x/y`/frac so the world tile under the anchor is
preserved (one closed-form assignment from the same transform), which is what
makes the zoom feel "mouse-directed" rather than "zoom then lurch".

### 4.4 Texel quality during motion

While `s > 1` the map is magnified from baked texels — momentary softness or
chunkiness, worst 1.5× for the 16→24 px step. Mitigations:

- `SDL_SetTextureScaleMode(map_tex, SDL_ScaleModeLinear)` during anim → smooth
  bilinear glide (this is exactly what RimWorld-style engines do); switch to
  nearest / land on `s = 1.0` at rest for crispness. Make it a console option.
- The window is short (~150 ms per step at normal wheel speed).
- Optional polish: crossfade `map_tex[prev]` (old cell) into `map_tex[cur]` (new
  cell) over the first ~80 ms after a commit to soften the texel-size pop. With
  two retained textures this is two blits and an alpha ramp — cheap, and entirely
  optional.

---

## 5. The retained-frame bridge (fixes dim-lag as a side effect)

Double-buffer the target: `map_tex[cur]` receives this frame's bake;
`map_tex[prev]` holds the last frame that passed a completeness check.

Per frame at composite time:

1. Classify the captured bake: complete if the map passes covered the viewport
   (reuse the existing NARROW detection — `dim_x*cell` vs `cur_w`, and/or the
   per-frame map-blit count that the F9 pipeline already tracks).
2. If complete → composite `cur`, then swap roles (`prev ← cur`).
3. If NARROW/incomplete (only happens inside the zoom-transition window, C4) →
   composite `prev` at its matching transform. HUD still draws live on top.

Result: the user never sees a partial bake. This closes out the "12% strip"
artifact accepted in 3.23.6 — and it works for **vanilla stepped zoom too**, so
**Phase A has standalone user value before any smooth easing exists.**

Note this is exactly the "re-present the previous frame" idea from master-plan
§0.5 P-lag branch — it was impossible in the direct-to-backbuffer architecture
and is trivial here.

---

## 6. Mouse compensation

During an active anim, the map transform is `screen = a + (tex − a)·s` where
`tex` space equals today's compensated screen space. So the pick inverse is:

```text
tex = a + (screen − a) / s          // undo composite scale
precise = tex − origin + round(render_shift)   // existing GPS comp, unchanged
```

One extra affine step in `apply_mouse_compensation`, still GPS-only, still in
feed/logic only (C11). Since `s = 1` whenever no anim is active, the extra step
is exactly identity in normal play — pan mouse behavior untouched.

Designation/ghost sync (3.23.13's `designation_sync.cpp`) consumes the same
compensated precise coords, so it inherits correctness. Still: designating
mid-anim should be regression-tested explicitly; if anything is off, the fallback
is to freeze comp during the ~150 ms anim window (master-plan Stage 4 fallback).

Minimap + FFD on commit: treat a zoom commit like a tile step (`mustmake` +
`force_full_display_count` bump), as already planned.

---

## 7. Risks and the Phase 0 probe that retires them

The single load-bearing unknown: **how DF uses render targets during map passes.**
`renderer_2d` has a `screen_tex`, and `SDL_SetRenderTarget` is already MinHook'd
(observe-only). Three scenarios:

- **S1 — target is stable (backbuffer or screen_tex) for the whole map layer:**
  ideal; we bracket first-map-pass → map-layer-end with our own target swap.
- **S2 — DF switches targets mid-pass** (e.g. bakes into intermediate textures,
  then composites): we redirect only the segments whose target is the screen
  target, passing DF's own target switches through. More bookkeeping, same idea.
- **S3 — driver/renderer doesn't support target textures:** essentially
  impossible for DF Premium's SDL renderer paths (D3D11/GL all support it), but
  probe checks `SDL_RendererInfo.flags & SDL_RENDERER_TARGETTEXTURE` anyway;
  compositor stays off without it (toggle default).

**Phase 0 (pure telemetry, ~no risk):** extend the existing trace/probe to log,
for 30 frames: every `SetRenderTarget` with the current pass index, the renderer
info flags, clip/viewport state at pass boundaries, and where the map layer ends
relative to the first HUD blit. One F9 session answers S1 vs S2 and pins the
composite anchor point.

Other known sharp edges (all with clear mitigations):

| Risk | Mitigation |
|---|---|
| SDL clip state is per-target; our clip snap must be re-applied after target swaps | Wrap target swaps with save/restore of the tracked clip (`track_clip` already exists) |
| Texture recreate on window resize / renderer reset | Recreate when `GetRendererOutputSize` changes; drop bridge (`prev` invalid) for one frame |
| `SDL_RenderClear` calls while our target is bound could wipe or miss the texture | Probe for Clear calls inside the map layer (add to Phase 0 log); clear `map_tex` ourselves at capture begin |
| Perf: one extra fullscreen texture copy per frame | Negligible next to thousands of tile blits; verify with F7 (budget: < 0.3 ms) |
| Blit classification differences when target ≠ backbuffer | None expected — classification is dst-rect based; verify with F9 `map%`/`tile%` parity vs direct mode |
| DF drawing map-layer content *after* our composite point | The existing pass gates (`g_in_main_viewport_update`, `g_in_post_viewport_map_shift`) already delimit this; F9 parity check catches leaks |

---

## 8. Phased delivery

Each phase independently shippable, each behind the `smoothpan compositor` /
`smoothpan zoom` toggles, each validated against GOLDEN_PATH plus the criteria
below. Direct mode remains one console command away at every stage.

| Phase | Deliverable | Pass criteria (F9-verifiable) |
|---|---|---|
| **0 — Probe** | Render-target/Clear/clip trace during map passes; renderer caps | S1/S2 determined; composite anchor point chosen |
| **A — Compositor at s=1** | Capture + 1:1 composite; toggle + fallback | Pixel-parity BMP diff vs direct mode (idle, pan, MMB); F9 `map%`/`tile%` unchanged; F7 delta < 0.3 ms |
| **B — Retained bridge** | Double-buffer + NARROW bridge on vanilla stepped zoom | Wheel sweep 256→64 F9: no present with map coverage shortfall; the 3.23.6 "artifact lines" gone; pan regression clean |
| **C — Smooth zoom** | `v` model, both directions, our-wheel commits, cursor anchor, dt-based ease | `s != 1.0` visible in telemetry during anim; ≥ N anim frames per step at 60 FPS; no bands (`rGap` ≤ pan baseline every frame); gps steps exactly once per crossing; world point under cursor fixed across commit |
| **D — Polish** | Anim mouse-comp inverse, scale-mode option, optional crossfade, `smoothpan zoom on/off/duration`, minimap/ffd on commit | Designate mid-anim correct; full GOLDEN_PATH matrix; ship as v1.3.0 |

Phase B is the earliest user-visible win (kills the dim-lag artifact) and proves
the whole capture/bridge machinery before any easing code exists — the
data-before-code discipline the master plan asks for.

---

## 9. Alternatives considered (and why not)

1. **Per-blit scale (master plan Stages 2–3 as written).** Workable on paper, but:
   the scale plumbing failed silently once already (C9/frozen), it must interact
   with edge-extend and clip per blit, and it faces the crack/seam risk (C13)
   that no telemetry has yet measured because scale never applied. Higher
   integration risk for a worse ceiling (no retained bridge, so the dim-lag
   artifact stays).
2. **Margin-ring bake (Phase R).** Requires influencing vanilla's allocation —
   every approach touching `dim`/alloc has crashed (C1). Dead end per evidence.
3. **`dispx_z` / `dim` forcing on commit frames.** Tried three ways
   (3.23.0/3.23.2/3.23.5); broke UI or crashed. The bridge achieves the same
   user-visible outcome with no DF writes.
4. **Skip/black-hole the NARROW present.** Without a retained frame you'd show a
   stale *backbuffer* you don't control, or a stutter. The compositor is this
   idea done properly.
5. **Scale the final present (backbuffer-level).** Scales HUD too — instant
   anti-pattern (HUD wobble).

---

## 10. One-line summary

*Capture the map layer into a texture we own, composite it with one transform:
pan stays byte-identical, zoom scale finally has a single place to apply, scale
≥ 1 + cursor anchor is provably void-free, and the retained previous frame
bridges vanilla's broken commit presents — fixing the dim-lag artifact before the
first easing frame ever ships.*

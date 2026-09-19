# Smooth Zoom (3.24.0) — retained-frame compositor + eased zoom camera

**Status:** implemented in 3.24.0, **awaiting first in-game test**. This document
is the single source of truth for how smooth zoom works now. Everything that came
before (3.12 → 3.23.x: per-blit scaling attempts, dim/dispx hacks, the postmortem,
the master plan) is in `docs/archive/` for history only — do not build on it.

**Feel target:** RimWorld-style — wheel over the map glides the view in and out
about the cursor, no snap, no black bands, HUD rock-steady, map crisp whenever it
is at rest. Pan (WASD / MMB), mouse and designation behave exactly as in 3.23.13.

---

## 1. Why every earlier attempt failed, in one paragraph

DF bakes the map into a fixed `dim_x × dim_y` buffer and blits it tile by tile
straight into the backbuffer. Scaling those thousands of blits in place never
reliably reached the screen (`frozen=100`), scaling below 1 exposed texels that
were never baked (black rings), and vanilla's zoom commit is non-atomic: for one
present the cell size has changed but `dim_x` has not, so the bake covers only
part of the screen (the NARROW frame). Fixing that by writing DF's own fields
(`dispx_z`, `vp->dim`) broke the HUD or crashed. **Once the map is on the
backbuffer it is gone — you cannot re-present it, scale it, or skip it.**

## 2. The fix: own the map layer

`compositor.cpp` redirects the SDL render target to a screen-sized texture that
SmoothPan owns for the duration of the map layer (first map-tile pass until the
HUD starts drawing). Everything DF blits in that window lands in the texture at
its screen position, in order. The per-blit sub-tile **pan** shift and edge
extend are applied inside the texture, **unchanged** from 3.23.13. The layer is
then drawn back onto DF's target with **one** `RenderCopyF`:

```text
screen = anchor + (bake − anchor) · s        s = visual_cell / baked_cell ≥ 1
```

At `s = 1` this is a pixel-exact copy (nearest filtering) — the idle map is
identical to the direct path. The HUD draws afterwards on DF's target, untouched.

Two textures are kept: the frame being captured and the **last complete frame**.
When the capture is a partial vanilla rebake (NARROW: `dim_x·cell` short of the
screen and of the last accepted frame; or, inside a zoom transition, a collapsed
blit count), the previous complete frame is composited instead, at the same
transform. That is the dim-lag fix: **no DF memory is written**, the broken
present is simply never shown. It works for vanilla stepped zoom too
(`smoothpan zoom off` keeps the bridge).

Why scale ≥ 1 never leaves a gap, even anchored at the cursor: the pixels we must
fill are the map region `R`; they sample `anchor + (R − anchor)/s`, which for
`anchor ∈ R` and `s ≥ 1` is `R` shrunk toward an interior point — a subset of the
bake. Cursor anchoring is free.

## 3. The zoom camera

`zoom_camera.cpp` keeps the split-brain model:

| Layer | State |
|---|---|
| Logic | `gps->viewport_zoom_factor` on vanilla's ladder (cells 16 24 32 40 48 56 64 px) |
| Visual | `v` — continuous visual cell, eased in log space toward the wheel target |
| Render | `s = v / cell_of_displayed_bake`, anchor frozen once per frame |

Invariant: the displayed bake's cell is always ≤ `v`, so `s ≥ 1`.

A wheel notch over the map is **consumed** (vanilla never sees it) and moves the
target one ladder step. A gesture starts with the anchor = cursor (or viewport
centre over UI / with `smoothpan zoom anchor centre`) and the world point under it.
Each frame `v` eases toward the target (`rate` 16/s ≈ 0.2 s per step), and the
bake we need is the ladder step at-or-below `min(v, target)`:

- **Zoom-in:** `v` grows on the current bake (magnified up to the next step,
  1.14× at the top of the ladder, 1.5× at the bottom). When `v` reaches the next
  cell we commit that step. The retained frame bridges the rebake presents at the
  same magnification, then the new bake continues at `s ≈ 1`. Seamless.
- **Zoom-out:** the target is below the current cell, so we commit immediately
  and hold `v` at the current cell (≤ 2 presents) until the new bake is on
  screen; it appears magnified to the old size and eases down to 1:1.
- **At rest:** `v` settles exactly on a ladder cell, `s = 1`, crisp.

A **commit** is vanilla's own path: we feed a synthetic `ZOOM_IN`/`ZOOM_OUT` key
into `viewscreen_dwarfmodest::feed` (`g_zoom_inject_depth` makes our feed
interpose pass it through). When the new `viewport_zoom_factor` is observed —
usually synchronously — the camera is moved so the world point under the anchor
is exactly where it was (`true = W_anchor − (anchor − origin)/cell_new`), and if
vanilla later recentres `window_x/y` during its rebake the correction is
re-applied. `smoothpan zoom commit direct` calls `set_viewport_zoom_factor`
instead, for comparison.

Safety: if vanilla does not deliver a commit within 15 frames the ladder limit is
learnt and the camera resyncs to the real bake; an external zoom (wheel over UI,
keyboard zoom while over UI, another plugin) resyncs immediately. There is no
pre-arming and nothing bounces.

## 4. Mouse

GPS compensation gains one step: `bake = anchor + (raw − anchor)/s`, then the
existing pan bump. Identity at `s = 1`, so pan mouse behaviour is untouched.
MMB grab divides by the visual cell while easing so the grab tracks the cursor.

## 5. Fallbacks and switches

| Command | Effect |
|---|---|
| `smoothpan zoom off` | wheel = vanilla stepped zoom; compositor still bridges rebake frames |
| `smoothpan compositor off` | the exact 3.23.13 direct path (no capture at all); smooth zoom unavailable |
| `smoothpan zoom rate <n>` | ease rate (8 slow … 30 snappy; default 16) |
| `smoothpan zoom anchor cursor\|centre` | cursor-directed (default) or vanilla-like centre zoom |
| `smoothpan zoom filter linear\|nearest` | texture filtering while easing (idle is always nearest) |
| `smoothpan zoom commit feed\|direct` | commit path (default feed = vanilla's own handler) |
| `smoothpan zoom test in\|out` | one vanilla ladder step through the commit path (isolates the commit) |
| `smoothpan zoom` / `smoothpan compositor` | status lines |

The compositor **disables itself** (console message + reason in
`smoothpan_compositor.txt`) if the renderer lacks target textures, texture
creation fails, or DF is found to use a non-1 render scale. Pan is unaffected in
every fallback.

## 6. First test protocol (3.24.0)

Deploy (`deploy.ps1`), then in DFHack:

```
enable smoothpan            → "SmoothPan 3.24.0 enabled" and, on the first map
                              frame, "SmoothPan compositor active (renderer=…)"
```

1. **Idle parity.** Do nothing for a few seconds. The map must look exactly like
   3.23.13 (crisp, HUD normal). Then `smoothpan compositor off` / `on` — no
   visible difference at all. If anything differs (blur, missing overlay, black
   strip): F9 in both states and send the two captures + `smoothpan_compositor.txt`.
2. **Pan.** WASD and MMB pan, designate a mine, place a workshop — identical to
   3.23.13.
3. **Vanilla zoom + bridge.** `smoothpan zoom off`; wheel in/out several notches
   standing still and while panning. Expect stepped zoom **without** the black
   band / artifact lines that 3.23.6 accepted.
4. **Smooth zoom.** `smoothpan zoom on`; wheel over the map: one notch, several
   fast, in then out, while panning. Expect a glide about the cursor, HUD still,
   no bands. Then hover a tile mid-ease and click / designate — must hit what is
   under the cursor.
5. **Wheel over UI** still scrolls lists (vanilla).

What to send back for anything off: `smoothpan zoomcap <label>` then F9 during
the action, plus `dfhack-config/smoothpan/smoothpan_compositor.txt` and
`smoothpan_zoom.txt`. The F9 dump now has `Compositor:` and `Zoom:` lines per
frame (`choice=1` normal, `2` bridged with retained frame, `3` forced; `end=b/f/r`
what ended the layer; `Zoom: pending/desired/landed` for commits).

## 7. Known compromises

- While easing, the map is a magnified bake (≤ 1.5×, linear filtered by default)
  for ~0.2 s per step. At rest it is always 1:1.
- Zooming out has ≤ 2 presents of hold before the glide starts (rebake latency).
- Panning during those 1–2 bridged presents shows the retained frame, i.e. the
  pan pauses for those presents.
- If DF draws HUD chrome *between* two map passes inside the map region, the
  first frame where the pass count grows composites over it once (the pass count
  is learnt per frame; steady state is order-exact).

## 8. Source map

| File | Role |
|---|---|
| `compositor.cpp/.h` | capture target, retained frame, NARROW/count completeness, single composite, layer-end heuristic, log |
| `zoom_camera.cpp/.h` | wheel target, log-space ease, ladder commits via feed, anchor invariance, mouse inverse data |
| `sdl_fn.h` | shared SDL trampolines |
| `sdl_hook.cpp` | SetRenderTarget interception, blit/fill routing into the capture, straggler transform, present finalize, F9 lines |
| `renderer_hook.cpp` | pass begin/end around map tile passes, render-end fallback |
| `smoothpan.cpp` | feed interception of ZOOM keys, per-frame zoom update/freeze, GPS mouse inverse, console commands |
| `camera.cpp` | `external_window_move` detection, MMB visual-cell divisor |

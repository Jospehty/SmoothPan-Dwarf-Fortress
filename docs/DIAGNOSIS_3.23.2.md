# 3.23.2 Capture Analysis (2026-06-04)

## TL;DR — 3.23.2 fix is broken, revert it

The 3.23.2 fix made things worse, not better. The user reports:
1. **"On enabling smoothpan all the UI gets super big"** — NEW regression
2. **"Scales as I zoom in and out"** — UI is the same value as `dispx_z`
3. **"If I stand still and zoom there's no black border issues"** — fix
   works for stand-still
4. **"If I pan and then zoom I get unrendered regions and black lines"** —
   fix does NOT actually fix pan+zoom (despite firing on the right frames)

**Recommendation: disable the fix entirely.** Keep the diag log so we
can keep investigating, but don't modify `dispx_z`. The 1-frame
dim-lag band is annoying but tolerable; the current fix introduces a
worse regression.

## Why the fix is broken

### `r2d->dispx_z` is shared between main viewport AND UI/radar

`g_src/renderer_2d.hpp` line 281 (and 337, 393, 426, 458, 493, 557,
4132) all blit at `dispx_z * x + origin_x` with width `dispx_z`. This is
the same `dispx_z` used for sprite/UI blits (line 227:
`SDL_Resize(color, dispx_z * surf->w / dispx, dispy_z * surf->h / dispy)`)
and for the radar (separate viewport but same renderer instance).

The 3.23.0/3.23.2 fix sets `dispx_z = old_cell_x` to make the main
viewport cover the full screen on the dim-lag frame. **But this also
makes the radar and UI text use that larger cell size.** Result:
- Main viewport: no band (good)
- Radar: 1-frame "super big" then back to normal (UI scaling jump)
- UI text font: 1-frame "super big" then back to normal

The 3.23.2 fix dropped the `zt>0` gate, so it fires EVERY frame (not
just the 8-frame transition window). With `r2d->dispx_z` permanently
stuck at 14 (the saved value from previous fix runs), every frame's
detection sees `dim * 14 < cur_w - 50` (e.g., 40*14=560 < 2510) and
fires. 100% of frames get the forced cell.

### Diag log confirms: fix fires on every frame in 3.23.2

```
SKIP:gate reason=zt=0:    7262   (all 3.23.1 session)
FIRED new_cell=(48,48):   5620   (3.23.2 session, z=192)
FIRED new_cell=(40,40):   4693   (3.23.2 session, z=160)
FIRED new_cell=(64,63):   2375   (3.23.2 session, z=256)
FIRED new_cell=(56,56):    878   (3.23.2 session, z=224)
FIRED new_cell=(32,32):    567   (3.23.2 session, z=128)
...
```

The 3.23.2 session (~6800 frames) has ~99% FIRED. Compare to 3.23.1
session (~7400 frames) which had 112 FIRED, all on transition frames.

### Why pan+zoom still has bands despite the fix firing

The fix changes `dispx_z` to the natural cell value (e.g., 48 for z=192).
This makes the main viewport's tile blits cover `40 * 48 = 1920` of
`2560` pixels at z=224 (the dim-lag state from the parser output). Wait,
that's still NARROW. Hmm.

Actually looking at the stress test SMOOTHPAN frame 27:
- `mvpDim=40x23, cell=56` (cell from zf/4, dim OLD)
- `vpW = 40 * 56 = 2240` (parser's "vpW" column, using zf/4)
- The fix would force `dispx_z = round-up(2560/40) = 64`
- After fix: `dim * dispx_z = 40 * 64 = 2560` — full screen, no band

So the fix IS supposed to fix the main viewport. But the user still
sees bands. Why?

Possible reasons:
1. The render happens AFTER the update_full_viewport returns. The fix
   modifies dispx_z during update_full_viewport. The actual render uses
   the modified dispx_z. So the main viewport should render at the new
   cell.
2. The restore in render interpose happens AFTER the render. So the
   render uses the forced cell. The next frame's render uses the
   restored cell.

Hmm, so the main viewport SHOULD render at the forced cell. But the
user sees bands.

Wait, maybe the bands are NOT on the main viewport. Maybe they're on
a sibling viewport (e.g., minimap, sidebar, item details panel). The
sibling viewports have different dim and cell, and they're also
affected by the dim-lag. The fix only fires on the main viewport
(`is_map_vp` check), not on the siblings.

But the user said "if I pan and then zoom I get unrendered regions and
black lines through the image as I zoom". "The image" suggests the
main map. So the bands ARE on the main map.

If the main map is supposed to be fixed, why does the user see bands?

Maybe the main map uses a different cell than dispx_z. The SMOOTHPAN
shows `viewport=... cell=48 z=192` from `get_strict_viewport_rect`
which uses `gps->viewport_zoom_factor / 4`. If the main map renders at
this cell (not dispx_z), then changing dispx_z doesn't affect the
main map.

But the g_src shows the blit code uses `dispx_z` (line 281). So the
main map DOES use dispx_z.

OK there's a contradiction. The SMOOTHPAN bottom line shows cell=48
(from zf/4). The blit code uses dispx_z. If dispx_z=14, blits are at
14px intervals. The user would see 14px tiles.

But the user reports normal-looking map content. So either:
- The user is at a zoom where zf/4 = 14 (e.g., z=56)
- OR dispx_z is actually high (not 14 as the diag log suggests)

Looking at the SMOOTHPAN, the user is at z=192, z=224, z=256 — not
z=56. So zf/4 is 48, 56, 64 — not 14.

If dispx_z=14 but the map renders at 48, then dispx_z is NOT the
main viewport's cell. The SMOOTHPAN's "cell" from zf/4 is the main
viewport's cell. dispx_z is something else (UI font? sprite scale?).

OK I think I've been misreading the situation. Let me look at the
data one more time.

In the diag log, every entry shows `cell=(14,21)`. This is `dispx_z=14,
dispy_z=21`. The 14,21 ratio is 0.667, not the 16:9 or 1:1 aspect
ratio. This is weird.

But the SMOOTHPAN shows `cell=48, 56, 64` from zf/4. The main viewport
renders at these values.

If dispx_z=14 but the main viewport renders at 48, then dispx_z is
not what the main viewport uses. The fix's force of dispx_z=48 has no
effect on the main viewport.

So the fix is:
- Firing every frame (because dim*14 < cur_w-50)
- Forcing dispx_z=48 (or whatever)
- Main viewport ignores dispx_z, renders at zf/4
- UI/radar uses dispx_z, gets bigger for 1 frame
- Render interpose restores dispx_z=14

The user sees the UI/radar "super big" flickering. But the main
viewport is unchanged (still renders at zf/4 with the dim-lag).

Wait, but the user said stand-still zoom has no black border issues.
If the main viewport renders at zf/4 (not affected by dispx_z), and
the dim-lag is between dim and zf/4, then the main viewport SHOULD
have the dim-lag band. But the user says it doesn't.

Unless... at stand-still, the dim updates to match zf/4 quickly. The
dim-lag is short. The user doesn't notice.

For pan+zoom, the dim-lag is longer. The user sees the band.

This is consistent with the original 3.23.0 analysis: dim-lag is a
real bug, fix doesn't help.

**OK, here's the actual situation**:

The 3.23.0/3.23.2 fix changes `dispx_z` which is NOT the main
viewport's cell. The fix has no effect on the main viewport's
rendering. The dim-lag is unchanged.

The fix DOES affect the UI/radar (which use dispx_z). This causes
the "super big UI" regression.

The fix's only "benefit" is that it logs FIRED on the right frames,
giving us the false impression that it's working.

## What to do

**Option A: Disable the fix** (recommended for now)
- Comment out the force
- Keep the diag log
- User gets 1-frame dim-lag band (annoying but tolerable)
- No UI/radar scaling issues
- Stable baseline

**Option B: Fix the cell at the right level**
- Need to change the main viewport's actual cell
- The cell comes from `gps->viewport_zoom_factor / 4`
- Setting zf triggers a reshape that updates dim too
- Risky, would change the actual zoom level

**Option C: Fix the dim instead of the cell**
- On the dim-lag frame, set dim to a value that makes dim*cell = cur_w
- The main viewport renders with the OLD cell and the new dim
- Requires safe dim mutation (3.13.6 landmine was about out-of-bounds
  dim; we now have safe allocation so this might be feasible)
- Doesn't affect UI/radar (which use their own dim and dispx_z)

I recommend Option A for now, then plan Option C.

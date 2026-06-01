# SmoothPan — Project Status

**Release: v1.0.0** (plugin build **3.11.46**) — first public release, user-verified for normal play.

This document is the handoff / status summary for contributors and future sessions.

---

## Current state: complete for normal play

| Area | Status | Since |
|------|--------|-------|
| Visual sub-tile pan (all edges, zoom levels) | ✅ Working | 3.11.0–26 |
| HUD / panels stable while panning | ✅ Working | 3.11.26 |
| Lower-z / off-map pass alignment | ✅ Working | 3.11.0+ |
| Mouse: world + UI clicks while panned | ✅ Working | 3.11.34 |
| Mining designation drag while panned | ✅ Working | 3.11.46 |
| Map overlays (designation preview, cursor box) | ✅ Working | 3.11.34 |
| High-FPS flat pan (~170+ FPS underground) | ✅ Working | 3.11.40–41 |
| Minimap during pan | ✅ Acceptable (lazy lag OK) | 3.11.41 |

**No open user-reported bugs** as of 3.11.46.

---

## What the plugin does

1. Intercepts WASD in fortress mode and drives a float camera (`true_x/y`, `frac_x/y`).
2. Writes integer `window_x/y` only on tile boundary crossings.
3. Shifts map SDL blits by `-render_shift` where `render_shift = frac * (z/4)`.
4. Compensates mouse via `gps->precise_mouse_x/y` in feed/logic.
5. During **live rectangle designation drags**, briefly patches `mouse_x/y` and `selection_rect` from compensated picks (never globally).
6. Manually dirties minimap + display flags DF would set on vanilla scroll.
7. Skips expensive work on flat terrain (smart ffd) and throttles minimap rebuilds (lazy mode).

---

## Version history (3.11.x)

| Version | Milestone |
|---------|-----------|
| 3.11.0–1 | Visual panning: origin-based clip, edge fixes, lower-z pass gate |
| 3.11.19 | Correct GPS bump formula (`precise` only, `cell = z/4`) |
| 3.11.26 | HUD / render stable baseline |
| 3.11.27–33 | Failed mouse auto-sync attempts (warmup, both-at-boot) — see MOUSE_SYNC.md |
| **3.11.34** | **GPS-only boot; mouse sync user-verified** |
| 3.11.35 | Minimap outline on tile-step pan (`force_full_display_count >= 1`) |
| 3.11.37–38 | Minimap mustmake tuning (80 ms throttle + pan-stop sync) |
| **3.11.39** | F7 detailed perf profiler |
| **3.11.40** | Smart FFD — conditional pan z-rebake via viewport cliff scan |
| **3.11.41** | Lazy minimap default (2 s mustmake interval); user OK with laggy minimap |
| 3.11.42 | Failed global `mouse_x/y` sync — broke UI |
| 3.11.43 | Revert to GPS-only; designation still desynced |
| 3.11.44–45 | Designation-only `mouse_x/y` + `selection_rect` sync; mining fixed, UI regressed |
| **3.11.46** | **Live-drag gating + strict UI gate — mining + UI both verified** |
| **v1.0.0** | **First public GitHub release (ships 3.11.46 DLL)** |

Pre-3.11 history (shift formula `z/4`, SDL-only mode, overscan removal): see [PIPELINE.md](PIPELINE.md).

---

## Production defaults (3.11.46)

```
smoothpan ffd smart           # skip pan z-rebake on enclosed flat terrain
smoothpan minimap lazy        # mustmake ≤ every 2 s while panning + on pan stop
mouse mode: gps               # automatic on enable
```

Fallbacks if something regresses:

```
smoothpan ffd always          # old every-frame z-rebake while panning
smoothpan minimap outline     # snappier minimap (~80 ms mustmake)
```

---

## Performance summary (user captures, 3.11.40–41)

Measured with F7 at 180 Hz target. See [PERFORMANCE.md](PERFORMANCE.md) for detail.

| Scenario | ~Pan FPS | Notes |
|----------|----------|-------|
| Cliff-heavy / mixed viewport | ~114 | Smart mode keeps ffd; same as pre-optimization |
| Flat underground enclosed | **~172** | Smart ffd skip 100%; lazy minimap reduces spikes |
| Sub-tile pan without mustmake | **~180** | Best case |
| Tile step + mustmake | ~57–87 | Main remaining hitch (now ≤1 per 2 s in lazy mode) |

SmoothPan SDL hook: **~0.4–0.7 ms** — not the bottleneck.

---

## Build & deploy

Canonical source: `dfhack/plugins/smoothpan/` inside a DFHack checkout. This repo mirrors key sources under `src/`.

```powershell
cmake --build {DFHack build dir} --target smoothpan --config Release
powershell -ExecutionPolicy Bypass -File deploy.ps1
```

In DF:

```
plugin unload smoothpan
plugin load smoothpan
enable smoothpan
```

Expect: `SmoothPan 3.11.46 enabled`

---

## Key files (canonical tree)

| File | Role |
|------|------|
| `smoothpan.cpp` | feed/logic/render hooks, commands, hotkeys |
| `designation_sync.cpp` | Designation drag mouse_x/selection_rect sync |
| `camera.cpp` | Camera physics, minimap, tile-step ffd |
| `ffd_policy.cpp` | Smart pan z-rebake |
| `sdl_hook.cpp` | SDL blit shift, F7/F9 |
| `renderer_hook.cpp` | Map-pass gate, clip snap |
| `viewport.cpp` | Blit classification, UI gate |
| `mouse_comp.cpp` | GPS boot and compensation |
| `perf.cpp` | F7 profiler |
| `version.h` + `deploy.ps1` | Version string (keep in sync) |

Full map: [ARCHITECTURE.md](ARCHITECTURE.md)

---

## Regression checklist

Before releasing changes, verify:

1. **Visual:** Pan NE/SW at max speed — smooth, no shimmer on all four edges; HUD static.
2. **Cliff edge:** Open cavern / z-drop visible — no lower-z jiggle (`ffd smart` or `always`).
3. **Mouse:** F9 shows `mouse=gps`; map click and UI tab click correct mid-pan.
4. **Designation:** Pan, stop, drag-mine — designated tiles match preview; F9 `desig drag=0` when not dragging.
5. **Minimap:** Outline eventually correct after pan stop (lazy mode OK if laggy during pan).
6. **Perf:** F7 flat hall — `ffd_r=flat` dominant; no sustained sub-60 FPS while panning.

Detailed criteria: [GOLDEN_PATH.md](GOLDEN_PATH.md), [MOUSE_SYNC.md](MOUSE_SYNC.md)

---

## Documentation index

| Doc | Purpose |
|-----|---------|
| [README.md](../README.md) | GitHub front page, quick start |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Technical deep dive |
| [PERFORMANCE.md](PERFORMANCE.md) | FFD + minimap optimization |
| [MOUSE_SYNC.md](MOUSE_SYNC.md) | Mouse + designation fix history |
| [DIAGNOSTICS.md](DIAGNOSTICS.md) | F7/F9 tooling |
| [GOLDEN_PATH.md](GOLDEN_PATH.md) | Anti-patterns |
| [PIPELINE.md](PIPELINE.md) | Render pipeline RE |
| [PLAN_3.11.md](PLAN_3.11.md) | 3.11.0 fix plan |

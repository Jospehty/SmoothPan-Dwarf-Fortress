# SmoothPan Performance

How SmoothPan achieves high-FPS panning and how to measure it.

**Version:** 3.11.41

---

## Problem statement

At 180 Hz (5.56 ms/frame), vanilla-style full display refresh during pan is too expensive. Profiling (3.11.39) showed:

| Phase | Avg frame | ~FPS |
|-------|-----------|------|
| Idle (`ffd=0`) | 6.35 ms | 167 |
| Steady pan (all frames `ffd≥2`) | ~8 ms | ~125 |
| Tile step + minimap `mustmake` | ~19 ms | ~54 |

SmoothPan's SDL hook: **~0.64 ms** — negligible.

Two dominant costs:

1. **`force_full_display_count`** during pan — forces DF to rebake all z-level viewport passes (needed at cliffs).
2. **`minimap.mustmake`** — full minimap texture rebuild on tile steps.

---

## Optimization 1: Smart FFD (3.11.40)

**File:** `ffd_policy.cpp`  
**Command:** `smoothpan ffd smart|always|off`  
**Default:** `smart`

### What `force_full_display_count` does

When SmoothPan pans sub-tile, lower-z show-through passes must stay aligned with the shifted main layer. Setting `gps->force_full_display_count >= 2` forces DF to rebake those passes each frame. Without it, **open-air cliff edges jiggle** (stale bake + SDL shift).

Tile steps always set `>= 1` for minimap rectangle updates — separate from pan z-rebake.

### Smart mode logic

Lower-z visibility depends on world tiles in view, which only changes on integer tile steps. So:

1. **On tile step** (or cache miss): scan viewport perimeter + sparse interior.
2. **Cache** result: `needs_pan_ffd` for `(window_x, window_y, window_z)`.
3. **Each pan frame:** bump ffd to 2 only if cache says cliffs/open/edge; else skip.

| Scan hit | Action |
|----------|--------|
| Off-map / invalid tile | Need ffd |
| Open air (z > 0) | Need ffd |
| Ramp, stair | Need ffd |
| Grate / hatch (`FlowPassableDown`) | Need ffd |
| Floor beside open neighbor | Need ffd (cliff) |
| Enclosed solid hall | Skip pan ffd |

### Measured results (user captures)

| Fort layout | Pan FPS | `ffd_r` tags |
|-------------|---------|--------------|
| Cliff / mixed surface | ~114 | mostly `cliff` |
| Flat underground hall | **~172** | 100% `flat`, all pan frames skipped bump |

**Note:** DF may keep `force_full_display_count` at 3 for several frames after a cliff even when smart skips new bumps — sticky counter. Pure flat runs still show large FPS gains because the viewport is simpler and rebake work is reduced when not re-triggered.

**Fallback:** `smoothpan ffd always` if any cliff jiggle appears.

---

## Optimization 2: Lazy minimap (3.11.41)

**File:** `camera.cpp` (`minimap_mark_camera_moved`)  
**Command:** `smoothpan minimap lazy|outline|full|fast`  
**Default:** `lazy` (2000 ms interval)

### Minimap flags

| Flag | Set when | Cost |
|------|----------|------|
| `minimap.update = 1` | Every tile step | Low |
| `minimap.mustmake = 1` | Full rebuild | **High (~12–20 ms)** |

### Modes

| Mode | mustmake during pan | Pan stop |
|------|---------------------|----------|
| **lazy** | At most every **2 s** (configurable) | Always sync |
| **outline** | Every **80 ms** (3.11.38 behavior) | Always sync |
| **full** | Every tile step | — |
| **fast** | Never | Outline frozen |

Custom interval:

```
smoothpan minimap interval 5000
```

Range: 80–60000 ms.

### Tradeoff

User preference (3.11.41): **minimap can lag during pan**; outline catches up on pan stop. This removes periodic 12–20 ms spikes during sustained 180 Hz pan.

---

## Measuring performance

### F7 hotkey (preferred)

1. Reload plugin, `enable smoothpan`
2. Stand still ~3 s → pan WASD ~4 s → release ~3 s
3. Read log or console summary

```
smoothpan perf summary
```

Log path: `{DF}/dfhack-config/smoothpan/smoothpan_perf.txt`

### Key summary sections

| Block | Use |
|-------|-----|
| `PERF_SUMMARY` | idle vs pan averages |
| `PERF_CATEGORIES` | `pan_ffd_skipped`, `tile_mustmake`, `tile_update_only` |
| `PERF_FFD_POLICY` | flat/cliff/open/edge skip counts |
| `PERF_HISTOGRAM` | % frames ≥180 / 144 / 120 Hz |
| `PERF_SLOWEST` | Usually `mm_m=1` tile steps |

### Per-frame tags (PERF2 lines)

| Field | Meaning |
|-------|---------|
| `ffd` | Current `force_full_display_count` |
| `ffd_skip` | Smart mode skipped pan bump this frame |
| `ffd_r` | `flat`, `cliff`, `open`, `edge`, … |
| `mm_m` | Minimap mustmake this frame |
| `hook_us` | SmoothPan SDL hook time (µs) |
| `est_df_ms` | `dt_ms - hook_us` (DF + GPU) |

---

## Realistic FPS expectations

| Goal | Achievable? |
|------|-------------|
| ~180 FPS pan in flat enclosed hall | **Yes** (user-verified ~172 avg, ~180 without mustmake) |
| ~180 FPS pan at cliff edges | **No** — full z-rebake required for correctness |
| ~180 FPS pan everywhere | **No** — idle baseline alone is ~6 ms mean |
| Smooth visuals at 180 Hz | **Yes** with smart ffd + lazy minimap |

---

## Tuning guide

**Max FPS, don't care about minimap lag:**

```
smoothpan ffd smart
smoothpan minimap lazy
smoothpan minimap interval 10000
```

**Snappier minimap, accept spikes:**

```
smoothpan minimap outline
```

**Debug cliff jiggle:**

```
smoothpan ffd off        # expect jiggle at cliffs
smoothpan ffd always     # confirm fix
```

**Compare captures:** Run F7 in same fort, same zoom, same pan path; compare `pan_all` and `tile_mustmake` in summary.

---

## Future research (not implemented)

| Idea | Status |
|------|--------|
| Actively cap / decay `ffd` on flat (not just skip bump) | Sticky `ffd=3` limits mixed-route gains |
| `ffd=1` vs `2` during pan on flat | A/B only if zero visual delta |
| Pulse ffd on cliffs | Rejected — 1-frame jiggle |
| Remove tile-step ffd=1 | Breaks minimap dirty path |

See conditional FFD plan in project history (3.11.40).

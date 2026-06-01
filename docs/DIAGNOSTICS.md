# SmoothPan Diagnostics

## F7 / F12 / smoothpan perf — detailed FPS profiler (3.11.39+)

**Zero overhead unless capturing.** Prefer **F7** if F12 is bound to screenshot.

```
smoothpan perf 450
```

Or **F7** / **F12** (450 frames ≈ 2.5s at 180Hz).

**Protocol:** stand still ~3s → pan WASD ~4s → release and stand still ~3s

Output: `{dfhack-config}/smoothpan/smoothpan_perf.txt`

### Per-frame lines (`PERF2`)

Each frame logs:

| Field | Meaning |
|-------|---------|
| `dt_ms` / `fps` | Present-to-present frame time |
| `est_df_ms` | `dt_ms - hook_us` (everything except our SDL hook) |
| `pan` / `keys` | Sub-tile pan active / WASD held |
| `ffd` | `force_full_display_count` |
| `ffd_pan` / `ffd_tile` | We bumped ffd for z-rebake / tile step |
| `ffd_skip` / `ffd_r` | Smart mode skipped pan ffd / reason tag |
| `fx` `fy` `vx` `vy` `sx` `sy` | Frac×100, vel×10, render_shift×10 |
| `tile` / `pend` | Integer tile step / pan-end minimap sync |
| `mm_u` / `mm_m` | minimap update / mustmake |
| `cls_us` / `edge_us` / `hook_us` | classify / edge-fill / total hook time |

### Summary sections

| Block | Contents |
|-------|----------|
| `PERF_SUMMARY` | Overall + idle vs pan averages |
| `PERF_HISTOGRAM` | Bucket counts vs 180/144/120/90/60 Hz targets |
| `PERF_CATEGORIES` | Avg FPS + p95 per scenario (`pan_ffd_skipped`, `pan_ffd2plus`, …) |
| `PERF_FFD_BREAKDOWN` | Avg frame time by ffd value |
| `PERF_FFD_POLICY` | Counts of skip reasons (flat, cliff, open, edge, …) |
| `PERF_PERCENTILES_MS` | p50/p95/p99 all vs pan |
| `PERF_SLOWEST` | Top 12 slowest frames with tags |

**180Hz target:** pan p95 ≤ **5.56 ms**. Compare `pan_ffd_skipped` vs `pan_ffd2plus` and `tile_mustmake` in summary.

```
smoothpan perf summary
```

Reprints last capture summary to DFHack console.

---

## smoothpan ffd — pan z-rebake policy (3.11.40+)

Controls `force_full_display_count>=2` during sub-tile pan (lower-z cliff jiggle fix). Tile-step `ffd=1` for minimap is **always** applied.

```
smoothpan ffd smart    # default — skip pan ffd on flat enclosed interior
smoothpan ffd always   # 3.11.39 behavior — ffd every pan frame
smoothpan ffd off      # debug — never pan ffd (expect cliff jiggle)
```

### A/B validation protocol

Run F7 450 frames in each mode on the **same fort** at the same zoom:

1. **Flat enclosed hall** — diagonal WASD hold. Expect `pan_ffd_skipped` fps near idle; `ffd_r=flat` on most sub-tile frames.
2. **Cliff / open cavern edge** — same capture. Expect `pan_ffd_skipped` count ≈ 0; visuals match `ffd always`.
3. **Ramp/staircase, map boundary, lower z** — golden-path visual check (see [GOLDEN_PATH.md](GOLDEN_PATH.md)).

If smart mode regresses on cliffs: `smoothpan ffd always` until reported.

---

## smoothpan minimap — pan rebuild rate (3.11.41+)

Minimap `mustmake` (full rebuild) is the main pan hitch at high FPS. Default is now **lazy**.

```
smoothpan minimap lazy      # default — mustmake at most every ~2s while panning
smoothpan minimap outline   # responsive outline (~80ms, 3.11.38)
smoothpan minimap full      # every tile step
smoothpan minimap fast      # update only (outline frozen)
smoothpan minimap interval 5000   # custom ms for lazy/outline (80-60000)
```

While panning: `minimap.update` still runs each tile step (cheap). Full `mustmake` is throttled. **Pan stop always triggers one mustmake** so the outline catches up when you release WASD.

---

## F9 telemetry (3.11.46+)

**F9** dumps ~30 frames to `{dfhack-config}/smoothpan/smoothpan_telemetry.txt`.

Key per-frame lines:

| Field | Meaning |
|-------|---------|
| `SMOOTHPAN_3.11.46` | Build version |
| `mouse=gps` | Production mouse mode |
| `comp feed=` / `rend=` | GPS compensation reason codes |
| `desig active=` `paint=` `drag=` | Designation sync state |
| `patched_mx,my=` | Temporary `mouse_x/y` during live drag (should be absent when idle) |
| `sel start=` / `end=` | `selection_rect` world coords |
| `click#` ring | Recent feed clicks with `inui`, `tile`, `expected` |

When not drag-designating, expect `desig active=0` or `drag=0`. UI tab clicks should show `inui=5` with no designation patch.

---

## F9 probe cycle (legacy — shift mode A/B)

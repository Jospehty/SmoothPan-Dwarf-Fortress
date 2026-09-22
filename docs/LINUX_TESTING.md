# SmoothPan on Linux — test brief

**Audience:** the Claude Code agent running on the player's Linux machine, plus
the player. The developer (a remote Claude session) cannot run Dwarf Fortress,
so everything it learns comes from what you send back. Work through the steps in
order, record every command's output, and return the results as described in
step 6. Budget: about 20 minutes, most of it automated.

**What you are testing:** SmoothPan 3.25.0, a DFHack plugin that adds smooth
sub-tile panning and RimWorld-style smooth zoom to Dwarf Fortress. This is the
first Linux build. The Windows version works; the Linux-specific parts are the
SDL hook installer (rewrites the GOT slots through which DF calls SDL), keyboard
polling through SDL, and the build itself.

## Ground rules

- Only touch `<DF>/hack/plugins/smoothpan.plug.so`, `<DF>/dfhack-config/smoothpan/`,
  and (optionally) `<DF>/dfhack-config/init/dfhack.init`. Nothing else in the game folder.
- You drive the running game through `dfhack-run` (DFHack's remote console,
  in the DF folder). Ask the player only for what you cannot do: launching the
  game through Steam, loading a fortress, using mouse and keyboard, and saying
  how things look and feel.
- The player's save is safe: SmoothPan never writes game data. The self-test
  pauses the game and restores the previous pause state.

## 1. Install

```bash
# From the release tarball (smoothpan-v1.3.0-linux-dfhack-<ver>.tar.gz), or a clone of the repo:
./tools/linux/install.sh
```

It finds the DF folder (Steam, Flatpak Steam, `DF_DIR=...`), reads the
installed DFHack version, and refuses to install a plugin built for a different
DFHack or a newer glibc. If it refuses, build for this machine instead:

```bash
./tools/linux/build.sh --install-deps     # uses sudo for the package manager; builds ~5–10 min the first time
```

Record the full output of whichever you ran.

## 2. Launch and enable

Ask the player to start Dwarf Fortress through Steam, load a fortress, and
leave the main map view open with no menus. Then:

```bash
DF="$(bash -c 'source tools/linux/common.sh; sp_find_df')"
cd "$DF"
./dfhack-run enable smoothpan
```

Expected: `SmoothPan 3.25.0 enabled (linux, DFHack <ver>)`. Shortly after, the
plugin prints `SmoothPan compositor active (renderer=...)` to the DFHack console
(that line is also in `dfhack-config/smoothpan/smoothpan_compositor.txt`).

Any line starting with `SmoothPan WARNING:` matters — record it.

If `dfhack-run` cannot connect, DFHack's remote server is not running. Ask the
player to open the in-game DFHack launcher (the backtick key) and type the
commands there instead, then read results from the files named below.

## 3. Automated checks

### 3a. Diagnostics

```bash
./dfhack-run smoothpan diag
```

This writes `dfhack-config/smoothpan/smoothpan_diag.txt`. Check the `CHECK` lines
and the hook table:

| Symptom | Meaning |
|---|---|
| `CHECK hooks_render_copy FAIL` | DF's calls into SDL were not found. Map shifting cannot work. The `binaries.txt` from step 6 is the key evidence. |
| `hooks: ... sites=0` for most rows | Same as above |
| `= FAILED` on an interpose line | DFHack could not attach to a DF virtual method on this build |
| `CHECK compositor FAIL (reason)` | The render-target compositor turned itself off. Pan still works; smooth zoom does not. |
| `renderer_2d: virtual_cast failed` | Unexpected renderer type |
| `(HIGHDPI: window != output)` | Window and pixel sizes differ (fractional scaling). Mouse picking may be off. |

### 3b. Self-test

Tell the player: keep the DF window visible on the fortress map, and don't
touch mouse or keyboard for about 20 seconds. Then:

```bash
./dfhack-run smoothpan selftest
while ./dfhack-run smoothpan selftest status | grep -q running; do sleep 3; done
./dfhack-run smoothpan selftest status
```

The self-test pauses the game and runs a script: idle with the compositor on,
then off (pixel parity), smooth zoom one notch away and back, two notches away
and back, pan right and left, then vanilla stepped zoom with the bridge. It
measures black gaps at the viewport edges every frame and saves screenshots.

Read `dfhack-config/smoothpan/smoothpan_selftest.txt`. The `== VERDICT ==`
section has one line per check (`OK` / `WARN` / `FAIL`) and a final
`RESULT PASS|WARN|FAIL`.

### 3c. Look at the screenshots

Open the `smoothpan_selftest_*.png` (or `.bmp`) files in
`dfhack-config/smoothpan/` and describe what you see in each, concretely:

- `01_idle_compositor_on` vs `02_idle_compositor_off`: these should be identical.
  Note any difference in the map area: blur, offset, missing overlays, black strips.
- `03`/`05`/`06` (mid-zoom): the map is part-way through a zoom. It should be
  magnified, with no black bands at any edge. The HUD must be unchanged and
  crisp.
- `04`/`07` (end of zoom): crisp map, no black edges.
- `08_pan_mid`: map mid-pan, edges filled.
- `09`/`10` (vanilla zoom commit frames): must NOT show a large black band on the
  right or bottom. That band is the bug the compositor exists to hide.

## 4. Player checks

Ask the player to do each of these and report in their own words. Record the
answers verbatim.

1. **Idle parity.** Run `./dfhack-run smoothpan compositor off`, then
   `./dfhack-run smoothpan compositor on`. Does the map change at all between the two?
2. **Pan.** Hold WASD, then drag with the middle mouse button. Is it smooth?
   Any jitter, black edges, or HUD movement?
3. **Smooth zoom.** Scroll the wheel over the map: one notch, several fast,
   in and out. Does it glide around the cursor? Any black edges, flicker, or a
   jump at the end?
4. **Wheel over UI.** Scroll over a list, such as the unit list. It should scroll the
   list, not zoom.
5. **Clicking after zoom.** Zoom, then hover a tile and designate something,
   such as dig. Does it land exactly under the cursor? Try it again during a zoom.
6. **Vanilla zoom.** Run `./dfhack-run smoothpan zoom off`, then scroll the wheel.
   Is the zoom stepped, and are there any black bands? Run `./dfhack-run smoothpan zoom on` afterwards.
7. **Performance.** Does the frame rate feel different with the plugin on?
8. **Anything else** odd.

If the player sees a glitch, have them press **F9** while it is happening,
with the DF window focused. That captures 30 frames of telemetry and
screenshots. Note which question they were on.

Tuning commands, if the player wants to try them:
`smoothpan zoom rate 10` (slower) / `24` (snappier),
`smoothpan zoom anchor centre`, `smoothpan zoom filter nearest`.

## 5. If something breaks

- **"was not built for this version of DFHack"** or **"GLIBC_x.y not found"** →
  `./tools/linux/build.sh --install-deps`
- **Game crashes on `enable smoothpan`** → restart the game, then isolate the cause:
  ```
  ./dfhack-run plugin load smoothpan
  ./dfhack-run smoothpan compositor off
  ./dfhack-run smoothpan zoom off
  ./dfhack-run enable smoothpan
  ```
  If that survives, turn the compositor back on (`smoothpan compositor on`) to
  see whether it triggers the crash. Record which step crashed.
- **Pan does not move the map at all** → `smoothpan diag` hook table (step 3a).
- **Keys do nothing** → the DF window must have keyboard focus. On Linux,
  SmoothPan reads SDL's keyboard state, not global key state.

## 6. Collect and return

```bash
./tools/linux/collect.sh
```

This creates `~/smoothpan-debug-<timestamp>/` and a `.tar.gz` of it. The bundle
contains system and graphics info, DF/DFHack versions, which libraries DF loads
and its SDL relocations, a live `smoothpan diag` if the game is running, DF logs,
crash backtraces if any, and every SmoothPan log and screenshot from the last day.

Then return the results through the repo so the developer can read them
directly:

```bash
cd <SmoothPan repo clone>
BR="linux-test-results/$(date +%Y%m%d-%H%M)"
git checkout -b "$BR"
mkdir -p test-results && cp -r ~/smoothpan-debug-<timestamp> "test-results/"
# Write test-results/<that folder>/REPORT.md containing:
#   - install/build output (step 1) and enable output (step 2)
#   - the diag CHECK lines and self-test VERDICT section
#   - your description of each screenshot (step 3c)
#   - the player's answers (step 4), verbatim
#   - anything that broke and what you tried (step 5)
find test-results -size +20M -delete   # keep the push small
git add test-results && git commit -m "Linux test results $(date +%F)" && git push -u origin "$BR"
```

Tell the player the branch name so they can pass it to the developer. If
pushing is not possible, give them the `.tar.gz` path to upload instead.

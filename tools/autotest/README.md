# SmoothPan autonomous test harness (Linux)

Drives the live game through `dfhack-run` and judges every frame from the
plugin's own telemetry (`smoothpan watch record`, pixel truth) and, for display
timing, the X Present extension on DF's window (`vblmsc`).  Nothing needs a
human at the keyboard.

Paths assume the Steam install under `~/.local/share/Steam/steamapps/common/`.

| Script | Purpose |
| --- | --- |
| `build.sh` / `redeploy.sh` | build against the installed DFHack (local gcc15, see `get_gcc15.sh`), reload the plugin in the running game |
| `dfr.sh` / `dflua.sh` | run a DFHack command / one Lua chunk in the game |
| `recenter.sh` | known state before every test: cell 48, camera on a citizen (tests must not walk the camera to the map edge) |
| `zoomrec2.sh <label> <frames> "<steps>" [nopix]` | wheel notches from inside the game loop (`out3 w160 in3`), then black / glide / corner-pop / slow-frame stats |
| `panrec.sh <label> <frames> "<steps>"` | virtual pans (`pan1,0,1200`), duplicated frames, speed spikes, edge strips; `PANREC_NOPIX=nopix` for timing without readback |
| `suite.sh <gfps> <tag>` | full regression: selftest, Z1-Z3 zoom, P1 pan, ZP zoom-while-panning |
| `glide.py <label>` | per-frame zoom trace (cell, scale, visual size, HOLD / BAKE / REV) |
| `refhist.py <label>...` | present intervals in display refreshes (cadence) |
| `capprobe.sh <gfps> <pace> <label>` | pan cadence for a graphics cap and pacing mode |
| `midshot.sh <label> <rate> "<frames>"` | screenshots mid-glide (with geometry sidecar `.txt`) |
| `vblmsc.c`, `cadtest.sh`, `jointest.sh`, `joinshow.py` | when frames were really shown (Present UST), joined to what the plugin intended |

Pixel readback (`watch record` without `nopix`) is a GPU sync per frame and
costs ~2-4% of frames a refresh; judge cadence with `nopix`.

# SmoothPan Performance

How SmoothPan achieves high-FPS panning and how to measure it.

---

## Problem statement

At 180 Hz (5.56 ms/frame), a vanilla-style full display refresh during pan is too expensive. Continuous sub-pixel panning requires the game to update its camera variables at 180fps.

SmoothPan circumvents this by separating the logical camera from the visual camera. The SDL hooks themselves execute in **~0.64 ms** — completely negligible overhead.

---

## Rendering Optimizations

### Smart FFD Policy (Force Full Display)
During a sub-tile pan, Dwarf Fortress may skip rebaking lower-z viewport passes unless `gps->force_full_display_count >= 2`. Without this, open-space lower-z tiles "jiggle" at cliff edges.
However, forcing a full display rebake on every frame costs massive performance.
**The Solution:** The Smart FFD Policy scans the viewport once per logical tile step. It detects if there are cliffs, open air, ramps, or grates visible. If the viewport is a fully enclosed flat hall, it skips the FFD bump entirely, saving massive render time.

### Lazy Minimap
Dwarf Fortress's minimap is highly expensive to rebuild (~12-20ms).
**The Solution:** During a continuous WASD pan, SmoothPan explicitly throttles the minimap `mustmake` flag. The minimap is rebuilt lazily (at most every 2 seconds). Once the user releases the pan keys, a final synchronous `mustmake` is triggered so the minimap perfectly catches up.

### Avoid Double-Buffering
We discovered that buffering the camera offset to delay execution or attempting to synchronize the camera phase in `SDL_RenderPresent` caused major pipeline stalls and visual judder.
**The Solution:** The plugin intercepts the `SDL_RenderCopy` calls directly inline, applying the math synchronously to the active frame buffer. This avoids all memory barriers and pipeline delays, keeping the framerate uncapped.

---

## Diagnostics

Use the **F7** key in-game to start a 450-frame present-to-present timing capture.
The output will be saved to `dfhack-config/smoothpan/smoothpan_perf.txt`.

Tags per frame include:
- Pan state
- FFD level
- FFD skip reason
- Minimap mustmake
- Hook microseconds

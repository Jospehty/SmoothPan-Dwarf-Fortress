# SmoothPan — Project Status

**Release: 3.12** — functional plugin, user-verified for normal play.

This document serves as a status summary and history log for the SmoothPan project.

---

## Current State

| Area | Status | Since |
|------|--------|-------|
| Visual sub-tile pan (all edges, zoom levels) | ✅ Working perfectly | 3.11.20 |
| HUD / panels stable while panning | ✅ Working perfectly | 3.11.26 |
| Mouse: world + UI clicks while panned | ✅ Working perfectly | 3.11.34 |
| Map overlays (designations, cursor box) | ✅ Working perfectly | 3.11.34 |
| High-FPS flat pan (~170+ FPS underground) | ✅ Working perfectly | 3.11.40 |
| Minimap during pan | ✅ Acceptable (lazy lag OK) | 3.11.41 |

## Version History Breakthroughs

### The Phase 3.8 / 4.0 Stabilization
The project went through multiple iterations of trying to solve UI jittering and frame desync.

1. **Initial Float Problems:** We discovered that simply shifting `SDL_RenderCopy` calls by a sub-pixel float caused severe edge shimmering on pixel art due to SDL2 rounding inconsistencies. This was fixed by rigorously casting the final calculated offset back to integer pixels.
2. **The `SDL_RenderCopyF` Trap:** We found that creatures and items were not panning smoothly with the map. This was because they are drawn with `SDL_RenderCopyF`. Hooking both APIs solved the issue.
3. **The Phase Desync Trap:** We attempted to solve UI jittering by deferring the read of the camera's `frac_x/y` offset to the end of the frame (`SDL_RenderPresent`) or by decoupling it into a separate variable. This was mathematically doomed and caused catastrophic 1-frame whole-screen juddering when the camera crossed a tile boundary. The solution was reverting to purely synchronous reads of the atomic `frac_x/y` directly inside the `SDL_RenderCopy` intercepts.

## Next Steps
The core panning and rendering synchronization is considered complete and stable. Future work may explore:
- Smooth continuous zooming (interpolating `viewport_zoom_factor`).
- Middle-mouse click-and-drag panning refinements.
- Alternative minimap caching strategies to eliminate the remaining minimap lag during heavy panning.

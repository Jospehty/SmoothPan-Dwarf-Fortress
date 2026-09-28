#pragma once

// Display cadence: what the monitor actually shows.
//
// DF presents on its own sleep-based cap with vsync off, so frames reach the
// window at irregular intervals, while the monitor refreshes on a fixed grid
// and shows the newest completed frame at each refresh.  Per-frame correctness
// (every frame drawn at the right place for its time) does not make motion
// smooth on screen if the frames land unevenly against that grid.  This module
// finds the refresh rate of the display DF's window is on and, where the GL
// stack exposes it, the vblank counter/timestamp, so the plugin can measure --
// and pace against -- the real refresh.

#include <SDL.h>
#include <cstddef>

// Render thread, once per present (cheap unless a probe is armed).
void vblank_on_present(SDL_Renderer* r);

// Arm a probe: records display info, GLX extension support and per-present
// vblank counters for the next n presents to smoothpan_vblank.txt.
void vblank_probe(int presents);

// Refresh period of the window's display in microseconds (0 = unknown).
long long vblank_refresh_us();
void vblank_status(char* buf, size_t n);

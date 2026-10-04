#pragma once

// Frame pacing against the real display refresh.
//
// DF renders with vsync off on its own sleep-based cap, so its frames reach
// the compositor at irregular intervals (measured 4.8-8.7 ms, ~160 fps average
// at a 180 cap) while a 180 Hz display refreshes every 5.56 ms and shows the
// newest frame at each refresh.  Some refreshes get no new frame at irregular
// moments, and each frame is seen 0-5.5 ms after the instant its content was
// computed for: on-screen judder, however correct each frame is on its own.
// SDL_RenderSetVSync is accepted but has no effect under XWayland here.
//
// This module:
//  * tracks the refresh grid (period + phase) from XWayland through the X
//    Present extension on DF's own window (a background thread with its own
//    X connection; XWayland reports the latest refresh count and timestamp,
//    and the lower envelope of those timestamps traces the refresh boundaries);
//  * at the start of every frame picks the refresh it will be shown on --
//    every Nth refresh, N chosen so DF can always make it -- and publishes that
//    display time, so pan and zoom compute their content for when the frame is
//    actually SEEN, not when it happens to be rendered;
//  * holds the present until just before that refresh's compositor deadline.
// Result: one new frame on exactly every Nth refresh, each showing the scene
// at its display time -- evenly spaced motion on screen.

#include <SDL.h>
#include <cstddef>

void pacing_on_frame_start(SDL_Renderer* r);   // render thread, renderer_2d::render begin
void pacing_before_present(SDL_Renderer* r);   // render thread, just before the real present
void pacing_after_present();                   // render thread, just after it

// Microseconds from now to the display time of the frame being rendered
// (0 when pacing is off or the grid is unknown).  Content should be computed
// for now + this.
long long pacing_frame_lead_us();
// Time until the frame now being presented reaches the screen (0 if not pacing).
long long pacing_us_until_target();

// 0 = off; otherwise show a new frame every `divisor` refreshes; -1 = auto.
void pacing_set_mode(int divisor);
int pacing_mode();
void pacing_status(char* buf, size_t n);
void pacing_shutdown();
// DF's frame cost is about to change step-wise (zoom commit): re-learn it.
// dir: +1 cheaper (zoom in, larger cell), -1 costlier, 0 unknown.
// new_cell: the bake cell now in effect (seeds the divisor from the last
// steady one seen there).
void pacing_note_workload_change(int dir, int new_cell);
void pacing_record(int frames, const char* label);
void pacing_set_latch(int us);

// For measurement: the grid (period and one boundary, CLOCK_MONOTONIC us).
bool pacing_grid(double* period_us, long long* boundary_us);

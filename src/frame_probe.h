#pragma once

// Ground truth from the pixels actually presented.
//
// Every other SmoothPan diagnostic describes what the plugin *believes* it drew
// (layer choice, blit counts, scale).  The 3.27.0 strobe showed every frame as
// a clean choice=1 composite while the screen flashed black/white at full rate,
// so those beliefs cannot be the safety net.  This module reads back a sparse
// cross of the finished frame just before SDL_RenderPresent and measures:
//
//   black  fraction of sampled map pixels that are near-black, ANYWHERE on the
//          sample lines (the old gap metric only saw black runs touching an
//          edge on all three lines at once, so a partial rebake block was 0);
//   d1/d2  mean luminance change vs the previous frame / the frame before
//          that.  A frame that differs a lot from the last one but matches the
//          one before it is flipping between two images: shimmer, or at full
//          contrast, a strobe.
//
// It is both an always-on watchdog (sampled in short bursts, and every frame
// around zoom activity) and, when armed with frame_probe_record, the per-frame
// instrument the tests read from dfhack-config/smoothpan/smoothpan_watch.txt.

#include <SDL.h>
#include <cstddef>

void frame_probe_on_present(SDL_Renderer* r);   // just before the real present
void frame_probe_reset();

// Sample every frame for the next `frames` presents and write one line per
// frame to smoothpan_watch.txt (appended; `label` marks the block).
// nopix: log timing and state only, no pixel readback (see frame_probe.cpp).
void frame_probe_record(int frames, const char* label, bool nopix = false);
bool frame_probe_recording();

// The strobe watchdog.  On: trips the compositor off if the presented map
// alternates between two very different images for several frames in a row.
void frame_probe_set_watchdog(bool on);
bool frame_probe_watchdog();
int frame_probe_trips();

void frame_probe_status(char* buf, size_t n);
// Test only: for the next n sampled frames, overwrite the measured black
// fraction with an alternating 0/1 pattern so the watchdog's trip path can be
// exercised without anything strobing on screen.
void frame_probe_test_strobe(int n);

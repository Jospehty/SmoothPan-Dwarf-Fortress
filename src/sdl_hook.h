#pragma once

#include <string>

bool InitSDLHooks();
void CleanupSDLHooks();

// Clip the current render target to the map's bake-grid rect [origin, origin+dim*cell]
// while a map pass is baking, so shifted tiles cannot paint into the on-screen
// left/top margin (the vanilla "few-pixel" border).  enable=false restores no-clip.
// sdl_renderer is renderer_2d::sdl_renderer (an SDL_Renderer*).
void smoothpan_apply_map_clip(void* sdl_renderer, bool enable);

extern int g_test_dump_frames;
extern int g_test_dump_delay;
extern int g_classify_log_frames;

// Arm a fresh F9-style capture (bumps per-label seq, resets counters, removes
// any stale file at the new path).  See smoothpan_arm_capture() in sdl_hook.cpp.
void smoothpan_arm_capture(int frames, int delay);

// Raw SDL mouse via unhooked trampoline (no compensation).
bool smoothpan_raw_sdl_mouse(int* x, int* y);

// Stage 1 A/B toggle: when legacy=true, the map clip is constrained on EVERY
// map-bake frame (the 3.20.0 behavior that caused black bands on vanilla zoom),
// so the user can capture before/after with identical ClipStarve telemetry.
// Default false = gated (only constrain while genuinely panning, relaxed during
// the zoom-transition window).
extern bool g_force_legacy_clip;
void smoothpan_set_legacy_clip(bool legacy);
bool smoothpan_legacy_clip();
bool sdl_shift_mode_active();  // true when shift mode is Sdl or SeqPreToolbar

// Stage 2 (3.22.0) capture hygiene: per-capture file naming + opt-in blit log.
// g_capture_label + g_capture_seq form the unique suffix of the F9 telemetry
// file (smoothpan_telemetry_<label>_<seq>.txt) and the BMP frame prefix, so
// successive F9 captures never overwrite each other.  Set via
// smoothpan zoomcap <label>.  g_blit_log_enabled gates the per-blit "Blit:"
// lines (they bloat captures 1000x; ClipStarve provides the same map_total
// figure for the clip-starvation test).  Default OFF.
extern std::string g_capture_label;
extern int g_capture_seq;
extern bool g_blit_log_enabled;
void smoothpan_set_capture_label(const char* label);
void smoothpan_set_blit_logging(bool enabled);
bool smoothpan_blit_logging();
const char* smoothpan_capture_label();

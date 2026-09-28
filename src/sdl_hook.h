#pragma once

#include <atomic>
#include <string>

bool InitSDLHooks();
void CleanupSDLHooks();

// SDL hook lifecycle.  DF's render thread keeps presenting while DFHack
// console commands run, so hook removal and re-install are coordinated through
// one atomic state, every transition a compare-and-swap:
//
//   REMOVED --sp_ensure_hooks--> LIVE --sp_request_hook_teardown--> TEARDOWN
//   TEARDOWN --present hook--> REMOVING --> REMOVED     (render thread)
//   TEARDOWN --sp_ensure_hooks--> LIVE                  (re-enable cancels)
//
// Removal happens only on the render thread, after compositor_guard_present has
// handed DF its render target back; removing on the console thread could strand
// an open capture with our texture bound (the 3.27.0 permanent strobe).  A plain
// "pending" flag was not enough: enable could clear it and skip re-installing
// while the render thread was already inside the removal, leaving the plugin
// enabled with no hooks (seen live in 3.28.2).
enum SpHookState : int { SP_HOOKS_REMOVED = 0, SP_HOOKS_LIVE = 1, SP_HOOKS_TEARDOWN = 2, SP_HOOKS_REMOVING = 3 };
extern std::atomic<int> g_sp_hook_state;
extern std::atomic<int> g_sp_vsync_request;
extern int g_sp_vsync_state;
extern int g_sp_vsync_result;
bool sp_ensure_hooks();                         // console thread; true if hooks are live
void sp_request_hook_teardown();                // any thread; LIVE -> TEARDOWN
void sp_render_thread_teardown_point();         // present hook, after the real present
void sp_force_remove_hooks();                   // plugin unload
void sp_reinstall_hooks();                      // console thread; re-resolve all hooks

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

#define NOMINMAX
#include "sdl_hook.h"
#include "camera.h"
#include "viewport.h"
#include "debug_paths.h"
#include "version.h"
#include "zoom_probe.h"
#include "probe.h"
#include "trace.h"
#include "shift_mode.h"
#include "mouse_comp.h"
#include "frame_seq.h"
#include "renderer_hook.h"
#include "perf.h"
#include "designation_sync.h"
#include "VTableInterpose.h"
#include "MinHook.h"
#include <SDL.h>
#include <cmath>
#include <windows.h>
#undef min
#undef max
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_map_portst.h"
#include "df/gamest.h"
#include "df/main_interface.h"
#include "df/enabler.h"
#include "df/renderer_2d.h"
#include "df/zoom_commands.h"
#include "df/graphic_viewportst.h"
#include <algorithm>
#include <string>
#include <cstdio>
#include <atomic>
#include <vector>

typedef int(*SDL_RenderCopy_t)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
typedef int(*SDL_RenderCopyEx_t)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*, const double, const SDL_Point*, const SDL_RendererFlip);
typedef int(*SDL_RenderCopyF_t)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_FRect*);
typedef int(*SDL_RenderCopyExF_t)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_FRect*, const double, const SDL_FPoint*, const SDL_RendererFlip);
typedef int(*SDL_GetRendererOutputSize_t)(SDL_Renderer*, int*, int*);
typedef void(*SDL_RenderPresent_t)(SDL_Renderer*);
typedef uint32_t(*SDL_GetMouseState_t)(int*, int*);
typedef uint32_t(*SDL_GetGlobalMouseState_t)(int*, int*);
typedef int(*SDL_RenderSetClipRect_t)(SDL_Renderer*, const SDL_Rect*);
typedef int(*SDL_SetRenderTarget_t)(SDL_Renderer*, SDL_Texture*);
typedef int(*SDL_RenderSetViewport_t)(SDL_Renderer*, const SDL_Rect*);

SDL_RenderCopy_t True_SDL_RenderCopy = nullptr;
SDL_RenderCopyEx_t True_SDL_RenderCopyEx = nullptr;
SDL_RenderCopyF_t True_SDL_RenderCopyF = nullptr;
SDL_RenderCopyExF_t True_SDL_RenderCopyExF = nullptr;
SDL_GetRendererOutputSize_t GetRendererOutputSize_func = nullptr;
SDL_RenderPresent_t True_SDL_RenderPresent = nullptr;
SDL_GetMouseState_t GetMouseState_func = nullptr;
SDL_GetGlobalMouseState_t GetGlobalMouseState_func = nullptr;
SDL_RenderSetClipRect_t True_SDL_RenderSetClipRect = nullptr;
SDL_SetRenderTarget_t True_SDL_SetRenderTarget = nullptr;
SDL_RenderSetViewport_t True_SDL_RenderSetViewport = nullptr;
typedef int(*SDL_RenderReadPixels_t)(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int);
typedef int(*SDL_SaveBMP_RW_t)(SDL_Surface*, SDL_RWops*, int);
typedef SDL_Surface*(*SDL_CreateRGBSurfaceWithFormat_t)(Uint32, int, int, int, Uint32);
typedef void(*SDL_FreeSurface_t)(SDL_Surface*);
typedef SDL_RWops*(*SDL_RWFromFile_t)(const char*, const char*);
SDL_RenderReadPixels_t True_SDL_RenderReadPixels = nullptr;
SDL_SaveBMP_RW_t True_SDL_SaveBMP_RW = nullptr;
SDL_CreateRGBSurfaceWithFormat_t True_SDL_CreateRGBSurfaceWithFormat = nullptr;
SDL_FreeSurface_t True_SDL_FreeSurface = nullptr;
SDL_RWFromFile_t True_SDL_RWFromFile = nullptr;

int g_test_dump_frames = 0;
int g_test_dump_delay = 0;
int g_classify_log_frames = 0;
std::string g_telemetry_log;

extern bool &is_enabled;

// Mouse-compensation diagnostics (defined in smoothpan.cpp).
extern int g_sp_comp_reason_feed;
extern int g_sp_comp_reason_rend;
extern int g_sp_comp_feed_calls;
extern int g_sp_comp_rend_calls;
extern int g_sp_comp_sx, g_sp_comp_sy;
extern int g_sp_comp_mx_before, g_sp_comp_mx_after;
extern int g_sp_comp_my_before, g_sp_comp_my_after;
extern int g_sp_comp_raw_x, g_sp_comp_raw_y;
extern int g_click_count, g_click_reason, g_click_keys;
extern int g_click_raw_x, g_click_raw_y;
extern int g_click_mx_before, g_click_mx_after;
extern int g_click_shift_x, g_click_shift_y;
extern int g_click_inui_reason;
extern int g_click_vp_left, g_click_vp_top, g_click_vp_right, g_click_vp_bottom;
extern int g_click_w_x1, g_click_w_y1, g_click_w_x2, g_click_w_y2;
extern int g_click_w_container;

extern int g_sp_gate_sdl_inui;
extern int g_sp_gate_unified_inui;
extern int g_sp_gate_mismatch;
extern int g_sp_gate_pick_x, g_sp_gate_pick_y;

extern int g_sp_mmb_held;
extern int g_sp_middle_drag;
extern int g_sp_mmb_scroll;
extern int g_sp_mmb_sticky;
extern int g_sp_mmb_gate;
extern int g_sp_mmb_dx;
extern int g_sp_mmb_dy;

extern int g_sp_desig_active;
extern int g_sp_desig_paint;
extern int g_sp_desig_drag;
extern int g_sp_desig_patched_mx, g_sp_desig_patched_my;
extern int g_sp_desig_sel_sx, g_sp_desig_sel_sy, g_sp_desig_sel_sz;
extern int g_sp_desig_sel_ex, g_sp_desig_sel_ey, g_sp_desig_sel_ez;
extern int g_sp_desig_mpos_x, g_sp_desig_mpos_y;

struct SpClickRec {
    int n, rawx, rawy, reason, inui;
    int sx, sy, mxb, mxa, myb, mya, px, py;
    int mz, bm, scroll;
    int cell, fsx100, fsy100, tx, ty;
    int gapx, gapy, tpx;
    int ex, ey, ex_t, ey_t, cx, cy, dx, dy;
    int gate_sdl, gate_unified;
};
static const int SP_CLICK_RING = 6;
extern SpClickRec g_click_ring[SP_CLICK_RING];
extern int g_click_ring_pos;

static std::atomic<bool> g_in_map_pass{false};
static int g_clip_log_remaining = 0;

static int g_frame_blit_total = 0;
static int g_frame_blit_shifted = 0;
static int g_frame_map_total = 0;
static int g_frame_map_shifted = 0;
static int g_frame_tile_total = 0;
static int g_frame_tile_shifted = 0;
static int g_frame_sprite_total = 0;
static int g_frame_sprite_shifted = 0;
static int g_frame_pass_shifted = 0;
static int g_frame_ui_leak_shifted = 0;

// --- Stage 1 (3.21.0): clip gating + starvation telemetry --------------------
// The active SDL clip rect (whatever we last handed to True_SDL_RenderSetClipRect)
// and whether a clip is currently set.  Updated at every clip call site via
// track_clip() so the blit hooks can measure how many map blits the clip would
// discard ("starvation") on vanilla's NARROW zoom-rebake frames.
static SDL_Rect g_active_clip_rect = { 0, 0, 0, 0 };
static bool g_active_clip_set = false;
static int g_frame_map_clip_out = 0;
// A/B toggle (see sdl_hook.h).  false = gated fix; true = 3.20.0 always-clip.
bool g_force_legacy_clip = false;

void smoothpan_set_legacy_clip(bool legacy) { g_force_legacy_clip = legacy; }
bool smoothpan_legacy_clip() { return g_force_legacy_clip; }

// --- Stage 2 (3.22.0) capture hygiene: per-capture file naming + blit log gate
// See sdl_hook.h.  Default label is empty (legacy single-file behavior) and
// default blit logging is OFF (huge per-blit lines; ClipStarve suffices).
std::string g_capture_label;
int g_capture_seq = 0;
bool g_blit_log_enabled = false;
void smoothpan_set_capture_label(const char* label) {
    g_capture_label = label ? label : "";
}
void smoothpan_set_blit_logging(bool enabled) { g_blit_log_enabled = enabled; }
bool smoothpan_blit_logging() { return g_blit_log_enabled; }
const char* smoothpan_capture_label() { return g_capture_label.c_str(); }

static void track_clip(const SDL_Rect* rect) {
    if (rect) { g_active_clip_rect = *rect; g_active_clip_set = true; }
    else { g_active_clip_set = false; }
}

// True only when the origin-grid clip is genuinely needed: we are actually
// panning (sub-tile shift ≥ 0.5 px on some axis) AND not inside the zoom
// transition window.  At rest (shift≈0) and across vanilla's multi-frame zoom
// rebake the clip is RELAXED so it cannot starve map blits (Problem A / black
// edge bands).  During real panning with no zoom this is unchanged from before.
static bool map_clip_should_constrain() {
    if (g_force_legacy_clip) return true;  // 3.20.0 behavior for A/B capture
    if (g_zoom_transition_frames.load(std::memory_order_relaxed) > 0) return false;
    float sx = std::fabs(g_camera.render_shift_x());
    float sy = std::fabs(g_camera.render_shift_y());
    return sx >= 0.5f || sy >= 0.5f;
}

// Count a map-class blit whose destination is fully outside the active clip
// (i.e. the clip would discard every pixel of it — direct evidence of starvation).
static void note_clip_starve(int x, int y, int w, int h, const BlitClassification& c) {
    if (c.cls != BlitClass::Map || c.in_ui) return;
    if (!g_active_clip_set) return;
    const SDL_Rect& cl = g_active_clip_rect;
    const bool fully_outside =
        x >= cl.x + cl.w || x + w <= cl.x ||
        y >= cl.y + cl.h || y + h <= cl.y;
    if (fully_outside) g_frame_map_clip_out++;
}

void smoothpan_apply_map_clip(void* sdl_renderer, bool enable) {
    if (!True_SDL_RenderSetClipRect || !sdl_renderer) return;
    SDL_Renderer* r = reinterpret_cast<SDL_Renderer*>(sdl_renderer);
    static bool s_clip_applied = false;
    if (!enable) {
        // Only restore (null) the clip if WE imposed it.  When we did not
        // constrain (at rest / during a zoom transition) we leave DF's own clip
        // state untouched — exactly like vanilla — so map blits are not starved.
        if (s_clip_applied) {
            True_SDL_RenderSetClipRect(r, nullptr);
            track_clip(nullptr);
            s_clip_applied = false;
        }
        return;
    }
    if (!sdl_shift_mode_active()) return;
    if (!map_clip_should_constrain()) return;  // relaxed: no origin clip
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;
    // Pin the left/top of the clip to the bake-grid origin (where col/row 0 is
    // actually drawn), NOT to vp.left/top (screen_x/y, a different coordinate
    // space that fluctuates frame-to-frame).  Width/height span the full map.
    SDL_Rect clip = { vp.origin_x, vp.origin_y,
                      vp.right - vp.left, vp.bottom - vp.top };
    True_SDL_RenderSetClipRect(r, &clip);
    track_clip(&clip);
    s_clip_applied = true;
}

static void apply_mouse_shift(int& mx, int& my) {
    // The ONLY correction sub-tile panning needs is to undo the visual pixel
    // shift applied to the map tiles, so the cursor maps to the tile actually
    // drawn under it.  The previous static (screen_x - origin_x) term added a
    // constant ~18px offset to every query, double-compensating and scaling
    // badly with zoom (the "off by a few tiles" desync).  Removed.
    mx += static_cast<int>(std::lround(g_camera.render_shift_x()));
    my += static_cast<int>(std::lround(g_camera.render_shift_y()));
}

static void compensate_mouse(int* x, int* y) {
    if (!x || !y || !is_enabled) return;
    if (!mouse_comp_sdl_enabled()) return;
    if (g_shift_mode == ShiftMode::None) return;

    int pick_x = 0, pick_y = 0;
    map_pick_screen_for_gate(*x, *y, &pick_x, &pick_y);
    g_sp_gate_pick_x = pick_x;
    g_sp_gate_pick_y = pick_y;

    int unified_inui = 0;
    bool compensate = mouse_gate_should_compensate(pick_x, pick_y, &unified_inui);
    g_sp_gate_unified_inui = unified_inui;

    int sdl_inui = IsMouseInUI_reason(*x, *y, nullptr);
    g_sp_gate_sdl_inui = sdl_inui;
    g_sp_gate_mismatch = (sdl_inui != unified_inui) ? 1 : 0;

    if (!compensate) return;
    apply_mouse_shift(*x, *y);
}

uint32_t Hook_SDL_GetMouseState(int* x, int* y) {
    uint32_t state = GetMouseState_func(x, y);
    compensate_mouse(x, y);
    return state;
}

uint32_t Hook_SDL_GetGlobalMouseState(int* x, int* y) {
    uint32_t state = GetGlobalMouseState_func(x, y);
    compensate_mouse(x, y);
    return state;
}

int Hook_SDL_SetRenderTarget(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (is_enabled && g_clip_log_remaining > 0 && g_test_dump_delay == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Target: %p\n", static_cast<void*>(texture));
        g_telemetry_log += buf;
        g_clip_log_remaining--;
    }
    if (g_probe_frames > 0 || probe_auto_cycle_active()) probe_note_target_event();
    if (trace_is_active()) trace_on_set_render_target(texture);
    return True_SDL_SetRenderTarget(renderer, texture);
}

int Hook_SDL_RenderSetViewport(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (is_enabled && g_clip_log_remaining > 0 && g_test_dump_delay == 0 && rect) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Viewport: (%d,%d,%d,%d)\n", rect->x, rect->y, rect->w, rect->h);
        g_telemetry_log += buf;
        g_clip_log_remaining--;
    }
    if (g_probe_frames > 0 || probe_auto_cycle_active()) probe_note_viewport_event();
    if (trace_is_active() && rect) trace_on_set_viewport(rect->x, rect->y, rect->w, rect->h);
    return True_SDL_RenderSetViewport(renderer, rect);
}

int Hook_SDL_RenderSetClipRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (is_enabled && rect && clip_rect_matches_viewport(rect)) {
        ViewportRect vp;
        if (get_strict_viewport_rect(&vp)) {
            // Only expand the clip rect when viewport-bake overscan is active
            // (window_x/y was modified to include extra tiles).  When overscan
            // is off (ShiftMode::Sdl), pass the rect unchanged — previous
            // code expanded by tile_px=192 unconditionally, which allowed
            // 4-tile-wide bleed from adjacent regions into the viewport.
            if (g_camera.overscan_active) {
                SDL_Rect expanded = *rect;
                int cell = vp.cell_size;  // one tile in pixels
                if (g_camera.overscan_tiles_x < 0) {
                    expanded.x -= cell;
                    expanded.w += cell;
                } else if (g_camera.overscan_tiles_x > 0) {
                    expanded.w += cell;
                }
                if (g_camera.overscan_tiles_y < 0) {
                    expanded.y -= cell;
                    expanded.h += cell;
                } else if (g_camera.overscan_tiles_y > 0) {
                    expanded.h += cell;
                }
                g_in_map_pass.store(true, std::memory_order_relaxed);
                if (g_probe_frames > 0 || probe_auto_cycle_active()) {
                    probe_note_clip_event();
                    probe_note_map_pass();
                }
                if (g_clip_log_remaining > 0 && g_test_dump_delay == 0) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Clip: expand (%d,%d,%d,%d) os=(%d,%d)\n",
                             expanded.x, expanded.y, expanded.w, expanded.h,
                             g_camera.overscan_tiles_x, g_camera.overscan_tiles_y);
                    g_telemetry_log += buf;
                    g_clip_log_remaining--;
                }
                track_clip(&expanded);
                return True_SDL_RenderSetClipRect(renderer, &expanded);
            }
            // Overscan off: mark the pass but leave clip rect untouched.
            g_in_map_pass.store(true, std::memory_order_relaxed);
            if (g_probe_frames > 0 || probe_auto_cycle_active()) {
                probe_note_clip_event();
                probe_note_map_pass();
            }
        }
    }

    if (is_enabled) {
        bool map_pass = rect && clip_rect_matches_viewport(rect);
        g_in_map_pass.store(map_pass, std::memory_order_relaxed);

        // PROBLEM 2 FIX (safety net): if DF sets a clip DURING a map pass, force
        // it to the bake-grid origin rect so shifted tiles can't bleed into the
        // on-screen left/top margin.  The primary clip is set proactively by
        // smoothpan_apply_map_clip() from the renderer hook (DF often sets no
        // clip at all during the pass).
        // Stage 1 (3.21.0): only force the origin snap when we are genuinely
        // panning and outside a zoom transition.  Otherwise DF's clip passes
        // through unchanged so vanilla's NARROW zoom-rebake frames are not
        // starved into black bands.
        if (g_in_main_viewport_update.load(std::memory_order_relaxed) &&
            sdl_shift_mode_active() && map_clip_should_constrain()) {
            ViewportRect vp;
            if (get_strict_viewport_rect(&vp)) {
                SDL_Rect snapped = { vp.origin_x, vp.origin_y,
                                     vp.right - vp.left, vp.bottom - vp.top };
                if (g_clip_log_remaining > 0 && g_test_dump_delay == 0) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "Clip: in=(%d,%d,%d,%d) -> origin=(%d,%d,%d,%d)\n",
                             rect ? rect->x : -1, rect ? rect->y : -1,
                             rect ? rect->w : -1, rect ? rect->h : -1,
                             snapped.x, snapped.y, snapped.w, snapped.h);
                    g_telemetry_log += buf;
                    g_clip_log_remaining--;
                }
                track_clip(&snapped);
                return True_SDL_RenderSetClipRect(renderer, &snapped);
            }
        }

        if (g_clip_log_remaining > 0 && g_test_dump_delay == 0) {
            char buf[256];
            if (rect) {
                snprintf(buf, sizeof(buf), "Clip: (%d,%d,%d,%d) map_match=%d\n",
                         rect->x, rect->y, rect->w, rect->h, map_pass ? 1 : 0);
            } else {
                snprintf(buf, sizeof(buf), "Clip: (null)\n");
            }
            g_telemetry_log += buf;
            g_clip_log_remaining--;
        }
    }
    track_clip(rect);
    return True_SDL_RenderSetClipRect(renderer, rect);
}

// Forward declaration — extend last grid tile to viewport edge (see below).
static void map_blit_extend_viewport_edges(const SDL_FRect* orig, SDL_FRect* shifted);

static void map_blit_apply_pan_shift(SDL_FRect* rect, SDL_FPoint* center_opt) {
    const float fsx = g_camera.render_shift_x();
    const float fsy = g_camera.render_shift_y();
    rect->x -= fsx;
    rect->y -= fsy;
    if (center_opt) {
        center_opt->x -= fsx;
        center_opt->y -= fsy;
    }
}

static bool should_shift_blit(const BlitClassification& c) {
    if (c.cls != BlitClass::Map) return false;
    // Post-viewport compositing (vmap=0) draws HUD sprites after the bake:
    // 168×168 portraits, 56×56 moon, etc.  Only grid-aligned map tiles belong
    // here (lower-z show-through); everything else must stay fixed.
    if (g_in_post_viewport_map_shift.load(std::memory_order_relaxed) &&
        !g_in_main_viewport_update.load(std::memory_order_relaxed)) {
        return c.tile_sized && c.tile_aligned;
    }
    return true;
}

static void record_blit_counters(const BlitClassification& c, bool shifted) {
    const bool probing = g_probe_frames > 0 || probe_auto_cycle_active();
    const bool logging = (g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0;
    if (!probing && !logging) return;

    if (probing) probe_accumulate_blit(c, shifted);

    if (!logging) return;

    g_frame_blit_total++;
    if (shifted) g_frame_blit_shifted++;

    if (shifted) {
        if (g_in_map_pass.load(std::memory_order_relaxed)) {
            g_frame_pass_shifted++;
        } else {
            g_frame_ui_leak_shifted++;
        }
    }

    if (!c.in_ui && c.intersects_viewport) {
        g_frame_map_total++;
        if (shifted) g_frame_map_shifted++;
        if (c.tile_sized) {
            g_frame_tile_total++;
            if (shifted) g_frame_tile_shifted++;
        } else {
            g_frame_sprite_total++;
            if (shifted) g_frame_sprite_shifted++;
        }
    }
}

static void log_blit_telemetry(const BlitClassification& c, int src_x, int src_y, int src_w, int src_h, int x, int y, int w, int h, bool shifted) {
    if (g_blit_log_enabled && g_test_dump_frames > 0 && g_test_dump_delay == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Blit: frame=%d dst=(%d,%d,%d,%d) src=(%d,%d,%d,%d) vpDim=(%d,%d) pass=%d vpass=%d vmap=%d vpscr=(%d,%d) shifted=%d in_ui=%d tile=%d align=%d suspect=%d\n",
                 frame_seq_frame_index(),
                 x, y, w, h,
                 src_x, src_y, src_w, src_h,
                 g_cur_pass_dim_x.load(std::memory_order_relaxed),
                 g_cur_pass_dim_y.load(std::memory_order_relaxed),
                 g_in_map_pass.load(std::memory_order_relaxed) ? 1 : 0,
                 g_viewport_pass_index.load(std::memory_order_relaxed),
                 g_cur_pass_is_map.load(std::memory_order_relaxed) ? 1 : 0,
                 g_cur_pass_screen_x.load(std::memory_order_relaxed),
                 g_cur_pass_screen_y.load(std::memory_order_relaxed),
                 shifted ? 1 : 0,
                 c.in_ui, c.tile_sized, c.tile_aligned, c.suspected_hud_false_positive);
        g_telemetry_log += buf;
    }

    if (g_classify_log_frames > 0 && c.suspected_hud_false_positive) {
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "[classify] dst=(%d,%d,%d,%d) pass=%d in_ui=%d tile=%d align=%d\n",
                 x, y, w, h, g_in_map_pass.load(std::memory_order_relaxed) ? 1 : 0,
                 c.in_ui, c.tile_sized, c.tile_aligned);
        g_telemetry_log += buf;
    }
}

static void log_blit_telemetry_f(const BlitClassification& c, float src_x, float src_y, float src_w, float src_h, float x, float y, float w, float h, bool shifted) {
    if (g_blit_log_enabled && g_test_dump_frames > 0 && g_test_dump_delay == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "BlitF: frame=%d dst=(%.2f,%.2f,%.2f,%.2f) src=(%.2f,%.2f,%.2f,%.2f) vpDim=(%d,%d) pass=%d shifted=%d in_ui=%d tile=%d\n",
                 frame_seq_frame_index(),
                 x, y, w, h,
                 src_x, src_y, src_w, src_h,
                 g_cur_pass_dim_x.load(std::memory_order_relaxed),
                 g_cur_pass_dim_y.load(std::memory_order_relaxed),
                 g_in_map_pass.load(std::memory_order_relaxed) ? 1 : 0,
                 shifted ? 1 : 0,
                 c.in_ui, c.tile_sized);
        g_telemetry_log += buf;
    }
}

// Build the per-capture telemetry file path.  Each F9 pair (arm + dump)
// yields a distinct file so back-to-back F9s never overwrite.  Format:
//   with label:    smoothpan_telemetry_<label>_<seq>.txt
//   without label: smoothpan_telemetry_<seq>.txt  (3.23.4: was .txt, no seq)
static std::string telemetry_file_name() {
    char buf[128];
    if (!g_capture_label.empty()) {
        snprintf(buf, sizeof(buf), "smoothpan_telemetry_%s_%d.txt",
                 g_capture_label.c_str(), g_capture_seq);
    } else {
        snprintf(buf, sizeof(buf), "smoothpan_telemetry_%d.txt",
                 g_capture_seq);
    }
    return buf;
}

static std::string active_log_path() {
    if (g_test_dump_frames > 0 || g_test_dump_delay > 0) {
        return smoothpan_log_path(telemetry_file_name().c_str());
    }
    return smoothpan_log_path("smoothpan_classify.txt");
}

// Build the per-capture BMP prefix (e.g. "smoothpan_frame_c3_") so frame BMPs
// never collide between successive captures.  The frame counter (which counts
// DOWN from N) is appended to this prefix by the present hook.  3.23.4:
// always include the seq so back-to-back F9s without a label don't collide.
static std::string telemetry_bmp_prefix() {
    char buf[64];
    if (!g_capture_label.empty()) {
        snprintf(buf, sizeof(buf), "smoothpan_frame_%s_c%d_",
                 g_capture_label.c_str(), g_capture_seq);
    } else {
        snprintf(buf, sizeof(buf), "smoothpan_frame_c%d_", g_capture_seq);
    }
    return buf;
}

// Arm a fresh telemetry capture: bump the per-label sequence (so the new file
// has a unique name), reset frame counters, and remove any leftover file from
// a previous (same-label) capture so we start from a clean slate.  Safe to
// call repeatedly with the same label — each call yields a new file.
void smoothpan_arm_capture(int frames, int delay) {
    // 3.23.4: always bump seq, even when label is empty, so back-to-back
    // F9s without a label still get distinct files.
    g_capture_seq++;
    g_test_dump_frames = frames;
    g_test_dump_delay = delay;
    g_classify_log_frames = 0;
    std::string log_path = smoothpan_log_path(telemetry_file_name().c_str());
    remove(log_path.c_str());
}

bool sdl_shift_mode_active() {
    return g_shift_mode == ShiftMode::Sdl || g_shift_mode == ShiftMode::SeqPreToolbar;
}

static void note_frame_blit(int x, int y, int w, int h) {
    if (is_enabled || trace_is_active()) {
        frame_seq_note_blit(x, y, w, h);
    }
}

static bool map_shift_gate_active() {
    return g_in_main_viewport_update.load(std::memory_order_relaxed) ||
           g_in_post_viewport_map_shift.load(std::memory_order_relaxed);
}

static bool process_map_blit(int src_x, int src_y, int src_w, int src_h, int x, int y, int w, int h, int* out_x, int* out_y) {
    LARGE_INTEGER q0, q1, q_cls, freq;
    const bool time_it = perf_is_active();
    if (time_it) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&q0);
    }

    auto finish = [&](bool shifted, double classify_us) {
        if (time_it) {
            QueryPerformanceCounter(&q1);
            double total_us = (q1.QuadPart - q0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
            perf_note_blit_hook(shifted, classify_us, total_us);
        }
    };

    if (trace_is_active()) { finish(false, 0); return false; }
    if (!is_enabled || !sdl_shift_mode_active()) { finish(false, 0); return false; }
    if (g_shift_mode == ShiftMode::SeqPreToolbar && !frame_seq_shift_allowed()) { finish(false, 0); return false; }
    if (!map_shift_gate_active()) { finish(false, 0); return false; }

    if (time_it) QueryPerformanceCounter(&q_cls);
    BlitClassification c = classify_blit(x, y, w, h);
    double classify_us = 0;
    if (time_it) {
        QueryPerformanceCounter(&q1);
        classify_us = (q1.QuadPart - q_cls.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
    }

    bool shifted = should_shift_blit(c);
    record_blit_counters(c, shifted);
    log_blit_telemetry(c, src_x, src_y, src_w, src_h, x, y, w, h, shifted);
    if (g_test_dump_frames > 0 || g_classify_log_frames > 0)
        note_clip_starve(x, y, w, h, c);

    if (!shifted) {
        finish(false, classify_us);
        return false;
    }

    *out_x = x - static_cast<int>(std::lround(g_camera.render_shift_x()));
    *out_y = y - static_cast<int>(std::lround(g_camera.render_shift_y()));
    finish(true, classify_us);
    return true;
}

static bool process_map_blit_f(float src_x, float src_y, float src_w, float src_h, float x, float y, float w, float h) {
    LARGE_INTEGER q0, q1, q_cls, freq;
    const bool time_it = perf_is_active();
    if (time_it) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&q0);
    }

    auto finish = [&](bool shifted, double classify_us) {
        if (time_it) {
            QueryPerformanceCounter(&q1);
            double total_us = (q1.QuadPart - q0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
            perf_note_blit_hook(shifted, classify_us, total_us);
        }
    };

    if (trace_is_active()) { finish(false, 0); return false; }
    if (!is_enabled || !sdl_shift_mode_active()) { finish(false, 0); return false; }
    if (g_shift_mode == ShiftMode::SeqPreToolbar && !frame_seq_shift_allowed()) { finish(false, 0); return false; }
    if (!map_shift_gate_active()) { finish(false, 0); return false; }

    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    int iw = static_cast<int>(std::ceil(w));
    int ih = static_cast<int>(std::ceil(h));
    if (time_it) QueryPerformanceCounter(&q_cls);
    BlitClassification c = classify_blit(ix, iy, iw, ih);
    double classify_us = 0;
    if (time_it) {
        QueryPerformanceCounter(&q1);
        classify_us = (q1.QuadPart - q_cls.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
    }
    bool shifted = should_shift_blit(c);
    record_blit_counters(c, shifted);
    log_blit_telemetry_f(c, src_x, src_y, src_w, src_h, x, y, w, h, shifted);
    if (g_test_dump_frames > 0 || g_classify_log_frames > 0)
        note_clip_starve(ix, iy, iw, ih, c);
    if (g_probe_frames > 0) probe_accumulate_blit(c, shifted);
    finish(shifted, classify_us);
    return shifted;
}

// Reads a thin horizontal strip and returns the x of the first/last pixel whose
// RGB sum exceeds `thresh`.  Used to quantify the on-screen left/right content
// border so edge shimmer is visible as a fluctuating number across frames.
static void edge_profile_scan_row(SDL_Renderer* renderer, int screen_w, int y,
                                  int* left_start, int* right_end) {
    *left_start = -1;
    *right_end = -1;
    if (!True_SDL_RenderReadPixels) return;
    const int band = 200;
    static uint32_t px[200];
    SDL_Rect lr = { 0, y, band, 1 };
    if (True_SDL_RenderReadPixels(renderer, &lr, SDL_PIXELFORMAT_ARGB8888, px, band * 4) == 0) {
        for (int i = 0; i < band; i++) {
            uint32_t p = px[i];
            int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (r + g + b > 70) { *left_start = i; break; }
        }
    }
    SDL_Rect rr = { screen_w - band, y, band, 1 };
    if (True_SDL_RenderReadPixels(renderer, &rr, SDL_PIXELFORMAT_ARGB8888, px, band * 4) == 0) {
        for (int i = band - 1; i >= 0; i--) {
            uint32_t p = px[i];
            int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (r + g + b > 70) { *right_end = (screen_w - band) + i; break; }
        }
    }
}

static void edge_profile_log(SDL_Renderer* renderer) {
    if (!GetRendererOutputSize_func) return;
    int w, h;
    if (GetRendererOutputSize_func(renderer, &w, &h) != 0) return;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;

    int rows[3] = { vp.top + vp.cell_size / 2, (vp.top + vp.bottom) / 2,
                    vp.bottom - vp.cell_size / 2 };
    char buf[256];
    snprintf(buf, sizeof(buf),
             "EdgeProfile: vp.left=%d vp.right=%d vp.top=%d vp.bottom=%d "
             "origin_x=%d shift_x=%.2f cell=%d\n",
             vp.left, vp.right, vp.top, vp.bottom,
             vp.origin_x, g_camera.render_shift_x(), vp.cell_size);
    g_telemetry_log += buf;
    int max_left_gap = 0;
    int max_right_gap = 0;
    for (int i = 0; i < 3; i++) {
        int ls, re;
        edge_profile_scan_row(renderer, w, rows[i], &ls, &re);
        const int left_gap = (ls >= 0) ? (vp.left - ls) : -1;
        const int right_gap = (re >= 0) ? (re - (vp.right - 1)) : -1;
        if (left_gap > max_left_gap) max_left_gap = left_gap;
        if (right_gap > max_right_gap) max_right_gap = right_gap;
        snprintf(buf, sizeof(buf),
                 "  row y=%d leftContentStart=%d rightContentEnd=%d "
                 "leftGap=%d rightGap=%d rightMargin=%d\n",
                 rows[i], ls, re, left_gap, right_gap,
                 (re >= 0 ? (w - 1 - re) : -1));
        g_telemetry_log += buf;
    }
}

void Hook_SDL_RenderPresent(SDL_Renderer* renderer) {
    const SmoothCamera::FrameSnapshot& snap = g_camera.last_snapshot;
    int log_overscan_x = snap.overscan_x;
    int log_overscan_y = snap.overscan_y;

    if (g_test_dump_frames > 0 || g_classify_log_frames > 0) {
        if (g_test_dump_delay > 0) {
            g_test_dump_delay--;
        } else {
            g_clip_log_remaining = 40;
            if (g_test_dump_frames > 0) {
                if (df::global::gps) {
                    edge_profile_log(renderer);
                }
            }
            if (g_test_dump_frames > 0 && True_SDL_RenderReadPixels && True_SDL_CreateRGBSurfaceWithFormat &&
                True_SDL_SaveBMP_RW && True_SDL_RWFromFile && True_SDL_FreeSurface) {
                int w, h;
                if (GetRendererOutputSize_func && GetRendererOutputSize_func(renderer, &w, &h) == 0) {
                    SDL_Surface* surface = True_SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                    if (surface) {
                        if (True_SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0) {
                            char filename[256];
                            snprintf(filename, sizeof(filename), "%s%d.bmp",
                                     telemetry_bmp_prefix().c_str(), g_test_dump_frames);
                            std::string path = smoothpan_log_path(filename);
                            SDL_RWops* rwops = True_SDL_RWFromFile(path.c_str(), "wb");
                            if (rwops) {
                                True_SDL_SaveBMP_RW(surface, rwops, 1);
                            }
                        }
                        True_SDL_FreeSurface(surface);
                    }
                }
            }

            std::string log_path = active_log_path();
            FILE* f = fopen(log_path.c_str(), "a");
            if (f) {
                float map_rate = g_frame_map_total > 0
                    ? 100.0f * static_cast<float>(g_frame_map_shifted) / static_cast<float>(g_frame_map_total)
                    : 0.0f;
                float tile_rate = g_frame_tile_total > 0
                    ? 100.0f * static_cast<float>(g_frame_tile_shifted) / static_cast<float>(g_frame_tile_total)
                    : 0.0f;
                float sprite_rate = g_frame_sprite_total > 0
                    ? 100.0f * static_cast<float>(g_frame_sprite_shifted) / static_cast<float>(g_frame_sprite_total)
                    : 0.0f;

                fprintf(f, "SMOOTHPAN_%s frame=%d fx=%.3f fy=%.3f shift=(%.2f,%.2f) px=(%d,%d) overscan=(%d,%d) os_act=%d reason=%s mode=%s mouse=%s\n",
                        SMOOTHPAN_BUILD_VERSION,
                        g_test_dump_frames > 0 ? g_test_dump_frames : g_classify_log_frames,
                        snap.frac_x, snap.frac_y,
                        snap.shift_x, snap.shift_y,
                        static_cast<int>(std::lround(snap.shift_x)), static_cast<int>(std::lround(snap.shift_y)),
                        log_overscan_x, log_overscan_y,
                        snap.overscan_active ? 1 : 0,
                        snap.overscan_reason,
                        shift_mode_name(g_shift_mode),
                        mouse_comp_effective_name());
                if (g_test_dump_frames > 0 && df::global::gps && df::global::gps->main_map_port) {
                    auto* mp = df::global::gps->main_map_port;
                    fprintf(f, "  map_port pixel_perc=(%d,%d) dim=(%d,%d)\n",
                            mp->pixel_perc_x, mp->pixel_perc_y, mp->dim_x, mp->dim_y);
                }
                if (g_test_dump_frames > 0 && df::global::gps && df::global::gps->main_viewport) {
                    auto* mvp = df::global::gps->main_viewport;
                    // 3.23.2: read cell from gps->viewport_zoom_factor/4
                    // (the natural cell for the main viewport), not from
                    // r2d->dispx_z.  The 3.23.1 diagnostic revealed that
                    // r2d->dispx_z can be a stale value (e.g. 14 when the
                    // main viewport actually renders at 48).  zf/4 is the
                    // single source of truth for the main viewport cell.
                    int cell_x = 0, cell_y = 0;
                    int zf = df::global::gps->viewport_zoom_factor;
                    if (zf > 0) {
                        cell_x = zf / 4;
                        cell_y = zf / 4;
                    }
                    fprintf(f, "  main_viewport dim=(%d,%d) cell=(%d,%d) zf=%d screen=(%d,%d)\n",
                            mvp->dim_x, mvp->dim_y,
                            cell_x, cell_y, zf,
                            mvp->screen_x, mvp->screen_y);
                }
                // Mouse diagnostics: compare the raw SDL mouse (via the original
                // trampoline, no compensation) against DF's resolved mouse tile /
                // pixel.  Lets us see whether our SDL_GetMouseState compensation
                // actually reaches DF's world hit-test (gps->mouse_x/precise_*).
                if (g_test_dump_frames > 0 && df::global::gps) {
                    int rawx = -1, rawy = -1;
                    if (GetMouseState_func) GetMouseState_func(&rawx, &rawy);
                    int origin_x = 0, origin_y = 0;
                    if (df::global::enabler) {
                        auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
                        if (r2d) { origin_x = r2d->origin_x; origin_y = r2d->origin_y; }
                    }
                    ViewportRect mvp; get_strict_viewport_rect(&mvp);
                    int origin_precise_x = df::global::gps->precise_mouse_x + origin_x;
                    int origin_precise_y = df::global::gps->precise_mouse_y + origin_y;
                    int sdl_match = (rawx == origin_precise_x && rawy == origin_precise_y) ? 1 : 0;
                    fprintf(f, "  mouse raw_sdl=(%d,%d) df_tile=(%d,%d) df_precise=(%d,%d) "
                               "shift=(%.2f,%.2f) origin=(%d,%d) screen=(%d,%d) cell=%d\n",
                            rawx, rawy,
                            df::global::gps->mouse_x, df::global::gps->mouse_y,
                            df::global::gps->precise_mouse_x, df::global::gps->precise_mouse_y,
                            g_camera.render_shift_x(), g_camera.render_shift_y(),
                            origin_x, origin_y, mvp.left, mvp.top, mvp.cell_size);
                    fprintf(f, "  mouse origin+precise=(%d,%d) sdl_match=%d gate pick=(%d,%d) "
                               "inui sdl=%d unified=%d mismatch=%d\n",
                            origin_precise_x, origin_precise_y, sdl_match,
                            g_sp_gate_pick_x, g_sp_gate_pick_y,
                            g_sp_gate_sdl_inui, g_sp_gate_unified_inui, g_sp_gate_mismatch);
                    fprintf(f, "  comp feed=%d/%dcalls rend=%d/%dcalls sx,sy=(%d,%d) "
                               "mx %d->%d my %d->%d\n",
                            g_sp_comp_reason_feed, g_sp_comp_feed_calls,
                            g_sp_comp_reason_rend, g_sp_comp_rend_calls,
                            g_sp_comp_sx, g_sp_comp_sy,
                            g_sp_comp_mx_before, g_sp_comp_mx_after,
                            g_sp_comp_my_before, g_sp_comp_my_after);
                    fprintf(f, "  mmb held=%d drag=%d scroll=%d gate=%d sticky=%d anchor_delta=(%d,%d)\n",
                            g_sp_mmb_held, g_sp_middle_drag, g_sp_mmb_scroll,
                            g_sp_mmb_gate, g_sp_mmb_sticky, g_sp_mmb_dx, g_sp_mmb_dy);
                    fprintf(f, "  zoom pan-only gps_z=%d (wheel=vanilla)\n",
                            df::global::gps ? df::global::gps->viewport_zoom_factor : 0);
                    zoom_probe_write_f9(f);
                    zoom_probe_flush_discovery_log();
                    fprintf(f, "  desig active=%d paint=%d drag=%d patched_mx,my=(%d,%d) mpos=(%d,%d) "
                               "sel start=(%d,%d,%d) end=(%d,%d,%d)\n",
                            g_sp_desig_active, g_sp_desig_paint, g_sp_desig_drag,
                            g_sp_desig_patched_mx, g_sp_desig_patched_my,
                            g_sp_desig_mpos_x, g_sp_desig_mpos_y,
                            g_sp_desig_sel_sx, g_sp_desig_sel_sy, g_sp_desig_sel_sz,
                            g_sp_desig_sel_ex, g_sp_desig_sel_ey, g_sp_desig_sel_ez);
                    // World cursor (df::global::cursor) at F9 time — DF resets
                    // this to (-30000) at end of frame, so this is usually the
                    // sentinel.  The rend_start/rend_end captures below are the
                    // authoritative values.
                    if (df::global::cursor) {
                        int cur_x = df::global::cursor->x;
                        int cur_y = df::global::cursor->y;
                        int cur_z = df::global::cursor->z;
                        int gps_px = df::global::gps ? df::global::gps->precise_mouse_x : 0;
                        int gps_py = df::global::gps ? df::global::gps->precise_mouse_y : 0;
                        int cell_sz = mvp.cell_size > 0 ? mvp.cell_size : 16;
                        int exp_x = mvp.left + gps_px / cell_sz;
                        int exp_y = mvp.top  + gps_py / cell_sz;
                        fprintf(f, "  world_cursor_f9=(%d,%d,%d) expected_from_precise_f9=(%d,%d) cell=%d vp=(%d,%d)\n",
                                cur_x, cur_y, cur_z, exp_x, exp_y, cell_sz, mvp.left, mvp.top);
                    }
                    // Cursor captured at start/end of render interpose — these
                    // are the values DF uses to anchor the building ghost.
                    fprintf(f, "  world_cursor_rend_start=(%d,%d,%d) precise_start=(%d,%d) "
                               "rend_end=(%d,%d,%d) precise_end=(%d,%d)\n",
                            g_sp_cur_rend_start_x, g_sp_cur_rend_start_y, g_sp_cur_rend_start_z,
                            g_sp_cur_precise_rend_start_x, g_sp_cur_precise_rend_start_y,
                            g_sp_cur_rend_end_x,   g_sp_cur_rend_end_y,   g_sp_cur_rend_end_z,
                            g_sp_cur_precise_rend_end_x,   g_sp_cur_precise_rend_end_y);
                    // Mode state — tells us whether user is actually in
                    // BUILDING_PLACEMENT / ZONE_PAINT / etc.
                    if (df::global::game) {
                        auto& mi = df::global::game->main_interface;
                        fprintf(f, "  mode bottom=%d desig=%d cursor_xy=(%d,%d)\n",
                                (int)mi.bottom_mode_selected,
                                (int)mi.main_designation_selected,
                                df::global::gps ? df::global::gps->mouse_x : -1,
                                df::global::gps ? df::global::gps->mouse_y : -1);
                    }
                    // Last wants_render trace — which sub-check passed/failed.
                    fprintf(f, "  wants_render last=%d drag=%d placement=%d overmap=%d in=(%d,%d)\n",
                            g_sp_wants_render_last, g_sp_wants_render_drag,
                            g_sp_wants_render_placement, g_sp_wants_render_overmap,
                            g_sp_wants_render_ux, g_sp_wants_render_uy);
                    // designation_over_map sub-check trace.
                    fprintf(f, "  overmap inui=%d widget=%d gate=%d raw=(%d,%d)\n",
                            g_sp_overmap_inui, g_sp_overmap_widget,
                            g_sp_overmap_gate, g_sp_overmap_rawx, g_sp_overmap_rawy);
                    fprintf(f, "  lastclick #%d keys=%d reason=%d raw=(%d,%d) "
                               "shift=(%d,%d) mx %d->%d inui=%d vp=[%d,%d..%d,%d] "
                               "wdg=%s[%d,%d..%d,%d]\n",
                            g_click_count, g_click_keys, g_click_reason,
                            g_click_raw_x, g_click_raw_y,
                            g_click_shift_x, g_click_shift_y,
                            g_click_mx_before, g_click_mx_after,
                            g_click_inui_reason,
                            g_click_vp_left, g_click_vp_top,
                            g_click_vp_right, g_click_vp_bottom,
                            g_click_w_container ? "C" : "L",
                            g_click_w_x1, g_click_w_y1,
                            g_click_w_x2, g_click_w_y2);
                    for (int ri = 0; ri < SP_CLICK_RING; ++ri) {
                        int idx = (g_click_ring_pos - 1 - ri + SP_CLICK_RING * 2) % SP_CLICK_RING;
                        const SpClickRec& rc = g_click_ring[idx];
                        if (rc.n == 0) continue;
                        fprintf(f, "    click#%d reason=%d inui=%d raw=(%d,%d) "
                                   "precise=(%d,%d) cell=%d tpx=%d gap=(%d,%d) "
                                   "fshift=(%.2f,%.2f) mx %d->%d / %d->%d tile=(%d,%d) "
                                   "expected=(%d,%d) trunc=(%d,%d) cursor=(%d,%d) delta=(%d,%d) "
                                   "gate sdl=%d unified=%d "
                                   "mzone=%d bmode=%d scroll=%d\n",
                                rc.n, rc.reason, rc.inui, rc.rawx, rc.rawy,
                                rc.px, rc.py, rc.cell, rc.tpx, rc.gapx, rc.gapy,
                                rc.fsx100 / 100.0, rc.fsy100 / 100.0,
                                rc.mxb, rc.mxa, rc.myb, rc.mya,
                                rc.tx, rc.ty,
                                rc.ex, rc.ey, rc.ex_t, rc.ey_t,
                                rc.cx, rc.cy, rc.dx, rc.dy,
                                rc.gate_sdl, rc.gate_unified,
                                rc.mz, rc.bm, rc.scroll);
                    }
                }
                fprintf(f, "  blits=%d shifted=%d pass_shift=%d ui_leak=%d map=%d/%d (%.1f%%) tile=%d/%d (%.1f%%) sprite=%d/%d (%.1f%%) log=%s\n",
                        g_frame_blit_total, g_frame_blit_shifted,
                        g_frame_pass_shifted, g_frame_ui_leak_shifted,
                        g_frame_map_shifted, g_frame_map_total, map_rate,
                        g_frame_tile_shifted, g_frame_tile_total, tile_rate,
                        g_frame_sprite_shifted, g_frame_sprite_total, sprite_rate,
                        log_path.c_str());
                // Stage 1 (3.21.0) zoom black-band diagnostics.  ztrans>0 means
                // we are inside the post-zoom transition window (clip relaxed);
                // constrain=1 means the origin clip WAS imposed this frame.
                // map_clipped_out>0 is direct evidence the clip discarded map
                // blits (Problem A / black bands).
                fprintf(f, "  ZoomDiag: gps_z=%d prev_z=%d ztrans=%d constrain=%d clip_set=%d shift=(%.2f,%.2f)\n",
                        df::global::gps ? df::global::gps->viewport_zoom_factor : 0,
                        g_zoom_prev_z.load(std::memory_order_relaxed),
                        g_zoom_transition_frames.load(std::memory_order_relaxed),
                        map_clip_should_constrain() ? 1 : 0,
                        g_active_clip_set ? 1 : 0,
                        g_camera.render_shift_x(), g_camera.render_shift_y());
                fprintf(f, "  ClipStarve: clip=(%d,%d,%d,%d) set=%d map_total=%d map_clipped_out=%d\n",
                        g_active_clip_rect.x, g_active_clip_rect.y,
                        g_active_clip_rect.w, g_active_clip_rect.h,
                        g_active_clip_set ? 1 : 0,
                        g_frame_map_total, g_frame_map_clip_out);
                g_frame_blit_total = 0;
                g_frame_blit_shifted = 0;
                g_frame_map_total = 0;
                g_frame_map_shifted = 0;
                g_frame_tile_total = 0;
                g_frame_tile_shifted = 0;
                g_frame_sprite_total = 0;
                g_frame_sprite_shifted = 0;
                g_frame_pass_shifted = 0;
                g_frame_ui_leak_shifted = 0;
                g_frame_map_clip_out = 0;
                if (g_test_dump_frames > 0) {
                    ViewportRect vp;
                    if (get_strict_viewport_rect(&vp)) {
                        fprintf(f, "  viewport=(%d,%d,%d,%d) cell=%d z=%d\n",
                                vp.left, vp.top, vp.right, vp.bottom, vp.cell_size, vp.tile_px);
                    }
                }
                fprintf(f, "%s", g_telemetry_log.c_str());
                fclose(f);
            }

            if (g_test_dump_frames > 0) g_test_dump_frames--;
            if (g_classify_log_frames > 0) g_classify_log_frames--;
        }
        g_telemetry_log.clear();
    }

    g_frame_blit_total = 0;
    g_frame_blit_shifted = 0;
    g_frame_map_total = 0;
    g_frame_map_shifted = 0;
    g_frame_tile_total = 0;
    g_frame_tile_shifted = 0;
    g_frame_sprite_total = 0;
    g_frame_sprite_shifted = 0;
    g_frame_pass_shifted = 0;
    g_frame_ui_leak_shifted = 0;
    g_frame_map_clip_out = 0;

    // Stage 1: count down the post-zoom transition window once per present.
    {
        int zt = g_zoom_transition_frames.load(std::memory_order_relaxed);
        if (zt > 0) g_zoom_transition_frames.store(zt - 1, std::memory_order_relaxed);
    }

    g_camera.end_render_overscan();

    probe_on_present();
    trace_on_present();
    frame_seq_on_present();
    renderer_hook_on_present();
    g_in_map_pass.store(false, std::memory_order_relaxed);

    perf_on_present(is_enabled);

    True_SDL_RenderPresent(renderer);
}

int Hook_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) {
    if (dstrect) {
        note_frame_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        if (trace_is_active()) {
            trace_on_sdl_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        }
    }
    if (dstrect && True_SDL_RenderCopyF) {
        int sx = 0, sy = 0;
        int src_x = srcrect ? srcrect->x : 0;
        int src_y = srcrect ? srcrect->y : 0;
        int src_w = srcrect ? srcrect->w : 0;
        int src_h = srcrect ? srcrect->h : 0;
        if (process_map_blit(src_x, src_y, src_w, src_h, dstrect->x, dstrect->y, dstrect->w, dstrect->h, &sx, &sy)) {
            SDL_FRect orig_f  = { static_cast<float>(dstrect->x), static_cast<float>(dstrect->y),
                                  static_cast<float>(dstrect->w), static_cast<float>(dstrect->h) };
            SDL_FRect shifted = orig_f;
            map_blit_apply_pan_shift(&shifted, nullptr);
            map_blit_extend_viewport_edges(&orig_f, &shifted);
            return True_SDL_RenderCopyF(renderer, texture, srcrect, &shifted);
        }
    }
    return True_SDL_RenderCopy(renderer, texture, srcrect, dstrect);
}

int Hook_SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip) {
    if (dstrect) {
        note_frame_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        if (trace_is_active()) {
            trace_on_sdl_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        }
    }
    if (dstrect && True_SDL_RenderCopyExF) {
        int sx = 0, sy = 0;
        int src_x = srcrect ? srcrect->x : 0;
        int src_y = srcrect ? srcrect->y : 0;
        int src_w = srcrect ? srcrect->w : 0;
        int src_h = srcrect ? srcrect->h : 0;
        if (process_map_blit(src_x, src_y, src_w, src_h, dstrect->x, dstrect->y, dstrect->w, dstrect->h, &sx, &sy)) {
            SDL_FRect orig_f  = { static_cast<float>(dstrect->x), static_cast<float>(dstrect->y),
                                  static_cast<float>(dstrect->w), static_cast<float>(dstrect->h) };
            SDL_FRect shifted = orig_f;
            SDL_FPoint center_f;
            SDL_FPoint* center_fp = nullptr;
            if (center) {
                center_f.x = static_cast<float>(center->x);
                center_f.y = static_cast<float>(center->y);
                center_fp = &center_f;
            }
            map_blit_apply_pan_shift(&shifted, center_fp);
            map_blit_extend_viewport_edges(&orig_f, &shifted);
            return True_SDL_RenderCopyExF(renderer, texture, srcrect, &shifted, angle, center_fp, flip);
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect, angle, center, flip);
}

// Last column/row tiles: stretch dst to the viewport edge in the same blit as
// the pan shift so gaps track render state every frame (no second pass).
static void map_blit_extend_viewport_edges(const SDL_FRect* orig, SDL_FRect* shifted) {
    if (!orig || !shifted || orig->w <= 0.0f || orig->h <= 0.0f) return;

    const float sx = g_camera.render_shift_x();
    const float sy = g_camera.render_shift_y();

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;

    int origin_x = 0, origin_y = 0;
    if (df::global::enabler) {
        auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
        if (r2d) { origin_x = r2d->origin_x; origin_y = r2d->origin_y; }
    }

    const float tile_right  = static_cast<float>(origin_x + (vp.right  - vp.left));
    const float tile_bottom = static_cast<float>(origin_y + (vp.bottom - vp.top));

    const bool is_first_col = std::abs(orig->x - static_cast<float>(origin_x)) < 2.0f;
    const bool is_last_col  = std::abs(orig->x + orig->w - tile_right)  < 2.0f;
    const bool is_first_row = std::abs(orig->y - static_cast<float>(origin_y)) < 2.0f;
    const bool is_last_row  = std::abs(orig->y + orig->h - tile_bottom) < 2.0f;

    const bool extend_right = is_last_col && sx >= 0.5f;
    const bool extend_bottom = is_last_row && sy >= 0.5f;
    const bool extend_left = is_first_col && sx <= -0.5f;
    const bool extend_top = is_first_row && sy <= -0.5f;

    if (!extend_right && !extend_bottom && !extend_left && !extend_top) return;

    if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "EdgeScan: orig=(%.0f,%.0f +%.0fx%.0f) vp=(%d..%d,%d..%d) "
            "origin=(%d,%d) tr=%.0f tb=%.0f sx=%.1f sy=%.1f "
            "L=%d R=%d T=%d B=%d\n",
            orig->x, orig->y, orig->w, orig->h,
            vp.left, vp.right, vp.top, vp.bottom,
            origin_x, origin_y, tile_right, tile_bottom,
            sx, sy,
            extend_left ? 1 : 0, extend_right ? 1 : 0,
            extend_top ? 1 : 0, extend_bottom ? 1 : 0);
        g_telemetry_log += buf;
    }

    LARGE_INTEGER edge_t0, edge_t1, edge_freq;
    const bool time_edge = perf_is_active();
    if (time_edge) {
        QueryPerformanceFrequency(&edge_freq);
        QueryPerformanceCounter(&edge_t0);
    }

    if (extend_left) {
        const float target_x = static_cast<float>(vp.left);
        const float extra = shifted->x - target_x;
        shifted->x = target_x;
        shifted->w += extra;
        if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  EdgeScale: left +%.1f w -> %.1f\n", extra, shifted->w);
            g_telemetry_log += buf;
        }
    }

    if (extend_top) {
        const float target_y = static_cast<float>(vp.top);
        const float extra = shifted->y - target_y;
        shifted->y = target_y;
        shifted->h += extra;
        if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  EdgeScale: top +%.1f h -> %.1f\n", extra, shifted->h);
            g_telemetry_log += buf;
        }
    }

    if (extend_right) {
        const float target_w = static_cast<float>(vp.right) - shifted->x;
        if (target_w > shifted->w + 0.5f) {
            if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  EdgeScale: right w %.1f -> %.1f (gap=%.1f)\n",
                         shifted->w, target_w, target_w - shifted->w);
                g_telemetry_log += buf;
            }
            shifted->w = target_w;
        }
    }

    if (extend_bottom) {
        const float target_h = static_cast<float>(vp.bottom) - shifted->y;
        if (target_h > shifted->h + 0.5f) {
            if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  EdgeScale: bottom h %.1f -> %.1f (gap=%.1f)\n",
                         shifted->h, target_h, target_h - shifted->h);
                g_telemetry_log += buf;
            }
            shifted->h = target_h;
        }
    }

    if (time_edge) {
        QueryPerformanceCounter(&edge_t1);
        double us = (edge_t1.QuadPart - edge_t0.QuadPart) * 1e6 / static_cast<double>(edge_freq.QuadPart);
        perf_note_edge_fill(us);
    }
}

int Hook_SDL_RenderCopyF(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect) {
    if (dstrect) {
        note_frame_blit(static_cast<int>(std::floor(dstrect->x)),
                        static_cast<int>(std::floor(dstrect->y)),
                        static_cast<int>(std::ceil(dstrect->w)),
                        static_cast<int>(std::ceil(dstrect->h)));
        if (trace_is_active()) {
            trace_on_sdl_blit(static_cast<int>(std::floor(dstrect->x)),
                              static_cast<int>(std::floor(dstrect->y)),
                              static_cast<int>(std::ceil(dstrect->w)),
                              static_cast<int>(std::ceil(dstrect->h)));
        }
    }
    if (dstrect && True_SDL_RenderCopyF) {
        float src_x = srcrect ? static_cast<float>(srcrect->x) : 0.0f;
        float src_y = srcrect ? static_cast<float>(srcrect->y) : 0.0f;
        float src_w = srcrect ? static_cast<float>(srcrect->w) : 0.0f;
        float src_h = srcrect ? static_cast<float>(srcrect->h) : 0.0f;
        if (process_map_blit_f(src_x, src_y, src_w, src_h, dstrect->x, dstrect->y, dstrect->w, dstrect->h)) {
            SDL_FRect orig_f = *dstrect;
            SDL_FRect shifted = orig_f;
            map_blit_apply_pan_shift(&shifted, nullptr);
            map_blit_extend_viewport_edges(&orig_f, &shifted);
            return True_SDL_RenderCopyF(renderer, texture, srcrect, &shifted);
        }
    }
    return True_SDL_RenderCopyF(renderer, texture, srcrect, dstrect);
}

int Hook_SDL_RenderCopyExF(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect, const double angle, const SDL_FPoint* center, const SDL_RendererFlip flip) {
    if (dstrect) {
        note_frame_blit(static_cast<int>(std::floor(dstrect->x)),
                        static_cast<int>(std::floor(dstrect->y)),
                        static_cast<int>(std::ceil(dstrect->w)),
                        static_cast<int>(std::ceil(dstrect->h)));
        if (trace_is_active()) {
            trace_on_sdl_blit(static_cast<int>(std::floor(dstrect->x)),
                              static_cast<int>(std::floor(dstrect->y)),
                              static_cast<int>(std::ceil(dstrect->w)),
                              static_cast<int>(std::ceil(dstrect->h)));
        }
    }
    if (dstrect && True_SDL_RenderCopyExF) {
        float src_x = srcrect ? static_cast<float>(srcrect->x) : 0.0f;
        float src_y = srcrect ? static_cast<float>(srcrect->y) : 0.0f;
        float src_w = srcrect ? static_cast<float>(srcrect->w) : 0.0f;
        float src_h = srcrect ? static_cast<float>(srcrect->h) : 0.0f;
        if (process_map_blit_f(src_x, src_y, src_w, src_h, dstrect->x, dstrect->y, dstrect->w, dstrect->h)) {
            SDL_FRect orig_f = *dstrect;
            SDL_FRect shifted = orig_f;
            SDL_FPoint center_f;
            SDL_FPoint* center_fp = nullptr;
            if (center) {
                center_f = *center;
                center_fp = &center_f;
            }
            map_blit_apply_pan_shift(&shifted, center_fp);
            map_blit_extend_viewport_edges(&orig_f, &shifted);
            return True_SDL_RenderCopyExF(renderer, texture, srcrect, &shifted, angle, center_fp, flip);
        }
    }
    return True_SDL_RenderCopyExF(renderer, texture, srcrect, dstrect, angle, center, flip);
}

bool InitSDLHooks() {
    if (MH_Initialize() != MH_OK) return false;

    HMODULE sdl_module = GetModuleHandleA("SDL2.dll");
    if (!sdl_module) return false;

    void* render_copy_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopy");
    void* render_copy_ex_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyEx");
    void* render_present_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderPresent");
    void* get_mouse_state_addr = (void*)GetProcAddress(sdl_module, "SDL_GetMouseState");
    void* get_global_mouse_state_addr = (void*)GetProcAddress(sdl_module, "SDL_GetGlobalMouseState");
    void* render_set_clip_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderSetClipRect");
    void* set_render_target_addr = (void*)GetProcAddress(sdl_module, "SDL_SetRenderTarget");
    void* render_set_viewport_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderSetViewport");

    GetRendererOutputSize_func = (SDL_GetRendererOutputSize_t)GetProcAddress(sdl_module, "SDL_GetRendererOutputSize");
    True_SDL_RenderReadPixels = (SDL_RenderReadPixels_t)GetProcAddress(sdl_module, "SDL_RenderReadPixels");
    True_SDL_SaveBMP_RW = (SDL_SaveBMP_RW_t)GetProcAddress(sdl_module, "SDL_SaveBMP_RW");
    True_SDL_CreateRGBSurfaceWithFormat = (SDL_CreateRGBSurfaceWithFormat_t)GetProcAddress(sdl_module, "SDL_CreateRGBSurfaceWithFormat");
    True_SDL_FreeSurface = (SDL_FreeSurface_t)GetProcAddress(sdl_module, "SDL_FreeSurface");
    True_SDL_RWFromFile = (SDL_RWFromFile_t)GetProcAddress(sdl_module, "SDL_RWFromFile");
    True_SDL_RenderCopyF = (SDL_RenderCopyF_t)GetProcAddress(sdl_module, "SDL_RenderCopyF");
    True_SDL_RenderCopyExF = (SDL_RenderCopyExF_t)GetProcAddress(sdl_module, "SDL_RenderCopyExF");

    void* render_copy_f_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyF");
    void* render_copy_ex_f_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyExF");

    if (render_copy_addr) MH_CreateHook(render_copy_addr, &Hook_SDL_RenderCopy, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopy));
    if (render_copy_ex_addr) MH_CreateHook(render_copy_ex_addr, &Hook_SDL_RenderCopyEx, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyEx));
    if (render_copy_f_addr) MH_CreateHook(render_copy_f_addr, &Hook_SDL_RenderCopyF, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyF));
    if (render_copy_ex_f_addr) MH_CreateHook(render_copy_ex_f_addr, &Hook_SDL_RenderCopyExF, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyExF));
    if (render_present_addr) MH_CreateHook(render_present_addr, &Hook_SDL_RenderPresent, reinterpret_cast<LPVOID*>(&True_SDL_RenderPresent));
    if (get_mouse_state_addr) MH_CreateHook(get_mouse_state_addr, &Hook_SDL_GetMouseState, reinterpret_cast<LPVOID*>(&GetMouseState_func));
    if (get_global_mouse_state_addr) MH_CreateHook(get_global_mouse_state_addr, &Hook_SDL_GetGlobalMouseState, reinterpret_cast<LPVOID*>(&GetGlobalMouseState_func));
    if (render_set_clip_addr) MH_CreateHook(render_set_clip_addr, &Hook_SDL_RenderSetClipRect, reinterpret_cast<LPVOID*>(&True_SDL_RenderSetClipRect));
    if (set_render_target_addr) MH_CreateHook(set_render_target_addr, &Hook_SDL_SetRenderTarget, reinterpret_cast<LPVOID*>(&True_SDL_SetRenderTarget));
    if (render_set_viewport_addr) MH_CreateHook(render_set_viewport_addr, &Hook_SDL_RenderSetViewport, reinterpret_cast<LPVOID*>(&True_SDL_RenderSetViewport));

    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

bool smoothpan_raw_sdl_mouse(int* x, int* y) {
    if (!GetMouseState_func || !x || !y) return false;
    GetMouseState_func(x, y);
    return true;
}

void CleanupSDLHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

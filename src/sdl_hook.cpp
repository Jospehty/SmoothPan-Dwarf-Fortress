#define NOMINMAX
#include "sdl_hook.h"
#include "camera.h"
#include "viewport.h"
#include "debug_paths.h"
#include "version.h"
#include "probe.h"
#include "trace.h"
#include "shift_mode.h"
#include "frame_seq.h"
#include "renderer_hook.h"
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
typedef int(*SDL_PollEvent_t)(SDL_Event*);
typedef int(*SDL_PeepEvents_t)(SDL_Event*, int, SDL_eventaction, Uint32, Uint32);
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
SDL_PollEvent_t True_SDL_PollEvent = nullptr;
SDL_PeepEvents_t True_SDL_PeepEvents = nullptr;
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


static const int SP_CLICK_RING = 6;


static bool sdl_shift_mode_active();

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

void smoothpan_apply_map_clip(void* sdl_renderer, bool enable) {
    if (!True_SDL_RenderSetClipRect || !sdl_renderer) return;
    SDL_Renderer* r = reinterpret_cast<SDL_Renderer*>(sdl_renderer);
    if (!enable) {
        True_SDL_RenderSetClipRect(r, nullptr);
        return;
    }
    if (!sdl_shift_mode_active()) return;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;
    // Pin the left/top of the clip to the bake-grid origin (where col/row 0 is
    // actually drawn), NOT to vp.left/top (screen_x/y, a different coordinate
    // space that fluctuates frame-to-frame).  Width/height span the full map.
    SDL_Rect clip = { vp.origin_x, vp.origin_y,
                      vp.right - vp.left, vp.bottom - vp.top };
    True_SDL_RenderSetClipRect(r, &clip);
}

static bool should_spoof_mouse(int x, int y, int* shift_x, int* shift_y) {
    if (!is_enabled || g_shift_mode == ShiftMode::None) return false;
    
    // We must use last_snapshot.shift_x/y rather than calculating it manually
    // from frac_x, because the map rendering factors in overscan_tiles_x/y.
    // If overscan_tiles is 1, the map is drawn a full tile offset in the other direction.
    // last_snapshot accurately records the exact pixel offset applied to the visual
    // elements during the most recent render frame (which is what the user reacted to).
    float sx_f = g_camera.last_snapshot.shift_x;
    float sy_f = g_camera.last_snapshot.shift_y;
    if (sx_f == 0.0f && sy_f == 0.0f) return false;

    // Only spoof if the physical mouse is NOT in a UI region.
    // IsMouseInUI checks strict viewport bounds, so vanilla bottom tabs are protected.
    if (IsMouseInUI(x, y)) return false;

    if (shift_x) *shift_x = static_cast<int>(std::lround(sx_f));
    if (shift_y) *shift_y = static_cast<int>(std::lround(sy_f));
    return true;
}

static void compensate_mouse(int* x, int* y) {
    if (!x || !y) return;
    int sx, sy;
    if (should_spoof_mouse(*x, *y, &sx, &sy)) {
        *x += sx;
        *y += sy;
    }
}


int Hook_SDL_PollEvent(SDL_Event* event) {
    int ret = True_SDL_PollEvent(event);
    if (ret && event) {
        int sx, sy;
        if (event->type == SDL_MOUSEMOTION) {
            if (should_spoof_mouse(event->motion.x, event->motion.y, &sx, &sy)) {
                event->motion.x += sx;
                event->motion.y += sy;
            }
        } else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
            if (should_spoof_mouse(event->button.x, event->button.y, &sx, &sy)) {
                event->button.x += sx;
                event->button.y += sy;
            }
        }
    }
    return ret;
}

int Hook_SDL_PeepEvents(SDL_Event* events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType) {
    int ret = True_SDL_PeepEvents(events, numevents, action, minType, maxType);
    if (ret > 0 && events && action == SDL_GETEVENT) {
        for (int i = 0; i < ret; ++i) {
            int sx, sy;
            if (events[i].type == SDL_MOUSEMOTION) {
                if (should_spoof_mouse(events[i].motion.x, events[i].motion.y, &sx, &sy)) {
                    events[i].motion.x += sx;
                    events[i].motion.y += sy;
                }
            } else if (events[i].type == SDL_MOUSEBUTTONDOWN || events[i].type == SDL_MOUSEBUTTONUP) {
                if (should_spoof_mouse(events[i].button.x, events[i].button.y, &sx, &sy)) {
                    events[i].button.x += sx;
                    events[i].button.y += sy;
                }
            }
        }
    }
    return ret;
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
        if (g_in_main_viewport_update.load(std::memory_order_relaxed) && sdl_shift_mode_active()) {
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
    return True_SDL_RenderSetClipRect(renderer, rect);
}

// Forward declaration — defined just before Hook_SDL_RenderCopyF below.
static void fill_edge_gap(SDL_Renderer* renderer, SDL_Texture* texture,
                           const SDL_Rect* srcrect, const SDL_FRect* orig,
                           const SDL_FRect* shifted);

static bool should_shift_blit(const BlitClassification& c) {
    return c.cls == BlitClass::Map;
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

static void log_blit_telemetry(const BlitClassification& c, int x, int y, int w, int h, bool shifted) {
    if (g_test_dump_frames > 0 && g_test_dump_delay == 0) {
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "Blit: dst=(%d,%d,%d,%d) pass=%d vpass=%d vmap=%d shifted=%d in_ui=%d tile=%d align=%d suspect=%d\n",
                 x, y, w, h,
                 g_in_map_pass.load(std::memory_order_relaxed) ? 1 : 0,
                 g_viewport_pass_index.load(std::memory_order_relaxed),
                 g_cur_pass_is_map.load(std::memory_order_relaxed) ? 1 : 0,
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

static void log_blit_telemetry_f(const BlitClassification& c, float x, float y, float w, float h, bool shifted) {
    if (g_test_dump_frames > 0 && g_test_dump_delay == 0) {
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "BlitF: dst=(%.2f,%.2f,%.2f,%.2f) pass=%d shifted=%d in_ui=%d tile=%d\n",
                 x, y, w, h,
                 g_in_map_pass.load(std::memory_order_relaxed) ? 1 : 0,
                 shifted ? 1 : 0,
                 c.in_ui, c.tile_sized);
        g_telemetry_log += buf;
    }
}

static std::string active_log_path() {
    if (g_test_dump_frames > 0 || g_test_dump_delay > 0) {
        return smoothpan_log_path("smoothpan_telemetry.txt");
    }
    return smoothpan_log_path("smoothpan_classify.txt");
}

static bool sdl_shift_mode_active() {
    return g_shift_mode == ShiftMode::Sdl || g_shift_mode == ShiftMode::SeqPreToolbar;
}

static void note_frame_blit(int x, int y, int w, int h) {
    if (is_enabled || trace_is_active()) {
        frame_seq_note_blit(x, y, w, h);
    }
}

static bool process_map_blit(int x, int y, int w, int h, int* out_x, int* out_y) {
    if (trace_is_active()) return false;
    if (!is_enabled || !sdl_shift_mode_active()) return false;
    if (g_shift_mode == ShiftMode::SeqPreToolbar && !frame_seq_shift_allowed()) return false;
    // Only shift blits issued during the main map viewport update.  This
    // precisely excludes UI overlays, info-panel icons, toolbar passes, etc.
    if (!g_in_main_viewport_update.load(std::memory_order_relaxed)) return false;

    BlitClassification c = classify_blit(x, y, w, h);
    bool shifted = should_shift_blit(c);
    record_blit_counters(c, shifted);
    log_blit_telemetry(c, x, y, w, h, shifted);

    if (!shifted) return false;

    *out_x = x - static_cast<int>(std::lround(g_camera.render_shift_x()));
    *out_y = y - static_cast<int>(std::lround(g_camera.render_shift_y()));
    return true;
}

static bool process_map_blit_f(float x, float y, float w, float h) {
    if (trace_is_active()) return false;
    if (!is_enabled || !sdl_shift_mode_active()) return false;
    if (g_shift_mode == ShiftMode::SeqPreToolbar && !frame_seq_shift_allowed()) return false;
    if (!g_in_main_viewport_update.load(std::memory_order_relaxed)) return false;

    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    int iw = static_cast<int>(std::ceil(w));
    int ih = static_cast<int>(std::ceil(h));
    BlitClassification c = classify_blit(ix, iy, iw, ih);
    bool shifted = should_shift_blit(c);
    record_blit_counters(c, shifted);
    log_blit_telemetry_f(c, x, y, w, h, shifted);
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
             "EdgeProfile: vp.left=%d vp.right=%d origin_x=%d shift_x=%.2f\n",
             vp.left, vp.right, vp.origin_x, g_camera.render_shift_x());
    g_telemetry_log += buf;
    for (int i = 0; i < 3; i++) {
        int ls, re;
        edge_profile_scan_row(renderer, w, rows[i], &ls, &re);
        snprintf(buf, sizeof(buf),
                 "  row y=%d leftContentStart=%d rightContentEnd=%d rightMargin=%d\n",
                 rows[i], ls, re, (re >= 0 ? (w - 1 - re) : -1));
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
                edge_profile_log(renderer);
            }
            if (g_test_dump_frames > 0 && True_SDL_RenderReadPixels && True_SDL_CreateRGBSurfaceWithFormat &&
                True_SDL_SaveBMP_RW && True_SDL_RWFromFile && True_SDL_FreeSurface) {
                int w, h;
                if (GetRendererOutputSize_func && GetRendererOutputSize_func(renderer, &w, &h) == 0) {
                    SDL_Surface* surface = True_SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                    if (surface) {
                        if (True_SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0) {
                            char filename[256];
                            snprintf(filename, sizeof(filename), "smoothpan_frame_%d.bmp", g_test_dump_frames);
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

                fprintf(f, "SMOOTHPAN_%s frame=%d fx=%.3f fy=%.3f shift=(%.2f,%.2f) px=(%d,%d) overscan=(%d,%d) os_act=%d reason=%s mode=%s\n",
                        SMOOTHPAN_BUILD_VERSION,
                        g_test_dump_frames > 0 ? g_test_dump_frames : g_classify_log_frames,
                        snap.frac_x, snap.frac_y,
                        snap.shift_x, snap.shift_y,
                        static_cast<int>(std::lround(snap.shift_x)), static_cast<int>(std::lround(snap.shift_y)),
                        log_overscan_x, log_overscan_y,
                        snap.overscan_active ? 1 : 0,
                        snap.overscan_reason,
                        shift_mode_name(g_shift_mode));
                if (g_test_dump_frames > 0 && df::global::gps && df::global::gps->main_map_port) {
                    auto* mp = df::global::gps->main_map_port;
                    fprintf(f, "  map_port pixel_perc=(%d,%d) dim=(%d,%d)\n",
                            mp->pixel_perc_x, mp->pixel_perc_y, mp->dim_x, mp->dim_y);
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
                    fprintf(f, "  mouse raw_sdl=(%d,%d) df_tile=(%d,%d) df_precise=(%d,%d) "
                               "shift=(%.2f,%.2f) origin=(%d,%d) screen=(%d,%d) cell=%d\n",
                            rawx, rawy,
                            df::global::gps->mouse_x, df::global::gps->mouse_y,
                            df::global::gps->precise_mouse_x, df::global::gps->precise_mouse_y,
                            g_camera.render_shift_x(), g_camera.render_shift_y(),
                            origin_x, origin_y, mvp.left, mvp.top, mvp.cell_size);
                    
                                    }
                fprintf(f, "  blits=%d shifted=%d pass_shift=%d ui_leak=%d map=%d/%d (%.1f%%) tile=%d/%d (%.1f%%) sprite=%d/%d (%.1f%%) log=%s\n",
                        g_frame_blit_total, g_frame_blit_shifted,
                        g_frame_pass_shifted, g_frame_ui_leak_shifted,
                        g_frame_map_shifted, g_frame_map_total, map_rate,
                        g_frame_tile_shifted, g_frame_tile_total, tile_rate,
                        g_frame_sprite_shifted, g_frame_sprite_total, sprite_rate,
                        log_path.c_str());
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

    // All SDL blits for this frame have fired.  Now that render_shift_x()
    // has been used, it is safe to clear the overscan state and take the
    // per-frame snapshot.
    g_camera.end_render_overscan();

    probe_on_present();
    trace_on_present();
    frame_seq_on_present();
    renderer_hook_on_present();
    g_in_map_pass.store(false, std::memory_order_relaxed);

    True_SDL_RenderPresent(renderer);
}

int Hook_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) {
    if (dstrect) {
        note_frame_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        if (trace_is_active()) {
            trace_on_sdl_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        }
    }
    if (dstrect) {
        int sx, sy;
        if (process_map_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h, &sx, &sy)) {
            SDL_Rect shifted = *dstrect;
            shifted.x = sx;
            shifted.y = sy;
            int result = True_SDL_RenderCopy(renderer, texture, srcrect, &shifted);
            if (True_SDL_RenderCopyF) {
                SDL_FRect orig_f  = { (float)dstrect->x, (float)dstrect->y,
                                      (float)dstrect->w, (float)dstrect->h };
                SDL_FRect shift_f = { (float)shifted.x,  (float)shifted.y,
                                      (float)shifted.w,   (float)shifted.h };
                fill_edge_gap(renderer, texture, srcrect, &orig_f, &shift_f);
            }
            return result;
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
    if (dstrect) {
        int sx, sy;
        if (process_map_blit(dstrect->x, dstrect->y, dstrect->w, dstrect->h, &sx, &sy)) {
            SDL_Rect shifted = *dstrect;
            shifted.x = sx;
            shifted.y = sy;
            int result = True_SDL_RenderCopyEx(renderer, texture, srcrect, &shifted, angle, center, flip);
            if (True_SDL_RenderCopyF) {
                SDL_FRect orig_f  = { (float)dstrect->x, (float)dstrect->y,
                                      (float)dstrect->w, (float)dstrect->h };
                SDL_FRect shift_f = { (float)shifted.x,  (float)shifted.y,
                                      (float)shifted.w,   (float)shifted.h };
                fill_edge_gap(renderer, texture, srcrect, &orig_f, &shift_f);
            }
            return result;
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect, angle, center, flip);
}

// After shifting the last column/row tile, fill the right/bottom/corner gaps.
//
// WHY origin_x matters: DF tiles are always blitted at origin_x + n*cell,
// NOT at screen_x (vp.left) + n*cell.  The renderer's origin_x is fixed
// (typically 6px), while screen_x drifts based on map position / UI layout.
// The gap = (screen_x - origin_x) + render_shift pixels.  Using vp.right
// for detection would miss by exactly (screen_x - origin_x) — this was the
// root bug causing fills never to fire.
//
// The fill uses the FULL last tile source, SDL-stretched to fit the gap width.
// For normal panning the gap ≈ cell, so stretch is <5%.  Near map edges where
// screen_x >> origin_x the stretch can reach ~20% — still far better than a
// black bar of identical width.
static void fill_edge_gap(SDL_Renderer* renderer, SDL_Texture* texture,
                           const SDL_Rect* srcrect, const SDL_FRect* orig,
                           const SDL_FRect* shifted) {
    if (!True_SDL_RenderCopyF) return;
    if (orig->w <= 0.0f || orig->h <= 0.0f) return;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;

    float sx = g_camera.render_shift_x();
    float sy = g_camera.render_shift_y();
    if (sx < 0.5f && sy < 0.5f) return;

    // Read the renderer's fixed tile-grid origin.
    int origin_x = 0, origin_y = 0;
    if (df::global::enabler) {
        auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
        if (r2d) { origin_x = r2d->origin_x; origin_y = r2d->origin_y; }
    }

    // The actual tile-grid right/bottom boundary (where the last tile ends).
    // vp.right - vp.left = dim_x * cell = vp.bottom - vp.top = dim_y * cell.
    float tile_right  = static_cast<float>(origin_x + (vp.right  - vp.left));
    float tile_bottom = static_cast<float>(origin_y + (vp.bottom - vp.top));

    // Detect last column: tile's original right edge matches the tile grid end.
    bool is_last_col = sx >= 0.5f && std::abs(orig->x + orig->w - tile_right)  < 2.0f;
    bool is_last_row = sy >= 0.5f && std::abs(orig->y + orig->h - tile_bottom) < 2.0f;

    if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "EdgeScan: orig=(%.0f,%.0f +%.0fx%.0f) vp=(%d..%d,%d..%d) "
            "origin=(%d,%d) tr=%.0f tb=%.0f sx=%.1f sy=%.1f col=%d row=%d\n",
            orig->x, orig->y, orig->w, orig->h,
            vp.left, vp.right, vp.top, vp.bottom,
            origin_x, origin_y, tile_right, tile_bottom,
            sx, sy, is_last_col ? 1 : 0, is_last_row ? 1 : 0);
        g_telemetry_log += buf;
    }

    // Use the SAME integer rounding as process_map_blit (lround) so that the
    // fill starts exactly where the shifted tile ends — no sub-pixel seam.
    float sx_r = std::round(sx);
    float sy_r = std::round(sy);

    // NOTE: the left edge needs NO gap fill.  When panning, map content flows
    // leftward — column 0 slides out through the (now snapped) left clip while
    // columns 1+ cover the interior.  The left shimmer is fixed by the clip
    // snap in Hook_SDL_RenderSetClipRect, not by a fill here.

    if (!is_last_col && !is_last_row) return;

    // Right gap: [tile_right - sx_r, vp.right] × [shifted_y, shifted_y + cell]
    if (is_last_col) {
        float gap_x = tile_right - sx_r;
        float gap_w = static_cast<float>(vp.right) - gap_x;
        if (gap_w > 0.5f) {
            SDL_FRect dst = { gap_x, shifted->y, gap_w, shifted->h };
            True_SDL_RenderCopyF(renderer, texture, srcrect, &dst);
            if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  RightFill: (%.0f,%.0f,%.0f,%.0f)\n",
                         dst.x, dst.y, dst.w, dst.h);
                g_telemetry_log += buf;
            }
        }
    }

    // Bottom gap: [shifted_x, shifted_x + cell] × [tile_bottom - sy_r, vp.bottom]
    if (is_last_row) {
        float gap_y = tile_bottom - sy_r;
        float gap_h = static_cast<float>(vp.bottom) - gap_y;
        if (gap_h > 0.5f) {
            SDL_FRect dst = { shifted->x, gap_y, shifted->w, gap_h };
            True_SDL_RenderCopyF(renderer, texture, srcrect, &dst);
            if ((g_test_dump_frames > 0 || g_classify_log_frames > 0) && g_test_dump_delay == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  BottomFill: (%.0f,%.0f,%.0f,%.0f)\n",
                         dst.x, dst.y, dst.w, dst.h);
                g_telemetry_log += buf;
            }
        }
    }

    // Corner gap: [tile_right - sx_r, vp.right] × [tile_bottom - sy_r, vp.bottom]
    if (is_last_col && is_last_row) {
        float gap_x = tile_right  - sx_r;
        float gap_y = tile_bottom - sy_r;
        float gap_w = static_cast<float>(vp.right)  - gap_x;
        float gap_h = static_cast<float>(vp.bottom) - gap_y;
        if (gap_w > 0.5f && gap_h > 0.5f) {
            SDL_FRect dst = { gap_x, gap_y, gap_w, gap_h };
            True_SDL_RenderCopyF(renderer, texture, srcrect, &dst);
        }
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
        if (process_map_blit_f(dstrect->x, dstrect->y, dstrect->w, dstrect->h)) {
            SDL_FRect shifted = *dstrect;
            shifted.x -= g_camera.render_shift_x();
            shifted.y -= g_camera.render_shift_y();
            int result = True_SDL_RenderCopyF(renderer, texture, srcrect, &shifted);
            fill_edge_gap(renderer, texture, srcrect, dstrect, &shifted);
            return result;
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
        if (process_map_blit_f(dstrect->x, dstrect->y, dstrect->w, dstrect->h)) {
            SDL_FRect shifted = *dstrect;
            shifted.x -= g_camera.render_shift_x();
            shifted.y -= g_camera.render_shift_y();
            return True_SDL_RenderCopyExF(renderer, texture, srcrect, &shifted, angle, center, flip);
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
    void* poll_event_addr = (void*)GetProcAddress(sdl_module, "SDL_PollEvent");
    void* peep_events_addr = (void*)GetProcAddress(sdl_module, "SDL_PeepEvents");
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
    if (poll_event_addr) MH_CreateHook(poll_event_addr, &Hook_SDL_PollEvent, reinterpret_cast<LPVOID*>(&True_SDL_PollEvent));
    if (peep_events_addr) MH_CreateHook(peep_events_addr, &Hook_SDL_PeepEvents, reinterpret_cast<LPVOID*>(&True_SDL_PeepEvents));
    if (get_mouse_state_addr) MH_CreateHook(get_mouse_state_addr, &Hook_SDL_GetMouseState, reinterpret_cast<LPVOID*>(&GetMouseState_func));
    if (get_global_mouse_state_addr) MH_CreateHook(get_global_mouse_state_addr, &Hook_SDL_GetGlobalMouseState, reinterpret_cast<LPVOID*>(&GetGlobalMouseState_func));
    if (render_set_clip_addr) MH_CreateHook(render_set_clip_addr, &Hook_SDL_RenderSetClipRect, reinterpret_cast<LPVOID*>(&True_SDL_RenderSetClipRect));
    if (set_render_target_addr) MH_CreateHook(set_render_target_addr, &Hook_SDL_SetRenderTarget, reinterpret_cast<LPVOID*>(&True_SDL_SetRenderTarget));
    if (render_set_viewport_addr) MH_CreateHook(render_set_viewport_addr, &Hook_SDL_RenderSetViewport, reinterpret_cast<LPVOID*>(&True_SDL_RenderSetViewport));

    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

void CleanupSDLHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

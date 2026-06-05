#define NOMINMAX
#include "camera.h"
#include <cmath>
#include <windows.h>
#undef min
#undef max
#include <algorithm>
#include "df/global_objects.h"
#include "df/world.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/gamest.h"
#include "shift_mode.h"
#include "ffd_policy.h"
#include "perf.h"
#include "viewport.h"
#include "sdl_hook.h"
#include "zoom_probe.h"
#include <cstdio>
#include <cstring>

using namespace DFHack;

int g_sp_mmb_held = 0;
int g_sp_middle_drag = 0;
int g_sp_mmb_scroll = 0;
int g_sp_mmb_sticky = 0;
int g_sp_mmb_gate = 0;
int g_sp_mmb_dx = 0;
int g_sp_mmb_dy = 0;

static bool g_frame_minimap_dirty = false;
static bool g_frame_tile_step = false;
static bool g_frame_minimap_mustmake = false;
static bool g_frame_pan_end_sync = false;
static bool g_frame_ffd_bump_tile = false;
static bool g_frame_ffd_bump_pan = false;

enum class MinimapPanMode { Lazy, Throttled, Full, UpdateOnly };
static MinimapPanMode g_minimap_mode = MinimapPanMode::Lazy;
static std::chrono::steady_clock::time_point g_last_minimap_mustmake;
static bool g_was_pan_active = false;

static constexpr int kMinimapLazyIntervalMs = 2000;
static constexpr int kMinimapOutlineIntervalMs = 80;
static int g_minimap_mustmake_interval_ms = kMinimapLazyIntervalMs;

static std::chrono::milliseconds minimap_mustmake_interval() {
    return std::chrono::milliseconds(g_minimap_mustmake_interval_ms);
}

static void minimap_apply_mode_interval(MinimapPanMode mode) {
    switch (mode) {
    case MinimapPanMode::Lazy:
        g_minimap_mustmake_interval_ms = kMinimapLazyIntervalMs;
        break;
    case MinimapPanMode::Throttled:
        g_minimap_mustmake_interval_ms = kMinimapOutlineIntervalMs;
        break;
    default:
        break;
    }
}

static void minimap_mark_camera_moved(bool tile_step, bool force_mustmake) {
    if (!df::global::game) return;

    auto& mm = df::global::game->minimap;
    mm.update = 1;

    bool do_mustmake = false;
    switch (g_minimap_mode) {
    case MinimapPanMode::Full:
        do_mustmake = true;
        break;
    case MinimapPanMode::UpdateOnly:
        break;
    case MinimapPanMode::Lazy:
    case MinimapPanMode::Throttled:
        if (force_mustmake) {
            do_mustmake = true;
        } else if (tile_step) {
            auto now = std::chrono::steady_clock::now();
            if (g_last_minimap_mustmake == std::chrono::steady_clock::time_point{} ||
                now - g_last_minimap_mustmake >= minimap_mustmake_interval()) {
                do_mustmake = true;
            }
        }
        break;
    }

    if (do_mustmake) {
        mm.mustmake = 1;
        g_last_minimap_mustmake = std::chrono::steady_clock::now();
        g_frame_minimap_mustmake = true;
    }

    g_frame_minimap_dirty = true;
    if (tile_step) g_frame_tile_step = true;
    if (force_mustmake) g_frame_pan_end_sync = true;
}

static void notify_minimap_camera_moved() {
    minimap_mark_camera_moved(true, false);
}

static void sync_minimap_on_pan_end() {
    minimap_mark_camera_moved(false, true);
}

void smoothpan_notify_zoom_commit() {
}

void smoothpan_set_minimap_full_rebuild(bool full) {
    g_minimap_mode = full ? MinimapPanMode::Full : MinimapPanMode::Lazy;
    minimap_apply_mode_interval(g_minimap_mode);
}

bool smoothpan_minimap_full_rebuild() {
    return g_minimap_mode == MinimapPanMode::Full;
}

const char* smoothpan_minimap_mode_name() {
    switch (g_minimap_mode) {
    case MinimapPanMode::Lazy: return "lazy";
    case MinimapPanMode::Throttled: return "outline";
    case MinimapPanMode::Full: return "full";
    case MinimapPanMode::UpdateOnly: return "fast";
    }
    return "unknown";
}

int smoothpan_minimap_mustmake_interval_ms() {
    return g_minimap_mustmake_interval_ms;
}

void smoothpan_set_minimap_mustmake_interval_ms(int ms) {
    if (ms < 80) ms = 80;
    if (ms > 60000) ms = 60000;
    g_minimap_mustmake_interval_ms = ms;
}

void smoothpan_set_minimap_pan_mode(const char* mode) {
    if (!mode) return;
    if (strcmp(mode, "lazy") == 0) {
        g_minimap_mode = MinimapPanMode::Lazy;
    } else if (strcmp(mode, "full") == 0) {
        g_minimap_mode = MinimapPanMode::Full;
    } else if (strcmp(mode, "fast") == 0 || strcmp(mode, "update") == 0) {
        g_minimap_mode = MinimapPanMode::UpdateOnly;
    } else if (strcmp(mode, "outline") == 0 || strcmp(mode, "throttled") == 0) {
        g_minimap_mode = MinimapPanMode::Throttled;
    } else {
        g_minimap_mode = MinimapPanMode::Lazy;
    }
    minimap_apply_mode_interval(g_minimap_mode);
}

SmoothCamera g_camera;

// Zoom-transition window (Stage 1).  See camera.h.
std::atomic<int> g_zoom_transition_frames{0};
std::atomic<int> g_zoom_prev_z{0};
static constexpr int kZoomTransitionFrames = 8;

void SmoothCamera::reset() {
    first_frame = true;
    panning_up = false;
    panning_down = false;
    panning_left = false;
    panning_right = false;
    middle_drag_active = false;
    smoothpan_middle_mouse_reset();
    vel_x = 0;
    vel_y = 0;
    overscan_tiles_x = 0;
    overscan_tiles_y = 0;
    overscan_active = false;
    last_zoom = 0;
    last_vel_sign_x = 0;
    last_vel_sign_y = 0;
    last_snapshot = {};
}

static bool is_physical_up_held() { return (GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD8) & 0x8000); }
static bool is_physical_down_held() { return (GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD2) & 0x8000); }
static bool is_physical_left_held() { return (GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD4) & 0x8000); }
static bool is_physical_right_held() { return (GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD6) & 0x8000); }

bool smoothpan_middle_mouse_button_held() {
    return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

bool smoothpan_middle_mouse_map_gate(int precise_x, int precise_y) {
    if (!df::global::gps || precise_x < 0 || precise_y < 0) return false;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    int raw_x = precise_x + vp.origin_x;
    int raw_y = precise_y + vp.origin_y;
    if (IsMouseInUI_reason(raw_x, raw_y, nullptr) != 0) return false;
    if (mouse_over_ui_widget(raw_x, raw_y)) return false;
    return true;
}

void SmoothCamera::sync_true_from_window() {
    if (!df::global::window_x || !df::global::window_y) return;
    true_x = *df::global::window_x + static_cast<double>(frac_x.load(std::memory_order_relaxed));
    true_y = *df::global::window_y + static_cast<double>(frac_y.load(std::memory_order_relaxed));
}

bool SmoothCamera::commit_true_position() {
    if (!df::global::window_x || !df::global::window_y || !df::global::gps || !df::global::world) return false;

    int half_width_tiles = df::global::gps->main_viewport->dim_x / 8;
    int half_height_tiles = df::global::gps->main_viewport->dim_y / 8;
    int min_x = -half_width_tiles;
    int min_y = -half_height_tiles;
    int max_x = df::global::world->map.x_count - half_width_tiles;
    int max_y = df::global::world->map.y_count - half_height_tiles;

    if (true_x < min_x) { true_x = min_x; vel_x = 0; }
    if (true_x > max_x) { true_x = max_x; vel_x = 0; }
    if (true_y < min_y) { true_y = min_y; vel_y = 0; }
    if (true_y > max_y) { true_y = max_y; vel_y = 0; }

    int win_x = *df::global::window_x;
    int win_y = *df::global::window_y;
    int new_win_x = static_cast<int>(std::floor(true_x));
    int new_win_y = static_cast<int>(std::floor(true_y));
    const bool tile_step = new_win_x != win_x || new_win_y != win_y;

    if (tile_step) {
        *df::global::window_x = new_win_x;
        *df::global::window_y = new_win_y;
        notify_minimap_camera_moved();
        g_frame_ffd_bump_tile = true;
        if (df::global::gps->force_full_display_count < 1) {
            df::global::gps->force_full_display_count = 1;
        }
    }

    frac_x.store(static_cast<float>(true_x - std::floor(true_x)), std::memory_order_relaxed);
    frac_y.store(static_cast<float>(true_y - std::floor(true_y)), std::memory_order_relaxed);
    return tile_step;
}

static bool raw_viewport_precise(int* out_px, int* out_py) {
    int raw_x = -1, raw_y = -1;
    if (!smoothpan_raw_sdl_mouse(&raw_x, &raw_y)) return false;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    if (out_px) *out_px = raw_x - vp.origin_x;
    if (out_py) *out_py = raw_y - vp.origin_y;
    return true;
}

static bool g_mmb_was_active = false;
static int g_mmb_anchor_px = -1;
static int g_mmb_anchor_py = -1;
static double g_mmb_anchor_true_x = 0.0;
static double g_mmb_anchor_true_y = 0.0;

static void middle_mouse_end_session() {
    if (g_mmb_was_active)
        g_camera.commit_true_position();
    g_mmb_was_active = false;
    g_mmb_anchor_px = -1;
    g_mmb_anchor_py = -1;
    g_camera.middle_drag_active = false;
    g_sp_middle_drag = 0;
}

void smoothpan_middle_mouse_reset() {
    middle_mouse_end_session();
}

void smoothpan_middle_mouse_update() {
    g_sp_mmb_dx = 0;
    g_sp_mmb_dy = 0;
    g_sp_mmb_sticky = 0;

    if (!df::global::gps) {
        middle_mouse_end_session();
        g_sp_mmb_held = 0;
        g_sp_mmb_scroll = 0;
        return;
    }

    const bool scroll_flag = df::global::game && df::global::game->main_interface.mouse_scrolling_map;
    const bool mmb = smoothpan_middle_mouse_button_held();
    g_sp_mmb_held = mmb ? 1 : 0;
    g_sp_mmb_scroll = scroll_flag ? 1 : 0;

    if (!mmb) {
        middle_mouse_end_session();
        return;
    }

    int px = -1, py = -1;
    if (!raw_viewport_precise(&px, &py)) {
        px = df::global::gps->precise_mouse_x;
        py = df::global::gps->precise_mouse_y;
    }

    const bool on_map = smoothpan_middle_mouse_map_gate(px, py);
    g_sp_mmb_gate = on_map ? 1 : 0;

    const bool may_start = on_map || scroll_flag;

    if (!g_mmb_was_active && !may_start) {
        g_camera.middle_drag_active = false;
        g_sp_middle_drag = 0;
        return;
    }

    if (g_mmb_was_active && !on_map)
        g_sp_mmb_sticky = 1;

    g_camera.middle_drag_active = true;
    g_sp_middle_drag = 1;
    g_camera.vel_x = 0;
    g_camera.vel_y = 0;
    g_camera.panning_up = false;
    g_camera.panning_down = false;
    g_camera.panning_left = false;
    g_camera.panning_right = false;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) return;
    const int cell = vp.cell_size;

    if (!g_mmb_was_active) {
        g_camera.sync_true_from_window();
        g_mmb_anchor_true_x = g_camera.true_x;
        g_mmb_anchor_true_y = g_camera.true_y;
        g_mmb_anchor_px = px;
        g_mmb_anchor_py = py;
        g_mmb_was_active = true;
        g_camera.commit_true_position();
        return;
    }

    g_sp_mmb_dx = px - g_mmb_anchor_px;
    g_sp_mmb_dy = py - g_mmb_anchor_py;

    g_camera.true_x = g_mmb_anchor_true_x - static_cast<double>(g_sp_mmb_dx) / cell;
    g_camera.true_y = g_mmb_anchor_true_y - static_cast<double>(g_sp_mmb_dy) / cell;
    g_camera.commit_true_position();
}

void SmoothCamera::freeze_render_frac() {
    render_frac_x = frac_x.load(std::memory_order_relaxed);
    render_frac_y = frac_y.load(std::memory_order_relaxed);
    frame_frac_x.store(render_frac_x, std::memory_order_relaxed);
    frame_frac_y.store(render_frac_y, std::memory_order_relaxed);
    render_zoom_scale = 1.0f;
    last_snapshot.render_zoom_scale = 1.0f;
}

// Returns the pixel width of one graphical map tile.
// gps->viewport_zoom_factor encodes the tile size as 4× the pixel width
// (e.g. 192 → 48 px/tile), matching the renderer formula:
//   dst.w = viewport_zoom_factor * 32 / 128 = z / 4
static int tile_cell_px() {
    if (!df::global::gps) return 0;
    int z = df::global::gps->viewport_zoom_factor;
    return (z > 3) ? z / 4 : 0;
}

float SmoothCamera::render_shift_x() const {
    int cell = tile_cell_px();
    if (cell <= 0) return 0.0f;
    return render_frac_x * static_cast<float>(cell) * render_zoom_scale
         - static_cast<float>(overscan_tiles_x * cell);
}

float SmoothCamera::render_shift_y() const {
    int cell = tile_cell_px();
    if (cell <= 0) return 0.0f;
    return render_frac_y * static_cast<float>(cell) * render_zoom_scale
         - static_cast<float>(overscan_tiles_y * cell);
}

int SmoothCamera::pixel_shift_x() const {
    return static_cast<int>(std::lround(render_shift_x()));
}

int SmoothCamera::pixel_shift_y() const {
    return static_cast<int>(std::lround(render_shift_y()));
}

int SmoothCamera::gap_shift_x() const {
    int cell = tile_cell_px();
    if (cell <= 0) return 0;
    return static_cast<int>(std::lround(render_frac_x * static_cast<float>(cell)));
}

int SmoothCamera::gap_shift_y() const {
    int cell = tile_cell_px();
    if (cell <= 0) return 0;
    return static_cast<int>(std::lround(render_frac_y * static_cast<float>(cell)));
}

void SmoothCamera::begin_render_overscan() {
    freeze_render_frac();
    overscan_tiles_x = 0;
    overscan_tiles_y = 0;
    overscan_active = false;
    last_snapshot.overscan_reason[0] = '\0';

    if (g_shift_mode == ShiftMode::None) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "mode_none");
        return;
    }
    if (g_shift_mode == ShiftMode::MapPort) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "mode_mapport");
        return;
    }
    if (g_shift_mode == ShiftMode::SeqPreToolbar) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "mode_seqrange");
        return;
    }
    if (g_shift_mode == ShiftMode::Viewport) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "mode_viewport");
        return;
    }
    // ShiftMode::Sdl: SDL blit shift + edge fill only (no window pull).
    if (g_shift_mode == ShiftMode::Sdl) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "mode_sdl");
        return;
    }

    if (!df::global::window_x || !df::global::window_y) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "no_window");
        return;
    }

    auto set_reason = [&](const char* text) {
        snprintf(last_snapshot.overscan_reason, sizeof(last_snapshot.overscan_reason), "%s", text);
    };

    if (render_frac_x > 0.001f) {
        if (vel_x > 0.01 || panning_right) {
            overscan_tiles_x = -1;
            set_reason("vel_x+");
        } else if (vel_x < -0.01 || panning_left) {
            overscan_tiles_x = 1;
            set_reason("vel_x-");
        } else if (last_vel_sign_x > 0) {
            overscan_tiles_x = -1;
            set_reason("last_x+");
        } else if (last_vel_sign_x < 0) {
            overscan_tiles_x = 1;
            set_reason("last_x-");
        } else {
            set_reason("x_no_dir");
        }
    }
    if (render_frac_y > 0.001f) {
        if (vel_y > 0.01 || panning_down) {
            overscan_tiles_y = 1;
            if (last_snapshot.overscan_reason[0] == '\0') set_reason("vel_y+");
        } else if (vel_y < -0.01 || panning_up) {
            overscan_tiles_y = -1;
            if (last_snapshot.overscan_reason[0] == '\0') set_reason("vel_y-");
        } else if (last_vel_sign_y > 0) {
            overscan_tiles_y = 1;
            if (last_snapshot.overscan_reason[0] == '\0') set_reason("last_y+");
        } else if (last_vel_sign_y < 0) {
            overscan_tiles_y = -1;
            if (last_snapshot.overscan_reason[0] == '\0') set_reason("last_y-");
        } else if (last_snapshot.overscan_reason[0] == '\0') {
            set_reason("y_no_dir");
        }
    }

    if (overscan_tiles_x == 0 && overscan_tiles_y == 0) {
        if (last_snapshot.overscan_reason[0] == '\0') set_reason("frac_zero");
        return;
    }

    saved_window_x = *df::global::window_x;
    saved_window_y = *df::global::window_y;
    *df::global::window_x = saved_window_x + overscan_tiles_x;
    *df::global::window_y = saved_window_y + overscan_tiles_y;
    overscan_active = true;
}

void SmoothCamera::snapshot_render_state() {
    int keep_applied_x = last_snapshot.applied_ppc_x;
    int keep_applied_y = last_snapshot.applied_ppc_y;
    last_snapshot.true_x = true_x;
    last_snapshot.true_y = true_y;
    last_snapshot.frac_x = render_frac_x;
    last_snapshot.frac_y = render_frac_y;
    last_snapshot.vel_x = static_cast<float>(vel_x);
    last_snapshot.vel_y = static_cast<float>(vel_y);
    last_snapshot.panning_up = panning_up;
    last_snapshot.panning_down = panning_down;
    last_snapshot.panning_left = panning_left;
    last_snapshot.panning_right = panning_right;
    last_snapshot.overscan_x = overscan_tiles_x;
    last_snapshot.overscan_y = overscan_tiles_y;
    last_snapshot.overscan_active = overscan_active;
    last_snapshot.shift_x = render_shift_x();
    last_snapshot.shift_y = render_shift_y();
    last_snapshot.gap_x = gap_shift_x();
    last_snapshot.gap_y = gap_shift_y();
    if (overscan_active) {
        last_snapshot.saved_window_x = saved_window_x;
        last_snapshot.saved_window_y = saved_window_y;
    } else if (df::global::window_x && df::global::window_y) {
        last_snapshot.saved_window_x = *df::global::window_x;
        last_snapshot.saved_window_y = *df::global::window_y;
    }
    if (df::global::window_x && df::global::window_y) {
        last_snapshot.window_x = *df::global::window_x;
        last_snapshot.window_y = *df::global::window_y;
    }
    last_snapshot.applied_ppc_x = keep_applied_x;
    last_snapshot.applied_ppc_y = keep_applied_y;
}

void SmoothCamera::restore_window_overscan() {
    // Restore the real window position so game logic after the viewscreen
    // sees the correct tile coordinate.  Intentionally does NOT touch
    // overscan_tiles_x/y or overscan_active — those must stay set so that
    // the SDL blit hooks (clip rect expansion, render_shift_x compensation)
    // work correctly for the blits that fire between here and RenderPresent.
    if (overscan_active && df::global::window_x && df::global::window_y) {
        *df::global::window_x = saved_window_x;
        *df::global::window_y = saved_window_y;
    }
}

void SmoothCamera::end_render_overscan() {
    restore_window_overscan();
    snapshot_render_state();
    overscan_tiles_x = 0;
    overscan_tiles_y = 0;
    overscan_active = false;
}

void SmoothCamera::update() {
    auto now = std::chrono::steady_clock::now();
    if (first_frame) {
        last_frame = now;
        first_frame = false;
        if (df::global::window_x && df::global::window_y) {
            true_x = *df::global::window_x;
            true_y = *df::global::window_y;
        }
        if (df::global::gps) {
            last_zoom = df::global::gps->viewport_zoom_factor;
        }
        return;
    }
    
    float dt = std::chrono::duration<float>(now - last_frame).count();
    last_frame = now;
    if (dt > 0.1f) dt = 0.1f;

    if (!df::global::window_x || !df::global::window_y || !df::global::gps || !df::global::world) return;

    if (df::global::gps) {
        int zoom = df::global::gps->viewport_zoom_factor;
        if (zoom != last_zoom) {
            if (last_zoom != 0) {
                zoom_probe_note_gps_z_change(last_zoom, zoom);
                // Open the transition window so the SDL clip relaxes across
                // vanilla's multi-frame rebake (fixes black edge bands).
                g_zoom_prev_z.store(last_zoom, std::memory_order_relaxed);
                g_zoom_transition_frames.store(kZoomTransitionFrames,
                                               std::memory_order_relaxed);
            }
            last_zoom = zoom;
        }
    }
    
    if (!is_physical_up_held()) panning_up = false;
    if (!is_physical_down_held()) panning_down = false;
    if (!is_physical_left_held()) panning_left = false;
    if (!is_physical_right_held()) panning_right = false;

    if (smoothpan_middle_mouse_button_held())
        smoothpan_middle_mouse_update();
    else
        smoothpan_middle_mouse_reset();

    int win_x = *df::global::window_x;
    int win_y = *df::global::window_y;
    int expected_win_x = static_cast<int>(std::floor(true_x));
    int expected_win_y = static_cast<int>(std::floor(true_y));
    
    if (win_x != expected_win_x || win_y != expected_win_y) {
        if (middle_drag_active) {
            commit_true_position();
            win_x = *df::global::window_x;
            win_y = *df::global::window_y;
        } else {
            true_x = win_x + static_cast<double>(frac_x.load(std::memory_order_relaxed));
            true_y = win_y + static_cast<double>(frac_y.load(std::memory_order_relaxed));
            if (!panning_up && !panning_down && !panning_left && !panning_right) {
                vel_x = 0;
                vel_y = 0;
            }
        }
        expected_win_x = win_x;
        expected_win_y = win_y;
    }

    int half_width_tiles = df::global::gps->main_viewport->dim_x / 8;
    int half_height_tiles = df::global::gps->main_viewport->dim_y / 8;
    bool tile_step = false;

    if (!middle_drag_active) {
    float dx = 0, dy = 0;
    if (panning_up) dy -= 1.0f;
    if (panning_down) dy += 1.0f;
    if (panning_left) dx -= 1.0f;
    if (panning_right) dx += 1.0f;
    
    if (dx != 0 && dy != 0) {
        float len = std::sqrt(dx*dx + dy*dy);
        dx /= len;
        dy /= len;
    }
    
    if (dx != 0 || dy != 0) {
        vel_x = dx * max_speed;
        vel_y = dy * max_speed;
    } else {
        vel_x -= vel_x * friction * dt;
        vel_y -= vel_y * friction * dt;
        if (std::abs(vel_x) < 0.5f) vel_x = 0;
        if (std::abs(vel_y) < 0.5f) vel_y = 0;
    }

    if (std::abs(vel_x) > 0.5) last_vel_sign_x = (vel_x > 0) ? 1 : -1;
    if (std::abs(vel_y) > 0.5) last_vel_sign_y = (vel_y > 0) ? 1 : -1;
    
    true_x += vel_x * dt;
    true_y += vel_y * dt;
    
    int min_x = -half_width_tiles;
    int min_y = -half_height_tiles;
    int max_x = df::global::world->map.x_count - half_width_tiles;
    int max_y = df::global::world->map.y_count - half_height_tiles;
    
    if (true_x < min_x) { true_x = min_x; vel_x = 0; }
    if (true_x > max_x) { true_x = max_x; vel_x = 0; }
    if (true_y < min_y) { true_y = min_y; vel_y = 0; }
    if (true_y > max_y) { true_y = max_y; vel_y = 0; }
    
    int new_win_x = static_cast<int>(std::floor(true_x));
    int new_win_y = static_cast<int>(std::floor(true_y));
    tile_step = new_win_x != win_x || new_win_y != win_y;

    if (tile_step) {
        *df::global::window_x = new_win_x;
        *df::global::window_y = new_win_y;
        // Vanilla scroll sets minimap dirty flags; we write window_x/y directly.
        notify_minimap_camera_moved();
        g_frame_ffd_bump_tile = true;
        if (df::global::gps && df::global::gps->force_full_display_count < 1) {
            df::global::gps->force_full_display_count = 1;
        }
    }
    
    frac_x.store(static_cast<float>(true_x - std::floor(true_x)), std::memory_order_relaxed);
    frac_y.store(static_cast<float>(true_y - std::floor(true_y)), std::memory_order_relaxed);
    } else {
        tile_step = g_frame_tile_step;
    }

    // Pan-time ffd>=2: rebake lower-z viewport passes during sub-tile pan when
    // visible terrain can show lower z (cliffs, open air). Smart mode skips on
    // flat enclosed interior — see ffd_policy.cpp.
    if (df::global::gps) {
        float fx = frac_x.load(std::memory_order_relaxed);
        float fy = frac_y.load(std::memory_order_relaxed);

        const bool panning = std::abs(fx) > 0.001f || std::abs(fy) > 0.001f ||
                             std::abs(vel_x) > 0.5f || std::abs(vel_y) > 0.5f ||
                             panning_up || panning_down || panning_left || panning_right ||
                             middle_drag_active;

        if (panning && df::global::window_x && df::global::window_y && df::global::window_z) {
            ffd_policy_ensure_cache(*df::global::window_x, *df::global::window_y,
                                    *df::global::window_z,
                                    half_width_tiles, half_height_tiles,
                                    tile_step);
            if (ffd_policy_apply_pan(true))
                g_frame_ffd_bump_pan = true;
        }
        if (g_was_pan_active && !panning) {
            sync_minimap_on_pan_end();
        }
        g_was_pan_active = panning;

        if (perf_is_active()) {
            PerfCameraSample sample;
            sample.panning = panning;
            sample.pan_keys = panning_up || panning_down || panning_left || panning_right;
            sample.force_full_display = df::global::gps->force_full_display_count;
            sample.ffd_bump_tile = g_frame_ffd_bump_tile;
            sample.ffd_bump_pan = g_frame_ffd_bump_pan;
            sample.ffd_skip = panning && ffd_policy_last_pan_ffd_skipped();
            sample.ffd_reason = panning ? ffd_policy_last_reason() : "";
            sample.tile_step = g_frame_tile_step;
            sample.pan_end_sync = g_frame_pan_end_sync;
            sample.minimap_update = g_frame_minimap_dirty;
            sample.minimap_mustmake = g_frame_minimap_mustmake;
            if (df::global::window_x && df::global::window_y) {
                sample.window_x = *df::global::window_x;
                sample.window_y = *df::global::window_y;
            }
            sample.frac_x100 = static_cast<int>(fx * 100.0f);
            sample.frac_y100 = static_cast<int>(fy * 100.0f);
            sample.vel_x10 = static_cast<int>(vel_x * 10.0);
            sample.vel_y10 = static_cast<int>(vel_y * 10.0);
            sample.shift_x10 = static_cast<int>(g_camera.render_shift_x() * 10.0f);
            sample.shift_y10 = static_cast<int>(g_camera.render_shift_y() * 10.0f);
            sample.zoom = df::global::gps->viewport_zoom_factor;
            perf_note_camera_sample(sample);
        }

        g_frame_minimap_dirty = false;
        g_frame_minimap_mustmake = false;
        g_frame_tile_step = false;
        g_frame_pan_end_sync = false;
        g_frame_ffd_bump_tile = false;
        g_frame_ffd_bump_pan = false;
    }
}

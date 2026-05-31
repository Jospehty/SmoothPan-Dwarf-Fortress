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
#include "shift_mode.h"
#include <cstdio>

using namespace DFHack;

SmoothCamera g_camera;

void SmoothCamera::reset() {
    first_frame = true;
    panning_up = false;
    panning_down = false;
    panning_left = false;
    panning_right = false;
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

void SmoothCamera::freeze_render_frac() {
    render_frac_x = frac_x.load(std::memory_order_relaxed);
    render_frac_y = frac_y.load(std::memory_order_relaxed);
    frame_frac_x.store(render_frac_x, std::memory_order_relaxed);
    frame_frac_y.store(render_frac_y, std::memory_order_relaxed);
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
    return render_frac_x * static_cast<float>(cell)
         - static_cast<float>(overscan_tiles_x * cell);
}

float SmoothCamera::render_shift_y() const {
    int cell = tile_cell_px();
    if (cell <= 0) return 0.0f;
    return render_frac_y * static_cast<float>(cell)
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
    // ShiftMode::Sdl: single-sided window_x overscan removes the tile on the
    // opposite edge, producing a larger gap than no overscan at all.  Disable
    // it here; the SDL blit hook handles the sub-tile shift directly.
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
    *df::global::window_x = render_win_x + overscan_tiles_x;
    *df::global::window_y = render_win_y + overscan_tiles_y;
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
    // Takes the per-frame snapshot then clears the overscan state.
    // Must be called from SDL_RenderPresent, AFTER all SDL blits for the
    // frame have been issued, so render_shift_x() returns correct values
    // throughout the blit sequence.
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

    int zoom = df::global::gps->viewport_zoom_factor;
    if (zoom != last_zoom && last_zoom != 0) {
        true_x = *df::global::window_x;
        true_y = *df::global::window_y;
        vel_x = 0;
        vel_y = 0;
    }
    last_zoom = zoom;
    
    if (!is_physical_up_held()) panning_up = false;
    if (!is_physical_down_held()) panning_down = false;
    if (!is_physical_left_held()) panning_left = false;
    if (!is_physical_right_held()) panning_right = false;
    
    if (render_win_x == -1 && df::global::window_x) {
        render_win_x = *df::global::window_x;
        render_win_y = *df::global::window_y;
    }
    
    int expected_win_x = static_cast<int>(std::floor(true_x));
    int expected_win_y = static_cast<int>(std::floor(true_y));
    
    // If the game natively jumped the camera (e.g. recenter on event)
    if (*df::global::window_x != render_win_x || *df::global::window_y != render_win_y) {
        render_win_x = *df::global::window_x;
        render_win_y = *df::global::window_y;
        true_x = render_win_x + static_cast<double>(frac_x.load(std::memory_order_relaxed));
        true_y = render_win_y + static_cast<double>(frac_y.load(std::memory_order_relaxed));
        if (!panning_up && !panning_down && !panning_left && !panning_right) {
            vel_x = 0;
            vel_y = 0;
        }
        expected_win_x = render_win_x;
        expected_win_y = render_win_y;
    }

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
    
    // dim_x/dim_y are 4x4 text cells; window_x/y are tile coords
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
    
    int new_win_x = static_cast<int>(std::floor(true_x));
    int new_win_y = static_cast<int>(std::floor(true_y));
    
    if (new_win_x != render_win_x || new_win_y != render_win_y) {
        render_win_x = new_win_x;
        render_win_y = new_win_y;
    }
    
    frac_x.store(static_cast<float>(true_x - render_win_x), std::memory_order_relaxed);
    frac_y.store(static_cast<float>(true_y - render_win_y), std::memory_order_relaxed);
}

void SmoothCamera::sync_logic_window() {
    if (!df::global::window_x || !df::global::window_y) return;
    if (render_win_x == -1) return;
    if (*df::global::window_x != render_win_x || *df::global::window_y != render_win_y) {
        *df::global::window_x = render_win_x;
        *df::global::window_y = render_win_y;
        if (df::global::gps && df::global::gps->force_full_display_count < 1) {
            df::global::gps->force_full_display_count = 1;
        }
    }
}

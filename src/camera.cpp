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
}

void SmoothCamera::zoom_in() {
    if (target_zoom == 0 && df::global::gps) target_zoom = df::global::gps->viewport_zoom_factor;
    target_zoom += zoom_step;
    if (target_zoom > 128.0) target_zoom = 128.0; // clamp max zoom
}

void SmoothCamera::zoom_out() {
    if (target_zoom == 0 && df::global::gps) target_zoom = df::global::gps->viewport_zoom_factor;
    target_zoom -= zoom_step;
    if (target_zoom < 16.0) target_zoom = 16.0; // clamp min zoom
}

static bool is_physical_up_held() { return (GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD8) & 0x8000); }
static bool is_physical_down_held() { return (GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD2) & 0x8000); }
static bool is_physical_left_held() { return (GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD4) & 0x8000); }
static bool is_physical_right_held() { return (GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD6) & 0x8000); }

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
            true_zoom = target_zoom = df::global::gps->viewport_zoom_factor;
        }
        return;
    }
    
    float dt = std::chrono::duration<float>(now - last_frame).count();
    last_frame = now;
    if (dt > 0.1f) dt = 0.1f; // Clamp dt
    
    if (!df::global::window_x || !df::global::window_y || !df::global::gps || !df::global::world) return;
    
    if (!is_physical_up_held()) panning_up = false;
    if (!is_physical_down_held()) panning_down = false;
    if (!is_physical_left_held()) panning_left = false;
    if (!is_physical_right_held()) panning_right = false;
    
    int win_x = *df::global::window_x;
    int win_y = *df::global::window_y;
    int expected_win_x = static_cast<int>(std::floor(true_x));
    int expected_win_y = static_cast<int>(std::floor(true_y));
    
    if (win_x != expected_win_x || win_y != expected_win_y) {
        // External camera jump (middle-mouse or edge clamp)
        true_x = win_x;
        true_y = win_y;
        vel_x = 0;
        vel_y = 0;
        expected_win_x = win_x;
        expected_win_y = win_y;
    }
    
    // Zoom sync and interpolation
    int game_zoom = df::global::gps->viewport_zoom_factor;
    int expected_zoom = static_cast<int>(std::round(true_zoom));
    if (game_zoom != expected_zoom && std::abs(game_zoom - expected_zoom) > 1) {
        true_zoom = target_zoom = game_zoom;
    } else {
        float zoom_lerp = 1.0f - std::exp(-zoom_easing * dt);
        true_zoom += (target_zoom - true_zoom) * zoom_lerp;
        int new_zoom = static_cast<int>(std::round(true_zoom));
        if (new_zoom != game_zoom) {
            df::global::gps->viewport_zoom_factor = new_zoom;
        }
    }
    
    // Input vectors
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
    
    // Apply instant velocity or friction
    if (dx != 0 || dy != 0) {
        vel_x = dx * max_speed;
        vel_y = dy * max_speed;
    } else {
        vel_x -= vel_x * friction * dt;
        vel_y -= vel_y * friction * dt;
        if (std::abs(vel_x) < 0.5f) vel_x = 0;
        if (std::abs(vel_y) < 0.5f) vel_y = 0;
    }
    
    true_x += vel_x * dt;
    true_y += vel_y * dt;
    
    // Map edge clamping
    int width = df::global::gps->main_viewport->dim_x;
    int height = df::global::gps->main_viewport->dim_y;
    int min_x = -width / 2;
    int min_y = -height / 2;
    int max_x = df::global::world->map.x_count - (width / 2);
    int max_y = df::global::world->map.y_count - (height / 2);
    
    if (true_x < min_x) { true_x = min_x; vel_x = 0; }
    if (true_x > max_x) { true_x = max_x; vel_x = 0; }
    if (true_y < min_y) { true_y = min_y; vel_y = 0; }
    if (true_y > max_y) { true_y = max_y; vel_y = 0; }
    
    int new_win_x = static_cast<int>(std::floor(true_x));
    int new_win_y = static_cast<int>(std::floor(true_y));
    
    if (new_win_x != win_x || new_win_y != win_y) {
        *df::global::window_x = new_win_x;
        *df::global::window_y = new_win_y;
    }
}

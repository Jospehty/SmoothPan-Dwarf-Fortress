#include "camera.h"
#include "df/global_objects.h"
#include <cmath>
#include <windows.h>

using namespace DFHack;

SmoothCamera g_camera;

void SmoothCamera::reset() {
    first_frame = true;
    panning_up = false;
    panning_down = false;
    panning_left = false;
    panning_right = false;
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
            true_x = target_x = *df::global::window_x;
            true_y = target_y = *df::global::window_y;
        }
        return;
    }
    
    float dt = std::chrono::duration<float>(now - last_frame).count();
    last_frame = now;
    if (dt > 0.1f) dt = 0.1f; // Clamp dt to prevent huge jumps
    
    if (!df::global::window_x || !df::global::window_y) return;
    
    // Check if physical keys are released to cancel panning
    if (!is_physical_up_held()) panning_up = false;
    if (!is_physical_down_held()) panning_down = false;
    if (!is_physical_left_held()) panning_left = false;
    if (!is_physical_right_held()) panning_right = false;
    
    // Sync to game camera if it jumped externally (minimap, tracking, edge clamping)
    int win_x = *df::global::window_x;
    int win_y = *df::global::window_y;
    int expected_win_x = static_cast<int>(std::floor(true_x));
    int expected_win_y = static_cast<int>(std::floor(true_y));
    
    if (win_x != expected_win_x || win_y != expected_win_y) {
        true_x = target_x = win_x;
        true_y = target_y = win_y;
        expected_win_x = win_x;
        expected_win_y = win_y;
    }
    
    // Apply velocity
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
    
    target_x += dx * speed * dt;
    target_y += dy * speed * dt;
    
    // Decay target back to true_x if no keys held (stops runaway target if hitting map edge)
    if (dx == 0 && dy == 0) {
        target_x = true_x;
        target_y = true_y;
    }
    
    // Ease towards target
    float lerp_factor = 1.0f - std::exp(-easing_factor * dt);
    true_x += (target_x - true_x) * lerp_factor;
    true_y += (target_y - true_y) * lerp_factor;
    
    // Sync integer boundaries back to game
    int new_win_x = static_cast<int>(std::floor(true_x));
    int new_win_y = static_cast<int>(std::floor(true_y));
    
    if (new_win_x != win_x || new_win_y != win_y) {
        *df::global::window_x = new_win_x;
        *df::global::window_y = new_win_y;
    }
}

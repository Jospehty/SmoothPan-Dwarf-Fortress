#pragma once
#include <chrono>

struct SmoothCamera {
    double true_x = 0;
    double true_y = 0;
    
    double vel_x = 0;
    double vel_y = 0;
    
    double true_zoom = 0;
    double target_zoom = 0;
    
    std::chrono::steady_clock::time_point last_frame;
    bool first_frame = true;
    
    float max_speed = 50.0f;      // Max tiles per second panning
    float friction = 15.0f;       // Deceleration when keys released
    
    // Pan state activated by game context (feed), maintained by physical key hold (render)
    bool panning_up = false;
    bool panning_down = false;
    bool panning_left = false;
    bool panning_right = false;

    // Zoom tracking
    float true_zoom = 32.0f;
    float target_zoom = 32.0f;
    float zoom_easing = 15.0f;
    bool is_waiting_for_native_zoom = false;
    int last_game_zoom = 32;

    void reset();
    void update(float dt);
    
    // Zoom control
    void zoom_in() { target_zoom *= 1.2f; }
    void zoom_out() { target_zoom /= 1.2f; };
};

extern SmoothCamera g_camera;

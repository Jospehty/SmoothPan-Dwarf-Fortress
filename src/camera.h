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
    float zoom_easing = 15.0f;    // Zoom interpolation speed
    float zoom_step = 8.0f;       // Pixels to jump per scroll
    
    // Pan state activated by game context (feed), maintained by physical key hold (render)
    bool panning_up = false;
    bool panning_down = false;
    bool panning_left = false;
    bool panning_right = false;
    
    void update();
    void reset();
    void zoom_in();
    void zoom_out();
};

extern SmoothCamera g_camera;

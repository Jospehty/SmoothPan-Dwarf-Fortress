#pragma once
#include <chrono>

struct SmoothCamera {
    double true_x = 0;
    double true_y = 0;
    
    double target_x = 0;
    double target_y = 0;
    
    std::chrono::steady_clock::time_point last_frame;
    bool first_frame = true;
    
    float speed = 25.0f;          // tiles per second panning
    float easing_factor = 15.0f;  // exponential smoothing rate
    
    // Pan state activated by game context (feed), maintained by physical key hold (render)
    bool panning_up = false;
    bool panning_down = false;
    bool panning_left = false;
    bool panning_right = false;
    
    void update();
    void reset();
};

extern SmoothCamera g_camera;

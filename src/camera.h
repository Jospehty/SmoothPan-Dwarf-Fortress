#pragma once
#include <chrono>
#include <atomic>

struct SmoothCamera {
    double true_x = 0;
    double true_y = 0;
    
    double vel_x = 0;
    double vel_y = 0;
    
    std::atomic<float> next_frac_x{0.0f};
    std::atomic<float> next_frac_y{0.0f};
    std::atomic<float> current_frac_x{0.0f};
    std::atomic<float> current_frac_y{0.0f};
    std::chrono::steady_clock::time_point last_frame;
    bool first_frame = true;
    
    float max_speed = 50.0f;      // Max tiles per second panning
    float friction = 15.0f;       // Deceleration when keys released
    
    // Pan state activated by game context (feed), maintained by physical key hold (render)
    bool panning_up = false;
    bool panning_down = false;
    bool panning_left = false;
    bool panning_right = false;



    void reset();
    void update();
};

extern SmoothCamera g_camera;

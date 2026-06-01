#pragma once
#include <chrono>
#include <atomic>

struct SmoothCamera {
    struct FrameSnapshot {
        double true_x = 0;
        double true_y = 0;
        int window_x = 0;
        int window_y = 0;
        float frac_x = 0;
        float frac_y = 0;
        float vel_x = 0;
        float vel_y = 0;
        bool panning_up = false;
        bool panning_down = false;
        bool panning_left = false;
        bool panning_right = false;
        int overscan_x = 0;
        int overscan_y = 0;
        bool overscan_active = false;
        char overscan_reason[64] = "none";
        float shift_x = 0;
        float shift_y = 0;
        int gap_x = 0;
        int gap_y = 0;
        int saved_window_x = 0;
        int saved_window_y = 0;
        int applied_ppc_x = -1;
        int applied_ppc_y = -1;
    };

    double true_x = 0;
    double true_y = 0;
    
    double vel_x = 0;
    double vel_y = 0;
    
    std::atomic<float> frac_x{0.0f};
    std::atomic<float> frac_y{0.0f};
    std::atomic<float> frame_frac_x{0.0f};
    std::atomic<float> frame_frac_y{0.0f};
    std::chrono::steady_clock::time_point last_frame;
    bool first_frame = true;
    
    float max_speed = 35.0f;
    float friction = 12.0f;
    
    bool panning_up = false;
    bool panning_down = false;
    bool panning_left = false;
    bool panning_right = false;

    int overscan_tiles_x = 0;
    int overscan_tiles_y = 0;
    int saved_window_x = 0;
    int saved_window_y = 0;
    bool overscan_active = false;
    int last_zoom = 0;
    int last_vel_sign_x = 0;
    int last_vel_sign_y = 0;

    FrameSnapshot last_snapshot;

    void reset();
    void update();
    void begin_render_overscan();
    void restore_window_overscan();
    void end_render_overscan();
    void snapshot_render_state();
    void freeze_render_frac();

    float render_frac_x = 0.0f;
    float render_frac_y = 0.0f;

    float render_shift_x() const;
    float render_shift_y() const;
    int pixel_shift_x() const;
    int pixel_shift_y() const;
    int gap_shift_x() const;
    int gap_shift_y() const;
};

extern SmoothCamera g_camera;

void smoothpan_set_minimap_full_rebuild(bool full);
bool smoothpan_minimap_full_rebuild();
void smoothpan_set_minimap_pan_mode(const char* mode);
const char* smoothpan_minimap_mode_name();
int smoothpan_minimap_mustmake_interval_ms();
void smoothpan_set_minimap_mustmake_interval_ms(int ms);

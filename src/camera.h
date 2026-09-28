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
        float render_zoom_scale = 1.0f;
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
    bool middle_drag_active = false;

    int overscan_tiles_x = 0;
    int overscan_tiles_y = 0;
    int saved_window_x = 0;
    int saved_window_y = 0;
    bool overscan_active = false;
    int last_zoom = 0;
    int last_vel_sign_x = 0;
    int last_vel_sign_y = 0;
    // Set by update() when something other than SmoothPan moved window_x/y
    // since the previous tick (vanilla recentre on zoom, scripts, ...).
    bool external_window_move = false;

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
    float render_zoom_scale = 1.0f;

    // Present-time pan offset.  DF presents more often than it runs the
    // dwarfmode render() interpose (where freeze_render_frac runs) once the
    // graphics cap is above the main loop rate: at 180 fps every other present
    // re-drew the map with the same frozen frac -- pixel-identical to the frame
    // before (measured d1 = 0.0), a 2:1 judder while panning.  begin_present()
    // runs on the render thread at the start of every frame (renderer_2d::
    // render) and extrapolates the frozen offset by the camera's measured
    // velocity, clamped inside the current tile (the bake only covers it).
    // Render-thread only; the main thread keeps using render_frac_x/y.
    std::atomic<long long> frozen_us{0};
    std::atomic<float> frozen_vx{0.0f};          // tiles/s, finite difference of shown position
    std::atomic<float> frozen_vy{0.0f};
    double last_freeze_pos_x = 0.0, last_freeze_pos_y = 0.0;
    long long last_freeze_us = 0;
    float present_frac_x = 0.0f;
    float present_frac_y = 0.0f;
    bool present_valid = false;
    void begin_present();
    float shown_frac_x() const { return present_valid ? present_frac_x : render_frac_x; }
    float shown_frac_y() const { return present_valid ? present_frac_y : render_frac_y; }

    float render_shift_x() const;
    float render_shift_y() const;
    int pixel_shift_x() const;
    int pixel_shift_y() const;
    int gap_shift_x() const;
    int gap_shift_y() const;

    void sync_true_from_window();
    bool commit_true_position();
};

extern SmoothCamera g_camera;

// Zoom-transition window (Stage 1, build 3.21.0+; wall-clock since 3.25.1).
// Opened whenever gps->viewport_zoom_factor changes so the SDL clip code can
// relax the origin-grid clip across vanilla's non-atomic multi-frame rebake
// (the NARROW/WIDE alternation that caused black edge bands).
//
// This used to be a count of 8 frames decremented once per SDL_RenderPresent,
// which made the guard's real duration scale with the frame rate: 160 ms at
// DF's default 50 fps cap but only ~44 ms at 180 fps.  Vanilla's rebake takes
// a wall-clock amount of time, so above ~60 fps the window closed while the
// rebake was still running, the clip was re-imposed mid-bake and starved map
// blits, and compositor::count_ok() stopped guarding -- the black sections and
// "notched" zoom players saw at high fps.  It is now a real deadline.
extern std::atomic<long long> g_zoom_transition_until_us;
extern std::atomic<int> g_zoom_prev_z;

// True while the post-zoom rebake guard is open.
bool zoom_transition_active();
// Milliseconds left on that guard (0 when closed).  Telemetry only.
int zoom_transition_remaining_ms();

extern int g_sp_mmb_held;
extern int g_sp_middle_drag;
extern int g_sp_mmb_scroll;
extern int g_sp_mmb_sticky;
extern int g_sp_mmb_gate;
extern int g_sp_mmb_dx;
extern int g_sp_mmb_dy;

bool smoothpan_middle_mouse_button_held();
bool smoothpan_middle_mouse_map_gate(int precise_x, int precise_y);
void smoothpan_middle_mouse_update();
void smoothpan_middle_mouse_reset();

void smoothpan_notify_zoom_commit();

void smoothpan_set_minimap_full_rebuild(bool full);
bool smoothpan_minimap_full_rebuild();
void smoothpan_set_minimap_pan_mode(const char* mode);
const char* smoothpan_minimap_mode_name();
int smoothpan_minimap_mustmake_interval_ms();
void smoothpan_set_minimap_mustmake_interval_ms(int ms);

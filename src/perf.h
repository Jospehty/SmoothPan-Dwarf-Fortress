#pragma once

namespace DFHack {
class color_ostream;
}

// Frame profiler — zero overhead unless capturing (F12 / smoothpan perf).

struct PerfCameraSample {
    bool panning = false;
    bool pan_keys = false;
    int force_full_display = 0;
    bool ffd_bump_tile = false;
    bool ffd_bump_pan = false;
    bool ffd_skip = false;
    const char* ffd_reason = "";
    bool tile_step = false;
    bool pan_end_sync = false;
    bool minimap_update = false;
    bool minimap_mustmake = false;
    int window_x = 0;
    int window_y = 0;
    int frac_x100 = 0;
    int frac_y100 = 0;
    int vel_x10 = 0;
    int vel_y10 = 0;
    int shift_x10 = 0;
    int shift_y10 = 0;
    int zoom = 0;
};

bool perf_is_active();
void perf_start_capture(int frames);
int perf_frames_remaining();

void perf_note_blit_hook(bool shifted, double classify_us, double total_us);
void perf_note_edge_fill(double fill_us);
void perf_note_camera_sample(const PerfCameraSample& sample);

void perf_on_present(bool plugin_enabled);

void perf_print_summary(DFHack::color_ostream& out);
void perf_reset_summary();

bool perf_capture_just_finished();
void perf_clear_capture_finished();

// Legacy shim — avoid if possible.
void perf_note_camera_update(bool panning, int force_full_display,
                             bool minimap_dirty, bool tile_step);

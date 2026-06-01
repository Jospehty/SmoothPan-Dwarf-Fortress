#include "perf.h"

#include "ColorText.h"

using namespace DFHack;

#include "debug_paths.h"
#include "version.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

namespace {

struct PerfFrame {
    int index = 0;
    double dt_ms = 0;
    bool plugin_on = false;
    bool panning = false;
    bool pan_keys = false;
    int frac_x100 = 0;
    int frac_y100 = 0;
    int vel_x10 = 0;
    int vel_y10 = 0;
    int shift_x10 = 0;
    int shift_y10 = 0;
    int win_x = 0;
    int win_y = 0;
    int zoom = 0;
    int ffd = 0;
    bool ffd_bump_tile = false;
    bool ffd_bump_pan = false;
    bool ffd_skip = false;
    char ffd_reason[16] = {};
    bool tile_step = false;
    bool pan_end = false;
    bool mm_update = false;
    bool mm_mustmake = false;
    int blits = 0;
    int shifted = 0;
    int edge_fills = 0;
    double classify_us = 0;
    double edge_us = 0;
    double hook_us = 0;
};

static int g_perf_frames_remaining = 0;
static bool g_perf_capture_finished = false;
static FILE* g_perf_file = nullptr;
static int g_frame_index = 0;

static std::chrono::steady_clock::time_point g_last_present;
static bool g_have_last_present = false;

static PerfCameraSample g_cur_cam;
static bool g_have_cam = false;

static int g_cur_blits = 0;
static int g_cur_shifted = 0;
static int g_cur_edge_fills = 0;
static double g_cur_classify_us = 0;
static double g_cur_edge_us = 0;
static double g_cur_hook_us = 0;
static bool g_cur_plugin_on = false;

static std::vector<PerfFrame> g_history;
static constexpr size_t kHistoryMax = 900;

static double fps_from_dt(double dt_ms) {
    return dt_ms > 0.001 ? 1000.0 / dt_ms : 0.0;
}

static double avg_fps(const std::vector<PerfFrame>& frames) {
    double sum = 0;
    int n = 0;
    for (const auto& fr : frames) {
        if (fr.dt_ms > 0.001) {
            sum += fps_from_dt(fr.dt_ms);
            n++;
        }
    }
    return n ? sum / n : 0.0;
}

static double avg_dt(const std::vector<PerfFrame>& frames) {
    double sum = 0;
    int n = 0;
    for (const auto& fr : frames) {
        if (fr.dt_ms > 0.001) {
            sum += fr.dt_ms;
            n++;
        }
    }
    return n ? sum / n : 0.0;
}

static double percentile_dt(std::vector<double> dts, double p) {
    if (dts.empty()) return 0;
    std::sort(dts.begin(), dts.end());
    size_t idx = static_cast<size_t>(p * (dts.size() - 1));
    return dts[idx];
}

static void perf_close_file() {
    if (!g_perf_file) return;
    fclose(g_perf_file);
    g_perf_file = nullptr;
}

static void perf_log_frame(const PerfFrame& f) {
    if (!g_perf_file) return;
    fprintf(g_perf_file,
            "PERF2 idx=%d dt_ms=%.3f fps=%.1f est_df_ms=%.3f plugin=%d "
            "pan=%d keys=%d ffd=%d ffd_tile=%d ffd_pan=%d ffd_skip=%d ffd_r=%s "
            "fx=%d fy=%d vx=%d vy=%d sx=%d sy=%d win=(%d,%d) zoom=%d "
            "tile=%d pend=%d mm_u=%d mm_m=%d "
            "blits=%d shifted=%d edge_fills=%d cls_us=%.0f edge_us=%.0f hook_us=%.0f\n",
            f.index, f.dt_ms, fps_from_dt(f.dt_ms),
            std::max(0.0, f.dt_ms - f.hook_us / 1000.0),
            f.plugin_on ? 1 : 0,
            f.panning ? 1 : 0, f.pan_keys ? 1 : 0,
            f.ffd, f.ffd_bump_tile ? 1 : 0, f.ffd_bump_pan ? 1 : 0,
            f.ffd_skip ? 1 : 0, f.ffd_reason[0] ? f.ffd_reason : "-",
            f.frac_x100, f.frac_y100, f.vel_x10, f.vel_y10, f.shift_x10, f.shift_y10,
            f.win_x, f.win_y, f.zoom,
            f.tile_step ? 1 : 0, f.pan_end ? 1 : 0,
            f.mm_update ? 1 : 0, f.mm_mustmake ? 1 : 0,
            f.blits, f.shifted, f.edge_fills,
            f.classify_us, f.edge_us, f.hook_us);
    fflush(g_perf_file);
}

static void perf_write_histogram(FILE* f, const std::vector<PerfFrame>& frames) {
    int b_180 = 0;  // <= 5.56ms
    int b_144 = 0;  // <= 6.94
    int b_120 = 0;  // <= 8.33
    int b_90 = 0;   // <= 11.11
    int b_60 = 0;   // <= 16.67
    int b_slow = 0;
    for (const auto& fr : frames) {
        if (fr.dt_ms <= 0.001) continue;
        if (fr.dt_ms <= 5.56) b_180++;
        else if (fr.dt_ms <= 6.94) b_144++;
        else if (fr.dt_ms <= 8.33) b_120++;
        else if (fr.dt_ms <= 11.11) b_90++;
        else if (fr.dt_ms <= 16.67) b_60++;
        else b_slow++;
    }
    const int n = static_cast<int>(frames.size());
    fprintf(f, "PERF_HISTOGRAM (frame time buckets, target 180Hz = <=5.56ms)\n");
    fprintf(f, "  >=180fps: %d (%.0f%%)\n", b_180, n ? 100.0 * b_180 / n : 0.0);
    fprintf(f, "  144-180:  %d (%.0f%%)\n", b_144, n ? 100.0 * b_144 / n : 0.0);
    fprintf(f, "  120-144:  %d (%.0f%%)\n", b_120, n ? 100.0 * b_120 / n : 0.0);
    fprintf(f, "  90-120:   %d (%.0f%%)\n", b_90, n ? 100.0 * b_90 / n : 0.0);
    fprintf(f, "  60-90:    %d (%.0f%%)\n", b_60, n ? 100.0 * b_60 / n : 0.0);
    fprintf(f, "  <60fps:   %d (%.0f%%)\n", b_slow, n ? 100.0 * b_slow / n : 0.0);
}

static void perf_write_category(FILE* f, const char* name,
                                const std::vector<PerfFrame>& frames) {
    if (frames.empty()) {
        fprintf(f, "  %-28s count=0\n", name);
        return;
    }
    fprintf(f, "  %-28s count=%zu fps_avg=%.1f dt_avg=%.2fms p95=%.2fms hook_us=%.0f\n",
            name, frames.size(), avg_fps(frames), avg_dt(frames),
            percentile_dt([&frames]() {
                std::vector<double> dts;
                for (const auto& fr : frames) {
                    if (fr.dt_ms > 0.001) dts.push_back(fr.dt_ms);
                }
                return dts;
            }(), 0.95),
            std::accumulate(frames.begin(), frames.end(), 0.0,
                [](double a, const PerfFrame& fr) { return a + fr.hook_us; }) / frames.size());
}

static void perf_write_summary_block(FILE* f) {
    if (g_history.empty()) return;

    std::vector<PerfFrame> idle, pan, pan_ffd0, pan_ffd2, pan_ffd_skipped, tile, tile_mm, tile_no_mm;
    std::vector<PerfFrame> pan_end_frames, spikes;
    std::vector<PerfFrame> by_ffd0, by_ffd1, by_ffd2, by_ffd3;
    int reason_flat = 0, reason_cliff = 0, reason_open = 0, reason_edge = 0, reason_other = 0;

    double min_fps = 1e9;
    for (const auto& fr : g_history) {
        if (fr.dt_ms > 0.001) {
            min_fps = std::min(min_fps, fps_from_dt(fr.dt_ms));
        }
        if (!fr.panning) idle.push_back(fr);
        else pan.push_back(fr);
        if (fr.panning && fr.ffd <= 0) pan_ffd0.push_back(fr);
        if (fr.panning && fr.ffd >= 2) pan_ffd2.push_back(fr);
        if (fr.panning && fr.ffd_skip) pan_ffd_skipped.push_back(fr);
        if (fr.tile_step) tile.push_back(fr);
        if (fr.tile_step && fr.mm_mustmake) tile_mm.push_back(fr);
        if (fr.tile_step && !fr.mm_mustmake) tile_no_mm.push_back(fr);
        if (fr.pan_end) pan_end_frames.push_back(fr);
        if (fr.dt_ms > 12.0) spikes.push_back(fr);
        if (fr.ffd <= 0) by_ffd0.push_back(fr);
        else if (fr.ffd == 1) by_ffd1.push_back(fr);
        else if (fr.ffd == 2) by_ffd2.push_back(fr);
        else by_ffd3.push_back(fr);
        if (fr.ffd_skip && fr.ffd_reason[0]) {
            if (strcmp(fr.ffd_reason, "flat") == 0) reason_flat++;
            else if (strcmp(fr.ffd_reason, "cliff") == 0) reason_cliff++;
            else if (strcmp(fr.ffd_reason, "open") == 0) reason_open++;
            else if (strcmp(fr.ffd_reason, "edge") == 0) reason_edge++;
            else reason_other++;
        }
    }

    fprintf(f, "PERF_SUMMARY frames=%zu fps_avg=%.1f fps_min=%.1f build=%s\n",
            g_history.size(), avg_fps(g_history), min_fps < 1e8 ? min_fps : 0.0,
            SMOOTHPAN_BUILD_VERSION);
    fprintf(f, "  idle_fps_avg=%.1f (%zu)  pan_fps_avg=%.1f (%zu)\n",
            avg_fps(idle), idle.size(), avg_fps(pan), pan.size());
    fprintf(f, "  hook_us_avg=%.0f  classify_us_avg=%.0f  edge_us_avg=%.0f  blits_avg=%.0f\n",
            std::accumulate(g_history.begin(), g_history.end(), 0.0,
                [](double a, const PerfFrame& fr) { return a + fr.hook_us; }) / g_history.size(),
            std::accumulate(g_history.begin(), g_history.end(), 0.0,
                [](double a, const PerfFrame& fr) { return a + fr.classify_us; }) / g_history.size(),
            std::accumulate(g_history.begin(), g_history.end(), 0.0,
                [](double a, const PerfFrame& fr) { return a + fr.edge_us; }) / g_history.size(),
            std::accumulate(g_history.begin(), g_history.end(), 0.0,
                [](double a, const PerfFrame& fr) { return a + fr.blits; }) / g_history.size());
    fprintf(f, "\n");

    perf_write_histogram(f, g_history);
    fprintf(f, "\nPERF_CATEGORIES\n");
    perf_write_category(f, "idle", idle);
    perf_write_category(f, "pan_all", pan);
    perf_write_category(f, "pan_ffd0", pan_ffd0);
    perf_write_category(f, "pan_ffd2plus", pan_ffd2);
    perf_write_category(f, "pan_ffd_skipped", pan_ffd_skipped);
    perf_write_category(f, "tile_step", tile);
    perf_write_category(f, "tile_mustmake", tile_mm);
    perf_write_category(f, "tile_update_only", tile_no_mm);
    perf_write_category(f, "pan_end_sync", pan_end_frames);
    perf_write_category(f, "spike_gt_12ms", spikes);

    fprintf(f, "\nPERF_FFD_BREAKDOWN\n");
    perf_write_category(f, "ffd=0", by_ffd0);
    perf_write_category(f, "ffd=1", by_ffd1);
    perf_write_category(f, "ffd=2", by_ffd2);
    perf_write_category(f, "ffd>=3", by_ffd3);

    fprintf(f, "\nPERF_FFD_POLICY (pan ffd_skip reason counts)\n");
    fprintf(f, "  flat=%d cliff=%d open=%d edge=%d other=%d\n",
            reason_flat, reason_cliff, reason_open, reason_edge, reason_other);

    std::vector<double> all_dts, pan_dts;
    for (const auto& fr : g_history) {
        if (fr.dt_ms > 0.001) {
            all_dts.push_back(fr.dt_ms);
            if (fr.panning) pan_dts.push_back(fr.dt_ms);
        }
    }
    fprintf(f, "\nPERF_PERCENTILES_MS\n");
    fprintf(f, "  all  p50=%.2f p95=%.2f p99=%.2f\n",
            percentile_dt(all_dts, 0.50), percentile_dt(all_dts, 0.95), percentile_dt(all_dts, 0.99));
    fprintf(f, "  pan  p50=%.2f p95=%.2f p99=%.2f\n",
            percentile_dt(pan_dts, 0.50), percentile_dt(pan_dts, 0.95), percentile_dt(pan_dts, 0.99));

    fprintf(f, "\nPERF_SLOWEST (top 12 by dt_ms)\n");
    std::vector<PerfFrame> sorted = g_history;
    std::sort(sorted.begin(), sorted.end(),
              [](const PerfFrame& a, const PerfFrame& b) { return a.dt_ms > b.dt_ms; });
    for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
        const auto& s = sorted[i];
        if (s.dt_ms <= 0.001) continue;
        fprintf(f, "  #%d dt=%.2fms pan=%d ffd=%d tile=%d mm_m=%d pend=%d ffd_pan=%d blits=%d hook=%.0fus\n",
                s.index, s.dt_ms, s.panning ? 1 : 0, s.ffd,
                s.tile_step ? 1 : 0, s.mm_mustmake ? 1 : 0, s.pan_end ? 1 : 0,
                s.ffd_bump_pan ? 1 : 0, s.blits, s.hook_us);
    }

    fprintf(f, "\n# Field guide:\n");
    fprintf(f, "#  est_df_ms = dt_ms - hook_us (SmoothPan SDL hook only; rest is DF+GPU)\n");
    fprintf(f, "#  ffd_pan = we set force_full_display_count>=2 this tick (z-level rebake)\n");
    fprintf(f, "#  ffd_tile = we set force_full_display_count>=1 on window_x/y step\n");
    fprintf(f, "#  mm_m = minimap mustmake (full minimap rebuild)\n");
    fprintf(f, "#  pend = pan-end minimap sync\n");
    fprintf(f, "#  ffd_skip = smart mode skipped pan ffd bump (flat interior)\n");
    fprintf(f, "#  ffd_r = skip/apply reason: flat|cliff|open|edge|ramp|stair|grate|always|off\n");
    fprintf(f, "#  Target 180Hz: need pan p95 <= 5.56ms — compare pan_ffd_skipped vs pan_ffd2plus\n");
    fflush(f);
}

} // namespace

bool perf_is_active() {
    return g_perf_frames_remaining > 0;
}

void perf_start_capture(int frames) {
    perf_close_file();
    g_history.clear();
    g_perf_frames_remaining = frames > 0 ? frames : 0;
    g_have_last_present = false;
    g_have_cam = false;
    g_frame_index = 0;

    if (frames <= 0) return;

    const char* path = smoothpan_log_path("smoothpan_perf.txt").c_str();
    g_perf_file = fopen(path, "w");
    if (g_perf_file) {
        fprintf(g_perf_file, "# SmoothPan detailed perf %s — %d frames\n", SMOOTHPAN_BUILD_VERSION, frames);
        fprintf(g_perf_file, "# Protocol: stand still ~3s -> pan WASD ~4s -> coast/stop ~3s\n");
        fprintf(g_perf_file, "# F12 or: smoothpan perf %d\n\n", frames);
        fflush(g_perf_file);
    }
}

int perf_frames_remaining() {
    return g_perf_frames_remaining;
}

void perf_note_blit_hook(bool shifted, double classify_us, double total_us) {
    if (!perf_is_active()) return;
    g_cur_blits++;
    if (shifted) g_cur_shifted++;
    g_cur_classify_us += classify_us;
    g_cur_hook_us += total_us;
}

void perf_note_edge_fill(double fill_us) {
    if (!perf_is_active()) return;
    g_cur_edge_fills++;
    g_cur_edge_us += fill_us;
}

void perf_note_camera_sample(const PerfCameraSample& sample) {
    if (!perf_is_active()) return;
    g_cur_cam = sample;
    g_have_cam = true;
}

void perf_note_camera_update(bool panning, int force_full_display,
                             bool minimap_dirty, bool tile_step) {
    if (!perf_is_active()) return;
    PerfCameraSample s;
    s.panning = panning;
    s.force_full_display = force_full_display;
    s.minimap_update = minimap_dirty;
    s.tile_step = tile_step;
    perf_note_camera_sample(s);
}

void perf_on_present(bool plugin_enabled) {
    if (!perf_is_active() && !g_have_last_present) return;

    auto now = std::chrono::steady_clock::now();
    double dt_ms = 0;
    if (g_have_last_present) {
        dt_ms = std::chrono::duration<double, std::milli>(now - g_last_present).count();
    }
    g_last_present = now;
    g_have_last_present = true;

    if (!perf_is_active()) return;

    PerfFrame f;
    f.index = ++g_frame_index;
    f.dt_ms = dt_ms;
    f.plugin_on = plugin_enabled;
    f.blits = g_cur_blits;
    f.shifted = g_cur_shifted;
    f.edge_fills = g_cur_edge_fills;
    f.classify_us = g_cur_classify_us;
    f.edge_us = g_cur_edge_us;
    f.hook_us = g_cur_hook_us;

    if (g_have_cam) {
        f.panning = g_cur_cam.panning;
        f.pan_keys = g_cur_cam.pan_keys;
        f.ffd = g_cur_cam.force_full_display;
        f.ffd_bump_tile = g_cur_cam.ffd_bump_tile;
        f.ffd_bump_pan = g_cur_cam.ffd_bump_pan;
        f.ffd_skip = g_cur_cam.ffd_skip;
        if (g_cur_cam.ffd_reason) {
            strncpy(f.ffd_reason, g_cur_cam.ffd_reason, sizeof(f.ffd_reason) - 1);
            f.ffd_reason[sizeof(f.ffd_reason) - 1] = '\0';
        }
        f.tile_step = g_cur_cam.tile_step;
        f.pan_end = g_cur_cam.pan_end_sync;
        f.mm_update = g_cur_cam.minimap_update;
        f.mm_mustmake = g_cur_cam.minimap_mustmake;
        f.win_x = g_cur_cam.window_x;
        f.win_y = g_cur_cam.window_y;
        f.frac_x100 = g_cur_cam.frac_x100;
        f.frac_y100 = g_cur_cam.frac_y100;
        f.vel_x10 = g_cur_cam.vel_x10;
        f.vel_y10 = g_cur_cam.vel_y10;
        f.shift_x10 = g_cur_cam.shift_x10;
        f.shift_y10 = g_cur_cam.shift_y10;
        f.zoom = g_cur_cam.zoom;
    }

    g_cur_blits = 0;
    g_cur_shifted = 0;
    g_cur_edge_fills = 0;
    g_cur_classify_us = 0;
    g_cur_edge_us = 0;
    g_cur_hook_us = 0;
    g_have_cam = false;

    if (g_history.size() >= kHistoryMax) {
        g_history.erase(g_history.begin());
    }
    g_history.push_back(f);
    perf_log_frame(f);

    g_perf_frames_remaining--;
    if (g_perf_frames_remaining <= 0) {
        if (g_perf_file) perf_write_summary_block(g_perf_file);
        perf_close_file();
        g_perf_capture_finished = true;
    }
}

bool perf_capture_just_finished() {
    return g_perf_capture_finished;
}

void perf_clear_capture_finished() {
    g_perf_capture_finished = false;
}

void perf_print_summary(color_ostream& out) {
    if (g_history.empty()) {
        out.print("No perf data yet. Run: smoothpan perf 450 (or F12 in fortress)\n");
        return;
    }
    char buf[768];
    FILE* mem = tmpfile();
    if (mem) {
        perf_write_summary_block(mem);
        rewind(mem);
        while (fgets(buf, sizeof(buf), mem)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }
            out.print("{}\n", buf);
        }
        fclose(mem);
    }
    out.print("Full log: {}\n", smoothpan_log_path("smoothpan_perf.txt"));
}

void perf_reset_summary() {
    g_history.clear();
}

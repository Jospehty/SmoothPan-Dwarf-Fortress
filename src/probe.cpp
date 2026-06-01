#include "probe.h"

#include "camera.h"
#include "debug_paths.h"
#include "shift_mode.h"
#include "version.h"
#include "viewport.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_map_portst.h"

#include <cstdio>
#include <cmath>

int g_probe_frames = 0;

static const ShiftMode kCycleModes[] = {ShiftMode::None, ShiftMode::Sdl, ShiftMode::MapPort};
static const int kCycleModeCount = 3;
static const int kFramesPerMode = 120;

static bool g_probe_auto = false;
static int g_probe_phase = 0;
static int g_probe_phase_frame = 0;

static int g_probe_clip_events = 0;
static int g_probe_target_events = 0;
static int g_probe_viewport_events = 0;
static int g_probe_map_pass = 0;

static int g_probe_blit_total = 0;
static int g_probe_blit_shifted = 0;
static int g_probe_map_total = 0;
static int g_probe_map_shifted = 0;
static int g_probe_tile_total = 0;
static int g_probe_tile_shifted = 0;
static int g_probe_sprite_total = 0;
static int g_probe_sprite_shifted = 0;
static int g_probe_pass_shifted = 0;
static int g_probe_ui_leak_shifted = 0;
static int g_probe_vp_tile_unshifted = 0;
static int g_probe_vp_sprite_unshifted = 0;
static int g_probe_hud_false_pos = 0;

struct ProbePhaseSummary {
    int frames = 0;
    int mismatch_frames = 0;
    int low_map_shift_frames = 0;
    int ui_leak_frames = 0;
    int unshifted_vp_frames = 0;
    long long blit_total = 0;
    long long blit_shifted = 0;
    long long map_total = 0;
    long long map_shifted = 0;
    long long tile_total = 0;
    long long tile_shifted = 0;
    long long sprite_total = 0;
    long long sprite_shifted = 0;
    long long ui_leak = 0;
    long long vp_tile_unshifted = 0;
    long long vp_sprite_unshifted = 0;
    int max_ui_leak_frame = 0;
    int max_unshifted_tile_frame = 0;
};

static ProbePhaseSummary g_phase_summary;

void probe_reset_frame_counters() {
    g_probe_clip_events = 0;
    g_probe_target_events = 0;
    g_probe_viewport_events = 0;
    g_probe_map_pass = 0;
    g_probe_blit_total = 0;
    g_probe_blit_shifted = 0;
    g_probe_map_total = 0;
    g_probe_map_shifted = 0;
    g_probe_tile_total = 0;
    g_probe_tile_shifted = 0;
    g_probe_sprite_total = 0;
    g_probe_sprite_shifted = 0;
    g_probe_pass_shifted = 0;
    g_probe_ui_leak_shifted = 0;
    g_probe_vp_tile_unshifted = 0;
    g_probe_vp_sprite_unshifted = 0;
    g_probe_hud_false_pos = 0;
}

bool probe_auto_cycle_active() {
    return g_probe_auto;
}

void probe_note_clip_event() { g_probe_clip_events++; }
void probe_note_target_event() { g_probe_target_events++; }
void probe_note_viewport_event() { g_probe_viewport_events++; }
void probe_note_map_pass() { g_probe_map_pass = 1; }

void probe_accumulate_blit(const BlitClassification& c, bool shifted) {
    g_probe_blit_total++;
    if (shifted) g_probe_blit_shifted++;

    if (shifted && g_probe_map_pass) {
        g_probe_pass_shifted++;
    } else if (shifted) {
        g_probe_ui_leak_shifted++;
    }

    if (!c.in_ui && c.intersects_viewport) {
        g_probe_map_total++;
        if (shifted) g_probe_map_shifted++;
        if (c.tile_sized) {
            g_probe_tile_total++;
            if (shifted) g_probe_tile_shifted++;
            else g_probe_vp_tile_unshifted++;
        } else {
            g_probe_sprite_total++;
            if (shifted) g_probe_sprite_shifted++;
            else g_probe_vp_sprite_unshifted++;
        }
    }

    if (c.suspected_hud_false_positive) g_probe_hud_false_pos++;
}

static int pct(int num, int den) {
    if (den <= 0) return -1;
    return static_cast<int>((100LL * num + den / 2) / den);
}

static void probe_write_viewport_header(FILE* f) {
    ViewportRect vp;
    int zoom = 0;
    if (df::global::gps) zoom = df::global::gps->viewport_zoom_factor;
    if (get_strict_viewport_rect(&vp)) {
        fprintf(f, "# viewport=(%d,%d,%d,%d) tile_px=%d zoom=%d\n",
                vp.left, vp.top, vp.right, vp.bottom, vp.tile_px, zoom);
    } else {
        fprintf(f, "# viewport=unknown zoom=%d\n", zoom);
    }
    fprintf(f, "# Per-frame: blits=shift/tot map/tile/sprite rates, pass/ui_leak, vp_unshifted tiles+sprites, mismatch flag\n");
    fprintf(f, "# Phase footer summarizes averages — high vp_unshifted or tile-vs-sprite mismatch => layer ghosting\n");
}

static void probe_write_phase_header(FILE* f, int phase_index) {
    fprintf(f, "\n# --- phase %d/%d mode=%s frames=%d --- wiggle all directions ---\n",
            phase_index + 1, kCycleModeCount,
            shift_mode_name(kCycleModes[phase_index]),
            kFramesPerMode);
    if (phase_index == 0) probe_write_viewport_header(f);
}

static void probe_absorb_frame_into_phase() {
    ProbePhaseSummary& s = g_phase_summary;
    s.frames++;

    const int map_rate = pct(g_probe_map_shifted, g_probe_map_total);
    const int tile_rate = pct(g_probe_tile_shifted, g_probe_tile_total);
    const int sprite_rate = pct(g_probe_sprite_shifted, g_probe_sprite_total);

    if (tile_rate >= 0 && sprite_rate >= 0 && std::abs(tile_rate - sprite_rate) > 15) {
        s.mismatch_frames++;
    }
    if (map_rate >= 0 && map_rate < 50) s.low_map_shift_frames++;
    if (g_probe_ui_leak_shifted > 0) s.ui_leak_frames++;
    if (g_probe_vp_tile_unshifted > 0 || g_probe_vp_sprite_unshifted > 0) s.unshifted_vp_frames++;

    s.blit_total += g_probe_blit_total;
    s.blit_shifted += g_probe_blit_shifted;
    s.map_total += g_probe_map_total;
    s.map_shifted += g_probe_map_shifted;
    s.tile_total += g_probe_tile_total;
    s.tile_shifted += g_probe_tile_shifted;
    s.sprite_total += g_probe_sprite_total;
    s.sprite_shifted += g_probe_sprite_shifted;
    s.ui_leak += g_probe_ui_leak_shifted;
    s.vp_tile_unshifted += g_probe_vp_tile_unshifted;
    s.vp_sprite_unshifted += g_probe_vp_sprite_unshifted;

    if (g_probe_ui_leak_shifted > s.max_ui_leak_frame) s.max_ui_leak_frame = g_probe_ui_leak_shifted;
    if (g_probe_vp_tile_unshifted > s.max_unshifted_tile_frame) s.max_unshifted_tile_frame = g_probe_vp_tile_unshifted;
}

static void probe_write_phase_summary(FILE* f, const char* mode_name) {
    const ProbePhaseSummary& s = g_phase_summary;
    if (s.frames <= 0) return;

    fprintf(f, "# --- phase summary mode=%s frames=%d ---\n", mode_name, s.frames);
    fprintf(f, "#   blit_shift=%lld/%lld (%.1f%%) map=%lld/%lld (%.1f%%) tile=%lld/%lld (%.1f%%) sprite=%lld/%lld (%.1f%%)\n",
            s.blit_shifted, s.blit_total,
            s.blit_total ? 100.0 * s.blit_shifted / s.blit_total : 0.0,
            s.map_shifted, s.map_total,
            s.map_total ? 100.0 * s.map_shifted / s.map_total : 0.0,
            s.tile_shifted, s.tile_total,
            s.tile_total ? 100.0 * s.tile_shifted / s.tile_total : 0.0,
            s.sprite_shifted, s.sprite_total,
            s.sprite_total ? 100.0 * s.sprite_shifted / s.sprite_total : 0.0);
    fprintf(f, "#   ui_leak_shifted=%lld max_frame=%d vp_unshifted_tile=%lld sprite=%lld\n",
            s.ui_leak, s.max_ui_leak_frame, s.vp_tile_unshifted, s.vp_sprite_unshifted);
    fprintf(f, "#   frames: tile_sprite_mismatch=%d low_map_shift=%d ui_leak=%d vp_unshifted=%d\n",
            s.mismatch_frames, s.low_map_shift_frames, s.ui_leak_frames, s.unshifted_vp_frames);
    g_phase_summary = {};
}

static void probe_advance_phase() {
    FILE* f = fopen(smoothpan_log_path("smoothpan_probe.txt").c_str(), "a");
    if (f) {
        probe_write_phase_summary(f, shift_mode_name(kCycleModes[g_probe_phase]));
        fclose(f);
    }

    g_probe_phase++;
    if (g_probe_phase >= kCycleModeCount) {
        g_probe_auto = false;
        g_shift_mode = ShiftMode::Sdl;
        f = fopen(smoothpan_log_path("smoothpan_probe.txt").c_str(), "a");
        if (f) {
            fprintf(f, "\n# === auto cycle complete (%d modes) — restored mode=sdl ===\n", kCycleModeCount);
            fprintf(f, "# Send smoothpan_probe.txt — focus on sdl phase vp_unshifted and tile/sprite mismatch counts\n");
            fclose(f);
        }
        return;
    }

    g_shift_mode = kCycleModes[g_probe_phase];
    g_probe_phase_frame = 0;
    probe_reset_frame_counters();
    g_phase_summary = {};

    f = fopen(smoothpan_log_path("smoothpan_probe.txt").c_str(), "a");
    if (f) {
        probe_write_phase_header(f, g_probe_phase);
        fclose(f);
    }
}

void probe_start_auto_cycle() {
    g_probe_auto = true;
    g_probe_frames = 0;
    g_probe_phase = 0;
    g_probe_phase_frame = 0;
    g_shift_mode = kCycleModes[0];
    g_phase_summary = {};
    probe_reset_frame_counters();

    const char* path = smoothpan_log_path("smoothpan_probe.txt").c_str();
    remove(path);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "# SmoothPan auto diagnostic cycle %s\n", SMOOTHPAN_BUILD_VERSION);
        fprintf(f, "# F9 once, then wiggle WASD all directions through all %d phases (~%d frames, ~%d sec).\n",
                kCycleModeCount, kCycleModeCount * kFramesPerMode,
                (kCycleModeCount * kFramesPerMode + 59) / 60);
        probe_write_phase_header(f, 0);
        fclose(f);
    }
}

static void probe_log_line(FILE* f, int frame_label, const char* mode_name) {
    const SmoothCamera::FrameSnapshot& s = g_camera.last_snapshot;
    int zoom = 0;
    int ppc_present_x = -1;
    int ppc_present_y = -1;
    int mp_dim_x = -1;
    int mp_dim_y = -1;
    if (df::global::gps) {
        zoom = df::global::gps->viewport_zoom_factor;
        if (df::global::gps->main_map_port) {
            auto* mp = df::global::gps->main_map_port;
            ppc_present_x = mp->pixel_perc_x;
            ppc_present_y = mp->pixel_perc_y;
            mp_dim_x = mp->dim_x;
            mp_dim_y = mp->dim_y;
        }
    }

    const int map_rate = pct(g_probe_map_shifted, g_probe_map_total);
    const int tile_rate = pct(g_probe_tile_shifted, g_probe_tile_total);
    const int sprite_rate = pct(g_probe_sprite_shifted, g_probe_sprite_total);
    int mismatch = 0;
    if (tile_rate >= 0 && sprite_rate >= 0 && std::abs(tile_rate - sprite_rate) > 15) mismatch = 1;

    const int win_drift_x = s.window_x - static_cast<int>(std::floor(s.true_x));
    const int win_drift_y = s.window_y - static_cast<int>(std::floor(s.true_y));

    fprintf(f,
            "SMOOTHPAN_%s f=%d mode=%s true=(%.3f,%.3f) win=(%d,%d) saved_win=(%d,%d) drift=(%d,%d) "
            "frac=(%.3f,%.3f) vel=(%.2f,%.2f) pan=%c%c%c%c zoom=%d "
            "os=(%d,%d) os_act=%d os_reason=%s shift=(%.1f,%.1f) gap=(%d,%d) "
            "blits=%d/%d map=%d/%d(%d%%) tile=%d/%d(%d%%) sprite=%d/%d(%d%%) "
            "pass=%d ui_leak=%d vp_unshifted=%d/%d hud_fp=%d map_pass=%d "
            "mismatch=%d clip=%d target=%d viewport=%d "
            "ppc_render=(%d,%d) ppc_present=(%d,%d) dim=(%d,%d)\n",
            SMOOTHPAN_BUILD_VERSION,
            frame_label,
            mode_name,
            s.true_x, s.true_y,
            s.window_x, s.window_y,
            s.saved_window_x, s.saved_window_y,
            win_drift_x, win_drift_y,
            s.frac_x, s.frac_y,
            s.vel_x, s.vel_y,
            s.panning_up ? 'U' : '-',
            s.panning_down ? 'D' : '-',
            s.panning_left ? 'L' : '-',
            s.panning_right ? 'R' : '-',
            zoom,
            s.overscan_x, s.overscan_y,
            s.overscan_active ? 1 : 0,
            s.overscan_reason,
            s.shift_x, s.shift_y,
            s.gap_x, s.gap_y,
            g_probe_blit_shifted, g_probe_blit_total,
            g_probe_map_shifted, g_probe_map_total, map_rate >= 0 ? map_rate : 0,
            g_probe_tile_shifted, g_probe_tile_total, tile_rate >= 0 ? tile_rate : 0,
            g_probe_sprite_shifted, g_probe_sprite_total, sprite_rate >= 0 ? sprite_rate : 0,
            g_probe_pass_shifted, g_probe_ui_leak_shifted,
            g_probe_vp_tile_unshifted, g_probe_vp_sprite_unshifted,
            g_probe_hud_false_pos, g_probe_map_pass,
            mismatch,
            g_probe_clip_events, g_probe_target_events, g_probe_viewport_events,
            s.applied_ppc_x, s.applied_ppc_y,
            ppc_present_x, ppc_present_y,
            mp_dim_x, mp_dim_y);
}

void probe_on_present() {
    if (!g_probe_auto && g_probe_frames <= 0) return;

    if (g_probe_auto) probe_absorb_frame_into_phase();

    const char* path = smoothpan_log_path("smoothpan_probe.txt").c_str();
    FILE* f = fopen(path, "a");
    if (!f) return;

    if (g_probe_auto) {
        g_probe_phase_frame++;
        probe_log_line(f, g_probe_phase_frame, shift_mode_name(g_shift_mode));
        fclose(f);
        probe_reset_frame_counters();

        if (g_probe_phase_frame >= kFramesPerMode) {
            probe_advance_phase();
        }
        return;
    }

    probe_log_line(f, g_probe_frames, shift_mode_name(g_shift_mode));
    fclose(f);
    probe_reset_frame_counters();
    g_probe_frames--;
}

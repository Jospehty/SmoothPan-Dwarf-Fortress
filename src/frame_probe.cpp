#define NOMINMAX
#include "frame_probe.h"

#include "camera.h"
#include "compositor.h"
#include "debug_paths.h"
#include "frame_seq.h"
#include "platform.h"
#include "sdl_fn.h"
#include "sdl_hook.h"
#include "viewport.h"
#include "zoom_camera.h"

#include "Console.h"
#include "Core.h"
#include "df/global_objects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern bool& is_enabled;
extern float g_sp_first_blit_shift_x;
extern int g_sp_first_blit_wx;
extern float g_sp_first_blit_frac;

namespace {

typedef int (*RP_t)(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int);

constexpr int kLines = 6;            // 3 rows + 3 columns across the map
constexpr int kPts = 96;             // signature points per line
constexpr int kSig = kLines * kPts;
constexpr int kBurstPeriod = 60;     // idle: sample kBurstLen consecutive frames every kBurstPeriod
constexpr int kBurstLen = 4;
constexpr int kHotTailMs = 600;      // keep sampling every frame this long after zoom activity

// Strobe: black fraction swinging this much between consecutive frames, this
// many consecutive pairs in a row.  Normal zoom/pan changes the black fraction
// smoothly; a stale-buffer strobe flips it on every present.
constexpr float kStrobeBlackSwing = 0.35f;
constexpr int kStrobeRun = 4;
// Flip-flop: frame differs strongly from the previous one but matches the one
// before (d2 < kFlipRatio * d1).  Smooth motion has d2 >= d1.
constexpr float kFlipD1 = 40.0f;
constexpr float kFlipRatio = 0.35f;
constexpr int kFlipRun = 6;
// Blackout: during zoom activity the map turning black well beyond what it
// was when the activity began, for longer than a rebake stall.  Scoped to zoom
// activity so a legitimately dark view (an unexplored z-level) cannot trip it.
// Added after 'commit direct' left 58-72% of the map black with no alternation
// for the flip/strobe rules to see.
constexpr float kBlackoutRise = 0.40f;
constexpr int kBlackoutMs = 250;

bool g_watchdog = true;
int g_trips = 0;
int g_present = 0;                   // our own present counter
long long g_last_present_us = 0;
long long g_hot_until_us = 0;
bool g_was_hot = false;
float g_hot_base_black = -1.0f;      // black fraction when zoom activity began
long long g_blackout_since_us = 0;

int g_record_left = 0;
bool g_record_nopix = false;   // timing/state only: no GPU readback
int g_test_strobe_left = 0;
std::string g_shot_label;            // non-empty: save the next presented frame
FILE* g_rec = nullptr;

struct Sample {
    bool valid = false;
    int present = -1;
    float black = 0.0f;
    unsigned char sig[kSig];
    // Where the black is: for the three rows, the black run in from the LEFT
    // and RIGHT ends; for the three columns, from the TOP and BOTTOM.  An edge
    // band (vanilla's post-zoom dim lag) shows as a run on one side of every
    // line; interior holes show in 'black' but not here.
    int lead[kLines] = {0};
    int trail[kLines] = {0};
    // Black run from the SCREEN's left edge (x = 0) on the middle row: where the
    // map actually starts on screen.  lead[] counts from DF's viewport left,
    // which is itself a moving value, so it cannot see a strip left of it.
    int left0 = -1;
};
Sample g_s[3];                       // [0] newest, [1] previous, [2] the one before
int g_strobe_run = 0;
int g_flip_run = 0;

// Last-computed values, for status.
float g_last_black = 0.0f, g_last_d1 = -1.0f, g_last_d2 = -1.0f;
int g_samples = 0;

long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

RP_t read_pixels() {
    static RP_t rp = reinterpret_cast<RP_t>(sp_sdl_sym("SDL_RenderReadPixels"));
    return rp;
}

inline int luma(uint32_t p) {
    const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    return (2 * r + 5 * g + b) >> 3;
}
inline bool near_black(uint32_t p) {
    return ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF) < 30;
}

// Sample the finished frame.  false if there is no map viewport to look at.
bool g_take_light = false;   // watchdog bursts: 2 lines instead of 6 (see on_present)

bool take(SDL_Renderer* r, Sample& out) {
    RP_t rp = read_pixels();
    if (!rp || !GetRendererOutputSize_func) return false;
    int w = 0, h = 0;
    if (GetRendererOutputSize_func(r, &w, &h) != 0 || w <= 0 || h <= 0) return false;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    const int L = std::max(0, vp.left), R = std::min(w, vp.right);
    const int T = std::max(0, vp.top), B = std::min(h, vp.bottom);
    if (R - L < 16 || B - T < 16) return false;

    static std::vector<uint32_t> buf;
    long black = 0, total = 0;
    int li = 0;
    for (int k = 1; k <= 3; ++k) {
        for (int axis = 0; axis < 2; ++axis, ++li) {
            // Light mode (watchdog bursts outside recordings) samples only the
            // middle row and column: each readback is a GPU sync, and a burst of
            // six per frame measurably pushed frames off their target refresh.
            if (g_take_light && k != 2) {
                for (int p = 0; p < kPts; ++p) out.sig[li * kPts + p] = 0;
                continue;
            }
            SDL_Rect rc = axis == 0 ? SDL_Rect{ L, T + (B - T) * k / 4, R - L, 1 }
                                    : SDL_Rect{ L + (R - L) * k / 4, T, 1, B - T };
            const int n = rc.w * rc.h;
            buf.resize(static_cast<size_t>(n));
            if (rp(r, &rc, SDL_PIXELFORMAT_ARGB8888, buf.data(), rc.w * 4) != 0) return false;
            for (int i = 0; i < n; ++i) black += near_black(buf[i]) ? 1 : 0;
            { int a = 0; while (a < n && near_black(buf[a])) ++a; out.lead[li] = a; }
            { int b = 0; while (b < n && near_black(buf[n - 1 - b])) ++b; out.trail[li] = b; }
            total += n;
            for (int p = 0; p < kPts; ++p)
                out.sig[li * kPts + p] = static_cast<unsigned char>(luma(buf[static_cast<size_t>(p) * n / kPts]));
        }
    }
    out.black = total ? static_cast<float>(black) / static_cast<float>(total) : 0.0f;
    if (!g_take_light) {
        const int span = std::min(240, w);
        SDL_Rect rc = { 0, T + (B - T) / 2, span, 1 };
        buf.resize(static_cast<size_t>(span));
        if (rp(r, &rc, SDL_PIXELFORMAT_ARGB8888, buf.data(), span * 4) == 0) {
            int a = 0; while (a < span && near_black(buf[a])) ++a;
            out.left0 = a;
        }
    }
    out.valid = true;
    return true;
}

// Camera position actually shown this frame: window tile + the sub-tile frac
// frozen for the render.
float cam_x() {
    return (df::global::window_x ? static_cast<float>(*df::global::window_x) : 0.0f) +
           g_camera.shown_frac_x();
}
float cam_y() {
    return (df::global::window_y ? static_cast<float>(*df::global::window_y) : 0.0f) +
           g_camera.shown_frac_y();
}

int vp_left() { ViewportRect v; return get_strict_viewport_rect(&v) ? v.left : -1; }
int vp_top() { ViewportRect v; return get_strict_viewport_rect(&v) ? v.top : -1; }
int grid_ox() { ViewportRect v; return get_strict_viewport_rect(&v) ? v.origin_x : -1; }
int grid_oy() { ViewportRect v; return get_strict_viewport_rect(&v) ? v.origin_y : -1; }

float sig_diff(const Sample& a, const Sample& b) {
    long s = 0;
    for (int i = 0; i < kSig; ++i) s += std::abs(static_cast<int>(a.sig[i]) - static_cast<int>(b.sig[i]));
    return static_cast<float>(s) / kSig;
}

void trip(const char* why) {
    g_trips++;
    char msg[200];
    if (compositor_active()) {
        snprintf(msg, sizeof(msg), "strobe watchdog: %s", why);
        compositor_emergency_disable(msg);
        DFHack::Core::getInstance().getConsole().printerr(
            "SmoothPan: presented frames were alternating ({}). Compositor switched off.\n", why);
    } else {
        // Still flipping with the compositor already off: take the whole
        // plugin out of the render path.  Interposes fall through when
        // is_enabled is false; the SDL hooks come off at the next present.
        is_enabled = false;
        sp_request_hook_teardown();
        g_camera.end_render_overscan();
        DFHack::Core::getInstance().getConsole().printerr(
            "SmoothPan: display still alternating with the compositor off ({}). Plugin taken out of "
            "the render path; run 'disable smoothpan' to finish.\n", why);
    }
    if (g_rec) fprintf(g_rec, "# WATCHDOG TRIP %d: %s\n", g_trips, why);
    g_strobe_run = 0;
    g_flip_run = 0;
}

}  // namespace

void frame_probe_reset() {
    for (auto& s : g_s) s.valid = false;
    g_strobe_run = 0;
    g_flip_run = 0;
    g_present = 0;
    g_last_present_us = 0;
    g_hot_until_us = 0;
    g_samples = 0;
    g_last_black = 0.0f;
    g_last_d1 = g_last_d2 = -1.0f;
}

static void save_shot(SDL_Renderer* r) {
    RP_t rp = read_pixels();
    int w = 0, h = 0;
    if (!rp || !GetRendererOutputSize_func || GetRendererOutputSize_func(r, &w, &h) != 0 || w <= 0 || h <= 0) return;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    SDL_Rect rc = { 0, 0, w, h };
    if (rp(r, &rc, SDL_PIXELFORMAT_ARGB8888, px.data(), w * 4) != 0) return;
    const std::string path = smoothpan_log_path(("shot_" + g_shot_label + ".ppm").c_str());
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::vector<unsigned char> row(static_cast<size_t>(w) * 3);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t p = px[static_cast<size_t>(y) * w + x];
                row[x * 3] = (p >> 16) & 0xFF; row[x * 3 + 1] = (p >> 8) & 0xFF; row[x * 3 + 2] = p & 0xFF;
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
}

void frame_probe_shot(const char* label) { g_shot_label = label ? label : "shot"; }

void frame_probe_on_present(SDL_Renderer* r) {
    if (!r) return;
    if (!g_shot_label.empty()) { save_shot(r); g_shot_label.clear(); }
    const long long now = now_us();
    const float frame_ms = g_last_present_us ? static_cast<float>(now - g_last_present_us) / 1000.0f : 0.0f;
    g_last_present_us = now;
    const int present = g_present++;

    ZoomCameraInfo zi;
    zoom_camera_info(&zi);
    const bool zoom_hot = zi.gesture || zi.pending || zoom_transition_active();
    if (zoom_hot) g_hot_until_us = now + kHotTailMs * 1000LL;
    const bool hot = now < g_hot_until_us;

    const bool recording = g_record_left > 0;
    const bool testing = g_test_strobe_left > 0;
    const bool burst = (present % kBurstPeriod) < kBurstLen;
    if (!recording && !testing && !(g_watchdog && (hot || burst))) return;

    if (recording && g_record_nopix) {
        // Timing/state only.  Reading pixels back forces a GPU sync, which on a
        // frame where DF is uploading a rebake inflates exactly the frame time
        // we are trying to measure.
        if (g_rec) {
            CompositorFrameInfo ci;
            compositor_last_frame(&ci);
            fprintf(g_rec,
                    "f=%d ms=%.1f black=-1 d1=-1 d2=-1 flip=0 choice=%d cell=%d scale=%.5f "
                    "map=%d v=%.4f tgt=%d baked=%d gest=%d pend=%d trans=%d leaks=%d\n",
                    present, frame_ms, ci.choice, ci.cell, ci.scale, ci.map_blits, zi.v, zi.target_cell,
                    zi.baked_cell, zi.gesture ? 1 : 0, zi.pending ? 1 : 0,
                    zoom_transition_active() ? 1 : 0, compositor_target_leaks());
        }
        if (--g_record_left == 0 && g_rec) { fprintf(g_rec, "# end\n"); fclose(g_rec); g_rec = nullptr; }
        return;
    }

    // Shift history and sample.
    g_s[2] = g_s[1];
    g_s[1] = g_s[0];
    Sample& s = g_s[0];
    s.valid = false;
    s.present = present;
    g_take_light = !recording;
    if (!take(r, s)) {
        if (recording && g_rec) fprintf(g_rec, "f=%d nomap\n", present);
        if (recording && --g_record_left == 0 && g_rec) { fclose(g_rec); g_rec = nullptr; }
        return;
    }
    g_samples++;
    if (g_test_strobe_left > 0) {
        g_test_strobe_left--;
        s.black = (present & 1) ? 1.0f : 0.0f;   // synthetic, detector-only
    }

    const bool c1 = g_s[1].valid && g_s[1].present == present - 1;
    const bool c2 = c1 && g_s[2].valid && g_s[2].present == present - 2;
    const float d1 = c1 ? sig_diff(s, g_s[1]) : -1.0f;
    const float d2 = c2 ? sig_diff(s, g_s[2]) : -1.0f;
    const bool flip = c2 && d1 >= kFlipD1 && d2 < kFlipRatio * d1;
    const float swing = c1 ? std::fabs(s.black - g_s[1].black) : 0.0f;
    g_last_black = s.black;
    // Blackout bookkeeping (zoom activity only).
    if (hot && !g_was_hot) { g_hot_base_black = -1.0f; g_blackout_since_us = 0; }
    g_was_hot = hot;
    if (hot) {
        if (g_hot_base_black < 0.0f) g_hot_base_black = s.black;
        if (s.black > g_hot_base_black + kBlackoutRise) {
            if (!g_blackout_since_us) g_blackout_since_us = now;
        } else {
            g_blackout_since_us = 0;
        }
    } else {
        g_blackout_since_us = 0;
    }
    g_last_d1 = d1;
    g_last_d2 = d2;

    // Watchdog runs over consecutive samples only.
    if (c1 && swing > kStrobeBlackSwing) g_strobe_run++; else g_strobe_run = 0;
    if (flip) g_flip_run++; else if (c2) g_flip_run = 0;

    if (recording && g_rec) {
        CompositorFrameInfo ci;
        compositor_last_frame(&ci);
        fprintf(g_rec,
                "f=%d ms=%.1f black=%.4f d1=%.1f d2=%.1f flip=%d choice=%d cell=%d scale=%.5f "
                "map=%d v=%.4f tgt=%d baked=%d gest=%d pend=%d trans=%d leaks=%d "
                "L=%d,%d,%d R=%d,%d,%d T=%d,%d,%d B=%d,%d,%d cx=%.4f cy=%.4f vpx=%d vpy=%d ox=%d oy=%d L0=%d "
                "bshift=%.2f bwx=%d bfrac=%.3f\n",
                present, frame_ms, s.black, d1, d2, flip ? 1 : 0, ci.choice, ci.cell, ci.scale,
                ci.map_blits, zi.v, zi.target_cell, zi.baked_cell, zi.gesture ? 1 : 0,
                zi.pending ? 1 : 0, zoom_transition_active() ? 1 : 0, compositor_target_leaks(),
                s.lead[0], s.lead[2], s.lead[4], s.trail[0], s.trail[2], s.trail[4],
                s.lead[1], s.lead[3], s.lead[5], s.trail[1], s.trail[3], s.trail[5],
                cam_x(), cam_y(), vp_left(), vp_top(), grid_ox(), grid_oy(), s.left0,
                g_sp_first_blit_shift_x, g_sp_first_blit_wx, g_sp_first_blit_frac);
    }

    if (g_watchdog) {
        char why[120];
        if (g_strobe_run >= kStrobeRun) {
            snprintf(why, sizeof(why), "black fraction swung >%.0f%% on %d consecutive frames",
                     kStrobeBlackSwing * 100.0f, g_strobe_run);
            trip(why);
        } else if (g_blackout_since_us && (now - g_blackout_since_us) / 1000 > kBlackoutMs) {
            snprintf(why, sizeof(why), "map %.0f%% black during zoom (was %.0f%%) for >%d ms",
                     s.black * 100.0f, g_hot_base_black * 100.0f, kBlackoutMs);
            trip(why);
            g_blackout_since_us = 0;
        } else if (g_flip_run >= kFlipRun) {
            snprintf(why, sizeof(why), "frame flip-flop d1=%.0f d2=%.0f on %d consecutive frames",
                     d1, d2, g_flip_run);
            trip(why);
        }
    }

    if (recording && --g_record_left == 0 && g_rec) {
        fprintf(g_rec, "# end\n");
        fclose(g_rec);
        g_rec = nullptr;
    }
}

void frame_probe_record(int frames, const char* label, bool nopix) {
    g_record_nopix = nopix;
    if (g_rec) { fclose(g_rec); g_rec = nullptr; }
    g_record_left = std::max(0, frames);
    if (!g_record_left) return;
    g_rec = fopen(smoothpan_log_path("smoothpan_watch.txt").c_str(), "a");
    if (g_rec) fprintf(g_rec, "# block label=%s frames=%d\n", label ? label : "-", frames);
}

bool frame_probe_recording() { return g_record_left > 0; }
void frame_probe_test_strobe(int n) { g_test_strobe_left = n; }
void frame_probe_set_watchdog(bool on) { g_watchdog = on; }
bool frame_probe_watchdog() { return g_watchdog; }
int frame_probe_trips() { return g_trips; }

void frame_probe_status(char* buf, size_t n) {
    if (!buf || !n) return;
    snprintf(buf, n, "watch=%s trips=%d samples=%d recording=%d last(black=%.3f d1=%.1f d2=%.1f)",
             g_watchdog ? "on" : "off", g_trips, g_samples, g_record_left,
             g_last_black, g_last_d1, g_last_d2);
}

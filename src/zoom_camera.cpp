#define NOMINMAX
#include "zoom_camera.h"
#include "pacing.h"

#include "camera.h"
#include "compositor.h"
#include "viewport.h"
#include "sdl_hook.h"
#include "debug_paths.h"
#include "version.h"
#include "frame_seq.h"

#include "DataDefs.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/enabler.h"
#include "df/renderer_2d.h"
#include "df/viewscreen.h"
#include "df/interface_key.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

using namespace DFHack;

int g_zoom_inject_depth = 0;

namespace {

// Ladder of baked cell sizes (px per tile = viewport_zoom_factor / 4).
// Seeded with the verified Premium ladder; any other value vanilla lands on
// is inserted when observed.
constexpr int kMaxLadder = 32;
static int g_ladder[kMaxLadder] = { 16, 24, 32, 40, 48, 56, 64 };
static int g_ladder_n = 7;
static int g_min_idx = 0;        // usable range; shrinks when vanilla refuses a step
static int g_max_idx = 6;
static bool g_min_learnt = false;
static bool g_max_learnt = false;

static bool g_enabled = true;
// Ease rate (1/s) in log-cell space.  Was 16, which is a ~62 ms time constant
// -- about 3x faster than the "~0.2 s per step" this was designed for, so the
// glide was really a snap and the one-frame re-bake pop dominated it.  Measured
// on the selftest frame table (worst-case normalised jerk of the visual cell,
// across all four zoom steps): rate 16 -> 0.168, rate 8 -> 0.101, rate 5 ->
// 0.163.  8 wins because it is the only one that also improves the multi-notch
// step, and it keeps the magnification excursion moderate (1.58 vs 2.00 at 5).
static float g_rate = 16.0f;             // spring stiffness omega, 1/s
static bool g_anchor_cursor = true;
static bool g_commit_direct = false;     // false = inject vanilla ZOOM key; true = set_viewport_zoom_factor

static float g_v = 0.0f;                 // visual cell (px/tile), continuous
static float g_vel = 0.0f;               // d log(v)/dt of the spring, 1/s
static int g_target_idx = -1;            // ladder index the wheel is asking for
static int g_desired_z = 0;              // z we have asked vanilla to bake (0 = none)
static bool g_pending = false;           // commit requested, complete bake not yet displayed
static int g_pending_frames = 0;         // telemetry only; the timeout is wall-clock
static long long g_pending_since_us = 0; // when the current commit was requested
static long long g_inject_ready_us = 0;  // earliest time we may inject the next step

// Both of these used to be frame counts (3 frames between injections, 15
// frames to time a commit out).  Frame counts make the real interval scale
// with the frame rate: 3 frames is 60 ms at DF's default 50 fps cap but only
// 17 ms at 180 fps, so a fast multi-notch scroll fired its whole ladder of
// commits into a handful of milliseconds -- vanilla rebaked continuously and
// the ease never had time to run, which is the "completely notched and ugly"
// zoom at high frame rates.  They are wall-clock now, set to what the old
// counts gave at 50 fps.
static constexpr int kInjectIntervalMs = 60;
static constexpr int kCommitTimeoutMs = 300;
// Zoom-in magnifies the bake on screen until arrival; beyond this scale take an
// intermediate rung instead (2.05 lets any single flick of up to 2x -- e.g.
// 24 -> 48 -- commit exactly once).
static constexpr float kMaxHoldScale = 2.05f;

static long long zc_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Ease clock --------------------------------------------------------------
// The glide is driven by a FRAME-PACED clock, not the wall clock.  Every ladder
// commit makes DF rebake the map, which blocks the render thread for 55-75 ms
// (measured identical with smooth zoom off: it is DF's cost, not ours).  On the
// wall clock the next frame then shows 75 ms of ease at once -- a lurch in the
// middle of the glide.  This clock credits each present with at most kMaxFrames
// typical frame times, so across a hitch the glide pauses and then carries on
// smoothly.  Both the main-thread update and the present-time evaluation read
// it, so they never disagree about where the ease is.
static std::atomic<long long> g_ease_clock_us{0};     // advanced on present
static std::atomic<long long> g_ease_last_present_us{0};
static std::atomic<long long> g_ease_typ_frame_us{16667};
static constexpr float kMaxFrames = 2.0f;

static long long ease_cap_us() {
    const long long typ = g_ease_typ_frame_us.load(std::memory_order_relaxed);
    return static_cast<long long>(kMaxFrames * static_cast<float>(typ));
}

// Ease clock "now", callable from either thread.
static long long ease_now_us() {
    const long long last = g_ease_last_present_us.load(std::memory_order_acquire);
    if (last == 0) return zc_now_us();                 // no presents yet: wall clock
    long long since = zc_now_us() - last;
    if (since < 0) since = 0;
    const long long cap = ease_cap_us();
    if (since > cap) since = cap;
    return g_ease_clock_us.load(std::memory_order_acquire) + since;
}

static void ease_clock_on_present() {
    const long long now = zc_now_us();
    const long long last = g_ease_last_present_us.load(std::memory_order_relaxed);
    if (last == 0) {
        g_ease_clock_us.store(now, std::memory_order_release);
        g_ease_last_present_us.store(now, std::memory_order_release);
        return;
    }
    long long dt = now - last;
    if (dt < 0) dt = 0;
    // Typical frame: slow EMA over non-outlier frames.
    long long typ = g_ease_typ_frame_us.load(std::memory_order_relaxed);
    const long long clipped = dt > 3 * typ ? 3 * typ : dt;
    typ = (typ * 15 + clipped) / 16;
    if (typ < 1000) typ = 1000;
    g_ease_typ_frame_us.store(typ, std::memory_order_relaxed);
    const long long cap = ease_cap_us();
    g_ease_clock_us.fetch_add(dt > cap ? cap : dt, std::memory_order_acq_rel);
    g_ease_last_present_us.store(now, std::memory_order_release);
}

// ---- Ease: critically damped spring in log space ----------------------------
// x = log(visual cell), velocity in log units per second, stiffness omega
// (g_rate).  Replaces the exponential approach, which started every glide at
// its MAXIMUM speed (a 3-notch flick went from rest to ~116 px/frame at the far
// corner in one frame) and jumped velocity again on every extra notch.  The
// spring starts from rest, keeps velocity continuous when the target moves
// mid-glide, and has an exact closed form, so the present-time evaluation stays
// exact across any frame split.
//
// Finish: the last kTailLog of distance is covered at no less than
// kMinLogSpeed (< 0.5 px/frame of corner movement at 180 fps) so the glide lands
// exactly in finite time instead of creeping for most of a second -- zoom-in
// commits its sharp bake on arrival, so a long tail would mean a long blur.
static constexpr float kMinLogSpeed = 0.04f;
static constexpr float kTailLog = 0.004f;

static float spring_step(float v, float* vel, float target, float omega, float dt) {
    if (target <= 0.0f || dt <= 0.0f) return v;
    const float x0 = std::log(std::max(1.0f, v));
    const float xt = std::log(target);
    const float d0 = xt - x0;                    // signed distance still to go
    if (std::fabs(d0) < 1e-5f) { *vel = 0.0f; return target; }
    const float sgn = d0 > 0.0f ? 1.0f : -1.0f;
    const float c1 = x0 - xt;
    const float c2 = *vel + omega * c1;
    const float e = std::exp(-omega * dt);
    float x = xt + (c1 + c2 * dt) * e;
    float nv = (c2 - omega * (c1 + c2 * dt)) * e;
    // Never cross the target.  A critically damped spring still overshoots once
    // when it arrives with velocity to spare (target pulled closer mid-glide);
    // overshoot-and-return on screen is a direction reversal, so land instead.
    if ((x - xt) * sgn >= 0.0f) { *vel = 0.0f; return target; }
    // Finish: inside the last kTailLog, move toward the target at no less than
    // kMinLogSpeed.  Direction comes from start -> target: taking it from where
    // the spring ended (3.31.2) pushed the glide BACKWARDS after a hairline
    // overshoot.
    if (std::fabs(xt - x) < kTailLog) {
        const float need = kMinLogSpeed * dt;
        if ((x - x0) * sgn < need) {
            x = x0 + sgn * std::min(std::fabs(d0), need);
            nv = sgn * kMinLogSpeed;
        }
        if (std::fabs(xt - x) < 1e-5f) { *vel = 0.0f; return target; }
    }
    *vel = nv;
    return std::exp(x);
}
static bool g_gesture = false;           // an ease toward target is in progress
static float g_anchor_x = 0.0f, g_anchor_y = 0.0f;   // window px
static bool g_anchor_valid = false;
// The anchor the last gesture actually used, kept after the gesture ends so a
// test can ask "did the world point under the anchor stay put?".  Measuring
// drift at the viewport centre while also anchoring there is degenerate -- it
// cannot detect an anchor error at all.
static float g_last_anchor_x = 0.0f, g_last_anchor_y = 0.0f;
static bool g_last_anchor_valid = false;
// Test-only cursor override.  >= 0 means on_zoom_key uses this instead of the
// real SDL mouse, so the selftest can exercise the true cursor-anchored path
// (over_map = true) rather than falling back to the centre.
static int g_test_cursor_x = -1, g_test_cursor_y = -1;
static double g_anchor_world_x = 0.0, g_anchor_world_y = 0.0;
static bool g_anchor_world_valid = false;

static int g_last_gps_z = 0;
static int g_display_cell = 0;           // cell of the bake on screen (last composite)
static float g_display_scale = 1.0f;
static float g_display_ax = 0.0f, g_display_ay = 0.0f;
static float g_frame_v = 0.0f;
static float g_frame_ax = 0.0f, g_frame_ay = 0.0f;
static bool g_frozen_this_frame = false;   // freeze() ran since the last present
// Ease state as of the last freeze(), so the visual cell can be evaluated at
// PRESENT time.  DF presents more often than it runs the dwarfmode render()
// interpose (where update/freeze live) once the graphics cap exceeds the main
// loop rate: at 180 fps, roughly one present in three had no fresh freeze.
// Those frames used to composite at scale 1.0 -- the raw bake, a full zoom
// level away from the frames either side -- which is the high-fps "shimmer
// between two frames".  And even on frozen frames v only advanced at the
// interpose rate, so the glide moved in uneven steps.  The ease is an
// exponential in log space, which composes exactly, so evaluating it at present
// time from the last update's state puts every frame on the true curve.
static long long g_freeze_us = 0;
static long long g_freeze_ease_us = 0;
static float g_freeze_vel = 0.0f;
static bool g_freeze_hold = false;       // zoom-out commit in flight: glide held
static bool g_hold = false;              // set by update, read by freeze
static float g_freeze_target = 0.0f;
static bool g_freeze_gesture = false;
static float g_present_v = 0.0f;
static bool g_present_v_valid = false;
// No dwarfmode render() for this long: another screen is over the map, do not
// apply a stale ease (the original reason for the scale-1.0 fallback).
static constexpr int kFreezeStaleMs = 150;

static int g_test_step = 0;
static int g_commits = 0, g_landed = 0, g_timeouts = 0, g_external = 0, g_keys = 0;
static char g_last_event[96] = "";

static long long g_last_ease_us = 0;       // ease clock at the last update
static long long g_gesture_start_ease_us = 0;   // ease clock when the current gesture began
static bool g_have_time = false;

static FILE* g_log = nullptr;
static int g_log_lines = 0;
constexpr int kMaxLogLines = 4000;

static void zlog(const char* fmt, ...) {
    if (g_log_lines >= kMaxLogLines) return;
    if (!g_log) {
        g_log = fopen(smoothpan_log_path("smoothpan_zoom.txt").c_str(), "a");
        if (!g_log) return;
        fprintf(g_log, "# SmoothPan %s zoom log\n", SMOOTHPAN_BUILD_VERSION);
    }
    int gps_z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
    fprintf(g_log, "frame=%d gps=%d v=%.2f tgt=%d desired=%d pend=%d/%d disp=%d ",
            frame_seq_frame_index(), gps_z, g_v,
            (g_target_idx >= 0 && g_target_idx < g_ladder_n) ? g_ladder[g_target_idx] : -1,
            g_desired_z, g_pending ? 1 : 0, g_pending_frames, g_display_cell);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    g_log_lines++;
}

static void note(const char* text) {
    snprintf(g_last_event, sizeof(g_last_event), "%s", text);
    zlog("%s", text);
}

static void ladder_insert(int cell) {
    if (cell <= 0) return;
    for (int i = 0; i < g_ladder_n; ++i)
        if (g_ladder[i] == cell) return;
    if (g_ladder_n >= kMaxLadder) return;
    int pos = g_ladder_n;
    while (pos > 0 && g_ladder[pos - 1] > cell) { g_ladder[pos] = g_ladder[pos - 1]; --pos; }
    g_ladder[pos] = cell;
    g_ladder_n++;
    if (g_target_idx >= 0 && g_target_idx >= pos) g_target_idx++;
    // Keep the usable range covering the whole ladder unless a limit was learnt.
    if (g_max_learnt) { if (pos <= g_max_idx) g_max_idx++; } else g_max_idx = g_ladder_n - 1;
    if (g_min_learnt) { if (pos <= g_min_idx) g_min_idx++; } else g_min_idx = 0;
    zlog("ladder insert cell=%d n=%d", cell, g_ladder_n);
}

static int ladder_nearest_index(float cell) {
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < g_ladder_n; ++i) {
        float d = std::fabs(static_cast<float>(g_ladder[i]) - cell);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Largest ladder cell <= v (never below g_min_idx).
static int ladder_floor_index(float v) {
    int idx = g_min_idx;
    for (int i = 0; i < g_ladder_n; ++i)
        if (static_cast<float>(g_ladder[i]) <= v + 1e-3f) idx = i;
    if (idx < g_min_idx) idx = g_min_idx;
    if (idx > g_max_idx) idx = g_max_idx;
    return idx;
}

static int clamp_idx(int i) {
    if (i < g_min_idx) i = g_min_idx;
    if (i > g_max_idx) i = g_max_idx;
    if (i < 0) i = 0;
    if (i >= g_ladder_n) i = g_ladder_n - 1;
    return i;
}

static bool grid_origin(float* ox, float* oy) {
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    *ox = static_cast<float>(vp.origin_x);
    *oy = static_cast<float>(vp.origin_y);
    return true;
}

static void viewport_center(float* cx, float* cy) {
    ViewportRect vp;
    if (get_strict_viewport_rect(&vp)) {
        *cx = 0.5f * static_cast<float>(vp.left + vp.right);
        *cy = 0.5f * static_cast<float>(vp.top + vp.bottom);
    } else {
        *cx = 0.0f;
        *cy = 0.0f;
    }
}

static void record_anchor_world(int cb) {
    float ox = 0.0f, oy = 0.0f;
    if (cb <= 0 || !grid_origin(&ox, &oy)) { g_anchor_world_valid = false; return; }
    float ax = g_anchor_x, ay = g_anchor_y;
    if (!g_anchor_valid) viewport_center(&ax, &ay);
    // world(p) = true + (p - origin) / cell   (see camera.cpp: tile i is drawn
    // at origin + i*cell - frac*cell, so the origin pixel shows window+frac).
    g_anchor_world_x = g_camera.true_x + static_cast<double>(ax - ox) / cb;
    g_anchor_world_y = g_camera.true_y + static_cast<double>(ay - oy) / cb;
    g_anchor_world_valid = true;
}

// After a commit lands at cb_new: move the camera so the world point that was
// under the anchor is still under it.  Makes zoom cursor-directed and keeps the
// image continuous across the bake swap (scale/anchor math in compositor).
static void apply_anchor_invariance(int cb_new) {
    float ox = 0.0f, oy = 0.0f;
    if (cb_new <= 0 || !g_anchor_world_valid || !grid_origin(&ox, &oy)) return;
    float ax = g_anchor_x, ay = g_anchor_y;
    if (!g_anchor_valid) viewport_center(&ax, &ay);
    g_camera.true_x = g_anchor_world_x - static_cast<double>(ax - ox) / cb_new;
    g_camera.true_y = g_anchor_world_y - static_cast<double>(ay - oy) / cb_new;
    g_camera.commit_true_position();
}

static void external_resync(int cb, const char* why) {
    g_v = static_cast<float>(cb);
    g_vel = 0.0f;
    g_target_idx = ladder_nearest_index(static_cast<float>(cb));
    g_gesture = false;
    g_pending = false;
    g_pending_frames = 0;
    g_pending_since_us = 0;
    g_inject_ready_us = 0;
    g_desired_z = 0;
    g_anchor_valid = false;
    g_anchor_world_valid = false;
    g_display_cell = cb;
    g_display_scale = 1.0f;
    note(why);
}

static void inject_step(df::viewscreen* vs, int dir, int desired_z) {
    g_commits++;
    if (g_commit_direct) {
        auto* r2d = virtual_cast<df::renderer_2d>(
            df::global::enabler ? df::global::enabler->renderer : nullptr);
        if (r2d) r2d->set_viewport_zoom_factor(desired_z);
        zlog("inject direct set_viewport_zoom_factor(%d)", desired_z);
        return;
    }
    if (!vs) return;
    std::set<df::interface_key> keys;
    keys.insert(dir > 0 ? df::interface_key::ZOOM_IN : df::interface_key::ZOOM_OUT);
    g_zoom_inject_depth++;
    vs->feed(&keys);
    g_zoom_inject_depth--;
    zlog("inject feed %s (toward z=%d)", dir > 0 ? "ZOOM_IN" : "ZOOM_OUT", desired_z);
}

}  // namespace

// ---- public ---------------------------------------------------------------

void zoom_camera_reset() {
    g_v = 0.0f;
    g_vel = 0.0f;
    g_target_idx = -1;
    g_desired_z = 0;
    g_pending = false;
    g_pending_frames = 0;
    g_pending_since_us = 0;
    g_inject_ready_us = 0;
    g_gesture = false;
    g_anchor_valid = false;
    g_anchor_world_valid = false;
    g_last_gps_z = 0;
    g_display_cell = 0;
    g_display_scale = 1.0f;
    g_frame_v = 0.0f;
    g_freeze_us = 0;
    g_freeze_gesture = false;
    g_present_v_valid = false;
    g_test_step = 0;
    g_have_time = false;
    g_min_idx = 0;
    g_max_idx = g_ladder_n - 1;
    g_min_learnt = false;
    g_max_learnt = false;
    g_last_event[0] = '\0';
}

void zoom_camera_set_enabled(bool enabled) { g_enabled = enabled; }
bool zoom_camera_enabled() { return g_enabled; }
void zoom_camera_set_test_cursor(int x, int y) {
    g_test_cursor_x = x;
    g_test_cursor_y = y;
    // Seed the "last anchor" too, so measurements taken BEFORE the first
    // gesture (the idle steps) use the same anchor the zoom steps will.
    // Otherwise they fall back to the viewport centre and the first zoom step
    // shows a huge bogus drift that is only the change of reference point.
    if (x >= 0) {
        g_last_anchor_x = static_cast<float>(x);
        g_last_anchor_y = static_cast<float>(y);
        g_last_anchor_valid = true;
    }
}

void zoom_camera_log_wheel(int dir, int x, int y, int gate, bool gate_over_map, bool over_map, bool taken) {
    zlog("wheel dir=%+d at=(%d,%d) gate=%d gate_map=%d over_map=%d taken=%d%s",
         dir, x, y, gate, gate_over_map ? 1 : 0, over_map ? 1 : 0, taken ? 1 : 0,
         taken ? "" : (over_map ? " (on_zoom_key refused)" : " (-> vanilla)"));
}

bool zoom_camera_test_cursor(int* x, int* y) {
    if (g_test_cursor_x < 0) return false;
    if (x) *x = g_test_cursor_x;
    if (y) *y = g_test_cursor_y;
    return true;
}

bool zoom_camera_last_anchor_px(float* x, float* y) {
    if (!g_last_anchor_valid) return false;
    if (x) *x = g_last_anchor_x;
    if (y) *y = g_last_anchor_y;
    return true;
}

void zoom_camera_set_rate(float per_second) { g_rate = std::max(2.0f, std::min(60.0f, per_second)); }
float zoom_camera_rate() { return g_rate; }
void zoom_camera_set_anchor_cursor(bool cursor) { g_anchor_cursor = cursor; }
bool zoom_camera_anchor_cursor() { return g_anchor_cursor; }
void zoom_camera_set_commit_direct(bool direct) { g_commit_direct = direct; }
bool zoom_camera_commit_direct() { return g_commit_direct; }
void zoom_camera_queue_test_step(int dir) { g_test_step = dir; }
bool zoom_camera_in_gesture() { return g_gesture || g_pending; }
float zoom_camera_visual_cell() { return g_frame_v; }

bool zoom_camera_on_zoom_key(int dir, bool over_map) {
    if (!g_enabled || !compositor_active() || g_last_gps_z == 0 || dir == 0) return false;
    g_keys++;
    const int cb = g_last_gps_z / 4;
    if (!g_gesture) {
        g_gesture = true;
        g_gesture_start_ease_us = ease_now_us();
        if (g_v <= 0.0f) g_v = static_cast<float>(cb);
        if (g_target_idx < 0) g_target_idx = ladder_nearest_index(static_cast<float>(cb));
        // Anchor for the whole gesture: cursor on map, else viewport centre.
        ViewportRect vp;
        int rx = -1, ry = -1;
        bool ok = get_strict_viewport_rect(&vp);
        bool have_mouse;
        if (g_test_cursor_x >= 0) {
            rx = g_test_cursor_x;
            ry = g_test_cursor_y;
            have_mouse = true;
        } else {
            have_mouse = smoothpan_raw_sdl_mouse(&rx, &ry);
        }
        if (g_anchor_cursor && over_map && ok && have_mouse &&
            rx >= vp.left && rx < vp.right && ry >= vp.top && ry < vp.bottom) {
            g_anchor_x = static_cast<float>(rx);
            g_anchor_y = static_cast<float>(ry);
        } else {
            viewport_center(&g_anchor_x, &g_anchor_y);
        }
        g_anchor_valid = true;
        g_last_anchor_x = g_anchor_x;
        g_last_anchor_y = g_anchor_y;
        g_last_anchor_valid = true;
        // Pin the world point under the anchor ONCE, here, for the whole
        // gesture.  It used to be (re-)recorded at the start of every commit,
        // so a multi-notch gesture re-derived it 2-3 times: any residual error
        // left by the previous commit was re-recorded as the new truth and the
        // next commit preserved *that*, compounding instead of correcting.
        // One recording per gesture means every commit restores the same point.
        record_anchor_world(cb);
        zlog("gesture start anchor=(%.0f,%.0f) over_map=%d world=(%.2f,%.2f)",
             g_anchor_x, g_anchor_y, over_map ? 1 : 0, g_anchor_world_x, g_anchor_world_y);
    }
    int ni = clamp_idx(g_target_idx + dir);
    if (ni != g_target_idx) {
        g_target_idx = ni;
        zlog("notch dir=%d target=%d", dir, g_ladder[g_target_idx]);
    }
    return true;
}

void zoom_camera_update(df::viewscreen* vs) {
    // Ease time, not wall time (see g_ease_clock_us).
    const long long enow = ease_now_us();
    // A gesture that began after the last update only gets the time since it
    // began.  Integrating from the previous update credited the first visible
    // step with time from before the wheel notch: one static frame, then a
    // double step -- a small jolt at the start of every zoom.
    long long from = g_last_ease_us;
    if (g_gesture && g_gesture_start_ease_us > from) from = g_gesture_start_ease_us;
    float dt = 1.0f / 60.0f;
    if (g_have_time) dt = static_cast<float>(enow - from) / 1e6f;
    g_last_ease_us = enow;
    g_have_time = true;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;

    if (!df::global::gps) return;
    const int gps_z = df::global::gps->viewport_zoom_factor;
    const int cb = gps_z / 4;
    if (cb <= 0) return;
    ladder_insert(cb);

    if (g_last_gps_z == 0) {
        g_last_gps_z = gps_z;
        g_v = static_cast<float>(cb);
        g_target_idx = ladder_nearest_index(static_cast<float>(cb));
        g_display_cell = cb;
    }

    // Console test step: one vanilla ladder step through the commit path.
    if (g_test_step != 0) {
        int dir = g_test_step;
        g_test_step = 0;
        int idx = clamp_idx(ladder_nearest_index(static_cast<float>(cb)) + dir);
        int z = g_ladder[idx] * 4;
        if (z != gps_z) {
            inject_step(vs, dir, z);
            note("test step injected");
        }
    }

    if (gps_z != g_last_gps_z) {
        if (g_pending && g_anchor_world_valid) {
            apply_anchor_invariance(cb);
            g_landed++;
            g_pending_since_us = zc_now_us();    // progress: restart the timeout
            zlog("commit landed z %d -> %d (anchor kept)", g_last_gps_z, gps_z);
        } else {
            // Someone else moved the zoom: nothing of ours to pace.
            g_inject_ready_us = 0;
            g_external++;
            external_resync(cb, "external zoom change: resync");
        }
        g_last_gps_z = gps_z;
    } else if (g_pending && g_anchor_world_valid && g_camera.external_window_move) {
        // Vanilla recentred window_x/y as part of its rebake (after we had
        // already placed the camera): put the world point back under the anchor.
        apply_anchor_invariance(cb);
        zlog("vanilla moved window during pending commit: anchor re-applied");
    }

    const bool active = g_enabled && compositor_active();
    if (!active) {
        g_v = static_cast<float>(cb);
        g_target_idx = ladder_nearest_index(static_cast<float>(cb));
        g_gesture = false;
        g_pending = false;
        g_desired_z = 0;
        g_anchor_valid = false;
        g_anchor_world_valid = false;
        g_display_cell = cb;
        g_display_scale = 1.0f;
        return;
    }

    if (g_target_idx < 0 || g_target_idx >= g_ladder_n)
        g_target_idx = ladder_nearest_index(static_cast<float>(cb));
    const float target_cell = static_cast<float>(g_ladder[g_target_idx]);

    // Ease the visual cell toward the target in log space (RimWorld-style
    // exponential approach), snapping the asymptotic tail.
    // Zooming out, the glide waits at its starting size until every rung of the
    // commit has landed.  DF accepts one zoom step per frame and rebakes (a
    // 30-75 ms stall) for each, so a 3-notch flick is ~170 ms of stalls; gliding
    // between them stuttered.  Waiting reads as a short latency, then a clean
    // glide.
    const bool out_commit_pending = g_pending && g_desired_z > 0 && g_desired_z < gps_z;
    g_hold = out_commit_pending;
    if (g_gesture && !out_commit_pending) g_v = spring_step(g_v, &g_vel, target_cell, g_rate, dt);
    else if (out_commit_pending) g_vel = 0.0f;

    // Scale must stay >= 1 on whatever bake is on screen: while a commit is
    // pending that is the last displayed bake (possibly the retained one).
    const float floor_cell = static_cast<float>((g_pending && g_display_cell > 0) ? g_display_cell : cb);
    if (g_v < floor_cell) {
        g_v = floor_cell;
        if (g_vel < 0.0f) g_vel = 0.0f;       // held by the bake on screen: stop pushing into it
    }

    // The bake we need.  Every commit makes DF rebake the map, a 55-75 ms stall
    // of the render thread (DF's own cost, identical with smooth zoom off), so
    // the fewer commits land mid-glide the better.
    //  - Zooming OUT: the smaller-cell bake has to be on screen before the glide
    //    can shrink past the current one (scale must stay >= 1 to cover the
    //    viewport), so commit the target rung straight away; the floor clamp
    //    holds the glide at the old size until it lands.
    //  - Zooming IN: keep magnifying the bake already on screen and commit the
    //    target once, on arrival, when the map is at rest.  Committing at every
    //    rung crossed (as before) put a rebake stall in the middle of every
    //    multi-notch glide.  Magnification is capped at kMaxHoldScale: past it
    //    take the rung at-or-below v now rather than stretch pixels further.
    int didx;
    const float cbf = static_cast<float>(cb);
    if (target_cell > cbf + 0.5f) {
        if (g_v >= target_cell) didx = g_target_idx;
        else if (g_v > cbf * kMaxHoldScale) didx = ladder_floor_index(g_v);
        else didx = ladder_nearest_index(cbf);
    } else {
        didx = ladder_floor_index(std::min(g_v, target_cell));
    }
    const int desired_z = g_ladder[didx] * 4;

    if (g_pending && desired_z == gps_z && g_desired_z != gps_z) {
        // The rung we want now is the one already on screen (the zoom-in hold
        // rule stopped asking for the intermediate rung once the first step of
        // it landed, or the wheel moved the target).  Retire the stale desire so
        // the commit completes; leaving it made the timeout below fire 300 ms
        // later and "learn" the current rung as DF's zoom limit, capping every
        // later zoom-in there (seen at cell 24 in 3.31.4).
        g_desired_z = desired_z;
    }

    if (g_gesture && desired_z != gps_z) {
        if (!g_pending) {
            // Only if the gesture did not already pin one (see on_zoom_key).
            // Re-recording mid-gesture is what made multi-notch zoom drift.
            if (!g_anchor_world_valid) record_anchor_world(cb);
            g_pending = true;
            g_pending_frames = 0;
            g_pending_since_us = zc_now_us();
        }
        g_desired_z = desired_z;
        // Several rungs are injected back to back in this one update when vanilla
        // lands them synchronously, so a multi-rung commit costs DF one rebake
        // at the next render rather than one rebake per rung per frame.
        for (int rung = 0; rung < kMaxLadder && zc_now_us() >= g_inject_ready_us; ++rung) {
            const int from_z = df::global::gps->viewport_zoom_factor;
            if (from_z == desired_z) break;
            inject_step(vs, desired_z > from_z ? +1 : -1, desired_z);
            g_inject_ready_us = zc_now_us() + kInjectIntervalMs * 1000LL;
            g_pending_since_us = zc_now_us();          // timeout runs from the last attempt
            // Vanilla applies the step synchronously inside feed: place the
            // camera now so even this frame's bake already honours the anchor.
            const int now_z = df::global::gps->viewport_zoom_factor;
            if (now_z == from_z || now_z <= 0) break;   // not synchronous: wait for it
            {
                ladder_insert(now_z / 4);
                apply_anchor_invariance(now_z / 4);
                g_last_gps_z = now_z;
                g_landed++;
                g_pending_since_us = zc_now_us();    // progress: restart the timeout
                // Clear the gate: vanilla landed this one synchronously, so the
                // next ladder step may follow immediately.  Holding the gate
                // here (tried in 3.25.3) spaces commits by kInjectIntervalMs,
                // which lets the ease stretch the *current* bake up to 2x
                // before the new one lands -- a soft, magnified map that snaps
                // crisp on the commit.  That reads as judder, so commit promptly
                // and keep the composite scale near 1.
                g_inject_ready_us = 0;
                zlog("commit landed synchronously z %d -> %d (anchor kept)", from_z, now_z);
            }
        }
    }

    if (g_pending) {
        g_pending_frames++;
        const long long elapsed_ms =
            g_pending_since_us ? (zc_now_us() - g_pending_since_us) / 1000 : 0;
        // Only an injected step that vanilla never delivered is a limit.
        if (elapsed_ms > kCommitTimeoutMs && g_desired_z != gps_z) {
            // Vanilla did not deliver: learn the limit and fall back to the bake we have.
            if (g_desired_z > gps_z) { g_max_idx = ladder_nearest_index(static_cast<float>(cb)); g_max_learnt = true; }
            else if (g_desired_z < gps_z) { g_min_idx = ladder_nearest_index(static_cast<float>(cb)); g_min_learnt = true; }
            g_timeouts++;
            external_resync(cb, "commit timeout: ladder limit learnt, resync");
        }
    }

    if (g_gesture && !g_pending && gps_z == static_cast<int>(target_cell) * 4 &&
        std::fabs(g_v - target_cell) < 0.01f) {
        g_v = target_cell;
        g_vel = 0.0f;
        g_gesture = false;
        g_anchor_valid = false;
        g_anchor_world_valid = false;
        zlog("gesture end at cell=%d", static_cast<int>(target_cell));
    }
}

void zoom_camera_on_present() {
    g_frozen_this_frame = false;
    g_present_v_valid = false;
    ease_clock_on_present();
}

void zoom_camera_freeze() {
    g_frozen_this_frame = true;
    g_frame_v = g_v;
    g_freeze_us = zc_now_us();
    g_freeze_ease_us = ease_now_us();
    g_freeze_gesture = g_gesture;
    g_freeze_vel = g_vel;
    g_freeze_hold = g_hold;
    g_freeze_target = (g_target_idx >= 0 && g_target_idx < g_ladder_n)
                          ? static_cast<float>(g_ladder[g_target_idx]) : g_v;
    if (g_anchor_valid) {
        g_frame_ax = g_anchor_x;
        g_frame_ay = g_anchor_y;
    } else {
        viewport_center(&g_frame_ax, &g_frame_ay);
    }
}

// Visual cell for the frame being presented now (see g_freeze_us).
static float present_v() {
    if (g_present_v_valid) return g_present_v;
    float v = g_frame_v;
    // Held (zoom-out rungs still landing): show exactly the held size.  Extrapolating
    // here let the displayed size creep toward the target between rungs and snap
    // back when each one landed -- shimmer at the start of a zoom-out flick.
    if (g_freeze_gesture && !g_freeze_hold && g_freeze_us && v > 0.0f && g_freeze_target > 0.0f) {
        // Evaluate for when this frame will be SEEN (pacing lead).
        float dt = static_cast<float>(ease_now_us() + pacing_frame_lead_us() - g_freeze_ease_us) / 1e6f;
        if (dt > 0.05f) dt = 0.05f;                 // same cap as zoom_camera_update
        float vel = g_freeze_vel;
        v = spring_step(v, &vel, g_freeze_target, g_rate, dt);
    }
    g_present_v = v;
    g_present_v_valid = true;
    return v;
}

float zoom_camera_scale_for_cell(int cell) {
    // No dwarfmode render() for a while: another screen is over the map, so
    // never apply a stale ease.  A present without a fresh freeze is otherwise
    // normal at high frame rates and must keep the ease (see g_freeze_us).
    if (!g_freeze_us || (zc_now_us() - g_freeze_us) / 1000 > kFreezeStaleMs) return 1.0f;
    if (cell <= 0) return 1.0f;
    const float v = present_v();
    if (v <= 0.0f) return 1.0f;
    const float s = v / static_cast<float>(cell);
    return s < 1.0f ? 1.0f : s;
}

void zoom_camera_frame_anchor(float* ax, float* ay) {
    if (ax) *ax = g_frame_ax;
    if (ay) *ay = g_frame_ay;
}

void zoom_camera_on_displayed(int cell, bool is_current_bake, int gps_z_of_current) {
    if (cell > 0) g_display_cell = cell;
    g_display_scale = zoom_camera_scale_for_cell(cell);
    g_display_ax = g_frame_ax;
    g_display_ay = g_frame_ay;
    if (g_pending && is_current_bake && gps_z_of_current == g_desired_z &&
        gps_z_of_current == g_last_gps_z) {
        g_pending = false;
        g_pending_frames = 0;
        zlog("commit complete: bake z=%d on screen", gps_z_of_current);
    }
}

bool zoom_camera_display_transform(float* s, float* ax, float* ay) {
    if (g_display_scale <= 1.0001f) return false;
    if (s) *s = g_display_scale;
    if (ax) *ax = g_display_ax;
    if (ay) *ay = g_display_ay;
    return true;
}

void zoom_camera_info(ZoomCameraInfo* o) {
    if (!o) return;
    o->enabled = g_enabled;
    o->gesture = g_gesture;
    o->pending = g_pending;
    o->v = g_v;
    o->target_cell = (g_target_idx >= 0 && g_target_idx < g_ladder_n) ? g_ladder[g_target_idx] : 0;
    o->baked_cell = g_last_gps_z / 4;
    o->desired_z = g_desired_z;
    o->display_cell = g_display_cell;
    o->display_scale = g_display_scale;
    o->keys = g_keys;
    o->commits = g_commits;
    o->landed = g_landed;
    o->external = g_external;
    o->timeouts = g_timeouts;
}

void zoom_camera_write_f9(FILE* f) {
    if (!f) return;
    fprintf(f, "  Zoom: enabled=%d gesture=%d v=%.2f target=%d cb=%d desired=%d pending=%d(%d) wait_ms=%d "
               "anchor=(%.0f,%.0f)%s display cell=%d s=%.3f keys=%d commits=%d landed=%d ext=%d timeouts=%d "
               "range=[%d..%d] last=%s\n",
            g_enabled ? 1 : 0, g_gesture ? 1 : 0, g_v,
            (g_target_idx >= 0 && g_target_idx < g_ladder_n) ? g_ladder[g_target_idx] : -1,
            g_last_gps_z / 4, g_desired_z, g_pending ? 1 : 0, g_pending_frames,
            static_cast<int>(std::max(0LL, (g_inject_ready_us - zc_now_us()) / 1000)),
            g_frame_ax, g_frame_ay, g_anchor_valid ? "" : "(centre)",
            g_display_cell, g_display_scale, g_keys, g_commits, g_landed, g_external, g_timeouts,
            g_ladder[g_min_idx], g_ladder[g_max_idx], g_last_event);
}

void zoom_camera_status(char* buf, size_t n) {
    if (!buf || n == 0) return;
    snprintf(buf, n, "zoom=%s v=%.1f cb=%d target=%d pending=%d rate=%.0f anchor=%s commit=%s keys=%d commits=%d landed=%d ext=%d timeouts=%d",
             g_enabled ? "on" : "off", g_v, g_last_gps_z / 4,
             (g_target_idx >= 0 && g_target_idx < g_ladder_n) ? g_ladder[g_target_idx] : -1,
             g_pending ? 1 : 0, g_rate, g_anchor_cursor ? "cursor" : "centre",
             g_commit_direct ? "direct" : "feed",
             g_keys, g_commits, g_landed, g_external, g_timeouts);
}

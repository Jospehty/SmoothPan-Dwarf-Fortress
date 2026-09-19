#define NOMINMAX
#include "zoom_camera.h"

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

#include <windows.h>
#undef min
#undef max
#include <algorithm>
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
static float g_rate = 16.0f;             // ease rate (1/s) in log-cell space
static bool g_anchor_cursor = true;
static bool g_commit_direct = false;     // false = inject vanilla ZOOM key; true = set_viewport_zoom_factor

static float g_v = 0.0f;                 // visual cell (px/tile), continuous
static int g_target_idx = -1;            // ladder index the wheel is asking for
static int g_desired_z = 0;              // z we have asked vanilla to bake (0 = none)
static bool g_pending = false;           // commit requested, complete bake not yet displayed
static int g_pending_frames = 0;
static int g_inject_wait = 0;            // frames to wait before re-injecting a step
static bool g_gesture = false;           // an ease toward target is in progress
static float g_anchor_x = 0.0f, g_anchor_y = 0.0f;   // window px
static bool g_anchor_valid = false;
static double g_anchor_world_x = 0.0, g_anchor_world_y = 0.0;
static bool g_anchor_world_valid = false;

static int g_last_gps_z = 0;
static int g_display_cell = 0;           // cell of the bake on screen (last composite)
static float g_display_scale = 1.0f;
static float g_display_ax = 0.0f, g_display_ay = 0.0f;
static float g_frame_v = 0.0f;
static float g_frame_ax = 0.0f, g_frame_ay = 0.0f;
static bool g_frozen_this_frame = false;   // freeze() ran since the last present

static int g_test_step = 0;
static int g_commits = 0, g_landed = 0, g_timeouts = 0, g_external = 0, g_keys = 0;
static char g_last_event[96] = "";

static std::chrono::steady_clock::time_point g_last_time;
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
    g_target_idx = ladder_nearest_index(static_cast<float>(cb));
    g_gesture = false;
    g_pending = false;
    g_pending_frames = 0;
    g_inject_wait = 0;
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
    g_target_idx = -1;
    g_desired_z = 0;
    g_pending = false;
    g_pending_frames = 0;
    g_inject_wait = 0;
    g_gesture = false;
    g_anchor_valid = false;
    g_anchor_world_valid = false;
    g_last_gps_z = 0;
    g_display_cell = 0;
    g_display_scale = 1.0f;
    g_frame_v = 0.0f;
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
        if (g_v <= 0.0f) g_v = static_cast<float>(cb);
        if (g_target_idx < 0) g_target_idx = ladder_nearest_index(static_cast<float>(cb));
        // Anchor for the whole gesture: cursor on map, else viewport centre.
        ViewportRect vp;
        int rx = -1, ry = -1;
        bool ok = get_strict_viewport_rect(&vp);
        if (g_anchor_cursor && over_map && ok && smoothpan_raw_sdl_mouse(&rx, &ry) &&
            rx >= vp.left && rx < vp.right && ry >= vp.top && ry < vp.bottom) {
            g_anchor_x = static_cast<float>(rx);
            g_anchor_y = static_cast<float>(ry);
        } else {
            viewport_center(&g_anchor_x, &g_anchor_y);
        }
        g_anchor_valid = true;
        zlog("gesture start anchor=(%.0f,%.0f) over_map=%d", g_anchor_x, g_anchor_y, over_map ? 1 : 0);
    }
    int ni = clamp_idx(g_target_idx + dir);
    if (ni != g_target_idx) {
        g_target_idx = ni;
        zlog("notch dir=%d target=%d", dir, g_ladder[g_target_idx]);
    }
    return true;
}

void zoom_camera_update(df::viewscreen* vs) {
    auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (g_have_time) dt = std::chrono::duration<float>(now - g_last_time).count();
    g_last_time = now;
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
        g_inject_wait = 0;
        if (g_pending && g_anchor_world_valid) {
            apply_anchor_invariance(cb);
            g_landed++;
            zlog("commit landed z %d -> %d (anchor kept)", g_last_gps_z, gps_z);
        } else {
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
    if (g_gesture) {
        const float k = 1.0f - std::exp(-g_rate * dt);
        float lv = std::log(std::max(1.0f, g_v));
        const float lt = std::log(target_cell);
        lv += (lt - lv) * k;
        g_v = std::exp(lv);
        // A quarter-pixel of tile size is invisible: snap the asymptotic tail.
        if (std::fabs(g_v - target_cell) <= 0.25f) g_v = target_cell;
    }

    // Scale must stay >= 1 on whatever bake is on screen: while a commit is
    // pending that is the last displayed bake (possibly the retained one).
    const float floor_cell = static_cast<float>((g_pending && g_display_cell > 0) ? g_display_cell : cb);
    if (g_v < floor_cell) g_v = floor_cell;

    // The bake we need: ladder step at-or-below min(v, target).
    const int didx = ladder_floor_index(std::min(g_v, target_cell));
    const int desired_z = g_ladder[didx] * 4;

    if (g_gesture && desired_z != gps_z) {
        if (!g_pending) {
            record_anchor_world(cb);
            g_pending = true;
            g_pending_frames = 0;
        }
        g_desired_z = desired_z;
        if (g_inject_wait == 0) {
            inject_step(vs, desired_z > gps_z ? +1 : -1, desired_z);
            g_inject_wait = 3;
            // Vanilla applies the step synchronously inside feed: place the
            // camera now so even this frame's bake already honours the anchor.
            const int now_z = df::global::gps->viewport_zoom_factor;
            if (now_z != gps_z && now_z > 0) {
                ladder_insert(now_z / 4);
                apply_anchor_invariance(now_z / 4);
                g_last_gps_z = now_z;
                g_landed++;
                g_inject_wait = 0;
                zlog("commit landed synchronously z %d -> %d (anchor kept)", gps_z, now_z);
            }
        }
    }

    if (g_pending) {
        g_pending_frames++;
        if (g_pending_frames > 15) {
            // Vanilla did not deliver: learn the limit and fall back to the bake we have.
            if (g_desired_z > gps_z) { g_max_idx = ladder_nearest_index(static_cast<float>(cb)); g_max_learnt = true; }
            else if (g_desired_z < gps_z) { g_min_idx = ladder_nearest_index(static_cast<float>(cb)); g_min_learnt = true; }
            g_timeouts++;
            external_resync(cb, "commit timeout: ladder limit learnt, resync");
        }
    }
    if (g_inject_wait > 0) g_inject_wait--;

    if (g_gesture && !g_pending && gps_z == static_cast<int>(target_cell) * 4 &&
        std::fabs(g_v - target_cell) < 0.01f) {
        g_v = target_cell;
        g_gesture = false;
        g_anchor_valid = false;
        g_anchor_world_valid = false;
        zlog("gesture end at cell=%d", static_cast<int>(target_cell));
    }
}

void zoom_camera_on_present() {
    g_frozen_this_frame = false;
}

void zoom_camera_freeze() {
    g_frozen_this_frame = true;
    g_frame_v = g_v;
    if (g_anchor_valid) {
        g_frame_ax = g_anchor_x;
        g_frame_ay = g_anchor_y;
    } else {
        viewport_center(&g_frame_ax, &g_frame_ay);
    }
}

float zoom_camera_scale_for_cell(int cell) {
    // No dwarfmode render hook this frame (another screen over the map):
    // never apply a stale ease.
    if (!g_frozen_this_frame) return 1.0f;
    if (cell <= 0 || g_frame_v <= 0.0f) return 1.0f;
    float s = g_frame_v / static_cast<float>(cell);
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

void zoom_camera_write_f9(FILE* f) {
    if (!f) return;
    fprintf(f, "  Zoom: enabled=%d gesture=%d v=%.2f target=%d cb=%d desired=%d pending=%d(%d) wait=%d "
               "anchor=(%.0f,%.0f)%s display cell=%d s=%.3f keys=%d commits=%d landed=%d ext=%d timeouts=%d "
               "range=[%d..%d] last=%s\n",
            g_enabled ? 1 : 0, g_gesture ? 1 : 0, g_v,
            (g_target_idx >= 0 && g_target_idx < g_ladder_n) ? g_ladder[g_target_idx] : -1,
            g_last_gps_z / 4, g_desired_z, g_pending ? 1 : 0, g_pending_frames, g_inject_wait,
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

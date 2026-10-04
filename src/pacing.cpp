#include "pacing.h"

#include "camera.h"
#include "compositor.h"
#include "debug_paths.h"
#include "platform.h"

#include "df/global_objects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <time.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#endif

namespace {

long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------- settings
std::atomic<int> g_mode{-1};             // -1 auto, 0 off, N = every Nth refresh
// Present this long before the target refresh.  Measured (180 Hz, KWin,
// XWayland, joined against the compositor's own presentation timestamps):
// 2.0 ms -> 7/242 shown a refresh late; 2.5-3.5 ms -> 3-6/240, those tracking
// DF's own slow frames; 3.5-4.0 ms -> frames start being taken by the
// PREVIOUS refresh (9-12 early).  2.5 ms keeps clear of that edge.
std::atomic<int> g_latch_us{2500};
constexpr int kSafetyUs = 600;           // extra slack when choosing the target refresh

// ---------------------------------------------------------------- grid
std::mutex g_grid_mx;
double g_period_us = 0.0;                // refresh period
long long g_boundary_us = 0;             // one refresh time on the grid
long long g_grid_updated_us = 0;
int g_grid_samples = 0;
const char* g_grid_source = "none";
const char* g_tracker_state = "not started";
std::atomic<int> g_trk_sent{0}, g_trk_events{0}, g_trk_matched{0}, g_trk_ring{0}, g_trk_fitfail{0};
std::atomic<long long> g_trk_last_msc{0};

bool read_grid(double* p, long long* b) {
    std::lock_guard<std::mutex> lk(g_grid_mx);
    if (g_period_us <= 0.0 || !g_boundary_us) return false;
    *p = g_period_us;
    *b = g_boundary_us;
    return true;
}

// ---------------------------------------------------------------- tracker
#if !defined(_WIN32)
struct Xcb {
    bool ok = false;
    xcb_connection_t* (*connect)(const char*, int*) = nullptr;
    int (*has_error)(xcb_connection_t*) = nullptr;
    uint32_t (*gen_id)(xcb_connection_t*) = nullptr;
    int (*flush)(xcb_connection_t*) = nullptr;
    xcb_generic_event_t* (*poll)(xcb_connection_t*) = nullptr;
    const xcb_query_extension_reply_t* (*ext_data)(xcb_connection_t*, xcb_extension_t*) = nullptr;
    void (*disconnect)(xcb_connection_t*) = nullptr;
    xcb_void_cookie_t (*select_input)(xcb_connection_t*, xcb_present_event_t, xcb_window_t, uint32_t) = nullptr;
    xcb_void_cookie_t (*notify_msc)(xcb_connection_t*, xcb_window_t, uint32_t, uint64_t, uint64_t, uint64_t) = nullptr;
    xcb_extension_t* present_id = nullptr;
} X;

template <class T> void sym(void* h, const char* n, T& out) { out = reinterpret_cast<T>(h ? dlsym(h, n) : nullptr); }

bool load_xcb() {
    // Both are already mapped in DF's process (DF's GL stack uses them); do not
    // pull in anything new.
    void* hx = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD);
    void* hp = dlopen("libxcb-present.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!hx || !hp) return false;
    sym(hx, "xcb_connect", X.connect);
    sym(hx, "xcb_connection_has_error", X.has_error);
    sym(hx, "xcb_generate_id", X.gen_id);
    sym(hx, "xcb_flush", X.flush);
    sym(hx, "xcb_poll_for_event", X.poll);
    sym(hx, "xcb_get_extension_data", X.ext_data);
    sym(hx, "xcb_disconnect", X.disconnect);
    sym(hp, "xcb_present_select_input", X.select_input);
    sym(hp, "xcb_present_notify_msc", X.notify_msc);
    X.present_id = reinterpret_cast<xcb_extension_t*>(dlsym(hp, "xcb_present_id"));
    X.ok = X.connect && X.has_error && X.gen_id && X.flush && X.poll && X.ext_data && X.disconnect &&
           X.select_input && X.notify_msc && X.present_id;
    return X.ok;
}
#endif

std::atomic<bool> g_tracker_run{false};
std::atomic<unsigned long> g_window{0};  // DF's X window id (0 = unknown)
std::thread g_tracker;
double g_nominal_period_us = 0.0;        // from SDL's display mode, sanity bound

struct Sample { long long msc, ust; };

// Fit the refresh grid from XWayland's Present timestamps.
//
// Measured: XWayland's MSC counts DF's presented frames, not refreshes (with DF
// paced to 90 Hz it advances at 90 Hz, so ust/msc gives 11.1 ms), but each UST
// is the compositor's presentation time of that frame, and those land on the
// real refresh grid (gaps of exactly 2.00 refreshes, +-55 us).  So: take the
// refresh period from the display mode, refine it over the longest baseline by
// counting whole refreshes between the first and last timestamp, and take the
// phase as the median residual of every timestamp against that grid.
bool fit(const std::vector<Sample>& s, double* period, long long* boundary) {
    // Period: the display mode's.  Refining it from span/whole-refresh-count
    // (3.35.4) was thrown off by single off-grid timestamps at either end
    // (5542 us fitted vs a measured 2.00-refresh cadence at 5555.6), and the
    // phase then smeared and every later fit failed.  Phase: median residual
    // of the samples from the last 1.5 s only, so it tracks any slow drift.
    if (s.size() < 12 || g_nominal_period_us <= 0.0) return false;
    const double T = g_nominal_period_us;
    const long long newest = s.back().ust;
    std::vector<double> res;
    const long long ref = s.back().ust;
    for (const Sample& x : s) {
        if (newest - x.ust > 1500000) continue;
        const double d = static_cast<double>(x.ust - ref);
        res.push_back(d - T * std::round(d / T));
    }
    if (res.size() < 12) return false;
    std::vector<double> sorted = res;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double med = sorted[sorted.size() / 2];
    size_t near = 0;
    for (double r : res) if (std::fabs(r - med) < 400.0) ++near;
    if (near * 2 < res.size()) return false;         // no coherent phase
    *period = T;
    *boundary = ref + static_cast<long long>(std::llround(med));
    return true;
}

void tracker_main() {
#if !defined(_WIN32)
    xcb_connection_t* c = X.connect(nullptr, nullptr);
    if (!c || X.has_error(c)) { g_tracker_state = "xcb_connect failed"; if (c) X.disconnect(c); return; }
    const xcb_query_extension_reply_t* ext = X.ext_data(c, X.present_id);
    if (!ext || !ext->present) { g_tracker_state = "no Present extension"; X.disconnect(c); return; }
    const xcb_window_t win = static_cast<xcb_window_t>(g_window.load());
    X.select_input(c, X.gen_id(c), win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    X.flush(c);
    std::vector<Sample> ring;
    uint32_t serial = 0;
    while (g_tracker_run.load()) {
        ++serial;
        g_trk_sent++;
        X.notify_msc(c, win, serial, 0, 0, 0);
        X.flush(c);
        const long long give_up = now_us() + 20000;
        bool got = false;
        while (!got && now_us() < give_up && g_tracker_run.load()) {
            xcb_generic_event_t* ev = X.poll(c);
            if (!ev) {
                if (X.has_error(c)) { g_tracker_run = false; break; }
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                continue;
            }
            g_trk_events++;
            if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
                auto* ce = reinterpret_cast<xcb_present_complete_notify_event_t*>(ev);
                if (ce->event_type == XCB_PRESENT_EVENT_COMPLETE_NOTIFY &&
                    ce->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC && ce->serial == serial) {
                    got = true;
                    g_trk_matched++;
                    g_trk_last_msc = static_cast<long long>(ce->msc);
                    const Sample smp{ static_cast<long long>(ce->msc), static_cast<long long>(ce->ust) };
                    if (ring.empty() || smp.msc > ring.back().msc) {
                        ring.push_back(smp);
                        if (ring.size() > 400) ring.erase(ring.begin());
                    }
                    g_trk_ring = static_cast<int>(ring.size());
                }
            }
            free(ev);
        }
        double p = 0; long long b = 0;
        const bool fitted = fit(ring, &p, &b);
        if (!fitted && ring.size() >= 20) g_trk_fitfail++;
        if (fitted) {
            std::lock_guard<std::mutex> lk(g_grid_mx);
            g_period_us = p;
            g_boundary_us = b;
            g_grid_updated_us = now_us();
            g_grid_samples = static_cast<int>(ring.size());
            g_grid_source = "present";
        }
        // Irregular spacing so samples land at every phase of the refresh.
        std::this_thread::sleep_for(std::chrono::microseconds(9000 + (serial * 1733u) % 5000u));
    }
    X.disconnect(c);
#endif
}

// ---------------------------------------------------------------- render thread
bool g_started = false;
long long g_frame_start_us = 0;
long long g_target_us = 0;               // refresh this frame is aimed at (0 = none)
long long g_last_target_us = 0;
std::atomic<long long> g_lead_us{0};
double g_render_ema_us = 4000.0;         // frame start -> ready to present
double g_busy_ema_us = 6000.0;           // previous present done -> ready (DF's own cycle, our hold excluded)
double g_busy_peak_us = 6000.0;          // slow-decay peak of the same (telemetry)
double g_busy_ring[256];
int g_busy_n = 0, g_busy_i = 0;
// Set (any thread) when DF's per-frame cost is about to change step-wise -- a
// zoom commit (cell 24 frames cost ~4x cell 48 ones).  The next present skips
// the rebake frame and the slow frame after it, then restarts the history from
// the new cost, so the frame rate follows within a few frames instead of
// running the whole glide at the old level's rate.
std::atomic<int> g_busy_reset{0};
double busy_p90() {
    // Over the most recent 32 frames only: DF's frame cost changes fast with
    // zoom (cell 48 ~4 ms, cell 24 ~20 ms), and a 256-frame window kept the
    // schedule at a rate DF could no longer make for seconds after zooming out.
    if (g_busy_n < 4) return g_busy_ema_us * 1.3;   // p90 of 4..32 samples ~ their max: conservative
    constexpr int kWin = 32;
    double tmp[kWin];
    const int n = std::min(g_busy_n, kWin);
    for (int i = 0; i < n; ++i) tmp[i] = g_busy_ring[(g_busy_i - 1 - i + 256) % 256];
    std::nth_element(tmp, tmp + (n * 9) / 10, tmp + n);
    return tmp[(n * 9) / 10];
}
double g_cycle_ema_us = 6000.0;          // present -> present, measured
long long g_last_present_us = 0;
int g_divisor_used = 0;
long long g_frames = 0, g_late = 0, g_held = 0;
// Per-frame record for measuring what the display shows (pace record N).
int g_rec_left = 0;
FILE* g_rec = nullptr;

typedef SDL_Window* (*GetWindow_t)(SDL_Renderer*);
typedef int (*DisplayIndex_t)(SDL_Window*);
typedef int (*CurMode_t)(int, SDL_DisplayMode*);
// Mirror of SDL_SysWMinfo's X11 member (avoids SDL_syswm.h dragging in Xlib).
struct SysWM {
    SDL_version version;
    int subsystem;
    union { struct { void* display; unsigned long window; } x11; uint8_t pad[64]; } info;
};
typedef SDL_bool (*WMInfo_t)(SDL_Window*, SysWM*);

void start(SDL_Renderer* r) {
    g_started = true;
    static GetWindow_t gw = reinterpret_cast<GetWindow_t>(sp_sdl_sym("SDL_RenderGetWindow"));
    static DisplayIndex_t di = reinterpret_cast<DisplayIndex_t>(sp_sdl_sym("SDL_GetWindowDisplayIndex"));
    static CurMode_t cm = reinterpret_cast<CurMode_t>(sp_sdl_sym("SDL_GetCurrentDisplayMode"));
    static WMInfo_t wm = reinterpret_cast<WMInfo_t>(sp_sdl_sym("SDL_GetWindowWMInfo"));
    SDL_Window* w = gw ? gw(r) : nullptr;
    if (w && di && cm) {
        SDL_DisplayMode m;
        const int idx = di(w);
        if (idx >= 0 && cm(idx, &m) == 0 && m.refresh_rate > 0) {
            g_nominal_period_us = 1e6 / m.refresh_rate;
            std::lock_guard<std::mutex> lk(g_grid_mx);
            if (g_period_us <= 0.0) {
                // Until the tracker has a phase: regular slots at the nominal
                // rate (steady cadence, unknown phase).
                g_period_us = g_nominal_period_us;
                g_boundary_us = now_us();
                g_grid_source = "nominal";
            }
        }
    }
#if !defined(_WIN32)
    if (!w) g_tracker_state = "no SDL window";
    else if (!wm) g_tracker_state = "no SDL_GetWindowWMInfo";
    else if (!load_xcb()) g_tracker_state = "libxcb/libxcb-present not loaded";
    if (w && wm && X.ok) {
        SysWM info{};
        SDL_VERSION(&info.version);
        if (wm(w, &info) == SDL_TRUE && info.subsystem == 2 /* SDL_SYSWM_X11 */ && info.info.x11.window) {
            g_window = info.info.x11.window;
            g_tracker_run = true;
            g_tracker_state = "running";
            g_tracker = std::thread(tracker_main);
        } else {
            g_tracker_state = "not an X11 window (WMInfo failed)";
        }
    }
#endif
}

int auto_divisor(double period) {
    // Smallest N whose slot DF can reliably fill: its own cycle (previous
    // present -> next frame ready, our hold excluded), using a slow-decay peak
    // so occasional slow frames count, plus the latch margin and slack.
    const double need = busy_p90() + g_latch_us.load() + kSafetyUs;
    int n = static_cast<int>(std::ceil(need / period));
    return std::max(1, std::min(4, n));
}

void sleep_until(long long t) {
    for (;;) {
        const long long d = t - now_us();
        if (d <= 0) return;
        if (d > 700) {
#if !defined(_WIN32)
            timespec ts{ 0, static_cast<long>((d - 400) * 1000) };
            nanosleep(&ts, nullptr);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(d - 400));
#endif
        }
        // last few hundred us: spin for accuracy
    }
}

}  // namespace

void pacing_on_frame_start(SDL_Renderer* r) {
    if (!r) return;
    if (!g_started) start(r);
    const long long now = now_us();
    g_frame_start_us = now;
    g_target_us = 0;
    g_lead_us.store(0, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0) return;
    double P; long long B;
    if (!read_grid(&P, &B)) return;
    const int N = mode > 0 ? mode : auto_divisor(P);
    g_divisor_used = N;
    const double slot = N * P;
    const long long earliest = now + static_cast<long long>(g_render_ema_us) + g_latch_us.load() + kSafetyUs;
    long long target;
    if (g_last_target_us && earliest - g_last_target_us < 8 * static_cast<long long>(slot)) {
        // Keep the rhythm relative to where the PREVIOUS frame actually landed
        // (snapped to the refresh grid), not to a fixed parity of the grid.
        // When DF delivers a frame late it lands one refresh after its target;
        // re-anchoring there turns the old 3-then-1 refresh pair (two
        // irregular steps for one late frame) into 3, 2, 2, ... (one hitch).
        const double n = std::round(static_cast<double>(g_last_target_us - B) / P);
        const long long last = B + static_cast<long long>(n * P);
        const double k = std::max(1.0, std::ceil(static_cast<double>(earliest - last) / slot));
        target = last + static_cast<long long>(k * slot);
    } else {
        const double k = std::ceil(static_cast<double>(earliest - B) / slot);
        target = B + static_cast<long long>(k * slot);
    }
    g_target_us = target;
    g_lead_us.store(target - now, std::memory_order_relaxed);
}

void pacing_before_present(SDL_Renderer* r) {
    (void)r;
    const long long entry = now_us();
    if (g_last_present_us) {
        const double busy = static_cast<double>(entry - g_last_present_us);
        const int reset = g_busy_reset.load(std::memory_order_acquire);
        if (reset > 0) {
            if (reset > 1) {
                g_busy_reset.store(reset - 1, std::memory_order_release);
            } else if (busy > 0 && busy < 60000) {
                g_busy_reset.store(0, std::memory_order_release);
                g_busy_ema_us = busy;
                g_busy_n = 0;
            }
        } else if (busy > 0 && busy < 60000) {
            g_busy_ema_us = g_busy_ema_us * 0.9 + busy * 0.1;
            g_busy_peak_us = std::max(busy, g_busy_peak_us * 0.995 + g_busy_ema_us * 0.005);
            g_busy_ring[g_busy_i] = busy;
            g_busy_i = (g_busy_i + 1) % 256;
            if (g_busy_n < 256) g_busy_n++;
        }
    }
    if (g_frame_start_us) {
        const double rd = static_cast<double>(entry - g_frame_start_us);
        if (rd > 0 && rd < 60000) g_render_ema_us = g_render_ema_us * 0.9 + rd * 0.1;
    }
    if (!g_target_us) return;
    const long long deadline = g_target_us - g_latch_us.load();
    double P; long long B;
    if (entry < deadline) {
        sleep_until(deadline);
        g_held++;
        g_last_target_us = g_target_us;
    } else {
        g_late++;
        // Late: it will land on the first refresh it can still make.
        long long landing = g_target_us;
        if (read_grid(&P, &B)) {
            const long long need = entry + g_latch_us.load();
            while (landing < need) landing += static_cast<long long>(P);
        }
        g_last_target_us = landing;
    }
}

void pacing_after_present() {
    const long long now = now_us();
    if (g_last_present_us) {
        const double c = static_cast<double>(now - g_last_present_us);
        if (c > 0 && c < 60000) g_cycle_ema_us = g_cycle_ema_us * 0.95 + c * 0.05;
    }
    g_last_present_us = now;
    g_frames++;
    if (g_rec_left > 0 && g_rec) {
        double P = 0; long long B = 0;
        read_grid(&P, &B);
        CompositorFrameInfo ci;
        compositor_last_frame(&ci);
        const double cx = (df::global::window_x ? *df::global::window_x : 0) + g_camera.shown_frac_x();
        const double cy = (df::global::window_y ? *df::global::window_y : 0) + g_camera.shown_frac_y();
        fprintf(g_rec, "start=%lld done=%lld target=%lld P=%.3f B=%lld cx=%.5f cy=%.5f cell=%d scale=%.5f\n",
                g_frame_start_us, now, g_target_us, P, B, cx, cy, ci.cell, ci.scale);
        if (--g_rec_left == 0) { fclose(g_rec); g_rec = nullptr; }
    }
}

void pacing_record(int frames, const char* label) {
    if (g_rec) { fclose(g_rec); g_rec = nullptr; }
    g_rec_left = frames;
    if (frames <= 0) return;
    g_rec = fopen(smoothpan_log_path("smoothpan_pace.txt").c_str(), "a");
    if (g_rec) fprintf(g_rec, "# block label=%s mode=%d latch=%d\n", label ? label : "-", g_mode.load(), g_latch_us.load());
}

void pacing_set_latch(int us) { g_latch_us = std::max(0, std::min(10000, us)); }

long long pacing_frame_lead_us() { return g_lead_us.load(std::memory_order_relaxed); }

void pacing_set_mode(int divisor) {
    g_mode = divisor;
    g_last_target_us = 0;
}
int pacing_mode() { return g_mode.load(); }

bool pacing_grid(double* period_us, long long* boundary_us) { return read_grid(period_us, boundary_us); }

void pacing_status(char* buf, size_t n) {
    if (!buf || !n) return;
    double P = 0; long long B = 0;
    const bool ok = read_grid(&P, &B);
    long long upd;
    int samples;
    const char* src;
    {
        std::lock_guard<std::mutex> lk(g_grid_mx);
        upd = g_grid_updated_us; samples = g_grid_samples; src = g_grid_source;
    }
    const int m = g_mode.load();
    snprintf(buf, n,
             "pacing=%s every=%d tracker=%s grid=%s period=%.1fus (%.2fHz) samples=%d grid_age=%lldms "
             "render_ema=%.0fus busy_ema=%.0fus busy_p90=%.0fus latch=%dus frames=%lld held=%lld late=%lld "
             "trk(sent=%d ev=%d match=%d ring=%d fitfail=%d msc=%lld) rec_left=%d",
             m == 0 ? "off" : (m < 0 ? "auto" : "fixed"), g_divisor_used, g_tracker_state, ok ? src : "none", P,
             P > 0 ? 1e6 / P : 0.0, samples, upd ? (now_us() - upd) / 1000 : -1, g_render_ema_us,
             g_busy_ema_us, busy_p90(), g_latch_us.load(), g_frames, g_held, g_late,
             g_trk_sent.load(), g_trk_events.load(), g_trk_matched.load(), g_trk_ring.load(),
             g_trk_fitfail.load(), g_trk_last_msc.load(), g_rec_left);
}

void pacing_note_workload_change() { g_busy_reset.store(3, std::memory_order_release); }

void pacing_shutdown() {
    g_tracker_run = false;
    if (g_tracker.joinable()) g_tracker.join();
}

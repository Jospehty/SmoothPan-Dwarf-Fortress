#include "selftest.h"

#include "platform.h"
#include "sdl_fn.h"
#include "sdl_hook.h"
#include "camera.h"
#include "compositor.h"
#include "zoom_camera.h"
#include "renderer_hook.h"
#include "viewport.h"
#include "mouse_comp.h"
#include "shift_mode.h"
#include "debug_paths.h"
#include "version.h"

#include "Core.h"
#include "Console.h"
#include "DataDefs.h"
#include "DFHackVersion.h"
#include "modules/Gui.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/enabler.h"
#include "df/renderer_2d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace DFHack;

extern bool& is_enabled;
extern int g_sp_virtual_pan_x;
extern int g_sp_virtual_pan_y;

namespace {

// ---- SDL helpers (real functions, resolved once) ---------------------------
typedef SDL_Window* (*SP_RenderGetWindow_t)(SDL_Renderer*);
typedef void (*SP_GetWindowSize_t)(SDL_Window*, int*, int*);
typedef int (*SP_GetRendererInfo_t)(SDL_Renderer*, SDL_RendererInfo*);
typedef const char* (*SP_GetCurrentVideoDriver_t)(void);
typedef void (*SP_GetVersion_t)(SDL_version*);
typedef int (*SP_RenderReadPixels_t)(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int);
typedef SDL_Surface* (*SP_CreateRGBSurfaceWithFormatFrom_t)(void*, int, int, int, int, Uint32);
typedef void (*SP_FreeSurface_t)(SDL_Surface*);
typedef SDL_RWops* (*SP_RWFromFile_t)(const char*, const char*);
typedef int (*SP_SaveBMP_RW_t)(SDL_Surface*, SDL_RWops*, int);
typedef int (*SP_IMG_SavePNG_t)(SDL_Surface*, const char*);
typedef SDL_Texture* (*SP_GetRenderTarget_t)(SDL_Renderer*);

template <class T> static T sym(const char* name) { return reinterpret_cast<T>(sp_sdl_sym(name)); }

static SDL_Renderer* df_renderer() {
    if (!df::global::enabler) return nullptr;
    auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
    return r2d ? reinterpret_cast<SDL_Renderer*>(r2d->sdl_renderer) : nullptr;
}

static bool output_size(SDL_Renderer* r, int* w, int* h) {
    return r && GetRendererOutputSize_func && GetRendererOutputSize_func(r, w, h) == 0 && *w > 0 && *h > 0;
}

static bool read_rect(SDL_Renderer* r, const SDL_Rect& rc, std::vector<uint32_t>& buf) {
    static SP_RenderReadPixels_t rp = sym<SP_RenderReadPixels_t>("SDL_RenderReadPixels");
    if (!rp || rc.w <= 0 || rc.h <= 0) return false;
    buf.resize(static_cast<size_t>(rc.w) * rc.h);
    return rp(r, &rc, SDL_PIXELFORMAT_ARGB8888, buf.data(), rc.w * 4) == 0;
}

static inline bool is_black(uint32_t p) {
    return ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF) < 30;
}

// Save the current frame (PNG via SDL2_image when available, else BMP).
static std::string save_frame(SDL_Renderer* r, const char* tag) {
    int w = 0, h = 0;
    if (!output_size(r, &w, &h)) return "";
    std::vector<uint32_t> px;
    SDL_Rect all = { 0, 0, w, h };
    if (!read_rect(r, all, px)) return "";
    static SP_CreateRGBSurfaceWithFormatFrom_t mk = sym<SP_CreateRGBSurfaceWithFormatFrom_t>("SDL_CreateRGBSurfaceWithFormatFrom");
    static SP_FreeSurface_t fr = sym<SP_FreeSurface_t>("SDL_FreeSurface");
    static SP_RWFromFile_t rw = sym<SP_RWFromFile_t>("SDL_RWFromFile");
    static SP_SaveBMP_RW_t bmp = sym<SP_SaveBMP_RW_t>("SDL_SaveBMP_RW");
    static SP_IMG_SavePNG_t png = reinterpret_cast<SP_IMG_SavePNG_t>(sp_sdl_image_sym("IMG_SavePNG"));
    if (!mk || !fr) return "";
    SDL_Surface* s = mk(px.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return "";
    std::string name = std::string("smoothpan_selftest_") + tag;
    std::string path;
    if (png) {
        path = smoothpan_log_path((name + ".png").c_str());
        if (png(s, path.c_str()) != 0) path.clear();
    }
    if (path.empty() && rw && bmp) {
        path = smoothpan_log_path((name + ".bmp").c_str());
        SDL_RWops* o = rw(path.c_str(), "wb");
        if (!o || bmp(s, o, 1) != 0) path.clear();
    }
    fr(s);
    return path;
}

// ---- script ----------------------------------------------------------------
enum class Act { None, Settle, CompOn, CompOff, Away, Back, PanA, PanB, Coast, VanAway, VanBack };

struct Step {
    const char* name;
    int frames;
    Act act;
    int repeat_at;      // frame at which the act repeats (second notch), -1 none
    int shot_a;         // frames at which to save a screenshot (-1 none)
    int shot_b;
    const char* shot_a_tag;
    const char* shot_b_tag;
};

static const Step kSteps[] = {
    { "settle",       30, Act::Settle,  -1, -1, -1, nullptr, nullptr },
    { "idle_on",      20, Act::CompOn,  -1, 19, -1, "01_idle_compositor_on", nullptr },
    { "idle_off",     20, Act::CompOff, -1, 19, -1, "02_idle_compositor_off", nullptr },
    { "idle_on_2",    20, Act::CompOn,  -1, -1, -1, nullptr, nullptr },
    { "zoom_away_1",  60, Act::Away,    -1,  3, 59, "03_zoom_away_mid", "04_zoom_away_end" },
    { "zoom_back_1",  60, Act::Back,    -1,  3, -1, "05_zoom_back_mid", nullptr },
    { "zoom_away_2",  80, Act::Away,     4,  8, -1, "06_zoom_away2_mid", nullptr },
    { "zoom_back_2",  80, Act::Back,     4, -1, 79, nullptr, "07_zoom_restored" },
    { "pan_a",        45, Act::PanA,    -1, 20, -1, "08_pan_mid", nullptr },
    { "pan_b",        45, Act::PanB,    -1, -1, -1, nullptr, nullptr },
    { "pan_coast",    30, Act::Coast,   -1, -1, -1, nullptr, nullptr },
    { "vanilla_away", 40, Act::VanAway, -1,  1,  2, "09_vanilla_commit_f1", "10_vanilla_commit_f2" },
    { "vanilla_back", 40, Act::VanBack, -1, -1, -1, nullptr, nullptr },
};
constexpr int kNumSteps = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

struct Rec {
    int step = 0, f = 0;
    double ms = 0;
    CompositorFrameInfo ci;
    ZoomCameraInfo zi;
    int gap[4] = { -1, -1, -1, -1 };   // L R T B consecutive black px at the viewport edge (min over samples)
    float fx = 0, fy = 0;
    int wx = 0, wy = 0;
    int vpw = 0, vph = 0;
};

struct StepEnd {
    double world_cx = 0, world_cy = 0;
    bool valid = false;
    int cell = 0;
};

// Threads: selftest_tick runs on DF's simulation/interface thread (viewscreen
// render), selftest_on_present on the SDL render thread.  Step progression and
// all recording happen on the render thread; the tick only applies inputs.
static std::atomic<bool> g_running{false};
static std::atomic<int> g_step{0};
static std::atomic<int> g_frame{0};      // frames presented in the current step
static std::atomic<bool> g_step_entered{false};
static int g_presents_without_tick = 0;
static std::atomic<bool> g_ticked_this_frame{false};
static std::atomic<bool> g_abort_req{false};
static std::string g_abort_req_reason;
static int g_dir = -1;           // direction of "away" notches (-1 out, +1 in)
static uint64_t g_last_present = 0;

static bool g_saved_pause_valid = false;
static bool g_saved_pause = false;
static bool g_saved_comp = true;
static bool g_saved_zoom = true;

static std::vector<Rec> g_recs;
static std::vector<std::string> g_shots;
static std::vector<uint32_t> g_parity_a;
static SDL_Rect g_parity_rc = { 0, 0, 0, 0 };
static double g_parity_diff_frac = -1, g_parity_mean = -1;
static StepEnd g_step_end[kNumSteps + 1];
static std::string g_diag_at_start;
static std::string g_abort_reason;
static std::atomic<bool> g_finished_flag{false};
static std::string g_summary;
static std::string g_last_verdict = "never run";

static void set_pan(int x, int y) {
    g_sp_virtual_pan_x = x;
    g_sp_virtual_pan_y = y;
    if (x > 0) g_camera.panning_right = true;
    if (x < 0) g_camera.panning_left = true;
    if (y > 0) g_camera.panning_down = true;
    if (y < 0) g_camera.panning_up = true;
}

static void world_centre(double* wx, double* wy, int* cell) {
    ViewportRect vp;
    *wx = *wy = 0;
    *cell = 0;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) return;
    const double cx = 0.5 * (vp.left + vp.right), cy = 0.5 * (vp.top + vp.bottom);
    *wx = g_camera.true_x + (cx - vp.origin_x) / vp.cell_size;
    *wy = g_camera.true_y + (cy - vp.origin_y) / vp.cell_size;
    *cell = vp.cell_size;
}

static void measure_gaps(SDL_Renderer* r, Rec& rec) {
    ViewportRect vp;
    int w = 0, h = 0;
    if (!get_strict_viewport_rect(&vp) || !output_size(r, &w, &h)) return;
    const int L = std::max(0, vp.left), R = std::min(w, vp.right);
    const int T = std::max(0, vp.top), B = std::min(h, vp.bottom);
    rec.vpw = R - L;
    rec.vph = B - T;
    if (R - L < 8 || B - T < 8) return;
    std::vector<uint32_t> px;
    int gl = 1 << 30, gr = 1 << 30, gt = 1 << 30, gb = 1 << 30;
    for (int k = 1; k <= 3; ++k) {
        const int y = T + (B - T) * k / 4;
        SDL_Rect row = { L, y, R - L, 1 };
        if (read_rect(r, row, px)) {
            int a = 0; while (a < row.w && is_black(px[a])) ++a;
            int b = 0; while (b < row.w && is_black(px[row.w - 1 - b])) ++b;
            gl = std::min(gl, a);
            gr = std::min(gr, b);
        }
        const int x = L + (R - L) * k / 4;
        SDL_Rect col = { x, T, 1, B - T };
        if (read_rect(r, col, px)) {
            int a = 0; while (a < col.h && is_black(px[a])) ++a;
            int b = 0; while (b < col.h && is_black(px[col.h - 1 - b])) ++b;
            gt = std::min(gt, a);
            gb = std::min(gb, b);
        }
    }
    rec.gap[0] = gl == (1 << 30) ? -1 : gl;
    rec.gap[1] = gr == (1 << 30) ? -1 : gr;
    rec.gap[2] = gt == (1 << 30) ? -1 : gt;
    rec.gap[3] = gb == (1 << 30) ? -1 : gb;
}

static void parity_capture(SDL_Renderer* r, bool reference) {
    ViewportRect vp;
    int w = 0, h = 0;
    if (!get_strict_viewport_rect(&vp) || !output_size(r, &w, &h)) return;
    SDL_Rect rc = { std::max(0, vp.left), std::max(0, vp.top), 0, 0 };
    rc.w = std::min(w, vp.right) - rc.x;
    rc.h = std::min(h, vp.bottom) - rc.y;
    if (rc.w <= 0 || rc.h <= 0) return;
    if (reference) {
        g_parity_rc = rc;
        if (!read_rect(r, rc, g_parity_a)) g_parity_a.clear();
        return;
    }
    std::vector<uint32_t> b;
    if (g_parity_a.empty() || rc.x != g_parity_rc.x || rc.y != g_parity_rc.y ||
        rc.w != g_parity_rc.w || rc.h != g_parity_rc.h || !read_rect(r, rc, b)) {
        g_parity_diff_frac = -2;   // geometry changed between captures
        return;
    }
    size_t differ = 0;
    double sum = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const uint32_t p = g_parity_a[i], q = b[i];
        int dmax = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            const int d = std::abs(static_cast<int>((p >> sh) & 0xFF) - static_cast<int>((q >> sh) & 0xFF));
            sum += d;
            dmax = std::max(dmax, d);
        }
        if (dmax > 32) ++differ;
    }
    g_parity_diff_frac = b.empty() ? -1 : static_cast<double>(differ) / b.size();
    g_parity_mean = b.empty() ? -1 : sum / (3.0 * b.size());
}

static void enter_step(int i) {
    const Step& s = kSteps[i];
    switch (s.act) {
    case Act::Settle:
        compositor_set_enabled(true);
        zoom_camera_set_enabled(true);
        break;
    case Act::CompOn: compositor_set_enabled(true); break;
    case Act::CompOff: compositor_set_enabled(false); break;
    case Act::Away: zoom_camera_on_zoom_key(g_dir, false); break;
    case Act::Back: zoom_camera_on_zoom_key(-g_dir, false); break;
    case Act::VanAway:
        zoom_camera_set_enabled(false);
        zoom_camera_queue_test_step(g_dir);
        break;
    case Act::VanBack:
        zoom_camera_queue_test_step(-g_dir);
        break;
    default: break;
    }
}

static void finish(const char* abort_reason);

}  // namespace

// =============================================================================
// diag
// =============================================================================
void smoothpan_write_diag(std::string& out) {
    char b[512];
    time_t now = time(nullptr);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    snprintf(b, sizeof(b), "SmoothPan %s diag  %s\n", SMOOTHPAN_BUILD_VERSION, ts);
    out += b;
    snprintf(b, sizeof(b), "platform=%s input=%s dfhack=%s (%s) df=%s enabled=%d\n",
             sp_platform_name(), sp_input_backend(), Version::dfhack_version(),
             Version::git_description(), Version::df_version(), is_enabled ? 1 : 0);
    out += b;
#if !defined(_WIN32)
    const char* envs[] = { "XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY", "SDL_VIDEODRIVER",
                           "SDL_RENDER_DRIVER", "LD_PRELOAD", "STEAM_RUNTIME", "PRESSURE_VESSEL_RUNTIME" };
    out += "env:";
    for (const char* e : envs) {
        const char* v = getenv(e);
        snprintf(b, sizeof(b), " %s=%s", e, v ? v : "-");
        out += b;
    }
    out += "\n";
#endif
    SDL_version ver = { 0, 0, 0 };
    if (auto gv = sym<SP_GetVersion_t>("SDL_GetVersion")) gv(&ver);
    auto vd = sym<SP_GetCurrentVideoDriver_t>("SDL_GetCurrentVideoDriver");
    snprintf(b, sizeof(b), "sdl=%d.%d.%d video=%s lib=%s image_lib=%s\n", ver.major, ver.minor, ver.patch,
             vd && vd() ? vd() : "?", sp_sdl_module_name(),
             sp_sdl_image_sym("IMG_SavePNG") ? "yes" : "no");
    out += b;

    SDL_Renderer* r = df_renderer();
    if (r) {
        SDL_RendererInfo info;
        memset(&info, 0, sizeof(info));
        if (auto gi = sym<SP_GetRendererInfo_t>("SDL_GetRendererInfo")) gi(r, &info);
        int ow = 0, oh = 0, ww = 0, wh = 0;
        output_size(r, &ow, &oh);
        auto gw = sym<SP_RenderGetWindow_t>("SDL_RenderGetWindow");
        auto gs = sym<SP_GetWindowSize_t>("SDL_GetWindowSize");
        if (gw && gs && gw(r)) gs(gw(r), &ww, &wh);
        auto gt = sym<SP_GetRenderTarget_t>("SDL_GetRenderTarget");
        snprintf(b, sizeof(b), "renderer=%s flags=0x%x target_textures=%d max_tex=%dx%d output=%dx%d window=%dx%d%s current_target=%p\n",
                 info.name ? info.name : "?", info.flags, (info.flags & SDL_RENDERER_TARGETTEXTURE) ? 1 : 0,
                 info.max_texture_width, info.max_texture_height, ow, oh, ww, wh,
                 (ow != ww || oh != wh) ? " (HIGHDPI: window != output)" : "",
                 gt ? static_cast<void*>(gt(r)) : nullptr);
        out += b;
    } else {
        out += "renderer=<renderer_2d not found>\n";
    }
    if (df::global::enabler) {
        auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
        if (r2d) {
            snprintf(b, sizeof(b), "renderer_2d origin=(%d,%d) dispx=(%d,%d) dispx_z=(%d,%d) cur=(%d,%d) screen_tex=%p\n",
                     r2d->origin_x, r2d->origin_y, r2d->dispx, r2d->dispy, r2d->dispx_z, r2d->dispy_z,
                     r2d->cur_w, r2d->cur_h, static_cast<void*>(r2d->screen_tex));
            out += b;
        } else {
            out += "renderer_2d: virtual_cast failed (renderer is not renderer_2d)\n";
        }
    }
    if (df::global::gps) {
        auto* g = df::global::gps;
        ViewportRect vp;
        get_strict_viewport_rect(&vp);
        snprintf(b, sizeof(b), "gps zoom_factor=%d cell=%d main_viewport dim=(%d,%d) screen=(%d,%d) vp=[%d,%d..%d,%d] precise_mouse=(%d,%d)\n",
                 g->viewport_zoom_factor, vp.cell_size,
                 g->main_viewport ? g->main_viewport->dim_x : -1, g->main_viewport ? g->main_viewport->dim_y : -1,
                 g->main_viewport ? g->main_viewport->screen_x : -1, g->main_viewport ? g->main_viewport->screen_y : -1,
                 vp.left, vp.top, vp.right, vp.bottom, g->precise_mouse_x, g->precise_mouse_y);
        out += b;
    }
    snprintf(b, sizeof(b), "shift_mode=%s mouse=%s camera true=(%.3f,%.3f) window=(%d,%d)\n",
             shift_mode_name(g_shift_mode), mouse_comp_effective_name(), g_camera.true_x, g_camera.true_y,
             df::global::window_x ? *df::global::window_x : -1, df::global::window_y ? *df::global::window_y : -1);
    out += b;
    sp_hooks_report(out);
    renderer_hook_report(out);
    extern void smoothpan_dwarfmode_hook_report(std::string&);
    smoothpan_dwarfmode_hook_report(out);
    char st[400];
    compositor_status(st, sizeof(st));
    out += st;
    out += "\n";
    zoom_camera_status(st, sizeof(st));
    out += st;
    out += "\n";
    out += "selftest: " + selftest_status() + "\n";

    // Verdict lines the testing agent can grep for.
    const int rc_sites = sp_hook_sites("SDL_RenderCopy") + sp_hook_sites("SDL_RenderCopyF");
    out += rc_sites > 0 ? "CHECK hooks_render_copy OK\n"
                        : "CHECK hooks_render_copy FAIL (no DF call sites for SDL_RenderCopy[F] were patched)\n";
    out += sp_hook_sites("SDL_RenderPresent") > 0 ? "CHECK hooks_present OK\n" : "CHECK hooks_present FAIL\n";
    out += compositor_state() == CompositorState::Disabled
               ? std::string("CHECK compositor FAIL (") + compositor_disable_reason() + ")\n"
               : std::string("CHECK compositor ") + compositor_state_name() + "\n";
}

// =============================================================================
// selftest
// =============================================================================
bool selftest_running() { return g_running.load(); }

bool selftest_start(std::string& why) {
    if (g_running.load()) { why = "already running"; return false; }
    if (!is_enabled) { why = "plugin not enabled (run: enable smoothpan)"; return false; }
    bool fort = false;
    for (const std::string& f : Gui::getCurFocus(true))
        if (f.rfind("dwarfmode", 0) == 0) fort = true;
    if (!fort) { why = "not in fortress mode (load a fortress and close menus first)"; return false; }

    ZoomCameraInfo zi;
    zoom_camera_info(&zi);
    g_dir = (zi.baked_cell >= 32) ? -1 : +1;

    g_diag_at_start.clear();
    smoothpan_write_diag(g_diag_at_start);

    g_saved_comp = compositor_user_enabled();
    g_saved_zoom = zoom_camera_enabled();
    g_saved_pause_valid = df::global::pause_state != nullptr;
    if (g_saved_pause_valid) {
        g_saved_pause = *df::global::pause_state;
        *df::global::pause_state = true;   // freeze the world so frames are comparable
    }
    g_recs.clear();
    g_recs.reserve(800);
    g_shots.clear();
    g_parity_a.clear();
    g_parity_diff_frac = g_parity_mean = -1;
    for (auto& e : g_step_end) e = StepEnd{};
    g_abort_reason.clear();
    g_summary.clear();
    g_step = 0;
    g_frame = 0;
    g_step_entered = false;
    g_presents_without_tick = 0;
    g_ticked_this_frame = false;
    g_abort_req = false;
    g_last_present = 0;
    g_finished_flag = false;
    g_running = true;
    return true;
}

void selftest_abort(const char* why) {
    if (!g_running.load()) return;
    if (!is_enabled) {
        // Plugin being disabled: hooks are going away, finish synchronously.
        finish(why ? why : "aborted");
        return;
    }
    g_abort_req_reason = why ? why : "aborted";
    g_abort_req = true;   // finished on the render thread at the next present
}

void selftest_tick() {
    if (!g_running.load()) return;
    const int step = g_step.load();
    if (step < 0 || step >= kNumSteps) return;
    if (!g_step_entered.exchange(true)) enter_step(step);
    g_ticked_this_frame = true;
    const Step& s = kSteps[step];
    if (s.repeat_at >= 0 && g_frame.load() == s.repeat_at) {
        if (s.act == Act::Away) zoom_camera_on_zoom_key(g_dir, false);
        if (s.act == Act::Back) zoom_camera_on_zoom_key(-g_dir, false);
    }
    switch (s.act) {
    case Act::PanA: set_pan(+1, 0); break;
    case Act::PanB: set_pan(-1, 0); break;
    default: set_pan(0, 0); break;
    }
}

void selftest_on_present(SDL_Renderer* r) {
    if (!g_running.load()) return;
    if (g_abort_req.exchange(false)) { finish(g_abort_req_reason.c_str()); return; }
    if (!g_ticked_this_frame.load()) {
        if (++g_presents_without_tick > 120) finish("fortress map not rendering (menu or other screen open?)");
        return;
    }
    g_ticked_this_frame = false;
    g_presents_without_tick = 0;
    if (!is_enabled) { finish("plugin disabled during test"); return; }

    const uint64_t now = sp_perf_counter();
    Rec rec;
    rec.step = g_step.load();
    rec.f = g_frame.load();
    rec.ms = g_last_present ? sp_perf_us(g_last_present, now) / 1000.0 : 0.0;
    g_last_present = now;
    compositor_last_frame(&rec.ci);
    zoom_camera_info(&rec.zi);
    rec.fx = g_camera.render_frac_x;
    rec.fy = g_camera.render_frac_y;
    rec.wx = df::global::window_x ? *df::global::window_x : 0;
    rec.wy = df::global::window_y ? *df::global::window_y : 0;
    measure_gaps(r, rec);
    g_recs.push_back(rec);

    const Step& s = kSteps[g_step.load()];
    if (g_frame.load() == s.shot_a && s.shot_a_tag) {
        std::string p = save_frame(r, s.shot_a_tag);
        if (!p.empty()) g_shots.push_back(p);
        if (s.act == Act::CompOn && g_step.load() == 1) parity_capture(r, true);
        if (s.act == Act::CompOff) parity_capture(r, false);
    }
    if (g_frame.load() == s.shot_b && s.shot_b_tag) {
        std::string p = save_frame(r, s.shot_b_tag);
        if (!p.empty()) g_shots.push_back(p);
    }

    if (g_frame.load() + 1 >= s.frames) {
        StepEnd& e = g_step_end[g_step.load()];
        world_centre(&e.world_cx, &e.world_cy, &e.cell);
        e.valid = true;
        g_frame = 0;
        g_step_entered = false;
        if (g_step.load() + 1 >= kNumSteps) finish(nullptr);
        else g_step = g_step.load() + 1;
    } else {
        g_frame = g_frame.load() + 1;
    }
}

std::string selftest_status() {
    if (g_running.load()) {
        char b[128];
        const int st = std::min(g_step.load(), kNumSteps - 1);
        snprintf(b, sizeof(b), "running step %d/%d (%s) frame %d", st + 1, kNumSteps,
                 kSteps[st].name, g_frame.load());
        return b;
    }
    return g_last_verdict;
}

bool selftest_take_finished(std::string& summary) {
    if (!g_finished_flag.exchange(false)) return false;
    summary = g_summary;
    return true;
}

// ---- report -------------------------------------------------------------------
namespace {

static const char* step_name(int i) { return (i >= 0 && i < kNumSteps) ? kSteps[i].name : "?"; }

static void finish(const char* abort_reason) {
    g_running = false;
    set_pan(0, 0);
    g_sp_virtual_pan_x = g_sp_virtual_pan_y = 0;
    compositor_set_enabled(g_saved_comp);
    zoom_camera_set_enabled(g_saved_zoom);
    if (g_saved_pause_valid && df::global::pause_state) *df::global::pause_state = g_saved_pause;
    if (abort_reason) g_abort_reason = abort_reason;

    // Baselines from the idle step (compositor on, at rest).
    int base[4] = { 0, 0, 0, 0 };
    {
        std::vector<int> v[4];
        for (const Rec& r : g_recs)
            if (r.step == 1)
                for (int k = 0; k < 4; ++k) if (r.gap[k] >= 0) v[k].push_back(r.gap[k]);
        for (int k = 0; k < 4; ++k) {
            if (v[k].empty()) continue;
            std::sort(v[k].begin(), v[k].end());
            base[k] = v[k][v[k].size() / 2];
        }
    }

    std::string rep;
    char b[640];
    snprintf(b, sizeof(b), "# SmoothPan %s selftest\n", SMOOTHPAN_BUILD_VERSION);
    rep += b;
    rep += g_diag_at_start;
    rep += "\n== VERDICT ==\n";
    int fails = 0, warns = 0;
    auto line = [&](const char* level, const std::string& text) {
        if (!strcmp(level, "FAIL")) ++fails;
        if (!strcmp(level, "WARN")) ++warns;
        rep += level;
        rep += " ";
        rep += text;
        rep += "\n";
    };
    if (!g_abort_reason.empty()) line("FAIL", "aborted: " + g_abort_reason);

    snprintf(b, sizeof(b), "baseline_gaps L=%d R=%d T=%d B=%d (idle, compositor on; black map areas count too)",
             base[0], base[1], base[2], base[3]);
    line("INFO", b);

    // Parity.
    if (g_parity_diff_frac >= 0) {
        snprintf(b, sizeof(b), "parity compositor on vs off: differing_px=%.3f%% mean_abs=%.2f",
                 100.0 * g_parity_diff_frac, g_parity_mean);
        line(g_parity_diff_frac < 0.02 ? "OK" : (g_parity_diff_frac < 0.10 ? "WARN" : "FAIL"), b);
    } else {
        line("WARN", g_parity_diff_frac == -2 ? "parity: viewport geometry changed between captures"
                                              : "parity: not captured");
    }

    // Compositor health over the whole run.
    {
        int frames_on = 0, captured = 0, bridged = 0, forced = 0, late = 0;
        int end_counts[5] = { 0, 0, 0, 0, 0 };
        for (const Rec& r : g_recs) {
            if (r.step == 2) continue;   // compositor off
            ++frames_on;
            if (!r.ci.started) continue;
            ++captured;
            if (r.ci.choice == 2) ++bridged;
            if (r.ci.choice == 3) ++forced;
            if (r.ci.late_passes > 0) ++late;
            const char* ends = "bfrp-";
            const char* p = strchr(ends, r.ci.end_by);
            end_counts[p ? p - ends : 4]++;
        }
        snprintf(b, sizeof(b), "compositor: captured %d/%d frames, bridged=%d forced=%d late_pass_frames=%d layer_end b=%d f=%d r=%d p=%d renderer=%s",
                 captured, frames_on, bridged, forced, late, end_counts[0], end_counts[1], end_counts[2], end_counts[3],
                 compositor_renderer_name());
        line(captured == 0 ? "FAIL" : (captured < frames_on * 9 / 10 ? "WARN" : "OK"), b);
    }

    // Per-step summaries.
    for (int i = 0; i < kNumSteps; ++i) {
        int n = 0, bridged = 0, forced = 0, notcap = 0, settle = -1;
        float smin = 1e9f, smax = 0;
        int excess[4] = { 0, 0, 0, 0 };
        int excess_frame[4] = { -1, -1, -1, -1 };
        int commits0 = -1, commits1 = 0, landed0 = -1, landed1 = 0, to0 = -1, to1 = 0, ext0 = -1, ext1 = 0;
        int cell_first = 0, cell_last = 0;
        double ms_max = 0;
        for (const Rec& r : g_recs) {
            if (r.step != i) continue;
            ++n;
            if (commits0 < 0) { commits0 = r.zi.commits; landed0 = r.zi.landed; to0 = r.zi.timeouts; ext0 = r.zi.external; cell_first = r.zi.baked_cell; }
            commits1 = r.zi.commits; landed1 = r.zi.landed; to1 = r.zi.timeouts; ext1 = r.zi.external; cell_last = r.zi.baked_cell;
            if (!r.ci.started && i != 2) ++notcap;
            if (r.ci.choice == 2) ++bridged;
            if (r.ci.choice == 3) ++forced;
            if (r.ci.started) { smin = std::min(smin, r.ci.scale); smax = std::max(smax, r.ci.scale); }
            if (settle < 0 && r.f > 0 && !r.zi.gesture && !r.zi.pending && std::fabs(r.ci.scale - 1.0f) < 0.001f) settle = r.f;
            ms_max = std::max(ms_max, r.ms);
            for (int k = 0; k < 4; ++k) {
                if (r.gap[k] < 0) continue;
                const int ex = r.gap[k] - base[k];
                if (ex > excess[k]) { excess[k] = ex; excess_frame[k] = r.f; }
            }
        }
        if (!n) continue;
        const int band = std::max(std::max(excess[0], excess[1]), std::max(excess[2], excess[3]));
        const int cell = std::max(1, cell_last);
        double drift = -1;
        const Step& s = kSteps[i];
        const bool zoom_step = s.act == Act::Away || s.act == Act::Back || s.act == Act::VanAway || s.act == Act::VanBack;
        if (zoom_step && i > 0 && g_step_end[i - 1].valid && g_step_end[i].valid)
            drift = std::hypot(g_step_end[i].world_cx - g_step_end[i - 1].world_cx,
                               g_step_end[i].world_cy - g_step_end[i - 1].world_cy);
        snprintf(b, sizeof(b),
                 "%-13s frames=%d cell %d->%d scale=[%.3f..%.3f] settle_f=%d bridged=%d forced=%d uncaptured=%d "
                 "commits=+%d landed=+%d timeouts=+%d external=+%d band_px(L,R,T,B)=%d@%d,%d@%d,%d@%d,%d@%d drift_tiles=%.2f max_ms=%.1f",
                 s.name, n, cell_first, cell_last, smin > 1e8f ? 0.0f : smin, smax, settle, bridged, forced, notcap,
                 commits1 - commits0, landed1 - landed0, to1 - to0, ext1 - ext0,
                 excess[0], excess_frame[0], excess[1], excess_frame[1], excess[2], excess_frame[2], excess[3], excess_frame[3],
                 drift, ms_max);
        const char* level = "OK";
        if (band > cell / 2) level = "FAIL";
        else if (band > 2) level = "WARN";
        if (s.act == Act::Away || s.act == Act::Back) {
            if (cell_first == cell_last) level = "FAIL";          // zoom did not change the level
            if (to1 - to0 > 0) level = "FAIL";
            if (smax < 1.001f && strcmp(level, "FAIL")) level = "WARN";   // no visible easing
            if (drift > 0.75) level = "FAIL";
        }
        if ((s.act == Act::VanAway || s.act == Act::VanBack) && cell_first == cell_last) level = "FAIL";
        if (notcap > n / 10 && i != 2) level = "FAIL";
        line(level, b);
    }
    for (const std::string& p : g_shots) line("INFO", "screenshot " + p);

    snprintf(b, sizeof(b), "RESULT %s fails=%d warns=%d", fails ? "FAIL" : (warns ? "WARN" : "PASS"), fails, warns);
    line("INFO", b);
    const std::string result_line = b;

    rep += "\n== FRAMES ==\n";
    rep += "step          f    ms  cap ch end pas late  map  scale cell narrow vpw/ref     v    tgt baked pend gest  gapL gapR gapT gapB    fx    fy  win\n";
    for (const Rec& r : g_recs) {
        snprintf(b, sizeof(b),
                 "%-12s %3d %5.1f  %d   %d  %c  %2d  %2d %5d %6.3f %4d  %d  %5d/%-5d %6.2f %4d %4d  %d    %d   %4d %4d %4d %4d %5.2f %5.2f %d,%d\n",
                 step_name(r.step), r.f, r.ms, r.ci.started ? 1 : 0, r.ci.choice, r.ci.end_by, r.ci.passes, r.ci.late_passes,
                 r.ci.map_blits, r.ci.scale, r.ci.cell, r.ci.narrow ? 1 : 0, r.ci.vpw, r.ci.refw,
                 r.zi.v, r.zi.target_cell, r.zi.baked_cell, r.zi.pending ? 1 : 0, r.zi.gesture ? 1 : 0,
                 r.gap[0], r.gap[1], r.gap[2], r.gap[3], r.fx, r.fy, r.wx, r.wy);
        rep += b;
    }

    const std::string path = smoothpan_log_path("smoothpan_selftest.txt");
    if (FILE* f = fopen(path.c_str(), "w")) {
        fwrite(rep.data(), 1, rep.size(), f);
        fclose(f);
    }
    g_last_verdict = result_line + " -> " + path;
    g_summary = "SmoothPan selftest finished: " + result_line + "\n  report: " + path + "\n";
    g_recs.clear();
    g_recs.shrink_to_fit();
    g_parity_a.clear();
    g_parity_a.shrink_to_fit();
    g_finished_flag = true;
}

}  // namespace

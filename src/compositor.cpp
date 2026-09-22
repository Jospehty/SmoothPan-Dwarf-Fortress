#define NOMINMAX
#include "compositor.h"

#include "sdl_fn.h"
#include "zoom_camera.h"
#include "camera.h"
#include "viewport.h"
#include "debug_paths.h"
#include "version.h"
#include "frame_seq.h"

#include "Core.h"
#include "Console.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"

#include "platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

extern bool& is_enabled;

namespace {

// ---- SDL entry points resolved from SDL2.dll (not hooked) ------------------
typedef SDL_Texture* (*SP_CreateTexture_t)(SDL_Renderer*, Uint32, int, int, int);
typedef void (*SP_DestroyTexture_t)(SDL_Texture*);
typedef SDL_Texture* (*SP_GetRenderTarget_t)(SDL_Renderer*);
typedef int (*SP_RenderClear_t)(SDL_Renderer*);
typedef int (*SP_SetRenderDrawColor_t)(SDL_Renderer*, Uint8, Uint8, Uint8, Uint8);
typedef int (*SP_GetRenderDrawColor_t)(SDL_Renderer*, Uint8*, Uint8*, Uint8*, Uint8*);
typedef int (*SP_SetTextureBlendMode_t)(SDL_Texture*, SDL_BlendMode);
typedef int (*SP_SetTextureScaleMode_t)(SDL_Texture*, int);   // SDL_ScaleMode (enum, int ABI)
typedef int (*SP_GetRendererInfo_t)(SDL_Renderer*, SDL_RendererInfo*);
typedef void (*SP_RenderGetScale_t)(SDL_Renderer*, float*, float*);
typedef void (*SP_RenderGetViewport_t)(SDL_Renderer*, SDL_Rect*);
typedef void (*SP_RenderGetClipRect_t)(SDL_Renderer*, SDL_Rect*);
typedef SDL_bool (*SP_RenderIsClipEnabled_t)(SDL_Renderer*);
typedef int (*SP_QueryTexture_t)(SDL_Texture*, Uint32*, int*, int*, int*);
typedef const char* (*SP_GetError_t)(void);

struct SdlFns {
    SP_CreateTexture_t CreateTexture = nullptr;
    SP_DestroyTexture_t DestroyTexture = nullptr;
    SP_GetRenderTarget_t GetRenderTarget = nullptr;
    SP_RenderClear_t RenderClear = nullptr;
    SP_SetRenderDrawColor_t SetRenderDrawColor = nullptr;
    SP_GetRenderDrawColor_t GetRenderDrawColor = nullptr;
    SP_SetTextureBlendMode_t SetTextureBlendMode = nullptr;
    SP_SetTextureScaleMode_t SetTextureScaleMode = nullptr;   // optional (SDL >= 2.0.12)
    SP_GetRendererInfo_t GetRendererInfo = nullptr;
    SP_RenderGetScale_t RenderGetScale = nullptr;
    SP_RenderGetViewport_t RenderGetViewport = nullptr;
    SP_RenderGetClipRect_t RenderGetClipRect = nullptr;
    SP_RenderIsClipEnabled_t RenderIsClipEnabled = nullptr;
    SP_QueryTexture_t QueryTexture = nullptr;
    SP_GetError_t GetError = nullptr;                          // optional
    bool resolved = false;
};
static SdlFns fn;

// SDL_ScaleMode values (enum in SDL_render.h >= 2.0.12; use ints for ABI safety).
constexpr int kScaleNearest = 0;
constexpr int kScaleLinear = 1;

// ---- state ------------------------------------------------------------------
struct Layer {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool valid = false;            // holds a complete, displayable map frame
    int cell = 0;                  // px per tile of the bake in this texture
    int gps_z = 0;
    SDL_Rect region = {0, 0, 0, 0};  // map region, DF logical (viewport-relative) coords
    int vp_off_x = 0, vp_off_y = 0;  // DF viewport offset at capture: texture = logical + off
    int count = 0;                 // shifted map blits captured
    int vpw = 0;                   // dim_x * cell at capture
    int frame = 0;
};

static bool g_user_enabled = true;
static bool g_filter_linear = true;
static CompositorState g_state = CompositorState::Init;
static char g_disable_reason[160] = "";
static bool g_init_done = false;
static Uint32 g_tex_format = SDL_PIXELFORMAT_ARGB8888;
static char g_renderer_name[64] = "?";

static Layer g_layers[2];
static int g_cur = 0;                     // index receiving this frame's bake

static SDL_Renderer* g_renderer = nullptr;   // renderer of the current capture
static bool g_frame_started = false;      // a capture happened this frame
static bool g_capturing = false;          // first map pass .. layer end
static bool g_in_pass = false;            // inside a map tile pass
static bool g_layer_done = false;         // composited this frame (HUD phase)
static bool g_bound_ours = false;         // our texture is the live target
static SDL_Texture* g_df_target = nullptr;   // DF's target when capture began
static SDL_Texture* g_df_logical = nullptr;  // DF's most recently requested target
static SDL_Rect g_df_viewport = {0, 0, 0, 0};
static SDL_Rect g_df_clip = {0, 0, 0, 0};
static bool g_df_clip_on = false;
static int g_target_w = 0, g_target_h = 0;

static int g_passes_expected = 1;         // map passes seen last frame (layer-end heuristic)
static int g_frame_passes = 0;
static int g_frame_late_passes = 0;       // passes that began after the layer was composited
static int g_frame_between = 0;           // non-map blits captured between passes
static int g_frame_count_map = 0;
static SDL_Rect g_frame_bbox = {0, 0, 0, 0};
static bool g_frame_bbox_valid = false;
static bool g_frame_narrow = false;
static int g_frame_vpw = 0, g_frame_refw = 0;
static int g_frame_choice = 0;            // 0 none, 1 cur, 2 prev (bridge), 3 cur forced
static bool g_frame_complete = false;
static int g_frame_target_switches = 0;
static char g_frame_end_by = '-';         // 'b' blit, 'f' fill, 'r' render end, 'p' present
static float g_frame_scale = 1.0f;        // transform actually on screen
static float g_frame_ax = 0.0f, g_frame_ay = 0.0f;   // logical coords
static int g_bridge_streak = 0;
static int g_last_complete_count = 0;
static int g_last_complete_cell = 0;
static int g_last_complete_vpw = 0;
static int g_frames_seen = 0;
static int g_bridged_total = 0;
static int g_forced_total = 0;
static int g_composites_total = 0;

constexpr int kMaxBridge = 4;             // consecutive frames we may show the retained bake
constexpr int kProbeFrames = 120;         // per-frame lines written to the log after enable
constexpr int kMaxLogLines = 5000;

static FILE* g_log = nullptr;
static int g_log_lines = 0;
static CompositorFrameInfo g_last_info;

static void cplog(const char* fmt, ...) {
    if (g_log_lines >= kMaxLogLines) return;
    if (!g_log) {
        g_log = fopen(smoothpan_log_path("smoothpan_compositor.txt").c_str(), "a");
        if (!g_log) return;
        fprintf(g_log, "# SmoothPan %s compositor log\n", SMOOTHPAN_BUILD_VERSION);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    g_log_lines++;
}

static void disable(const char* reason) {
    if (g_state == CompositorState::Disabled) return;
    g_state = CompositorState::Disabled;
    snprintf(g_disable_reason, sizeof(g_disable_reason), "%s", reason ? reason : "?");
    cplog("DISABLED: %s", g_disable_reason);
    DFHack::Core::getInstance().getConsole().print(
        "SmoothPan compositor disabled: {} (direct pan path in use; smooth zoom off).\n",
        g_disable_reason);
}

static bool resolve_functions() {
    if (fn.resolved) return true;
    if (!sp_sdl_sym("SDL_CreateTexture")) { disable("SDL2 library not found"); return false; }
    fn.CreateTexture = (SP_CreateTexture_t)sp_sdl_sym("SDL_CreateTexture");
    fn.DestroyTexture = (SP_DestroyTexture_t)sp_sdl_sym("SDL_DestroyTexture");
    fn.GetRenderTarget = (SP_GetRenderTarget_t)sp_sdl_sym("SDL_GetRenderTarget");
    fn.RenderClear = (SP_RenderClear_t)sp_sdl_sym("SDL_RenderClear");
    fn.SetRenderDrawColor = (SP_SetRenderDrawColor_t)sp_sdl_sym("SDL_SetRenderDrawColor");
    fn.GetRenderDrawColor = (SP_GetRenderDrawColor_t)sp_sdl_sym("SDL_GetRenderDrawColor");
    fn.SetTextureBlendMode = (SP_SetTextureBlendMode_t)sp_sdl_sym("SDL_SetTextureBlendMode");
    fn.SetTextureScaleMode = (SP_SetTextureScaleMode_t)sp_sdl_sym("SDL_SetTextureScaleMode");
    fn.GetRendererInfo = (SP_GetRendererInfo_t)sp_sdl_sym("SDL_GetRendererInfo");
    fn.RenderGetScale = (SP_RenderGetScale_t)sp_sdl_sym("SDL_RenderGetScale");
    fn.RenderGetViewport = (SP_RenderGetViewport_t)sp_sdl_sym("SDL_RenderGetViewport");
    fn.RenderGetClipRect = (SP_RenderGetClipRect_t)sp_sdl_sym("SDL_RenderGetClipRect");
    fn.RenderIsClipEnabled = (SP_RenderIsClipEnabled_t)sp_sdl_sym("SDL_RenderIsClipEnabled");
    fn.QueryTexture = (SP_QueryTexture_t)sp_sdl_sym("SDL_QueryTexture");
    fn.GetError = (SP_GetError_t)sp_sdl_sym("SDL_GetError");

    const bool ok = fn.CreateTexture && fn.DestroyTexture && fn.GetRenderTarget &&
                    fn.RenderClear && fn.SetRenderDrawColor && fn.GetRenderDrawColor &&
                    fn.SetTextureBlendMode && fn.GetRendererInfo && fn.RenderGetScale &&
                    fn.RenderGetViewport && fn.RenderGetClipRect && fn.RenderIsClipEnabled &&
                    fn.QueryTexture &&
                    True_SDL_RenderCopyF && True_SDL_RenderSetClipRect &&
                    True_SDL_SetRenderTarget && True_SDL_RenderSetViewport &&
                    GetRendererOutputSize_func;
    if (!ok) { disable("missing SDL2 entry points"); return false; }
    fn.resolved = true;
    return true;
}

static const char* sdl_error() {
    return fn.GetError ? fn.GetError() : "?";
}

static bool ensure_init(SDL_Renderer* r) {
    if (g_state == CompositorState::Disabled) return false;
    if (g_init_done) return true;
    if (!resolve_functions()) return false;

    SDL_RendererInfo info;
    memset(&info, 0, sizeof(info));
    if (fn.GetRendererInfo(r, &info) != 0) { disable("GetRendererInfo failed"); return false; }
    snprintf(g_renderer_name, sizeof(g_renderer_name), "%s", info.name ? info.name : "?");
    if (!(info.flags & SDL_RENDERER_TARGETTEXTURE)) { disable("renderer has no target-texture support"); return false; }
    if (info.num_texture_formats > 0) g_tex_format = info.texture_formats[0];

    int ow = 0, oh = 0;
    GetRendererOutputSize_func(r, &ow, &oh);
    SDL_Texture* t = fn.GetRenderTarget(r);
    cplog("INIT renderer=%s flags=0x%x fmt=0x%x output=%dx%d df_target=%p",
          g_renderer_name, info.flags, g_tex_format, ow, oh, static_cast<void*>(t));
    g_init_done = true;
    g_state = CompositorState::Active;
    DFHack::Core::getInstance().getConsole().print(
        "SmoothPan compositor active (renderer={}, {}x{}, target={}).\n",
        g_renderer_name, ow, oh, t ? "texture" : "backbuffer");
    return true;
}

static bool target_size(SDL_Renderer* r, SDL_Texture* target, int* w, int* h) {
    if (target) {
        Uint32 f = 0; int acc = 0;
        return fn.QueryTexture(target, &f, &acc, w, h) == 0;
    }
    return GetRendererOutputSize_func(r, w, h) == 0;
}

static void destroy_layer(Layer& L) {
    if (L.tex) fn.DestroyTexture(L.tex);
    L.tex = nullptr;
    L.w = L.h = 0;
    L.valid = false;
}

static bool ensure_textures(SDL_Renderer* r, int w, int h) {
    for (int i = 0; i < 2; ++i) {
        Layer& L = g_layers[i];
        if (L.tex && (L.w != w || L.h != h)) {
            cplog("RESIZE layer%d %dx%d -> %dx%d", i, L.w, L.h, w, h);
            destroy_layer(L);
            g_last_complete_count = 0;
            g_last_complete_vpw = 0;
        }
        if (!L.tex) {
            L.tex = fn.CreateTexture(r, g_tex_format, SDL_TEXTUREACCESS_TARGET, w, h);
            if (!L.tex) {
                char buf[160];
                snprintf(buf, sizeof(buf), "CreateTexture %dx%d failed: %s", w, h, sdl_error());
                disable(buf);
                return false;
            }
            fn.SetTextureBlendMode(L.tex, SDL_BLENDMODE_NONE);
            if (fn.SetTextureScaleMode) fn.SetTextureScaleMode(L.tex, kScaleNearest);
            L.w = w; L.h = h;
            L.valid = false;
        }
    }
    return true;
}

// Read DF's viewport/clip from whatever target is current.
static void read_df_state(SDL_Renderer* r) {
    fn.RenderGetViewport(r, &g_df_viewport);
    g_df_clip_on = fn.RenderIsClipEnabled(r) == SDL_TRUE;
    fn.RenderGetClipRect(r, &g_df_clip);
}

// Apply DF's viewport/clip to whatever target is current (SDL keeps these per
// target and resets them on every target switch).
static void apply_df_state(SDL_Renderer* r) {
    True_SDL_RenderSetViewport(r, &g_df_viewport);
    True_SDL_RenderSetClipRect(r, g_df_clip_on ? &g_df_clip : nullptr);
}

static SDL_Rect rect_union(const SDL_Rect& a, const SDL_Rect& b) {
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x1 = std::min(a.x, b.x), y1 = std::min(a.y, b.y);
    int x2 = std::max(a.x + a.w, b.x + b.w), y2 = std::max(a.y + a.h, b.y + b.h);
    return SDL_Rect{ x1, y1, x2 - x1, y2 - y1 };
}

static SDL_Rect rect_intersect(const SDL_Rect& a, const SDL_Rect& b) {
    int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1) return SDL_Rect{ 0, 0, 0, 0 };
    return SDL_Rect{ x1, y1, x2 - x1, y2 - y1 };
}

// Map region this frame, DF logical coords: the tile bake rect (origin grid)
// unioned with everything captured, clamped to the DF viewport.
static SDL_Rect compute_region() {
    SDL_Rect reg = { 0, 0, 0, 0 };
    ViewportRect vp;
    if (get_strict_viewport_rect(&vp)) {
        reg = SDL_Rect{ vp.origin_x, vp.origin_y, vp.right - vp.left, vp.bottom - vp.top };
    }
    if (g_frame_bbox_valid) reg = rect_union(reg, g_frame_bbox);
    SDL_Rect bounds = { 0, 0, g_df_viewport.w, g_df_viewport.h };
    return rect_intersect(reg, bounds);
}

// NARROW = vanilla's 1-present dim lag: dim_x * new_cell falls short of the
// screen AND of the last accepted frame (so a layout where the map is
// legitimately narrower than the window is accepted after one forced frame).
static bool compute_narrow(int target_w) {
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) { g_frame_vpw = 0; g_frame_refw = 0; return false; }
    int ref_w = target_w;
    if (g_df_viewport.w > 0) ref_w = std::min(ref_w, g_df_viewport.w);
    g_frame_vpw = vp.right - vp.left;
    g_frame_refw = ref_w;
    const int half = vp.cell_size / 2;
    if (g_frame_vpw + half >= ref_w) return false;
    if (g_last_complete_vpw > 0 && g_frame_vpw + half >= g_last_complete_vpw) return false;
    return true;
}

static bool count_ok(int count, int cell) {
    // Outside a zoom transition the bake is trusted (count varies legitimately
    // with z-level / map edge).  Inside the window, guard against vanilla's
    // "wide viewport, zero blits" intermediate presents.
    if (g_zoom_transition_frames.load(std::memory_order_relaxed) <= 0) return true;
    if (g_last_complete_count <= 0 || g_last_complete_cell <= 0 || cell <= 0) return count > 0;
    double expected = static_cast<double>(g_last_complete_count) *
                      (static_cast<double>(g_last_complete_cell) * g_last_complete_cell) /
                      (static_cast<double>(cell) * cell);
    return static_cast<double>(count) >= 0.35 * expected;
}

static void composite(SDL_Renderer* r, const Layer& L, const SDL_Rect* cover_also) {
    float s = zoom_camera_scale_for_cell(L.cell);
    float ax = 0.0f, ay = 0.0f;
    zoom_camera_frame_anchor(&ax, &ay);
    // Anchor arrives in window pixels; blits are DF-viewport-relative.
    ax -= static_cast<float>(g_df_viewport.x);
    ay -= static_cast<float>(g_df_viewport.y);

    SDL_Rect reg = L.region;
    if (cover_also) reg = rect_union(reg, *cover_also);
    if (reg.w <= 0 || reg.h <= 0) return;

    SDL_Rect src = { L.region.x + L.vp_off_x, L.region.y + L.vp_off_y, L.region.w, L.region.h };
    SDL_Rect tex_bounds = { 0, 0, L.w, L.h };
    src = rect_intersect(src, tex_bounds);
    if (src.w <= 0 || src.h <= 0) return;

    SDL_FRect dst = { static_cast<float>(src.x - L.vp_off_x), static_cast<float>(src.y - L.vp_off_y),
                      static_cast<float>(src.w), static_cast<float>(src.h) };
    if (s > 1.0001f) {
        dst.x = ax + (dst.x - ax) * s;
        dst.y = ay + (dst.y - ay) * s;
        dst.w *= s;
        dst.h *= s;
    }
    if (cover_also) {
        // Retained bake bridging a partial one: make sure the whole current map
        // region is covered even if the regions differ by an edge strip.
        float rx2 = static_cast<float>(reg.x + reg.w), ry2 = static_cast<float>(reg.y + reg.h);
        if (dst.x > reg.x) { dst.w += dst.x - reg.x; dst.x = static_cast<float>(reg.x); }
        if (dst.y > reg.y) { dst.h += dst.y - reg.y; dst.y = static_cast<float>(reg.y); }
        if (dst.x + dst.w < rx2) dst.w = rx2 - dst.x;
        if (dst.y + dst.h < ry2) dst.h = ry2 - dst.y;
    }

    SDL_Rect clip = reg;
    if (g_df_clip_on) clip = rect_intersect(clip, g_df_clip);
    if (clip.w <= 0 || clip.h <= 0) return;

    // Pixel-exact 1:1 copy at rest (nearest); filtered only while easing.
    if (fn.SetTextureScaleMode)
        fn.SetTextureScaleMode(L.tex, (s > 1.0001f && g_filter_linear) ? kScaleLinear : kScaleNearest);

    True_SDL_RenderSetClipRect(r, &clip);
    True_SDL_RenderCopyF(r, L.tex, &src, &dst);
    True_SDL_RenderSetClipRect(r, g_df_clip_on ? &g_df_clip : nullptr);

    g_frame_scale = s;
    g_frame_ax = ax;
    g_frame_ay = ay;
    g_composites_total++;
}

static void fail_frame(SDL_Renderer* r, const char* why) {
    // Restore DF's target and abandon this frame's capture (direct path shows
    // whatever DF draws next; the map content already captured is lost for one
    // frame, which is why this only happens on real errors).
    if (g_capturing && r) {
        True_SDL_SetRenderTarget(r, g_df_logical);
        apply_df_state(r);
    }
    g_capturing = false;
    g_bound_ours = false;
    g_in_pass = false;
    cplog("frame abandoned: %s", why);
}

static bool bind_capture(SDL_Renderer* r) {
    Layer& cur = g_layers[g_cur];
    if (True_SDL_SetRenderTarget(r, cur.tex) != 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "SetRenderTarget(capture) failed: %s", sdl_error());
        disable(buf);
        return false;
    }
    apply_df_state(r);
    g_bound_ours = true;
    return true;
}

// End the map layer: hand the target back to DF, choose which bake to show,
// draw it, tell the zoom camera what went on screen.
static void layer_end(SDL_Renderer* r, char by) {
    if (!g_capturing) return;
    // DF's latest viewport/clip intent (applied to our texture) goes back to
    // DF's own target.
    if (g_bound_ours) read_df_state(r);
    g_capturing = false;
    g_bound_ours = false;
    g_in_pass = false;
    True_SDL_SetRenderTarget(r, g_df_logical);
    apply_df_state(r);

    Layer& cur = g_layers[g_cur];
    Layer& prev = g_layers[1 - g_cur];
    cur.count = g_frame_count_map;
    cur.region = compute_region();
    cur.vpw = g_frame_vpw;

    const bool complete = !g_frame_narrow && count_ok(cur.count, cur.cell);
    const Layer* show = &cur;
    int choice = 1;
    if (!complete) {
        if (prev.valid && g_bridge_streak < kMaxBridge) { show = &prev; choice = 2; }
        else { choice = 3; }
    }
    g_frame_choice = choice;
    g_frame_complete = complete;
    g_frame_end_by = by;
    g_layer_done = true;

    composite(r, *show, (show != &cur) ? &cur.region : nullptr);
    zoom_camera_on_displayed(show->cell, show == &cur, cur.gps_z);
}

}  // namespace

// ---- public -----------------------------------------------------------------

void compositor_set_enabled(bool enabled) {
    g_user_enabled = enabled;
    if (enabled && g_state == CompositorState::Disabled) {
        // Allow a retry after the user re-enables (e.g. renderer changed).
        g_state = CompositorState::Init;
        g_init_done = false;
        g_disable_reason[0] = '\0';
    }
}
bool compositor_user_enabled() { return g_user_enabled; }
CompositorState compositor_state() { return g_state; }
const char* compositor_state_name() {
    switch (g_state) {
    case CompositorState::Init: return "init";
    case CompositorState::Active: return "active";
    case CompositorState::Disabled: return "disabled";
    }
    return "?";
}
const char* compositor_disable_reason() { return g_disable_reason; }

bool compositor_active() {
    return is_enabled && g_user_enabled && g_state != CompositorState::Disabled;
}
bool compositor_capturing() { return g_capturing; }
bool compositor_layer_done() { return g_frame_started && g_layer_done; }

void compositor_set_filter_linear(bool linear) { g_filter_linear = linear; }
bool compositor_filter_linear() { return g_filter_linear; }

void compositor_reset() {
    g_frame_started = false;
    g_capturing = false;
    g_in_pass = false;
    g_layer_done = false;
    g_bound_ours = false;
    g_frame_choice = 0;
    g_frame_scale = 1.0f;
    g_bridge_streak = 0;
    g_last_complete_count = 0;
    g_last_complete_cell = 0;
    g_last_complete_vpw = 0;
    g_passes_expected = 1;
    g_frames_seen = 0;
    g_bridged_total = 0;
    g_forced_total = 0;
    g_composites_total = 0;
    for (int i = 0; i < 2; ++i) g_layers[i].valid = false;
    if (g_state == CompositorState::Disabled) {
        // Fresh enable: give the compositor another chance.
        g_state = CompositorState::Init;
        g_init_done = false;
        g_disable_reason[0] = '\0';
    }
}

void compositor_pass_begin(void* sdl_renderer, bool is_main_vp) {
    (void)is_main_vp;
    if (!compositor_active() || !sdl_renderer) return;
    if (g_in_pass) return;
    SDL_Renderer* r = reinterpret_cast<SDL_Renderer*>(sdl_renderer);
    if (!ensure_init(r)) return;

    if (g_capturing) {
        // Another map pass while the layer is still open (normal: lower-z
        // siblings, anything DF drew in between is already in the texture).
        if (!g_bound_ours) {
            // DF left its target on some other texture; keep passing through.
        }
        g_in_pass = true;
        g_frame_passes++;
        return;
    }

    SDL_Texture* df_target = fn.GetRenderTarget(r);
    int tw = 0, th = 0;
    if (!target_size(r, df_target, &tw, &th) || tw <= 0 || th <= 0) return;
    if (!ensure_textures(r, tw, th)) return;

    float sx = 1.0f, sy = 1.0f;
    fn.RenderGetScale(r, &sx, &sy);
    if (std::fabs(sx - 1.0f) > 0.001f || std::fabs(sy - 1.0f) > 0.001f) {
        disable("renderer scale != 1 (logical size in use)");
        return;
    }

    read_df_state(r);
    g_renderer = r;
    g_target_w = tw;
    g_target_h = th;
    g_df_target = df_target;
    g_df_logical = df_target;

    Layer& cur = g_layers[g_cur];
    if (!g_frame_started) {
        g_frame_started = true;
        g_layer_done = false;
        g_frame_passes = 0;
        g_frame_late_passes = 0;
        g_frame_between = 0;
        g_frame_count_map = 0;
        g_frame_bbox_valid = false;
        g_frame_bbox = SDL_Rect{ 0, 0, 0, 0 };
        g_frame_choice = 0;
        g_frame_complete = false;
        g_frame_target_switches = 0;
        g_frame_end_by = '-';
        g_frame_scale = 1.0f;

        int zf = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        cur.valid = false;
        cur.cell = zf / 4;
        cur.gps_z = zf;
        cur.vp_off_x = g_df_viewport.x;
        cur.vp_off_y = g_df_viewport.y;
        cur.frame = frame_seq_frame_index();
        cur.region = SDL_Rect{ 0, 0, 0, 0 };
        cur.count = 0;
        g_frame_narrow = compute_narrow(tw);

        if (!bind_capture(r)) return;
        // Fresh layer: opaque black, like DF's cleared backbuffer under the map.
        Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
        fn.GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
        fn.SetRenderDrawColor(r, 0, 0, 0, 255);
        True_SDL_RenderSetClipRect(r, nullptr);
        fn.RenderClear(r);
        fn.SetRenderDrawColor(r, cr, cg, cb, ca);
        apply_df_state(r);
    } else {
        // A map pass after the layer was already composited (more passes than
        // last frame): capture it too; the layer is composited again at the
        // next layer end.  g_passes_expected catches up next frame.
        g_frame_late_passes++;
        g_layer_done = false;
        if (!bind_capture(r)) return;
    }

    g_capturing = true;
    g_in_pass = true;
    g_frame_passes++;
}

void compositor_pass_end(void* sdl_renderer) {
    (void)sdl_renderer;
    // The layer stays open (texture bound) until the HUD starts drawing —
    // see compositor_on_blit / compositor_render_end.
    g_in_pass = false;
}

void compositor_render_end(void* sdl_renderer) {
    if (!g_capturing) return;
    SDL_Renderer* r = sdl_renderer ? reinterpret_cast<SDL_Renderer*>(sdl_renderer) : g_renderer;
    if (!r) return;
    layer_end(r, 'r');
}

bool compositor_on_set_render_target(SDL_Renderer* r, SDL_Texture* requested, int* out_result) {
    if (!g_capturing) return false;
    g_frame_target_switches++;
    g_df_logical = requested;
    if (requested == g_df_target) {
        // DF re-selects its screen target: keep the capture texture bound.
        *out_result = True_SDL_SetRenderTarget(r, g_layers[g_cur].tex);
        apply_df_state(r);
        g_bound_ours = true;
        return true;
    }
    // DF draws into some other texture of its own: pass through.
    g_bound_ours = false;
    return false;
}

void compositor_on_blit(SDL_Renderer* r, float x, float y, float w, float h, bool map_class, bool map_shifted) {
    if (!g_capturing) return;
    if (!g_in_pass && !map_class && g_frame_passes >= g_passes_expected) {
        // First HUD blit after the last expected map pass: the layer is done.
        layer_end(r ? r : g_renderer, 'b');
        return;
    }
    if (!g_bound_ours) return;
    if (!g_in_pass && !map_class) g_frame_between++;
    if (w <= 0.0f || h <= 0.0f) return;
    SDL_Rect b = { static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                   static_cast<int>(std::ceil(w)) + 1, static_cast<int>(std::ceil(h)) + 1 };
    if (!g_frame_bbox_valid) { g_frame_bbox = b; g_frame_bbox_valid = true; }
    else g_frame_bbox = rect_union(g_frame_bbox, b);
    if (map_shifted) g_frame_count_map++;
}

void compositor_on_fill(SDL_Renderer* r, float x, float y, float w, float h) {
    if (!g_capturing) return;
    if (!g_in_pass && g_frame_passes >= g_passes_expected) {
        layer_end(r ? r : g_renderer, 'f');
        return;
    }
    if (!g_bound_ours || w <= 0.0f || h <= 0.0f) return;
    SDL_Rect b = { static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                   static_cast<int>(std::ceil(w)) + 1, static_cast<int>(std::ceil(h)) + 1 };
    if (!g_frame_bbox_valid) { g_frame_bbox = b; g_frame_bbox_valid = true; }
    else g_frame_bbox = rect_union(g_frame_bbox, b);
}

bool compositor_post_blit(SDL_FRect* rect) {
    if (!g_frame_started || !g_layer_done || !rect) return true;
    if (g_frame_choice == 2) return false;   // showing the retained bake
    if (g_frame_scale > 1.0001f) {
        const float s = g_frame_scale;
        rect->x = g_frame_ax + (rect->x - g_frame_ax) * s;
        rect->y = g_frame_ay + (rect->y - g_frame_ay) * s;
        rect->w *= s;
        rect->h *= s;
    }
    return true;
}

void compositor_on_present(SDL_Renderer* r) {
    if (g_capturing) {
        // render END never ran (should not happen) — composite now so the map
        // is not lost, then hand the target back.
        if (r) layer_end(r, 'p');
        else fail_frame(nullptr, "present while capturing, no renderer");
    }
    if (!g_frame_started) {
        g_frame_scale = 1.0f;
        g_last_info = CompositorFrameInfo{};
        zoom_camera_on_present();
        return;
    }
    g_last_info.started = true;
    g_last_info.choice = g_frame_choice;
    g_last_info.complete = g_frame_complete;
    g_last_info.narrow = g_frame_narrow;
    g_last_info.passes = g_frame_passes;
    g_last_info.late_passes = g_frame_late_passes;
    g_last_info.map_blits = g_frame_count_map;
    g_last_info.end_by = g_frame_end_by;
    g_last_info.scale = g_frame_scale;
    g_last_info.cell = g_layers[g_cur].cell;
    g_last_info.vpw = g_frame_vpw;
    g_last_info.refw = g_frame_refw;
    g_last_info.target_switches = g_frame_target_switches;
    Layer& cur = g_layers[g_cur];
    if (g_frame_choice == 1 || g_frame_choice == 3) {
        cur.valid = true;
        if (g_frame_choice == 1 || g_bridge_streak >= kMaxBridge || g_last_complete_count == 0) {
            g_last_complete_count = cur.count;
            g_last_complete_cell = cur.cell;
        }
        g_last_complete_vpw = cur.vpw;
        if (g_frame_choice == 3) g_forced_total++;
        g_bridge_streak = 0;
        g_cur = 1 - g_cur;     // this frame becomes the retained one
    } else {
        g_bridge_streak++;
        g_bridged_total++;
    }
    if (g_frame_passes > 0) g_passes_expected = g_frame_passes;
    g_frames_seen++;
    if (g_frames_seen <= kProbeFrames || g_frame_choice != 1 || g_frame_late_passes > 0 || g_frame_end_by != 'b') {
        cplog("frame=%d passes=%d late=%d between=%d map=%d bbox=(%d,%d,%d,%d) region=(%d,%d,%d,%d) "
              "vpw=%d ref=%d narrow=%d choice=%d complete=%d end=%c streak=%d tsw=%d scale=%.3f cell=%d z=%d "
              "vp=(%d,%d,%d,%d) clip=%d",
              cur.frame, g_frame_passes, g_frame_late_passes, g_frame_between, cur.count,
              g_frame_bbox.x, g_frame_bbox.y, g_frame_bbox.w, g_frame_bbox.h,
              cur.region.x, cur.region.y, cur.region.w, cur.region.h,
              g_frame_vpw, g_frame_refw, g_frame_narrow ? 1 : 0,
              g_frame_choice, g_frame_complete ? 1 : 0, g_frame_end_by, g_bridge_streak,
              g_frame_target_switches, g_frame_scale, cur.cell, cur.gps_z,
              g_df_viewport.x, g_df_viewport.y, g_df_viewport.w, g_df_viewport.h,
              g_df_clip_on ? 1 : 0);
    }
    g_frame_started = false;
    g_layer_done = false;
    zoom_camera_on_present();
}

void compositor_last_frame(CompositorFrameInfo* out) {
    if (out) *out = g_last_info;
}

const char* compositor_renderer_name() { return g_renderer_name; }

void compositor_write_f9(FILE* f) {
    if (!f) return;
    const Layer& cur = g_layers[g_cur];
    fprintf(f, "  Compositor: state=%s%s%s user=%d passes=%d/%d late=%d between=%d map=%d narrow=%d(vpw=%d ref=%d) "
               "choice=%d complete=%d end=%c streak=%d tsw=%d scale=%.3f anchor=(%.0f,%.0f) region=(%d,%d,%d,%d) "
               "lastok=%d@%d bridged=%d forced=%d\n",
            compositor_state_name(),
            g_disable_reason[0] ? " reason=" : "", g_disable_reason,
            g_user_enabled ? 1 : 0,
            g_frame_passes, g_passes_expected, g_frame_late_passes, g_frame_between, g_frame_count_map,
            g_frame_narrow ? 1 : 0, g_frame_vpw, g_frame_refw,
            g_frame_choice, g_frame_complete ? 1 : 0, g_frame_end_by, g_bridge_streak, g_frame_target_switches,
            g_frame_scale, g_frame_ax, g_frame_ay,
            cur.region.x, cur.region.y, cur.region.w, cur.region.h,
            g_last_complete_count, g_last_complete_cell, g_bridged_total, g_forced_total);
}

void compositor_status(char* buf, size_t n) {
    if (!buf || n == 0) return;
    snprintf(buf, n, "compositor=%s%s%s (renderer=%s, %dx%d, frames=%d, composites=%d, bridged=%d, forced=%d, passes/frame=%d, filter=%s)",
             g_user_enabled ? compositor_state_name() : "off",
             g_disable_reason[0] ? " reason=" : "", g_disable_reason,
             g_renderer_name, g_target_w, g_target_h, g_frames_seen, g_composites_total,
             g_bridged_total, g_forced_total, g_passes_expected,
             g_filter_linear ? "linear" : "nearest");
}

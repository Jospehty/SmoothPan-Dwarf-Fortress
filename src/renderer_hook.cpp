#include "renderer_hook.h"

#include "camera.h"
#include "debug_paths.h"
#include "frame_seq.h"
#include "shift_mode.h"
#include "sdl_hook.h"
#include "trace.h"
#include "version.h"

#include "VTableInterpose.h"

#include "df/global_objects.h"
#include "df/enabler.h"
#include "df/graphic.h"
#include "df/graphic_map_portst.h"
#include "df/graphic_viewportst.h"
#include "df/renderer_2d.h"

#include <cstdio>
#include <cstdlib>

using namespace DFHack;

extern bool& is_enabled;

// Set to true for the duration of update_full_viewport on the main map
// viewport, false at all other times.  SDL blit hook reads this to limit
// sub-tile shifting to genuine map tile blits only.
std::atomic<bool> g_in_main_viewport_update{false};
std::atomic<bool> g_in_post_viewport_map_shift{false};

std::atomic<int> g_viewport_pass_index{0};
std::atomic<int> g_cur_pass_dim_x{0};
std::atomic<int> g_cur_pass_dim_y{0};
std::atomic<int> g_cur_pass_screen_x{0};
std::atomic<int> g_cur_pass_screen_y{0};
std::atomic<bool> g_cur_pass_is_map{false};

static int g_renderer_log_frames = 0;
static FILE* g_renderer_log = nullptr;

static bool is_world_viewport_bake_pass(df::graphic_viewportst* vp) {
    if (!vp || !df::global::gps || !df::global::gps->main_viewport) return false;

    auto* main = df::global::gps->main_viewport;
    if (vp->dim_x != main->dim_x || vp->dim_y != main->dim_y) return false;
    if (vp->screen_x == 0 && vp->screen_y == 0) return false;
    // Skip top-row chrome bakes that sit above the map (trace: screen_y=15 vs map top ~33).
    if (vp->screen_y + 8 < main->screen_y) return false;
    return true;
}

// SDL sub-tile shift gate: same-dim world-map passes including lower-z siblings.
// Unlike is_world_viewport_bake_pass, this does NOT reject (0,0) passes or z-level
// bands one row above main->screen_y (3.11.23's +8 rule misclassified those).
static bool is_map_tile_shift_pass(df::graphic_viewportst* vp) {
    if (!vp || !df::global::gps || !df::global::gps->main_viewport) return false;

    auto* main = df::global::gps->main_viewport;
    if (vp->dim_x != main->dim_x || vp->dim_y != main->dim_y) return false;

    int cell = df::global::gps->viewport_zoom_factor / 4;
    if (cell <= 0) cell = 1;

    // Left-edge toolbar row (trace: screen=(2,10) while map uses main->screen_x).
    if (vp->screen_x < cell && vp->screen_y + cell / 2 < main->screen_y)
        return false;

    // Side minimap / off-map panel bakes (trace: screen=(98,89)).
    if (vp->screen_x > main->screen_x + cell * 2 &&
        vp->screen_y >= main->screen_y)
        return false;

    // Top-row sibling bakes above the main map (3.11.23 is_world rule).  Keeps
    // HUD/pop-bar row passes from shifting while still allowing lower-z passes
    // at screen_y within one cell of main->screen_y.
    if (vp->screen_y + 8 < main->screen_y)
        return false;

    return true;
}

static bool viewport_bake_shift_active() {
    return is_enabled && !trace_is_active() && g_shift_mode == ShiftMode::Viewport;
}

static void renderer_log_open_if_needed() {
    if (!g_renderer_log && (g_renderer_log_frames > 0 || trace_is_active())) {
        g_renderer_log = fopen(smoothpan_log_path("smoothpan_renderer.txt").c_str(), "a");
    }
}

static void renderer_log_viewport_shift(df::graphic_viewportst* vp, int saved_x, int saved_y, int applied) {
    if (!applied || (g_renderer_log_frames <= 0 && !trace_is_active())) return;

    renderer_log_open_if_needed();
    if (!g_renderer_log || !vp) return;

    fprintf(g_renderer_log,
            "SMOOTHPAN_%s viewport_bake_shift frame=%d before=(%d,%d) delta=(%d,%d) after=(%d,%d) fx=%.3f fy=%.3f\n",
            SMOOTHPAN_BUILD_VERSION, frame_seq_frame_index(),
            saved_x, saved_y,
            saved_x - vp->screen_x, saved_y - vp->screen_y,
            vp->screen_x, vp->screen_y,
            g_camera.render_frac_x, g_camera.render_frac_y);
    fflush(g_renderer_log);
}

static void renderer_log_line(const char* tag, df::graphic_map_portst* mp, df::graphic_viewportst* vp) {
    if (g_renderer_log_frames <= 0 && !trace_is_active()) return;

    renderer_log_open_if_needed();
    if (!g_renderer_log) return;

    int win_x = df::global::window_x ? *df::global::window_x : -1;
    int win_y = df::global::window_y ? *df::global::window_y : -1;

    fprintf(g_renderer_log,
            "SMOOTHPAN_%s %s frame=%d sdl_seq=%d sdl_rel=%d first_above_rel=%d win=(%d,%d)",
            SMOOTHPAN_BUILD_VERSION, tag,
            frame_seq_frame_index(), frame_seq_global(), frame_seq_relative(),
            frame_seq_first_above_relative(), win_x, win_y);

    if (mp) {
        fprintf(g_renderer_log, " ppc=(%d,%d) dim=(%d,%d) corner=(%d,%d)",
                mp->pixel_perc_x, mp->pixel_perc_y, mp->dim_x, mp->dim_y,
                mp->top_left_corner_x, mp->top_left_corner_y);
    }
    if (vp) {
        fprintf(g_renderer_log, " vp_dim=(%d,%d) vp_screen=(%d,%d)",
                vp->dim_x, vp->dim_y, vp->screen_x, vp->screen_y);
    }

    auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler ? df::global::enabler->renderer : nullptr);
    if (r2d) {
        fprintf(g_renderer_log, " screen_tex=%p origin=(%d,%d) dispx=(%d,%d)",
                r2d->screen_tex, r2d->origin_x, r2d->origin_y, r2d->dispx, r2d->dispy);
    }

    fprintf(g_renderer_log, "\n");
    fflush(g_renderer_log);
}

void renderer_log_start(int frames) {
    g_renderer_log_frames = frames;
    const char* path = smoothpan_log_path("smoothpan_renderer.txt").c_str();
    remove(path);
    g_renderer_log = fopen(path, "w");
    if (g_renderer_log) {
        fprintf(g_renderer_log, "# SmoothPan renderer hook log %s\n", SMOOTHPAN_BUILD_VERSION);
        fprintf(g_renderer_log, "# Logs update_full_map_port / update_full_viewport / render with SDL seq at call time\n\n");
        fclose(g_renderer_log);
        g_renderer_log = nullptr;
    }
}

bool renderer_log_active() {
    return g_renderer_log_frames > 0 || trace_is_active();
}

static void renderer_log_on_present_tick() {
    if (g_renderer_log_frames > 0) {
        g_renderer_log_frames--;
        if (g_renderer_log_frames <= 0 && g_renderer_log) {
            fprintf(g_renderer_log, "\n# === renderer log complete ===\n");
            fclose(g_renderer_log);
            g_renderer_log = nullptr;
        }
    }
}

struct smoothpan_renderer_2d_hook : public df::renderer_2d {
    typedef df::renderer_2d interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, update_full_map_port, (df::graphic_map_portst* vp)) {
        trace_on_update_full_map_port_begin(vp);
        renderer_log_line("update_full_map_port BEGIN", vp, nullptr);
        INTERPOSE_NEXT(update_full_map_port)(vp);
        trace_on_update_full_map_port_end(vp);
        renderer_log_line("update_full_map_port END", vp, nullptr);
    }

    DEFINE_VMETHOD_INTERPOSE(void, update_full_viewport, (df::graphic_viewportst* vp)) {
        trace_on_update_full_viewport_begin(vp);
        renderer_log_line("update_full_viewport BEGIN", nullptr, vp);

        int saved_x = 0;
        int saved_y = 0;
        bool shifted = false;
        if (viewport_bake_shift_active() && is_world_viewport_bake_pass(vp)) {
            saved_x = vp->screen_x;
            saved_y = vp->screen_y;
            vp->screen_x -= g_camera.pixel_shift_x();
            vp->screen_y -= g_camera.pixel_shift_y();
            shifted = true;
            renderer_log_viewport_shift(vp, saved_x, saved_y, 1);
        }

        // Mark map-content passes so the SDL blit hook shifts their tiles.
        // DF runs many update_full_viewport passes per frame (main map, lower
        // z-level show-through, off-map reveal, ...), ALL sharing the main
        // viewport's dim_x/dim_y.  Earlier builds matched only the single main
        // viewport pointer, so sibling map passes (lower z / off-map) rendered
        // UNSHIFTED and visibly misaligned next to the shifted main pass.
        // Gate on dim match instead: every same-dim pass is map content and
        // must shift by the same render_shift to stay aligned.
        auto* main_vp = df::global::gps ? df::global::gps->main_viewport : nullptr;
        bool is_map_vp = (main_vp && vp && (
            vp == main_vp ||
            (vp->dim_x == main_vp->dim_x && vp->dim_y == main_vp->dim_y)
        ));

        // Telemetry: record pass identity for blit attribution.
        g_viewport_pass_index.fetch_add(1, std::memory_order_relaxed);
        if (vp) {
            g_cur_pass_dim_x.store(vp->dim_x, std::memory_order_relaxed);
            g_cur_pass_dim_y.store(vp->dim_y, std::memory_order_relaxed);
            g_cur_pass_screen_x.store(vp->screen_x, std::memory_order_relaxed);
            g_cur_pass_screen_y.store(vp->screen_y, std::memory_order_relaxed);
        }
        g_cur_pass_is_map.store(is_map_vp, std::memory_order_relaxed);

        void* sdl_renderer = nullptr;
        {
            auto* r2d = virtual_cast<df::renderer_2d>(
                df::global::enabler ? df::global::enabler->renderer : nullptr);
            if (r2d) sdl_renderer = r2d->sdl_renderer;
        }

        // Shift gate: same-dim world-map passes (main + lower-z siblings).
        bool shift_pass = is_map_tile_shift_pass(vp);

        if (is_map_vp) {
            // Clip tile baking to the origin rect so shifted tiles can't bleed
            // into the on-screen left/top margin (fixes left/top edge shimmer).
            smoothpan_apply_map_clip(sdl_renderer, true);
        }
        if (shift_pass) {
            g_in_main_viewport_update.store(true, std::memory_order_relaxed);
        }

        INTERPOSE_NEXT(update_full_viewport)(vp);

        if (is_map_vp) {
            smoothpan_apply_map_clip(sdl_renderer, false);
        }
        g_cur_pass_is_map.store(false, std::memory_order_relaxed);

        // Per-pass gate: UI compositing after viewport END must not go through
        // the shift hook (3.11.24 kept the gate open until render → HUD jiggle).
        if (shift_pass) {
            g_in_main_viewport_update.store(false, std::memory_order_relaxed);
            g_in_post_viewport_map_shift.store(true, std::memory_order_relaxed);
        }

        if (shifted) {
            vp->screen_x = saved_x;
            vp->screen_y = saved_y;
        }

        trace_on_update_full_viewport_end(vp);
        renderer_log_line("update_full_viewport END", nullptr, vp);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ()) {
        trace_on_renderer_render_begin();
        renderer_log_line("render BEGIN", nullptr, nullptr);
        INTERPOSE_NEXT(render)();
        g_in_main_viewport_update.store(false, std::memory_order_relaxed);
        g_in_post_viewport_map_shift.store(false, std::memory_order_relaxed);
        trace_on_renderer_render_end();
        renderer_log_line("render END", nullptr, nullptr);
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_renderer_2d_hook, update_full_map_port);
IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_renderer_2d_hook, update_full_viewport);
IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_renderer_2d_hook, render);

bool renderer_hook_install() {
    bool ok = true;
    ok = INTERPOSE_HOOK(smoothpan_renderer_2d_hook, update_full_map_port).apply() && ok;
    ok = INTERPOSE_HOOK(smoothpan_renderer_2d_hook, update_full_viewport).apply() && ok;
    ok = INTERPOSE_HOOK(smoothpan_renderer_2d_hook, render).apply() && ok;
    return ok;
}

void renderer_hook_remove() {
    INTERPOSE_HOOK(smoothpan_renderer_2d_hook, update_full_map_port).remove();
    INTERPOSE_HOOK(smoothpan_renderer_2d_hook, update_full_viewport).remove();
    INTERPOSE_HOOK(smoothpan_renderer_2d_hook, render).remove();
    if (g_renderer_log) {
        fclose(g_renderer_log);
        g_renderer_log = nullptr;
    }
}

void renderer_hook_on_present() {
    renderer_log_on_present_tick();
}

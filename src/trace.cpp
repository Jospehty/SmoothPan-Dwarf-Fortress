#include "trace.h"

#include "debug_paths.h"
#include "frame_seq.h"
#include "renderer_hook.h"
#include "version.h"
#include "viewport.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_map_portst.h"
#include "df/graphic_viewportst.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>

using namespace DFHack;

static int g_trace_frames_remaining = 0;
static int g_trace_frame_index = 0;
static int g_trace_detail_frames = 3;
static int g_dwarf_begin_seq = 0;
static int g_dwarf_end_seq = 0;
static int g_renderer_render_begin_seq = 0;
static int g_renderer_render_end_seq = 0;
static int g_map_port_begin_seq = 0;
static int g_map_port_end_seq = 0;
static int g_viewport_update_begin_seq = 0;
static int g_viewport_update_end_seq = 0;

static int g_blit_total = 0;
static int g_blit_above = 0;
static int g_blit_in_vp = 0;
static int g_blit_below = 0;
static int g_blit_outside = 0;
static int g_blit_tile = 0;
static int g_blit_after_dwarf = 0;
static int g_first_blit_seq = -1;
static int g_last_blit_seq = -1;
static int g_first_in_vp_seq = -1;
static int g_last_in_vp_seq = -1;
static int g_first_above_seq = -1;
static int g_last_above_seq = -1;

static void* g_last_target = nullptr;
static int g_target_changes = 0;
static int g_viewport_events = 0;

static FILE* g_trace_file = nullptr;

static bool trace_detail() {
    return g_trace_frame_index <= g_trace_detail_frames;
}

static void trace_logf(const char* fmt, ...) {
    if (!g_trace_file || !trace_is_active()) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_trace_file, fmt, args);
    va_end(args);
}

static void trace_log_detail(const char* fmt, ...) {
    if (!trace_detail()) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_trace_file, fmt, args);
    va_end(args);
}

static const char* band_name(int above, int in_vp, int below) {
    if (in_vp) return "in_vp";
    if (above) return "above_vp";
    if (below) return "below_vp";
    return "outside";
}

static void trace_reset_frame_counters() {
    g_dwarf_begin_seq = 0;
    g_dwarf_end_seq = 0;
    g_renderer_render_begin_seq = 0;
    g_renderer_render_end_seq = 0;
    g_map_port_begin_seq = 0;
    g_map_port_end_seq = 0;
    g_viewport_update_begin_seq = 0;
    g_viewport_update_end_seq = 0;
    g_blit_total = 0;
    g_blit_above = 0;
    g_blit_in_vp = 0;
    g_blit_below = 0;
    g_blit_outside = 0;
    g_blit_tile = 0;
    g_blit_after_dwarf = 0;
    g_first_blit_seq = -1;
    g_last_blit_seq = -1;
    g_first_in_vp_seq = -1;
    g_last_in_vp_seq = -1;
    g_first_above_seq = -1;
    g_last_above_seq = -1;
    g_target_changes = 0;
    g_viewport_events = 0;
}

bool trace_is_active() {
    return g_trace_frames_remaining > 0;
}

void trace_start(int frames) {
    g_trace_frames_remaining = frames;
    g_trace_frame_index = 0;
    frame_seq_reset_session();
    trace_reset_frame_counters();
    renderer_log_start(frames);

    const char* path = smoothpan_log_path("smoothpan_trace.txt").c_str();
    remove(path);
    g_trace_file = fopen(path, "w");
    if (!g_trace_file) return;

    fprintf(g_trace_file, "# SmoothPan render pipeline trace %s\n", SMOOTHPAN_BUILD_VERSION);
    fprintf(g_trace_file, "# F9 = trace mode: NO SDL shift, NO overscan, NO mapport spoofing (observe vanilla draw order)\n");
    fprintf(g_trace_file, "# Wiggle WASD while tracing. First %d frames include detail markers; all frames get summaries.\n", g_trace_detail_frames);
    ViewportRect vp;
    int zoom = 0;
    if (df::global::gps) zoom = df::global::gps->viewport_zoom_factor;
    if (get_strict_viewport_rect(&vp)) {
        fprintf(g_trace_file, "# viewport=(%d,%d,%d,%d) tile_px=%d zoom=%d\n",
                vp.left, vp.top, vp.right, vp.bottom, vp.tile_px, zoom);
    }
    fprintf(g_trace_file, "# Bands: above_vp=y+h<=top, in_vp=intersects, below_vp=y>=bottom\n");
    fprintf(g_trace_file, "# Key question: seq order of above_vp vs in_vp blits, and blits after dwarfmode render END\n\n");
    fflush(g_trace_file);
}

void trace_on_dwarf_render_begin() {
    if (!trace_is_active()) return;
    g_trace_frame_index++;
    trace_reset_frame_counters();
    g_dwarf_begin_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK dwarfmode_render BEGIN seq=%d win=(%d,%d)\n",
                     g_trace_frame_index, frame_seq_global(),
                     df::global::window_x ? *df::global::window_x : -1,
                     df::global::window_y ? *df::global::window_y : -1);
}

void trace_on_dwarf_render_end() {
    if (!trace_is_active()) return;
    g_dwarf_end_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK dwarfmode_render END seq=%d\n", g_trace_frame_index, frame_seq_global());
}

void trace_on_update_full_map_port_begin(void* vp) {
    if (!trace_is_active()) return;
    g_map_port_begin_seq = frame_seq_global();
    auto* mp = static_cast<df::graphic_map_portst*>(vp);
    trace_log_detail("FRAME %d MARK update_full_map_port BEGIN seq=%d ppc=(%d,%d) dim=(%d,%d) corner=(%d,%d)\n",
                     g_trace_frame_index, frame_seq_global(),
                     mp ? mp->pixel_perc_x : -1, mp ? mp->pixel_perc_y : -1,
                     mp ? mp->dim_x : -1, mp ? mp->dim_y : -1,
                     mp ? mp->top_left_corner_x : -1, mp ? mp->top_left_corner_y : -1);
}

void trace_on_update_full_map_port_end(void* vp) {
    if (!trace_is_active()) return;
    (void)vp;
    g_map_port_end_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK update_full_map_port END seq=%d\n", g_trace_frame_index, frame_seq_global());
}

void trace_on_update_full_viewport_begin(void* vp) {
    if (!trace_is_active()) return;
    g_viewport_update_begin_seq = frame_seq_global();
    auto* vps = static_cast<df::graphic_viewportst*>(vp);
    trace_log_detail("FRAME %d MARK update_full_viewport BEGIN seq=%d dim=(%d,%d) screen=(%d,%d)\n",
                     g_trace_frame_index, frame_seq_global(),
                     vps ? vps->dim_x : -1, vps ? vps->dim_y : -1,
                     vps ? vps->screen_x : -1, vps ? vps->screen_y : -1);
}

void trace_on_update_full_viewport_end(void* vp) {
    if (!trace_is_active()) return;
    (void)vp;
    g_viewport_update_end_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK update_full_viewport END seq=%d\n", g_trace_frame_index, frame_seq_global());
}

void trace_on_renderer_render_begin() {
    if (!trace_is_active()) return;
    g_renderer_render_begin_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK renderer::render BEGIN seq=%d\n", g_trace_frame_index, frame_seq_global());
}

void trace_on_renderer_render_end() {
    if (!trace_is_active()) return;
    g_renderer_render_end_seq = frame_seq_global();
    trace_log_detail("FRAME %d MARK renderer::render END seq=%d\n", g_trace_frame_index, frame_seq_global());
}

void trace_on_set_render_target(void* target) {
    if (!trace_is_active()) return;
    if (target != g_last_target) {
        g_target_changes++;
        trace_log_detail("FRAME %d MARK SetRenderTarget seq=%d target=%p\n", g_trace_frame_index, frame_seq_global(), target);
        g_last_target = target;
    }
}

void trace_on_set_viewport(int x, int y, int w, int h) {
    if (!trace_is_active()) return;
    g_viewport_events++;
    trace_log_detail("FRAME %d MARK SetViewport seq=%d rect=(%d,%d,%d,%d)\n",
                     g_trace_frame_index, frame_seq_global(), x, y, w, h);
}

void trace_on_sdl_blit(int x, int y, int w, int h) {
    if (!trace_is_active()) return;

    const int seq = frame_seq_global();
    g_blit_total++;
    if (g_dwarf_end_seq > 0 && seq > g_dwarf_end_seq) g_blit_after_dwarf++;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return;

    SDL_Rect r = {x, y, w, h};
    bool intersects = !(r.x + r.w <= vp.left || r.x >= vp.right || r.y + r.h <= vp.top || r.y >= vp.bottom);
    bool above = (r.y + r.h <= vp.top);
    bool below = (r.y >= vp.bottom);

    if (intersects) {
        g_blit_in_vp++;
        if (g_first_in_vp_seq < 0) g_first_in_vp_seq = seq;
        g_last_in_vp_seq = seq;
    } else if (above) {
        g_blit_above++;
        if (g_first_above_seq < 0) g_first_above_seq = seq;
        g_last_above_seq = seq;
    } else if (below) {
        g_blit_below++;
    } else {
        g_blit_outside++;
    }

    if (is_tile_sized(w, h, vp.tile_px)) g_blit_tile++;

    if (g_first_blit_seq < 0) g_first_blit_seq = seq;
    g_last_blit_seq = seq;

    if (trace_detail() && (g_blit_total <= 8 || g_blit_total % 500 == 0)) {
        trace_log_detail("  blit seq=%d dst=(%d,%d,%d,%d) band=%s tile=%d\n",
                         seq, x, y, w, h,
                         band_name(above, intersects, below),
                         is_tile_sized(w, h, vp.tile_px) ? 1 : 0);
    }
}

static void trace_write_frame_summary() {
    if (!g_trace_file) return;

    const char* order_hint = "unknown";
    if (g_first_above_seq > 0 && g_first_in_vp_seq > 0) {
        if (g_first_above_seq < g_first_in_vp_seq) order_hint = "above_vp_before_in_vp";
        else if (g_first_in_vp_seq < g_first_above_seq) order_hint = "in_vp_before_above_vp";
    }
    if (g_blit_after_dwarf > 0) {
        order_hint = "post_dwarfmode_sdl_work";
    }

    fprintf(g_trace_file,
            "SUMMARY frame=%d present_seq=%d dwarf=(%d-%d) renderer_render=(%d-%d) "
            "map_port=(%d-%d) full_viewport=(%d-%d) "
            "blits=%d above=%d in_vp=%d below=%d outside=%d tile=%d after_dwarf=%d "
            "first_blit=%d first_above=%d first_in_vp=%d last_in_vp=%d first_above_rel=%d "
            "target_changes=%d sdl_viewport_events=%d order_hint=%s\n",
            g_trace_frame_index, frame_seq_global(),
            g_dwarf_begin_seq, g_dwarf_end_seq,
            g_renderer_render_begin_seq, g_renderer_render_end_seq,
            g_map_port_begin_seq, g_map_port_end_seq,
            g_viewport_update_begin_seq, g_viewport_update_end_seq,
            g_blit_total, g_blit_above, g_blit_in_vp, g_blit_below, g_blit_outside, g_blit_tile,
            g_blit_after_dwarf,
            g_first_blit_seq, g_first_above_seq, g_first_in_vp_seq, g_last_in_vp_seq,
            frame_seq_first_above_relative(),
            g_target_changes, g_viewport_events, order_hint);
    fflush(g_trace_file);
}

void trace_on_present() {
    if (!trace_is_active()) return;

    trace_log_detail("FRAME %d MARK Present seq=%d\n\n", g_trace_frame_index, frame_seq_global());
    trace_write_frame_summary();

    g_trace_frames_remaining--;
    if (g_trace_frames_remaining <= 0) {
        fprintf(g_trace_file, "\n# === trace complete — send smoothpan_trace.txt and smoothpan_renderer.txt ===\n");
        fclose(g_trace_file);
        g_trace_file = nullptr;
    }
}

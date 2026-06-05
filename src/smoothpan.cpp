#include "Console.h"
#include "Core.h"
#include "DataDefs.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include <cstring>

#include "modules/Gui.h"

#include "df/viewscreen_dwarfmodest.h"
#include "df/graphic.h"
#include "df/graphic_map_portst.h"
#include "df/world.h"
#include "df/gamest.h"
#include "df/main_interface.h"

#define NOMINMAX
#include <windows.h>
#undef min
#undef max
#include "camera.h"
#include "sdl_hook.h"
#include "debug_paths.h"
#include "viewport.h"
#include "version.h"
#include "probe.h"
#include "shift_mode.h"
#include "mouse_comp.h"
#include "trace.h"
#include "renderer_hook.h"
#include "perf.h"
#include "ffd_policy.h"
#include "designation_sync.h"
#include "zoom_probe.h"
#include <SDL.h>
#include <cstdio>

using namespace DFHack;

DFHACK_PLUGIN("smoothpan");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);
REQUIRE_GLOBAL(gview);
REQUIRE_GLOBAL(cursor);

static void smoothpan_on_mouse_both_armed() {
    CleanupSDLHooks();
    InitSDLHooks();
    Core::getInstance().getConsole().print("SmoothPan mouse: both (world + UI).\n");
}

static void smoothpan_poll_debug_hotkeys();

static int g_saved_map_port_ppx = 0;
static int g_saved_map_port_ppy = 0;
static bool g_map_port_shift_applied = false;

static void apply_map_port_shift() {
    if (g_shift_mode != ShiftMode::MapPort) return;
    if (!df::global::gps || !df::global::gps->main_map_port) return;

    auto* mp = df::global::gps->main_map_port;
    g_saved_map_port_ppx = mp->pixel_perc_x;
    g_saved_map_port_ppy = mp->pixel_perc_y;
    mp->pixel_perc_x = static_cast<int32_t>(g_camera.render_frac_x * 100.0f);
    mp->pixel_perc_y = static_cast<int32_t>(g_camera.render_frac_y * 100.0f);
    g_camera.last_snapshot.applied_ppc_x = mp->pixel_perc_x;
    g_camera.last_snapshot.applied_ppc_y = mp->pixel_perc_y;
    g_map_port_shift_applied = true;
}

static void restore_map_port_shift() {
    if (!g_map_port_shift_applied) return;
    if (df::global::gps && df::global::gps->main_map_port) {
        auto* mp = df::global::gps->main_map_port;
        mp->pixel_perc_x = g_saved_map_port_ppx;
        mp->pixel_perc_y = g_saved_map_port_ppy;
    }
    g_map_port_shift_applied = false;
}


static bool g_mouse_comp_applied = false;

// ---- comp diagnostics (read by sdl_hook telemetry) ----
int g_sp_comp_reason_feed = 0;
int g_sp_comp_reason_rend = 0;
int g_sp_comp_feed_calls = 0;
int g_sp_comp_rend_calls = 0;
int g_sp_comp_sx = 0, g_sp_comp_sy = 0;
int g_sp_comp_mx_before = 0, g_sp_comp_mx_after = 0;
int g_sp_comp_my_before = 0, g_sp_comp_my_after = 0;
int g_sp_comp_raw_x = 0, g_sp_comp_raw_y = 0;
int g_sp_comp_px = 0, g_sp_comp_py = 0;
int g_sp_comp_inui_reason = 0;
int g_sp_comp_vp_left = 0, g_sp_comp_vp_top = 0;
int g_sp_comp_vp_right = 0, g_sp_comp_vp_bottom = 0;
int g_sp_comp_fsx100 = 0, g_sp_comp_fsy100 = 0, g_sp_comp_cell = 0;
int g_sp_comp_gapx = 0, g_sp_comp_gapy = 0, g_sp_comp_tpx = 0;
int g_sp_comp_tx = 0, g_sp_comp_ty = 0;

int g_sp_gate_sdl_inui = 0;
int g_sp_gate_unified_inui = 0;
int g_sp_gate_mismatch = 0;
int g_sp_gate_pick_x = 0, g_sp_gate_pick_y = 0;

int g_click_count = 0, g_click_reason = 0, g_click_keys = 0;
int g_click_raw_x = 0, g_click_raw_y = 0;
int g_click_mx_before = 0, g_click_mx_after = 0;
int g_click_shift_x = 0, g_click_shift_y = 0;
int g_click_inui_reason = 0;
int g_click_vp_left = 0, g_click_vp_top = 0, g_click_vp_right = 0, g_click_vp_bottom = 0;
int g_click_w_x1 = 0, g_click_w_y1 = 0, g_click_w_x2 = 0, g_click_w_y2 = 0;
int g_click_w_container = 0;

struct SpClickRec {
    int n, rawx, rawy, reason, inui;
    int sx, sy, mxb, mxa, myb, mya, px, py;
    int mz, bm, scroll;
    int cell, fsx100, fsy100, tx, ty;
    int gapx, gapy, tpx;
    int ex, ey, ex_t, ey_t, cx, cy, dx, dy;
    int gate_sdl, gate_unified;
};
static const int SP_CLICK_RING = 6;
SpClickRec g_click_ring[SP_CLICK_RING] = {};
int g_click_ring_pos = 0;

extern int g_matched_widget_x1, g_matched_widget_y1, g_matched_widget_x2, g_matched_widget_y2;
extern int g_matched_widget_is_container;

// ---- Mouse compensation (world hit-test) -------------------------------------
// DF sets two mouse fields with DIFFERENT meanings (see g_src/renderer_2d.hpp
// get_precise_mouse_coords + enabler.cpp event loop):
//   precise_mouse_x/y = SDL pixel offset from renderer origin (origin_x/y)
//   mouse_x/y           = precise / dispx_z  (TEXT-GRID cell index for UI)
// Map/world picking uses precise (+ window_x via getMousePos).  UI uses
// get_mouse_text_coords() → mouse_x/y only.  We therefore bump ONLY
// precise_mouse during feed/logic; never touch mouse_x/y or window_x/y.
// Applied in feed (clicks) and logic (hover tooltip) only.

static int g_comp_px_add = 0, g_comp_py_add = 0;

static void apply_mouse_compensation(char src) {
    int* reason = (src == 'f') ? &g_sp_comp_reason_feed : &g_sp_comp_reason_rend;
    if (src == 'f') g_sp_comp_feed_calls++;
    else g_sp_comp_rend_calls++;

    // Render normally skips GPS comp; allow it only during designation rectangle drag
    // or any placement mode (BUILDING_PLACEMENT / ZONE_PAINT / STOCKPILE_PAINT /
    // BURROW_PAINT / main_designation_selected != NONE) when cursor is over the map.
    if (src == 'r') {
        int uncomp_px = (df::global::gps && df::global::gps->precise_mouse_x >= 0)
            ? df::global::gps->precise_mouse_x : -1;
        int uncomp_py = (df::global::gps && df::global::gps->precise_mouse_y >= 0)
            ? df::global::gps->precise_mouse_y : -1;
        if (!designation_sync_wants_render(uncomp_px, uncomp_py)) { *reason = 8; return; }
    }

    if (g_mouse_comp_applied) { *reason = 7; return; }
    if (!mouse_comp_gps_enabled()) { *reason = 9; return; }
    if (g_shift_mode == ShiftMode::None) { *reason = 2; return; }
    if (!df::global::gps || !df::global::window_x || !df::global::window_y) { *reason = 3; return; }

    auto* gps = df::global::gps;
    g_sp_comp_mx_before = gps->mouse_x;
    g_sp_comp_my_before = gps->mouse_y;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) { *reason = 4; return; }

    if (gps->precise_mouse_x < 0 || gps->precise_mouse_y < 0) { *reason = 6; return; }

    int raw_x = gps->precise_mouse_x + vp.origin_x;
    int raw_y = gps->precise_mouse_y + vp.origin_y;
    g_sp_comp_raw_x = raw_x;
    g_sp_comp_raw_y = raw_y;
    g_sp_comp_px = gps->precise_mouse_x;
    g_sp_comp_py = gps->precise_mouse_y;

    ViewportRect uivp;
    int inui = IsMouseInUI_reason(raw_x, raw_y, &uivp);
    g_sp_comp_inui_reason = inui;
    g_sp_comp_vp_left = uivp.left;
    g_sp_comp_vp_top = uivp.top;
    g_sp_comp_vp_right = uivp.right;
    g_sp_comp_vp_bottom = uivp.bottom;
    if (!mouse_gate_should_compensate(raw_x, raw_y, nullptr)) {
        *reason = 6; return;
    }

    float fsx = g_camera.render_shift_x();
    float fsy = g_camera.render_shift_y();
    int cell = vp.cell_size;
    g_sp_comp_sx = static_cast<int>(std::lround(fsx));
    g_sp_comp_sy = static_cast<int>(std::lround(fsy));
    g_sp_comp_fsx100 = static_cast<int>(std::lround(fsx * 100.0f));
    g_sp_comp_fsy100 = static_cast<int>(std::lround(fsy * 100.0f));
    g_sp_comp_cell = cell;
    g_sp_comp_gapx = vp.left - vp.origin_x;
    g_sp_comp_gapy = vp.top - vp.origin_y;
    g_sp_comp_tpx = gps->tile_pixel_x;
    g_sp_comp_mx_after = gps->mouse_x;
    g_sp_comp_my_after = gps->mouse_y;
    g_sp_comp_tx = *df::global::window_x + gps->precise_mouse_x / cell;
    g_sp_comp_ty = *df::global::window_y + gps->precise_mouse_y / cell;
    if (fsx == 0.0f && fsy == 0.0f) { *reason = 5; return; }

    g_comp_px_add = static_cast<int>(std::lround(fsx));
    g_comp_py_add = static_cast<int>(std::lround(fsy));

    gps->precise_mouse_x += g_comp_px_add;
    gps->precise_mouse_y += g_comp_py_add;
    g_sp_comp_mx_after = gps->mouse_x;   // intentionally unchanged
    g_sp_comp_my_after = gps->mouse_y;
    g_sp_comp_tx = *df::global::window_x + gps->precise_mouse_x / cell;
    g_sp_comp_ty = *df::global::window_y + gps->precise_mouse_y / cell;
    *reason = 1;
    g_mouse_comp_applied = true;
}

static void restore_mouse_compensation() {
    if (!g_mouse_comp_applied) return;
    if (df::global::gps) {
        df::global::gps->precise_mouse_x -= g_comp_px_add;
        df::global::gps->precise_mouse_y -= g_comp_py_add;
    }
    g_comp_px_add = 0;
    g_comp_py_add = 0;
    g_mouse_comp_applied = false;
}

static void uncompensated_precise(int* out_px, int* out_py) {
    int px = -1, py = -1;
    if (df::global::gps && df::global::gps->precise_mouse_x >= 0) {
        px = df::global::gps->precise_mouse_x;
        py = df::global::gps->precise_mouse_y;
        if (g_mouse_comp_applied) {
            px -= g_comp_px_add;
            py -= g_comp_py_add;
        }
    }
    if (out_px) *out_px = px;
    if (out_py) *out_py = py;
}

struct smoothpan_dwarfmode_hook : public df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, feed, (std::set<df::interface_key> *input)) {
        if (!is_enabled) {
            INTERPOSE_NEXT(feed)(input);
            return;
        }

        const bool mmb_held = smoothpan_middle_mouse_button_held();
        const bool map_scrolling = df::global::game &&
            df::global::game->main_interface.mouse_scrolling_map;
        bool mmb_map_grab = false;
        if (df::global::gps && mmb_held) {
            int gate_px = df::global::gps->precise_mouse_x;
            int gate_py = df::global::gps->precise_mouse_y;
            int raw_x = -1, raw_y = -1;
            ViewportRect vp;
            if (smoothpan_raw_sdl_mouse(&raw_x, &raw_y) && get_strict_viewport_rect(&vp)) {
                gate_px = raw_x - vp.origin_x;
                gate_py = raw_y - vp.origin_y;
            }
            mmb_map_grab = smoothpan_middle_mouse_map_gate(gate_px, gate_py);
        }

        if (mmb_held && (g_camera.middle_drag_active || map_scrolling || mmb_map_grab)) {
            input->erase(df::interface_key::CURSOR_UP);
            input->erase(df::interface_key::CURSOR_DOWN);
            input->erase(df::interface_key::CURSOR_LEFT);
            input->erase(df::interface_key::CURSOR_RIGHT);
        } else {
            if (input->count(df::interface_key::CURSOR_UP)) {
                g_camera.panning_up = true;
                input->erase(df::interface_key::CURSOR_UP);
            }
            if (input->count(df::interface_key::CURSOR_DOWN)) {
                g_camera.panning_down = true;
                input->erase(df::interface_key::CURSOR_DOWN);
            }
            if (input->count(df::interface_key::CURSOR_LEFT)) {
                g_camera.panning_left = true;
                input->erase(df::interface_key::CURSOR_LEFT);
            }
            if (input->count(df::interface_key::CURSOR_RIGHT)) {
                g_camera.panning_right = true;
                input->erase(df::interface_key::CURSOR_RIGHT);
            }
        }

        bool comp = is_enabled && !trace_is_active();
        int pending_click_ring_idx = -1;
        if (comp) apply_mouse_compensation('f');

        int uncomp_x = -1, uncomp_y = -1;
        if (comp) {
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_before_vanilla(uncomp_x, uncomp_y);
        }

        if (comp && input && !input->empty()) {
            pending_click_ring_idx = g_click_ring_pos;
            g_click_count++;
            g_click_keys = static_cast<int>(input->size());
            g_click_reason = g_sp_comp_reason_feed;
            g_click_raw_x = g_sp_comp_raw_x;
            g_click_raw_y = g_sp_comp_raw_y;
            g_click_mx_before = g_sp_comp_mx_before;
            g_click_mx_after = g_sp_comp_mx_after;
            g_click_shift_x = static_cast<int>(std::lround(g_camera.render_shift_x()));
            g_click_shift_y = static_cast<int>(std::lround(g_camera.render_shift_y()));
            g_click_inui_reason = g_sp_comp_inui_reason;
            g_click_vp_left = g_sp_comp_vp_left;
            g_click_vp_top = g_sp_comp_vp_top;
            g_click_vp_right = g_sp_comp_vp_right;
            g_click_vp_bottom = g_sp_comp_vp_bottom;
            g_click_w_x1 = g_matched_widget_x1;
            g_click_w_y1 = g_matched_widget_y1;
            g_click_w_x2 = g_matched_widget_x2;
            g_click_w_y2 = g_matched_widget_y2;
            g_click_w_container = g_matched_widget_is_container;

            SpClickRec& rec = g_click_ring[g_click_ring_pos];
            rec.n = g_click_count;
            rec.rawx = g_sp_comp_raw_x;
            rec.rawy = g_sp_comp_raw_y;
            rec.reason = g_sp_comp_reason_feed;
            rec.inui = g_sp_comp_inui_reason;
            rec.sx = g_sp_comp_sx;
            rec.sy = g_sp_comp_sy;
            rec.mxb = g_sp_comp_mx_before;
            rec.mxa = g_sp_comp_mx_after;
            rec.myb = g_sp_comp_my_before;
            rec.mya = g_sp_comp_my_after;
            rec.px = g_sp_comp_px;
            rec.py = g_sp_comp_py;
            rec.cell = g_sp_comp_cell;
            rec.fsx100 = g_sp_comp_fsx100;
            rec.fsy100 = g_sp_comp_fsy100;
            rec.tx = g_sp_comp_tx;
            rec.ty = g_sp_comp_ty;
            rec.gapx = g_sp_comp_gapx;
            rec.gapy = g_sp_comp_gapy;
            rec.tpx = g_sp_comp_tpx;
            rec.ex = 0;
            rec.ey = 0;
            rec.ex_t = 0;
            rec.ey_t = 0;
            rec.cx = 0;
            rec.cy = 0;
            rec.dx = 0;
            rec.dy = 0;
            {
                float fsx = g_camera.render_shift_x();
                float fsy = g_camera.render_shift_y();
                compute_expected_world_tile(rec.px, rec.py, fsx, fsy, &rec.ex, &rec.ey);
                compute_expected_world_tile_trunc_shift(rec.px, rec.py, fsx, fsy, &rec.ex_t, &rec.ey_t);
            }
            rec.gate_unified = g_sp_comp_inui_reason;
            {
                int sdl_x = -1, sdl_y = -1;
                if (smoothpan_raw_sdl_mouse(&sdl_x, &sdl_y)) {
                    rec.gate_sdl = IsMouseInUI_reason(sdl_x, sdl_y, nullptr);
                } else {
                    rec.gate_sdl = -1;
                }
            }
            if (df::global::game) {
                rec.mz = df::global::game->main_interface.mouse_zone;
                rec.bm = static_cast<int>(df::global::game->main_interface.bottom_mode_selected);
                rec.scroll = df::global::game->main_interface.mouse_scrolling_map ? 1 : 0;
            }
            g_click_ring_pos = (g_click_ring_pos + 1) % SP_CLICK_RING;
        }

        INTERPOSE_NEXT(feed)(input);

        if (comp) {
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_after_vanilla(uncomp_x, uncomp_y);
            designation_sync_restore();
        }

        if (comp && pending_click_ring_idx >= 0) {
            SpClickRec& rec = g_click_ring[pending_click_ring_idx];
            if (df::global::cursor) {
                rec.cx = df::global::cursor->x;
                rec.cy = df::global::cursor->y;
                rec.dx = rec.cx - rec.ex;
                rec.dy = rec.cy - rec.ey;
            }
        }

        if (comp) restore_mouse_compensation();
    }


    DEFINE_VMETHOD_INTERPOSE(void, logic, ()) {
        bool comp = is_enabled && !trace_is_active();
        const bool mmb_track = is_enabled && !trace_is_active();
        if (comp) apply_mouse_compensation('l');
        int uncomp_x = -1, uncomp_y = -1;
        if (comp) {
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_before_vanilla(uncomp_x, uncomp_y);
        }
        if (mmb_track)
            smoothpan_middle_mouse_update();
        INTERPOSE_NEXT(logic)();
        if (comp) {
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_after_vanilla(uncomp_x, uncomp_y);
            designation_sync_restore();
        }
        if (mmb_track)
            smoothpan_middle_mouse_update();
        if (comp)
            restore_mouse_compensation();
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t unk)) {
        if (is_enabled) {
            smoothpan_poll_debug_hotkeys();
            if (perf_capture_just_finished()) {
                Core::getInstance().getConsole().print("SmoothPan perf capture complete.\n");
                perf_print_summary(Core::getInstance().getConsole());
                perf_clear_capture_finished();
            }
            g_camera.update();
            g_camera.last_snapshot.applied_ppc_x = -1;
            g_camera.last_snapshot.applied_ppc_y = -1;
            g_camera.begin_render_overscan();
            apply_map_port_shift();
        }

        bool comp_rend = false;
        int uncomp_x = -1, uncomp_y = -1;
        uncompensated_precise(&uncomp_x, &uncomp_y);
        comp_rend = is_enabled && !trace_is_active() && designation_sync_wants_render(uncomp_x, uncomp_y);
        // Capture cursor at start-of-render (before comp).  DF resets
        // df::global::cursor to (-30000) at end of frame, so we must
        // snapshot it here to know where the ghost is anchored.
        if (df::global::cursor && df::global::gps) {
            g_sp_cur_rend_start_x = df::global::cursor->x;
            g_sp_cur_rend_start_y = df::global::cursor->y;
            g_sp_cur_rend_start_z = df::global::cursor->z;
            g_sp_cur_precise_rend_start_x = df::global::gps->precise_mouse_x;
            g_sp_cur_precise_rend_start_y = df::global::gps->precise_mouse_y;
        }
        if (comp_rend) {
            apply_mouse_compensation('r');
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_before_vanilla(uncomp_x, uncomp_y);
        }

        INTERPOSE_NEXT(render)(unk);

        // Capture cursor at end-of-render (after comp, before restore).
        if (df::global::cursor && df::global::gps) {
            g_sp_cur_rend_end_x = df::global::cursor->x;
            g_sp_cur_rend_end_y = df::global::cursor->y;
            g_sp_cur_rend_end_z = df::global::cursor->z;
            g_sp_cur_precise_rend_end_x = df::global::gps->precise_mouse_x;
            g_sp_cur_precise_rend_end_y = df::global::gps->precise_mouse_y;
        }
        if (comp_rend) {
            uncompensated_precise(&uncomp_x, &uncomp_y);
            designation_sync_after_vanilla(uncomp_x, uncomp_y);
            designation_sync_restore();
            restore_mouse_compensation();
        }

        if (is_enabled) {
            restore_map_port_shift();
            g_camera.end_render_overscan();
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, logic);
IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, render);

static void smoothpan_poll_debug_hotkeys() {
    static bool f8_was_down = false;
    static bool f9_was_down = false;
    static bool f10_was_down = false;
    static bool f11_was_down = false;
    static bool f12_was_down = false;
    static bool f7_was_down = false;

    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;

    if (f8 && !f8_was_down) {
        MouseCompMode prev = g_mouse_comp_mode;
        g_mouse_comp_mode = mouse_comp_mode_cycle_next(g_mouse_comp_mode);
        if (g_mouse_comp_mode == MouseCompMode::Both && prev != MouseCompMode::Both) {
            mouse_comp_notify_both_armed();
        } else {
            Core::getInstance().getConsole().print(
                "SmoothPan mouse: gps (map only). F8 again for UI.\n");
        }
    }

    if (f9 && !f9_was_down) {
        smoothpan_arm_capture(30, 0);
    }
    if (f10 && !f10_was_down) {
        g_classify_log_frames = 60;
        g_test_dump_frames = 0;
        g_test_dump_delay = 0;
        std::string log_path = smoothpan_log_path("smoothpan_classify.txt");
        remove(log_path.c_str());
    }
    if (f11 && !f11_was_down) {
        g_probe_frames = 120;
        probe_reset_frame_counters();
        std::string log_path = smoothpan_log_path("smoothpan_probe.txt");
        remove(log_path.c_str());
    }
    if ((f7 && !f7_was_down) || (f12 && !f12_was_down)) {
        perf_start_capture(450);
        remove(smoothpan_log_path("smoothpan_perf.txt").c_str());
        Core::getInstance().getConsole().print(
            "SmoothPan detailed perf: 450 frames -> smoothpan_perf.txt\n"
            "(stand still ~3s, pan WASD ~4s, stop ~3s). Prefer F7 if F12 is screenshot.\n");
    }

    f8_was_down = f8;
    f9_was_down = f9;
    f10_was_down = f10;
    f11_was_down = f11;
    f12_was_down = f12;
    f7_was_down = f7;
}

command_result smoothpan_cmd(color_ostream &out, std::vector<std::string> &parameters) {
    if (parameters.size() >= 2 && parameters[0] == "dump") {
        int frames = std::stoi(parameters[1]);
        int delay = (parameters.size() == 3) ? std::stoi(parameters[2]) : 150;

        smoothpan_arm_capture(frames, delay);

        // Build the actual file name we just armed (matches telemetry_file_name()
        // in sdl_hook.cpp so the message agrees with what the file will be).
        std::string fname;
        if (g_capture_label.empty()) {
            fname = "smoothpan_telemetry.txt";
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "smoothpan_telemetry_%s_%d.txt",
                          g_capture_label.c_str(), g_capture_seq);
            fname = buf;
        }
        out.print("Dumping {} frames after a {} frame delay to {}\n",
                  frames, delay, smoothpan_log_path(fname.c_str()));
    } else if (parameters.size() >= 2 && parameters[0] == "classify") {
        int frames = std::stoi(parameters[1]);
        int delay = (parameters.size() == 3) ? std::stoi(parameters[2]) : 0;

        g_classify_log_frames = frames;
        g_test_dump_delay = delay;

        std::string log_path = smoothpan_log_path("smoothpan_classify.txt");
        remove(log_path.c_str());
        out.print("Logging suspected HUD false positives for {} frames to {}\n", frames, log_path);
    } else if (parameters.size() >= 2 && parameters[0] == "trace") {
        int frames = std::stoi(parameters[1]);
        trace_start(frames);
        out.print("Pipeline trace {} frames (no spoofing). Log: {}\n",
                  frames, smoothpan_log_path("smoothpan_trace.txt"));
    } else if (parameters.size() >= 2 && parameters[0] == "probe") {
        int frames = std::stoi(parameters[1]);
        g_probe_frames = frames;
        probe_reset_frame_counters();
        std::string log_path = smoothpan_log_path("smoothpan_probe.txt");
        remove(log_path.c_str());
        out.print("Probe logging {} frames to {}\n", frames, log_path);
    } else if (parameters.size() == 2 && parameters[0] == "mouse") {
        if (strcmp(parameters[1].c_str(), "sync") == 0
            || strcmp(parameters[1].c_str(), "ui") == 0) {
            if (mouse_comp_arm_both_ui()) {
                out.print("SmoothPan mouse: both (world + UI).\n");
            } else {
                out.print("Mouse already in both mode.\n");
            }
            return CR_OK;
        }
        MouseCompMode mode;
        if (!parse_mouse_comp_mode(parameters[1].c_str(), &mode)) {
            out.print("Unknown mouse mode '{}'. Use: sync, ui, none, sdl, gps, both (or 0-3)\n",
                      parameters[1]);
            return CR_FAILURE;
        }
        MouseCompMode prev = g_mouse_comp_mode;
        g_mouse_comp_mode = mode;
        if (g_mouse_comp_mode == MouseCompMode::Both && prev != MouseCompMode::Both) {
            mouse_comp_notify_both_armed();
        }
        out.print("Mouse compensation set to {} (sdl={}, gps={})\n",
                  mouse_comp_mode_name(g_mouse_comp_mode),
                  mouse_comp_sdl_enabled() ? "on" : "off",
                  mouse_comp_gps_enabled() ? "on" : "off");
    } else if (parameters.size() >= 2 && parameters[0] == "minimap") {
        if (parameters.size() >= 3 && parameters[1] == "interval") {
            int ms = std::stoi(parameters[2]);
            smoothpan_set_minimap_mustmake_interval_ms(ms);
            out.print("Minimap mustmake interval: {}ms (mode={}).\n",
                      smoothpan_minimap_mustmake_interval_ms(),
                      smoothpan_minimap_mode_name());
        } else if (parameters[1] == "lazy") {
            smoothpan_set_minimap_pan_mode("lazy");
            out.print("Minimap on pan: lazy mustmake (~{}ms) + sync on pan stop.\n",
                      smoothpan_minimap_mustmake_interval_ms());
        } else if (parameters[1] == "outline" || parameters[1] == "throttled") {
            smoothpan_set_minimap_pan_mode("outline");
            out.print("Minimap on pan: outline mustmake (~{}ms) + sync on pan stop.\n",
                      smoothpan_minimap_mustmake_interval_ms());
        } else if (parameters[1] == "full") {
            smoothpan_set_minimap_pan_mode("full");
            out.print("Minimap on pan: mustmake every tile step (3.11.35 behavior).\n");
        } else if (parameters[1] == "fast" || parameters[1] == "update") {
            smoothpan_set_minimap_pan_mode("update");
            out.print("Minimap on pan: update only (outline frozen, fastest).\n");
        } else {
            out.print("Unknown minimap mode '{}'. Use: lazy, outline, full, fast\n", parameters[1]);
            out.print("  smoothpan minimap interval <ms> — set lazy/outline throttle (80-60000)\n");
            return CR_FAILURE;
        }
    } else if (parameters.size() >= 2 && parameters[0] == "zoom") {
        out.print("Smooth wheel zoom was removed in 3.20.0 (pan-only). Wheel uses vanilla DF.\n");
        out.print("See docs/ZOOM_POSTMORTEM.md for history.\n");
    } else if (parameters.size() >= 2 && parameters[0] == "clip") {
        if (parameters[1] == "legacy" || parameters[1] == "old") {
            smoothpan_set_legacy_clip(true);
            out.print("Map clip: LEGACY (constrain every frame = 3.20.0 black-band behavior).\n");
            out.print("Use F9 during vanilla zoom to capture the starved baseline, then 'smoothpan clip gated'.\n");
        } else if (parameters[1] == "gated" || parameters[1] == "new") {
            smoothpan_set_legacy_clip(false);
            out.print("Map clip: GATED (3.21.0 fix — relax clip at rest / during zoom transition).\n");
        } else {
            out.print("Map clip mode: {} . Use: legacy | gated\n",
                      smoothpan_legacy_clip() ? "legacy" : "gated");
        }
    } else if (parameters[0] == "zoomcap") {
        // Per-capture labelling + opt-in blit logging.  When a label is set,
        // every F9 / dump writes a fresh, uniquely-numbered file and never
        // overwrites a prior capture.  Stage 0 / STEP 0 of SMOOTH_ZOOM_MASTER_PLAN.md.
        bool show_status = true;
        for (size_t i = 1; i < parameters.size(); ++i) {
            const std::string& a = parameters[i];
            if (a == "blit") {
                if (i + 1 < parameters.size()) {
                    const std::string& v = parameters[++i];
                    if (v == "on" || v == "1") {
                        smoothpan_set_blit_logging(true);
                        out.print("Per-blit telemetry: ON (large files — only when debugging).\n");
                    } else if (v == "off" || v == "0") {
                        smoothpan_set_blit_logging(false);
                        out.print("Per-blit telemetry: OFF (default — ClipStarve suffices for the clip test).\n");
                    } else {
                        out.print("Unknown blit mode '{}'. Use: on | off\n", v);
                        return CR_FAILURE;
                    }
                    show_status = false;
                }
            } else {
                // Treat the first non-keyword arg as a label.
                smoothpan_set_capture_label(a.c_str());
                out.print("Capture label set to '{}'.  Next F9 -> smoothpan_telemetry_{}_{}.txt (auto-incremented).\n",
                          a, a, g_capture_seq + 1);
                show_status = false;
            }
        }
        if (show_status) {
            out.print("Capture label: '{}'  next seq: {}  blit log: {}\n",
                      smoothpan_capture_label(),
                      g_capture_seq + 1,
                      smoothpan_blit_logging() ? "on" : "off");
            if (g_capture_label.empty()) {
                out.print("  WARNING: no label set — captures overwrite 'smoothpan_telemetry.txt' (legacy).\n");
                out.print("  Use 'smoothpan zoomcap <label>' (e.g. 'baseline', 'gated_in') before each capture.\n");
            }
        }
    } else if (parameters.size() >= 2 && parameters[0] == "ffd") {
        if (strcmp(parameters[1].c_str(), "always") == 0 ||
            strcmp(parameters[1].c_str(), "smart") == 0 ||
            strcmp(parameters[1].c_str(), "off") == 0) {
            ffd_policy_set_mode(parameters[1].c_str());
            out.print("Pan ffd policy: {} (tile-step ffd=1 unchanged).\n",
                      ffd_policy_mode_name(ffd_policy_get_mode()));
        } else {
            out.print("Unknown ffd mode '{}'. Use: always, smart, off\n", parameters[1]);
            return CR_FAILURE;
        }
    } else if (parameters.size() >= 1 && parameters[0] == "perf") {
        if (parameters.size() >= 2 && parameters[1] == "summary") {
            perf_print_summary(out);
        } else {
            int frames = (parameters.size() >= 2) ? std::stoi(parameters[1]) : 450;
            perf_start_capture(frames);
            remove(smoothpan_log_path("smoothpan_perf.txt").c_str());
            out.print("Detailed perf capture {} frames -> {}\n",
                      frames, smoothpan_log_path("smoothpan_perf.txt"));
            out.print("Protocol: stand still ~3s, pan WASD ~4s, stop ~3s.\n");
        }
    } else if (parameters.size() >= 2 && parameters[0] == "mode") {
        ShiftMode mode;
        if (!parse_shift_mode(parameters[1].c_str(), &mode)) {
            out.print("Unknown mode '{}'. Use: sdl, none, mapport, seqrange, viewport\n", parameters[1]);
            return CR_FAILURE;
        }
        g_shift_mode = mode;
        out.print("Shift mode set to {}\n", shift_mode_name(g_shift_mode));
    } else {
        out.print("Usage:\n");
        out.print("  smoothpan dump <frames> [delay]     — blit telemetry + BMPs\n");
        out.print("  smoothpan classify <frames> [delay]\n");
        out.print("  smoothpan probe <frames>            — compact per-frame probe log\n");
        out.print("  smoothpan perf [frames|summary]     — detailed FPS capture (F7/F12=450)\n");
        out.print("  smoothpan mode <sdl|none|mapport>   — A/B shift backend\n");
        out.print("  smoothpan mouse <sync|ui|none|sdl|gps|both> — mouse layers\n");
        out.print("  smoothpan minimap <lazy|outline|full|fast> — minimap cost on pan\n");
        out.print("  smoothpan minimap interval <ms>           — lazy/outline throttle\n");
        out.print("  smoothpan ffd <always|smart|off>       — pan z-rebake policy\n");
        out.print("  smoothpan clip <legacy|gated>          — zoom black-band A/B (gated=fix)\n");
        out.print("  smoothpan zoomcap [<label>] [blit on|off]  — per-capture label + blit log\n");
        out.print("Hotkeys: F7/F12=perf, F8=mouse, F9=dump, F10=classify, F11=probe\n");
        out.print("Shift mode: {}\n", shift_mode_name(g_shift_mode));
        out.print("Pan ffd policy: {}\n", ffd_policy_mode_name(ffd_policy_get_mode()));
        out.print("Minimap pan: {} (mustmake every {}ms + on pan stop)\n",
                  smoothpan_minimap_mode_name(),
                  smoothpan_minimap_mustmake_interval_ms());
        out.print("Mouse: {} (sdl={}, gps={})\n",
                  mouse_comp_effective_name(),
                  mouse_comp_sdl_enabled() ? "on" : "off",
                  mouse_comp_gps_enabled() ? "on" : "off");
        out.print("Zoom: vanilla (smooth wheel zoom removed in 3.20.0)\n");
        out.print("Map clip: {} (gated=3.21.0 fix for vanilla-zoom black bands)\n",
                  smoothpan_legacy_clip() ? "legacy" : "gated");
        out.print("Capture label: '{}'  next seq: {}  blit log: {}\n",
                  smoothpan_capture_label(),
                  g_capture_seq + 1,
                  smoothpan_blit_logging() ? "on" : "off");
    }
    return CR_OK;
}

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    (void)SMOOTHPAN_BUILD_TAG;
    commands.push_back(PluginCommand("smoothpan", "SmoothPan debugging commands", smoothpan_cmd));
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (enable) {
        mouse_comp_set_both_armed_handler(smoothpan_on_mouse_both_armed);
        if (!is_enabled) {
            is_enabled = true;
            g_camera.reset();
            zoom_probe_reset();
            g_shift_mode = ShiftMode::Sdl;
            InitSDLHooks();
            renderer_hook_install();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).apply();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, logic).apply();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).apply();
            out.print("SmoothPan {} enabled. WASD pan.\n", SMOOTHPAN_BUILD_VERSION);
            out.print("Mouse: gps (map clicks). F8 once if toolbar/UI feels wrong.\n");
        }
        mouse_comp_boot_gps();
    } else if (is_enabled) {
        is_enabled = false;
        restore_map_port_shift();
        g_camera.end_render_overscan();
        renderer_hook_remove();
        CleanupSDLHooks();
        INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
        INTERPOSE_HOOK(smoothpan_dwarfmode_hook, logic).remove();
        INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
        out.print("SmoothPan disabled. Reverting to native camera.\n");
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    if (is_enabled) {
        restore_map_port_shift();
        g_camera.end_render_overscan();
        renderer_hook_remove();
        CleanupSDLHooks();
    }
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, logic).remove();
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
    return CR_OK;
}

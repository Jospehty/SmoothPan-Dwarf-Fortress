#include "Core.h"

#include "Console.h"

#include "Core.h"

#include "DataDefs.h"

#include "Export.h"

#include "PluginManager.h"

#include "VTableInterpose.h"



#include "modules/Gui.h"



#include "df/viewscreen_dwarfmodest.h"

#include "df/graphic.h"

#include "df/graphic_map_portst.h"

#include "df/world.h"



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

#include "trace.h"

#include "shift_mode.h"

#include "renderer_hook.h"

#include <SDL.h>

#include <cstdio>

#include <cmath>



using namespace DFHack;



DFHACK_PLUGIN("smoothpan");

DFHACK_PLUGIN_IS_ENABLED(is_enabled);



REQUIRE_GLOBAL(window_x);

REQUIRE_GLOBAL(window_y);

REQUIRE_GLOBAL(window_z);

REQUIRE_GLOBAL(gview);



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







struct smoothpan_dwarfmode_hook : public df::viewscreen_dwarfmodest {

    typedef df::viewscreen_dwarfmodest interpose_base;



    DEFINE_VMETHOD_INTERPOSE(void, feed, (std::set<df::interface_key> *input)) {

        if (!is_enabled) {

            INTERPOSE_NEXT(feed)(input);

            return;

        }



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



        INTERPOSE_NEXT(feed)(input);

    }







    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t unk)) {

        if (is_enabled) {

            smoothpan_poll_debug_hotkeys();

            g_camera.update();

            if (!trace_is_active()) {

                g_camera.last_snapshot.applied_ppc_x = -1;

                g_camera.last_snapshot.applied_ppc_y = -1;

                g_camera.begin_render_overscan();

                apply_map_port_shift();

            }

            trace_on_dwarf_render_begin();

        }

        INTERPOSE_NEXT(render)(unk);

        if (is_enabled) {

            trace_on_dwarf_render_end();

            if (!trace_is_active()) {

                restore_map_port_shift();

                // Restore window_x/y immediately so DF sees the real tile
                // coordinate, but keep overscan_tiles state alive so the SDL
                // blits (which fire later in the same frame) get the correct
                // render_shift_x compensation and clip-rect expansion.
                // end_render_overscan() is called from Hook_SDL_RenderPresent.
                g_camera.restore_window_overscan();

            }

        }

    }

};



IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, feed);



IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, render);



static void smoothpan_poll_debug_hotkeys() {
    static bool f9_was_down = false;
    static bool f10_was_down = false;

    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

    if (f9 && !f9_was_down) {
        if (!trace_is_active() && g_test_dump_frames <= 0) {
            // First press: 10-frame telemetry+BMP dump (shift is active, EdgeScan/Fill logged).
            // Trace is deliberately NOT started here — trace mode disables all blit
            // processing (process_map_blit returns false immediately), which would give
            // blits=0 in the telemetry.  Use "smoothpan trace N" in the console if
            // pipeline order info is separately needed.
            g_test_dump_frames = 10;
            g_test_dump_delay  = 0;
            std::string tel_path = smoothpan_log_path("smoothpan_telemetry.txt");
            remove(tel_path.c_str());
        } else if (trace_is_active()) {
            // Already tracing — do nothing (don't reset mid-trace).
        } else {
            // Telemetry already running — also kick off a pipeline trace (60fr).
            trace_start(60);
        }
    }
    if (f10 && !f10_was_down && !trace_is_active()) {
        g_shift_mode = shift_mode_cycle_next(g_shift_mode);
        Core::getInstance().getConsole().print("SmoothPan shift mode: {}\n", shift_mode_name(g_shift_mode));
    }

    f9_was_down = f9;
    f10_was_down = f10;
}



command_result smoothpan_cmd(color_ostream &out, std::vector<std::string> &parameters) {

    if (parameters.size() >= 2 && parameters[0] == "dump") {

        int frames = std::stoi(parameters[1]);

        int delay = (parameters.size() == 3) ? std::stoi(parameters[2]) : 150;



        g_test_dump_frames = frames;

        g_test_dump_delay = delay;



        std::string log_path = smoothpan_log_path("smoothpan_telemetry.txt");

        remove(log_path.c_str());

        out.print("Dumping {} frames after a {} frame delay to {}\n", frames, delay, log_path);

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
        if (parameters[1] == "auto") {
            probe_start_auto_cycle();
            out.print("Auto cycle started (none -> sdl -> mapport). Keep panning. Log: {}\n",
                      smoothpan_log_path("smoothpan_probe.txt"));
        } else {
            int frames = std::stoi(parameters[1]);
            g_probe_frames = frames;
            probe_reset_frame_counters();
            std::string log_path = smoothpan_log_path("smoothpan_probe.txt");
            remove(log_path.c_str());
            out.print("Probe logging {} frames to {}\n", frames, log_path);
        }

    } else if (parameters.size() == 2 && parameters[0] == "mode") {

        ShiftMode mode;

        if (!parse_shift_mode(parameters[1].c_str(), &mode)) {

            out.print("Unknown mode '{}'. Use: sdl, none, mapport, seqrange, viewport\n", parameters[1]);

            return CR_FAILURE;

        }

        g_shift_mode = mode;

        out.print("Shift mode set to {}\n", shift_mode_name(g_shift_mode));

    } else {

        out.print("Usage:\n");

        out.print("  smoothpan trace <frames>             — pipeline trace (same as F9)\n");
        out.print("  smoothpan probe auto               — shift mode A/B cycle\n");
        out.print("  smoothpan probe <frames>           — single-mode probe log\n");
        out.print("  smoothpan dump <frames> [delay]    — heavy blit telemetry + BMPs\n");
        out.print("  smoothpan classify <frames> [delay]\n");
        out.print("  smoothpan mode <sdl|none|mapport|seqrange|viewport>  — manual shift backend\n");
        out.print("Hotkeys: F9=pipeline trace (60fr), F10=cycle sdl/seqrange/viewport\n");

        out.print("Current mode: {}\n", shift_mode_name(g_shift_mode));

    }

    return CR_OK;

}



DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {

    (void)SMOOTHPAN_BUILD_TAG;

    commands.push_back(PluginCommand("smoothpan", "SmoothPan debugging commands", smoothpan_cmd));

    return CR_OK;

}



DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {

    if (enable != is_enabled) {

        is_enabled = enable;

        if (enable) {

            g_camera.reset();

            g_shift_mode = ShiftMode::Sdl;

            InitSDLHooks();

            if (!renderer_hook_install()) {
                out.print("Warning: renderer_2d vtable hooks failed (trace markers may be incomplete).\n");
            }

            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).apply();

            

            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).apply();

            out.print("SmoothPan {} enabled. WASD pan. F9=trace (60fr). F10=cycle shift mode. Log: {}\n",

                      SMOOTHPAN_BUILD_VERSION,

                      smoothpan_log_path("smoothpan_trace.txt"));

        } else {

            restore_map_port_shift();

            g_camera.restore_window_overscan();

            g_camera.end_render_overscan();

            CleanupSDLHooks();

            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();

            

            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();

            renderer_hook_remove();

            out.print("SmoothPan disabled. Reverting to native camera.\n");

        }

    }

    return CR_OK;

}



DFhackCExport command_result plugin_shutdown(color_ostream &out) {

    if (is_enabled) {

        restore_map_port_shift();

        g_camera.restore_window_overscan();

        g_camera.end_render_overscan();

        CleanupSDLHooks();

    }

    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();

    

    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();

    renderer_hook_remove();

    return CR_OK;

}


#include "Console.h"
#include "Core.h"
#include "DataDefs.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"

#include "df/viewscreen_dwarfmodest.h"
#include "df/graphic.h"

#include "df/world.h"

#define NOMINMAX
#include <windows.h>
#undef min
#undef max
#include "camera.h"
#include "sdl_hook.h"

using namespace DFHack;

DFHACK_PLUGIN("smoothpan");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);

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
        
        if (input->count(df::interface_key::ZOOM_IN)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                if (df::global::window_z && df::global::world) {
                    if (*df::global::window_z < df::global::world->map.z_count - 1) (*df::global::window_z)++;
                }
            } else {
                g_camera.zoom_in();
            }
            input->erase(df::interface_key::ZOOM_IN);
        }
        if (input->count(df::interface_key::ZOOM_OUT)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                if (df::global::window_z) {
                    if (*df::global::window_z > 0) (*df::global::window_z)--;
                }
            } else {
                g_camera.zoom_out();
            }
            input->erase(df::interface_key::ZOOM_OUT);
        }

        // Dwarf Fortress often binds scroll wheel to Z-levels
        if (input->count(df::interface_key::CURSOR_UP_Z)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                // Let native Z-level up happen
            } else {
                g_camera.zoom_in();
                input->erase(df::interface_key::CURSOR_UP_Z);
            }
        }
        if (input->count(df::interface_key::CURSOR_DOWN_Z)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                // Let native Z-level down happen
            } else {
                g_camera.zoom_out();
                input->erase(df::interface_key::CURSOR_DOWN_Z);
            }
        }

        // Dwarf Fortress sometimes binds scroll wheel to STANDARDSCROLL
        if (input->count(df::interface_key::STANDARDSCROLL_UP)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                if (df::global::window_z && df::global::world) {
                    if (*df::global::window_z < df::global::world->map.z_count - 1) (*df::global::window_z)++;
                }
            } else {
                g_camera.zoom_in();
            }
            input->erase(df::interface_key::STANDARDSCROLL_UP);
        }
        if (input->count(df::interface_key::STANDARDSCROLL_DOWN)) {
            if (GetAsyncKeyState(VK_MENU) & 0x8000) {
                if (df::global::window_z) {
                    if (*df::global::window_z > 0) (*df::global::window_z)--;
                }
            } else {
                g_camera.zoom_out();
            }
            input->erase(df::interface_key::STANDARDSCROLL_DOWN);
        }
        


        INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t unk)) {
        if (is_enabled) {
            g_camera.update();
        }
        INTERPOSE_NEXT(render)(unk);
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(smoothpan_dwarfmode_hook, render);

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (enable != is_enabled) {
        is_enabled = enable;
        if (enable) {
            g_camera.reset();
            InitSDLHooks();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).apply();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).apply();
            out.print("SmoothPan enabled. Try using WASD to pan the map.\n");
        } else {
            CleanupSDLHooks();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
            out.print("SmoothPan disabled. Reverting to native camera.\n");
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    if (is_enabled) CleanupSDLHooks();
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
    return CR_OK;
}

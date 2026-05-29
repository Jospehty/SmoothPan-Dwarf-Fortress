#include "Console.h"
#include "Core.h"
#include "DataDefs.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"

#include "df/viewscreen_dwarfmodest.h"
#include "df/graphic.h"

using namespace DFHack;

DFHACK_PLUGIN("smoothpan");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);
REQUIRE_GLOBAL(gps);

struct smoothpan_dwarfmode_hook : public df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, feed, (std::set<df::interface_key> *input)) {
        if (!is_enabled) {
            INTERPOSE_NEXT(feed)(input);
            return;
        }

        if (input->count(df::interface_key::CURSOR_UP)) {
            Core::getInstance().getConsole().print("SmoothPan: intercepted CURSOR_UP\n");
            input->erase(df::interface_key::CURSOR_UP);
        }
        if (input->count(df::interface_key::CURSOR_DOWN)) {
            Core::getInstance().getConsole().print("SmoothPan: intercepted CURSOR_DOWN\n");
            input->erase(df::interface_key::CURSOR_DOWN);
        }
        if (input->count(df::interface_key::CURSOR_LEFT)) {
            Core::getInstance().getConsole().print("SmoothPan: intercepted CURSOR_LEFT\n");
            input->erase(df::interface_key::CURSOR_LEFT);
        }
        if (input->count(df::interface_key::CURSOR_RIGHT)) {
            Core::getInstance().getConsole().print("SmoothPan: intercepted CURSOR_RIGHT\n");
            input->erase(df::interface_key::CURSOR_RIGHT);
        }

        INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t unk)) {
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
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).apply();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).apply();
            out.print("SmoothPan enabled.\n");
        } else {
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
            INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
            out.print("SmoothPan disabled.\n");
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, feed).remove();
    INTERPOSE_HOOK(smoothpan_dwarfmode_hook, render).remove();
    return CR_OK;
}

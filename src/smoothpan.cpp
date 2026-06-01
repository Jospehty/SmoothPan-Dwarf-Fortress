#include <SDL.h>
#include "Console.h"
#include "Core.h"
#include "DataDefs.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"

#include "df/viewscreen_dwarfmodest.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/world.h"
#include "df/widget.h"
#include "df/widget_container.h"
#include "df/interfacest.h"
#include "df/gamest.h"
#include "df/main_interface.h"
#include "df/info_interfacest.h"

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
REQUIRE_GLOBAL(gview);

// UI hit-testing function
static bool check_widget_visible_at(df::widget* w, int mx, int my) {
    if (!w) return false;
    if (!w->flag.bits.VISIBILITY_VISIBLE) return false;

    if (mx >= w->rect.x1 && mx <= w->rect.x2 &&
        my >= w->rect.y1 && my <= w->rect.y2) {
        return true;
    }

    df::widget_container* container = virtual_cast<df::widget_container>(w);
    if (container) {
        for (auto& child : container->children) {
            if (check_widget_visible_at(child.get(), mx, my)) {
                return true;
            }
        }
    }
    return false;
}

static bool check_widget_intersects(df::widget* w, const SDL_Rect* r) {
    if (!w) return false;
    if (!w->flag.bits.VISIBILITY_VISIBLE) return false;

    if (!(r->x > w->rect.x2 || r->x + r->w < w->rect.x1 ||
          r->y > w->rect.y2 || r->y + r->h < w->rect.y1)) {
        return true;
    }

    df::widget_container* container = virtual_cast<df::widget_container>(w);
    if (container) {
        for (auto& child : container->children) {
            if (check_widget_intersects(child.get(), r)) {
                return true;
            }
        }
    }
    return false;
}

bool IsRectInUI(const SDL_Rect* r) {
    if (!r) return false;

    // Check native widget tree (standard DFHack/DF overlays)
    if (df::global::gview) {
        df::viewscreen* vs = &df::global::gview->view;
        while (vs->child) vs = vs->child;
        if (check_widget_intersects(&vs->widgets, r)) {
            return true;
        }
    }

    // Check Premium DF's main_interface panels that overlay the map
    if (df::global::game) {
        auto& mi = df::global::game->main_interface;
        if (mi.info.open) {
            if (!(r->x > mi.info.rect.x2 || r->x + r->w < mi.info.rect.x1 ||
                  r->y > mi.info.rect.y2 || r->y + r->h < mi.info.rect.y1)) {
                return true;
            }
        }
    }

    return false;
}

bool IsMouseInUI(int real_mx, int real_my) {
    if (!df::global::gps || !df::global::gps->main_viewport) return false;

    // 1. Anything outside the map viewport is UI (toolbars, sidebars, etc.)
    auto vp = df::global::gps->main_viewport;
    int z = df::global::gps->viewport_zoom_factor;
    // dim_x/dim_y are in text-grid cells (4 cells per graphical tile)
    int cell_size = z / 4;
    int vp_left   = vp->screen_x;
    int vp_top    = vp->screen_y;
    int vp_right  = vp_left + vp->dim_x * cell_size;
    int vp_bottom = vp_top  + vp->dim_y * cell_size;

    if (real_mx < vp_left || real_mx > vp_right ||
        real_my < vp_top  || real_my > vp_bottom) {
        return true;
    }

    // 2. Check native widget tree (standard DFHack/DF overlays)
    if (df::global::gview) {
        df::viewscreen* vs = &df::global::gview->view;
        while (vs->child) vs = vs->child;
        if (check_widget_visible_at(&vs->widgets, real_mx, real_my)) {
            return true;
        }
    }

    // 3. Check Premium DF's main_interface panels that overlay the map
    if (df::global::game) {
        auto& mi = df::global::game->main_interface;
        if (mi.info.open) {
            if (real_mx >= mi.info.rect.x1 && real_mx <= mi.info.rect.x2 &&
                real_my >= mi.info.rect.y1 && real_my <= mi.info.rect.y2) {
                return true;
            }
        }
    }

    return false;
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

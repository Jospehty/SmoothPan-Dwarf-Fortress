#include "designation_sync.h"

#include "modules/Gui.h"
#include "viewport.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/gamest.h"
#include "df/main_interface.h"
#include "df/main_designation_type.h"
#include "df/main_bottom_mode_type.h"
#include "df/civzone_interfacest.h"
#include "df/stockpile_interfacest.h"
#include "df/burrow_interfacest.h"

using namespace DFHack;

int g_sp_desig_active = 0;
int g_sp_desig_patched_mx = 0;
int g_sp_desig_patched_my = 0;
int g_sp_desig_sel_sx = 0, g_sp_desig_sel_sy = 0, g_sp_desig_sel_sz = 0;
int g_sp_desig_sel_ex = 0, g_sp_desig_sel_ey = 0, g_sp_desig_sel_ez = 0;
int g_sp_desig_mpos_x = 0, g_sp_desig_mpos_y = 0;
int g_sp_cur_rend_start_x = -30000, g_sp_cur_rend_start_y = -30000, g_sp_cur_rend_start_z = -30000;
int g_sp_cur_rend_end_x   = -30000, g_sp_cur_rend_end_y   = -30000, g_sp_cur_rend_end_z   = -30000;
int g_sp_cur_precise_rend_start_x = 0, g_sp_cur_precise_rend_start_y = 0;
int g_sp_cur_precise_rend_end_x   = 0, g_sp_cur_precise_rend_end_y   = 0;
int g_sp_wants_render_last = -1;
int g_sp_wants_render_drag = -1;
int g_sp_wants_render_placement = -1;
int g_sp_wants_render_overmap = -1;
int g_sp_wants_render_ux = 0, g_sp_wants_render_uy = 0;
int g_sp_overmap_inui = -1;
int g_sp_overmap_widget = -1;
int g_sp_overmap_gate = -1;
int g_sp_overmap_rawx = 0, g_sp_overmap_rawy = 0;
int g_sp_desig_paint = 0;
int g_sp_desig_drag = 0;

static int g_saved_mx = 0;
static int g_saved_my = 0;
static bool g_mouse_patched = false;
static int g_prev_start_x = -30000;

static bool selection_rect_live() {
    return df::global::selection_rect && df::global::selection_rect->start_x > -30000;
}

static bool rectangle_drag_flag() {
    if (!df::global::game) return false;
    auto& mi = df::global::game->main_interface;
    if (mi.main_designation_doing_rectangles) return true;
    if (mi.civzone.doing_rectangle) return true;
    if (mi.stockpile.doing_rectangle) return true;
    if (mi.burrow.doing_rectangle) return true;
    return false;
}

static bool real_rectangle_drag() {
    return rectangle_drag_flag() && selection_rect_live();
}

// Forward decl: defined below; consulted by designation_sync_wants_render.
static bool designation_over_map(int uncomp_precise_x, int uncomp_precise_y);

static bool designation_paint_active() {
    if (!df::global::game) return false;
    auto& mi = df::global::game->main_interface;
    if (mi.main_designation_selected != df::main_designation_type::NONE) return true;
    switch (mi.bottom_mode_selected) {
        case df::main_bottom_mode_type::ZONE_PAINT:
        case df::main_bottom_mode_type::STOCKPILE_PAINT:
        case df::main_bottom_mode_type::BURROW_PAINT:
            return true;
        default:
            break;
    }
    return false;
}

// 3.23.7: placement mode = user is in a placement mode that draws a "ghost"
// cursor at the mouse position (building ghost, zone paint cursor, etc.).
// DF draws the ghost at the un-compensated precise_mouse position during
// render, so it appears out of sync. The click handler (feed) already
// applies comp so the actual placement is correct — only the visual is off.
// We extend render-time comp to also fire for these modes (when cursor is
// over the map), so the ghost draws at the correct position.
static bool placement_mode_active() {
    if (!df::global::game) return false;
    auto& mi = df::global::game->main_interface;
    switch (mi.bottom_mode_selected) {
        case df::main_bottom_mode_type::BUILDING_PLACEMENT:
        case df::main_bottom_mode_type::ZONE_PAINT:
        case df::main_bottom_mode_type::STOCKPILE_PAINT:
        case df::main_bottom_mode_type::BURROW_PAINT:
            return true;
        default:
            break;
    }
    if (mi.main_designation_selected != df::main_designation_type::NONE) return true;
    return false;
}

bool designation_sync_wanted() {
    return real_rectangle_drag();
}

bool designation_sync_wants_render(int uncomp_precise_x, int uncomp_precise_y) {
    g_sp_wants_render_ux = uncomp_precise_x;
    g_sp_wants_render_uy = uncomp_precise_y;
    g_sp_wants_render_drag = real_rectangle_drag() ? 1 : 0;
    g_sp_wants_render_placement = placement_mode_active() ? 1 : 0;
    g_sp_wants_render_overmap = -1;
    if (g_sp_wants_render_drag) { g_sp_wants_render_last = 1; return true; }
    if (g_sp_wants_render_placement) {
        g_sp_wants_render_overmap = designation_over_map(uncomp_precise_x, uncomp_precise_y) ? 1 : 0;
        if (g_sp_wants_render_overmap) { g_sp_wants_render_last = 1; return true; }
    }
    g_sp_wants_render_last = 0;
    return false;
}

static bool designation_sync_after_paint() {
    return designation_paint_active();
}

static bool designation_over_map(int uncomp_precise_x, int uncomp_precise_y) {
    g_sp_overmap_inui = -1;
    g_sp_overmap_widget = -1;
    g_sp_overmap_gate = -1;
    if (!df::global::gps || uncomp_precise_x < 0 || uncomp_precise_y < 0) return false;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    int raw_x = uncomp_precise_x + vp.origin_x;
    int raw_y = uncomp_precise_y + vp.origin_y;
    g_sp_overmap_rawx = raw_x;
    g_sp_overmap_rawy = raw_y;
    // For rectangle drag, keep the strict check (container-rect aware) to
    // avoid patching while user drags over sidebar panels.
    if (real_rectangle_drag()) {
        int inui = IsMouseInUI_reason(raw_x, raw_y, nullptr);
        g_sp_overmap_inui = inui;
        if (inui != 0) return false;
    } else {
        // For placement modes (BUILDING_PLACEMENT / ZONE_PAINT / etc.),
        // IsMouseInUI_reason returns 5 (widgets_contain_point) when the
        // building-placement panel's CONTAINER rect covers the map area —
        // even though the cursor is actually over the map, not a leaf widget.
        // The leaf-only mouse_over_ui_widget check correctly returns false
        // in that case.  Skip the container-rect check and rely on the
        // leaf-only + gate checks instead.
        g_sp_overmap_inui = 0;
    }
    bool widget = mouse_over_ui_widget(raw_x, raw_y);
    g_sp_overmap_widget = widget ? 1 : 0;
    if (widget) return false;
    bool gate = mouse_gate_should_compensate(raw_x, raw_y, nullptr);
    g_sp_overmap_gate = gate ? 1 : 0;
    return gate;
}

static void patch_mouse_tile_indices() {
    if (!df::global::gps) return;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) return;

    auto* gps = df::global::gps;
    int cell = vp.cell_size;
    g_saved_mx = gps->mouse_x;
    g_saved_my = gps->mouse_y;
    gps->mouse_x = gps->precise_mouse_x / cell;
    gps->mouse_y = gps->precise_mouse_y / cell;
    g_sp_desig_patched_mx = gps->mouse_x;
    g_sp_desig_patched_my = gps->mouse_y;
    g_mouse_patched = true;
}

static void sync_selection_rect_from_mouse(bool fix_fresh_start) {
    if (!df::global::selection_rect) return;
    df::coord pos = Gui::getMousePos(true);
    if (!pos.isValid()) return;

    g_sp_desig_mpos_x = pos.x;
    g_sp_desig_mpos_y = pos.y;

    auto* sr = df::global::selection_rect;
    g_sp_desig_sel_sx = sr->start_x;
    g_sp_desig_sel_sy = sr->start_y;
    g_sp_desig_sel_sz = sr->start_z;
    g_sp_desig_sel_ex = sr->end_x;
    g_sp_desig_sel_ey = sr->end_y;
    g_sp_desig_sel_ez = sr->end_z;

    if (fix_fresh_start && sr->start_x > -30000 && g_prev_start_x <= -30000) {
        sr->start_x = pos.x;
        sr->start_y = pos.y;
        sr->start_z = pos.z;
    }

    if (sr->start_x > -30000) {
        sr->end_x = pos.x;
        sr->end_y = pos.y;
        sr->end_z = pos.z;
    }

    g_sp_desig_sel_sx = sr->start_x;
    g_sp_desig_sel_sy = sr->start_y;
    g_sp_desig_sel_sz = sr->start_z;
    g_sp_desig_sel_ex = sr->end_x;
    g_sp_desig_sel_ey = sr->end_y;
    g_sp_desig_sel_ez = sr->end_z;
    g_prev_start_x = sr->start_x;
}

void designation_sync_before_vanilla(int uncomp_precise_x, int uncomp_precise_y) {
    g_sp_desig_active = 0;
    g_sp_desig_paint = designation_paint_active() ? 1 : 0;
    g_sp_desig_drag = real_rectangle_drag() ? 1 : 0;
    g_sp_desig_patched_mx = 0;
    g_sp_desig_patched_my = 0;

    if (!real_rectangle_drag()) return;
    if (!designation_over_map(uncomp_precise_x, uncomp_precise_y)) return;

    g_sp_desig_active = 1;
    patch_mouse_tile_indices();
    sync_selection_rect_from_mouse(false);
}

void designation_sync_after_vanilla(int uncomp_precise_x, int uncomp_precise_y) {
    const bool drag = real_rectangle_drag();
    const bool paint = designation_sync_after_paint();

    if (!drag && !paint) {
        if (df::global::selection_rect)
            g_prev_start_x = df::global::selection_rect->start_x;
        g_sp_desig_paint = 0;
        g_sp_desig_drag = 0;
        return;
    }
    if (!designation_over_map(uncomp_precise_x, uncomp_precise_y)) return;

    g_sp_desig_active = 1;
    g_sp_desig_paint = paint ? 1 : 0;
    g_sp_desig_drag = drag ? 1 : 0;
    sync_selection_rect_from_mouse(true);
}

void designation_sync_restore() {
    if (!g_mouse_patched || !df::global::gps) return;
    df::global::gps->mouse_x = g_saved_mx;
    df::global::gps->mouse_y = g_saved_my;
    g_mouse_patched = false;
}

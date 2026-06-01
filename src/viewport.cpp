#include "viewport.h"

#include "DataDefs.h"
#include "camera.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/enabler.h"
#include "df/renderer_2d.h"
#include "df/gamest.h"
#include "df/main_interface.h"
#include "df/interfacest.h"
#include "df/viewscreen.h"
#include "df/widget.h"
#include "df/widget_container.h"

#include <cmath>

using namespace DFHack;

static bool widget_covers_screen(const df::widget* w);  // defined below

// Widget rects (extentst / sx,sy,ex,ey) are in TEXT-CELL units, not pixels.
// One text cell = viewport_zoom_factor / 16 pixels (12 px at the default z=192).
// All pixel ↔ text-cell conversions go through this helper.
static int text_cell_px() {
    if (!df::global::gps) return 0;
    int z = df::global::gps->viewport_zoom_factor;
    return (z >= 16) ? z / 16 : 0;
}

// Diagnostics: rect (text cells) of the widget that matched the last
// widgets_contain_point() hit, plus whether it was a container.
int g_matched_widget_x1 = 0, g_matched_widget_y1 = 0;
int g_matched_widget_x2 = 0, g_matched_widget_y2 = 0;
int g_matched_widget_is_container = 0;

static bool check_widget_visible_at(df::widget* w, int mx, int my) {
    if (!w) return false;
    int tc = text_cell_px();
    if (tc <= 0) return false;
    // Convert pixel mouse position to text-cell coordinates for widget rect comparison.
    int tx = mx / tc;
    int ty = my / tc;

    df::widget_container* container = virtual_cast<df::widget_container>(w);
    if (container) {
        if (!widget_covers_screen(w)) {
            if (tx >= w->rect.x1 && tx <= w->rect.x2 &&
                ty >= w->rect.y1 && ty <= w->rect.y2) {
                g_matched_widget_x1 = w->rect.x1; g_matched_widget_y1 = w->rect.y1;
                g_matched_widget_x2 = w->rect.x2; g_matched_widget_y2 = w->rect.y2;
                g_matched_widget_is_container = 1;
                return true;
            }
        }
        for (auto& child : container->children) {
            if (check_widget_visible_at(child.get(), mx, my)) {
                return true;
            }
        }
        return false;
    }

    // Leaf widget
    if (tx >= w->rect.x1 && tx <= w->rect.x2 &&
        ty >= w->rect.y1 && ty <= w->rect.y2) {
        g_matched_widget_x1 = w->rect.x1; g_matched_widget_y1 = w->rect.y1;
        g_matched_widget_x2 = w->rect.x2; g_matched_widget_y2 = w->rect.y2;
        g_matched_widget_is_container = 0;
        return true;
    }
    return false;
}

static bool widget_covers_screen(const df::widget* w) {
    if (!w || !df::global::gps) return false;
    int ww = w->rect.x2 - w->rect.x1;  // text cells
    int wh = w->rect.y2 - w->rect.y1;  // text cells
    int tc = text_cell_px();
    if (tc <= 0) return false;
    // Convert screen pixel size to text-cell units for apples-to-apples comparison.
    int sw = df::global::gps->screen_pixel_x / tc;
    int sh = df::global::gps->screen_pixel_y / tc;
    if (sw <= 0 || sh <= 0) return ww > 130 && wh > 40;
    return ww >= sw * 85 / 100 && wh >= sh * 85 / 100;
}

static bool check_widget_overlay_intersects(df::widget* w, const SDL_Rect* r) {
    if (!w || !w->flag.bits.VISIBILITY_VISIBLE) return false;
    int tc = text_cell_px();
    if (tc <= 0) return false;
    // Convert pixel blit rect to text-cell coordinates for widget rect comparison.
    int rx1 = r->x / tc;
    int ry1 = r->y / tc;
    int rx2 = (r->x + r->w) / tc;
    int ry2 = (r->y + r->h) / tc;

    df::widget_container* container = virtual_cast<df::widget_container>(w);
    if (container) {
        if (!widget_covers_screen(w)) {
            if (!(rx1 > w->rect.x2 || rx2 < w->rect.x1 ||
                  ry1 > w->rect.y2 || ry2 < w->rect.y1)) {
                return true;
            }
        }
        for (auto& child : container->children) {
            if (check_widget_overlay_intersects(child.get(), r)) {
                return true;
            }
        }
        return false;
    }

    return !(rx1 > w->rect.x2 || rx2 < w->rect.x1 ||
             ry1 > w->rect.y2 || ry2 < w->rect.y1);
}

static bool widgets_intersect_rect(const SDL_Rect* r) {
    if (!r || !df::global::gview) return false;

    for (df::viewscreen* vs = &df::global::gview->view; vs; vs = vs->child) {
        if (check_widget_overlay_intersects(&vs->widgets, r)) {
            return true;
        }
    }
    return false;
}

static bool widgets_contain_point(int mx, int my) {
    if (!df::global::gview) return false;

    for (df::viewscreen* vs = &df::global::gview->view; vs; vs = vs->child) {
        if (check_widget_visible_at(&vs->widgets, mx, my)) {
            return true;
        }
    }
    return false;
}

// Leaf-only hit test for the WORLD-MOUSE gate.  Unlike check_widget_visible_at,
// this NEVER matches a container's own rect — it descends to the actual drawn
// leaf widgets (tabs/buttons/labels).  The dwarfmode map is NOT a widget, so a
// mid-map cursor matches no leaf (→ compensate), while a cursor over a real UI
// element matches that leaf (→ skip compensation).  Invisible leaves are ignored.
static bool leaf_widget_contains_point(df::widget* w, int mx, int my, int tc) {
    if (!w) return false;
    df::widget_container* container = virtual_cast<df::widget_container>(w);
    if (container) {
        for (auto& child : container->children) {
            if (leaf_widget_contains_point(child.get(), mx, my, tc)) return true;
        }
        return false;  // do NOT match the container region itself
    }
    if (!w->flag.bits.VISIBILITY_VISIBLE) return false;
    int tx = mx / tc;
    int ty = my / tc;
    if (tx >= w->rect.x1 && tx <= w->rect.x2 &&
        ty >= w->rect.y1 && ty <= w->rect.y2) {
        g_matched_widget_x1 = w->rect.x1; g_matched_widget_y1 = w->rect.y1;
        g_matched_widget_x2 = w->rect.x2; g_matched_widget_y2 = w->rect.y2;
        g_matched_widget_is_container = 0;
        return true;
    }
    return false;
}

bool mouse_over_ui_widget(int mx, int my) {
    if (!df::global::gview) return false;
    int tc = text_cell_px();
    if (tc <= 0) return false;
    for (df::viewscreen* vs = &df::global::gview->view; vs; vs = vs->child) {
        if (leaf_widget_contains_point(&vs->widgets, mx, my, tc)) return true;
    }
    return false;
}

// Diagnostic accessors for DF's own mouse-zone / bottom-mode state.
int sp_mouse_zone() {
    if (!df::global::game) return -99;
    return df::global::game->main_interface.mouse_zone;
}
int sp_bottom_mode() {
    if (!df::global::game) return -99;
    return static_cast<int>(df::global::game->main_interface.bottom_mode_selected);
}
int sp_mouse_scrolling_map() {
    if (!df::global::game) return -99;
    return df::global::game->main_interface.mouse_scrolling_map ? 1 : 0;
}

bool rect_intersects_open_panel(const SDL_Rect* r, bool open, int x1, int y1, int x2, int y2) {
    if (!open || !r) return false;
    return !(r->x > x2 || r->x + r->w < x1 ||
             r->y > y2 || r->y + r->h < y1);
}

static bool premium_panels_intersect_rect(const SDL_Rect* r) {
    if (!r || !df::global::game) return false;
    int tc = text_cell_px();
    if (tc <= 0) return false;
    // mi.info.rect is in text-cell units (inherited from widget/extentst).
    // Convert the pixel rect to text cells before comparing.
    SDL_Rect tr = { r->x / tc, r->y / tc,
                    std::max(1, r->w / tc),
                    std::max(1, r->h / tc) };

    auto& mi = df::global::game->main_interface;

    if (rect_intersects_open_panel(&tr, mi.info.open,
            mi.info.rect.x1, mi.info.rect.y1, mi.info.rect.x2, mi.info.rect.y2)) {
        return true;
    }

    return false;
}

static bool premium_panels_contain_point(int mx, int my) {
    SDL_Rect pt = { mx, my, 1, 1 };
    return premium_panels_intersect_rect(&pt);
}

bool rect_fully_inside_viewport(const SDL_Rect* r, const ViewportRect& vp) {
    return r->x >= vp.left && r->y >= vp.top &&
           r->x + r->w <= vp.right && r->y + r->h <= vp.bottom;
}

static bool rect_intersects_viewport(const SDL_Rect* r, const ViewportRect& vp) {
    return !(r->x >= vp.right || r->x + r->w <= vp.left ||
             r->y >= vp.bottom || r->y + r->h <= vp.top);
}

bool get_strict_viewport_rect(ViewportRect* out) {
    if (!out || !df::global::gps || !df::global::gps->main_viewport) return false;

    auto vp = df::global::gps->main_viewport;
    int z = df::global::gps->viewport_zoom_factor;
    if (z <= 0) return false;

    out->tile_px = z;
    out->cell_size = z / 4;
    out->left = vp->screen_x;
    out->top = vp->screen_y;
    out->right = out->left + vp->dim_x * out->cell_size;
    out->bottom = out->top + vp->dim_y * out->cell_size;

    // The tile bake grid starts at renderer_2d::origin, NOT at the viewport
    // screen position.  Alignment/HUD detection must use this grid origin.
    out->origin_x = out->left;
    out->origin_y = out->top;
    if (df::global::enabler) {
        auto* r2d = virtual_cast<df::renderer_2d>(df::global::enabler->renderer);
        if (r2d) {
            out->origin_x = r2d->origin_x;
            out->origin_y = r2d->origin_y;
        }
    }
    return true;
}

bool get_overscan_viewport_rect(ViewportRect* out) {
    if (!get_strict_viewport_rect(out)) return false;
    if (g_camera.overscan_tiles_x < 0) {
        out->left -= out->tile_px;
    } else if (g_camera.overscan_tiles_x > 0) {
        out->right += out->tile_px;
    }
    if (g_camera.overscan_tiles_y < 0) {
        out->top -= out->tile_px;
    } else if (g_camera.overscan_tiles_y > 0) {
        out->bottom += out->tile_px;
    }
    return true;
}

bool is_tile_sized(int w, int h, int tile_px) {
    if (w <= 0 || h <= 0 || tile_px <= 0) return false;
    int cell = tile_px / 4;
    if (cell <= 0) return false;
    if (w > tile_px * 4 || h > tile_px * 4) return false;
    if (w % cell == 0 && h % cell == 0) return true;
    return (w % tile_px == 0) && (h % tile_px == 0);
}

bool is_interface_blit(int w, int h) {
    if ((w == 14 && h == 21) || (w == 21 && h == 14)) return true;
    if (w >= 200 && h >= 200) return true;
    return false;
}

bool is_viewport_map_aligned(int x, int y, const ViewportRect& vp) {
    if (vp.cell_size <= 0) return false;
    // Measure against the renderer's bake grid (origin), not the viewport screen
    // position.  The two differ by a non-cell-multiple offset, so a vp.left/top
    // based test would report EVERY map tile as misaligned.
    int rx = x - vp.origin_x;
    int ry = y - vp.origin_y;
    rx %= vp.cell_size; if (rx < 0) rx += vp.cell_size;
    ry %= vp.cell_size; if (ry < 0) ry += vp.cell_size;
    return rx == 0 && ry == 0;
}

bool clip_rect_matches_viewport(const SDL_Rect* rect) {
    if (!rect) return false;
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;
    int vp_w = vp.right - vp.left;
    int vp_h = vp.bottom - vp.top;
    return std::abs(rect->x - vp.left) <= 2 &&
           std::abs(rect->y - vp.top) <= 2 &&
           std::abs(rect->w - vp_w) <= vp.tile_px &&
           std::abs(rect->h - vp_h) <= vp.tile_px;
}

static bool is_left_toolbar_blit(const SDL_Rect* r, const ViewportRect& vp) {
    if (!r) return false;
    if (r->x + r->w <= vp.left) return true;
    // Left toolbar tiles straddle vp.left when the viewport sits low on screen
    // (F9 3.11.24: x=6 w=40 into vp.left=32 → moon row shifted, was pinned at vp.left=63).
    if (r->x < vp.left && r->x + r->w > vp.left &&
        r->y < vp.top + vp.cell_size) {
        return true;
    }
    return false;
}

static bool is_origin_row_hud_blit(const SDL_Rect* r, const ViewportRect& vp) {
    if (!r || vp.cell_size <= 0) return false;
    // Bake row 0 at origin_y straddles the viewport top: pop-bar portraits share
    // this row with partially visible map.  Shift the whole tile and HUD jiggles
    // against fixed 14x21 overlays; pin the row when it crosses vp.top.
    if (r->y != vp.origin_y) return false;
    if (r->y + r->h <= vp.top) return false;
    if (r->y >= vp.top) return false;
    if (!is_tile_sized(r->w, r->h, vp.tile_px)) return false;
    return true;
}

static bool is_top_chrome_band_blit(const SDL_Rect* r, const ViewportRect& vp) {
    if (!r || vp.cell_size <= 0) return false;
    // Moon/HUD sprites sit in the cell band above the first in-viewport map row
    // (F9: 56×56 at y=8 while vp.top≈21–30).
    return r->y + r->h <= vp.top + vp.cell_size;
}

static bool is_top_toolbar_blit(const SDL_Rect* r, const ViewportRect& vp) {
    // Only fully-above-viewport blits are top chrome.  Row 0 of the map bakes at
    // origin_y (above vp.top) and is partially visible inside the viewport, so it
    // must remain shiftable rather than being mistaken for the toolbar.
    return r->y + r->h <= vp.top;
}

static bool is_bottom_toolbar_blit(const SDL_Rect* r, const ViewportRect& vp) {
    return r->y >= vp.bottom;
}

static bool is_embedded_hud_tile_blit(const SDL_Rect* r, const ViewportRect& vp) {
    if (!r || vp.cell_size <= 0) return false;
    // Resource/status rows inside the viewport use map-sized tiles off the
    // bake grid (F9: y≈vp.top+11).  On-grid tiles in this band are real map.
    if (r->y < vp.top) return false;
    if (r->y >= vp.top + vp.cell_size) return false;
    if (!is_tile_sized(r->w, r->h, vp.tile_px)) return false;
    return !is_viewport_map_aligned(r->x, r->y, vp);
}


BlitClassification classify_blit(int x, int y, int w, int h) {
    BlitClassification result;
    SDL_Rect r = { x, y, w, h };

    ViewportRect strict_vp;
    if (!get_strict_viewport_rect(&strict_vp)) return result;

    result.intersects_viewport = rect_intersects_viewport(&r, strict_vp);
    result.outside_viewport = !rect_fully_inside_viewport(&r, strict_vp);
    if (w > 0 && h > 0) {
        result.tile_sized = is_tile_sized(w, h, strict_vp.tile_px);
        result.tile_aligned = is_viewport_map_aligned(x, y, strict_vp);
    }

    if (w <= 0 || h <= 0) {
        result.in_ui = true;
    } else if (!result.intersects_viewport) {
        result.in_ui = true;
    } else if (is_left_toolbar_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (is_top_toolbar_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (is_top_chrome_band_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (is_origin_row_hud_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (is_bottom_toolbar_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (is_embedded_hud_tile_blit(&r, strict_vp)) {
        result.in_ui = true;
    } else if (w > 0 && h > 0 && is_interface_blit(w, h)) {
        result.in_ui = true;
    } else {
        // classify_blit runs only during g_in_main_viewport_update (world-map
        // bake).  Everything else here is a map cell or creature sprite.
        result.cls = BlitClass::Map;
        result.in_ui = false;
        return result;
    }

    if (result.intersects_viewport && !result.in_ui && !result.tile_sized) {
        result.suspected_hud_false_positive = true;
    }

    return result;
}

bool IsRectInUI(const SDL_Rect* r) {
    if (!r) return false;

    ViewportRect vp;
    if (get_strict_viewport_rect(&vp) && !rect_fully_inside_viewport(r, vp)) {
        return true;
    }

    if (is_interface_blit(r->w, r->h)) return true;
    if (widgets_intersect_rect(r)) return true;
    if (premium_panels_intersect_rect(r)) return true;

    return false;
}

bool IsMouseInUI(int mx, int my) {
    return IsMouseInUI_reason(mx, my, nullptr) != 0;
}

int IsMouseInUI_reason(int mx, int my, ViewportRect* out_vp) {
    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return 0;
    if (out_vp) *out_vp = vp;

    if (mx < vp.left)   return 1;
    if (mx > vp.right)  return 2;
    if (my < vp.top)    return 3;
    if (my > vp.bottom) return 4;

    if (widgets_contain_point(mx, my)) return 5;
    if (premium_panels_contain_point(mx, my)) return 6;

    return 0;
}

bool map_pick_screen_from_gps(int* screen_x, int* screen_y) {
    if (!screen_x || !screen_y || !df::global::gps) return false;
    auto* gps = df::global::gps;
    if (gps->precise_mouse_x < 0 || gps->precise_mouse_y < 0) return false;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp)) return false;

    *screen_x = gps->precise_mouse_x + vp.origin_x;
    *screen_y = gps->precise_mouse_y + vp.origin_y;
    return true;
}

bool map_pick_screen_for_gate(int sdl_x, int sdl_y, int* screen_x, int* screen_y) {
    if (!screen_x || !screen_y) return false;
    if (map_pick_screen_from_gps(screen_x, screen_y)) return true;
    *screen_x = sdl_x;
    *screen_y = sdl_y;
    return true;
}

bool mouse_gate_should_compensate(int pick_x, int pick_y, int* out_inui_reason) {
    ViewportRect uivp;
    int inui = IsMouseInUI_reason(pick_x, pick_y, &uivp);
    if (out_inui_reason) *out_inui_reason = inui;
    if ((inui >= 1 && inui <= 4) || inui == 6) return false;
    if (mouse_over_ui_widget(pick_x, pick_y)) return false;
    return true;
}

void compute_expected_world_tile(int precise_x, int precise_y,
                                 float shift_x, float shift_y,
                                 int* out_wx, int* out_wy) {
    if (!out_wx || !out_wy) return;
    *out_wx = 0;
    *out_wy = 0;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) return;
    if (!df::global::window_x || !df::global::window_y) return;

    int cell = vp.cell_size;
    int adj_x = precise_x + static_cast<int>(std::lround(shift_x));
    int adj_y = precise_y + static_cast<int>(std::lround(shift_y));
    *out_wx = *df::global::window_x + adj_x / cell;
    *out_wy = *df::global::window_y + adj_y / cell;
}

void compute_expected_world_tile_trunc_shift(int precise_x, int precise_y,
                                             float shift_x, float shift_y,
                                             int* out_wx, int* out_wy) {
    if (!out_wx || !out_wy) return;
    *out_wx = 0;
    *out_wy = 0;

    ViewportRect vp;
    if (!get_strict_viewport_rect(&vp) || vp.cell_size <= 0) return;
    if (!df::global::window_x || !df::global::window_y) return;

    int cell = vp.cell_size;
    int adj_x = precise_x + static_cast<int>(shift_x);
    int adj_y = precise_y + static_cast<int>(shift_y);
    *out_wx = *df::global::window_x + adj_x / cell;
    *out_wy = *df::global::window_y + adj_y / cell;
}

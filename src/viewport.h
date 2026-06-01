#pragma once

#include <SDL.h>

struct ViewportRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int cell_size = 0;
    int tile_px = 0;   // viewport_zoom_factor (pixels per graphical tile)
    int origin_x = 0;  // renderer_2d tile-grid origin (where col 0 is baked)
    int origin_y = 0;  // renderer_2d tile-grid origin (where row 0 is baked)
};

enum class BlitClass {
    PassThrough,
    Map,
};

struct BlitClassification {
    BlitClass cls = BlitClass::PassThrough;
    bool intersects_viewport = false;
    bool outside_viewport = false;
    bool in_ui = false;
    bool tile_sized = false;
    bool tile_aligned = false;
    bool suspected_hud_false_positive = false;
};

bool get_strict_viewport_rect(ViewportRect* out);
bool get_overscan_viewport_rect(ViewportRect* out);

bool is_tile_sized(int w, int h, int tile_px);
bool is_viewport_map_aligned(int x, int y, const ViewportRect& vp);
bool is_interface_blit(int w, int h);
bool clip_rect_matches_viewport(const SDL_Rect* rect);
bool rect_fully_inside_viewport(const SDL_Rect* r, const ViewportRect& vp);

BlitClassification classify_blit(int x, int y, int w, int h);

bool IsRectInUI(const SDL_Rect* r);
bool IsMouseInUI(int mx, int my);
// Leaf-only widget hit test for the world-mouse compensation gate.
bool mouse_over_ui_widget(int mx, int my);
// DF main_interface state accessors (diagnostics for the mouse gate).
int sp_mouse_zone();
int sp_bottom_mode();
int sp_mouse_scrolling_map();
// Returns 0=inside-map, 1=left, 2=right, 3=top, 4=bottom, 5=widget, 6=panel.
// Fills out_vp with the bounds used (may be null).
int IsMouseInUI_reason(int mx, int my, ViewportRect* out_vp);

bool rect_intersects_open_panel(const SDL_Rect* r, bool open, int x1, int y1, int x2, int y2);

// Plan 5 — unified map-pick pixel for UI gate (origin-relative screen space).
bool map_pick_screen_from_gps(int* screen_x, int* screen_y);
bool map_pick_screen_for_gate(int sdl_x, int sdl_y, int* screen_x, int* screen_y);
bool mouse_gate_should_compensate(int pick_x, int pick_y, int* out_inui_reason);

// Plan 3 — expected world tile from geometry (uncompensated precise + render_shift).
void compute_expected_world_tile(int precise_x, int precise_y,
                                 float shift_x, float shift_y,
                                 int* out_wx, int* out_wy);
void compute_expected_world_tile_trunc_shift(int precise_x, int precise_y,
                                             float shift_x, float shift_y,
                                             int* out_wx, int* out_wy);

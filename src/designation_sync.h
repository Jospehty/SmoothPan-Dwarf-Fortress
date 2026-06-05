#pragma once

// Rectangle designation uses gps->mouse_x/y (precise/dispx_z), not precise alone.
// Patch those tile indices only in designate paint / drag over the map.

bool designation_sync_wanted();
bool designation_sync_wants_render(int uncomp_precise_x, int uncomp_precise_y);

void designation_sync_before_vanilla(int uncomp_precise_x, int uncomp_precise_y);
void designation_sync_after_vanilla(int uncomp_precise_x, int uncomp_precise_y);
void designation_sync_restore();

extern int g_sp_desig_active;
extern int g_sp_desig_paint;
extern int g_sp_desig_drag;
extern int g_sp_desig_patched_mx;
extern int g_sp_desig_patched_my;
extern int g_sp_desig_sel_sx, g_sp_desig_sel_sy, g_sp_desig_sel_sz;
extern int g_sp_desig_sel_ex, g_sp_desig_sel_ey, g_sp_desig_sel_ez;
extern int g_sp_desig_mpos_x, g_sp_desig_mpos_y;

// World cursor position captured during render interpose.  DF resets
// df::global::cursor to (-30000,-30000,-30000) at end of frame, so reading
// it at F9 time (SDL_RenderPresent) shows the sentinel.  These capture
// the cursor at start-of-render (before comp) and end-of-render (after comp,
// before restore) so we can see whether DF's ghost anchor gets the comp.
extern int g_sp_cur_rend_start_x, g_sp_cur_rend_start_y, g_sp_cur_rend_start_z;
extern int g_sp_cur_rend_end_x,   g_sp_cur_rend_end_y,   g_sp_cur_rend_end_z;
extern int g_sp_cur_precise_rend_start_x, g_sp_cur_precise_rend_start_y;
extern int g_sp_cur_precise_rend_end_x,   g_sp_cur_precise_rend_end_y;

// Per-call trace of designation_sync_wants_render — tells us exactly
// which sub-check is failing.
extern int g_sp_wants_render_last;       // final return value
extern int g_sp_wants_render_drag;       // real_rectangle_drag() result
extern int g_sp_wants_render_placement;  // placement_mode_active() result
extern int g_sp_wants_render_overmap;    // designation_over_map() result
extern int g_sp_wants_render_ux, g_sp_wants_render_uy; // coords passed in
// Sub-check trace for designation_over_map — which guard returned false.
extern int g_sp_overmap_inui;            // IsMouseInUI_reason result
extern int g_sp_overmap_widget;          // mouse_over_ui_widget result
extern int g_sp_overmap_gate;            // mouse_gate_should_compensate result
extern int g_sp_overmap_rawx, g_sp_overmap_rawy; // raw coords used

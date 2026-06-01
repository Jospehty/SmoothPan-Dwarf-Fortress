#pragma once

// Rectangle designation uses gps->mouse_x/y (precise/dispx_z), not precise alone.
// Patch those tile indices only in designate paint / drag over the map.

bool designation_sync_wanted();
bool designation_sync_wants_render();

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

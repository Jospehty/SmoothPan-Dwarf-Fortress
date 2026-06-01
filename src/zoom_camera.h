#pragma once

// Phase 1+ — smooth zoom state machine (visual scale in Phase 2).
// Intercepts feed ZOOM_IN/OUT; commits viewport_zoom_factor once per step.

void smoothpan_zoom_reset();
void smoothpan_zoom_sync_baked();

// Returns false while sub-tile pan / MMB drag — wheel passes to vanilla (Phase 3 adds anchor).
bool smoothpan_zoom_should_intercept();
bool smoothpan_zoom_can_feed(bool zoom_in);
void smoothpan_zoom_feed(bool zoom_in);
// Process queued wheel zoom — call from logic after vanilla logic().
void smoothpan_zoom_tick(float dt);
void smoothpan_zoom_update(float dt);

bool smoothpan_zoom_animating();
float smoothpan_zoom_anim_t();
int smoothpan_zoom_baked_z();
int smoothpan_zoom_from_z();
int smoothpan_zoom_to_z();

// Visual z during anim (lerp); equals baked when idle. Used by Phase 2 SDL scale.
float smoothpan_zoom_visual_z();
float smoothpan_zoom_render_scale();

// camera.update(): distinguish our commit from external gps z changes.
bool smoothpan_zoom_take_committed_flag();
void smoothpan_zoom_on_our_commit(int prev_z, int new_z);
void smoothpan_zoom_on_external_change(int prev_z, int new_z);

// Phase 2: wait for rebake before easing scale (zoom-out pop-in fix).
void smoothpan_zoom_note_main_map_baked();
void smoothpan_zoom_on_present(int map_shifted, int map_total);
bool smoothpan_zoom_holding_bake();
bool smoothpan_zoom_is_zoom_in();
float smoothpan_zoom_start_scale_value();

extern int g_sp_zoom_anim;
extern int g_sp_zoom_baked;
extern int g_sp_zoom_target;
extern int g_sp_zoom_pending;
extern int g_sp_zoom_intercept;
extern int g_sp_zoom_commit_fail;
extern int g_sp_zoom_fallback;
extern int g_sp_zoom_hold;
extern int g_sp_zoom_dir;

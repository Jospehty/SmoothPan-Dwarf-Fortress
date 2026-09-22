#pragma once

// Smooth zoom camera (3.24.0).
//
// Split-brain model, same as pan:
//   Logic  : gps->viewport_zoom_factor stays on vanilla's discrete ladder
//            (cells 16,24,32,40,48,56,64 px).  Changed only by commits.
//   Visual : v = continuous "visual cell" (px per map tile) eased in log space
//            toward the wheel target.  The compositor draws the current bake
//            at scale s = v / baked_cell, always >= 1 (subset of baked texels,
//            so no missing-texel voids — ever, including cursor anchoring).
//   Commit : the baked cell is always the ladder step at-or-below v.  When v
//            crosses a step (zoom-in) or the wheel target drops (zoom-out) we
//            commit through vanilla's own feed path (ZOOM_IN/ZOOM_OUT), and the
//            compositor bridges the rebake presents with the retained frame.
//            On the commit landing, the camera is moved so the world point
//            under the anchor is exactly where it was (cursor-directed zoom).

#include <cstdio>
#include <cstddef>

namespace df { struct viewscreen; }

// Depth counter: >0 while we are injecting a synthetic ZOOM key into vanilla
// feed (the feed interpose must pass straight through).
extern int g_zoom_inject_depth;

void zoom_camera_reset();

void zoom_camera_set_enabled(bool enabled);
bool zoom_camera_enabled();

// Feed: a ZOOM_IN (+1) / ZOOM_OUT (-1) key arrived.  over_map = cursor is on
// the map (not UI).  Returns true when consumed (erase from vanilla input).
bool zoom_camera_on_zoom_key(int dir, bool over_map);

// Once per render frame, after SmoothCamera::update() and before vanilla
// render.  vs = the dwarfmode viewscreen (for feed injection).
void zoom_camera_update(df::viewscreen* vs);
// Freeze v / anchor for this frame (after update, alongside freeze_render_frac).
void zoom_camera_freeze();
// From the present hook: the frozen state is stale after this.
void zoom_camera_on_present();

// Frame transform for a bake at 'cell' px/tile (>= 1.0).
float zoom_camera_scale_for_cell(int cell);
// Frame anchor in screen (SDL window) pixels.
void zoom_camera_frame_anchor(float* ax, float* ay);
// Visual px per tile this frame (0 when unknown).
float zoom_camera_visual_cell();

// Compositor feedback: which layer went to screen this frame.
void zoom_camera_on_displayed(int cell, bool is_current_bake, int gps_z_of_current);

// Transform currently on screen (for mouse compensation).  Returns false when
// the map is at scale 1 (nothing to invert).
bool zoom_camera_display_transform(float* s, float* ax, float* ay);

bool zoom_camera_in_gesture();

// Config.
void zoom_camera_set_rate(float per_second);
float zoom_camera_rate();
void zoom_camera_set_anchor_cursor(bool cursor);
bool zoom_camera_anchor_cursor();
void zoom_camera_set_commit_direct(bool direct);
bool zoom_camera_commit_direct();
// Console: queue one vanilla ladder step (+1/-1) to be committed next frame.
void zoom_camera_queue_test_step(int dir);

struct ZoomCameraInfo {
    bool enabled = false;
    bool gesture = false;
    bool pending = false;
    float v = 0.0f;            // visual cell (px/tile)
    int target_cell = 0;
    int baked_cell = 0;        // gps->viewport_zoom_factor / 4 as last seen
    int desired_z = 0;
    int display_cell = 0;
    float display_scale = 1.0f;
    int keys = 0, commits = 0, landed = 0, external = 0, timeouts = 0;
};
void zoom_camera_info(ZoomCameraInfo* out);

void zoom_camera_write_f9(FILE* f);
void zoom_camera_status(char* buf, size_t n);

#include "zoom_camera.h"
#include "camera.h"

#include "df/global_objects.h"
#include "df/graphic.h"

#include <algorithm>
#include <cmath>

// Phase 2 — zoom state tracking only.  SDL scale during zoom-out requires
// baking tiles outside the fixed viewport buffer (same constraint as pan
// overscan); ad-hoc dim/window expansion caused garbage lines and crashes.
// Vanilla commits gps z and rebakes instantly for both directions until a
// safe margin path exists (see PLAN_SMOOTH_ZOOM.md).

static float g_anim_t = 1.0f;
static int g_from_z = 0;
static int g_to_z = 0;
static bool g_animating = false;

int g_sp_zoom_anim = 0;
int g_sp_zoom_baked = 0;
int g_sp_zoom_target = 0;
int g_sp_zoom_pending = 0;
int g_sp_zoom_intercept = 0;
int g_sp_zoom_commit_fail = 0;
int g_sp_zoom_fallback = 0;
int g_sp_zoom_hold = 0;
int g_sp_zoom_dir = 0;

static int current_baked_z() {
    if (df::global::gps)
        return df::global::gps->viewport_zoom_factor;
    return 0;
}

static void sync_zoom_level(int z) {
    g_from_z = z;
    g_to_z = z;
    g_sp_zoom_target = z;
    g_sp_zoom_baked = z;
    g_anim_t = 1.0f;
    g_animating = false;
    g_sp_zoom_anim = 0;
    g_sp_zoom_hold = 0;
    g_sp_zoom_dir = 0;
}

void smoothpan_zoom_reset() {
    g_anim_t = 1.0f;
    g_from_z = 0;
    g_to_z = 0;
    g_animating = false;
    g_sp_zoom_anim = 0;
    g_sp_zoom_baked = 0;
    g_sp_zoom_target = 0;
    g_sp_zoom_pending = 0;
    g_sp_zoom_intercept = 0;
    g_sp_zoom_commit_fail = 0;
    g_sp_zoom_fallback = 0;
    g_sp_zoom_hold = 0;
    g_sp_zoom_dir = 0;
}

void smoothpan_zoom_sync_baked() {
    g_sp_zoom_baked = current_baked_z();
    if (!g_animating) {
        g_from_z = g_sp_zoom_baked;
        g_to_z = g_sp_zoom_baked;
    }
}

bool smoothpan_zoom_should_intercept() {
    return false;
}

bool smoothpan_zoom_can_feed(bool zoom_in) {
    (void)zoom_in;
    return false;
}

void smoothpan_zoom_feed(bool zoom_in) {
    (void)zoom_in;
}

void smoothpan_zoom_note_main_map_baked() {
}

void smoothpan_zoom_on_present(int map_shifted, int map_total) {
    (void)map_shifted;
    (void)map_total;
}

void smoothpan_zoom_tick(float dt) {
    (void)dt;
    g_sp_zoom_baked = current_baked_z();
}

void smoothpan_zoom_update(float dt) {
    smoothpan_zoom_tick(dt);
}

bool smoothpan_zoom_animating() {
    return false;
}

bool smoothpan_zoom_holding_bake() {
    return false;
}

bool smoothpan_zoom_is_zoom_in() {
    return false;
}

float smoothpan_zoom_start_scale_value() {
    return 1.0f;
}

float smoothpan_zoom_anim_t() {
    return 0.0f;
}

int smoothpan_zoom_baked_z() {
    return current_baked_z();
}

int smoothpan_zoom_from_z() {
    return current_baked_z();
}

int smoothpan_zoom_to_z() {
    return current_baked_z();
}

float smoothpan_zoom_visual_z() {
    return static_cast<float>(current_baked_z());
}

float smoothpan_zoom_render_scale() {
    return 1.0f;
}

bool smoothpan_zoom_take_committed_flag() {
    return false;
}

void smoothpan_zoom_on_our_commit(int prev_z, int new_z) {
    (void)prev_z;
    (void)new_z;
    g_sp_zoom_baked = current_baked_z();
}

void smoothpan_zoom_on_external_change(int prev_z, int new_z) {
    (void)prev_z;
    if (new_z <= 0) return;
    sync_zoom_level(new_z);
}

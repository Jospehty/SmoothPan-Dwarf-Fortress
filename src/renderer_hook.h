#pragma once

#include <atomic>

// True only while renderer_2d::update_full_viewport is executing for the
// main map viewport.  Used by the SDL blit hook to restrict shifts to actual
// map tile blits and nothing else.
extern std::atomic<bool> g_in_main_viewport_update;
// True after the last map viewport bake until renderer::render() finishes.
// Shifts post-viewport map compositing only (classify_blit still filters UI).
extern std::atomic<bool> g_in_post_viewport_map_shift;

// Increments on every update_full_viewport call; the SDL blit hook tags each
// blit with the current value so telemetry can attribute blits to passes.
extern std::atomic<int> g_viewport_pass_index;

// dim_x/dim_y/screen of the viewport currently being baked (for telemetry).
extern std::atomic<int> g_cur_pass_dim_x;
extern std::atomic<int> g_cur_pass_dim_y;
extern std::atomic<int> g_cur_pass_screen_x;
extern std::atomic<int> g_cur_pass_screen_y;
extern std::atomic<bool> g_cur_pass_is_map;

#include <string>

bool renderer_hook_install();
// Per-interpose apply results (for smoothpan diag).
void renderer_hook_report(std::string& out);
void renderer_hook_remove();

void renderer_log_start(int frames);
bool renderer_log_active();
void renderer_hook_on_present();

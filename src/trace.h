#pragma once

bool trace_is_active();
void trace_start(int frames);
void trace_on_dwarf_render_begin();
void trace_on_dwarf_render_end();

void trace_on_update_full_map_port_begin(void* vp);
void trace_on_update_full_map_port_end(void* vp);
void trace_on_update_full_viewport_begin(void* vp);
void trace_on_update_full_viewport_end(void* vp);
void trace_on_renderer_render_begin();
void trace_on_renderer_render_end();

void trace_on_set_render_target(void* target);
void trace_on_set_viewport(int x, int y, int w, int h);
void trace_on_sdl_blit(int x, int y, int w, int h);
void trace_on_present();

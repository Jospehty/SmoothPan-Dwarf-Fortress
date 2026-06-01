#pragma once

bool InitSDLHooks();
void CleanupSDLHooks();

// Clip the current render target to the map's bake-grid rect [origin, origin+dim*cell]
// while a map pass is baking, so shifted tiles cannot paint into the on-screen
// left/top margin (the vanilla "few-pixel" border).  enable=false restores no-clip.
// sdl_renderer is renderer_2d::sdl_renderer (an SDL_Renderer*).
void smoothpan_apply_map_clip(void* sdl_renderer, bool enable);

extern int g_test_dump_frames;
extern int g_test_dump_delay;
extern int g_classify_log_frames;

// Raw SDL mouse via unhooked trampoline (no compensation).
bool smoothpan_raw_sdl_mouse(int* x, int* y);

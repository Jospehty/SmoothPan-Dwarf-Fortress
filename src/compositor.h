#pragma once

// Retained-frame map compositor (3.24.0).
//
// From the first map-tile bake pass of a frame (renderer_2d::update_full_viewport
// on a map viewport) until the HUD starts drawing, the SDL render target is
// redirected to a screen-sized texture SmoothPan owns.  Every blit DF issues in
// that window — main pass, lower-z sibling passes, whatever it draws between
// them — lands in the texture exactly where it would have landed on screen and
// in the same order (the per-blit sub-tile pan shift and edge extend are applied
// inside the texture, unchanged from the direct path).  The layer is then drawn
// back onto DF's own target with ONE RenderCopyF carrying the zoom transform
// (scale >= 1 about an anchor), after which the HUD draws on top as usual.
//
// Two textures are kept: the frame being captured and the last COMPLETE frame.
// When vanilla's zoom rebake produces a partial ("NARROW") bake — the 1-present
// dim_x lag that caused black bands / mismatched strips — the previous complete
// frame is composited instead.  No DF memory is written by any of this.
//
// Everything degrades to the plain direct path when disabled or when the
// renderer cannot support it (reason logged to smoothpan_compositor.txt).

#include <SDL.h>
#include <cstdio>

enum class CompositorState { Init = 0, Active = 1, Disabled = 2 };

// User toggle (smoothpan compositor on|off).  Default on.
void compositor_set_enabled(bool enabled);
bool compositor_user_enabled();

CompositorState compositor_state();
const char* compositor_state_name();
const char* compositor_disable_reason();
// True when the compositor should take part in rendering this frame.
bool compositor_active();
// True while the map layer is being captured (first map pass .. layer end).
bool compositor_capturing();
// True once this frame's layer has been composited (HUD phase): map-class
// blits arriving now must go through compositor_post_blit.
bool compositor_layer_done();

// Texture filtering used while the zoom scale != 1 (default linear).
void compositor_set_filter_linear(bool linear);
bool compositor_filter_linear();

// renderer_hook integration ------------------------------------------------
// Around INTERPOSE_NEXT(update_full_viewport) for map tile passes.
// sdl_renderer is renderer_2d::sdl_renderer.
void compositor_pass_begin(void* sdl_renderer, bool is_main_vp);
void compositor_pass_end(void* sdl_renderer);
// After INTERPOSE_NEXT(render): fallback layer end if the HUD never triggered it.
void compositor_render_end(void* sdl_renderer);

// sdl_hook integration ------------------------------------------------------
// SetRenderTarget interception while capturing.  Returns true when the call
// was handled (result in *out_result); false = pass through unchanged.
bool compositor_on_set_render_target(SDL_Renderer* r, SDL_Texture* requested, int* out_result);
// Every RenderCopy* blit, BEFORE it is drawn (final dst rect).  map_class =
// classified as map content; map_shifted = pan shift applied.  A non-map blit
// after the last expected map pass ends the layer (composite) first.
void compositor_on_blit(SDL_Renderer* r, float x, float y, float w, float h, bool map_class, bool map_shifted);
// Every RenderFillRect(F), BEFORE it is drawn.
void compositor_on_fill(SDL_Renderer* r, float x, float y, float w, float h);
// Map-class blit in the HUD phase: apply the frame's zoom transform, or return
// false when the blit must be dropped (frame is showing the retained previous
// bake, so this bake's stragglers do not belong on screen).
bool compositor_post_blit(SDL_FRect* rect);
// Called from the SDL_RenderPresent hook (after telemetry).
void compositor_on_present(SDL_Renderer* r);

// Diagnostics.
void compositor_write_f9(FILE* f);
void compositor_status(char* buf, size_t n);
// Reset transient state (plugin enable).  Textures are kept for reuse.
void compositor_reset();

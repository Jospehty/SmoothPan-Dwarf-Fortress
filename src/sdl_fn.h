#pragma once

// Shared SDL2 function-pointer typedefs + the MinHook trampolines ("True_")
// resolved by sdl_hook.cpp.  Other translation units (compositor.cpp) call
// through these so they bypass our own hooks and never recurse.

#include <SDL.h>

typedef int (*SDL_RenderCopyF_t)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_FRect*);
typedef int (*SDL_RenderSetClipRect_t)(SDL_Renderer*, const SDL_Rect*);
typedef int (*SDL_SetRenderTarget_t)(SDL_Renderer*, SDL_Texture*);
typedef int (*SDL_RenderSetViewport_t)(SDL_Renderer*, const SDL_Rect*);
typedef int (*SDL_GetRendererOutputSize_t)(SDL_Renderer*, int*, int*);

extern SDL_RenderCopyF_t True_SDL_RenderCopyF;
extern SDL_RenderSetClipRect_t True_SDL_RenderSetClipRect;
extern SDL_SetRenderTarget_t True_SDL_SetRenderTarget;
extern SDL_RenderSetViewport_t True_SDL_RenderSetViewport;
extern SDL_GetRendererOutputSize_t GetRendererOutputSize_func;

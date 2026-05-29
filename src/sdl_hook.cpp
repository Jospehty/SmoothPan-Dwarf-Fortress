#define NOMINMAX
#include "sdl_hook.h"
#include "camera.h"
#include "MinHook.h"
#include <SDL.h>
#include <cmath>
#include <windows.h>
#undef min
#undef max
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/enabler.h"
#include "df/world.h"
#include "df/zoom_commands.h"
#include <mutex>

typedef int(*SDL_PollEvent_t)(SDL_Event* event);
typedef int(*SDL_RenderCopy_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect);
typedef int(*SDL_RenderCopyEx_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip);
typedef int(*SDL_GetRendererOutputSize_t)(SDL_Renderer* renderer, int* w, int* h);
typedef int(*SDL_RenderCopyF_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect);
typedef int(*SDL_RenderCopyExF_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect, const double angle, const SDL_FPoint* center, const SDL_RendererFlip flip);

SDL_PollEvent_t True_SDL_PollEvent = nullptr;
SDL_RenderCopy_t True_SDL_RenderCopy = nullptr;
SDL_RenderCopyEx_t True_SDL_RenderCopyEx = nullptr;
SDL_GetRendererOutputSize_t GetRendererOutputSize_func = nullptr;
SDL_RenderCopyF_t True_SDL_RenderCopyF = nullptr;
SDL_RenderCopyExF_t True_SDL_RenderCopyExF = nullptr;

int Hook_SDL_PollEvent(SDL_Event* event) {
    int result = True_SDL_PollEvent(event);
    if (result && event->type == SDL_MOUSEWHEEL) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) { // Ctrl+Scroll
            if (event->wheel.y > 0) {
                if (df::global::window_z && df::global::world && *df::global::window_z < df::global::world->map.z_count - 1) {
                    (*df::global::window_z)++;
                }
            } else if (event->wheel.y < 0) {
                if (df::global::window_z && *df::global::window_z > 0) {
                    (*df::global::window_z)--;
                }
            }
            event->type = SDL_FIRSTEVENT; // Nullify event so game ignores it
        }
        // Let normal scroll for Zoom pass through to the engine so it zooms natively!
    }
    return result;
}

int Hook_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) {
    if (dstrect) {
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0) {
            bool is_map_element = ((dstrect->w == z || dstrect->w == z * 2 || dstrect->w == z * 3) &&
                                   (dstrect->h == z || dstrect->h == z * 2 || dstrect->h == z * 3));
            if (is_map_element && True_SDL_RenderCopyF) {
                float frac_x = g_camera.true_x - std::floor(g_camera.true_x);
                float frac_y = g_camera.true_y - std::floor(g_camera.true_y);
                SDL_FRect fdstr;
                fdstr.x = static_cast<float>(dstrect->x) - (frac_x * static_cast<float>(z));
                fdstr.y = static_cast<float>(dstrect->y) - (frac_y * static_cast<float>(z));
                fdstr.w = static_cast<float>(dstrect->w);
                fdstr.h = static_cast<float>(dstrect->h);
                return True_SDL_RenderCopyF(renderer, texture, srcrect, &fdstr);
            }
        }
    }
    return True_SDL_RenderCopy(renderer, texture, srcrect, dstrect);
}

int Hook_SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip) {
    if (dstrect) {
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0) {
            bool is_map_element = ((dstrect->w == z || dstrect->w == z * 2 || dstrect->w == z * 3) &&
                                   (dstrect->h == z || dstrect->h == z * 2 || dstrect->h == z * 3));
            if (is_map_element && True_SDL_RenderCopyExF) {
                float frac_x = g_camera.true_x - std::floor(g_camera.true_x);
                float frac_y = g_camera.true_y - std::floor(g_camera.true_y);
                SDL_FRect fdstr;
                fdstr.x = static_cast<float>(dstrect->x) - (frac_x * static_cast<float>(z));
                fdstr.y = static_cast<float>(dstrect->y) - (frac_y * static_cast<float>(z));
                fdstr.w = static_cast<float>(dstrect->w);
                fdstr.h = static_cast<float>(dstrect->h);
                SDL_FPoint fcenter;
                SDL_FPoint* fcenter_ptr = nullptr;
                if (center) {
                    fcenter.x = static_cast<float>(center->x);
                    fcenter.y = static_cast<float>(center->y);
                    fcenter_ptr = &fcenter;
                }
                return True_SDL_RenderCopyExF(renderer, texture, srcrect, &fdstr, angle, fcenter_ptr, flip);
            }
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect, angle, center, flip);
}

bool InitSDLHooks() {
    if (MH_Initialize() != MH_OK) return false;
    
    HMODULE sdl_module = GetModuleHandleA("SDL2.dll");
    if (!sdl_module) return false;
    
    void* poll_event_addr = (void*)GetProcAddress(sdl_module, "SDL_PollEvent");
    void* render_copy_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopy");
    void* render_copy_ex_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyEx");
    GetRendererOutputSize_func = (SDL_GetRendererOutputSize_t)GetProcAddress(sdl_module, "SDL_GetRendererOutputSize");
    True_SDL_RenderCopyF = (SDL_RenderCopyF_t)GetProcAddress(sdl_module, "SDL_RenderCopyF");
    True_SDL_RenderCopyExF = (SDL_RenderCopyExF_t)GetProcAddress(sdl_module, "SDL_RenderCopyExF");
    
    if (poll_event_addr) MH_CreateHook(poll_event_addr, &Hook_SDL_PollEvent, reinterpret_cast<LPVOID*>(&True_SDL_PollEvent));
    if (render_copy_addr) MH_CreateHook(render_copy_addr, &Hook_SDL_RenderCopy, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopy));
    if (render_copy_ex_addr) MH_CreateHook(render_copy_ex_addr, &Hook_SDL_RenderCopyEx, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyEx));
    
    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

void CleanupSDLHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

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

typedef int(*SDL_RenderCopy_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect);
typedef int(*SDL_RenderCopyEx_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip);
typedef int(*SDL_GetRendererOutputSize_t)(SDL_Renderer* renderer, int* w, int* h);
typedef void(*SDL_RenderPresent_t)(SDL_Renderer* renderer);

SDL_RenderCopy_t True_SDL_RenderCopy = nullptr;
SDL_RenderCopyEx_t True_SDL_RenderCopyEx = nullptr;
SDL_GetRendererOutputSize_t GetRendererOutputSize_func = nullptr;
SDL_RenderPresent_t True_SDL_RenderPresent = nullptr;

void Hook_SDL_RenderPresent(SDL_Renderer* renderer) {
    g_camera.current_frac_x.store(g_camera.next_frac_x.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_camera.current_frac_y.store(g_camera.next_frac_y.load(std::memory_order_relaxed), std::memory_order_relaxed);
    True_SDL_RenderPresent(renderer);
}

int Hook_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) {
    SDL_Rect modified_dst;
    if (dstrect) {
        modified_dst = *dstrect;
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0) {
            bool is_map_element = ((dstrect->w == z || dstrect->w == z * 2 || dstrect->w == z * 3) &&
                                   (dstrect->h == z || dstrect->h == z * 2 || dstrect->h == z * 3));
            if (is_map_element) {
                float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
                float fy = g_camera.current_frac_y.load(std::memory_order_relaxed);
                modified_dst.x -= static_cast<int>(std::round(fx * z));
                modified_dst.y -= static_cast<int>(std::round(fy * z));
            }
        }
    }
    return True_SDL_RenderCopy(renderer, texture, srcrect, dstrect ? &modified_dst : nullptr);
}

int Hook_SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip) {
    SDL_Rect modified_dst;
    if (dstrect) {
        modified_dst = *dstrect;
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0) {
            bool is_map_element = ((dstrect->w == z || dstrect->w == z * 2 || dstrect->w == z * 3) &&
                                   (dstrect->h == z || dstrect->h == z * 2 || dstrect->h == z * 3));
            if (is_map_element) {
                float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
                float fy = g_camera.current_frac_y.load(std::memory_order_relaxed);
                modified_dst.x -= static_cast<int>(std::round(fx * z));
                modified_dst.y -= static_cast<int>(std::round(fy * z));
            }
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect ? &modified_dst : nullptr, angle, center, flip);
}

bool InitSDLHooks() {
    if (MH_Initialize() != MH_OK) return false;
    
    HMODULE sdl_module = GetModuleHandleA("SDL2.dll");
    if (!sdl_module) return false;
    
    void* render_copy_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopy");
    void* render_copy_ex_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyEx");
    void* render_present_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderPresent");
    GetRendererOutputSize_func = (SDL_GetRendererOutputSize_t)GetProcAddress(sdl_module, "SDL_GetRendererOutputSize");
    
    if (render_copy_addr) MH_CreateHook(render_copy_addr, &Hook_SDL_RenderCopy, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopy));
    if (render_copy_ex_addr) MH_CreateHook(render_copy_ex_addr, &Hook_SDL_RenderCopyEx, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyEx));
    if (render_present_addr) MH_CreateHook(render_present_addr, &Hook_SDL_RenderPresent, reinterpret_cast<LPVOID*>(&True_SDL_RenderPresent));
    
    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

void CleanupSDLHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

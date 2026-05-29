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

SDL_PollEvent_t True_SDL_PollEvent = nullptr;
SDL_RenderCopy_t True_SDL_RenderCopy = nullptr;
SDL_RenderCopyEx_t True_SDL_RenderCopyEx = nullptr;
SDL_GetRendererOutputSize_t GetRendererOutputSize_func = nullptr;

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
        } else { // Normal scroll for Zoom
            if (event->wheel.y > 0) {
                g_camera.zoom_in();
            } else if (event->wheel.y < 0) {
                g_camera.zoom_out();
            }
            event->type = SDL_FIRSTEVENT;
        }
    }
    return result;
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
                float frac_x = g_camera.true_x - std::floor(g_camera.true_x);
                float frac_y = g_camera.true_y - std::floor(g_camera.true_y);
                modified_dst.x -= static_cast<int>(frac_x * z);
                modified_dst.y -= static_cast<int>(frac_y * z);

                float sdl_scale = g_camera.true_zoom / static_cast<float>(z);
                if (std::abs(sdl_scale - 1.0f) > 0.001f && GetRendererOutputSize_func) {
                    int win_w = 1920, win_h = 1080;
                    GetRendererOutputSize_func(renderer, &win_w, &win_h);
                    float cx = win_w / 2.0f;
                    float cy = win_h / 2.0f;
                    float dx = modified_dst.x - cx;
                    float dy = modified_dst.y - cy;
                    modified_dst.x = static_cast<int>(std::round(cx + dx * sdl_scale));
                    modified_dst.y = static_cast<int>(std::round(cy + dy * sdl_scale));
                    modified_dst.w = static_cast<int>(std::ceil(modified_dst.w * sdl_scale));
                    modified_dst.h = static_cast<int>(std::ceil(modified_dst.h * sdl_scale));
                }
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
                float frac_x = g_camera.true_x - std::floor(g_camera.true_x);
                float frac_y = g_camera.true_y - std::floor(g_camera.true_y);
                modified_dst.x -= static_cast<int>(frac_x * z);
                modified_dst.y -= static_cast<int>(frac_y * z);

                float sdl_scale = g_camera.true_zoom / static_cast<float>(z);
                if (std::abs(sdl_scale - 1.0f) > 0.001f && GetRendererOutputSize_func) {
                    int win_w = 1920, win_h = 1080;
                    GetRendererOutputSize_func(renderer, &win_w, &win_h);
                    float cx = win_w / 2.0f;
                    float cy = win_h / 2.0f;
                    float dx = modified_dst.x - cx;
                    float dy = modified_dst.y - cy;
                    modified_dst.x = static_cast<int>(std::round(cx + dx * sdl_scale));
                    modified_dst.y = static_cast<int>(std::round(cy + dy * sdl_scale));
                    modified_dst.w = static_cast<int>(std::ceil(modified_dst.w * sdl_scale));
                    modified_dst.h = static_cast<int>(std::ceil(modified_dst.h * sdl_scale));
                }
            }
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect ? &modified_dst : nullptr, angle, center, flip);
}

bool InitSDLHooks() {
    if (MH_Initialize() != MH_OK) return false;
    
    HMODULE sdl_module = GetModuleHandleA("SDL2.dll");
    if (!sdl_module) return false;
    
    void* poll_event_addr = (void*)GetProcAddress(sdl_module, "SDL_PollEvent");
    void* render_copy_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopy");
    void* render_copy_ex_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyEx");
    GetRendererOutputSize_func = (SDL_GetRendererOutputSize_t)GetProcAddress(sdl_module, "SDL_GetRendererOutputSize");
    
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

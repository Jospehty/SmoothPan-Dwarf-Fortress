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
#include "df/graphic_viewportst.h"
#include <mutex>

typedef int(*SDL_RenderCopy_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect);
typedef int(*SDL_RenderCopyEx_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip);
typedef int(*SDL_RenderCopyF_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect);
typedef int(*SDL_RenderCopyExF_t)(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_FRect* dstrect, const double angle, const SDL_FPoint* center, const SDL_RendererFlip flip);
typedef int(*SDL_GetRendererOutputSize_t)(SDL_Renderer* renderer, int* w, int* h);
typedef void(*SDL_RenderPresent_t)(SDL_Renderer* renderer);
typedef uint32_t(*SDL_GetMouseState_t)(int*, int*);
typedef int(*SDL_RenderSetClipRect_t)(SDL_Renderer*, const SDL_Rect*);

SDL_RenderCopy_t True_SDL_RenderCopy = nullptr;
SDL_RenderCopyEx_t True_SDL_RenderCopyEx = nullptr;
SDL_RenderCopyF_t True_SDL_RenderCopyF = nullptr;
SDL_RenderCopyExF_t True_SDL_RenderCopyExF = nullptr;
SDL_GetRendererOutputSize_t GetRendererOutputSize_func = nullptr;
SDL_RenderPresent_t True_SDL_RenderPresent = nullptr;
SDL_GetMouseState_t GetMouseState_func = nullptr;
SDL_RenderSetClipRect_t True_SDL_RenderSetClipRect = nullptr;

extern bool IsMouseInUI(int mx, int my);
extern bool IsRectInUI(const SDL_Rect* r);

void apply_mouse_shift(int& mx, int& my) {
    if (!df::global::gps || !df::global::gps->main_viewport) return;
    int z = df::global::gps->viewport_zoom_factor;
    if (z <= 0) return;
    
    int tile_size = z / 4;
    float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
    float fy = g_camera.current_frac_y.load(std::memory_order_relaxed);
    
    mx += static_cast<int>(std::round(fx * tile_size));
    my += static_cast<int>(std::round(fy * tile_size));
}

uint32_t Hook_SDL_GetMouseState(int* x, int* y) {
    uint32_t state = GetMouseState_func(x, y);
    if (x && y) {
        bool in_ui = IsMouseInUI(*x, *y);
        
        static int log_counter = 0;
        if (log_counter++ % 120 == 0) {
            FILE* f = fopen("smoothpan_debug.log", "a");
            if (f) {
                float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
                fprintf(f, "[Mouse] raw=(%d,%d) in_ui=%d frac=(%.2f) shifted=(%d,%d)\n", 
                        *x, *y, in_ui, fx, 
                        in_ui ? *x : *x + (int)std::round(fx * (df::global::gps->viewport_zoom_factor / 4)),
                        in_ui ? *y : *y + (int)std::round(g_camera.current_frac_y.load() * (df::global::gps->viewport_zoom_factor / 4)));
                fclose(f);
            }
        }

        if (!in_ui) {
            apply_mouse_shift(*x, *y);
        }
    }
    return state;
}

int Hook_SDL_RenderSetClipRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (rect && df::global::gps && df::global::gps->main_viewport) {
        auto vp = df::global::gps->main_viewport;
        int z = df::global::gps->viewport_zoom_factor;
        int cell_size = z / 4;
        int vp_w = vp->dim_x * cell_size;
        int vp_h = vp->dim_y * cell_size;

        if (std::abs(rect->x - vp->screen_x) <= 2 &&
            std::abs(rect->y - vp->screen_y) <= 2 &&
            std::abs(rect->w - vp_w) <= z &&
            std::abs(rect->h - vp_h) <= z) {

            SDL_Rect expanded = *rect;
            expanded.x -= z;
            expanded.y -= z;
            expanded.w += z * 2;
            expanded.h += z * 2;
            return True_SDL_RenderSetClipRect(renderer, &expanded);
        }
    }
    return True_SDL_RenderSetClipRect(renderer, rect);
}

void Hook_SDL_RenderPresent(SDL_Renderer* renderer) {
    g_camera.current_frac_x.store(g_camera.next_frac_x.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_camera.current_frac_y.store(g_camera.next_frac_y.load(std::memory_order_relaxed), std::memory_order_relaxed);
    True_SDL_RenderPresent(renderer);
}

int Hook_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) {
    if (dstrect) {
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0 && df::global::gps && df::global::gps->main_viewport) {
            auto vp = df::global::gps->main_viewport;
            int tile_size = z / 4;
            int vp_left = vp->screen_x;
            int vp_top = vp->screen_y;
            int vp_right = vp_left + vp->dim_x * tile_size;
            int vp_bottom = vp_top + vp->dim_y * tile_size;
            
            bool is_tile_size = ((dstrect->w == tile_size || dstrect->w == tile_size * 2 || dstrect->w == tile_size * 3) &&
                                 (dstrect->h == tile_size || dstrect->h == tile_size * 2 || dstrect->h == tile_size * 3));
            bool inside_vp = (dstrect->x >= vp_left - tile_size * 4 &&
                              dstrect->x + dstrect->w <= vp_right + tile_size * 4 &&
                              dstrect->y >= vp_top - tile_size * 4 &&
                              dstrect->y + dstrect->h <= vp_bottom + tile_size * 4);
                              
            bool is_map_element = is_tile_size && inside_vp && !IsRectInUI(dstrect);
                                   
            if (is_map_element && True_SDL_RenderCopyF) {
                SDL_FRect fdstr;
                fdstr.w = static_cast<float>(dstrect->w);
                fdstr.h = static_cast<float>(dstrect->h);
                float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
                float fy = g_camera.current_frac_y.load(std::memory_order_relaxed);
                fdstr.x = static_cast<float>(dstrect->x) - (fx * static_cast<float>(tile_size));
                fdstr.y = static_cast<float>(dstrect->y) - (fy * static_cast<float>(tile_size));
                return True_SDL_RenderCopyF(renderer, texture, srcrect, &fdstr);
            }
        }
    }
    return True_SDL_RenderCopy(renderer, texture, srcrect, dstrect);
}

int Hook_SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect, const double angle, const SDL_Point* center, const SDL_RendererFlip flip) {
    if (dstrect) {
        int z = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
        if (z > 0 && df::global::gps && df::global::gps->main_viewport) {
            auto vp = df::global::gps->main_viewport;
            int tile_size = z / 4;
            int vp_left = vp->screen_x;
            int vp_top = vp->screen_y;
            int vp_right = vp_left + vp->dim_x * tile_size;
            int vp_bottom = vp_top + vp->dim_y * tile_size;
            
            bool is_tile_size = ((dstrect->w == tile_size || dstrect->w == tile_size * 2 || dstrect->w == tile_size * 3) &&
                                 (dstrect->h == tile_size || dstrect->h == tile_size * 2 || dstrect->h == tile_size * 3));
            bool inside_vp = (dstrect->x >= vp_left - tile_size * 4 &&
                              dstrect->x + dstrect->w <= vp_right + tile_size * 4 &&
                              dstrect->y >= vp_top - tile_size * 4 &&
                              dstrect->y + dstrect->h <= vp_bottom + tile_size * 4);
                              
            bool is_map_element = is_tile_size && inside_vp && !IsRectInUI(dstrect);
                                   
            if (is_map_element && True_SDL_RenderCopyExF) {
                SDL_FRect fdstr;
                fdstr.w = static_cast<float>(dstrect->w);
                fdstr.h = static_cast<float>(dstrect->h);
                float fx = g_camera.current_frac_x.load(std::memory_order_relaxed);
                float fy = g_camera.current_frac_y.load(std::memory_order_relaxed);
                fdstr.x = static_cast<float>(dstrect->x) - (fx * static_cast<float>(tile_size));
                fdstr.y = static_cast<float>(dstrect->y) - (fy * static_cast<float>(tile_size));
                
                SDL_FPoint fcenter;
                if (center) {
                    fcenter.x = static_cast<float>(center->x);
                    fcenter.y = static_cast<float>(center->y);
                }
                
                return True_SDL_RenderCopyExF(renderer, texture, srcrect, &fdstr, angle, center ? &fcenter : nullptr, flip);
            }
        }
    }
    return True_SDL_RenderCopyEx(renderer, texture, srcrect, dstrect, angle, center, flip);
}

bool InitSDLHooks() {
    if (MH_Initialize() != MH_OK) return false;
    
    HMODULE sdl_module = GetModuleHandleA("SDL2.dll");
    if (!sdl_module) return false;
    
    void* render_copy_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopy");
    void* render_copy_ex_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderCopyEx");
    void* render_present_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderPresent");
    void* get_mouse_state_addr = (void*)GetProcAddress(sdl_module, "SDL_GetMouseState");
    void* render_set_clip_addr = (void*)GetProcAddress(sdl_module, "SDL_RenderSetClipRect");
    GetRendererOutputSize_func = (SDL_GetRendererOutputSize_t)GetProcAddress(sdl_module, "SDL_GetRendererOutputSize");
    
    True_SDL_RenderCopyF = (SDL_RenderCopyF_t)GetProcAddress(sdl_module, "SDL_RenderCopyF");
    True_SDL_RenderCopyExF = (SDL_RenderCopyExF_t)GetProcAddress(sdl_module, "SDL_RenderCopyExF");
    
    if (render_copy_addr) MH_CreateHook(render_copy_addr, &Hook_SDL_RenderCopy, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopy));
    if (render_copy_ex_addr) MH_CreateHook(render_copy_ex_addr, &Hook_SDL_RenderCopyEx, reinterpret_cast<LPVOID*>(&True_SDL_RenderCopyEx));
    if (render_present_addr) MH_CreateHook(render_present_addr, &Hook_SDL_RenderPresent, reinterpret_cast<LPVOID*>(&True_SDL_RenderPresent));
    if (get_mouse_state_addr) MH_CreateHook(get_mouse_state_addr, &Hook_SDL_GetMouseState, reinterpret_cast<LPVOID*>(&GetMouseState_func));
    if (render_set_clip_addr) MH_CreateHook(render_set_clip_addr, &Hook_SDL_RenderSetClipRect, reinterpret_cast<LPVOID*>(&True_SDL_RenderSetClipRect));
    
    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

void CleanupSDLHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

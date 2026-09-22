#pragma once

// Platform layer (3.25.0): everything OS-specific lives behind this header.
//
//   Windows : GetAsyncKeyState, QueryPerformanceCounter, SDL2.dll exports,
//             MinHook inline hooks on the SDL2.dll entry points.
//   Linux   : SDL keyboard/mouse state, clock_gettime, the libSDL2 that DF
//             loaded, and hooks installed by rewriting the GOT slots through
//             which DF's own modules call SDL (dl_iterate_phdr + ELF dynamic
//             relocations).  SDL's own code is never patched.

#include <cstdint>
#include <cstddef>
#include <string>

const char* sp_platform_name();

// ---- high-resolution timer --------------------------------------------------
uint64_t sp_perf_counter();
uint64_t sp_perf_frequency();
inline double sp_perf_us(uint64_t t0, uint64_t t1) {
    return static_cast<double>(t1 - t0) * 1e6 / static_cast<double>(sp_perf_frequency());
}

// ---- SDL symbols ------------------------------------------------------------
// Address of a real SDL2 function in the SDL2 library DF loaded (never one of
// our hooks on Linux; on Windows a hooked function's entry jumps to the hook,
// so call hooked functions through their True_ trampolines instead).
void* sp_sdl_sym(const char* name);
// Path/name of that SDL2 library ("" when not found).
const char* sp_sdl_module_name();
// Symbol from SDL2_image if DF loaded it (e.g. IMG_SavePNG), else nullptr.
void* sp_sdl_image_sym(const char* name);

// ---- physical input state ---------------------------------------------------
enum class SpKey {
    W, A, S, D,
    Up, Down, Left, Right,
    Kp8, Kp2, Kp4, Kp6,
    F7, F8, F9, F10, F11, F12,
};
bool sp_key_down(SpKey k);
bool sp_mouse_middle_down();
const char* sp_input_backend();

// ---- SDL function hooks -----------------------------------------------------
// Usage: sp_hooks_begin(); sp_hook_add(...) for each; sp_hooks_commit();
// sp_hooks_end() removes everything.  *original receives the function to call
// to reach real SDL (trampoline on Windows, the real function on Linux).
bool sp_hooks_begin();
bool sp_hook_add(const char* sdl_name, void* detour, void** original);
bool sp_hooks_commit();
void sp_hooks_end();
bool sp_hooks_active();
// Human-readable per-hook status (which module slots were patched, etc.).
void sp_hooks_report(std::string& out);
// Number of call sites patched for one hooked function (Linux: GOT slots in
// DF modules; Windows: 1 when the inline hook is enabled).
int sp_hook_sites(const char* sdl_name);

#include "vblank.h"

#include "debug_paths.h"
#include "platform.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {

typedef SDL_Window* (*GetWindow_t)(SDL_Renderer*);
typedef int (*DisplayIndex_t)(SDL_Window*);
typedef int (*CurMode_t)(int, SDL_DisplayMode*);

// GLX, resolved from the libGL DF's SDL already loaded.
typedef void* (*GetCurrentDisplay_t)();
typedef unsigned long (*GetCurrentDrawable_t)();
typedef const char* (*QueryExtStr_t)(void*, int);
typedef int (*GetSyncValuesOML_t)(void*, unsigned long, int64_t*, int64_t*, int64_t*);
typedef int (*GetVideoSyncSGI_t)(unsigned int*);

struct Glx {
    bool tried = false;
    GetCurrentDisplay_t cur_dpy = nullptr;
    GetCurrentDrawable_t cur_draw = nullptr;
    QueryExtStr_t ext = nullptr;
    GetSyncValuesOML_t oml = nullptr;
    GetVideoSyncSGI_t sgi = nullptr;
} g;

long long g_refresh_us = 0;
int g_refresh_hz = 0;
int g_display = -1;
int g_probe_left = 0;
FILE* g_f = nullptr;

long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void* glx_sym(const char* name) {
#if !defined(_WIN32)
    static void* libs[3] = { nullptr, nullptr, nullptr };
    static bool opened = false;
    if (!opened) {
        opened = true;
        libs[0] = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        libs[1] = dlopen("libGLX.so.0", RTLD_NOW | RTLD_NOLOAD);
        libs[2] = dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
    }
    for (void* h : libs)
        if (h)
            if (void* p = dlsym(h, name)) return p;
    return dlsym(RTLD_DEFAULT, name);
#else
    (void)name;
    return nullptr;
#endif
}

void resolve_glx() {
    if (g.tried) return;
    g.tried = true;
    g.cur_dpy = reinterpret_cast<GetCurrentDisplay_t>(glx_sym("glXGetCurrentDisplay"));
    g.cur_draw = reinterpret_cast<GetCurrentDrawable_t>(glx_sym("glXGetCurrentDrawable"));
    g.ext = reinterpret_cast<QueryExtStr_t>(glx_sym("glXQueryExtensionsString"));
    g.oml = reinterpret_cast<GetSyncValuesOML_t>(glx_sym("glXGetSyncValuesOML"));
    g.sgi = reinterpret_cast<GetVideoSyncSGI_t>(glx_sym("glXGetVideoSyncSGI"));
}

void read_display(SDL_Renderer* r) {
    static GetWindow_t gw = reinterpret_cast<GetWindow_t>(sp_sdl_sym("SDL_RenderGetWindow"));
    static DisplayIndex_t di = reinterpret_cast<DisplayIndex_t>(sp_sdl_sym("SDL_GetWindowDisplayIndex"));
    static CurMode_t cm = reinterpret_cast<CurMode_t>(sp_sdl_sym("SDL_GetCurrentDisplayMode"));
    if (!gw || !di || !cm) return;
    SDL_Window* w = gw(r);
    if (!w) return;
    const int idx = di(w);
    SDL_DisplayMode m;
    if (idx < 0 || cm(idx, &m) != 0 || m.refresh_rate <= 0) return;
    g_display = idx;
    g_refresh_hz = m.refresh_rate;
    g_refresh_us = 1000000LL / m.refresh_rate;
}

}  // namespace

void vblank_probe(int presents) {
    g_probe_left = presents;
}

long long vblank_refresh_us() { return g_refresh_us; }

void vblank_status(char* buf, size_t n) {
    if (!buf || !n) return;
    snprintf(buf, n, "display=%d refresh=%dHz (%lld us) glx_oml=%d glx_sgi=%d",
             g_display, g_refresh_hz, g_refresh_us, g.oml ? 1 : 0, g.sgi ? 1 : 0);
}

void vblank_on_present(SDL_Renderer* r) {
    static int tick = 0;
    if (!r) return;
    if ((tick++ % 300) == 0) read_display(r);   // cheap; the window can move displays
    if (g_probe_left <= 0) return;

    resolve_glx();
    if (!g_f) {
        g_f = fopen(smoothpan_log_path("smoothpan_vblank.txt").c_str(), "a");
        if (!g_f) { g_probe_left = 0; return; }
        read_display(r);
        fprintf(g_f, "# vblank probe: display=%d refresh=%dHz\n", g_display, g_refresh_hz);
        fprintf(g_f, "# glx: cur_dpy=%p cur_draw=%p ext=%p oml=%p sgi=%p\n",
                reinterpret_cast<void*>(g.cur_dpy), reinterpret_cast<void*>(g.cur_draw),
                reinterpret_cast<void*>(g.ext), reinterpret_cast<void*>(g.oml),
                reinterpret_cast<void*>(g.sgi));
        void* dpy = g.cur_dpy ? g.cur_dpy() : nullptr;
        if (dpy && g.ext) {
            const char* e = g.ext(dpy, 0);
            fprintf(g_f, "# glx extensions: %s\n", e ? e : "(null)");
        } else {
            fprintf(g_f, "# no current GLX display (EGL or not GL)\n");
        }
    }
    void* dpy = g.cur_dpy ? g.cur_dpy() : nullptr;
    unsigned long draw = g.cur_draw ? g.cur_draw() : 0;
    int64_t ust = -1, msc = -1, sbc = -1;
    int oml_ok = 0;
    if (g.oml && dpy && draw) oml_ok = g.oml(dpy, draw, &ust, &msc, &sbc);
    unsigned int sgi = 0;
    int sgi_rc = -1;
    if (g.sgi && dpy) sgi_rc = g.sgi(&sgi);
    fprintf(g_f, "t=%lld oml=%d ust=%lld msc=%lld sbc=%lld sgi_rc=%d sgi=%u\n",
            now_us(), oml_ok, static_cast<long long>(ust), static_cast<long long>(msc),
            static_cast<long long>(sbc), sgi_rc, sgi);
    if (--g_probe_left == 0) { fclose(g_f); g_f = nullptr; }
}

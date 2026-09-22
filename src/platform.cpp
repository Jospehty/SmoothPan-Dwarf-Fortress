#include "platform.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct HookSpec {
    std::string name;
    void* detour = nullptr;
    void** original = nullptr;
    void* target = nullptr;   // SDL entry point (Windows) / real function (Linux)
    int sites = 0;
    std::string where;        // modules patched (Linux) / status (Windows)
};

static std::vector<HookSpec> g_specs;
static bool g_hooks_active = false;

}  // namespace

#if defined(_WIN32)
// =============================================================================
// Windows
// =============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#include "MinHook.h"

const char* sp_platform_name() { return "windows"; }

uint64_t sp_perf_counter() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

uint64_t sp_perf_frequency() {
    static uint64_t f = 0;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = static_cast<uint64_t>(q.QuadPart);
        if (!f) f = 1;
    }
    return f;
}

static HMODULE sdl_module() {
    static HMODULE m = nullptr;
    if (!m) m = GetModuleHandleA("SDL2.dll");
    return m;
}

void* sp_sdl_sym(const char* name) {
    HMODULE m = sdl_module();
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

void* sp_sdl_image_sym(const char* name) {
    HMODULE m = GetModuleHandleA("SDL2_image.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

const char* sp_sdl_module_name() {
    static char path[MAX_PATH] = "";
    if (!path[0] && sdl_module()) GetModuleFileNameA(sdl_module(), path, MAX_PATH);
    return path;
}

bool sp_key_down(SpKey k) {
    int vk = 0;
    switch (k) {
    case SpKey::W: vk = 'W'; break;
    case SpKey::A: vk = 'A'; break;
    case SpKey::S: vk = 'S'; break;
    case SpKey::D: vk = 'D'; break;
    case SpKey::Up: vk = VK_UP; break;
    case SpKey::Down: vk = VK_DOWN; break;
    case SpKey::Left: vk = VK_LEFT; break;
    case SpKey::Right: vk = VK_RIGHT; break;
    case SpKey::Kp8: vk = VK_NUMPAD8; break;
    case SpKey::Kp2: vk = VK_NUMPAD2; break;
    case SpKey::Kp4: vk = VK_NUMPAD4; break;
    case SpKey::Kp6: vk = VK_NUMPAD6; break;
    case SpKey::F7: vk = VK_F7; break;
    case SpKey::F8: vk = VK_F8; break;
    case SpKey::F9: vk = VK_F9; break;
    case SpKey::F10: vk = VK_F10; break;
    case SpKey::F11: vk = VK_F11; break;
    case SpKey::F12: vk = VK_F12; break;
    }
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool sp_mouse_middle_down() { return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0; }
const char* sp_input_backend() { return "GetAsyncKeyState"; }

bool sp_hooks_begin() {
    g_specs.clear();
    if (MH_Initialize() != MH_OK) return false;
    return sdl_module() != nullptr;
}

bool sp_hook_add(const char* sdl_name, void* detour, void** original) {
    HookSpec s;
    s.name = sdl_name;
    s.detour = detour;
    s.original = original;
    s.target = sp_sdl_sym(sdl_name);
    if (!s.target) { s.where = "missing export"; g_specs.push_back(s); return false; }
    MH_STATUS st = MH_CreateHook(s.target, detour, reinterpret_cast<LPVOID*>(original));
    s.where = (st == MH_OK) ? "inline" : MH_StatusToString(st);
    if (st == MH_OK) s.sites = 1;
    g_specs.push_back(s);
    return st == MH_OK;
}

bool sp_hooks_commit() {
    g_hooks_active = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    return g_hooks_active;
}

void sp_hooks_end() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_hooks_active = false;
}

#elif defined(__linux__)
// =============================================================================
// Linux
// =============================================================================
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

const char* sp_platform_name() { return "linux"; }

uint64_t sp_perf_counter() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t sp_perf_frequency() { return 1000000000ull; }

namespace {

static std::string g_sdl_path;
static void* g_sdl_handle = nullptr;
static bool g_sdl_searched = false;

// Core SDL2 library only (not SDL2_image / SDL2_ttf / SDL2_mixer).
static bool is_core_sdl_name(const char* path) {
    if (!path || !*path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strncmp(base, "libSDL2-2.0.so", 14) == 0 || strncmp(base, "libSDL2.so", 10) == 0;
}

static bool is_any_sdl_name(const char* path) {
    if (!path || !*path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strncmp(base, "libSDL2", 7) == 0;
}

static int find_sdl_cb(dl_phdr_info* info, size_t, void*) {
    if (is_core_sdl_name(info->dlpi_name)) {
        g_sdl_path = info->dlpi_name;
        return 1;
    }
    return 0;
}

static void locate_sdl() {
    if (g_sdl_searched) return;
    g_sdl_searched = true;
    dl_iterate_phdr(find_sdl_cb, nullptr);
    if (!g_sdl_path.empty())
        g_sdl_handle = dlopen(g_sdl_path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!g_sdl_handle) {
        const char* names[] = { "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so" };
        for (const char* n : names) {
            g_sdl_handle = dlopen(n, RTLD_NOW | RTLD_NOLOAD);
            if (g_sdl_handle) { if (g_sdl_path.empty()) g_sdl_path = n; break; }
        }
    }
}

}  // namespace

void* sp_sdl_sym(const char* name) {
    locate_sdl();
    void* p = g_sdl_handle ? dlsym(g_sdl_handle, name) : nullptr;
    if (!p) p = dlsym(RTLD_DEFAULT, name);   // SDL linked into the executable
    return p;
}

const char* sp_sdl_module_name() {
    locate_sdl();
    return g_sdl_path.c_str();
}

namespace {
static std::string g_img_path;
static int find_img_cb(dl_phdr_info* info, size_t, void*) {
    const char* p = info->dlpi_name;
    if (p && strstr(p, "libSDL2_image")) { g_img_path = p; return 1; }
    return 0;
}
}

void* sp_sdl_image_sym(const char* name) {
    static void* h = nullptr;
    static bool searched = false;
    if (!searched) {
        searched = true;
        dl_iterate_phdr(find_img_cb, nullptr);
        if (!g_img_path.empty()) h = dlopen(g_img_path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libSDL2_image-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    }
    void* p = h ? dlsym(h, name) : nullptr;
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

namespace {
typedef const Uint8* (*SP_GetKeyboardState_t)(int*);
typedef Uint32 (*SP_GetMouseState_t)(int*, int*);
}

bool sp_key_down(SpKey k) {
    static SP_GetKeyboardState_t get_kb = nullptr;
    if (!get_kb) get_kb = reinterpret_cast<SP_GetKeyboardState_t>(sp_sdl_sym("SDL_GetKeyboardState"));
    if (!get_kb) return false;
    int n = 0;
    const Uint8* st = get_kb(&n);
    if (!st) return false;
    int sc = 0;
    switch (k) {
    case SpKey::W: sc = SDL_SCANCODE_W; break;
    case SpKey::A: sc = SDL_SCANCODE_A; break;
    case SpKey::S: sc = SDL_SCANCODE_S; break;
    case SpKey::D: sc = SDL_SCANCODE_D; break;
    case SpKey::Up: sc = SDL_SCANCODE_UP; break;
    case SpKey::Down: sc = SDL_SCANCODE_DOWN; break;
    case SpKey::Left: sc = SDL_SCANCODE_LEFT; break;
    case SpKey::Right: sc = SDL_SCANCODE_RIGHT; break;
    case SpKey::Kp8: sc = SDL_SCANCODE_KP_8; break;
    case SpKey::Kp2: sc = SDL_SCANCODE_KP_2; break;
    case SpKey::Kp4: sc = SDL_SCANCODE_KP_4; break;
    case SpKey::Kp6: sc = SDL_SCANCODE_KP_6; break;
    case SpKey::F7: sc = SDL_SCANCODE_F7; break;
    case SpKey::F8: sc = SDL_SCANCODE_F8; break;
    case SpKey::F9: sc = SDL_SCANCODE_F9; break;
    case SpKey::F10: sc = SDL_SCANCODE_F10; break;
    case SpKey::F11: sc = SDL_SCANCODE_F11; break;
    case SpKey::F12: sc = SDL_SCANCODE_F12; break;
    }
    return sc > 0 && sc < n && st[sc] != 0;
}

bool sp_mouse_middle_down() {
    static SP_GetMouseState_t get_ms = nullptr;
    if (!get_ms) get_ms = reinterpret_cast<SP_GetMouseState_t>(sp_sdl_sym("SDL_GetMouseState"));
    if (!get_ms) return false;
    return (get_ms(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
}

const char* sp_input_backend() { return "SDL_GetKeyboardState"; }

// ---- GOT hooking -------------------------------------------------------------
namespace {

struct PatchedSlot {
    void** slot = nullptr;
    void* original = nullptr;
    void* detour = nullptr;
};
static std::vector<PatchedSlot> g_slots;

// Protection of the page holding addr, from /proc/self/maps (-1 if unknown).
static int page_prot(uintptr_t addr) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int prot = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = "";
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= lo && addr < hi) {
            prot = 0;
            if (perms[0] == 'r') prot |= PROT_READ;
            if (perms[1] == 'w') prot |= PROT_WRITE;
            if (perms[2] == 'x') prot |= PROT_EXEC;
            break;
        }
    }
    fclose(f);
    return prot;
}

static bool write_slot(void** slot, void* value) {
    const uintptr_t pagesz = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    const uintptr_t page = addr & ~(pagesz - 1);
    int prot = page_prot(addr);
    if (prot < 0) prot = PROT_READ;
    if (!(prot & PROT_WRITE)) {
        if (mprotect(reinterpret_cast<void*>(page), pagesz, prot | PROT_WRITE) != 0) return false;
    }
    __atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
    if (!(prot & PROT_WRITE)) mprotect(reinterpret_cast<void*>(page), pagesz, prot);
    return true;
}

struct IterCtx {
    uintptr_t self_base = 0;
    int modules_scanned = 0;
};
static IterCtx g_iter;

static int patch_cb(dl_phdr_info* info, size_t, void* data) {
    IterCtx* ctx = static_cast<IterCtx*>(data);
    const char* name = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "<main>";
    if (is_any_sdl_name(info->dlpi_name)) return 0;           // never patch SDL itself
    if (strstr(name, "linux-vdso") || strstr(name, "ld-linux")) return 0;
    const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
    if (base == ctx->self_base) return 0;                      // our own plugin

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(base + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 0;
    ctx->modules_scanned++;

    // glibc relocates these d_ptr values in place for most objects; keep both
    // cases working (a raw offset is far below any load base).
    auto fix = [base](uintptr_t p) { return (base && p < base) ? p + base : p; };
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strsz = 0;
    const ElfW(Rela)* jmprel = nullptr;
    size_t jmprelsz = 0;
    const ElfW(Rela)* rela = nullptr;
    size_t relasz = 0;
    bool pltrel_is_rela = true;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(fix(d->d_un.d_ptr)); break;
        case DT_STRTAB: strtab = reinterpret_cast<const char*>(fix(d->d_un.d_ptr)); break;
        case DT_STRSZ: strsz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rela)*>(fix(d->d_un.d_ptr)); break;
        case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
        case DT_PLTREL: pltrel_is_rela = (d->d_un.d_val == DT_RELA); break;
        case DT_RELA: rela = reinterpret_cast<const ElfW(Rela)*>(fix(d->d_un.d_ptr)); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab) return 0;

    auto scan = [&](const ElfW(Rela)* table, size_t bytes) {
        if (!table || !bytes) return;
        const size_t n = bytes / sizeof(ElfW(Rela));
        for (size_t i = 0; i < n; ++i) {
            const ElfW(Rela)& r = table[i];
            const unsigned type = ELF64_R_TYPE(r.r_info);
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
            const size_t si = ELF64_R_SYM(r.r_info);
            if (!si) continue;
            const ElfW(Word) off = symtab[si].st_name;
            if (strsz && off >= strsz) continue;
            const char* sname = strtab + off;
            if (sname[0] != 'S' || sname[1] != 'D' || sname[2] != 'L') continue;
            for (HookSpec& s : g_specs) {
                if (s.name != sname || !s.detour) continue;
                void** slot = reinterpret_cast<void**>(base + r.r_offset);
                void* cur = *slot;
                if (cur == s.detour) break;   // already ours
                if (write_slot(slot, s.detour)) {
                    PatchedSlot ps;
                    ps.slot = slot;
                    ps.original = cur;
                    ps.detour = s.detour;
                    g_slots.push_back(ps);
                    s.sites++;
                    const char* b = strrchr(name, '/');
                    b = b ? b + 1 : name;
                    if (s.where.find(b) == std::string::npos) {
                        if (!s.where.empty()) s.where += ",";
                        s.where += b;
                    }
                }
                break;
            }
        }
    };
#if defined(__x86_64__)
    if (pltrel_is_rela) scan(jmprel, jmprelsz);
    scan(rela, relasz);
#endif
    return 0;
}

static uintptr_t self_base() {
    Dl_info di;
    if (dladdr(reinterpret_cast<void*>(&self_base), &di) && di.dli_fbase)
        return reinterpret_cast<uintptr_t>(di.dli_fbase);
    return 0;
}

}  // namespace

bool sp_hooks_begin() {
    g_specs.clear();
    locate_sdl();
    return sp_sdl_sym("SDL_RenderCopy") != nullptr;
}

bool sp_hook_add(const char* sdl_name, void* detour, void** original) {
    HookSpec s;
    s.name = sdl_name;
    s.detour = detour;
    s.original = original;
    s.target = sp_sdl_sym(sdl_name);
    if (original) *original = s.target;
    g_specs.push_back(s);
    return s.target != nullptr;
}

bool sp_hooks_commit() {
    g_iter = IterCtx{};
    g_iter.self_base = self_base();
    for (HookSpec& s : g_specs) {
        if (!s.target) { s.detour = nullptr; s.where = "missing export"; }
    }
    dl_iterate_phdr(patch_cb, &g_iter);
    g_hooks_active = true;
    int total = 0;
    for (const HookSpec& s : g_specs) total += s.sites;
    return total > 0;
}

void sp_hooks_end() {
    for (auto it = g_slots.rbegin(); it != g_slots.rend(); ++it) {
        if (*it->slot == it->detour) write_slot(it->slot, it->original);
    }
    g_slots.clear();
    g_hooks_active = false;
}

#else
#error "SmoothPan supports Windows and Linux only"
#endif

bool sp_hooks_active() { return g_hooks_active; }

int sp_hook_sites(const char* sdl_name) {
    for (const HookSpec& s : g_specs)
        if (s.name == sdl_name) return s.sites;
    return 0;
}

void sp_hooks_report(std::string& out) {
    char buf[512];
    snprintf(buf, sizeof(buf), "hooks: platform=%s active=%d sdl=%s\n",
             sp_platform_name(), g_hooks_active ? 1 : 0, sp_sdl_module_name());
    out += buf;
    for (const HookSpec& s : g_specs) {
        snprintf(buf, sizeof(buf), "  %-26s sites=%d target=%p %s\n",
                 s.name.c_str(), s.sites, s.target, s.where.c_str());
        out += buf;
    }
}

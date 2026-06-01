#include "mouse_comp.h"

#include <cstring>

MouseCompMode g_mouse_comp_mode = MouseCompMode::GpsOnly;

static MouseCompBothArmedFn g_both_armed_fn = nullptr;

const char* mouse_comp_mode_name(MouseCompMode mode) {
    switch (mode) {
        case MouseCompMode::None: return "none";
        case MouseCompMode::SdlOnly: return "sdl";
        case MouseCompMode::GpsOnly: return "gps";
        case MouseCompMode::Both: return "both";
    }
    return "gps";
}

const char* mouse_comp_effective_name() {
    return mouse_comp_mode_name(g_mouse_comp_mode);
}

MouseCompMode mouse_comp_mode_cycle_next(MouseCompMode current) {
    switch (current) {
        case MouseCompMode::GpsOnly: return MouseCompMode::Both;
        case MouseCompMode::Both: return MouseCompMode::GpsOnly;
        default: return MouseCompMode::GpsOnly;
    }
}

bool parse_mouse_comp_mode(const char* name, MouseCompMode* out) {
    if (!name || !out) return false;
    if (strcmp(name, "none") == 0 || strcmp(name, "0") == 0) {
        *out = MouseCompMode::None; return true;
    }
    if (strcmp(name, "sdl") == 0 || strcmp(name, "1") == 0) {
        *out = MouseCompMode::SdlOnly; return true;
    }
    if (strcmp(name, "gps") == 0 || strcmp(name, "2") == 0) {
        *out = MouseCompMode::GpsOnly; return true;
    }
    if (strcmp(name, "both") == 0 || strcmp(name, "3") == 0) {
        *out = MouseCompMode::Both; return true;
    }
    return false;
}

bool mouse_comp_sdl_enabled() {
    return g_mouse_comp_mode == MouseCompMode::SdlOnly
        || g_mouse_comp_mode == MouseCompMode::Both;
}

bool mouse_comp_gps_enabled() {
    return g_mouse_comp_mode == MouseCompMode::GpsOnly
        || g_mouse_comp_mode == MouseCompMode::Both;
}

void mouse_comp_set_both_armed_handler(MouseCompBothArmedFn fn) {
    g_both_armed_fn = fn;
}

void mouse_comp_notify_both_armed() {
    if (g_both_armed_fn) {
        g_both_armed_fn();
    }
}

void mouse_comp_boot_gps() {
    g_mouse_comp_mode = MouseCompMode::GpsOnly;
}

bool mouse_comp_arm_both_ui() {
    if (g_mouse_comp_mode == MouseCompMode::Both) return false;
    g_mouse_comp_mode = MouseCompMode::Both;
    mouse_comp_notify_both_armed();
    return true;
}

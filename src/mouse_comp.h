#pragma once

// Two compensation layers (Plan 2 / MOUSE_PLANS.md):
//   GPS bump  — world designation (feed/logic precise_mouse)
//   SDL hook  — UI widget hit tests (GetMouseState)
enum class MouseCompMode {
    None = 0,
    SdlOnly = 1,
    GpsOnly = 2,
    Both = 3,
};

extern MouseCompMode g_mouse_comp_mode;

const char* mouse_comp_mode_name(MouseCompMode mode);
const char* mouse_comp_effective_name();
bool parse_mouse_comp_mode(const char* name, MouseCompMode* out);

// Production toggle: gps (world) <-> both (world+UI).
MouseCompMode mouse_comp_mode_cycle_next(MouseCompMode current);

bool mouse_comp_sdl_enabled();
bool mouse_comp_gps_enabled();

typedef void (*MouseCompBothArmedFn)();
void mouse_comp_set_both_armed_handler(MouseCompBothArmedFn fn);
void mouse_comp_notify_both_armed();

void mouse_comp_boot_gps();
bool mouse_comp_arm_both_ui();

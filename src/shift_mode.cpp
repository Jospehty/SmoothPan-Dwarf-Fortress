#include "shift_mode.h"

#include <cstring>

ShiftMode g_shift_mode = ShiftMode::Sdl;

const char* shift_mode_name(ShiftMode mode) {
    switch (mode) {
        case ShiftMode::Sdl: return "sdl";
        case ShiftMode::None: return "none";
        case ShiftMode::MapPort: return "mapport";
        case ShiftMode::SeqPreToolbar: return "seqrange";
        case ShiftMode::Viewport: return "viewport";
    }
    return "sdl";
}

ShiftMode shift_mode_cycle_next(ShiftMode current) {
    switch (current) {
        case ShiftMode::Sdl: return ShiftMode::SeqPreToolbar;
        case ShiftMode::SeqPreToolbar: return ShiftMode::Viewport;
        case ShiftMode::Viewport: return ShiftMode::Sdl;
        default: return ShiftMode::Sdl;
    }
}

bool parse_shift_mode(const char* name, ShiftMode* out) {
    if (!name || !out) return false;
    if (strcmp(name, "sdl") == 0) { *out = ShiftMode::Sdl; return true; }
    if (strcmp(name, "none") == 0) { *out = ShiftMode::None; return true; }
    if (strcmp(name, "mapport") == 0) { *out = ShiftMode::MapPort; return true; }
    if (strcmp(name, "seqrange") == 0) { *out = ShiftMode::SeqPreToolbar; return true; }
    if (strcmp(name, "viewport") == 0) { *out = ShiftMode::Viewport; return true; }
    return false;
}

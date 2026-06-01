#pragma once

enum class ShiftMode {
    Sdl = 0,
    None = 1,
    MapPort = 2,
    SeqPreToolbar = 3,
    Viewport = 4,
};

extern ShiftMode g_shift_mode;

const char* shift_mode_name(ShiftMode mode);
bool parse_shift_mode(const char* name, ShiftMode* out);
ShiftMode shift_mode_cycle_next(ShiftMode current);

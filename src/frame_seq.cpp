#include "frame_seq.h"

#include "viewport.h"

#include <SDL.h>

static int g_global_seq = 0;
static int g_frame_base_seq = 0;
static int g_frame_index = 0;
static int g_first_above_rel = 0;

void frame_seq_reset_session() {
    g_global_seq = 0;
    g_frame_base_seq = 0;
    g_frame_index = 0;
    g_first_above_rel = 0;
}

void frame_seq_on_present() {
    g_frame_index++;
    g_frame_base_seq = g_global_seq;
    g_first_above_rel = 0;
}

int frame_seq_note_blit(int x, int y, int w, int h) {
    g_global_seq++;

    ViewportRect vp;
    if (get_strict_viewport_rect(&vp)) {
        SDL_Rect r = {x, y, w, h};
        bool strict_above = (r.y + r.h <= vp.top);
        if (strict_above && g_first_above_rel == 0) {
            g_first_above_rel = g_global_seq - g_frame_base_seq;
        }
    }

    return g_global_seq;
}

int frame_seq_global() {
    return g_global_seq;
}

int frame_seq_frame_index() {
    return g_frame_index;
}

int frame_seq_relative() {
    return g_global_seq - g_frame_base_seq;
}

int frame_seq_first_above_relative() {
    return g_first_above_rel;
}

bool frame_seq_shift_allowed() {
    if (g_first_above_rel <= 0) return true;
    return frame_seq_relative() < g_first_above_rel;
}

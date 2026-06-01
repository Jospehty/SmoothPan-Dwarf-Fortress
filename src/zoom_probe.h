#pragma once

#include "df/zoom_commands.h"

#include <cstdio>

// Phase 0 — smooth zoom discovery (no visual zoom yet).
// Records zoom ladder, input paths, and renderer call order for F9 / log file.

void zoom_probe_reset();

void zoom_probe_note_feed_zoom_in();
void zoom_probe_note_feed_zoom_out();

void zoom_probe_note_renderer_zoom(df::zoom_commands cmd);
void zoom_probe_note_set_viewport_zoom(int nfactor, int prev_z);

void zoom_probe_note_gps_z_change(int prev_z, int new_z);

void zoom_probe_write_f9(FILE* f);
void zoom_probe_flush_discovery_log();

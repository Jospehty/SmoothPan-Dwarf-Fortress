#pragma once

#include "camera.h"
#include "viewport.h"

extern int g_probe_frames;

void probe_reset_frame_counters();
void probe_note_clip_event();
void probe_note_target_event();
void probe_note_viewport_event();
void probe_accumulate_blit(const BlitClassification& c, bool shifted);
void probe_note_map_pass();

void probe_start_auto_cycle();
bool probe_auto_cycle_active();

void probe_on_present();

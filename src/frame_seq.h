#pragma once

void frame_seq_reset_session();
void frame_seq_on_present();
int frame_seq_note_blit(int x, int y, int w, int h);
int frame_seq_global();
int frame_seq_frame_index();
int frame_seq_relative();
int frame_seq_first_above_relative();
bool frame_seq_shift_allowed();

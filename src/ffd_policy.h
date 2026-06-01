#pragma once

enum class FfdPanPolicy { Always, Smart, Off };

void ffd_policy_set_mode(const char* mode);
FfdPanPolicy ffd_policy_get_mode();
const char* ffd_policy_mode_name(FfdPanPolicy mode);

// Refresh lower-z visibility cache (tile step or cache miss). Smart runs viewport scan.
void ffd_policy_ensure_cache(int window_x, int window_y, int window_z,
                             int half_width_tiles, int half_height_tiles,
                             bool force_rescan);

// Apply pan-time force_full_display_count>=2 per policy. Returns true if ffd was bumped.
bool ffd_policy_apply_pan(bool pan_active);

bool ffd_policy_last_pan_ffd_applied();
bool ffd_policy_last_pan_ffd_skipped();
const char* ffd_policy_last_reason();

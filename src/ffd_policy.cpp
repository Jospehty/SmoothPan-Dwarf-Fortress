#include "ffd_policy.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/world.h"

#include "modules/Maps.h"
#include "TileTypes.h"

#include <climits>
#include <cstring>

using namespace DFHack;

namespace {

FfdPanPolicy g_policy = FfdPanPolicy::Smart;

int g_cache_wx = INT_MIN;
int g_cache_wy = INT_MIN;
int g_cache_wz = INT_MIN;
bool g_cached_needs_pan_ffd = true;

bool g_last_applied = false;
bool g_last_skipped = false;
const char* g_last_reason = "init";

enum class ScanHit { None, Edge, Open, Ramp, Stair, Grate, Cliff };

const char* scan_hit_name(ScanHit hit) {
    switch (hit) {
    case ScanHit::Edge: return "edge";
    case ScanHit::Open: return "open";
    case ScanHit::Ramp: return "ramp";
    case ScanHit::Stair: return "stair";
    case ScanHit::Grate: return "grate";
    case ScanHit::Cliff: return "cliff";
    default: return "flat";
    }
}

ScanHit classify_tile(int x, int y, int z) {
    if (!Maps::isValidTilePos(x, y, z))
        return ScanHit::Edge;

    df::tiletype* tt = Maps::getTileType(x, y, z);
    if (!tt)
        return ScanHit::None;

    const df::tiletype t = *tt;
    if (isOpenTerrain(t) && z > 0)
        return ScanHit::Open;
    if (isRampTerrain(t))
        return ScanHit::Ramp;
    if (isStairTerrain(t))
        return ScanHit::Stair;
    if (isFloorTerrain(t) && FlowPassableDown(t))
        return ScanHit::Grate;

    static const int kDx[] = {0, 1, 0, -1};
    static const int kDy[] = {-1, 0, 1, 0};
    for (int i = 0; i < 4; ++i) {
        const int nx = x + kDx[i];
        const int ny = y + kDy[i];
        if (!Maps::isValidTilePos(nx, ny, z))
            return ScanHit::Edge;
        df::tiletype* nt = Maps::getTileType(nx, ny, z);
        if (nt && isOpenTerrain(*nt))
            return ScanHit::Cliff;
    }
    return ScanHit::None;
}

bool viewport_needs_lower_z_rebake(int wx, int wy, int wz,
                                   int half_w, int half_h,
                                   ScanHit* first_hit) {
    if (!df::global::world || !df::global::gps)
        return true;

    const int x0 = wx - half_w;
    const int x1 = wx + half_w;
    const int y0 = wy - half_h;
    const int y1 = wy + half_h;

    auto note = [&](ScanHit hit) -> bool {
        if (hit != ScanHit::None) {
            if (first_hit && *first_hit == ScanHit::None)
                *first_hit = hit;
            return true;
        }
        return false;
    };

    for (int x = x0; x <= x1; ++x) {
        if (note(classify_tile(x, y0, wz))) return true;
        if (note(classify_tile(x, y1, wz))) return true;
    }
    for (int y = y0 + 1; y < y1; ++y) {
        if (note(classify_tile(x0, y, wz))) return true;
        if (note(classify_tile(x1, y, wz))) return true;
    }
    for (int y = y0 + 1; y < y1; y += 2) {
        for (int x = x0 + 1; x < x1; x += 2) {
            if (note(classify_tile(x, y, wz)))
                return true;
        }
    }
    return false;
}

void refresh_cache(int wx, int wy, int wz, int half_w, int half_h) {
    g_cache_wx = wx;
    g_cache_wy = wy;
    g_cache_wz = wz;

    switch (g_policy) {
    case FfdPanPolicy::Always:
        g_cached_needs_pan_ffd = true;
        g_last_reason = "always";
        break;
    case FfdPanPolicy::Off:
        g_cached_needs_pan_ffd = false;
        g_last_reason = "off";
        break;
    case FfdPanPolicy::Smart: {
        ScanHit hit = ScanHit::None;
        g_cached_needs_pan_ffd =
            viewport_needs_lower_z_rebake(wx, wy, wz, half_w, half_h, &hit);
        g_last_reason = g_cached_needs_pan_ffd ? scan_hit_name(hit) : "flat";
        break;
    }
    }
}

} // namespace

void ffd_policy_set_mode(const char* mode) {
    if (!mode) return;
    if (strcmp(mode, "always") == 0) {
        g_policy = FfdPanPolicy::Always;
    } else if (strcmp(mode, "smart") == 0) {
        g_policy = FfdPanPolicy::Smart;
    } else if (strcmp(mode, "off") == 0) {
        g_policy = FfdPanPolicy::Off;
    }
    g_cache_wx = INT_MIN;
    g_cache_wy = INT_MIN;
    g_cache_wz = INT_MIN;
}

FfdPanPolicy ffd_policy_get_mode() {
    return g_policy;
}

const char* ffd_policy_mode_name(FfdPanPolicy mode) {
    switch (mode) {
    case FfdPanPolicy::Always: return "always";
    case FfdPanPolicy::Smart: return "smart";
    case FfdPanPolicy::Off: return "off";
    }
    return "unknown";
}

void ffd_policy_ensure_cache(int window_x, int window_y, int window_z,
                             int half_width_tiles, int half_height_tiles,
                             bool force_rescan) {
    if (force_rescan ||
        window_x != g_cache_wx || window_y != g_cache_wy || window_z != g_cache_wz) {
        refresh_cache(window_x, window_y, window_z,
                      half_width_tiles, half_height_tiles);
    }
}

bool ffd_policy_apply_pan(bool pan_active) {
    g_last_applied = false;
    g_last_skipped = false;

    if (!pan_active || !df::global::gps)
        return false;

    if (g_policy == FfdPanPolicy::Off) {
        g_last_skipped = true;
        g_last_reason = "off";
        return false;
    }

    if (g_policy == FfdPanPolicy::Always || g_cached_needs_pan_ffd) {
        if (df::global::gps->force_full_display_count < 2) {
            df::global::gps->force_full_display_count = 2;
            g_last_applied = true;
            if (g_policy == FfdPanPolicy::Always)
                g_last_reason = "always";
            return true;
        }
        return false;
    }

    g_last_skipped = true;
    if (g_last_reason == nullptr || strcmp(g_last_reason, "init") == 0)
        g_last_reason = "flat";
    return false;
}

bool ffd_policy_last_pan_ffd_applied() {
    return g_last_applied;
}

bool ffd_policy_last_pan_ffd_skipped() {
    return g_last_skipped;
}

const char* ffd_policy_last_reason() {
    return g_last_reason ? g_last_reason : "unknown";
}

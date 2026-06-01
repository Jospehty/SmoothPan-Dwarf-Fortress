#include "zoom_probe.h"

#include "debug_paths.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "frame_seq.h"
#include "version.h"

#include <cstdio>
#include <cstring>

namespace {

static constexpr int kMaxLevels = 16;
static constexpr int kEventRing = 24;

struct ZoomEvent {
    int frame = 0;
    char tag[32] = {};
    int a = 0;
    int b = 0;
};

static int g_levels[kMaxLevels] = {};
static int g_level_count = 0;
static int g_last_gps_z = 0;
static int g_feed_in = 0;
static int g_feed_out = 0;
static int g_renderer_zoom_calls = 0;
static int g_set_viewport_calls = 0;
static ZoomEvent g_events[kEventRing] = {};
static int g_event_pos = 0;

static void record_level(int z) {
    if (z <= 0) return;
    for (int i = 0; i < g_level_count; ++i) {
        if (g_levels[i] == z) return;
    }
    if (g_level_count < kMaxLevels)
        g_levels[g_level_count++] = z;
}

static void push_event(const char* tag, int a, int b) {
    ZoomEvent& e = g_events[g_event_pos % kEventRing];
    g_event_pos++;
    e.frame = frame_seq_frame_index();
    std::snprintf(e.tag, sizeof(e.tag), "%s", tag ? tag : "?");
    e.a = a;
    e.b = b;
}

static const char* zoom_cmd_name(df::zoom_commands cmd) {
    switch (cmd) {
    case df::zoom_commands::zoom_in: return "zoom_in";
    case df::zoom_commands::zoom_out: return "zoom_out";
    case df::zoom_commands::zoom_reset: return "zoom_reset";
    case df::zoom_commands::zoom_fullscreen: return "zoom_fullscreen";
    case df::zoom_commands::zoom_resetgrid: return "zoom_resetgrid";
    default: return "zoom_?";
    }
}

static void sort_levels() {
    for (int i = 0; i < g_level_count; ++i) {
        for (int j = i + 1; j < g_level_count; ++j) {
            if (g_levels[j] < g_levels[i]) {
                int t = g_levels[i];
                g_levels[i] = g_levels[j];
                g_levels[j] = t;
            }
        }
    }
}

}  // namespace

void zoom_probe_reset() {
    g_level_count = 0;
    g_last_gps_z = 0;
    g_feed_in = 0;
    g_feed_out = 0;
    g_renderer_zoom_calls = 0;
    g_set_viewport_calls = 0;
    g_event_pos = 0;
    std::memset(g_levels, 0, sizeof(g_levels));
    std::memset(g_events, 0, sizeof(g_events));
}

void zoom_probe_note_feed_zoom_in() {
    g_feed_in++;
    push_event("feed_ZOOM_IN", g_last_gps_z, 0);
}

void zoom_probe_note_feed_zoom_out() {
    g_feed_out++;
    push_event("feed_ZOOM_OUT", g_last_gps_z, 0);
}

void zoom_probe_note_renderer_zoom(df::zoom_commands cmd) {
    g_renderer_zoom_calls++;
    push_event(zoom_cmd_name(cmd), g_last_gps_z, static_cast<int>(cmd));
}

void zoom_probe_note_set_viewport_zoom(int nfactor, int prev_z) {
    g_set_viewport_calls++;
    record_level(nfactor);
    push_event("set_viewport_z", prev_z, nfactor);
    g_last_gps_z = nfactor;
}

void zoom_probe_note_gps_z_change(int prev_z, int new_z) {
    record_level(prev_z);
    record_level(new_z);
    push_event("gps_z_change", prev_z, new_z);
    g_last_gps_z = new_z;
}

void zoom_probe_write_f9(FILE* f) {
    if (!f) return;
    sort_levels();
    const int live_gps = df::global::gps ? df::global::gps->viewport_zoom_factor : 0;
    fprintf(f, "  zoom probe: gps_z=%d live_gps=%d levels=%d feed_in=%d feed_out=%d "
               "renderer_zoom=%d set_viewport=%d\n",
            g_last_gps_z, live_gps, g_level_count, g_feed_in, g_feed_out,
            g_renderer_zoom_calls, g_set_viewport_calls);
    if (g_level_count > 0) {
        fprintf(f, "  zoom ladder:");
        for (int i = 0; i < g_level_count; ++i)
            fprintf(f, " %d", g_levels[i]);
        fprintf(f, " (cell= z/4)\n");
    }
    const int n = g_event_pos < kEventRing ? g_event_pos : kEventRing;
    for (int i = 0; i < n; ++i) {
        int idx = (g_event_pos - 1 - i + kEventRing * 2) % kEventRing;
        const ZoomEvent& e = g_events[idx];
        if (e.tag[0] == '\0') continue;
        fprintf(f, "    zoom_evt frame=%d %s a=%d b=%d\n",
                e.frame, e.tag, e.a, e.b);
    }
}

void zoom_probe_flush_discovery_log() {
    std::string path = smoothpan_log_path("smoothpan_zoom_discovery.txt");
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    fprintf(f, "=== SMOOTHPAN_%s zoom_discovery flush frame=%d ===\n",
            SMOOTHPAN_BUILD_VERSION, frame_seq_frame_index());
    zoom_probe_write_f9(f);
    fprintf(f, "\n");
    fclose(f);
}

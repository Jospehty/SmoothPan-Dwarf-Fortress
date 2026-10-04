// Refresh grid of an X window's output via the X Present extension.
// XWayland answers PresentNotifyMSC immediately with the latest (msc, ust) --
// the refresh count and CLOCK_MONOTONIC time of the last refresh it heard about
// from the compositor.  Sampling every few ms reconstructs the refresh grid:
// period = dUST/dMSC, phase = ust modulo period.
//   vblmsc <window-id> <samples> <interval_us>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/present.h>

static long long mono_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: vblmsc <window-id> <samples> <interval_us>\n"); return 2; }
    xcb_window_t win = (xcb_window_t)strtoul(argv[1], NULL, 0);
    int count = atoi(argv[2]);
    useconds_t iv = (useconds_t)atoi(argv[3]);
    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { fprintf(stderr, "connect failed\n"); return 1; }
    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(c, &xcb_present_id);
    if (!ext || !ext->present) { fprintf(stderr, "no Present extension\n"); return 1; }
    xcb_present_event_t eid = xcb_generate_id(c);
    xcb_present_select_input(c, eid, win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    for (int i = 0; i < count; ++i) {
        long long sent = mono_us();
        xcb_present_notify_msc(c, win, (uint32_t)(i + 1), 0, 0, 0);
        xcb_flush(c);
        for (;;) {
            xcb_generic_event_t* ev = xcb_wait_for_event(c);
            if (!ev) { fprintf(stderr, "connection lost\n"); return 1; }
            int done = 0;
            if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
                xcb_present_complete_notify_event_t* ce = (xcb_present_complete_notify_event_t*)ev;
                if (ce->event_type == XCB_PRESENT_EVENT_COMPLETE_NOTIFY &&
                    ce->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC && ce->serial == (uint32_t)(i + 1)) {
                    printf("sent=%lld ust=%llu msc=%llu recv=%lld\n", sent, (unsigned long long)ce->ust,
                           (unsigned long long)ce->msc, mono_us());
                    done = 1;
                }
            }
            free(ev);
            if (done) break;
        }
        usleep(iv);
    }
    xcb_disconnect(c);
    return 0;
}

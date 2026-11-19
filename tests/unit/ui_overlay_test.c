/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../common/aztimestamp.h"
#include "overlay-constants.inc"
struct overlay_ctx {
    void *dpy;
    int dirty, need_raise;
};
static int g_cm_on_ui;
static char calls[256];
static unsigned ncalls;
static int banner, redraw, open_display, loops;
static long clock_ms;
static useconds_t slept;
static jmp_buf finished;
static int same_ms(long actual, long expected) {
    int order = compare_timestamps(actual, expected, 1000, 1000, 1);
    assert(order != AZ_TIME_INVALID);
    return order == AZ_TIME_EQUAL;
}
static void note(char c) {
    assert(ncalls + 1 < sizeof calls);
    calls[ncalls++] = c;
    calls[ncalls] = 0;
}
static void expected(const char *value) {
    assert(!strcmp(calls, value));
    ncalls = 0;
    calls[0] = 0;
}
static long now_ms(void) { return clock_ms; }
static int fixture_usleep(useconds_t duration) {
    slept = duration;
    clock_ms += duration / 1000;
    note('S');
    return 0;
}
static void bus_cfg_publish(void) { note('B'); }
static void vs_save_flush(void) { note('V'); }
static void wz_tick(long now) {
    assert(same_ms(now, clock_ms));
    note('Z');
}
static int ov_reload_banner(struct overlay_ctx *c) {
    assert(c->dpy);
    note('R');
    return banner;
}
static void sdk_status_ui_drain(void) { note('D'); }
static void ov_poll_inputs(struct overlay_ctx *c) {
    assert(c->dpy);
    note('I');
}
static void ov_update_state(struct overlay_ctx *c) {
    assert(c->dpy);
    note('U');
}
static void ov_place_windows(struct overlay_ctx *c) {
    assert(c->dpy);
    note('W');
}
static void ov_present(struct overlay_ctx *c) {
    assert(c->dirty);
    note('P');
}
static int sdk_any_redraw(void) {
    note('A');
    return redraw;
}
static void sdk_overlay_present(struct overlay_ctx *c, int dirty) {
    assert(dirty == c->dirty);
    note('O');
}
static void XFlush(void *display) {
    assert(display == (void *) 1);
    note('F');
}
static void ov_start_mods(void) {
    assert(g_cm_on_ui);
    note('M');
}
static int ov_open_display(struct overlay_ctx *c) {
    note('X');
    memset(c, 0, sizeof *c);
    c->dpy = (void *) 1;
    return open_display;
}
static void sdk_overlay_attach(struct overlay_ctx *c) {
    assert(c->dpy);
    note('a');
}
static void ov_create_windows(struct overlay_ctx *c) {
    assert(c->dpy);
    note('w');
}
static void ov_load_state(void) { note('L'); }
static int sdk_command_ui_drain(unsigned limit) {
    assert(limit == 8);
    note('C');
    return 3;
}
static long ov_tick_us(void) { return 16000; }
static unsigned ov_wait(struct overlay_ctx *c, long interval, long tick_start) {
    assert(c->dpy && interval == 16000 && same_ms(tick_start, clock_ms));
    if (loops++ == 0) {
        expected("MXawLSBVZRIUWAF");
        return OV_WAKE_COMMAND;
    }
    if (loops == 2) {
        expected("C"); /* command-only wakes must not force a UI frame */
        c->dirty = 1;
        c->need_raise = 1;
        clock_ms += 20;
        return OV_WAKE_COMMAND | OV_WAKE_FRAME;
    }
    expected("CBVZRIUWPOF");
    assert(!c->dirty && !c->need_raise);
    longjmp(finished, 1);
}
#define usleep fixture_usleep
#include ROUTING_IMPL_INC
#undef usleep

int main(void) {
    struct overlay_ctx c = {.dpy = (void *) 1, .dirty = 1, .need_raise = 1};
    long start = 100;
    clock_ms = 103;
    ov_frame(&c, &start);
    expected("SBVZRIUWPOF");
    assert(slept == 1000 && same_ms(start, 104) && !c.dirty && !c.need_raise);
    clock_ms = 130;
    ov_frame(&c, &start);
    expected("BVZRIUWAF");
    assert(same_ms(start, 130));
    redraw = 1;
    clock_ms += 20;
    ov_frame(&c, &start);
    expected("BVZRIUWAOF"); /* a mod redraw presents even without host damage */
    redraw = 0;
    banner = 1;
    c.dirty = 1;
    c.need_raise = 1;
    clock_ms += 20;
    ov_frame(&c, &start);
    expected("BVZRD");
    assert(c.dirty && c.need_raise); /* reload owns the display; status drain still runs */
    assert(!overlay_thread(NULL));
    expected("MX");
    open_display = 1;
    banner = 0;
    clock_ms = 100;
    if (!setjmp(finished)) overlay_thread(NULL);
    assert(loops == 3 && g_cm_on_ui);
    puts("PASS overlay: minimum cadence, reload isolation, independent redraw and command wakes");
    return 0;
}

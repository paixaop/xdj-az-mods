/* SPDX-License-Identifier: GPL-3.0-only */
/* The drawing services' logic (guest/azmod/sdk_surface_logic.h), natively: the view mapping,
 * surface descriptors and the per-mod / total limits, placement clamping, press and long-press
 * tracking, premultiplied image blending, the bounded line walk, and the pointer queue under
 * several producer threads. run.sh also builds this file against mutated copies of the header;
 * each must fail. */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "sdk_surface_logic.h"

static void paint(az_draw *d, az_rect a, void *u) { (void) d, (void) a, (void) u; }

static long g_plots;
static int g_minx, g_maxx, g_miny, g_maxy;
static void plot(int x, int y, int size, void *u) {
    (void) u;
    g_plots++;
    if (x < g_minx) g_minx = x;
    if (x + size > g_maxx) g_maxx = x + size;
    if (y < g_miny) g_miny = y;
    if (y + size > g_maxy) g_maxy = y + size;
}

static struct sdk_pq g_q;
static void *producer(void *arg) {
    int base = (int) (long) arg;
    for (int i = 0; i < 1000; i++) {
        struct sdk_pev ev = {base, i, 1, i & 1};
        while (!sdk_pq_post(&g_q, &ev)); /* full: spin until the consumer catches up */
    }
    return NULL;
}

int main(void) {
    ut_suite = "sdk drawing";
    /* 1. views: first match wins, in the documented order */
    CHECK("view: the menu covers the deck view", sdk_view_compute(1, 1, 1, 1, 1) == AZ_VIEW_MENU);
    CHECK("view: deck before search", sdk_view_compute(0, 1, 1, 0, 0) == AZ_VIEW_DECK);
    CHECK("view: search before source", sdk_view_compute(0, 0, 1, 1, 1) == AZ_VIEW_SEARCH);
    CHECK("view: source before browse", sdk_view_compute(0, 0, 0, 1, 1) == AZ_VIEW_SOURCE);
    CHECK("view: browse", sdk_view_compute(0, 0, 0, 0, 1) == AZ_VIEW_BROWSE);
    CHECK("view: nothing known is OTHER", sdk_view_compute(0, 0, 0, 0, 0) == AZ_VIEW_OTHER);
    CHECK("view: two-deck surfaces appear in the two-deck layout",
          sdk_surface_matches_view(AZ_VIEW_TWO_DECKS, AZ_VIEW_DECK, 0));
    CHECK("view: two-deck surfaces stay hidden in the four-deck layout",
          !sdk_surface_matches_view(AZ_VIEW_TWO_DECKS, AZ_VIEW_DECK, 1));
    CHECK("view: two-deck surfaces stay hidden on other screens",
          !sdk_surface_matches_view(AZ_VIEW_TWO_DECKS, AZ_VIEW_BROWSE, 0));
    CHECK("view: general deck surfaces appear in either layout",
          sdk_surface_matches_view(AZ_VIEW_BIT(AZ_VIEW_DECK), AZ_VIEW_DECK, 0) &&
              sdk_surface_matches_view(AZ_VIEW_BIT(AZ_VIEW_DECK), AZ_VIEW_DECK, 1));

    /* 2. descriptors */
    az_surface_v1 ok = {sizeof(az_surface_v1), AZ_LAYER_TOP, AZ_VIEW_BIT(AZ_VIEW_DECK), paint, NULL, NULL};
    CHECK("desc: a complete descriptor is accepted", sdk_surface_desc_ok(&ok));
    az_surface_v1 bad = ok;
    bad.size = 8;
    CHECK("desc: a truncated size is refused", !sdk_surface_desc_ok(&bad));
    bad = ok;
    bad.layer = 7;
    CHECK("desc: an unknown layer is refused", !sdk_surface_desc_ok(&bad));
    bad = ok;
    bad.on_paint = NULL;
    CHECK("desc: no on_paint is refused", !sdk_surface_desc_ok(&bad));
    bad = ok;
    bad.views = 0;
    CHECK("desc: no view is refused", !sdk_surface_desc_ok(&bad));
    CHECK("desc: NULL is refused", !sdk_surface_desc_ok(NULL));
    struct {
        az_surface_v1 s;
        char tail[16];
    } big;
    memset(&big, 0x5a, sizeof big);
    big.s = ok;
    big.s.size = (int) sizeof big;
    az_surface_v1 cp;
    sdk_surface_desc_copy(&cp, &big.s);
    CHECK("desc: a newer, larger descriptor is copied at this core's size",
          cp.size == (int) sizeof cp && cp.on_paint == paint && cp.layer == AZ_LAYER_TOP);

    /* 3. the table's limits */
    int used[SDK_SURF_MAX] = {0}, owner[SDK_SURF_MAX] = {0};
    int got = 0;
    for (int i = 0; i < SDK_SURF_PER_MOD + 2; i++) {
        int k = sdk_surface_slot(used, owner, 3);
        if (k >= 0) {
            used[k] = 1;
            owner[k] = 3;
            got++;
        }
    }
    CHECK("limit: one mod gets exactly SDK_SURF_PER_MOD surfaces", got == SDK_SURF_PER_MOD);
    CHECK("limit: the next one is refused as over the mod's limit", sdk_surface_slot(used, owner, 3) == -1);
    for (int m = 4, n = 0; n < SDK_SURF_MAX; m++)
        for (int j = 0; j < SDK_SURF_PER_MOD; j++) {
            int k = sdk_surface_slot(used, owner, m);
            if (k < 0)
                break;
            used[k] = 1;
            owner[k] = m;
            n = 0;
            for (int i = 0; i < SDK_SURF_MAX; i++)
                n += used[i];
            if (n >= SDK_SURF_MAX)
                break;
        }
    CHECK("limit: a full table refuses another mod", sdk_surface_slot(used, owner, 99) == -2);

    /* 4. placement clamping */
    az_rect r = sdk_rect_clamp((az_rect) {-50, 700, 5000, 900}, 1280, 800);
    CHECK("clamp: inside the screen and at least 1x1",
          r.x == 0 && r.y == 700 && r.w == 1280 && r.h == 100);
    r = sdk_rect_clamp((az_rect) {2000, 2000, 0, -3}, 1280, 800);
    CHECK("clamp: off-screen and empty becomes the last pixel", r.x == 1279 && r.y == 799 && r.w == 1 && r.h == 1);
    int keep;
    r = sdk_xwin_rect((az_rect) {4, 70, 0, 0}, 1280, 800, &keep);
    CHECK("xwin: no size = the window keeps its own, the position is set", keep && r.x == 4 && r.y == 70 && r.w == 0 && r.h == 0);
    r = sdk_xwin_rect((az_rect) {-9, 900, 765, 0}, 1280, 800, &keep);
    CHECK("xwin: no height alone keeps the size too; the position stays on screen", keep && r.x == 0 && r.y == 799);
    r = sdk_xwin_rect((az_rect) {4, 70, 765, 503}, 1280, 800, &keep);
    CHECK("xwin: a size is applied, clamped like a surface's", !keep && r.w == 765 && r.h == 503);

    /* 5. press tracking: one LONG per held press, none after a release */
    struct sdk_press p = {0};
    sdk_press_begin(&p, 10, 20, 1000);
    CHECK("press: no LONG before the threshold", sdk_press_tick(&p, 1000 + SDK_LONG_PRESS_MS - 1) == SDK_PRESS_NONE);
    CHECK("press: LONG at the threshold", sdk_press_tick(&p, 1000 + SDK_LONG_PRESS_MS) == SDK_PRESS_LONG);
    CHECK("press: LONG only once", sdk_press_tick(&p, 5000) == SDK_PRESS_NONE);
    CHECK("press: a release ends it", sdk_press_end(&p) == 1);
    CHECK("press: a second release is not another UP", sdk_press_end(&p) == 0);
    sdk_press_begin(&p, 0, 0, 0);
    sdk_press_end(&p);
    CHECK("press: no LONG after a quick release", sdk_press_tick(&p, 9999) == SDK_PRESS_NONE);
    sdk_press_begin(&p, 100, 100, 20000);
    sdk_press_move(&p, 100 + SDK_PRESS_SLOP, 100 - SDK_PRESS_SLOP);
    CHECK("press: a finger that stays within the slop is still a long press",
          sdk_press_tick(&p, 20000 + SDK_LONG_PRESS_MS) == SDK_PRESS_LONG);
    sdk_press_end(&p);
    sdk_press_begin(&p, 100, 100, 30000);
    sdk_press_move(&p, 100, 100 + SDK_PRESS_SLOP + 1);
    CHECK("press: a drag is no long press", sdk_press_tick(&p, 30000 + SDK_LONG_PRESS_MS) == SDK_PRESS_NONE);
    CHECK("press: a drag still ends with one UP", sdk_press_end(&p) == 1);

    /* 6. blending: premultiplied over 0x00RRGGBB, clipped */
    uint32_t fb[4 * 3];
    for (int i = 0; i < 12; i++)
        fb[i] = 0x00204060u;
    uint8_t img[2 * 2 * 4] = {
        255,
        0,
        0,
        255,   /* opaque red */
        0,
        0,
        0,
        0,       /* transparent */
        64,
        64,
        64,
        128,  /* half gray, premultiplied */
        0,
        128,
        0,
        255,   /* opaque green-ish */
    };
    long n = sdk_blend_image((uint8_t *) fb, 4, 3, 16, 3, 2, img, 2, 2);
    CHECK("blend: clipped to the framebuffer (1 of 4 pixels lands)", n == 1 && fb[2 * 4 + 3] == 0x00ff0000u);
    for (int i = 0; i < 12; i++)
        fb[i] = 0x00204060u;
    n = sdk_blend_image((uint8_t *) fb, 4, 3, 16, 0, 0, img, 2, 2);
    unsigned half = (unsigned) (64 + (0x20 * 127 + 127) / 255);
    CHECK("blend: opaque replaces", fb[0] == 0x00ff0000u);
    CHECK("blend: transparent leaves the pixel", fb[1] == 0x00204060u);
    CHECK("blend: half alpha mixes (red channel)", ((fb[4] >> 16) & 0xff) == half);
    CHECK("blend: 3 pixels written (the transparent one is skipped)", n == 3);
    CHECK("blend: an absurd size is refused", sdk_blend_image((uint8_t *) fb, 4, 3, 16, 0, 0, img, 5000, 1) == 0);
    /* a mod's x or y may be anything: x + w and y + h must not wrap (built with -fwrapv, so a
     * 32-bit clip wraps the same way on every compiler) */
    for (int i = 0; i < 12; i++)
        fb[i] = 0x00204060u;
    long ovf = sdk_blend_image((uint8_t *) fb, 4, 3, 16, 0, INT_MAX - 1, img, 2, 2) +
               sdk_blend_image((uint8_t *) fb, 4, 3, 16, INT_MAX - 1, 0, img, 2, 2) +
               sdk_blend_image((uint8_t *) fb, 4, 3, 16, INT_MIN, INT_MIN, img, 2, 2);
    int untouched = 1;
    for (int i = 0; i < 12; i++)
        untouched &= fb[i] == 0x00204060u;
    CHECK("blend: positions near the int limits draw nothing and write nothing", ovf == 0 && untouched);

    /* xwin_show's admission: only a window of a program the mod launched, one owner each */
    struct sdk_xw_ent xt[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int noop;
    CHECK("xwin admit: a window the mod did not launch is refused",
          sdk_xwin_admit(xt, 3, 1, 0x400001, 1, 0, &noop) == SDK_XW_NOTOWNED && !noop);
    CHECK("xwin admit: its own program's window takes a free slot",
          sdk_xwin_admit(xt, 3, 1, 0x400001, 1, 1, &noop) == 0);
    xt[0] = (struct sdk_xw_ent) {1, 2, 0x500001};
    CHECK("xwin admit: a window another mod shows is refused",
          sdk_xwin_admit(xt, 3, 1, 0x500001, 1, 1, &noop) == SDK_XW_OTHER);
    CHECK("xwin admit: the owner's shown window keeps its slot",
          sdk_xwin_admit(xt, 3, 2, 0x500001, 0, 0, &noop) == 0 && !noop);
    CHECK("xwin admit: hiding a window that is not shown is a no-op",
          sdk_xwin_admit(xt, 3, 1, 0x600001, 0, 0, &noop) == -1 && noop);
    xt[1] = (struct sdk_xw_ent) {1, 2, 0x500002};
    xt[2] = (struct sdk_xw_ent) {1, 2, 0x500003};
    CHECK("xwin admit: a full table refuses", sdk_xwin_admit(xt, 3, 1, 0x400001, 1, 1, &noop) == SDK_XW_FULL);

    /* 7. lines: the stamps cover both endpoints; a hostile line is bounded */
    g_plots = 0;
    g_minx = g_miny = 1 << 30;
    g_maxx = g_maxy = -(1 << 30);
    sdk_line_walk(10, 10, 20, 15, 3, plot, NULL);
    CHECK("line: covers both ends with the brush", g_minx == 9 && g_maxx == 22 && g_miny == 9 && g_maxy == 17);
    CHECK("line: one stamp per step of the longer axis", g_plots == 11);
    g_plots = 0;
    long steps = sdk_line_walk(-2000000000, 0, 2000000000, 0, 1, plot, NULL);
    CHECK("line: a hostile length is bounded", steps <= 8193 && g_plots <= 8193);

    /* 8. the pointer queue: 4 producers, one consumer, nothing lost or duplicated */
    sdk_pq_init(&g_q);
    pthread_t th[4];
    for (long t = 0; t < 4; t++)
        pthread_create(&th[t], NULL, producer, (void *) (t * 10000));
    static int seen[4][1000];
    int total = 0, order_ok = 1, last[4] = {-1, -1, -1, -1};
    while (total < 4000) {
        struct sdk_pev ev;
        if (!sdk_pq_take(&g_q, &ev))
            continue;
        int t = ev.x / 10000;
        if (t < 0 || t > 3 || ev.y < 0 || ev.y >= 1000)
            break;
        seen[t][ev.y]++;
        if (ev.y <= last[t])
            order_ok = 0;
        last[t] = ev.y;
        total++;
    }
    for (int t = 0; t < 4; t++)
        pthread_join(th[t], NULL);
    int exact = 1;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 1000; i++)
            if (seen[t][i] != 1)
                exact = 0;
    CHECK("queue: every event of 4 producers arrives exactly once", total == 4000 && exact);
    CHECK("queue: each producer's events stay in order", order_ok);
    struct sdk_pq q2;
    sdk_pq_init(&q2);
    int posted = 0;
    for (int i = 0; i < SDK_PQ_N + 10; i++) {
        struct sdk_pev ev = {i, 0, 0, 0};
        posted += sdk_pq_post(&q2, &ev);
    }
    CHECK("queue: a full queue drops (never blocks) and counts", posted == SDK_PQ_N && q2.dropped == 10);

    return ut_summary();
}

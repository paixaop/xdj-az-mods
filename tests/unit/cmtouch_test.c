/* SPDX-License-Identifier: GPL-3.0-only */
/* Community Mods home page by touch (screen_community.c, decided by menu_full.c azmod_pane_drag):
 *   - a drag scrolls the page with the finger and never opens or toggles the control it started
 *     on (the deck's stream: press, release, no motion; and the emulator's, with motion);
 *   - a tap opens the tile under it at the scrolled position, switches a tab; a tap beside the
 *     header's title turns nothing on or off; a tap where a control scrolled up under the header
 *     does nothing there, and the header's back square still leaves the screen;
 *   - a slow press on an order button does nothing (a quick tap picks it); a slow press on a
 *     tile still opens it (navigation);
 *   - the knob scrolls the page a row per detent, never past either end, with no knob focus
 *     drawn; its click fires nothing on the home page (Browse's Home view aside: there the knob
 *     moves a focus through the shelves' tiles, cmcat_test.c);
 *   - the offset stays inside the page (a shorter tab brings it back);
 *   - EP147's own scroll bar in the right margin only while the page overflows, its thumb at the
 *     top at offset 0 and at the bottom at the end;
 *   - a full-screen frame does not draw the Mod Menu's left list under it.
 * menu.c is #included (MENU_C) for its statics; CM_CATALOG points at a temp file. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include MENU_C

static int g_sysinfo; /* times the Mod Menu's left list drew its "System Info" row */
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    if (s && !strcmp(s, "System Info")) g_sysinfo++;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold,
                    uint32_t c, int split, uint32_t c2) {
    (void) split;
    (void) c2;
    return azmod_ttf_menu(fb, x, y, w, h, s, px, track, bold, c);
}

#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static azmod_state g_s;
static long g_t = 1000;

static void write_catalog(int nmod) {
    FILE *f = fopen(CM_CATALOG ".new", "w");
    fprintf(f, "v1\nrepo|https://example.com/r0/azmods-repo.json|Repo 0|1|1|\n");
    for (int i = 0; i < nmod; i++)
        fprintf(f, "mod|https://example.com/r0/azmods-repo.json|mod-%03d|Mod %d|1.0.0|someone|Does a thing|tools|0|any|1000|||||0\n", i, i);
    fclose(f);
    rename(CM_CATALOG ".new", CM_CATALOG);
    usleep(300000); /* cm_load re-reads at most 4x a second */
    cm_load();
}
static void draw(void) {
    memset(g_px, 0, sizeof g_px);
    azmod_composite(&g_s, &g_fb);
}
static uint32_t px_at(int x, int y) { return *(uint32_t *) (g_px + (size_t) y * W * 4 + (size_t) x * 4) & 0xffffffu; }
/* the page glides to where a drag or a detent sent it (scroll_glide.h): run its frames until it
 * stands still, as the menu's tick does */
static void settle(void) {
    for (int k = 0; k < 2000 && cm_glide_frame(); k++) usleep(1000);
}
static void knob(int d, bool click) {
    azmod_input(&g_s, &g_fb, 0, 0, false, d, click);
    settle();
}
/* the deck's stream: press, then release, nothing between */
static int touch(int x0, int y0, int x1, int y1, long ms) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(&g_s, &g_fb, x0, y0, true, 0, false);
    azmod_menu_clock(g_t + ms);
    int r = azmod_pane_drag(&g_s, &g_fb, x1, y1, 2);
    settle();
    return r;
}
/* the emulator's: motion on the way, 10 ms apart (on the real clock too: the glide reads it), then
 * a pause before the lift, so the finger has stopped and the lift does not fling */
static int touch_moving(int x0, int y0, int x1, int y1) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(&g_s, &g_fb, x0, y0, true, 0, false);
    for (int k = 1; k <= 4; k++) {
        usleep(10000);
        azmod_menu_clock(g_t + 10 * k);
        azmod_pane_drag(&g_s, &g_fb, x0 + (x1 - x0) * k / 4, y0 + (y1 - y0) * k / 4, 1);
    }
    usleep(200000);
    azmod_menu_clock(g_t + 50);
    int r = azmod_pane_drag(&g_s, &g_fb, x1, y1, 2);
    settle();
    return r;
}
static int g_tab0 = 1;
static void home(int tab) { /* the full screen on a tab, at the top */
    g_s.mode = AZ_COMMUNITY;
    g_cm_full = 1;
    g_cm_dir = 0;
    g_cm_view = CMV_HOME;
    g_cm_tab = tab;
    g_cm_scroll = 0;
    g_cm_knob = 0;
    draw();
}
/* the control of a kind and index in the current layout; one off the screen when there is none
 * (a check on it then fails instead of the suite stopping) */
static cm_ctl g_ctl[CM_MAXCTL], g_none = {-1, -1, -1000, -1000, 1, 1};
static const cm_ctl *ctl_of(int kind, int idx) {
    int n = cm_layout(&g_s, &g_fb, g_ctl, 0);
    for (int i = 0; i < n; i++)
        if (g_ctl[i].kind == kind && g_ctl[i].idx == idx) return &g_ctl[i];
    return &g_none;
}
static int view_top(void) { return dock_y(&g_fb) + menu_hdrh(&g_fb); }
static int view_bot(void) { return dock_y(&g_fb) + dock_h(&g_fb); }
/* the scroll bar's column: anything drawn there (the page's background is the scrim) */
static int bar_x(void) {
    int bx, by, bw, bh;
    cm_box(&g_fb, &bx, &by, &bw, &bh);
    return bx + bw + W / 80 - W * 2 / 1280 - 3; /* the middle of draw_vsb's 5 px thumb */
}
static int bar_rows(int *first, int *last) {   /* rows where the bar's column differs from the margin beside it */
    int n = 0, xb = bar_x();
    *first = *last = -1;
    for (int y = view_top(); y < view_bot(); y++)
        if (px_at(xb, y) != px_at(xb - 8, y)) {
            if (*first < 0) *first = y;
            *last = y;
            n++;
        }
    return n;
}

int main(void) {
    ut_suite = "community touch";
    azmod_state_init(&g_s);
    azmod_set_docked(true);
    g_s.menu_focused = 1;
    write_catalog(30);
    CHECK("30 mods in the catalog", g_ncc == 30);
    g_cm_fcat = CMB_ALL;   /* Browse's grid (All mods); Home's knob is checked in cmcat_test.c */
    home(g_tab0);
    CHECK("Browse with 30 tiles overflows the page", g_cm_smax > 0);
    int smax = g_cm_smax;

    /* scroll bar: shown, thumb at the top */
    int f0, l0, f1, l1;
    int nb = bar_rows(&f0, &l0);
    CHECK("the scroll bar is drawn while the page overflows", nb > 8);
    CHECK("at offset 0 its thumb starts at the top", nb > 0 && f0 < view_top() + 40);

    /* a drag on a tile, the deck's stream: scrolls by the finger, opens nothing */
    const cm_ctl *t = ctl_of(CMC_TILE, 7);
    CHECK("tile 7 is on screen", t->kind == CMC_TILE && t->y + t->h / 2 < view_bot());
    int x = t->x + t->w / 2, y = t->y + t->h / 2;
    int r = touch(x, y, x, y - 200, 60);
    CHECK("drag (press, release): the page follows the finger", g_cm_scroll == 200 && r == 1);
    CHECK("drag (press, release): no page opened", g_cm_view == CMV_HOME);
    /* each step of a moving finger is shown at once: no frames between it and the page */
    {
        int s0 = g_cm_scroll, at = 1;
        g_t += 1000;
        azmod_menu_clock(g_t);
        azmod_input(&g_s, &g_fb, x, y, true, 0, false);
        for (int k = 1; k <= 4; k++) {
            usleep(10000); /* the glide times steps on the real clock (cm_ms) */
            azmod_menu_clock(g_t + 10 * k);
            azmod_pane_drag(&g_s, &g_fb, x, y - 25 * k, 1);
            at &= g_cm_scroll == s0 + 25 * k;
        }
        usleep(200000); /* the finger stops before it lifts: no fling */
        azmod_menu_clock(g_t + 300);
        azmod_pane_drag(&g_s, &g_fb, x, y - 100, 2);
        settle();
        CHECK("drag: every step of the finger is the page's offset at once (no lag)", at && g_cm_scroll == s0 + 100);
        g_cm_scroll = s0;
        draw();
    }
    /* with motion */
    touch_moving(x, y, x, y - 100);
    CHECK("drag (with motion): the page follows the finger", g_cm_scroll == 300);
    CHECK("drag (with motion): no page opened", g_cm_view == CMV_HOME);
    /* never past either end */
    touch(x, view_top() + 20, x, view_bot() - 5, 60);
    touch(x, view_top() + 20, x, view_bot() - 5, 60);
    CHECK("dragging down stops at the top", g_cm_scroll == 0 && g_cm_view == CMV_HOME);
    for (int k = 0; k < 10; k++) touch(x, view_bot() - 5, x, view_top() + 20, 60);
    CHECK("dragging up stops at the end", g_cm_scroll == smax && g_cm_view == CMV_HOME);
    draw();
    nb = bar_rows(&f1, &l1);
    CHECK("at the end the thumb ends at the bottom", nb > 0 && l1 > view_bot() - 40 && f1 > f0);

    /* a tap opens the tile under it at the scrolled position */
    g_cm_scroll = 200;
    draw();
    t = ctl_of(CMC_TILE, 12);
    CHECK("tile 12 is on screen after the scroll", t->kind == CMC_TILE && t->y > view_top() && t->y + t->h / 2 < view_bot());
    r = touch(t->x + t->w / 2, t->y + t->h / 2, t->x + t->w / 2 + 3, t->y + t->h / 2 + 3, 80);
    CHECK("tap: the tile under the finger opens", r == 2 && g_cm_view == CMV_CAT && g_cm_cat == 12);
    azmod_back(&g_s);
    CHECK("BACK: home again, at the same offset", g_cm_view == CMV_HOME && g_cm_scroll == 200);
    r = touch(x, y, x, y - 100, 60);
    CHECK("after the offset was set from outside, a drag still scrolls", g_cm_scroll == 300 && r == 1);
    g_cm_scroll = 200;

    /* a tile scrolled up under the header: a tap there is the header's, never the tile's */
    home(g_tab0);
    g_cm_scroll = ctl_of(CMC_TILE, 0)->y - view_top() + 40;   /* 40 px of tile 0 under the header */
    draw();
    t = ctl_of(CMC_TILE, 0);
    CHECK("tile 0 sits partly under the header", t->kind == CMC_TILE && t->y < view_top() && t->y + t->h > view_top());
    int hx = t->x + t->w / 2;
    r = touch(hx, view_top() - 10, hx, view_top() - 10, 80);
    CHECK("tap on the header over a tile: nothing opens", g_cm_view == CMV_HOME && r == 0);
    const cm_ctl *bk = ctl_of(CMC_BACK, 0);
    r = touch(bk->x + bk->w / 2, bk->y + bk->h / 2, bk->x + bk->w / 2, bk->y + bk->h / 2, 80);
    CHECK("tap on the back square: the screen closes", r == 2 && cm_animating());
    g_cm_dir = 0;

    /* the header has no switch (the Mod Menu row turns community mods on and off): a tap
     * beside the title changes nothing */
    home(g_tab0);
    int off = cm_off_now(), sx = g_fb.w * 3 / 10, sy = dock_y(&g_fb) + menu_hdrh(&g_fb) / 2;
    touch(sx, sy, sx, sy, 80);
    CHECK("the header: a tap beside the title turns nothing on or off", cm_off_now() == off && g_cm_view == CMV_HOME);
    /* a slow press on a control that is not navigation (a view button) does nothing; a quick
     * tap on it switches the view */
    home(g_tab0);
    const cm_ctl *vb = ctl_of(CMC_FCAT, CMB_HOME);
    int vbx = vb->x + vb->w / 2, vby = vb->y + vb->h / 2;
    touch(vbx, vby, vbx, vby, 900);
    CHECK("slow press on a view button: the view stays", vb->kind == CMC_FCAT && g_cm_fcat == CMB_ALL && g_cm_view == CMV_HOME);
    home(g_tab0);
    touch(vbx, vby, vbx, vby, 80);
    CHECK("CONTROL: a quick tap on it switches the view", g_cm_fcat == CMB_HOME);
    g_cm_fcat = CMB_ALL;
    /* the orders: the Sort button opens them, a tap on one picks it and closes them; a tap
     * anywhere else, a knob turn or BACK only closes them */
    home(g_tab0);
    g_cm_sort = CMB_SORT_NAME;
    CHECK("orders closed: no order buttons", ctl_of(CMC_SORT, 0)->kind < 0 && ctl_of(CMC_SORTBTN, 0)->kind == CMC_SORTBTN);
    const cm_ctl *sb = ctl_of(CMC_SORTBTN, 0);
    int sbx = sb->x + sb->w / 2, sby = sb->y + sb->h / 2;
    touch(sbx, sby, sbx, sby, 80);
    CHECK("tap on Sort: the four orders open", g_cm_sortpop && ctl_of(CMC_SORT, CMB_NSORT - 1)->kind == CMC_SORT);
    const cm_ctl *so = ctl_of(CMC_SORT, CMB_SORT_UPDATED);
    int sox = so->x + so->w / 2, soy = so->y + so->h / 2;
    CHECK("the orders open under the button", so->y > sb->y && so->x == sb->x);
    touch(sox, soy, sox, soy, 900);
    CHECK("slow press on an order: it is picked and the orders close", g_cm_sort == CMB_SORT_UPDATED && !g_cm_sortpop && g_cm_view == CMV_HOME);
    g_cm_sort = CMB_SORT_NAME;
    home(g_tab0);
    touch(sbx, sby, sbx, sby, 80);
    t = ctl_of(CMC_TILE, 4);
    touch(t->x + 10, t->y + t->h - 10, t->x + 10, t->y + t->h - 10, 80);
    CHECK("orders open, a tap on a tile: they close, the tile does not open", !g_cm_sortpop && g_cm_view == CMV_HOME && g_cm_sort == CMB_SORT_NAME);
    touch(sbx, sby, sbx, sby, 80);
    knob(1, false);
    CHECK("orders open, a knob turn: they close, the page stays", !g_cm_sortpop && g_cm_scroll == 0);
    touch(sbx, sby, sbx, sby, 80);
    CHECK("BACK with the orders open: they close, Community stays open", cm_back() && !g_cm_sortpop && g_cm_view == CMV_HOME && g_cm_full);
    touch(sbx, sby, sbx, sby, 80);
    touch(sbx, sby, sbx, sby, 80);
    CHECK("a second tap on Sort closes them", !g_cm_sortpop);
    g_cm_fcat = CMB_HOME;
    home(g_tab0);
    CHECK("Home: no Sort button (its shelves have their own orders)", ctl_of(CMC_SORTBTN, 0)->kind < 0);
    g_cm_fcat = CMB_ALL;
    home(g_tab0);
    /* a slow press on a tile is navigation: it still opens */
    t = ctl_of(CMC_TILE, 2);
    touch(t->x + 10, t->y + 10, t->x + 10, t->y + 10, 900);
    CHECK("slow press on a tile: it opens", g_cm_view == CMV_CAT && g_cm_cat == 2);
    /* a tab */
    home(g_tab0);
    const cm_ctl *tb = ctl_of(CMC_TAB, 2);
    touch(tb->x + 10, tb->y + 5, tb->x + 10, tb->y + 5, 80);
    CHECK("tap on a tab: it switches", g_cm_tab == 2 && g_cm_view == CMV_HOME);
    draw();
    CHECK("a page that fits does not scroll", g_cm_smax == 0);
    nb = bar_rows(&f1, &l1);
    CHECK("a page that fits has no scroll bar", nb == 0);

    /* a press whose page goes away before the release fires nothing */
    home(g_tab0);
    t = ctl_of(CMC_TILE, 3);
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(&g_s, &g_fb, t->x + 10, t->y + 10, true, 0, false);
    g_cm_view = CMV_RELOAD;
    azmod_menu_clock(g_t + 50);
    r = azmod_pane_drag(&g_s, &g_fb, t->x + 10, t->y + 10, 2);
    CHECK("the page changed under the finger: the release fires nothing", r == 0 && g_cm_view == CMV_RELOAD && g_cm_cat != 3);

    /* the knob: scrolls a row per detent, no focus, click fires nothing */
    home(g_tab0);
    int rh = c_rrowh(&g_fb);
    knob(1, false);
    CHECK("one detent scrolls one row", g_cm_scroll == rh);
    knob(3, false);
    CHECK("three detents scroll three rows more", g_cm_scroll == 4 * rh);
    knob(-1, false);
    CHECK("back one detent", g_cm_scroll == 3 * rh);
    CHECK("the knob focuses nothing", !g_cm_knob);
    draw();
    int ring = 0;
    for (int yy = view_top(); yy < view_bot() && !ring; yy++)
        for (int xx = 0; xx < W && !ring; xx += 2)
            ring = px_at(xx, yy) == (C_HI & 0xffffff) && px_at(xx, yy + 1) == (C_HI & 0xffffff) && xx > cm_rail_w(&g_fb) + 40;   /* the rail lights its tab */
    CHECK("no knob focus ring is drawn on the page", !ring);
    int v = g_cm_view, tab = g_cm_tab, o = cm_off_now();
    knob(0, true);
    CHECK("the click fires nothing on the home page", g_cm_view == v && g_cm_tab == tab && cm_off_now() == o && !cm_animating());
    knob(200, false);
    CHECK("the knob stops at the end", g_cm_scroll == g_cm_smax);
    knob(-300, false);
    CHECK("and at the top", g_cm_scroll == 0);

    /* a shorter page brings the offset back inside it */
    g_cm_scroll = smax;
    g_cm_tab = 2;
    draw();
    CHECK("a shorter tab: the offset is clamped at the next draw", g_cm_scroll == 0);

    /* the full screen paints the whole menu area: the Mod Menu's left list under it is not drawn
     * (each scroll frame would pay for it unseen); the summary card beside the list still has it */
    g_sysinfo = 0;
    draw();
    CHECK("full screen: the left list under it is not drawn", g_sysinfo == 0);
    g_cm_full = 0;
    g_sysinfo = 0;
    draw();
    CHECK("CONTROL: the summary card beside the left list draws the list", g_sysinfo > 0);
    g_cm_full = 1;

    /* pure geometry */
    CHECK("scroll max: content past the view", cm_scroll_max(1000, 400) == 600);
    CHECK("scroll max: content that fits", cm_scroll_max(300, 400) == 0 && cm_scroll_max(10, 0) == 0);
    CHECK("clamp", cm_scroll_clamp(-5, 10) == 0 && cm_scroll_clamp(11, 10) == 10 && cm_scroll_clamp(7, 10) == 7);
    return ut_summary();
}

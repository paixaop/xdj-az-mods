/* SPDX-License-Identifier: GPL-3.0-only */
/* A community mod's SCREEN row in the Mod Menu's main list (register_row, AZ_ROW_SCREEN) opens
 * that mod's page by the path Community Mods > Installed > <mod> already takes (screen_community.c
 * cm_open_row -> cm_full_open + cm_fire CMC_MOD), and BACK returns to the row:
 *   - a knob press on the row's left entry, a tap on it, and a knob press on a SCREEN row in a
 *     right pane each open the full screen on the mod's page; the mod's on_enter runs once and its
 *     on_draw paints;
 *   - BACK (the button) or the page's back square: the mod hears AZ_EV_BACK first, then the menu
 *     is back on the same left entry, pane and row, with the mod's page gone;
 *   - a SCREEN row whose mod is not installed does nothing (no page opens);
 *   - a row with its own screen (az_row_v1.screen) opens that screen; with none it opens the mod's
 *     register_screen page; with neither it is grayed and does nothing; opening the mod from the
 *     Mod Index afterwards shows the mod's page again (the row's screen lasts one page);
 *   - the right pane of a SCREEN row shows the row's label in its band, not an empty bar.
 * menu.c is #included (MENU_C) for its statics; the community core is this file's stand-in
 * (azcm_count / azcm_info / azcm_screen_*: lanetest.c's stubs are renamed away by run.sh). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "check.h"
#include MENU_C

static char g_seen[1 << 16];
static size_t g_seen_n;
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    size_t L = strlen(s);
    if (g_seen_n + L + 2 < sizeof g_seen) {
        memcpy(g_seen + g_seen_n, s, L);
        g_seen_n += L;
        g_seen[g_seen_n++] = '\n';
        g_seen[g_seen_n] = 0;
    }
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold,
                    uint32_t c, int split, uint32_t c2) {
    (void) split;
    (void) c2;
    return azmod_ttf_menu(fb, x, y, w, h, s, px, track, bold, c);
}
static int seen(const char *s) {
    char k[96];
    snprintf(k, sizeof k, "%s\n", s);
    return strstr(g_seen, k) != NULL;
}

/* ---- the community core's stand-in: "testmod" (a screen of its own) and "nopagemod" (none),
 * both loaded; a page shows a SCREEN row's own screen when the row named one (community.c cm_scr) */
static int g_enter, g_back_heard, g_drawn, g_row_enter, g_consume_back, g_input;
static unsigned g_screen_flags;
static az_rect g_draw_area;
static const az_screen_v1 *g_page_scr;
static int g_page_mod = -1;
int azcm_count(void) { return 2; }
int azcm_info(int i, azcm_info_t *out) {
    if (i != 0 && i != 1) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, i ? "nopagemod" : "testmod");
    snprintf(out->name, sizeof out->name, i ? "No Page Mod" : "Test Mod");
    out->state = AZCM_LOADED;
    out->has_screen = i == 0 || (g_page_mod == i && g_page_scr);
    out->screen_flags = g_page_mod == i && g_page_scr ? g_page_scr->flags : g_screen_flags;
    return 0;
}
void azcm_page_screen(int i, const az_screen_v1 *scr) {
    g_page_scr = scr;
    g_page_mod = scr ? i : -1;
}
int azcm_row_target(const char *owner, const az_screen_v1 *row_scr) {
    if (!strcmp(owner, "testmod")) return 1;
    if (!strcmp(owner, "nopagemod")) return row_scr != NULL;
    return 0;
}
void azcm_screen_draw(int i, az_draw *d, az_rect area) {
    g_draw_area = area;
    if (i == g_page_mod && g_page_scr) {
        g_page_scr->on_draw(d, area, g_page_scr->user);
        return;
    }
    if (i != 0) return;
    g_drawn++;
    azcm_draw_text(d, area.x + 10, area.y + 10, "TESTMOD SCREEN", 20, 0x00FFFFFFu);
}
int azcm_screen_input(int i, const az_event *ev) {
    if ((i == 0 || i == g_page_mod) && ev->kind == AZ_EV_BACK) g_back_heard++;
    g_input++;
    return ev->kind == AZ_EV_BACK && g_consume_back;
}
void azcm_screen_enter(int i) {
    if (i == g_page_mod && g_page_scr) {
        if (g_page_scr->on_enter) g_page_scr->on_enter(g_page_scr->user);
        return;
    }
    if (i == 0) g_enter++;
}
static int row_target(int item) {
    const struct az_row *r = az_row(item);
    return azcm_row_target(azf_owner(item), r ? r->d.screen : NULL);
}

#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static void draw(azmod_state *s) {
    memset(g_px, 0, sizeof g_px);
    g_seen_n = 0;
    g_seen[0] = 0;
    azmod_composite(s, &g_fb);
}
static void knob(azmod_state *s, int d, bool click) { azmod_input(s, &g_fb, 0, 0, false, d, click); }
static void tap(azmod_state *s, int x, int y) {
    azmod_input(s, &g_fb, x, y, true, 0, false);
    azmod_pane_drag(s, &g_fb, x, y, 2);
}
/* run the slide to its end, as the overlay does: a tick and a frame until it stops */
static void settle(azmod_state *s) {
    for (int k = 0; k < 400 && (azmod_tick(s), cm_animating()); k++) {
        draw(s);
        struct timespec t = {0, 5 * 1000000L};
        nanosleep(&t, NULL);
    }
    draw(s);
}
static int select_left(azmod_state *s, const char *label) {
    g_last_left = -1;
    azmod_menu_reset(s);
    int first = s->left_sel;
    for (int k = 0; k < 64; k++) {
        if (az_label(s->left_sel) && !strcmp(az_label(s->left_sel), label)) return 1;
        knob(s, 1, false);
        if (s->left_sel == first) break;
    }
    return 0;
}
static int select_row(azmod_state *s, int item) {
    knob(s, 0, true);
    int r0 = s->menu_sel;
    for (int k = 0; k < 32; k++) {
        if (s->menu_sel == item) return 1;
        knob(s, 1, false);
        if (s->menu_sel == r0) break;
    }
    return 0;
}
static int on_page(azmod_state *s) { return s->mode == AZ_COMMUNITY && g_cm_full && g_cm_view == CMV_MOD && seen("TESTMOD SCREEN"); }
/* The Installed preview shows the Settings button, without drawing either screen. */
static int on_split_page(azmod_state *s) {
    return s->mode == AZ_COMMUNITY && g_cm_full && g_cm_view == CMV_HOME && g_cm_isel == (CMC_MOD << 16 | 0) && seen("Settings") && !seen("TESTMOD SCREEN") && !seen("ROW SCREEN");
}
static int on_row_page(azmod_state *s) { return s->mode == AZ_COMMUNITY && g_cm_full && g_cm_view == CMV_MOD && seen("ROW SCREEN") && !seen("TESTMOD SCREEN"); }
static int count_seen(const char *t) {
    char k[96];
    int n = 0;
    snprintf(k, sizeof k, "%s\n", t);
    for (const char *p = g_seen; (p = strstr(p, k)); p += strlen(k))
        if (p == g_seen || p[-1] == '\n') n++;
    return n;
}

/* a row's own screen (az_row_v1.screen) */
static void row_draw(az_draw *d, az_rect a, void *u) {
    (void) u;
    azcm_draw_text(d, a.x + 10, a.y + 10, "ROW SCREEN", 20, 0x00FFFFFFu);
}
static int row_input(const az_event *e, void *u) {
    (void) u;
    if (e->kind == AZ_EV_BACK) g_back_heard++;
    return 0;
}
static void row_enter(void *u) {
    (void) u;
    g_row_enter++;
}
static const az_screen_v1 ROWSCR = {
    .size = sizeof ROWSCR,
    .on_draw = row_draw,
    .on_input = row_input,
    .on_enter = row_enter,
};

int main(void) {
    ut_suite = "screen row";
    az_defaults();
    az_feature_v2 f;
#define ROW(MOD, ...) (f = (az_feature_v2) {.size = sizeof f, __VA_ARGS__}, azf_register(MOD, &f))
    int fx = ROW("testmod", .key = "fx", .label = "FX Bus", .kind = AZ_ROW_TOGGLE, .help = "A test switch.");
    int sub = ROW("testmod", .key = "fx-page", .label = "FX Page", .kind = AZ_ROW_SCREEN, .parent = "fx");
    int scr = ROW("testmod", .key = "page", .label = "Page", .kind = AZ_ROW_SCREEN);
    int orphan = ROW("gonemod", .key = "page", .label = "Gone Page", .kind = AZ_ROW_SCREEN);
    int own = ROW("testmod", .key = "own", .label = "Own Screen", .kind = AZ_ROW_SCREEN, .screen = &ROWSCR);
    int none = ROW("nopagemod", .key = "none", .label = "Nothing Here", .kind = AZ_ROW_SCREEN);
    int nown = ROW("nopagemod", .key = "nown", .label = "Its Own", .kind = AZ_ROW_SCREEN, .screen = &ROWSCR);
    CHECK("rows registered", fx >= AZI_REG0 && sub > fx && scr > sub && orphan > scr && own > orphan &&
                                 none > own && nown > none);
    azf_screen_target = row_target;
    az_set(fx, 1);

    azmod_state s;
    azmod_state_init(&s);
    azmod_set_docked(true);
    s.menu_focused = 1;
    s.mode = AZ_OPEN;

    /* 1. knob press on the row's left entry */
    CHECK("knob reaches Page", select_left(&s, "Page") && s.mode == AZ_OPEN);
    draw(&s);
    CHECK("the list shows Page, no mod page yet", seen("Page") && !seen("TESTMOD SCREEN"));
    knob(&s, 0, true);
    settle(&s);
    CHECK("knob press: the mod's page opens, full screen", on_page(&s));
    CHECK("knob press: the mod's on_enter ran once", g_enter == 1);
    CHECK("knob press: the left list keeps the row", s.left_sel == scr);
    CHECK("BACK is taken", azmod_back(&s));
    settle(&s);
    CHECK("BACK: the mod heard AZ_EV_BACK first", g_back_heard == 1);
    CHECK("BACK: on the Mod Menu again, on the row (left list)",
          s.mode == AZ_OPEN && !g_cm_full && s.left_sel == scr && s.pane == 0 && !seen("TESTMOD SCREEN") && seen("Page"));

    /* 2. a tap on the row's left entry */
    int tops[AZI_COUNT], nt = c_tops(tops), k = 0;
    while (k < nt && tops[k] != scr) k++;
    CHECK("Page is a left entry", k < nt);
    int x, y, w, h;
    c_left_rect(&g_fb, k, &x, &y, &w, &h);
    tap(&s, x + w / 3, y + h / 2);
    settle(&s);
    CHECK("tap: the mod's page opens", on_page(&s) && g_enter == 2);
    /* BACK through the page's own back square (the last control) */
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(&s, &g_fb, ctl, 0);
    CHECK("the page has its back square", n > 0 && ctl[n - 1].kind == CMC_BACK);
    tap(&s, ctl[n - 1].x + ctl[n - 1].w / 2, ctl[n - 1].y + ctl[n - 1].h / 2);
    settle(&s);
    CHECK("back square: on the Mod Menu again, on the row", s.mode == AZ_OPEN && !g_cm_full && s.left_sel == scr && g_back_heard == 2);

    /* 3. a SCREEN row in a right pane */
    CHECK("knob onto FX Page in FX Bus's pane", select_left(&s, "FX Bus") && select_row(&s, sub));
    knob(&s, 0, true);
    settle(&s);
    CHECK("right-pane press: the mod's page opens", on_page(&s) && g_enter == 3 && s.left_sel == fx);
    azmod_back(&s);
    settle(&s);
    CHECK("BACK: on FX Bus's pane again, on the same row",
          s.mode == AZ_OPEN && !g_cm_full && s.left_sel == fx && s.pane == 1 && s.menu_sel == sub && seen("FX Page"));

    /* 4. a mod that is not installed: no page */
    CHECK("knob reaches Gone Page", select_left(&s, "Gone Page"));
    int e = g_enter;
    knob(&s, 0, true);
    settle(&s);
    CHECK("not installed: no page opens", s.mode == AZ_OPEN && !g_cm_full && g_enter == e);

    /* 5. a row with its own screen opens that screen; BACK returns to the row */
    CHECK("knob reaches Own Screen", select_left(&s, "Own Screen"));
    draw(&s);
    CHECK("preview: the band carries the row's label (list, header, band)", count_seen("Own Screen") >= 3);
    knob(&s, 0, true);
    settle(&s);
    CHECK("own screen: it opens, not the mod's page", on_row_page(&s) && g_row_enter == 1 && g_enter == e);
    azmod_back(&s);
    settle(&s);
    CHECK("own screen: BACK is heard and returns to the row", s.mode == AZ_OPEN && !g_cm_full && s.left_sel == own);
    /* the mod's page from the Mod Index afterwards: its own page again */
    cm_full_open(&s);
    settle(&s);
    cm_ctl c = {CMC_MOD, 0, 0, 0, 0, 0};
    cm_fire(&c);
    settle(&s);
    CHECK("from the Mod Index afterwards: its Settings preview, without entering either screen", on_split_page(&s) && g_enter == e);
    azmod_back(&s);
    settle(&s);

    /* 6. a mod with no page: its row with a screen opens it; its row without one is grayed and inert */
    CHECK("a row of its own on a mod with no page: active", az_active(nown));
    CHECK("a row with nothing to open: grayed", !az_active(none));
    CHECK("knob reaches Nothing Here", select_left(&s, "Nothing Here"));
    int re = g_row_enter, pe = g_enter;
    knob(&s, 0, true);
    settle(&s);
    CHECK("nothing to open: a press does nothing", s.mode == AZ_OPEN && !g_cm_full && g_row_enter == re && g_enter == pe);
    CHECK("knob reaches Its Own", select_left(&s, "Its Own"));
    knob(&s, 0, true);
    settle(&s);
    CHECK("a mod with no page: the row's own screen opens", on_row_page(&s) && g_row_enter == re + 1);
    azmod_back(&s);
    settle(&s);

    /* Installed Settings: simple pages expand, dedicated editors use all the content height. */
    cm_full_open(&s);
    settle(&s);
    cm_split_select(CMC_MOD << 16);
    draw(&s);
    int before = g_enter;
    n = cm_layout(&s, &g_fb, ctl, 0);
    k = 0;
    while (k < n && ctl[k].kind != CMC_SETTINGS) k++;
    CHECK("Installed offers a Settings button without entering the screen", k < n && !g_cm_area.w && g_enter == before);
    CHECK("Settings has a full row touch target", k < n && ctl[k].h >= 44);
    if (k < n) tap(&s, ctl[k].x + ctl[k].w / 2, ctl[k].y + ctl[k].h / 2);
    CHECK("Settings starts the slide and enters once", cm_animating() && g_cm_view == CMV_MOD && g_enter == before + 1);
    int heard = g_input;
    knob(&s, 1, false);
    CHECK("input is held during the slide", g_input == heard);
    settle(&s);
    CHECK("simple settings use the standard full-width page", on_page(&s) && g_draw_area.x < W / 10 && g_draw_area.w > W * 9 / 10);
    knob(&s, 1, false);
    CHECK("the open page gets knob input", g_input == heard + 1);
    azmod_back(&s);
    settle(&s);
    CHECK("BACK restores the same Installed selection", on_split_page(&s) && g_enter == before + 1);

    g_screen_flags = AZ_SCREEN_DEDICATED;
    g_cm_knob = 1;
    g_cm_hfoc = CMC_MOD << 16;
    knob(&s, 0, true);
    CHECK("knob opens the dedicated editor with a slide", cm_animating() && g_cm_view == CMV_SETTINGS);
    settle(&s);
    int bx, by, bw, bh;
    cm_box(&g_fb, &bx, &by, &bw, &bh);
    CHECK("dedicated editor gets the whole content rectangle", g_draw_area.x == bx && g_draw_area.y == by && g_draw_area.w == bw && g_draw_area.h == bh);
    CHECK("dedicated editor has no management buttons", !seen("Disable") && !seen("Remove from USB") && seen("TESTMOD SCREEN"));
    heard = g_input;
    tap(&s, bx + bw / 2, by + bh / 2);
    CHECK("content taps reach the editor", g_input == heard + 1);
    heard = g_input;
    tap(&s, W - 5, dock_y(&g_fb) + 3);
    CHECK("header gaps never reach the editor", g_input == heard);
    g_consume_back = 1;
    azmod_back(&s);
    CHECK("the editor can consume BACK for its own subpage", !cm_animating() && g_cm_view == CMV_SETTINGS);
    g_consume_back = 0;
    n = cm_layout(&s, &g_fb, ctl, 0);
    tap(&s, ctl[n - 1].x + ctl[n - 1].w / 2, ctl[n - 1].y + ctl[n - 1].h / 2);
    settle(&s);
    CHECK("header BACK slides to the Installed preview", on_split_page(&s));

    /* A complex screen can also be opened from its full standard details page. */
    cm_open(CMV_MOD);
    draw(&s);
    CHECK("complex standard page offers Settings instead of embedding the editor", seen("Settings") && !seen("TESTMOD SCREEN"));
    cm_settings_open(&s, 0);
    settle(&s);
    azmod_back(&s);
    settle(&s);
    CHECK("nested editor BACK restores its standard details page", g_cm_view == CMV_MOD && seen("Settings") && !seen("TESTMOD SCREEN"));
    azmod_back(&s);
    settle(&s);
    azmod_back(&s);
    settle(&s);
    CHECK("dedicated mod menu shortcut is reachable", select_left(&s, "Page"));
    knob(&s, 0, true);
    settle(&s);
    CHECK("dedicated menu shortcut opens the editor directly", g_cm_view == CMV_SETTINGS && seen("TESTMOD SCREEN") && !seen("Disable"));
    azmod_back(&s);
    settle(&s);
    CHECK("dedicated shortcut BACK returns to the same menu row", !g_cm_full && s.left_sel == scr);
    return ut_summary();
}

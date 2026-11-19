/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual community management actions, confirmation ownership and navigation. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include MENU_C
static azcm_info_t installed;
static int allow_result, allow_calls, unsuspend_calls;
static void (*confirmed)(int, void *);
static void *confirm_user;
static char confirm_title[80], confirm_body[240], drawn[8192];
int azcm_count(void) { return 1; }
int azcm_ver_newer(const char *a, const char *b) { return strcmp(a, b) > 0; }
int azcm_info(int index, azcm_info_t *out) {
    if (index != 0) return -1;
    *out = installed;
    return 0;
}
int azcm_allow(int index) {
    assert(index == 0);
    ++allow_calls;
    if (!allow_result) installed.allowed = 1;
    return allow_result;
}
void azcm_unsuspend(const char *id) {
    assert(!strcmp(id, installed.id));
    ++unsuspend_calls;
}
int azcm_confirm(const char *title, const char *text, const char *yes, void (*done)(int, void *), void *user) {
    assert(!strcmp(yes, "Remove") && !confirmed);
    snprintf(confirm_title, sizeof confirm_title, "%s", title);
    snprintf(confirm_body, sizeof confirm_body, "%s", text);
    confirmed = done;
    confirm_user = user;
    return 0;
}
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *text, int px, int track, int bold, uint32_t color) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) px;
    (void) track;
    (void) bold;
    (void) color;
    size_t used = strlen(drawn);
    if (used < sizeof drawn - 2) snprintf(drawn + used, sizeof drawn - used, "%s\n", text);
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *text, int px, int track, int bold, uint32_t color, int split, uint32_t color2) {
    (void) split;
    (void) color2;
    return azmod_ttf_menu(fb, x, y, w, h, text, px, track, bold, color);
}
static unsigned char pixels[1280 * 800 * 4];
static azmod_fb fb = {pixels, 1280, 800, 1280 * 4};
static azmod_state menu;
static cm_ctl controls[CM_MAXCTL];
static void fire(int kind, int index) {
    cm_ctl control = {kind, index, 0, 0, 0, 0};
    cm_fire(&control);
}
static int render(int view) {
    g_cm_view = view;
    drawn[0] = 0;
    return cm_layout(&menu, &fb, controls, 1);
}
static bool has_control(int n, int kind) {
    for (int i = 0; i < n; ++i)
        if (controls[i].kind == kind) return true;
    return false;
}
static void request_is(const char *expected) {
    char line[512] = {0};
    FILE *f = fopen(CM_REQ, "r");
    if (f) {
        assert(fgets(line, sizeof line, f));
        fclose(f);
        unlink(CM_REQ);
    }
    line[strcspn(line, "\n")] = 0;
    if (strcmp(line, expected)) fprintf(stderr, "request expected [%s], got [%s]\n", expected, line);
    assert(!strcmp(line, expected));
}
static void answer(int yes) {
    assert(confirmed);
    void (*done)(int, void *) = confirmed;
    confirmed = NULL;
    done(yes, confirm_user);
}
static void test_pending_and_crashed(void) {
    installed.state = AZCM_NEW;
    snprintf(installed.from, sizeof installed.from, "%s", "author/repository");
    int n = render(CMV_MOD);
    assert(has_control(n, CMC_ALLOW) && has_control(n, CMC_MODACT) && strstr(drawn, "not yet allowed"));
    allow_result = -1;
    fire(CMC_ALLOW, 0);
    n = render(CMV_MOD);
    assert(allow_calls == 1 && strstr(drawn, "Could not save"));
    installed.from[0] = 0;
    render(CMV_MOD);
    assert(strstr(drawn, "copied there by hand"));
    allow_result = 0;
    fire(CMC_ALLOW, 0);
    n = render(CMV_MOD);
    assert(allow_calls == 2 && !has_control(n, CMC_ALLOW));
    installed.state = AZCM_SUSPENDED;
    snprintf(installed.reason, sizeof installed.reason, "crashed while this mod was starting");
    n = render(CMV_MOD);
    assert(has_control(n, CMC_UNSUSPEND) && strstr(drawn, "stopped the deck while it was starting"));
    fire(CMC_UNSUSPEND, 0);
    n = render(CMV_MOD);
    assert(unsuspend_calls == 1 && !has_control(n, CMC_UNSUSPEND) && cm_pend_get(installed.id) == 1);
    installed.state = AZCM_LOADED;
    installed.reason[0] = 0;
}
static void test_management(void) {
    fire(CMC_MODUPD, 0);
    request_is("install https://example.com/repo managed");
    fire(CMC_MODACT, CMA_DISABLE);
    request_is("disable managed");
    fire(CMC_MODACT, CMA_ENABLE);
    request_is("enable managed");
    fire(CMC_MODACT, CMA_REMOVE);
    assert(confirmed && strstr(confirm_body, "Managed is deleted"));
    request_is("");
    answer(0);
    request_is("");
    fire(CMC_MODACT, CMA_REMOVE);
    answer(1);
    request_is("remove managed");
    fire(CMC_DELREPO, 0);
    assert(confirmed && !strcmp(confirm_title, "Remove repository?"));
    answer(1);
    request_is("delrepo https://example.com/repo");
    strcpy(g_cs.state, "downloading");
    fire(CMC_MODACT, CMA_ENABLE);
    fire(CMC_MODUPD, 0);
    fire(CMC_DELREPO, 0);
    request_is("");
    assert(!confirmed);
    g_cs.state[0] = 0;
    g_cm_cat = 0;
    fire(CMC_ACT, CMA_DISABLE);
    request_is("disable managed");
    fire(CMC_ACT, CMA_ENABLE);
    request_is("enable managed");
    fire(CMC_ACT, CMA_REMOVE);
    answer(1);
    request_is("remove managed");
    fire(CMC_ACT, CMA_UPDATE);
    request_is("install https://example.com/repo managed");
    strcpy(g_cc[0].held, "other source");
    fire(CMC_ACT, CMA_INSTALL);
    request_is("");
    g_cc[0].held[0] = 0;
    strcpy(g_cc[0].fw, "different-deck");
    fire(CMC_ACT, CMA_INSTALL);
    request_is("");
    strcpy(g_cc[0].fw, "any");
    unlink(CM_TRUSTED);
    fire(CMC_ACT, CMA_INSTALL);
    assert(g_cm_view == CMV_TRUST && g_cm_sel == 1);
    request_is("");
    fire(CMC_TRUST_OK, 0);
    assert(g_cm_view == CMV_CAT && !access(CM_TRUSTED, F_OK));
    request_is("install https://example.com/repo managed");
    fire(CMC_ACT, CMA_INSTALL);
    request_is("install https://example.com/repo managed");
    unlink(CM_TRUSTED);
}
static void test_screenshots(void) {
    memset(g_cm_sh.have, 0, sizeof g_cm_sh.have);
    g_cm_sh.have[0] = g_cm_sh.have[2] = 1;
    g_cm_zoom = -1;
    fire(CMC_SHOT, 1);
    assert(g_cm_zoom == -1);
    fire(CMC_SHOT, 0);
    assert(g_cm_zoom == 0);
    fire(CMC_SHOTZ, 2);
    assert(g_cm_zoom == 2);
    fire(CMC_SHOTZ, 2);
    assert(g_cm_zoom == 0);
    fire(CMC_SHOTZ, 1);
    assert(g_cm_zoom == 2);
    fire(CMC_SHOTZ, 0);
    assert(g_cm_zoom == -1);
}
static void test_knob_and_preview(void) {
    g_cm_view = CMV_CAT;
    g_cm_cat = 0;
    for (unsigned i = 0; i + 5 < sizeof g_cc[0].desc; i += 5) memcpy(g_cc[0].desc + i, "line ", 5);
    fb.w = 320; /* narrow viewport forces the author description to scroll */
    int n = render(CMV_CAT);
    assert(g_cm_desc.max > 0 && g_cm_desc.r.h > 0);
    int count = n - (controls[n - 1].kind == CMC_BACK);
    g_cm_sel = count - 1;
    g_cm_knob = 1;
    assert(cm_input(&menu, &fb, 0, 0, false, 1, false));
    assert(g_cm_desc.skip == 2 && g_cm_sel == count - 1);
    cm_input(&menu, &fb, 0, 0, false, 1000, false);
    assert(g_cm_desc.skip == g_cm_desc.max);
    cm_input(&menu, &fb, 0, 0, false, -1000, false);
    assert(!g_cm_desc.skip);
    cm_input(&menu, &fb, 0, 0, false, -1, false);
    assert(g_cm_sel == count - 2);
    g_cm_knob = 0;
    cm_input(&menu, &fb, 0, 0, false, 1, false);
    assert(g_cm_knob && g_cm_sel == count - 2);
    cm_input(&menu, &fb, 0, 0, false, 1, false);
    assert(g_cm_sel == count - 1);
    /* Touching the description takes ownership of the vertical gesture without
     * firing a neighboring action. Movement uses whole lines and clamps at both ends. */
    g_cm_desc.skip = 1;
    int x = g_cm_desc.r.x + g_cm_desc.r.w / 2;
    int y = g_cm_desc.r.y + g_cm_desc.r.h / 2;
    g_cm_dd.on = false;
    assert(cm_input(&menu, &fb, x, y, true, 0, false));
    assert(g_cm_dd.on && g_cm_dd.y0 == y && g_cm_dd.s0 == 1 && g_cm_view == CMV_CAT);
    assert(azmod_cm_drag(&menu, &fb, y - 2 * g_cm_desc.lh, 1));
    assert(g_cm_desc.skip == 3);
    assert(azmod_cm_drag(&menu, &fb, y + 1000 * g_cm_desc.lh, 1) && !g_cm_desc.skip);
    assert(azmod_cm_drag(&menu, &fb, y - 1000 * g_cm_desc.lh, 1) && g_cm_desc.skip == g_cm_desc.max);
    assert(!azmod_cm_drag(&menu, &fb, y, 2) && !g_cm_dd.on);
    assert(cm_input(&menu, &fb, -100, -100, true, 0, false) && !g_cm_dd.on && g_cm_view == CMV_CAT);
    fb.w = 1280;
    g_cm_view = CMV_HOME;
    g_cm_tab = 0;
    g_cm_isel = CMC_MOD << 16;
    g_cm_hfoc = g_cm_isel;
    g_cm_knob = 1;
    g_cm_in = 0;
    assert(cm_split_knob(&menu, &fb, 0, true) && g_cm_in && g_cm_rsel >= 0);
    int first = g_cm_rsel;
    assert(cm_split_knob(&menu, &fb, 1, false) && g_cm_rsel > first);
    assert(cm_split_knob(&menu, &fb, -100, false) && g_cm_rsel == first);
    assert(cm_split_knob(&menu, &fb, 0, true));
    request_is("install https://example.com/repo managed");
    assert(cm_back() && !g_cm_in && g_cm_rsel == -1);
}
static void test_card_and_slide_log(void) {
    g_cm_dir = 0;
    g_cm_full = 0;
    menu.pane = 1;
    cm_input(&menu, &fb, 0, 0, false, 1, false);
    assert(!menu.pane && !g_cm_full);
    cm_input(&menu, &fb, 0, 0, false, 0, true);
    assert(g_cm_full && cm_animating());
    FILE *flag = fopen("/home/root/settings/azmod-logs.on", "w");
    assert(flag);
    fclose(flag);
    unlink("/tmp/azmod-cmslide.log");
    g_cm_nfr = 2;
    g_cm_fr[0][0] = 0;
    g_cm_fr[0][1] = 5;
    g_cm_fr[1][0] = 16;
    g_cm_fr[1][1] = 6;
    cm_slide_end(&menu);
    assert(!cm_animating());
    FILE *log = fopen("/tmp/azmod-cmslide.log", "r");
    char line[160];
    assert(log && fgets(line, sizeof line, log));
    fclose(log);
    assert(strstr(line, "open 2 frames") && strstr(line, "0/5ms 16/6ms"));
    unlink("/home/root/settings/azmod-logs.on");
    unlink("/tmp/azmod-cmslide.log");
    request_is("refresh");
}
int main(void) {
    mkdir("/home/root", 0755);
    mkdir("/home/root/settings", 0755);
    mkdir("/home/root/settings/azmod-community", 0755);
    unlink(CM_REQ);
    unlink(CM_STATUS);
    unlink(CM_CATALOG);
    cm_load();
    strcpy(installed.id, "managed");
    strcpy(installed.name, "Managed");
    strcpy(installed.version, "1.0.0");
    strcpy(installed.summary, "Managed controls");
    g_ncc = g_ncr = 1;
    strcpy(g_cc[0].id, installed.id);
    strcpy(g_cc[0].name, installed.name);
    strcpy(g_cc[0].repo, "https://example.com/repo");
    strcpy(g_cc[0].fw, "any");
    strcpy(g_cc[0].ver, "2.0.0");
    strcpy(g_cc[0].inst, "1.0.0");
    strcpy(g_cr[0].repo, g_cc[0].repo);
    strcpy(g_cr[0].name, "Example repository");
    g_cr[0].listed = 1;
    g_full = g_cm_full = 1;
    g_cm_page = 0;
    menu.pane = 1;
    menu.mode = AZ_COMMUNITY;
    g_cm_s = &menu;
    test_pending_and_crashed();
    test_management();
    test_screenshots();
    test_knob_and_preview();
    test_card_and_slide_log();
    puts("PASS community flows: approval/refusal, crash recovery, confirmation ownership, install compatibility, screenshots, description and preview navigation, slide logging");
    return 0;
}

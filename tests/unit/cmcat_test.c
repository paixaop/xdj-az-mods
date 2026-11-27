/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mod Menu's community catalog (menu.c cm_load / cm_layout) against catalog files:
 *   - every repository the fetcher can list (64 + 32) is held, and every mod line up to CM_NCAT;
 *   - mods past what the menu holds are never dropped silently: the Browse tab says
 *     "Showing the first N of M mods." (lines past the array plus the fetcher's more|<n>);
 *   - every tile drawn can be touched (a control per tile);
 *   - a mod from a repository that is not in the Mod Index carries a corner mark on its
 *     tile and the "Not in the Mod Index" note on its page; a listed one carries neither;
 *   - the same id elsewhere (fields 17-19): a line whose id another installed copy holds says so
 *     on its tile and page and offers no Install; a line others also offer says so; the name of
 *     an installed id never comes from another author's line;
 *   - Browse (field 20 and cm_browse.h through the real screen): the view buttons and their
 *     counts, the orders, and the native Add repository prompt;
 *   - the download count (field 21): read, drawn on a tile and a mod page only when there is
 *     one; the Downloads order; Home's shelves (one row each, the tiles that fit), See all, a
 *     shelf tile, the knob scrolling the page (no focus); the mod page's header, buttons and
 *     Permissions.
 * menu.c is #included (MENU_C) so its statics are visible; CM_CATALOG points at a temp file.
 * CM_NOW pins the clock a mod page's "updated N days ago" is counted from.
 * Text is captured through azmod_ttf_menu, which every c_text call goes through first. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#define CM_NOW() 1950134400LL /* 2031-10-19 UTC: a mod page's "updated N days ago" draws the same every day */
#define CM_REQ CM_CATALOG ".request"
#include MENU_C

static az_text_entry_v1 repo_prompt;
int azcm_text_entry(const az_text_entry_v1 *entry) {
    repo_prompt = *entry;
    return AZ_OK;
}

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
static int seen(const char *s) { return strstr(g_seen, s) != NULL; }
/* how many drawn texts are a count alone ("1,840", "0"): a tile's download line */
static int ncounts(void) {
    int k = 0;
    for (const char *p = g_seen; *p;) {
        const char *e = strchr(p, '\n');
        size_t L = e ? (size_t) (e - p) : strlen(p);
        int digits = L > 0;
        for (size_t i = 0; i < L; i++)
            if (!(p[i] >= '0' && p[i] <= '9') && p[i] != ',') digits = 0;
        k += digits;
        p += L + (e ? 1 : 0);
    }
    return k;
}

#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static azmod_state g_s;
static cm_ctl g_ctl[CM_MAXCTL];
static int g_nctl;

static void layout(int view, int tab, int cat) {
    memset(g_px, 0, sizeof g_px);
    g_seen_n = 0;
    g_seen[0] = 0;
    g_cm_full = 1;
    g_cm_view = view;
    g_cm_tab = tab;
    g_cm_cat = cat;
    g_cm_scroll = 0;
    g_nctl = cm_layout(&g_s, &g_fb, g_ctl, 1);
}
static int nkind(int kind) {
    int k = 0;
    for (int i = 0; i < g_nctl; i++) k += g_ctl[i].kind == kind;
    return k;
}
static uint32_t tile_px(int tile_idx, int left) {   /* the pixel just inside a tile's top corner */
    for (int i = 0; i < g_nctl; i++)
        if (g_ctl[i].kind == CMC_TILE && g_ctl[i].idx == tile_idx) {
            int x = left ? g_ctl[i].x + 1 : g_ctl[i].x + g_ctl[i].w - 2, y = g_ctl[i].y + 1;
            if (x < 0 || y < 0 || x >= W || y >= H) return 0xFFFFFFFFu;
            uint32_t v;
            memcpy(&v, g_px + (size_t) y * g_fb.stride + (size_t) x * 4, 4);
            return v & 0x00FFFFFFu;
        }
    return 0xFFFFFFFFu;
}
static uint32_t corner(int tile_idx) { return tile_px(tile_idx, 0); }

static void write_catalog(int nrepo, int nmod, int more) {
    FILE *f = fopen(CM_CATALOG ".new", "w");
    fprintf(f, "v1\n");
    for (int r = 0; r < nrepo; r++)   /* repository 0 is in the Mod Index, the rest were added */
        fprintf(f, "repo|https://example.com/r%d/azmods-repo.json|Repo %d|1|%d|\n", r, r, r == 0);
    /* mod 1 comes from an added repository, every other from repo 0; mod 2 is beta (field 22) and
     * names two screenshots, one of them a path the fetcher never writes (field 23) */
    for (int i = 0; i < nmod; i++)
        fprintf(f, "mod|https://example.com/r%d/azmods-repo.json|mod-%03d|Mod %d|1.0.0|someone|Does a thing|tools|0|any|1000|||||0%s\n",
                i == 1 ? 1 : 0, i, i,
                i == 2 ? "||||||1|" CM_SHOTDIR "0123456789abcdef.png /tmp/elsewhere/0123456789abcdef.png" : "");
    if (more) fprintf(f, "more|%d\n", more);
    fclose(f);
    rename(CM_CATALOG ".new", CM_CATALOG);
    usleep(300000);   /* cm_load re-reads at most 4x a second */
    cm_load();
}

static void write_raw(const char *body) {
    FILE *f = fopen(CM_CATALOG ".new", "w");
    fprintf(f, "v1\nrepo|https://example.com/r0/azmods-repo.json|Repo 0|1|1|\n%s", body);
    fclose(f);
    rename(CM_CATALOG ".new", CM_CATALOG);
    usleep(300000);
    cm_load();
}
static int ctl_of(int kind, int idx) {
    for (int i = 0; i < g_nctl; i++)
        if (g_ctl[i].kind == kind && g_ctl[i].idx == idx) return i;
    return -1;
}
/* the control itself; one off the screen when there is none (a check on it then fails) */
static const cm_ctl *ctl_p(int kind, int idx) {
    static const cm_ctl none = {-1, -1, -1000, -1000, 1, 1};
    int i = ctl_of(kind, idx);
    return i < 0 ? &none : &g_ctl[i];
}

int main(void) {
    ut_suite = "catalog";
    /* the fetcher's most: 64 + 32 repositories; more mods than the menu holds */
    write_catalog(96, 200, 50);
    CHECK("all 96 repositories the fetcher can list are held", g_ncr == 96 && g_cr_more == 0);
    CHECK("the first 128 mod lines are held", g_ncc == 128);
    CHECK("mods past the array and the fetcher's more| line are counted (72 + 50)", g_cc_more == 122);
    CHECK("Browse opens on Home", g_cm_fcat == CMB_HOME);
    g_cm_fcat = CMB_ALL;   /* the grid: All mods (Home is checked below) */
    layout(CMV_HOME, 1, -1);
    CHECK("Browse says it shows only the first ones", seen("Showing the first 128 of 250 mods."));
    CHECK("every one of the 128 tiles can be touched", nkind(CMC_TILE) == 128);
    CHECK("a mod from an added repository has the orange corner mark on its tile", corner(1) == C_HI);
    CHECK("CONTROL: a mod from the Mod Index has none", corner(0) != C_HI && corner(0) != 0xFFFFFFFFu);
    CHECK("a beta mod (field 22) is read as beta; CONTROL: the others are not", g_cc[2].beta && !g_cc[0].beta && !g_cc[1].beta);
    CHECK("a beta mod has the blue mark in its top-left corner", tile_px(2, 1) == CM_BETA_BG);
    CHECK("CONTROL: a mod that is not beta has none", tile_px(0, 1) != CM_BETA_BG && tile_px(1, 1) != CM_BETA_BG);
    CHECK("BETA is written inside the mark", seen("BETA"));
    CHECK("screenshots (field 23): only the fetcher's own directory is taken", g_cc[2].nshots == 1 &&
                                                                                   !strcmp(g_cc[2].shots[0], CM_SHOTDIR "0123456789abcdef.png") && g_cc[0].nshots == 0);
    layout(CMV_CAT, 0, 2);
    CHECK("a beta mod's page says so", seen("BETA: This mod may not be stable yet."));
    layout(CMV_CAT, 0, 0);
    CHECK("CONTROL: a mod that is not beta does not", !seen("BETA:") && seen("Permissions"));
    g_cm_fcat = CMB_ALL;
    layout(CMV_HOME, 1, -1);
    layout(CMV_CAT, 0, 2);
    CHECK("a beta mod's page says so", seen("BETA: This mod may not be stable yet."));
    layout(CMV_CAT, 0, 0);
    CHECK("CONTROL: a mod that is not beta does not", !seen("BETA:") && seen("Permissions"));
    g_cm_fcat = CMB_ALL;
    layout(CMV_HOME, 1, -1);
    layout(CMV_HOME, 2, -1);
    CHECK("every added repository has its Remove control (95)", nkind(CMC_DELREPO) == 95);
    CHECK("no repository note when none was dropped", !seen("repositories."));
    layout(CMV_CAT, 0, 1);
    CHECK("an added repository's mod page says \"" CM_UNLISTED "\"", seen(CM_UNLISTED));
    layout(CMV_CAT, 0, 0);
    CHECK("CONTROL: a listed repository's mod page does not", !seen(CM_UNLISTED) && seen("Permissions"));

    /* a small catalog: everything shown, nothing to say */
    write_catalog(2, 3, 0);
    layout(CMV_HOME, 1, -1);
    CHECK("CONTROL: a catalog the menu holds whole has no note", g_ncc == 3 && g_cc_more == 0 && !seen("Showing the first") && nkind(CMC_TILE) == 3);

    /* more repository lines than the menu holds (a newer fetcher): said, not dropped silently */
    write_catalog(100, 3, 0);
    layout(CMV_HOME, 2, -1);
    CHECK("repository lines past the array are counted and said", g_ncr == 96 && g_cr_more == 4 && seen("Showing the first 96 of 100 repositories."));
    /* the same id elsewhere: line 0 is held by a copy from a repository that is gone, line 1 is
     * offered by two other repositories too, line 2 by nobody else (the control) */
    write_raw("mod|https://example.com/r0/azmods-repo.json|beat-tool|Beat Tool|1.0.0|alice|s|tools|0|any|1000|||||0|example.com/bob|example.com/bob/azmods-repo.json|1\n"
              "mod|https://example.com/r0/azmods-repo.json|loop-tool|Loop Tool|1.0.0|carol|s|tools|0|any|1000|||||0||someone/loops|2\n"
              "mod|https://example.com/r0/azmods-repo.json|plain-tool|Plain Tool|1.0.0|dave|s|tools|0|any|1000|||||0||0\n");
    CHECK("fields 17-19 are read (held, also, nalso)", g_ncc == 3 && !strcmp(g_cc[0].held, "example.com/bob") && g_cc[1].nalso == 2 && !strcmp(g_cc[1].also, "someone/loops"));
    layout(CMV_HOME, 1, -1);
    CHECK("a tile whose id another installed copy holds says so", seen("Its ID is in use"));
    layout(CMV_CAT, 0, 0);
    CHECK("its page names where the installed one came from", seen("A different mod with this ID is installed (from example.com/bob). Remove it first."));
    CHECK("its page offers no Install (the control is drawn inactive: the press is refused too)", ctl_of(CMC_ACT, CMA_INSTALL) >= 0);
    layout(CMV_CAT, 0, 1);
    CHECK("a line others also offer says how many", seen("someone/loops and 1 more also offer a mod with this ID. Only one can be installed."));
    layout(CMV_CAT, 0, 2);
    CHECK("CONTROL: a line nobody else offers carries neither note", !seen("this ID") && seen("Permissions"));
    CHECK("the name of a held id is not taken from the other author's line", !strcmp(cm_cat_name("beat-tool"), "beat-tool"));
    CHECK("CONTROL: an id nobody holds is named from its line", !strcmp(cm_cat_name("plain-tool"), "Plain Tool"));
    write_raw("mod|https://example.com/r0/azmods-repo.json|beat-tool|Beat Tool|1.0.0|alice|s|tools|0|any|1000|||||0\n");
    layout(CMV_CAT, 0, 0);
    CHECK("CONTROL: a 16-field line (an older fetcher) still reads, with no notes", g_ncc == 1 && !g_cc[0].held[0] && g_cc[0].nalso == 0 && !seen("this ID"));

    /* Browse: field 20 (updated), the category row and the orders (cm_browse.h) */
    write_raw("mod|https://example.com/r0/azmods-repo.json|echo-out|Echo Out|1.0.0|dubwise|s|audio|0|any|1000|||||0||||2019-09-14\n"
              "mod|https://example.com/r0/azmods-repo.json|big-clock|Big Clock|1.0.0|nightowl|s|visual|0|any|1000|||||0||||2019-10-12\n"
              "mod|https://example.com/r0/azmods-repo.json|old-date|Aged|1.0.0|dubwise|s|tools|0|any|1000|||||0||||20191008\n");
    CHECK("field 20 is read; a malformed date is dropped", !strcmp(g_cc[0].upd, "2019-09-14") && !strcmp(g_cc[1].upd, "2019-10-12") && !g_cc[2].upd[0]);
    layout(CMV_HOME, 1, -1);
    CHECK("Browse: the Sort button, its orders closed, and six view buttons (Other has no mods: none)",
          nkind(CMC_SORTBTN) == 1 && nkind(CMC_SORT) == 0 && nkind(CMC_FCAT) == 6 && ctl_of(CMC_FCAT, CMB_OTHER) < 0);
    cm_fire(ctl_p(CMC_SORTBTN, 0));
    layout(CMV_HOME, 1, -1);
    CHECK("Sort opens the four orders", nkind(CMC_SORT) == 4 && g_cm_sortpop);
    CHECK("Browse: the buttons count the mods", seen("Home") && seen("All mods  3") && seen("Audio  1") && seen("Visual  1") && seen("Tools  1") && !seen("Other"));
    CHECK("A-Z: Aged, Big Clock, Echo Out", ctl_p(CMC_TILE, 2)->x < ctl_p(CMC_TILE, 1)->x && ctl_p(CMC_TILE, 1)->x < ctl_p(CMC_TILE, 0)->x);
    cm_fire(ctl_p(CMC_SORT, CMB_SORT_UPDATED));
    layout(CMV_HOME, 1, -1);
    CHECK("an order picked: the orders close", !g_cm_sortpop && nkind(CMC_SORT) == 0);
    CHECK("Updated: the newest first, the undated last", g_cm_sort == CMB_SORT_UPDATED && ctl_p(CMC_TILE, 1)->x < ctl_p(CMC_TILE, 0)->x &&
                                                             ctl_p(CMC_TILE, 0)->x < ctl_p(CMC_TILE, 2)->x);
    g_cm_scroll = 300;
    cm_fire(ctl_p(CMC_FCAT, 0));
    CHECK("a category button picks it and goes back to the top", g_cm_fcat == 0 && g_cm_scroll == 0);
    layout(CMV_HOME, 1, -1);
    CHECK("Visual: only its mod", nkind(CMC_TILE) == 1 && ctl_of(CMC_TILE, 1) >= 0);
    layout(CMV_HOME, 2, -1);
    cm_fire(ctl_p(CMC_ADDREPO, 0));
    CHECK("Add repository opens the native prompt", repo_prompt.on_result &&
                                                        !strcmp(repo_prompt.title, "Add repository") && !strcmp(repo_prompt.submit, "Add"));
    unlink(CM_REQ);
    CHECK("An empty address stays open", repo_prompt.on_result(AZ_OK, "", NULL) != NULL);
    CHECK("A line break is refused", repo_prompt.on_result(AZ_OK, "owner/repo\nnext", NULL) != NULL);
    CHECK("Cancel writes no request", !repo_prompt.on_result(AZ_CANCELLED, NULL, NULL) &&
                                          access(CM_REQ, F_OK) != 0);
    g_cs.state[0] = 0;
    CHECK("A repository address is accepted", !repo_prompt.on_result(AZ_OK, "owner/repo", NULL));
    char request[64] = "";
    FILE *repo_request = fopen(CM_REQ, "r");
    if (repo_request) {
        fgets(request, sizeof request, repo_request);
        fclose(repo_request);
    }
    CHECK("An accepted address reaches the repository worker", !strcmp(request, "addrepo owner/repo\n"));
    unlink(CM_REQ);
    g_cm_kb[0] = 0;
    g_cm_sort = CMB_SORT_NAME;

    /* the download count (field 21) and Home. Echo Out 1840, Big Clock 12 (an older version
     * installed), Aged 0, Loop Kit a damaged count, Hand Made from a repository the catalog does not
     * list and a 20-field line (no count) */
    write_raw("mod|https://example.com/r0/azmods-repo.json|echo-out|Echo Out|1.0.0|dubwise|s|audio|0|any|1000|||||0||||2031-10-10|1840\n" /* UTC */
              "mod|https://example.com/r0/azmods-repo.json|big-clock|Big Clock|1.0.0|nightowl|s|visual|0|any|1000||0.9.0|||0||||2031-10-18|12\n" /* UTC */
              "mod|https://example.com/r0/azmods-repo.json|old-date|Aged|1.0.0|dubwise|s|tools|0|any|1000|||||0|||||0\n"
              "mod|https://example.com/r0/azmods-repo.json|loop-kit|Loop Kit|1.0.0|carol|s|audio|0|any|1000|||||0||||2031-10-01|1e3\n" /* UTC */
              "mod|https://example.com/r1/azmods-repo.json|hand-made|Hand Made|1.0.0|eve|s|visual|0|any|1000|||||0||||2031-08-01\n" /* UTC */);
    CHECK("field 21 is read: a count, 0 too; a damaged one or none is no count",
          g_ncc == 5 && g_cc[0].counted && g_cc[0].dl == 1840 && g_cc[2].counted && g_cc[2].dl == 0 && !g_cc[3].counted && !g_cc[4].counted);
    g_cm_fcat = CMB_ALL;
    g_cm_sort = CMB_SORT_DOWNLOADS;
    layout(CMV_HOME, 1, -1);
    CHECK("Downloads: most first, then the mods with no count by name", ctl_p(CMC_TILE, 0)->x < ctl_p(CMC_TILE, 1)->x &&
                                                                            ctl_p(CMC_TILE, 1)->x < ctl_p(CMC_TILE, 2)->x &&
                                                                            ctl_p(CMC_TILE, 2)->x < ctl_p(CMC_TILE, 4)->x &&
                                                                            ctl_p(CMC_TILE, 4)->x < ctl_p(CMC_TILE, 3)->x);
    CHECK("a tile shows its count (\"1,840\", \"12\", and \"0\"), the two with none show nothing", seen("1,840") && ncounts() == 3);
    g_cm_fcat = CMB_HOME;
    g_cm_sort = CMB_SORT_NAME;
    layout(CMV_HOME, 1, -1);
    CHECK("Home: Most downloaded, New & updated, Visual, Audio (Tools holds one mod: not shown)",
          nkind(CMC_SEEALL) == 4 && ctl_of(CMC_SEEALL, CMB_SH_TOP) >= 0 && ctl_of(CMC_SEEALL, CMB_SH_NEW) >= 0 &&
              ctl_of(CMC_SEEALL, CMB_SH_CAT + 0) >= 0 && ctl_of(CMC_SEEALL, CMB_SH_CAT + 2) >= 0 && seen("Most downloaded") && seen("New & updated"));
    CHECK("Home: each tile once per shelf (2 + 4 + 2 + 2), no grid tile, no orders", nkind(CMC_STILE) == 10 && nkind(CMC_TILE) == 0 && nkind(CMC_SORT) == 0 && nkind(CMC_SORTBTN) == 0);
    CHECK("Home: Most downloaded holds only counts above 0, most first",
          ctl_p(CMC_STILE, CMB_SH_TOP << 8 | 0)->x < ctl_p(CMC_STILE, CMB_SH_TOP << 8 | 1)->x && ctl_of(CMC_STILE, CMB_SH_TOP << 8 | 2) < 0);
    CHECK("Home: New & updated, the newest first", ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 1)->x < ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 0)->x &&
                                                       ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 0)->x < ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 3)->x &&
                                                       ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 3)->x < ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 4)->x);
    CHECK("Home: no note about the counts below the shelves", !seen("how many times"));
    cm_fire(ctl_p(CMC_SEEALL, CMB_SH_TOP));
    CHECK("See all on Most downloaded: All mods by downloads", g_cm_fcat == CMB_ALL && g_cm_sort == CMB_SORT_DOWNLOADS);
    g_cm_fcat = CMB_HOME;
    layout(CMV_HOME, 1, -1);
    cm_fire(ctl_p(CMC_SEEALL, CMB_SH_NEW));
    CHECK("See all on New & updated: All mods by date", g_cm_fcat == CMB_ALL && g_cm_sort == CMB_SORT_UPDATED);
    g_cm_fcat = CMB_HOME;
    layout(CMV_HOME, 1, -1);
    cm_fire(ctl_p(CMC_SEEALL, CMB_SH_CAT + 0));
    CHECK("See all on Visual: Visual by downloads", g_cm_fcat == 0 && g_cm_sort == CMB_SORT_DOWNLOADS);
    g_cm_fcat = CMB_HOME;
    layout(CMV_HOME, 1, -1);
    cm_fire(ctl_p(CMC_STILE, CMB_SH_NEW << 8 | 3));
    CHECK("a shelf's tile opens that mod's page", g_cm_view == CMV_CAT && g_cm_cat == 3);
    g_cm_view = CMV_HOME;
    /* the knob on Home: it scrolls the whole page and focuses nothing; its press does nothing */
    cm_input(&g_s, &g_fb, 0, 0, false, 1, false);
    CHECK("knob on Home: a detent scrolls the page down", (g_cm_gl.on ? g_cm_gl.goal : (float) g_cm_scroll) > 0.f);
    cm_input(&g_s, &g_fb, 0, 0, false, -1, false);
    CHECK("knob on Home: and back up", (g_cm_gl.on ? g_cm_gl.goal : (float) g_cm_scroll) == 0.f);
    {
        int sort0 = g_cm_sort;
        cm_input(&g_s, &g_fb, 0, 0, false, 0, true);
        CHECK("knob on Home: a press opens nothing and changes no view or order", g_cm_view == CMV_HOME && g_cm_fcat == CMB_HOME && g_cm_sort == sort0);
    }
    glide_stop(&g_cm_gl);
    /* the mod page */
    layout(CMV_CAT, 1, 0);
    CHECK("page: \"author  .  category  .  version, updated N days ago\"", seen("dubwise  \xb7  Audio  \xb7  v1.0.0, updated 9 days ago"));
    CHECK("page: the download count and where it comes from", seen("1,840 downloads") && seen("from https://example.com/r0/azmods-repo.json"));
    CHECK("page: Install, Permissions beside the description", ctl_of(CMC_ACT, CMA_INSTALL) >= 0 && seen("Permissions") && seen("Firmware: any") &&
                                                                   seen("Uses only AZ-MODS features (no hooks)"));
    layout(CMV_CAT, 1, 1);
    /* (the stub's azcm_ver_newer never says newer, so "Update to <version>" is checked on the emulator) */
    CHECK("page: an installed mod names its version, offers Disable and Remove", seen("Installed: v0.9.0") && seen("updated yesterday") &&
                                                                                     ctl_of(CMC_ACT, CMA_DISABLE) >= 0 && ctl_of(CMC_ACT, CMA_REMOVE) >= 0);
    layout(CMV_CAT, 1, 2);
    CHECK("page: a count of 0 is said; no date, no \"updated\"", seen("0 downloads") && seen("dubwise" CM_DOT "Tools" CM_DOT "v1.0.0") && !seen("updated"));
    layout(CMV_CAT, 1, 3);
    CHECK("page: no count, no download line", !seen("downloads") && !seen(" download\n"));
    /* a shelf longer than a row: the tiles that fit, no more (See all shows the rest) */
    {
        char cat[4096];
        int o = 0;
        for (int i = 0; i < 8; i++)
            o += snprintf(cat + o, sizeof cat - o, "mod|https://example.com/r0/azmods-repo.json|aud-%d|Aud %d|1.0.0|a|s|audio|0|any|1000|||||0|||||%d\n", i, i, 100 - i);
        write_raw(cat);
    }
    g_cm_fcat = CMB_HOME;
    layout(CMV_HOME, 1, -1);
    int k2 = CMB_SH_CAT + 2, bx, by, bw, bh;
    cm_box(&g_fb, &bx, &by, &bw, &bh);
    {
        int n5 = 0, in = 1;
        for (int i = 0; i < g_nctl; i++)
            if (g_ctl[i].kind == CMC_STILE && g_ctl[i].idx >> 8 == k2) n5++, in &= g_ctl[i].x >= bx && g_ctl[i].x + g_ctl[i].w <= bx + bw;
        CHECK("a shelf of 8, the full screen: the 5 that fit, each whole inside the grid", n5 == 5 && in);
    }
    g_cm_full = 0;   /* 3 columns */
    g_nctl = cm_layout(&g_s, &g_fb, g_ctl, 0);
    cm_box(&g_fb, &bx, &by, &bw, &bh);
    {
        int n3 = 0, in = 1;
        for (int i = 0; i < g_nctl; i++)
            if (g_ctl[i].kind == CMC_STILE && g_ctl[i].idx >> 8 == k2) n3++, in &= g_ctl[i].x >= bx && g_ctl[i].x + g_ctl[i].w <= bx + bw;
        CHECK("the same shelf on the smaller screen: the 3 that fit, each whole inside the grid", n3 == 3 && in);
    }
    g_cm_full = 1;
    g_cm_fcat = CMB_ALL;
    g_cm_sort = CMB_SORT_NAME;
    unlink(CM_CATALOG);
    /* the header: the breadcrumb on every page, the trust note on the Installed tab only */
    layout(CMV_HOME, 0, -1);
    CHECK("Installed: \"Community Mods  >  Installed\" and the trust note",
          seen("Community Mods  >  Installed") && seen(CM_TRUST_NOTE));
    layout(CMV_HOME, 1, -1);
    CHECK("Browse: its breadcrumb, no trust note", seen("Community Mods  >  Browse") && !seen("Made by other"));
    layout(CMV_HOME, 2, -1);
    CHECK("Repositories: its breadcrumb, no trust note", seen("Community Mods  >  Repositories") && !seen("Made by other"));
    if (g_ncc > 0) {
        char want[256];
        snprintf(want, sizeof want, "Community Mods  >  Browse  >  %s", g_cc[0].name);
        layout(CMV_CAT, 1, 0);
        CHECK("a mod's card opened from Browse: \"Community Mods  >  Browse  >  <name>\"", seen(want));
        snprintf(want, sizeof want, "Community Mods  >  Installed  >  %s", g_cc[0].name);
        layout(CMV_CAT, 0, 0);
        CHECK("... from Installed: \"Community Mods  >  Installed  >  <name>\"", seen(want) && !seen("Made by other"));
        snprintf(want, sizeof want, "Community Mods  >  Browse  >  %s  >  Install", g_cc[0].name);
        layout(CMV_TRUST, 1, 0);
        CHECK("its install confirmation: \"...  >  <name>  >  Install\"", seen(want));
    } else
        CHECK("the catalog has a mod for the breadcrumb checks", 0);
    {
        char t[CM_CRUMB_MAX];
        g_cm_view = CMV_RELOAD;
        cm_crumbs(t, sizeof t, 0, 20);
        CHECK("the reload page: \"Community Mods  >  Reload\"", !strcmp(t, "Community Mods  >  Reload"));
        cm_ctl cancel = {CMC_RELOAD_NO, 0, 600, 300, 140, 50};
        struct cm_input_event touch = {
            .fb = &g_fb, .ctl = &cancel, .n = 1, .tx = 585, .ty = 315, .touched = true};
        int tab_before = g_cm_tab;
        g_cm_reload_from = CMV_HOME;
        cm_input_page_touch(&touch);
        CHECK("tapping outside the expanded Cancel target keeps the reload page", g_cm_view == CMV_RELOAD);
        touch.tx = 596; /* four pixels outside the painted button, inside its touch margin */
        cm_input_page_touch(&touch);
        CHECK("tapping inside the Cancel margin returns to the previous tab", g_cm_view == CMV_HOME && g_cm_tab == tab_before);
    }
    return ut_summary();
}

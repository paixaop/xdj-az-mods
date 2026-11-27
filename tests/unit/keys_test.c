/* SPDX-License-Identifier: GPL-3.0-only */
/* The official Mod Menu keys are a stable public list: community mods read official rows by
 * key (row_lookup), so a key renamed in features.def breaks them. This prints the table from
 * the release build's features.def (the rows row_lookup offers) and requires the published
 * table, between the markers in docs/MOD-MENU-KEYS.md, to be exactly it.
 *   keys_test <doc>          check (FAIL lists the expected table)
 *   keys_test --print        print the table (to update the doc deliberately) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include MENU_C
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) s;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    return 0;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold,
                    uint32_t c, int split, uint32_t c2) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) s;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    (void) split;
    (void) c2;
    return 0;
}
#define BEGIN "<!-- keys:begin (generated from features.def by keys_test.c --print, in the unit tests) -->\n"
#define END "<!-- keys:end -->\n"
static char g_t[16384];
static size_t g_n;
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_n += (size_t) vsnprintf(g_t + g_n, sizeof g_t - g_n, fmt, ap);
    va_end(ap);
}
static void table(void) {
    static const char *const KIND[] = {"toggle", "group", "action", "screen", "choice"};
    out("| key | label | kind | parent | values |\n|---|---|---|---|---|\n");
    int ord[AZI_COUNT], n = az_display_order(ord);
    for (int p = 0; p < n; p++) {
        int i = ord[p];
        if (i >= AZF_COUNT || azf_row_lookup(az_row(i)->d.key) != i)
            continue;
        const struct az_row *r = az_row(i);
        const char *lab = r->d.label;
        int par = az_parent(i);
        out("| `%s` | %s | %s | %s | ", r->d.key, lab, KIND[r->d.kind], par >= 0 ? az_row(par)->d.key : "-");
        if (r->d.kind == AZ_ROW_CHOICE)
            for (int k = 0; k < r->d.n_choices; k++)
                out("%s%d = \"%s\"", k ? ", " : "", k, r->d.choices[k]);
        else
            out("0 = off, 1 = on");
        out(" |\n");
    }
}
static void official_setting_test(void) {
    az_defaults();
    az_official_setting_v1 value = {.size = sizeof value};
    int item = azf_row_lookup("stems");
    CHECK("official setting copies the stable key and configured value",
          item >= 0 && azf_official_setting((unsigned) item, &value) == AZ_OK &&
              !strcmp(value.key, "stems") && value.configured == az_get(item));
    int old_value = value.configured;
    CHECK("official setting rejects an invalid toggle and stale expected value",
          azf_official_setting_write("stems", old_value, 4) == AZ_INVALID &&
              azf_official_setting_write("stems", !old_value, !old_value) == AZ_STALE);
    CHECK("official setting writes the exact requested toggle value",
          azf_official_setting_write("stems", old_value, !old_value) == AZ_OK &&
              azf_official_setting((unsigned) item, &value) == AZ_OK && value.configured == !old_value);
    CHECK("official setting enumeration ends explicitly",
          azf_official_setting(AZF_COUNT, &value) == AZ_NOT_FOUND &&
              azf_official_setting(0, NULL) == AZ_INVALID &&
              azf_official_setting_write("unknown-test-setting", 0, 1) == AZ_NOT_FOUND);
}

int main(int argc, char **argv) {
    ut_suite = "Mod Menu keys";
    table();
    if (argc == 2 && !strcmp(argv[1], "--print")) {
        fputs(BEGIN, stdout);
        fputs(g_t, stdout);
        fputs(END, stdout);
        return 0;
    }
    if (argc != 2) return 2;
    static char doc[1 << 16];
    FILE *f = fopen(argv[1], "rb");
    size_t dn = f ? fread(doc, 1, sizeof doc - 1, f) : 0;
    if (f) fclose(f);
    doc[dn] = 0;
    char *b = strstr(doc, BEGIN), *e = b ? strstr(b, END) : NULL;
    int ok = b && e && (size_t) (e - (b + strlen(BEGIN))) == g_n && !memcmp(b + strlen(BEGIN), g_t, g_n);
    CHECK("docs/MOD-MENU-KEYS.md lists exactly features.def's official keys (release build)", ok);
    if (!ok) printf("expected:\n%s%s%s", BEGIN, g_t, END);
    CHECK("every official key is unique", ({ int u = 1; for (int i = 0; i < AZF_COUNT; i++) for (int j = i + 1; j < AZF_COUNT; j++)
          u &= strcmp(az_row(i)->d.key, az_row(j)->d.key) != 0; u; }));
    CHECK("every key is [a-z0-9-], 1..24", ({ int u = 1; for (int i = 0; i < AZF_COUNT; i++) u &= az_key_ok(az_row(i)->d.key); u; }));
    CHECK("parents come before their children", ({ int u = 1; for (int i = 0; i < AZF_COUNT; i++) u &= az_parent(i) < i; u; }));
    CHECK("every legacy slot is a row's or retired, never both, never two rows'", ({ int u = 1;
          for (int k = 0; k < AZ_CFG_WIDTH; k++) { int rows = 0;
              for (int i = 0; i < AZF_COUNT; i++) { const struct az_row *r = az_row(i);
                  if (r->cfg >= 0 && k >= r->cfg && k < r->cfg + (r->radio_n ? r->radio_n : 1)) rows++; }
              u &= (rows == 1) != az_cfg_retired(k) && rows <= 1; } u; }));
    official_setting_test();
    return ut_summary();
}

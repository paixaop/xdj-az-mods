/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
static int fail_set;
int __real_azconf_set(struct azconf *, const char *, const char *);
int __wrap_azconf_set(struct azconf *conf, const char *key, const char *value) {
    if (fail_set && !strcmp(key, "track-analysis")) return -1;
    return __real_azconf_set(conf, key, value);
}
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    struct azconf *conf = azconf_new();
    assert(conf && !analysis_settings_migrate(conf));
    assert(!azconf_get(conf, "track-analysis"));
    assert(!azconf_set(conf, "track-analysis:enabled", "off"));
    assert(!azconf_set(conf, "unrelated-feature", "on"));
#ifdef __linux__
    fail_set = 1;
    assert(analysis_settings_migrate(conf) == -1);
    assert(!azconf_get(conf, "track-analysis"));
    assert(!strcmp(azconf_get(conf, "track-analysis:enabled"), "off"));
    fail_set = 0;
#endif
    assert(!analysis_settings_migrate(conf));
    assert(!azconf_get_bool(conf, "track-analysis", 1));
    assert(!azconf_get(conf, "track-analysis:enabled"));
    assert(!analysis_settings_migrate(conf));
    assert(!azconf_save(conf, argv[1]));
    assert(!azconf_load(conf, argv[1]));
    assert(!azconf_get_bool(conf, "track-analysis", 1));
    assert(azconf_get_bool(conf, "unrelated-feature", 0));
    assert(!azconf_set(conf, "track-analysis:enabled", "on"));
    assert(!analysis_settings_migrate(conf));
    assert(!azconf_get_bool(conf, "track-analysis", 1)); /* core setting wins */
    assert(!azconf_get(conf, "track-analysis:enabled"));
    azconf_free(conf);
    puts("analysis settings: migration, persistence, precedence, and failure passed");
}

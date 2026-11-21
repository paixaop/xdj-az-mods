/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
static int fail_set;
static const char *fail_key = "track-analysis";
int __real_azconf_set(struct azconf *, const char *, const char *);
int __wrap_azconf_set(struct azconf *conf, const char *key, const char *value) {
    if (fail_set && !strcmp(key, fail_key)) return -1;
    return __real_azconf_set(conf, key, value);
}
#endif
static void vocal_preferences(void) {
    struct azconf *conf = azconf_new();
    assert(conf && !analysis_settings_migrate(conf));
    assert(!azconf_get(conf, "vocal-lines-waveform"));
    assert(!azconf_get(conf, "vocal-lines-phrases"));
    assert(!azconf_set(conf, "vocal-indicator", "on"));
    assert(!azconf_set(conf, "vocal-indicator-waveform", "off"));
#ifdef __linux__
    fail_key = "vocal-lines-waveform";
    fail_set = 1;
    assert(analysis_settings_migrate(conf) == -1);
    assert(!azconf_get(conf, "vocal-lines-waveform"));
    fail_key = "vocal-lines-phrases";
    assert(analysis_settings_migrate(conf) == -1);
    assert(!azconf_get_bool(conf, "vocal-lines-waveform", 1));
    assert(!azconf_get(conf, "vocal-lines-phrases"));
    fail_set = 0;
    fail_key = "track-analysis";
#endif
    assert(!analysis_settings_migrate(conf));
    assert(!azconf_get_bool(conf, "vocal-lines-waveform", 1));
    assert(azconf_get_bool(conf, "vocal-lines-phrases", 0));
    assert(!azconf_set(conf, "vocal-lines-waveform", "on"));
    assert(!azconf_set(conf, "vocal-indicator", "off"));
    assert(!analysis_settings_migrate(conf));
    assert(azconf_get_bool(conf, "vocal-lines-waveform", 0));
    assert(!azconf_unset(conf, "vocal-lines-waveform"));
    assert(!azconf_unset(conf, "vocal-lines-phrases"));
    assert(!analysis_settings_migrate(conf));
    assert(!azconf_get_bool(conf, "vocal-lines-waveform", 1));
    assert(!azconf_get_bool(conf, "vocal-lines-phrases", 1));
    azconf_free(conf);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    vocal_preferences();
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

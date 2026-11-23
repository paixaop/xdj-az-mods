/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_settings.h"
static int migrate_analysis_toggle(struct azconf *settings) {
    const char *previous = azconf_get(settings, "track-analysis:enabled");
    if (!previous) return 0;
    if (!azconf_get(settings, "track-analysis") && azconf_set(settings, "track-analysis", previous)) return -1;
    return azconf_unset(settings, "track-analysis:enabled");
}
static int migrate_vocal_lines(struct azconf *settings) {
    if (!azconf_get(settings, "vocal-lines-waveform") && !azconf_get(settings, "vocal-lines-phrases")) return 0;
    int wave = azconf_get_bool(settings, "vocal-lines-waveform", 0);
    int preview = azconf_get_bool(settings, "vocal-lines-phrases", 1);
    if (azconf_set(settings, "vocal-indicator-waveform", wave ? "on" : "off")) return -1;
    if (azconf_set(settings, "vocal-indicator-preview", preview ? "on" : "off")) return -1;
    return azconf_set(settings, "vocal-indicator", wave || preview ? "on" : "off");
}
static int remove_setting(struct azconf *settings, const char *key) {
    return azconf_get(settings, key) ? azconf_unset(settings, key) : 0;
}
static int migrate_extraction_toggle(struct azconf *settings, const char *key) {
    const char *previous = azconf_get(settings, "track-analysis");
    if (!previous || azconf_get(settings, key)) return 0;
    return azconf_set(settings, key, previous);
}
static int migrate_extraction(struct azconf *settings) {
    if (migrate_extraction_toggle(settings, "phrase-extraction")) return -1;
    if (migrate_extraction_toggle(settings, "vocal-extraction")) return -1;
    return remove_setting(settings, "track-analysis");
}
static int retire_vocal_lines(struct azconf *settings) {
    if (remove_setting(settings, "vocal-lines-waveform")) return -1;
    if (remove_setting(settings, "vocal-lines-phrases")) return -1;
    return remove_setting(settings, "vocal-lines-merge-gap");
}
int analysis_settings_migrate(struct azconf *settings) {
    if (migrate_analysis_toggle(settings)) return -1;
    if (migrate_extraction(settings)) return -1;
    if (migrate_vocal_lines(settings)) return -1;
    return retire_vocal_lines(settings);
}

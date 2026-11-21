/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_settings.h"
static int migrate_analysis_toggle(struct azconf *settings) {
    const char *previous = azconf_get(settings, "track-analysis:enabled");
    if (!previous) return 0;
    if (!azconf_get(settings, "track-analysis") && azconf_set(settings, "track-analysis", previous)) return -1;
    return azconf_unset(settings, "track-analysis:enabled");
}
/* Retain old keys for row_lookup compatibility, but migrate their effective
 * display preferences once. Explicit settings for the new renderer win. */
static int migrate_vocal_target(struct azconf *settings, const char *old_key, const char *new_key) {
    if (!azconf_get(settings, "vocal-indicator") || azconf_get(settings, new_key)) return 0;
    int enabled = azconf_get_bool(settings, "vocal-indicator", 0) &&
                  azconf_get_bool(settings, old_key, 1);
    return azconf_set(settings, new_key, enabled ? "on" : "off");
}
int analysis_settings_migrate(struct azconf *settings) {
    if (migrate_analysis_toggle(settings)) return -1;
    if (migrate_vocal_target(settings, "vocal-indicator-waveform", "vocal-lines-waveform")) return -1;
    return migrate_vocal_target(settings, "vocal-indicator-preview", "vocal-lines-phrases");
}

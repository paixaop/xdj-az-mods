/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_settings.h"
int analysis_settings_migrate(struct azconf *settings) {
    const char *previous = azconf_get(settings, "track-analysis:enabled");
    if (!previous) return 0;
    if (!azconf_get(settings, "track-analysis") && azconf_set(settings, "track-analysis", previous)) return -1;
    return azconf_unset(settings, "track-analysis:enabled");
}

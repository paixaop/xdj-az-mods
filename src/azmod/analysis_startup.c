/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_host.h"
/* Host lifecycle: core services start even when the community loader has no
 * enabled packages. Source observation and automatic analysis are core work. */
#ifndef SDK_PROC_IMPL
static int core_analysis_assets(char *assets, size_t size) {
    char mods[1024];
    if (!usbmods_find(mods, sizeof mods)) return AZ_NOT_FOUND;
    int n = snprintf(assets, size, "%s/analysis/assets", mods);
    return n < 0 || (size_t) n >= size ? AZ_LIMIT : AZ_OK;
}
static void core_analysis_start(void) {
    char assets[1200];
    int rc = core_analysis_assets(assets, sizeof assets);
    if (rc == AZ_OK) rc = analysis_host_start(assets, az_on(AZF_TRACK_ANALYSIS));
    if (rc != AZ_OK) {
        dbg("analysis: core startup failed (%d)\n", rc);
        return;
    }
    azmod_analysis_enable = analysis_host_enable;
    rc = sdk_status_analysis_bind(analysis_runtime_load);
    if (rc == AZ_OK) azmod_analysis_status = analysis_runtime_deck_status;
    else dbg("analysis: automatic load observation unavailable (%d)\n", rc);
}
#endif
static void core_services_init(void) {
    sdk_status_start();
    cm_services_start();
#ifndef SDK_PROC_IMPL
    core_analysis_start();
#endif
}

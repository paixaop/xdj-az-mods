/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_host.h"
#include "vocal_lines.h"
/* Host lifecycle: core services start even when the community loader has no
 * enabled packages. Source observation and automatic analysis are core work. */
#ifndef SDK_PROC_IMPL
static int core_analysis_assets(char *assets, size_t size) {
    char mods[1024];
    if (!usbmods_find(mods, sizeof mods)) return AZ_NOT_FOUND;
    int n = snprintf(assets, size, "%s/analysis/assets", mods);
    return n < 0 || (size_t) n >= size ? AZ_LIMIT : AZ_OK;
}
static void core_analysis_error(const char *stage, int result) {
    char text[128];
    snprintf(text, sizeof text, "Analysis %s failed (%d)", stage, result);
    analysis_diagnostic(text);
}
static void core_analysis_load(unsigned deck, uint64_t order, uint64_t epoch, int loaded,
                               const az_track_ref *track) {
    analysis_runtime_load(deck, order, epoch, loaded, track);
    vocal_lines_load(deck, order, epoch, loaded);
}
static void core_vocal_lines_start(void) {
    int rc = vocal_lines_start();
    if (rc != AZ_OK) {
        core_analysis_error("vocal lines startup", rc);
        return;
    }
    azmod_vocal_lines_config = vocal_lines_config;
    azf_vocal_lines_changed(0, NULL);
}
static void core_analysis_start(void) {
    char assets[1200];
    int rc = core_analysis_assets(assets, sizeof assets);
    if (rc == AZ_OK) rc = analysis_host_start(assets, az_on(AZF_TRACK_ANALYSIS));
    if (rc != AZ_OK) {
        core_analysis_error("core startup", rc);
        return;
    }
    azmod_analysis_enable = analysis_host_enable;
    rc = sdk_status_analysis_bind(core_analysis_load);
    if (rc == AZ_OK) {
        azmod_analysis_status = analysis_runtime_deck_status;
        core_vocal_lines_start();
    } else core_analysis_error("automatic load observation", rc);
}
#endif
static void core_services_init(void) {
    sdk_status_start();
    cm_services_start();
#ifndef SDK_PROC_IMPL
    core_analysis_start();
#endif
}

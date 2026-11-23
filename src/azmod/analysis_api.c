/* SPDX-License-Identifier: GPL-3.0-only */
/* Authenticated SDK boundary, included after the source services. Built-in
 * analysis enters analysis_runtime directly and never calls this table. */
#include "analysis_runtime.h"
static az_analysis_owner sdk_analysis_caller(const char *id, uintptr_t caller) {
    if (g_sdk_on_tap_thread) return (az_analysis_owner){0};
    int slot = st_mod_owner(id, caller, "analysis");
    if (slot < 0) return (az_analysis_owner){0};
    return g_cm_on_ui ? analysis_owner_get((unsigned)slot) : analysis_owner_for_caller((unsigned)slot);
}
static int sdk_analysis_request(const char *id, const az_analysis_request_v1 *request, uint64_t *out) {
    return analysis_runtime_request(sdk_analysis_caller(id, CM_RA()), request, out);
}
static int sdk_analysis_status(const char *id, uint64_t request, az_analysis_status_v1 *out) {
    return analysis_runtime_status(sdk_analysis_caller(id, CM_RA()), request, out);
}
static int sdk_analysis_cancel_request(const char *id, uint64_t request) {
    return analysis_runtime_cancel(sdk_analysis_caller(id, CM_RA()), request);
}
static int sdk_analysis_release_request(const char *id, uint64_t request) {
    return analysis_runtime_release(sdk_analysis_caller(id, CM_RA()), request);
}
static int sdk_analysis_result(const char *id, uint64_t request, az_analysis_result_v1 *out,
                                az_analysis_phrase_v1 *phrases, uint32_t pc,
                                az_vocal_frame_v1 *vocals, uint32_t vc) {
    return analysis_runtime_result(sdk_analysis_caller(id, CM_RA()), request, out, phrases, pc, vocals, vc);
}
static const az_analysis_v1 sdk_analysis = {
    .abi = AZ_ANALYSIS_ABI, .size = sizeof sdk_analysis,
    .capabilities = analysis_runtime_capabilities,
    .request = sdk_analysis_request, .status = sdk_analysis_status,
    .cancel = sdk_analysis_cancel_request, .release = sdk_analysis_release_request,
    .result = sdk_analysis_result,
    .vocal_intervals = sdk_vocal_intervals,
    .vocal_intervals_loaded = sdk_vocal_intervals_loaded
};
static const az_analysis_v1 *sdk_analysis_service(uint32_t abi) {
    return abi == AZ_ANALYSIS_ABI ? &sdk_analysis : NULL;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_track.c. The one core-owned command store is shared by every
 * authenticated mod. The EXIT/RELOOP bridge queues copied scalars for the native
 * owner-thread adapter; unsupported operations create no retained request. */
#include "sdk_command_service.h"
#ifdef CM_MAX
static int sdk_command_async_authorized(cm_mod *mod, const az_command_request *request,
                                        az_command_completion_fn callback, az_command_result *output) {
    return g_cm_on_ui && mod && callback && scmd_output_ok(output) && scmd_request_valid(request) &&
           cm_command_code(mod, (uintptr_t) callback);
}
#endif

static int sdk_command_poll(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_poll");
    return owner < 0 ? AZ_INVALID : sdk_command_read(owner, request_id, out);
}
static int sdk_command_cancel(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_cancel");
    if (owner < 0) return AZ_INVALID;
    return sdk_exit_cancel_request(owner, request_id, out);
}
static int sdk_command_release(const char *id, uint64_t request_id) {
    int owner = st_mod_owner(id, CM_RA(), "command_release");
    return owner < 0 ? AZ_INVALID : sdk_command_drop(owner, request_id);
}
static __attribute__((unused)) int sdk_command_submit_async(const char *id, const az_command_request *r,
                                                            az_command_completion_fn callback, void *user,
                                                            az_command_result *out) {
#ifdef CM_MAX
    cm_mod *mod = cm_self(id, CM_RA(), "command_submit_async");
    if (!sdk_command_async_authorized(mod, r, callback, out)) return AZ_INVALID;
    int owner = (int) (mod - g_cm);
    if (mod->state != AZCM_LOADED && (!g_sdk_in_init || g_cm_loading != owner)) return AZ_INVALID;
    return sdk_exit_submit_async(owner, r, callback, user, out);
#else
    (void) id;
    (void) r;
    (void) callback;
    (void) user;
    (void) out;
    return AZ_INVALID;
#endif
}
static __attribute__((unused)) int sdk_command_acknowledge(const char *id, uint64_t request_id) {
#ifdef CM_MAX
    int owner = st_mod_owner(id, CM_RA(), "command_acknowledge");
    if (!g_cm_on_ui || owner < 0 ||
        (g_cm[owner].state != AZCM_LOADED && (!g_sdk_in_init || g_cm_loading != owner))) return AZ_INVALID;
    int rc = sdk_command_ack(owner, request_id);
    return rc;
#else
    (void) id;
    (void) request_id;
    return AZ_INVALID;
#endif
}

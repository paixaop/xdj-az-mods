/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_track.c. The one core-owned command store is shared by every
 * authenticated mod. The EXIT/RELOOP bridge queues copied scalars for the native
 * owner-thread adapter; unsupported operations create no retained request. */
#include "sdk_command_engine.h"
static struct scmd_engine g_sdk_commands = SCMD_ENGINE_INIT;
static int sdk_command_has_pending_notification(void);
#include "sdk_exit_bridge.c"

static int sdk_command_has_pending_notification(void) {
    int pending = 0;
    pthread_mutex_lock(&g_sdk_commands.mu);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &g_sdk_commands.records[i];
        if (r->asynchronous && r->phase == SCMD_FINAL && r->callback && !r->retired &&
            !r->terminal_delivered && scmd_owner_deliverable(&g_sdk_commands, r->owner, r->owner_lifetime)) {
            pending = 1;
            break;
        }
    }
    if (!pending) {
        for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
            struct scmd_pending_delivery *p = &g_sdk_commands.pending[i];
            if (p->occupied && !p->released_delivered &&
                scmd_owner_deliverable(&g_sdk_commands, p->owner, p->owner_lifetime)) {
                pending = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_sdk_commands.mu);
    return pending;
}

static int sdk_command_poll(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_poll");
    return owner < 0 ? AZ_INVALID : scmd_poll(&g_sdk_commands, owner, request_id, out);
}
static int sdk_command_cancel(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_cancel");
    if (owner < 0) return AZ_INVALID;
    return sdk_exit_cancel_request(owner, request_id, out);
}
static int sdk_command_release(const char *id, uint64_t request_id) {
    int owner = st_mod_owner(id, CM_RA(), "command_release");
    return owner < 0 ? AZ_INVALID : scmd_release(&g_sdk_commands, owner, request_id);
}
static __attribute__((unused)) int sdk_command_submit_async(const char *id, const az_command_request *r,
                                                            az_command_completion_fn callback, void *user,
                                                            az_command_result *out) {
#ifdef CM_MAX
    cm_mod *mod = cm_self(id, CM_RA(), "command_submit_async");
    if (!g_cm_on_ui || !mod || !callback || !scmd_output_ok(out) || !scmd_request_valid(r) ||
        !cm_command_code(mod, (uintptr_t) callback)) return AZ_INVALID;
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
    int rc = scmd_acknowledge(&g_sdk_commands, owner, request_id);
    if (rc == AZ_OK) sdk_command_wake();
    return rc;
#else
    (void) id;
    (void) request_id;
    return AZ_INVALID;
#endif
}

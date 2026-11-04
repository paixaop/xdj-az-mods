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
        if (r->asynchronous && r->phase == SCMD_FINAL && r->callback &&
            (!r->terminal_delivered || (r->acknowledged && !r->awaiting && !r->released_delivered))) {
            pending = 1;
            break;
        }
    }
    if (!pending) {
        for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
            struct scmd_pending_delivery *p = &g_sdk_commands.pending[i];
            if (p->occupied && !p->released_delivered) {
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
#ifndef CM_MAX
    if (owner >= 0) sdk_command_drive();
#endif
    return owner < 0 ? AZ_INVALID : scmd_poll(&g_sdk_commands, owner, request_id, out);
}
static int sdk_command_cancel(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_cancel");
    if (owner < 0) return AZ_INVALID;
    int rc = scmd_cancel(&g_sdk_commands, owner, request_id, out);
    if (rc != AZ_PENDING) return rc;
    sdk_exit_cancel_request(owner, request_id);
#ifndef CM_MAX
    sdk_command_drive();
#endif
    rc = scmd_poll(&g_sdk_commands, owner, request_id, out);
    if (rc != AZ_OK) return rc;
    if (out->outcome == AZ_COMMAND_CANCELLED) return AZ_OK;
    return out->outcome == AZ_COMMAND_ACCEPTED ? AZ_PENDING : AZ_STALE;
}
static int sdk_command_release(const char *id, uint64_t request_id) {
    int owner = st_mod_owner(id, CM_RA(), "command_release");
#ifndef CM_MAX
    if (owner >= 0) sdk_command_drive();
#endif
    return owner < 0 ? AZ_INVALID : scmd_release(&g_sdk_commands, owner, request_id);
}
static __attribute__((unused)) int sdk_command_submit_async(const char *id, const az_command_request *r,
                                                            az_command_completion_fn callback, void *user,
                                                            az_command_result *out) {
#ifdef CM_MAX
    cm_mod *mod = cm_self(id, CM_RA(), "command_submit_async");
    if (!mod || !callback || (!g_sdk_in_init && !g_cm_on_ui) || !scmd_output_ok(out) || !scmd_request_valid(r)) return AZ_INVALID;
    if (!mod->hi || (uintptr_t) callback < mod->lo || (uintptr_t) callback >= mod->hi) return AZ_INVALID;
    int owner = (int) (mod - g_cm);
    if (!(r->flags & AZ_COMMAND_NATIVE_LOAD)) return scmd_refuse(out, r, AZ_UNSUPPORTED, scmd_now(&g_sdk_commands));
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
    if (owner < 0 || (!g_sdk_in_init && !g_cm_on_ui)) return AZ_INVALID;
    return scmd_acknowledge(&g_sdk_commands, owner, request_id);
#else
    (void) id;
    (void) request_id;
    return AZ_INVALID;
#endif
}

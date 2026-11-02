/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_track.c. The one core-owned command store is shared by every
 * authenticated mod. The EXIT/RELOOP bridge queues copied scalars for the native
 * owner-thread adapter; unsupported operations create no retained request. */
#include "sdk_command_engine.h"
static struct scmd_engine g_sdk_commands = SCMD_ENGINE_INIT;
#include "sdk_exit_bridge.c"

static int sdk_command_poll(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_poll");
    if (owner >= 0) sdk_command_drive();
    return owner < 0 ? AZ_INVALID : scmd_poll(&g_sdk_commands, owner, request_id, out);
}
static int sdk_command_cancel(const char *id, uint64_t request_id, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_cancel");
    if (owner < 0) return AZ_INVALID;
    int rc = scmd_cancel(&g_sdk_commands, owner, request_id, out);
    if (rc != AZ_PENDING) return rc;
    sdk_exit_cancel_request(owner, request_id);
    sdk_command_drive();
    rc = scmd_poll(&g_sdk_commands, owner, request_id, out);
    if (rc != AZ_OK) return rc;
    if (out->outcome == AZ_COMMAND_CANCELLED) return AZ_OK;
    return out->outcome == AZ_COMMAND_ACCEPTED ? AZ_PENDING : AZ_STALE;
}
static int sdk_command_release(const char *id, uint64_t request_id) {
    int owner = st_mod_owner(id, CM_RA(), "command_release");
    if (owner >= 0) sdk_command_drive();
    return owner < 0 ? AZ_INVALID : scmd_release(&g_sdk_commands, owner, request_id);
}

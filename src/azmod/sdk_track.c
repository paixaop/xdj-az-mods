/* SPDX-License-Identifier: GPL-3.0-only */
/* Authenticated SDK entry points; stored-data state belongs to track_service.c. */
#include "sdk_data_engine.h"
#include "track_service.h"
#include "track_storage.h"
#ifndef ST_MOUNTINFO
#define ST_MOUNTINFO "/proc/self/mountinfo"
#endif
const char *az_storage_mountinfo(void) { return ST_MOUNTINFO; }
int az_storage_thread_create(pthread_t *thread, const pthread_attr_t *attributes,
                             void *(*entry)(void *), void *argument) {
    return az_thread_create(thread, attributes, entry, argument);
}
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out);
static uint32_t sd_live_fields(void);
#include "sdk_command_service.h"
static void sdk_data_owner_init(int owner) { track_service_owner_init(owner); }
static void sdk_data_retire_owner(int owner) {
    track_service_owner_retire(owner);
    sdk_command_retire_owner(owner);
}
static int st_mod_owner(const char *id, uintptr_t caller, const char *service) {
    cm_mod *c = cm_self(id, caller, service);
    return c && !g_cm_in_frame ? (int) (c - g_cm) : -1;
}
static int sdk_data_capabilities(az_data_capabilities *out) {
    int rc = track_service_capabilities(out);
    if (!rc && track_service_enabled()) {
        out->live_fields = sd_live_fields();
        out->commands = sdk_command_capabilities();
    }
    return rc;
}
static int sdk_track_resolve(const char *id, int stick, uint32_t cid, az_track_ref *out) {
    if (st_mod_owner(id, CM_RA(), "track_resolve") < 0 || g_cm_on_ui) return AZ_INVALID;
    return track_service_resolve(stick, cid, out);
}
static int sdk_track_request(const char *id, const az_track_ref *ref, uint32_t mask, uint64_t *ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_request");
    if (owner < 0) return AZ_INVALID;
    return track_service_request(owner, ref, mask, ticket);
}
static int sdk_track_acquire(const char *id, uint64_t ticket, const az_track_snapshot **out) {
    int owner = st_mod_owner(id, CM_RA(), "track_acquire");
    return owner < 0 ? AZ_INVALID : track_service_acquire(owner, ticket, out);
}
static int sdk_waveform_request(const char *id, const az_track_ref *ref, const az_wave_query *query, uint64_t *ticket) {
    int owner = st_mod_owner(id, CM_RA(), "waveform_request");
    if (owner < 0 || !query) return AZ_INVALID;
    return track_service_wave_request(owner, ref, query, ticket);
}
static int sdk_waveform_acquire(const char *id, uint64_t ticket, const az_wave_snapshot **out) {
    int owner = st_mod_owner(id, CM_RA(), "waveform_acquire");
    return owner < 0 ? AZ_INVALID : track_service_wave_acquire(owner, ticket, out);
}
static int sdk_track_release(const char *id, uint64_t ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_release");
    return owner < 0 ? AZ_INVALID : track_service_release(owner, ticket, 0);
}
static int sdk_track_cancel(const char *id, uint64_t ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_cancel");
    return owner < 0 ? AZ_INVALID : track_service_release(owner, ticket, 1);
}
static int sdk_data_event(const char *id, uint64_t after, az_data_change *out) {
    int owner = st_mod_owner(id, CM_RA(), "data_event");
    return owner < 0 ? AZ_INVALID : track_service_event(owner, after, out);
}
/* Only the opt-in native transient commands are admitted. Refusal performs no action. */
#include "sdk_command.c"
static int sdk_command_submit(const char *id, const az_command_request *r, az_command_result *out) {
    int owner = st_mod_owner(id, CM_RA(), "command_submit");
    if (owner < 0 || !scmd_output_ok(out) || !scmd_request_valid(r)) return AZ_INVALID;
    return sdk_exit_submit(owner, r, out);
}

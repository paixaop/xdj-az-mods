#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
/* SPDX-License-Identifier: GPL-3.0-only */
/* Real provider engines and callback dispatcher with a synthetic UI/mod host. */
#define _POSIX_C_SOURCE 200809L
#include "sdk_data_engine.h"
#include "sdk_command_engine.h"
#include <assert.h>
#include <stdio.h>
#define CM_MAX 3
#define AZCM_LOADED 1
typedef struct {
    uintptr_t lo, hi;
    int state;
} cm_mod;
static cm_mod g_cm[CM_MAX];
static int g_cm_n = CM_MAX, g_cm_loading = -1, g_sdk_in_init, g_cm_in_frame, g_cm_on_ui;
#define CM_RA() 1
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *service) {
    (void) service;
    if (caller != 1 || !id) return NULL;
    if (!strcmp(id, "one")) return &g_cm[0];
    if (!strcmp(id, "two")) return &g_cm[1];
    if (!strcmp(id, "failed")) return &g_cm[2];
    return NULL;
}
static struct st_engine g_st = ST_ENGINE_INIT;
static int st_mod_owner(const char *id, uintptr_t caller, const char *service) {
    cm_mod *c = cm_self(id, caller, service);
    return c && !g_cm_in_frame ? (int) (c - g_cm) : -1;
}
static int cm_command_code(cm_mod *mod, uintptr_t address) {
    return mod->lo <= address && address < mod->hi;
}
#include "sdk_command_service.c"
#include "sdk_command.c"
/* Callback routing fixture: the cache implementation is exercised through the
 * same production engine primitives; provider service tests link its actual owner. */
static int track_service_event_head(uint64_t *sequence) {
    if (pthread_mutex_trylock(&g_st.mu)) return AZ_BUSY;
    int rc = g_st.events_exhausted ? AZ_LIMIT : AZ_OK;
    if (!rc) *sequence = g_st.sequence;
    pthread_mutex_unlock(&g_st.mu);
    return rc;
}
static int track_service_owner_retired(int owner) { return st_owner_retired(&g_st, owner); }
static int track_service_event(int owner, uint64_t after, az_data_change *out) {
    return st_event_read(&g_st, owner, after, out);
}
#include "sdk_data_callbacks.c"
#include "analysis_source.h"
#include "analysis_callbacks.c"
static int counts[CM_MAX], commands[CM_MAX], saw_missed, last_status;
static uint64_t last_ticket;
static void data(const az_data_change *event, void *user) {
    int owner = (int) (uintptr_t) user;
    assert(!pthread_mutex_trylock(&g_st.mu));
    pthread_mutex_unlock(&g_st.mu);
    ++counts[owner];
    saw_missed |= event->missed;
    last_status = event->status;
    last_ticket = event->ticket;
    /* Reentry must be safe: even cancellation/release can be called here. */
    if (event->ticket && event->status == AZ_CANCELLED)
        assert(st_release(&g_st, owner, event->ticket, 0) == AZ_OK);
}
static void complete(const az_command_result *result, void *user) {
    int owner = (int) (uintptr_t) user;
    assert(!pthread_mutex_trylock(&g_sdk_commands.mu));
    pthread_mutex_unlock(&g_sdk_commands.mu);
    assert(result->outcome != AZ_COMMAND_ACCEPTED);
    ++commands[owner];
    assert(scmd_release(&g_sdk_commands, owner, result->request_id) == AZ_OK);
}
static void init(int owner) {
    g_cm_loading = owner;
    g_sdk_in_init = 1;
    g_cm[owner].lo = 1;
    g_cm[owner].hi = UINTPTR_MAX;
}
static int load(const az_track_ref *ref, uint32_t mask, struct st_data *out, void *ctx) {
    (void) mask;
    (void) out;
    (void) ctx;
    return ref->track_id == 99 ? AZ_FAILED : AZ_OK;
}
static int fresh(const az_track_ref *ref, const struct st_data *data, void *ctx) {
    (void) ref;
    (void) data;
    return *(int *) ctx;
}
static az_track_ref ref(unsigned id) {
    return (az_track_ref) {sizeof(az_track_ref), 1, 1, 1, 1, id, 0};
}
static uint64_t request(int owner, unsigned id, int wave) {
    az_track_ref r = ref(id);
    az_wave_query q = {sizeof q, 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    uint64_t ticket;
    assert(st_request_with_wave(&g_st, owner, &r, wave ? ST_DETAIL : AZ_DATA_METADATA,
                                wave ? &q : NULL, &ticket) == AZ_OK);
    return ticket;
}
static unsigned analysis_counts[3], analysis_gap;
static int retire_in_callback;
static void analysis_complete(const az_analysis_complete_v1 *event, void *user) {
    unsigned slot = (unsigned) (uintptr_t) user;
    assert(event->size == sizeof *event && event->version == 1 && event->request_id);
    assert(event->available_components == (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS));
    assert(event->generated_components == AZ_ANALYSIS_VOCALS);
    analysis_counts[slot]++;
    analysis_gap |= event->missed;
    /* Listener reentry is outside journal locks. */
    struct analysis_listener probe = {0};
    assert(analysis_events_listen(&probe, analysis_complete, NULL) == AZ_OK);
    if (retire_in_callback) analysis_owner_retire(slot);
}
static void analysis_callbacks_contract(void) {
    assert(sdk_on_analysis_complete("one", analysis_complete, NULL) == AZ_INVALID);
    for (unsigned slot = 0; slot < 3; ++slot) {
        init((int) slot);
        analysis_owner_begin(slot);
        const char *id = slot == 0 ? "one" : slot == 1 ? "two"
                                                       : "failed";
        assert(sdk_on_analysis_complete(id, NULL, NULL) == AZ_INVALID);
        assert(sdk_on_analysis_complete(id, analysis_complete, (void *) (uintptr_t) slot) == AZ_OK);
        assert(sdk_on_analysis_complete(id, analysis_complete, NULL) == AZ_LIMIT);
    }
    g_sdk_in_init = 0;
    g_cm_loading = -1;
    g_cm[0].state = g_cm[1].state = AZCM_LOADED;
    g_cm[2].state = 0;
    analysis_owner_retire(2);
    az_analysis_complete_v1 event = {.request_id = 42, .load_epoch = 7, .generated_components = AZ_ANALYSIS_VOCALS, .available_components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    analysis_events_publish(&event);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 1 && analysis_counts[1] == 1 && !analysis_counts[2]);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 1);
    for (unsigned i = 0; i < 80; ++i) analysis_events_publish(&event);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 17 && analysis_counts[1] == 17 && analysis_gap);
    retire_in_callback = 1;
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 18 && analysis_counts[1] == 18);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 18);
    analysis_owner_begin(0);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 18);
    init(0);
    assert(sdk_on_analysis_complete("one", analysis_complete, NULL) == AZ_OK);
    g_sdk_in_init = 0;
    g_cm_loading = -1;
    g_cm[0].state = AZCM_LOADED;
    retire_in_callback = 0;
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 18);
    analysis_events_publish(&event);
    sdk_analysis_callbacks_tick();
    assert(analysis_counts[0] == 19 && analysis_counts[1] == 18);
    puts("analysis callbacks: broadcast, ordering, bounded gap, reentry and lifetime passed");
}
int main(void) {
    assert(sdk_on_data_event("unknown", data, NULL) == AZ_INVALID);
    assert(sdk_on_data_event("one", data, NULL) == AZ_INVALID);
    init(0);
    assert(sdk_on_data_event("one", NULL, NULL) == AZ_INVALID);
    g_cm[0].hi = 1;
    assert(sdk_on_data_event("one", data, NULL) == AZ_INVALID);
    init(0);
    g_cm_in_frame = 1;
    assert(sdk_on_command_complete("one", complete, NULL) == AZ_INVALID);
    g_cm_in_frame = 0;
    assert(sdk_on_data_event("two", data, NULL) == AZ_INVALID);
    pthread_mutex_lock(&g_st.mu);
    assert(sdk_on_data_event("one", data, NULL) == AZ_BUSY);
    pthread_mutex_unlock(&g_st.mu);
    assert(sdk_on_data_event("one", data, NULL) == AZ_OK);
    assert(sdk_on_data_event("one", data, NULL) == AZ_LIMIT);
    assert(sdk_on_command_complete("one", complete, NULL) == AZ_OK);
    assert(sdk_on_command_complete("one", complete, NULL) == AZ_LIMIT);
    init(1);
    assert(sdk_on_data_event("two", data, (void *) 1) == AZ_OK);
    assert(sdk_on_command_complete("two", complete, (void *) 1) == AZ_OK);
    init(2);
    assert(sdk_on_data_event("failed", data, (void *) 2) == AZ_OK);
    assert(sdk_on_command_complete("failed", complete, (void *) 2) == AZ_OK);
    g_cm[0].state = g_cm[1].state = AZCM_LOADED;
    g_sdk_in_init = 0;
    g_cm_loading = -1;
    uint64_t a = request(0, 10, 0), b = request(1, 20, 1), failed = request(2, 30, 0);
    sdk_data_callbacks_tick();
    assert(!counts[0] && !counts[1] && !counts[2]);
    st_work(&g_st, load, NULL, NULL);
    pthread_mutex_lock(&g_st.mu);
    sdk_data_callbacks_tick(); /* contention defers delivery without consuming it */
    pthread_mutex_unlock(&g_st.mu);
    assert(!counts[0]);
    sdk_data_callbacks_tick();
    assert(counts[0] == 1 && counts[1] == 1 && !counts[2]);
    assert(last_ticket == b);
    const az_track_snapshot *track;
    const az_wave_snapshot *wave;
    assert(st_acquire(&g_st, 0, a, &track) == AZ_OK);
    assert(st_wave_acquire(&g_st, 1, b, &wave) == AZ_OK);
    az_data_change polled = {.size = sizeof polled};
    assert(st_event_read(&g_st, 0, 0, &polled) == AZ_OK && polled.ticket == a);
    sdk_data_callbacks_tick();
    assert(counts[0] == 1);
    assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    assert(st_release(&g_st, 1, b, 0) == AZ_OK);
    assert(st_release(&g_st, 2, failed, 0) == AZ_OK);
    a = request(0, 99, 0);
    st_work(&g_st, load, NULL, NULL);
    sdk_data_callbacks_tick();
    assert(last_status == AZ_FAILED && last_ticket == a);
    assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    a = request(0, 40, 1);
    assert(st_release(&g_st, 0, a, 1) == AZ_OK);
    sdk_data_callbacks_tick();
    assert(last_status == AZ_CANCELLED);
    /* A cache hit also publishes a completion, without another worker pass. */
    a = request(0, 10, 0);
    sdk_data_callbacks_tick();
    assert(last_ticket == a && last_status == AZ_OK);
    assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    for (int i = 0; i < 80; ++i) {
        a = request(0, 10, 0);
        assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    }
    int before = counts[0];
    sdk_data_callbacks_tick();
    assert(counts[0] == before + 16 && saw_missed);
    for (int i = 0; i < 4; ++i) sdk_data_callbacks_tick();
    a = request(0, 99, 1);
    st_work(&g_st, load, NULL, NULL);
    sdk_data_callbacks_tick();
    assert(last_ticket == a && last_status == AZ_FAILED);
    assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    a = request(0, 50, 0);
    b = request(0, 50, 1);
    int current = 1;
    st_work(&g_st, load, fresh, &current);
    sdk_data_callbacks_tick();
    current = 0;
    st_work(&g_st, load, fresh, &current);
    before = counts[0];
    sdk_data_callbacks_tick();
    assert(counts[0] == before + 2 && last_status == AZ_STALE);
    assert(st_release(&g_st, 0, a, 0) == AZ_OK);
    assert(st_release(&g_st, 0, b, 0) == AZ_OK);
    /* Real retained records: no dependency on command journal capacity/cursors. */
    g_sdk_commands.records[0] = (struct scmd_record) {.owner = 0, .request.request_id = 1, .token = 1, .phase = SCMD_FINAL, .result = {sizeof(az_command_result), 1, AZ_COMMAND_APPLIED, AZ_OK, 1, 1, 1}};
    g_sdk_commands.records[1] = (struct scmd_record) {.owner = 1, .request.request_id = 1, .token = 2, .phase = SCMD_FINAL, .awaiting = 1, .result = {sizeof(az_command_result), 1, AZ_COMMAND_UNCERTAIN, AZ_STALE, 1, 0, 1}};
    g_sdk_commands.records[2] = (struct scmd_record) {.owner = 2, .request.request_id = 1, .token = 3, .phase = SCMD_FINAL, .result = {sizeof(az_command_result), 1, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 1, 0, 1}};
    pthread_mutex_lock(&g_sdk_commands.mu);
    sdk_data_callbacks_tick();
    assert(!commands[0]);
    pthread_mutex_unlock(&g_sdk_commands.mu);
    sdk_data_callbacks_tick();
    assert(commands[0] == 1 && !commands[1] && !commands[2]);
    g_sdk_commands.records[1].awaiting = 0;
    g_sdk_commands.records[1].pinned = 1;
    sdk_data_callbacks_tick();
    assert(!commands[1]);
    g_sdk_commands.records[1].pinned = 0;
    sdk_data_callbacks_tick();
    assert(commands[1] == 1);
    sdk_data_callbacks_tick();
    assert(commands[0] == 1 && commands[1] == 1);
    g_sdk_commands.records[0] = (struct scmd_record) {.owner = 0, .request.request_id = 2, .token = 4, .phase = SCMD_FINAL, .result = {sizeof(az_command_result), 1, AZ_COMMAND_REJECTED, AZ_FAILED, 2, 0, 1}};
    sdk_data_callbacks_tick();
    assert(commands[0] == 2);
    st_retire_owner(&g_st, 0);
    g_sdk_commands.records[0] = (struct scmd_record) {.owner = 0, .token = 5, .phase = SCMD_FINAL};
    sdk_data_callbacks_tick();
    assert(commands[0] == 2);
    st_reopen_owner(&g_st, 0);
    g_sdk_commands.records[0].retired = 1;
    sdk_data_callbacks_tick();
    assert(commands[0] == 2);
    g_sdk_commands.records[0].retired = 0;
    ++g_sdk_commands.owner_lifetime[0];
    sdk_data_callbacks_tick();
    assert(commands[0] == 2 && "a restarted mod must not receive an old command completion");
    analysis_callbacks_contract();
    st_destroy(&g_st);
    puts("data callbacks: passed");
    return 0;
}

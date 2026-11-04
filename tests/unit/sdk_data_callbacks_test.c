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
static int g_cm_n = CM_MAX, g_cm_loading = -1, g_sdk_in_init, g_cm_in_frame;
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
#include "sdk_command.c"
#include "sdk_data_callbacks.c"
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
static struct azexit_request native_request;
static int native_ready, native_submits;
static unsigned native_mask(void) { return 1; }
static int native_target(unsigned deck, uint64_t *generation) {
    assert(deck == 0);
    *generation = 7;
    return AZ_OK;
}
static int native_submit(const struct azexit_request *request) {
    assert(!native_request.token);
    native_request = *request;
    ++native_submits;
    return AZ_PENDING;
}
static int native_poll(unsigned deck, struct azexit_result *result) {
    assert(deck == 0);
    if (!native_ready) return AZ_PENDING;
    *result = (struct azexit_result) {native_request.token, 7, 1, AZ_COMMAND_APPLIED, AZ_OK};
    return AZ_OK;
}
static int native_release(unsigned deck, uint64_t token) {
    assert(deck == 0 && token == native_request.token);
    native_request.token = 0;
    return AZ_OK;
}
static int native_cancel(unsigned deck, uint64_t token) {
    (void) deck;
    (void) token;
    return AZ_PENDING;
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
    /* Drive the actual public command bridge to completion without command_poll.
     * Only the native mailbox endpoint is synthetic; no hardware is contacted. */
    static const struct azexit_service api = {.size = AZEXIT_SERVICE_V1_SIZE, .version = 1, .mask = native_mask, .target = native_target, .submit = native_submit, .cancel = native_cancel, .poll = native_poll, .release = native_release};
    atomic_store(&g_sdk_exit_service, &api);
    g_sdk_commands.next_token = 5;
    az_command_request cmd = {.size = sizeof cmd, .version = 1, .operation = AZ_CMD_LOOP_EXIT, .deck = 0, .request_id = 2, .flags = AZ_COMMAND_NATIVE_LOAD, .expected_load_generation = 7, .deadline_ns = UINT64_MAX};
    az_command_result result = {.size = sizeof result};
    assert(sdk_exit_submit(1, &cmd, &result) == AZ_OK && result.outcome == AZ_COMMAND_ACCEPTED);
    assert(native_submits == 1);
    sdk_data_callbacks_tick();
    assert(commands[1] == 1);
    native_ready = 1;
    sdk_data_callbacks_tick();
    assert(commands[1] == 2 && !native_request.token);
    sdk_data_callbacks_tick();
    assert(commands[1] == 2 && native_submits == 1);
    st_destroy(&g_st);
    puts("data callbacks: passed");
    return 0;
}

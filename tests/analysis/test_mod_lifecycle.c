/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact loader entry/exit bodies, actual owner/runtime/engine, explicit host/native adapters. */
#define main existing_runtime_fixture_main
#define analysis_source_release lifecycle_source_release_real
#include "test_runtime.c"
#undef analysis_source_release
#undef main
#include "../../src/analysis/analysis.h"
#include <stdarg.h>
#include <unistd.h>

static pthread_t loader_thread;
static atomic_int engine_active, engine_pause_once, engine_paused, engine_resume, result_frees;
struct engine_callback {
    int (*cancelled)(void *);
    void *user;
    pthread_t coordinator;
};
static int lifecycle_cancelled(void *opaque) {
    struct engine_callback *callback = opaque;
    if (!pthread_equal(pthread_self(), callback->coordinator) && atomic_exchange(&engine_pause_once, 0)) {
        atomic_store(&engine_paused, 1);
        while (!atomic_load(&engine_resume)) pause_ms();
    }
    return callback->cancelled(callback->user);
}
int lifecycle_analyze(const ta_request *request, ta_result **result, char *error, size_t size) {
    struct engine_callback callback = {request->cancelled, request->user, pthread_self()};
    ta_request actual = *request;
    actual.cancelled = lifecycle_cancelled;
    actual.user = &callback;
    atomic_fetch_add(&engine_active, 1);
    int rc = ta_analyze(&actual, result, error, size); /* production joins all children before return */
    atomic_fetch_sub(&engine_active, 1);
    return rc;
}
void lifecycle_result_free(ta_result *result) {
    if (result) atomic_fetch_add(&result_frees, 1);
    ta_result_free(result);
}
void analysis_source_release(analysis_source_input *input) {
    assert(!pthread_equal(pthread_self(), loader_thread));
    assert(!atomic_load(&engine_active));
    lifecycle_source_release_real(input);
}

/* Host services outside this lifecycle boundary are explicit dependencies. */
enum {
    AZCM_LOADED = 1,
    AZCM_DISABLED,
    AZCM_FAILED
};
typedef struct {
    const az_mod_v1 *m;
    void *screen;
    int state, nhooks, hooks_ok, sticky, inflight, dirty;
    char id[33], version[16], reason[160];
} cm_mod;
static cm_mod g_cm[2];
static int g_cm_loading = -1, g_sdk_in_init, g_cm_live, fake_threads, stop_calls, init_failure;
static int g_sdk_callbacks[2];
static const char g_cm_fwtag[] = "fixture";
static const az_core_v1 g_cm_core = {.abi = AZ_MOD_ABI, .analysis_service = sdk_analysis_service};
static az_analysis_owner lifecycle_owner;
static uint64_t lifecycle_ids[4];
static unsigned lifecycle_count;
#define CM_INITING "/nonexistent/analysis-lifecycle-marker"
#define CM_LIVE_DRAIN_MS 500
#define CM_LIVE_THREADS_MS 1000
static void cm_set_state(cm_mod *mod, int state) { mod->state = state; }
static int cm_running(const cm_mod *mod) { return mod->state == AZCM_LOADED; }
static void cm_refuse(cm_mod *mod, int state, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(mod->reason, sizeof mod->reason, format, args);
    va_end(args);
    mod->state = state;
}
static void cm_logf(cm_mod *mod, int error, const char *format, ...) {
    (void) mod;
    (void) error;
    (void) format;
}
static void dbg(const char *format, ...) { (void) format; }
static void cm_mark_initing(const cm_mod *mod) { (void) mod; }
static void ds_retire(int owner) { assert(owner == 0); }
static void sdk_data_retire_owner(int owner) { assert(owner == 0); }
static void sdk_data_owner_init(int owner) { assert(owner == 0); }
static void sdk_command_owner_init(int owner) { assert(owner == 0); }
static void sdk_command_owner_ready(int owner) { assert(owner == 0); }
static void sdk_rows_forget(const cm_mod *mod) { assert(mod == g_cm); }
static void sdk_input_forget(const cm_mod *mod) { assert(mod == g_cm); }
static void sdk_deck_forget(int owner) { assert(owner == 0); }
static int sdk_proc_forget(const char *id) {
    assert(!strcmp(id, "analysis-client"));
    return 0;
}
static int sd_threads_of(int owner) {
    assert(owner == 0);
    return fake_threads;
}
static void azcm_flush_settings(void) {}
static void cm_write_status(void) {}
void analysis_source_retire(az_analysis_owner owner) {
    assert(owner.slot == lifecycle_owner.slot && owner.kind == lifecycle_owner.kind);
    assert(owner.generation == lifecycle_owner.generation && !analysis_owner_current(owner));
}
#include MOD_LIFECYCLE_INC

static uint64_t lifecycle_submit(void) {
    az_analysis_request_v1 r = request(1);
    assert(lifecycle_count < 4);
    uint64_t id = public_submit(&sdk_analysis, &r);
    lifecycle_ids[lifecycle_count++] = id;
    return id;
}
static int lifecycle_init(const az_core_v1 *core) {
    assert(core == &g_cm_core && g_sdk_in_init && g_cm_loading == 0);
    lifecycle_owner = analysis_owner_get(0);
    assert(analysis_owner_current(lifecycle_owner));
    lifecycle_count = 0;
    if (!init_failure) return 0;
    for (unsigned i = 0; i < 4; i++)
        assert(wait_state(lifecycle_owner, lifecycle_submit(), AZ_ANALYSIS_FAILED).result == AZ_BUSY);
    return -7;
}
static void expect_retired(int (*operation)(const char *, uint64_t), uint64_t id) {
    for (unsigned i = 0; i < 2000; i++) {
        int rc = operation("analysis-client", id);
        if (rc == AZ_STALE) return;
        if (rc != AZ_BUSY) fprintf(stderr, "Retired operation: request=%llu rc=%d\n", (unsigned long long) id, rc);
        assert(rc == AZ_BUSY); /* try-lock contention precedes the retired-owner lookup */
        pause_ms();
    }
    assert(!"retired public operation remained busy");
}
static void lifecycle_stop(void) {
    assert(!analysis_owner_current(lifecycle_owner));
    stop_calls++;
    for (unsigned i = 0; i < lifecycle_count; i++) {
        expect_retired(sdk_analysis.cancel, lifecycle_ids[i]);
        expect_retired(sdk_analysis.release, lifecycle_ids[i]);
    }
}
static const az_mod_v1 lifecycle_mod = {.abi = AZ_MOD_ABI, .size = sizeof lifecycle_mod, .id = "analysis-client", .init = lifecycle_init, .stop = lifecycle_stop};
static void start_mod(void) {
    g_cm[0].inflight = g_cm[0].dirty = 0;
    strcpy(g_cm[0].id, "analysis-client");
    assert(cm_run_init(g_cm, &lifecycle_mod) == 0 && cm_running(g_cm));
}
static void retire_mod(void) {
    assert(cm_live_off(g_cm) == 1 && g_cm[0].state == AZCM_DISABLED);
    assert(!analysis_owner_current(lifecycle_owner));
}
static void fill_failed(az_analysis_owner owner, uint64_t ids[4]) {
    az_analysis_request_v1 r = request(1);
    for (unsigned i = 0; i < 4; i++) {
        int rc = submit(owner, &r, &ids[i]);
        if (rc != AZ_OK) fprintf(stderr, "Retirement capacity: owner=%u item=%u rc=%d\n", owner.slot, i, rc);
        assert(rc == AZ_OK);
        assert(wait_state(owner, ids[i], AZ_ANALYSIS_FAILED).result == AZ_BUSY);
    }
}
static void release_failed(az_analysis_owner owner, const uint64_t ids[4]) {
    for (unsigned i = 0; i < 4; i++) {
        assert(wait_state(owner, ids[i], AZ_ANALYSIS_FAILED).result == AZ_BUSY);
        release_request(owner, ids[i]);
    }
}
static void failed_init_reclaims(void) {
    atomic_store(&acquire_result, AZ_BUSY);
    init_failure = 1;
    strcpy(g_cm[0].id, "analysis-client");
    assert(cm_run_init(g_cm, &lifecycle_mod) == -1);
    assert(g_cm[0].state == AZCM_FAILED && !g_cm[0].m && !stop_calls);
    assert(!analysis_owner_current(lifecycle_owner));
    uint64_t first[4], second[4];
    az_analysis_owner a = analysis_owner_begin(1), b = analysis_owner_begin(2);
    fill_failed(a, first);
    fill_failed(b, second); /* all eight global slots recovered */
    release_failed(a, first);
    release_failed(b, second);
    analysis_owner_retire(1);
    analysis_owner_retire(2);
    init_failure = 0;
    puts("Failed init: terminal FAILED/BUSY requests reclaimed without stop or cooperative close");
}
static void terminal_reclamation_and_isolation(void) {
    start_mod();
    uint64_t failed = lifecycle_submit();
    assert(wait_state(lifecycle_owner, failed, AZ_ANALYSIS_FAILED).result == AZ_BUSY);
    atomic_store(&acquire_result, AZ_OK);
    for (unsigned i = 0; i < 3; i++)
        assert(wait_state(lifecycle_owner, lifecycle_submit(), AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    az_analysis_result_v1 copied = {.size = sizeof copied};
    az_vocal_frame_v1 frames[512];
    assert(sdk_analysis.result("analysis-client", lifecycle_ids[1], &copied, NULL, 0, frames, 512) == AZ_OK);
    assert(copied.vocals.count && copied.vocals.frames == frames);
    unsigned first_frame = frames[0].activity;
    atomic_store(&acquire_result, AZ_BUSY);
    az_analysis_owner other = analysis_owner_begin(1);
    uint64_t other_ids[4];
    fill_failed(other, other_ids);
    az_analysis_owner old = lifecycle_owner;
    int freed = atomic_load(&result_frees), stopped = stop_calls;
    retire_mod();
    assert(stop_calls == stopped + 1 && atomic_load(&result_frees) == freed + 3);
    assert(frames[0].activity == first_frame); /* real caller-owned copy remains readable */
    start_mod();
    assert(lifecycle_owner.generation != old.generation);
    for (unsigned i = 0; i < 4; i++)
        assert(wait_state(lifecycle_owner, lifecycle_submit(), AZ_ANALYSIS_FAILED).result == AZ_BUSY);
    release_failed(other, other_ids);
    analysis_owner_retire(1);
    retire_mod();
    puts("Live disable: failed/completed slots and owned results reclaimed; replacement and other owner isolated");
}
static void wait_atomic(atomic_int *value, int expected) {
    for (unsigned i = 0; i < 10000 && atomic_load(value) != expected; i++) pause_ms();
    assert(atomic_load(value) == expected);
}
static void queued_running_and_core_isolation(void) {
    atomic_store(&acquire_result, AZ_OK);
    atomic_store(&blocked, 1);
    atomic_store(&entered, 0);
    atomic_store(&with_grid, 1);
    start_mod();
    for (unsigned i = 0; i < 4; i++) lifecycle_submit();
    wait_atomic(&entered, 1);
    az_analysis_owner other = analysis_owner_begin(1), core = analysis_owner_core_begin();
    az_analysis_request_v1 r = request(1);
    uint64_t other_id;
    assert(submit(other, &r, &other_id) == AZ_OK);
    analysis_runtime_enable(core, 1);
    analysis_runtime_load(0, 1, 100, 1, NULL);
    int released = atomic_load(&releases);
    retire_mod(); /* must return while source acquisition is still paused */
    assert(atomic_load(&blocked) && atomic_load(&releases) == released);
    start_mod();
    uint64_t replacement = lifecycle_submit();
    atomic_store(&blocked, 0);
    assert(wait_state(other, other_id, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    assert(wait_state(lifecycle_owner, replacement, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    assert(wait_deck(0, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    release_request(other, other_id);
    analysis_owner_retire(1);
    analysis_owner_core_retire();
    analysis_runtime_enable(core, 0);
    analysis_runtime_load(0, 2, 101, 0, NULL);
    retire_mod();
    atomic_store(&with_grid, 0);
    puts("Queued/running retirement returns before source resumes; replacement, other mod and core complete");
}
static void engine_children_join_before_release(void) {
    start_mod();
    atomic_store(&with_grid, 1);
    atomic_store(&engine_pause_once, 1);
    atomic_store(&engine_paused, 0);
    atomic_store(&engine_resume, 0);
    az_analysis_request_v1 r = request(2);
    r.components |= AZ_DATA_PHRASES;
    r.stage_mode = AZ_ANALYSIS_CONCURRENT;
    lifecycle_ids[lifecycle_count++] = public_submit(&sdk_analysis, &r);
    wait_atomic(&engine_paused, 1);
    int released = atomic_load(&releases);
    retire_mod();
    assert(atomic_load(&engine_active) == 1 && atomic_load(&releases) == released);
    atomic_store(&engine_resume, 1);
    wait_atomic(&engine_active, 0);
    wait_atomic(&releases, released + 1);
    atomic_store(&with_grid, 0);
    puts("Real engine child paused during retirement: joined cancellation precedes input reclamation");
}
static void saved_and_dirty_reclamation(void) {
    atomic_store(&pending, 1);
    atomic_store(&link_result, AZ_PENDING);
    for (unsigned dirty = 0; dirty < 3; dirty++) {
        start_mod();
        uint64_t id = lifecycle_submit();
        assert(wait_state(lifecycle_owner, id, AZ_ANALYSIS_SAVED).result == AZ_PENDING);
        int released = atomic_load(&releases), freed = atomic_load(&result_frees), stopped = stop_calls;
        g_cm[0].inflight = dirty == 1;
        fake_threads = dirty == 2;
        retire_mod();
        wait_atomic(&releases, released + 1);
        wait_atomic(&result_frees, freed + 1);
        assert(g_cm[0].dirty == !!dirty && stop_calls == stopped + (dirty != 1));
        g_cm[0].inflight = fake_threads = 0;
    }
    atomic_store(&pending, 0);
    puts("Saved requests reclaimed autonomously through normal stop and both existing dirty drain paths");
}
int main(int argc, char **argv) {
    assert(argc == 3);
    audio = argv[1];
    loader_thread = pthread_self();
    assert(analysis_runtime_start(argv[2]) == AZ_OK);
    failed_init_reclaims();
    terminal_reclamation_and_isolation();
    queued_running_and_core_isolation();
    engine_children_join_before_release();
    saved_and_dirty_reclamation();
    analysis_runtime_shutdown();
    assert(!atomic_load(&engine_active));
    puts("Actual loader MOD retirement, runtime reclamation, real joined computation and owner isolation passed");
}

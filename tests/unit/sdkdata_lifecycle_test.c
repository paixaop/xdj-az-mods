/* SPDX-License-Identifier: GPL-3.0-only */
/* Real engine and real community init-result branch; plugin callbacks are the
 * boundary. No library/file backend or independent resource-lifetime model. */
#define _POSIX_C_SOURCE 200809L
#include "sdk_data_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int provider(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    (void)mask; (void)ctx;
    snprintf(data->view.metadata.title, sizeof data->view.metadata.title, "source %llu", (unsigned long long)ref->source_id);
    data->view.metadata_info.availability = AZ_AVAIL_READY;
    return AZ_OK;
}
static int wave_provider(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    (void)ref; (void)ctx;
    assert(mask == ST_DETAIL);
    data->wave_view.column_count = data->wave_query.column_count;
    data->wave_view.info.availability = AZ_AVAIL_READY;
    data->detail[0] = 0;
    return AZ_OK;
}

#define CM_MAX 16
#define AZCM_FAILED 3
#define AZCM_LOADED 1
typedef struct {
    const az_mod_v1 *m;
    const az_screen_v1 *screen;
    int state, nhooks;
    char reason[112];
} cm_mod;
static cm_mod g_cm[CM_MAX];
static int g_cm_loading = -1, g_sdk_in_init, g_cm_live;
static const az_core_v1 g_cm_core;
static struct st_engine lifecycle = ST_ENGINE_INIT;
static int fail_init, borrow_data, wave_data, request_status;
static uint64_t owned[CM_MAX][ST_PER_OWNER];
static const az_track_snapshot *held;
static const az_wave_snapshot *held_wave;
static void sdk_command_retire_owner(int owner) { (void)owner; }
static az_track_ref lifecycle_ref = {sizeof(az_track_ref), AZ_DATA_VERSION, 1, 1, 9, 42, 0};
static void cm_refuse(cm_mod *c, int state, const char *format, ...) {
    (void)format;
    c->state = state;
}
static int fixture_unlink(const char *path) { (void)path; return 0; }
#define CM_INITING "unused-fixture-init-marker"
#define unlink fixture_unlink
#define dbg(...) ((void)0)
#define cm_logf(...) ((void)0)
#define g_st lifecycle
#include SDK_DATA_LIFECYCLE_INC
#undef g_st
#undef unlink

static int module_init(const az_core_v1 *core) {
    assert(core == &g_cm_core && g_sdk_in_init && g_cm_loading >= 0);
    int owner = g_cm_loading;
    unsigned count = fail_init ? ST_PER_OWNER : 1;
    for (unsigned i = 0; i < count; ++i) {
        az_wave_query query = {sizeof query, AZ_DATA_VERSION, AZ_WAVE_PWV3_MONO, 0, 1, 0};
        request_status = wave_data ? st_request_with_wave(&lifecycle, owner, &lifecycle_ref, ST_DETAIL, &query, &owned[owner][i])
                                   : st_request(&lifecycle, owner, &lifecycle_ref, AZ_DATA_METADATA, &owned[owner][i]);
        if (fail_init) assert(request_status == AZ_OK);
    }
    if (borrow_data) {
        st_work(&lifecycle, wave_data ? wave_provider : provider, NULL, NULL);
        assert((wave_data ? st_wave_acquire(&lifecycle, owner, owned[owner][0], &held_wave)
                          : st_acquire(&lifecycle, owner, owned[owner][0], &held)) == AZ_OK);
    }
    return fail_init ? -1 : 0;
}
static void reset_lifecycle(void) {
    memset(g_cm, 0, sizeof g_cm);
    memset(owned, 0, sizeof owned);
    held = NULL; held_wave = NULL;
    fail_init = borrow_data = wave_data = g_cm_live = 0;
}
static void reinitialize_engine(void) {
    memset(&lifecycle, 0, sizeof lifecycle);
    atomic_init(&lifecycle.retired_owners, 0);
    assert(!pthread_mutex_init(&lifecycle.mu, NULL));
}
static uint64_t adopted;
static int retire_during_load(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    assert(!pthread_mutex_trylock(&lifecycle.mu));
    pthread_mutex_unlock(&lifecycle.mu);
    sdk_data_retire_owner(0);
    assert(st_request(&lifecycle, 1, ref, mask, &adopted) == AZ_OK);
    return provider(ref, mask, data, ctx);
}
int main(void) {
    reset_lifecycle();
    const az_mod_v1 module = {.init = module_init};
    for (int i = 0; i < ST_TICKETS / ST_PER_OWNER; ++i) {
        fail_init = 1;
        wave_data = i & 1;
        run_actual_init(&g_cm[i], &module);
        assert(g_cm[i].state == AZCM_FAILED && !g_cm[i].m && g_cm_loading == -1 && !g_sdk_in_init);
    }
    puts("FAILED_INIT_DATA_BASELINE_OK: actual init/result branch ran for eight failed plugins");
    fflush(stdout);
    fail_init = wave_data = 0;
    run_actual_init(&g_cm[8], &module);
    assert(request_status == AZ_OK && "failed initialization must not strand unacquired data ticket quota");
    assert(g_cm[8].state == AZCM_LOADED && g_cm_live == 1);
    assert(st_release(&lifecycle, 8, owned[8][0], 0) == AZ_OK);
    st_destroy(&lifecycle);

    for (unsigned mode = 0; mode < 2; ++mode) {
        reinitialize_engine();
        reset_lifecycle();
        wave_data = mode;
        fail_init = borrow_data = 1;
        run_actual_init(&g_cm[0], &module);
        if (mode) assert(held_wave && held_wave->column_count == 1 && held_wave->columns[0] == 0);
        else assert(held && !strcmp(held->metadata.title, "source 1"));
        fail_init = borrow_data = 0;
        run_actual_init(&g_cm[1], &module);
        assert(request_status == AZ_OK);
        if (mode) assert(held_wave->column_count == 1 && held_wave->columns[0] == 0);
        else assert(!strcmp(held->metadata.title, "source 1"));
        unsigned retained = 0;
        for (unsigned i = 0; i < ST_TICKETS; ++i)
            if (lifecycle.tickets[i].id && lifecycle.tickets[i].owner == 0) ++retained;
        assert(retained == 1 && "retirement preserves acquired snapshots and removes only unborrowed tickets");
        const az_track_snapshot *out = NULL;
        const az_wave_snapshot *out_wave = NULL;
        assert((mode ? st_wave_acquire(&lifecycle, 0, owned[0][0], &out_wave)
                     : st_acquire(&lifecycle, 0, owned[0][0], &out)) == AZ_STALE && !out && !out_wave);
        uint64_t refused = UINT64_C(0xfedcba9876543210);
        assert(st_request(&lifecycle, 0, &lifecycle_ref, AZ_DATA_METADATA, &refused) == AZ_STALE);
        assert(refused == UINT64_C(0xfedcba9876543210));
        pthread_mutex_lock(&lifecycle.mu);
        sdk_data_retire_owner(0); /* GUI retirement must never wait for the engine. */
        pthread_mutex_unlock(&lifecycle.mu);
        assert(st_release(&lifecycle, 0, owned[0][0], 0) == AZ_OK);
        assert(st_release(&lifecycle, 1, owned[1][0], 0) == AZ_OK);
        st_destroy(&lifecycle);
    }
    reinitialize_engine();
    uint64_t abandoned;
    assert(st_request(&lifecycle, 0, &lifecycle_ref, AZ_DATA_METADATA, &abandoned) == AZ_OK);
    st_work(&lifecycle, retire_during_load, NULL, NULL);
    const az_track_snapshot *out = NULL;
    assert(st_acquire(&lifecycle, 0, abandoned, &out) == AZ_STALE && !out);
    assert(st_acquire(&lifecycle, 1, adopted, &out) == AZ_OK && !strcmp(out->metadata.title, "source 1"));
    assert(st_release(&lifecycle, 1, adopted, 0) == AZ_OK);
    st_destroy(&lifecycle);
    puts("PASS failed-init data cleanup: real init routing, bounded quota, immutable lifetime and nonblocking retirement");
}

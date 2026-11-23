/* SPDX-License-Identifier: GPL-3.0-only */
/* Production SDK wrappers + runtime + engine. Authentication/firmware are explicit
 * fixture dependencies; native source/storage behavior has separate production tests. */
static int g_sdk_on_tap_thread, g_cm_on_ui = 1;
#define CM_RA() ((uintptr_t)1)
static int st_mod_owner(const char *id, uintptr_t caller, const char *service) {
    assert(!strcmp(service, "analysis") || !strcmp(service, "vocal_intervals") ||
           !strcmp(service, "vocal_intervals_loaded"));
    return id && !strcmp(id, "analysis-client") && caller == 1 ? 0 : -1;
}
int analysis_vocal_intervals(az_analysis_owner owner, const az_track_ref *track,
    az_vocal_snapshot_v1 *out, az_vocal_interval *intervals, uint32_t capacity) {
    (void)owner; (void)track; (void)out; (void)intervals; (void)capacity;
    return AZ_UNSUPPORTED;
}
int analysis_vocal_intervals_loaded(az_analysis_owner owner, int deck, uint64_t epoch,
    az_vocal_snapshot_v1 *out, az_vocal_interval *intervals, uint32_t capacity) {
    (void)owner; (void)deck; (void)epoch; (void)out; (void)intervals; (void)capacity;
    return AZ_UNSUPPORTED;
}
#include "../../src/azmod/analysis_api.c"
static uint64_t public_submit(const az_analysis_v1 *service, az_analysis_request_v1 *request) {
    uint64_t id = 0;
    for (int i = 0; i < 2000; i++) {
        int rc = service->request("analysis-client", request, &id);
        if (rc == AZ_OK) return id;
        assert(rc == AZ_BUSY); pause_ms();
    }
    abort();
}
static void public_wait(const az_analysis_v1 *service, uint64_t id) {
    az_analysis_status_v1 status = {.size = sizeof status};
    for (int i = 0; i < 20000; i++) {
        int rc = service->status("analysis-client", id, &status);
        if (rc == AZ_OK && status.state >= AZ_ANALYSIS_COMPLETE) {
            if (status.result) fprintf(stderr, "public analysis: %s\n", status.diagnostic);
            assert(status.state == AZ_ANALYSIS_COMPLETE && status.result == AZ_OK);
            return;
        }
        assert(rc == AZ_BUSY || rc == AZ_OK); pause_ms();
    }
    abort();
}
static void public_mode(const az_analysis_v1 *service, az_analysis_request_v1 *r) {
    uint64_t id = public_submit(service, r);
    public_wait(service, id);
    az_analysis_result_v1 result = {.size = sizeof result};
    az_analysis_phrase_v1 phrases[1024];
    az_vocal_frame_v1 vocals[512] = {{0}};
    assert(service->result("analysis-client", id, &result, phrases, 1024, vocals, 512) == AZ_OK);
    assert(!!result.phrases.count == !!(r->components & AZ_DATA_PHRASES));
    assert(!!result.vocals.count == !!(r->components & AZ_ANALYSIS_VOCALS));
    unsigned activity = vocals[0].activity;
    assert(service->release("analysis-client", id) == AZ_OK);
    if (result.vocals.count) assert(vocals[0].activity == activity);
}
static void public_contract(az_analysis_owner owner) {
    const az_analysis_v1 *service = sdk_analysis_service(AZ_ANALYSIS_ABI);
    assert(service && !sdk_analysis_service(AZ_ANALYSIS_ABI + 1));
    az_analysis_capabilities_v1 caps = {.size = sizeof caps};
    assert(service->capabilities(&caps) == AZ_OK && caps.max_workers == 3);
    az_analysis_request_v1 r = request(1);
    uint64_t id = 0;
    assert(service->request("another-owner", &r, &id) != AZ_OK && !id);
    g_sdk_on_tap_thread = 1;
    assert(service->request("analysis-client", &r, &id) != AZ_OK && !id);
    g_sdk_on_tap_thread = 0; g_cm_on_ui = 0;
    assert(service->request("analysis-client", &r, &id) != AZ_OK && !id);
    analysis_owner_thread_bind(owner);
    az_vocal_snapshot_v1 snapshot = {.size = sizeof snapshot};
    az_vocal_interval intervals[4];
    assert(service->vocal_intervals("another-owner", &r.track, &snapshot, intervals, 4) == AZ_INVALID);
    assert(service->vocal_intervals_loaded("another-owner", 1, 1, &snapshot, intervals, 4) == AZ_INVALID);
    g_cm_on_ui = 1;
    assert(service->vocal_intervals("analysis-client", &r.track, &snapshot, intervals, 4) == AZ_INVALID);
    assert(service->vocal_intervals_loaded("analysis-client", 1, 1, &snapshot, intervals, 4) == AZ_INVALID);
    g_cm_on_ui = 0; g_sdk_on_tap_thread = 1;
    assert(service->vocal_intervals("analysis-client", &r.track, &snapshot, intervals, 4) == AZ_INVALID);
    assert(service->vocal_intervals_loaded("analysis-client", 1, 1, &snapshot, intervals, 4) == AZ_INVALID);
    g_sdk_on_tap_thread = 0;
    assert(service->vocal_intervals("analysis-client", &r.track, &snapshot, intervals, 4) == AZ_UNSUPPORTED);
    assert(service->vocal_intervals_loaded("analysis-client", 1, 1, &snapshot, intervals, 4) == AZ_UNSUPPORTED);
    atomic_store(&with_grid, 1);
    const unsigned components[] = {AZ_DATA_PHRASES, AZ_ANALYSIS_VOCALS, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    for (unsigned c = 0; c < 3; c++) {
        r.components = components[c]; r.stage_mode = AZ_ANALYSIS_SEQUENTIAL;
        for (unsigned p = 1; p <= 3; p++) for (unsigned v = 1; v <= 3; v++) {
            r.phrase_threads = p; r.vocal_threads = v; public_mode(service, &r);
        }
        r.stage_mode = AZ_ANALYSIS_CONCURRENT;
        const unsigned splits[][2] = {{1, 1}, {1, 2}, {2, 1}};
        for (unsigned split = 0; split < 3; split++) {
            r.phrase_threads = splits[split][0]; r.vocal_threads = splits[split][1];
            public_mode(service, &r);
        }
    }
    atomic_store(&blocked, 1); atomic_store(&entered, 0);
    id = public_submit(service, &r);
    for (unsigned i = 0; i < 2000 && !atomic_load(&entered); i++) pause_ms();
    assert(atomic_load(&entered));
    assert(service->cancel("analysis-client", id) == AZ_OK);
    atomic_store(&blocked, 0);
    assert(wait_state(owner, id, AZ_ANALYSIS_CANCELLED).result == AZ_STALE);
    assert(service->release("analysis-client", id) == AZ_OK);
    atomic_store(&with_grid, 0); g_cm_on_ui = 1;
    puts("Public request boundary: phrase/vocal/combined in all twelve execution configurations passed");
}

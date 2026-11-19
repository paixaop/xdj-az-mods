/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../../src/azmod/analysis_runtime.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const char *audio;
static atomic_int clock_offset, jump_at_acquire;
/* Controlled monotonic passage for the real coordinator deadline wait. */
int fixture_clock_gettime(clockid_t clock, struct timespec *out) {
    int rc = clock_gettime(clock, out);
    if (!rc && clock == CLOCK_MONOTONIC) out->tv_sec += atomic_load(&clock_offset);
    return rc;
}
#ifdef __linux__
int fixture_condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline) {
    struct timespec actual = *deadline;
    actual.tv_sec -= atomic_load(&clock_offset);
    return pthread_cond_timedwait(condition, mutex, &actual);
}
#endif
static atomic_int acquisitions, publications, releases, blocked, entered, pending, link_result, outcomes, acquire_result, with_grid, publish_busy, publish_attempts;
static void (*source_wake)(void);
static atomic_uint_fast64_t cancel_after_publish;
static void pause_ms(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
void analysis_source_set_wake(void (*wake)(void)) { source_wake = wake; }
int analysis_source_acquire(az_analysis_owner owner, const analysis_source_request *request,
                            analysis_source_input *input) {
    stack_t signal_stack;
    assert(!sigaltstack(NULL, &signal_stack) && !(signal_stack.ss_flags & SS_DISABLE));
    atomic_fetch_add(&acquisitions, 1);
    atomic_store(&entered, 1);
    while (atomic_load(&blocked)) pause_ms();
    if (!analysis_owner_current(owner)) return AZ_STALE;
    *input = (analysis_source_input){.lease = 1, .owner = owner,
        .needed = request->components, .deck = (unsigned)request->deck,
        .load_epoch = request->load, .track = request->track};
    snprintf(input->audio_path, sizeof input->audio_path, "%s", audio);
    if (atomic_load(&with_grid) && (request->components & AZ_DATA_PHRASES)) {
        input->beat_count = 26; input->bpm = 120;
        input->beats = calloc(input->beat_count, sizeof *input->beats);
        assert(input->beats);
        for (unsigned i = 0; i < input->beat_count; i++)
            input->beats[i] = (az_beat){.time_s = i * 0.5, .bpm = 120, .beat_in_bar = i % 4 + 1};
    }
    if (atomic_exchange(&jump_at_acquire, 0)) atomic_store(&clock_offset, 599);
    return atomic_load(&acquire_result);
}
int analysis_source_publish(az_analysis_owner owner, const analysis_source_input *input,
                            const az_analysis_result_v1 *result) {
    assert(input->lease && (result->vocals.count || result->phrases.count));
    if (!analysis_owner_current(owner)) return AZ_STALE;
    atomic_fetch_add(&publish_attempts, 1);
    if (atomic_load(&publish_busy) && atomic_fetch_sub(&publish_busy, 1) > 0) return AZ_BUSY;
    atomic_fetch_add(&publications, 1);
    uint64_t cancel_id = atomic_load(&cancel_after_publish);
    if (cancel_id) assert(analysis_runtime_cancel(owner, cancel_id) == AZ_OK);
    return atomic_load(&pending) ? AZ_PENDING : AZ_OK;
}
int analysis_source_outcome(az_analysis_owner owner, const analysis_source_input *input) {
    assert(input->lease);
    if (!analysis_owner_current(owner)) return AZ_STALE;
    atomic_fetch_add(&outcomes, 1);
    return atomic_load(&link_result);
}
void analysis_source_release(analysis_source_input *input) {
    if (input->lease) atomic_fetch_add(&releases, 1);
    free(input->beats);
    memset(input, 0, sizeof *input);
}
static az_analysis_request_v1 request(unsigned workers) {
    return (az_analysis_request_v1){.size = sizeof(az_analysis_request_v1),
        .version = AZ_ANALYSIS_REQUEST_VERSION, .source_kind = AZ_ANALYSIS_DECK,
        .components = AZ_ANALYSIS_VOCALS, .load_epoch = 42, .deck = 1,
        .stage_mode = AZ_ANALYSIS_SEQUENTIAL, .phrase_threads = 1, .vocal_threads = workers};
}
static int submit(az_analysis_owner owner, const az_analysis_request_v1 *r, uint64_t *id) {
    int rc;
    do { rc = analysis_runtime_request(owner, r, id); if (rc == AZ_BUSY) pause_ms(); } while (rc == AZ_BUSY);
    return rc;
}
static az_analysis_status_v1 wait_state(az_analysis_owner owner, uint64_t id, unsigned state) {
    az_analysis_status_v1 status = {.size = sizeof status};
    for (int i = 0; i < 20000; i++) {
        int rc = analysis_runtime_status(owner, id, &status);
        if (rc == AZ_OK && status.state == state) return status;
        assert(rc == AZ_OK || rc == AZ_BUSY);
        pause_ms();
    }
    fprintf(stderr, "wait state %u got %u/%d: %s\n", state, status.state, status.result, status.diagnostic);
    abort();
}
static void release_request(az_analysis_owner owner, uint64_t id) {
    int rc;
    do { rc = analysis_runtime_release(owner, id); if (rc == AZ_BUSY) pause_ms(); } while (rc == AZ_BUSY);
    assert(rc == AZ_OK);
}
static void computation(az_analysis_owner owner) {
    az_analysis_request_v1 r = request(3);
    uint64_t id = 0;
    assert(submit(owner, &r, &id) == AZ_OK && id);
    az_analysis_status_v1 status = wait_state(owner, id, AZ_ANALYSIS_COMPLETE);
    assert(status.result == AZ_OK && status.completed_components == AZ_ANALYSIS_VOCALS);
    az_analysis_result_v1 result = {.size = sizeof result};
    assert(analysis_runtime_result(owner, id, &result, NULL, 0, NULL, 0) == AZ_LIMIT);
    assert(result.vocals.count >= 9 && !result.vocals.frames && !result.phrases.entries);
    az_vocal_frame_v1 *frames = calloc(result.vocals.count, sizeof *frames);
    assert(frames);
    assert(analysis_runtime_result(owner, id, &result, NULL, 0, frames, result.vocals.count) == AZ_OK);
    unsigned first = frames[0].activity;
    release_request(owner, id);
    assert(frames[0].activity == first); /* caller-owned bytes survive release */
    free(frames);
    assert(analysis_runtime_result(owner, id, &result, NULL, 0, NULL, 0) == AZ_STALE);
}
static void invalid_requests(az_analysis_owner owner) {
    az_analysis_request_v1 r = request(1);
    uint64_t id = 0;
    r.version++;
    assert(submit(owner, &r, &id) == AZ_INVALID);
    r.version--; r.components |= AZ_DATA_PHRASES;
    r.stage_mode = AZ_ANALYSIS_CONCURRENT; r.phrase_threads = 2; r.vocal_threads = 2;
    assert(submit(owner, &r, &id) == AZ_INVALID);
    r.phrase_threads = 1; r.reserved[2] = 1;
    assert(submit(owner, &r, &id) == AZ_INVALID);
    assert(!id);
    r = request(1); r.source_kind = 99;
    assert(submit(owner, &r, &id) == AZ_INVALID);
    r.source_kind = AZ_ANALYSIS_TRACK;
    assert(submit(owner, &r, &id) == AZ_INVALID);
    r.track = (az_track_ref){.size = sizeof r.track, .version = AZ_DATA_VERSION,
        .track_id = 55, .source_id = 1};
    r.track.version++;
    assert(submit(owner, &r, &id) == AZ_INVALID); r.track.version--;
    r.track.source_id = 3;
    assert(submit(owner, &r, &id) == AZ_INVALID); r.track.source_id = 1;
    assert(submit(owner, &r, &id) == AZ_OK);
    assert(wait_state(owner, id, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    release_request(owner, id);
}
static void capacity_and_retirement(az_analysis_owner owner) {
    atomic_store(&blocked, 1); atomic_store(&entered, 0);
    az_analysis_request_v1 r = request(1);
    uint64_t ids[4];
    for (unsigned i = 0; i < 4; i++) assert(submit(owner, &r, &ids[i]) == AZ_OK);
    while (!atomic_load(&entered)) pause_ms();
    uint64_t rejected = 0;
    assert(submit(owner, &r, &rejected) == AZ_LIMIT && !rejected);
    az_analysis_owner second = analysis_owner_begin(1);
    uint64_t other;
    assert(submit(second, &r, &other) == AZ_OK);
    int before = atomic_load(&publications);
    az_analysis_owner retired = analysis_owner_retire(owner.slot);
    analysis_runtime_retire(retired);
    atomic_store(&blocked, 0);
    az_analysis_owner replacement = analysis_owner_begin(owner.slot);
    assert(replacement.generation != owner.generation);
    assert(submit(owner, &r, &rejected) == AZ_STALE);
    assert(submit(replacement, &r, &rejected) == AZ_OK);
    assert(wait_state(second, other, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    assert(wait_state(replacement, rejected, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    assert(atomic_load(&publications) == before + 2);
    az_analysis_status_v1 status = {.size = sizeof status};
    assert(analysis_runtime_status(owner, ids[0], &status) == AZ_STALE);
    assert(analysis_runtime_release(owner, ids[0]) == AZ_STALE);
    release_request(second, other); release_request(replacement, rejected);
}
static void cancel_publication_collection(az_analysis_owner owner) {
    atomic_store(&blocked, 1); atomic_store(&entered, 0);
    az_analysis_request_v1 r = request(1);
    uint64_t id;
    assert(submit(owner, &r, &id) == AZ_OK);
    while (!atomic_load(&entered)) pause_ms();
    atomic_store(&cancel_after_publish, id);
    atomic_store(&blocked, 0);
    az_analysis_status_v1 result = wait_state(owner, id, AZ_ANALYSIS_CANCELLED);
    assert(result.result == AZ_STALE && result.completed_components == AZ_ANALYSIS_VOCALS);
    atomic_store(&cancel_after_publish, 0);
    release_request(owner, id);
}
static void pending_link(az_analysis_owner owner) {
    atomic_store(&pending, 1); atomic_store(&link_result, AZ_PENDING);
    az_analysis_request_v1 r = request(1);
    uint64_t id;
    assert(submit(owner, &r, &id) == AZ_OK);
    az_analysis_status_v1 saved = wait_state(owner, id, AZ_ANALYSIS_SAVED);
    assert(saved.result == AZ_PENDING && saved.completed_components == AZ_ANALYSIS_VOCALS);
    int count = atomic_load(&publications);
    int checks = atomic_load(&outcomes);
    atomic_store(&link_result, AZ_BUSY);
    assert(source_wake); source_wake();
    for (int i = 0; i < 2000 && atomic_load(&outcomes) == checks; i++) pause_ms();
    assert(atomic_load(&outcomes) > checks);
    assert(wait_state(owner, id, AZ_ANALYSIS_SAVED).result == AZ_PENDING);
    atomic_store(&link_result, AZ_FAILED); /* retry must progress without another wake */
    assert(wait_state(owner, id, AZ_ANALYSIS_FAILED).result == AZ_FAILED);
    assert(atomic_load(&publications) == count); /* no second decode/publication */
    release_request(owner, id);
    atomic_store(&pending, 0);
}
static void pending_deadline(az_analysis_owner owner) {
    atomic_store(&acquire_result, AZ_PENDING); atomic_store(&link_result, AZ_PENDING);
    atomic_store(&jump_at_acquire, 1);
    int queries = atomic_load(&outcomes);
    az_analysis_request_v1 r = request(1);
    uint64_t id;
    assert(submit(owner, &r, &id) == AZ_OK);
    assert(wait_state(owner, id, AZ_ANALYSIS_FAILED).result == AZ_STALE);
    assert(atomic_load(&outcomes) == queries); /* deadline ends it without polling source */
    release_request(owner, id);
    atomic_store(&acquire_result, AZ_OK); atomic_store(&clock_offset, 0);
}
static void busy_publication(az_analysis_owner owner) {
    int opened = atomic_load(&acquisitions), committed = atomic_load(&publications);
    int attempts = atomic_load(&publish_attempts);
    atomic_store(&publish_busy, 2);
    az_analysis_request_v1 r = request(1);
    uint64_t id;
    assert(submit(owner, &r, &id) == AZ_OK);
    assert(wait_state(owner, id, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    assert(atomic_load(&acquisitions) == opened + 1);
    assert(atomic_load(&publications) == committed + 1);
    assert(atomic_load(&publish_attempts) == attempts + 3);
    release_request(owner, id);
}
static void acquisition_outcomes(az_analysis_owner owner) {
    const int outcomes[] = {AZ_NOT_FOUND, ANALYSIS_SOURCE_COMPLETE};
    const unsigned states[] = {AZ_ANALYSIS_FAILED, AZ_ANALYSIS_COMPLETE};
    az_analysis_request_v1 r = request(1);
    for (unsigned i = 0; i < 2; i++) {
        atomic_store(&acquire_result, outcomes[i]);
        uint64_t id;
        assert(submit(owner, &r, &id) == AZ_OK);
        assert(wait_state(owner, id, states[i]).result == AZ_NOT_FOUND);
        release_request(owner, id);
    }
    atomic_store(&acquire_result, AZ_OK);
}
static az_analysis_status_v1 wait_deck(unsigned deck, unsigned state) {
    az_analysis_status_v1 status = {.size = sizeof status};
    for (int i = 0; i < 10000; i++) {
        if (analysis_runtime_deck_status(deck, &status) == AZ_OK && status.state == state) return status;
        pause_ms();
    }
    abort();
}
static void automatic_operation(void) {
    az_analysis_owner core = analysis_owner_core_begin();
    int count = atomic_load(&acquisitions);
    /* Missing native grid gives a visible failure; it must run without a mod. */
    atomic_store(&blocked, 1); atomic_store(&entered, 0);
    analysis_runtime_enable(core, 1);
    analysis_runtime_load(0, 2, 17, 1, NULL);
    analysis_runtime_load(0, 1, 17, 0, NULL); /* delayed old reduction cannot unload the new cut */
    assert(wait_deck(0, AZ_ANALYSIS_RUNNING).request_id);
    atomic_store(&blocked, 0);
    az_analysis_status_v1 status = wait_deck(0, AZ_ANALYSIS_FAILED);
    assert(atomic_load(&acquisitions) > count && status.result == AZ_FAILED);
    atomic_store(&acquire_result, AZ_PENDING); atomic_store(&link_result, AZ_PENDING);
    analysis_runtime_load(0, 3, 18, 1, NULL);
    assert(wait_deck(0, AZ_ANALYSIS_SAVED).result == AZ_PENDING);
    atomic_store(&link_result, AZ_OK); source_wake();
    assert(wait_deck(0, AZ_ANALYSIS_COMPLETE).result == AZ_OK);
    atomic_store(&acquire_result, AZ_OK);
    assert(!setenv("TRACK_ANALYSIS_STAGE_MODE", "invalid", 1));
    analysis_runtime_load(0, 4, 19, 1, NULL);
    status = wait_deck(0, AZ_ANALYSIS_FAILED);
    assert(status.request_id && status.load_epoch == 19 && status.result == AZ_INVALID);
    assert(!unsetenv("TRACK_ANALYSIS_STAGE_MODE"));
    analysis_runtime_enable(core, 0);
    analysis_owner_core_retire();
    analysis_runtime_retire(core);
}
#include "test_api.inc"
int main(int argc, char **argv) {
    assert(argc == 3); audio = argv[1];
    az_analysis_capabilities_v1 caps = {.size = sizeof caps};
    assert(analysis_runtime_capabilities(&caps) == AZ_OK && caps.max_workers == 3);
    assert(analysis_runtime_start(argv[2]) == AZ_OK);
    az_analysis_owner owner = analysis_owner_begin(0);
    public_contract(owner);
    invalid_requests(owner); acquisition_outcomes(owner); busy_publication(owner); pending_deadline(owner); computation(owner); pending_link(owner); cancel_publication_collection(owner);
    capacity_and_retirement(owner); automatic_operation();
    analysis_runtime_shutdown();
    assert(atomic_load(&releases) <= atomic_load(&acquisitions));
    puts("Core runtime computation, copied results, owner retirement, capacity and link outcomes passed");
}

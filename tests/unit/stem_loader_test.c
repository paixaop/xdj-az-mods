/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual load dispatch, status qualification and completion; decoding and
 * native identity are independently tested service boundaries. */
#define _GNU_SOURCE
#define AZ_STATUS_NATIVE_PRODUCER 1
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/common/azloadobserver.h"
#include "../../src/common/azstatus_native.h"
#define NDECK 4
static struct {
    char loaded_track[256], track_path[600];
    long stem_frames;
} g_dk[NDECK];
static _Atomic int g_load_stale[NDECK], g_gate_hold[NDECK], g_load_again[NDECK], g_stem_loading[NDECK];
static _Atomic unsigned g_env_cid[NDECK];
static pthread_mutex_t g_adv_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_lti_on, native_path_ok, refuse_allocation, refuse_thread, decode_result, replace_during_decode;
static int load_reads, read_refusal, mutate_identity, decode_calls, discard_calls, status_calls, jobs;
static _Atomic unsigned allocations, releases;
static unsigned expected_deck;
static const char *expected_track, *expected_path;
static struct azsdk_load_sample identity = {.unit_generation = 3, .load_generation = 7, .invalidation_epoch = 11};
static struct azn_stem_job completed_job;
static int completed_playable;
static void logln(const char *format, ...) { assert(format); }
static int lti_native_path(int deck, char *out, size_t size) {
    assert(deck == (int) expected_deck);
    if (native_path_ok) snprintf(out, size, "/native/track.wav");
    return native_path_ok;
}
static unsigned lti_content_id(int deck) {
    assert(deck == (int) expected_deck);
    return 123;
}
int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    assert(deck == (int) expected_deck);
    ++load_reads;
    if (read_refusal == load_reads) return 0;
    *out = identity;
    if (load_reads == 2) {
        if (mutate_identity == 1) ++out->load_generation;
        if (mutate_identity == 2) ++out->invalidation_epoch;
        if (mutate_identity == 3) ++out->unit_generation;
    }
    return 1;
}
struct azn_stem_job azn_stems_job(unsigned deck, uint64_t load, const char *path) {
    assert(deck == expected_deck && load == identity.load_generation && !strcmp(path, expected_path));
    ++jobs;
    return (struct azn_stem_job) {load, 17, 19, 23};
}
void azn_stems_decoded(unsigned deck, struct azn_stem_job job, const char *path, int playable) {
    assert(deck == expected_deck && !strcmp(path, expected_path));
    ++status_calls;
    completed_job = job;
    completed_playable = playable;
}
static int load_stems_ogg(int deck, const char *track, const char *path) {
    assert(deck == (int) expected_deck && !strcmp(track, expected_track) && !strcmp(path, expected_path));
    assert(atomic_load(&g_stem_loading[deck]) && atomic_load(&g_env_cid[deck]) == 123);
    ++decode_calls;
    g_dk[deck].stem_frames = decode_result > 0 ? 1024 : 0;
    if (replace_during_decode) {
        atomic_store(&g_load_stale[deck], 1);
        atomic_store(&g_gate_hold[deck], 1);
    }
    return decode_result;
}
static void free_stems(int deck) {
    assert(deck == (int) expected_deck);
    ++discard_calls;
    g_dk[deck].stem_frames = 0;
}
static void *job_malloc(size_t size) {
    if (refuse_allocation) return NULL;
    void *value = malloc(size);
    assert(value);
    atomic_fetch_add(&allocations, 1);
    return value;
}
static void job_free(void *value) {
    assert(value);
    atomic_fetch_add(&releases, 1);
    free(value);
}
static pthread_mutex_t finished_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t finished_cv = PTHREAD_COND_INITIALIZER;
static int finished, detached;
static struct {
    void *(*entry)(void *);
    void *argument;
} dispatch;
static void *run_dispatch(void *unused) {
    (void) unused;
    void *result = dispatch.entry(dispatch.argument);
    pthread_mutex_lock(&finished_mu);
    finished = 1;
    pthread_cond_signal(&finished_cv);
    pthread_mutex_unlock(&finished_mu);
    return result;
}
static int az_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    assert(!attr && entry && arg);
    if (refuse_thread) return 11;
    dispatch.entry = entry;
    dispatch.argument = arg;
    return pthread_create(thread, NULL, run_dispatch, NULL);
}
static int detach(pthread_t thread) {
    ++detached;
    return pthread_detach(thread);
}
#define malloc job_malloc
#define free job_free
#define pthread_detach detach
#include STEM_LOADER_INC
#undef pthread_detach
#undef free
#undef malloc
static void source_identity(void) {
    expected_deck = 2;
    expected_path = "/fallback.wav";
    strcpy(g_dk[2].track_path, expected_path);
    for (int refusal = 0; refusal <= 2; ++refusal)
        for (int mutation = 0; mutation <= 3; ++mutation) {
            loadjob_t job = {0};
            read_refusal = refusal;
            mutate_identity = mutation;
            load_reads = jobs = 0;
            stem_load_source(&job, 2);
            assert(!strcmp(job.src, expected_path));
            assert(jobs == (!refusal && !mutation));
            assert(job.status_job.load == ((!refusal && !mutation) ? identity.load_generation : 0));
        }
    read_refusal = mutate_identity = load_reads = 0;
    g_lti_on = 1;
    native_path_ok = 1;
    expected_path = "/native/track.wav";
    loadjob_t job = {0};
    stem_load_source(&job, 2);
    assert(!strcmp(job.src, expected_path));
    native_path_ok = 0;
    load_reads = 0;
    expected_path = "";
    stem_load_source(&job, 2);
    assert(!job.src[0]);
    g_lti_on = 0;
}
static void dispatch_cases(void) {
    expected_track = "new song";
    expected_path = "/fallback.wav";
    for (unsigned deck = 0; deck < NDECK; ++deck)
        for (int threaded = 0; threaded < 2; ++threaded)
            for (int result = 0; result < 3; ++result) {
                expected_deck = deck;
                refuse_thread = !threaded;
                decode_result = result ? 1 : 0;
                replace_during_decode = result == 2;
                for (unsigned d = 0; d < NDECK; ++d) {
                    strcpy(g_dk[d].loaded_track, "prior");
                    strcpy(g_dk[d].track_path, expected_path);
                }
                finished = detached = load_reads = jobs = decode_calls = discard_calls = status_calls = 0;
                atomic_store(&g_load_again[deck], 0);
                unsigned before = atomic_load(&releases);
                start_load((int) deck, expected_track);
                if (threaded) {
                    pthread_mutex_lock(&finished_mu);
                    while (!finished) pthread_cond_wait(&finished_cv, &finished_mu);
                    pthread_mutex_unlock(&finished_mu);
                }
                assert(decode_calls == 1 && jobs == 1 && detached == threaded && atomic_load(&releases) == before + 1);
                assert(!atomic_load(&g_stem_loading[deck]));
                if (replace_during_decode) {
                    assert(discard_calls == 1 && !status_calls && !strcmp(g_dk[deck].loaded_track, "prior"));
                    assert(atomic_load(&g_load_again[deck]) && !atomic_load(&g_load_stale[deck]) && !atomic_load(&g_gate_hold[deck]));
                } else {
                    assert(discard_calls == !decode_result && status_calls == 1 && completed_playable == !!decode_result);
                    assert(completed_job.load == identity.load_generation && completed_job.pin == 17 && !strcmp(g_dk[deck].loaded_track, expected_track));
                    assert(g_dk[deck].stem_frames == (decode_result ? 1024 : 0));
                }
                for (unsigned d = 0; d < NDECK; ++d)
                    if (d != deck) assert(!strcmp(g_dk[d].loaded_track, "prior"));
            }
    refuse_allocation = 1;
    decode_calls = 0;
    load_reads = 0;
    unsigned before = atomic_load(&allocations);
    start_load((int) expected_deck, "refused");
    assert(atomic_load(&allocations) == before && !decode_calls && !load_reads && !atomic_load(&g_stem_loading[expected_deck]));
    assert(atomic_load(&allocations) == atomic_load(&releases));
}
int main(void) {
    source_identity();
    dispatch_cases();
    puts("PASS stem loader: matched source snapshots, four decks, async/inline dispatch, decode failure, stale load retirement and allocation refusal");
}

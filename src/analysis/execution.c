/* SPDX-License-Identifier: GPL-3.0-only */
#include "internal.h"
#include <pthread.h>

static int configuration_error(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return TA_FAILED;
}
static unsigned thread_count(const char *value)
{
    return value && value[0] >= '1' && value[0] <= '3' && !value[1] ? (unsigned)(value[0] - '0') : 0;
}
static bool concurrent_mode(const char *mode, bool phrases, bool vocals)
{
    return mode && !strcmp(mode, "concurrent") && phrases && vocals;
}
static bool invalid_override(bool needed, const char *value, unsigned *threads)
{
    return needed && value && !(*threads = thread_count(value));
}

static bool invalid_mode(const char *mode)
{
    return mode && strcmp(mode, "sequential") && strcmp(mode, "concurrent");
}
int ta_execution_environment(uint32_t needed, ta_execution *out, char *error, size_t size)
{
    const char *mode = getenv("TRACK_ANALYSIS_STAGE_MODE");
    if (invalid_mode(mode))
        return configuration_error(error, size, "invalidStageMode");
    bool phrases = (needed & AZ_DATA_PHRASES) != 0, vocals = (needed & AZ_ANALYSIS_VOCALS) != 0;
    *out = (ta_execution){ .mode = concurrent_mode(mode, phrases, vocals)
                                       ? TA_STAGES_CONCURRENT
                                       : TA_STAGES_SEQUENTIAL,
                           .phrase_threads = 1,
                           .vocal_threads = 1 };
    if (out->mode == TA_STAGES_CONCURRENT) out->vocal_threads = 2;
    const char *phrase = getenv("TRACK_ANALYSIS_PHRASE_THREADS"),
               *vocal = getenv("TRACK_ANALYSIS_PVDI_THREADS");
    if (invalid_override(phrases, phrase, &out->phrase_threads))
        return configuration_error(error, size, "invalidThreadCount");
    if (invalid_override(vocals, vocal, &out->vocal_threads))
        return configuration_error(error, size, "invalidVocalThreadCount");
    if (out->mode == TA_STAGES_CONCURRENT && out->phrase_threads + out->vocal_threads > 3)
        return configuration_error(error, size, "concurrentThreadBudgetExceeded");
    return TA_OK;
}

typedef struct ta_group ta_group;
typedef struct {
    ta_context context;
    ta_group *group;
    unsigned slot;
} ta_worker;
struct ta_group {
    atomic_size_t next;
    atomic_bool failed;
    size_t count;
    bool fixed;
    ta_parallel_fn body;
    void *job;
    ta_worker workers[3];
};

static void *run(void *argument)
{
    ta_worker *worker = argument;
    ta_group *group = worker->group;
    ta_context *c = &worker->context;
    /* A failure jumps only within this worker, then every child is joined. */
    if (setjmp(c->failure)) {
        atomic_store_explicit(&group->failed, true, memory_order_relaxed);
        return NULL;
    }
    if (group->fixed) {
        ta_check(c);
        group->body(c, group->job, worker->slot, worker->slot);
    } else {
        while (!atomic_load_explicit(&group->failed, memory_order_relaxed)) {
            size_t index = atomic_fetch_add_explicit(&group->next, 1, memory_order_relaxed);
            if (index >= group->count) break;
            ta_check(c);
            group->body(c, group->job, index, worker->slot);
        }
    }
    return NULL;
}
static int start_and_join(ta_group *group, unsigned threads)
{
    pthread_t children[2];
    unsigned started = 0;
    int failure = 0;
    for (unsigned i = 1; i < threads; i++) {
        /* Linux inherits the caller's A53 affinity and nice value. */
        failure = pthread_create(&children[started], NULL, run, &group->workers[i]);
        if (failure) {
            atomic_store_explicit(&group->failed, true, memory_order_relaxed);
            break;
        }
        started++;
    }
    if (!failure) run(&group->workers[0]);
    for (unsigned i = 0; i < started; i++)
        if (pthread_join(children[i], NULL)) abort();
    return failure;
}

static int collect_workers(ta_context *c, ta_group *group, unsigned threads, int failure, const char **error)
{
    int status = failure ? TA_FAILED : TA_OK;
    *error = failure ? "thread creation failed" : NULL;
    for (unsigned i = 0; i < threads; i++) {
        ta_context *child = &group->workers[i].context;
        if (child->status && !status) {
            status = child->status;
            *error = child->error;
        }
        if (child->decoder) {
            ta_decode_close(child->decoder);
            child->decoder = NULL;
        }
        ta_adopt(c, child);
    }
    return status;
}

static void execute_serial(ta_context *c, size_t count, ta_parallel_fn body, void *job)
{
    for (size_t index = 0; index < count; index++) {
        ta_check(c);
        body(c, job, index, 0);
    }
}

static ta_group *create_group(ta_context *c, size_t count, unsigned threads, bool fixed, ta_parallel_fn body, void *job)
{
    ta_share_budget(c);
    ta_group *group = ta_alloc(c, 1, sizeof *group);
    group->count = count;
    group->fixed = fixed;
    group->body = body;
    group->job = job;
    atomic_init(&group->next, 0);
    atomic_init(&group->failed, false);
    for (unsigned i = 0; i < threads; i++) {
        group->workers[i].group = group;
        group->workers[i].slot = i;
        group->workers[i].context.request = c->request;
        group->workers[i].context.execution = c->execution;
        group->workers[i].context.budget = c->budget;
    }
    return group;
}

static void execute(ta_context *c, size_t count, unsigned threads, bool fixed,
                    ta_parallel_fn body, void *job)
{
    if (!threads || threads > 3 || !body) ta_fail(c, TA_FAILED, "invalid thread count");
    if (!count) return;
    if (threads > count) threads = (unsigned)count;
    if (threads == 1) {
        execute_serial(c, count, body, job);
        return;
    }
    ta_group *group = create_group(c, count, threads, fixed, body, job);
    int failure = start_and_join(group, threads);
    const char *error;
    int status = collect_workers(c, group, threads, failure, &error);
    ta_release(c, group);
    if (status) ta_fail(c, status, error);
    ta_check(c);
}
void ta_parallel(ta_context *c, size_t count, unsigned threads, ta_parallel_fn body, void *job)
{
    execute(c, count, threads, false, body, job);
}
void ta_pair(ta_context *c, ta_parallel_fn body, void *job)
{
    execute(c, 2, 2, true, body, job);
}
void ta_profile_begin(ta_context *c)
{
    const char *enabled = getenv("TRACK_ANALYSIS_PHRASE_PROFILE");
    c->phrase_profile = enabled && !strcmp(enabled, "1");
    clock_gettime(CLOCK_MONOTONIC, &c->profile_previous);
}
void ta_profile_mark(ta_context *c, const char *stage)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec - c->profile_previous.tv_sec) +
                     (double)(now.tv_nsec - c->profile_previous.tv_nsec) * 1e-9;
    c->profile_previous = now;
    if (c->phrase_profile) fprintf(stderr, "PHRASE stage=%s elapsed_s=%.9f\n", stage, elapsed);
    const ta_diagnostics *d = c->request ? c->request->diagnostics : NULL;
    if (d && d->profile) d->profile(d->user, stage, elapsed);
}

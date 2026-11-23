/* SPDX-License-Identifier: GPL-3.0-only */
#include "internal.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
/* Exercise creation failure through the real executor without a production hook. */
static atomic_int live_threads;
static atomic_int create_calls;
static int fail_create;
struct start {
    void *(*fn)(void *);
    void *arg;
};
static void *tracked_start(void *arg)
{
    struct start start = *(struct start *)arg;
    free(arg);
    atomic_fetch_add(&live_threads, 1);
    void *result = start.fn(start.arg);
    atomic_fetch_sub(&live_threads, 1);
    return result;
}
static int test_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    if (atomic_fetch_add(&create_calls, 1) + 1 == fail_create) return 11;
    struct start *start = malloc(sizeof *start);
    assert(start);
    *start = (struct start){ fn, arg };
    int status = pthread_create(thread, attr, tracked_start, start);
    if (status) free(start);
    return status;
}
#define az_analysis_thread_create test_create
#include "../../src/analysis/execution.c"
#undef az_analysis_thread_create
void ta_decode_close(ta_decoder *decoder)
{
    (void)decoder;
    assert(!"unexpected decoder");
}

typedef struct {
    atomic_uint entered, finished;
    unsigned threads;
    pthread_t parent;
    bool pair;
    int *values[67];
} probe;
static void task(ta_context *c, void *data, size_t index, unsigned worker)
{
    probe *p = data;
    if (p->pair) assert((pthread_equal(pthread_self(), p->parent) != 0) == (index == 0));
    if (index < p->threads) {
        assert(worker < p->threads);
        atomic_fetch_add(&p->entered, 1);
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (atomic_load(&p->entered) < p->threads) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            assert(now.tv_sec - start.tv_sec < 10);
            sched_yield();
        }
    }
    p->values[index] = ta_alloc(c, 1, sizeof(int));
    *p->values[index] = (int)index + 101;
    atomic_fetch_add(&p->finished, 1);
}
static void fail_task(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)data;
    (void)index;
    (void)worker;
    ta_alloc(c, 7, sizeof(double));
    ta_fail(c, TA_FAILED, "deliberate failure");
}
static void limit_task(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)data;
    (void)index;
    (void)worker;
    ta_alloc(c, TA_MEMORY_LIMIT, 2);
}
static int cancelled(void *data)
{
    (void)data;
    return 1;
}
typedef struct {
    atomic_uint arrived;
    atomic_size_t reserved;
} memory_probe;
static void aggregate_limit(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)worker;
    memory_probe *p = data;
    if (index == 0) {
        /* Account for another stage's live allocations without allocating
         * half a gigabyte merely to exercise the shared-budget branch. */
        size_t reserved = TA_MEMORY_LIMIT - atomic_load(&c->budget->used) - 64;
        atomic_store(&p->reserved, reserved);
        atomic_fetch_add(&c->budget->used, reserved);
        atomic_store(&p->arrived, 1);
    } else {
        while (!atomic_load(&p->arrived)) sched_yield();
    }
    ta_alloc(c, 128, 1);
    assert(!"aggregate budget not enforced");
}
static void nested_leaf(ta_context *c, void *data, size_t index, unsigned worker)
{
    task(c, data, index + 1, worker + 1);
}
static void nested_task(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)worker;
    probe *p = data;
    if (index == 0) task(c, p, 0, 0);
    else ta_parallel(c, 2, 2, nested_leaf, p);
}
static void check_clean(ta_context *c)
{
    ta_cleanup(c);
    assert(!c->memory && !c->allocations && !c->decoder);
    assert(!c->budget || !atomic_load(&c->budget->used));
    assert(!atomic_load(&live_threads));
}
static void failure_check(ta_parallel_fn body, int expected, int create_failure, bool cancel)
{
    ta_request request = { .cancelled = cancel ? cancelled : NULL };
    ta_context *volatile c = calloc(1, sizeof *c);
    c->request = &request;
    fail_create = create_failure;
    create_calls = 0;
    if (!setjmp(c->failure)) {
        ta_parallel(c, 67, 3, body, NULL);
        assert(!"failure was lost");
    }
    assert(c->status == expected);
    check_clean(c);
    free(c);
}
static void configuration_checks(void)
{
    ta_execution execution;
    char error[100];
    uint32_t both = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS;
    unsetenv("TRACK_ANALYSIS_STAGE_MODE");
    unsetenv("TRACK_ANALYSIS_PHRASE_THREADS");
    unsetenv("TRACK_ANALYSIS_PVDI_THREADS");
    assert(!ta_execution_environment(both, &execution, error, sizeof error));
    assert(execution.mode == TA_STAGES_CONCURRENT && execution.phrase_threads == 1 && execution.vocal_threads == 2);
    assert(!ta_execution_environment(AZ_DATA_PHRASES, &execution, error, sizeof error));
    assert(execution.mode == TA_STAGES_SEQUENTIAL && execution.phrase_threads == 1);
    assert(!ta_execution_environment(AZ_ANALYSIS_VOCALS, &execution, error, sizeof error));
    assert(execution.mode == TA_STAGES_SEQUENTIAL && execution.vocal_threads == 1);
    setenv("TRACK_ANALYSIS_STAGE_MODE", "sequential", 1);
    assert(!ta_execution_environment(both, &execution, error, sizeof error));
    assert(execution.mode == TA_STAGES_SEQUENTIAL && execution.phrase_threads == 1 && execution.vocal_threads == 1);
    setenv("TRACK_ANALYSIS_STAGE_MODE", "concurrent", 1);
    assert(!ta_execution_environment(both, &execution, error, sizeof error));
    assert(execution.mode == TA_STAGES_CONCURRENT && execution.phrase_threads == 1 && execution.vocal_threads == 2);
    setenv("TRACK_ANALYSIS_PHRASE_THREADS", "2", 1);
    assert(ta_execution_environment(both, &execution, error, sizeof error) == TA_FAILED);
    assert(!strcmp(error, "concurrentThreadBudgetExceeded"));
    setenv("TRACK_ANALYSIS_PVDI_THREADS", "1", 1);
    assert(!ta_execution_environment(both, &execution, error, sizeof error));
    const char *invalid[] = { "", "0", "4", "-1", "2x", " 2" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("TRACK_ANALYSIS_PHRASE_THREADS", invalid[i], 1);
        assert(ta_execution_environment(both, &execution, error, sizeof error) == TA_FAILED);
        assert(!strcmp(error, "invalidThreadCount"));
    }
    unsetenv("TRACK_ANALYSIS_PHRASE_THREADS");
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("TRACK_ANALYSIS_PVDI_THREADS", invalid[i], 1);
        assert(ta_execution_environment(both, &execution, error, sizeof error) == TA_FAILED);
        assert(!strcmp(error, "invalidVocalThreadCount"));
    }
    assert(!ta_execution_environment(AZ_DATA_PHRASES, &execution, error, sizeof error));
    setenv("TRACK_ANALYSIS_STAGE_MODE", "yes", 1);
    assert(ta_execution_environment(both, &execution, error, sizeof error) == TA_FAILED);
    assert(!strcmp(error, "invalidStageMode"));
    unsetenv("TRACK_ANALYSIS_STAGE_MODE");
    unsetenv("TRACK_ANALYSIS_PVDI_THREADS");
}
int main(void)
{
    configuration_checks();
    for (unsigned threads = 1; threads <= 3; threads++) {
        probe p = { .threads = threads };
        ta_context *volatile c = calloc(1, sizeof *c);
        assert(!setjmp(c->failure));
        ta_parallel(c, 67, threads, task, &p);
        assert(atomic_load(&p.entered) == threads && atomic_load(&p.finished) == 67);
        for (int i = 0; i < 67; i++) {
            assert(*p.values[i] == i + 101);
            ta_release(c, p.values[i]);
        }
        check_clean(c);
        free(c);
    }
    probe pair = { .threads = 2, .parent = pthread_self(), .pair = true };
    ta_context *volatile c = calloc(1, sizeof *c);
    assert(!setjmp(c->failure));
    ta_pair(c, task, &pair);
    assert(atomic_load(&pair.finished) == 2);
    check_clean(c);
    free(c);
    /* One primary plus two vocal workers must overlap, including the caller. */
    probe nested = { .threads = 3 };
    c = calloc(1, sizeof *c);
    assert(!setjmp(c->failure));
    ta_pair(c, nested_task, &nested);
    assert(atomic_load(&nested.finished) == 3);
    check_clean(c);
    free(c);
    memory_probe *memory = calloc(1, sizeof *memory);
    c = calloc(1, sizeof *c);
    if (!setjmp(c->failure)) {
        ta_pair(c, aggregate_limit, memory);
        assert(!"expected aggregate memory failure");
    }
    assert(c->status == TA_LIMIT);
    assert(atomic_load(&memory->reserved) > 0);
    atomic_fetch_sub(&c->budget->used, atomic_load(&memory->reserved));
    free(memory);
    check_clean(c);
    free(c);
    failure_check(fail_task, TA_FAILED, 0, false);
    failure_check(limit_task, TA_LIMIT, 0, false);
    failure_check(fail_task, TA_FAILED, 1, false);
    failure_check(fail_task, TA_FAILED, 2, false);
    failure_check(fail_task, TA_CANCELLED, 0, true);
    puts("Concurrent execution, caller ownership, ordered slots, joined failures, cancellation and configuration passed");
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact SDK thread-start implementation with host identity and pthread failure seams. */
#define _GNU_SOURCE
#include "analysis_source.h"
#include "sdk_logic.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CM_MAX 2
#define CM_RA() ((uintptr_t) __builtin_return_address(0))
typedef struct { uintptr_t lo, hi; int running; char id[16]; } cm_mod;
static cm_mod g_cm[CM_MAX];
static _Thread_local int g_cm_on_ui;
static int g_cm_loading = -1, g_cm_in_frame, fail_alloc, fail_create;
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *service) {
    assert(caller && !strcmp(service, "thread_start"));
    return id && !strcmp(id, g_cm[0].id) ? &g_cm[0] : NULL;
}
static int cm_running(const cm_mod *mod) { return mod->running; }
static void cm_logf(cm_mod *mod, int level, const char *format, ...) {
    (void) mod; (void) level; (void) format;
}
static void *thread_allocate(size_t size) { return fail_alloc ? NULL : malloc(size); }
static int thread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*entry)(void *), void *argument) {
    int policy, inherit, detached;
    struct sched_param priority;
    assert(!pthread_attr_getschedpolicy(attributes, &policy) && policy == SCHED_OTHER);
    assert(!pthread_attr_getinheritsched(attributes, &inherit) && inherit == PTHREAD_EXPLICIT_SCHED);
    assert(!pthread_attr_getschedparam(attributes, &priority) && priority.sched_priority == 0);
    assert(!pthread_attr_getdetachstate(attributes, &detached) && detached == PTHREAD_CREATE_DETACHED);
    return fail_create ? EAGAIN : pthread_create(thread, attributes, entry, argument);
}
#ifdef __APPLE__
static int thread_name(pthread_t thread, const char *name) { (void) thread; return pthread_setname_np(name); }
#define pthread_setname_np thread_name
#endif
#define malloc thread_allocate
#define az_thread_create thread_create
#include THREAD_INC
#undef malloc
#undef az_thread_create
#ifdef __APPLE__
#undef pthread_setname_np
#endif
struct run { az_analysis_owner expected; _Atomic int entered, resume, finished; int spawn; };
static void *entry(void *opaque) {
    struct run *run = opaque;
    az_analysis_owner bound = analysis_owner_for_caller(0);
    assert(bound.generation == run->expected.generation && bound.kind == run->expected.kind);
    atomic_store(&run->entered, 1);
    while (!atomic_load(&run->resume)) usleep(1000);
    if (run->spawn) {
        struct run child = {.expected = run->expected, .resume = 1};
        assert(sd_thread_start("mod-a", "nested", entry, &child) == 0);
        while (!atomic_load(&child.finished)) usleep(1000);
        assert(!analysis_owner_current(bound));
    }
    atomic_store(&run->finished, 1);
    return NULL;
}
static void wait_for(_Atomic int *flag) {
    static unsigned ordinal;
    ++ordinal;
    for (unsigned i = 0; i < 3000 && !atomic_load(flag); ++i) usleep(1000);
    if (!atomic_load(flag)) fprintf(stderr, "thread phase %u timed out, active=%d\n", ordinal, sd_threads_of(0));
    assert(atomic_load(flag));
}
static void wait_empty(void) {
    for (unsigned i = 0; i < 3000 && sd_threads_of(0); ++i) usleep(1000);
    assert(!sd_threads_of(0));
}
int main(void) {
    alarm(15);
    snprintf(g_cm[0].id, sizeof g_cm[0].id, "mod-a");
    g_cm[0].lo = (uintptr_t) entry;
    g_cm[0].hi = (uintptr_t) entry + 1;
    g_cm_on_ui = 1;
    az_analysis_owner first = analysis_owner_begin(0);
    struct run run = {.expected = first, .resume = 1};
    assert(sd_thread_start("other", "invalid", entry, &run) == -1);
    assert(sd_thread_start("mod-a", "invalid", NULL, &run) == -1);
    assert(sd_thread_start("mod-a", "stopped", entry, &run) == -1);
    g_cm_loading = 0;
    assert(sd_thread_start("mod-a", NULL, entry, &run) == 0);
    wait_for(&run.finished); wait_empty();
    g_cm_loading = -1; g_cm[0].running = 1;
    g_cm_in_frame = 1;
    assert(sd_thread_start("mod-a", "frame", entry, &run) == -1);
    g_cm_in_frame = 0;
    assert(sd_thread_start("mod-a", "foreign", (void *(*)(void *))(uintptr_t) 1, &run) == -1);
    fail_alloc = 1;
    assert(sd_thread_start("mod-a", "alloc", entry, &run) == -1 && !sd_threads_of(0));
    fail_alloc = 0; fail_create = 1;
    assert(sd_thread_start("mod-a", "pthread", entry, &run) == -1 && !sd_threads_of(0));
    fail_create = 0;
    struct run blocked[4] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        blocked[i].expected = first;
        assert(sd_thread_start("mod-a", i ? "quota" : "", entry, &blocked[i]) == 0);
        wait_for(&blocked[i].entered);
    }
    assert(sd_threads_of(0) == 4 && sd_thread_start("mod-a", "overflow", entry, &run) == -1);
    for (unsigned i = 0; i < 4; ++i) atomic_store(&blocked[i].resume, 1);
    wait_empty();
    struct run retiring = {.expected = first, .spawn = 1};
    assert(sd_thread_start("mod-a", "old-worker", entry, &retiring) == 0);
    wait_for(&retiring.entered);
    analysis_owner_retire(0);
    az_analysis_owner next = analysis_owner_begin(0);
    assert(next.generation != first.generation);
    atomic_store(&retiring.resume, 1);
    wait_for(&retiring.finished); wait_empty();
    /* A raw/unbound worker never inherits the current generation through its code address. */
    g_cm_on_ui = 0;
    struct run unbound = {.resume = 1};
    assert(sd_thread_start("mod-a", "unbound", entry, &unbound) == 0);
    wait_for(&unbound.finished); wait_empty();
    g_cm_on_ui = 1;
    struct run current = {.expected = next, .resume = 1};
    assert(sd_thread_start("mod-a", "new-worker", entry, &current) == 0);
    wait_for(&current.finished); wait_empty();
    puts("SDK thread start: real callbacks, admission/quota failures, payload cleanup and epoch inheritance passed");
}

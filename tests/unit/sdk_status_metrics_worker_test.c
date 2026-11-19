/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "sdk_status_engine.c"
static void ds_wake(void) {}
static void ds_deadline(uint64_t ns) { (void) ns; }
static int ds_live(int owner) { return owner >= 0; }
static void ds_gui_interest(uint64_t topics) { (void) topics; }
static void ds_native_interest(unsigned deck, uint64_t topics) {
    (void) deck;
    (void) topics;
}
static void sdk_status_store_interest(unsigned deck, uint64_t topics) {
    (void) deck;
    (void) topics;
}
static void sdk_status_store_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t cut) {
    (void) deck;
    (void) epoch;
    (void) ref;
    (void) cut;
}
#define az_thread_create pthread_create
#include "sdk_status_metrics.c"
static pthread_t worker;
static unsigned creates, sleeps;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*) (void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*entry)(void *), void *arg) {
    int detached, inheritance, policy;
    struct sched_param priority;
    assert(!pthread_attr_getdetachstate(attributes, &detached) && detached == PTHREAD_CREATE_DETACHED);
    assert(!pthread_attr_getinheritsched(attributes, &inheritance) && inheritance == PTHREAD_EXPLICIT_SCHED);
    assert(!pthread_attr_getschedpolicy(attributes, &policy) && policy == SCHED_OTHER);
    assert(!pthread_attr_getschedparam(attributes, &priority) && !priority.sched_priority);
    pthread_attr_t joinable;
    assert(!pthread_attr_init(&joinable));
    assert(!pthread_attr_setinheritsched(&joinable, inheritance));
    assert(!pthread_attr_setschedpolicy(&joinable, policy));
    assert(!pthread_attr_setschedparam(&joinable, &priority));
    /* altstack-ok: test forwarding preserves the production entry and policy. */
    int rc = __real_pthread_create(thread, &joinable, entry, arg);
    assert(!pthread_attr_destroy(&joinable));
    worker = *thread;
    ++creates;
    return rc;
}
int __wrap_nanosleep(const struct timespec *requested, struct timespec *remaining) {
    assert(requested->tv_sec == 1 && !requested->tv_nsec);
    ++sleeps;
    if (sleeps == 1) {
        *remaining = *requested;
        errno = EINTR;
        return -1;
    }
    if (sleeps == 2) {
        pthread_mutex_lock(&ds.mu);
        ++ds.metrics[0].callbacks;
        pthread_mutex_unlock(&ds.mu);
        return 0;
    }
    pthread_exit(NULL);
}
int main(void) {
    ds_start_metrics();
    assert(!creates && !ds_proofs.enabled);
    ds.metrics[0] = (struct ds_metrics) {.capture_gaps = 5, .revision_losses = 7, .resyncs = 11, .callbacks = 37};
    ds.deck[0].current.sequence = 13;
    int marker = open(CM_ROOT "/azmod.deck-status.metrics", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(marker >= 0 && !close(marker));
    ds_start_metrics();
    assert(creates == 1 && !pthread_join(worker, NULL) && sleeps == 3);
    assert(ds_proofs.enabled && atomic_load(&ds_proof_enabled));
    FILE *file = fopen(DS_METRICS_PATH, "re");
    assert(file);
    char line[4096];
    unsigned enabled = 0, baseline = 0, fresh = 0;
    while (fgets(line, sizeof line, file)) {
        if (strstr(line, "provenance_enabled=1")) ++enabled;
        if (strstr(line, "provenance_base_deck=0 capture_gap_cuts=5 revision_loss_cuts=7 resyncs=11 sequence=13")) ++baseline;
        if (strstr(line, "deck=0 sequence=13 ") && strstr(line, "callbacks=38 ")) ++fresh;
    }
    assert(!fclose(file) && enabled == 1 && baseline == 1 && fresh == 1);
    assert(!unlink(CM_ROOT "/azmod.deck-status.metrics") && !unlink(DS_METRICS_PATH));
    puts("PASS metrics opt-in starts an ordinary-priority worker, retries interrupted sleep, retains baseline counters and atomically replaces the next snapshot");
}

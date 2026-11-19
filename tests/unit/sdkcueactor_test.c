/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcueactor.h"
#include "../../src/azmod/azmod-mod.h"
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <stdint.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>

struct Task {
    uintptr_t vptr;
    uint32_t token, padding;
    uint64_t number;
    void *manager;
    uint64_t padding2;
};
static uintptr_t manager_table[2], box_table[7], owner_table[16], task_table[4];
static uintptr_t manager[64], replacement[64], owner[4];
static uint64_t (*post_entry)(void *, void *);
static void (*exec_entry)(void *), (*dtor_entry)(void *);
static struct Task *tasks[1024];
static unsigned task_count;
static unsigned native_saves, executions, destroyed;
static _Atomic(bool) block_execute = false, entered = false, unblock = false;
static _Atomic(unsigned) body_reentry = 0;
static _Atomic(int) reentry_result = AZ_FAILED;
static void probe_reentry();
static int callback_result = AZ_OK;
static bool async_needed = true, fail_post, inline_post, fail_allocate, pending;
static bool needs(void *unused __attribute__((unused))) { return async_needed; }
static void native_exec(void *unused __attribute__((unused))) {
    ++native_saves;
    if (atomic_load_explicit(&(body_reentry), memory_order_seq_cst) == 1) probe_reentry();
}
static void native_dtor(void *unused __attribute__((unused))) {
    ++destroyed;
    if (atomic_load_explicit(&(body_reentry), memory_order_seq_cst) == 2) probe_reentry();
}
static uint64_t native_post(void *unused __attribute__((unused)), void *p) {
    struct Task *t = ((struct Task *) (p));
    if (fail_post) {
        dtor_entry(t);
        free(t);
        return 0;
    }
    if (inline_post) {
        exec_entry(t);
        dtor_entry(t);
        free(t);
        return 1;
    }
    assert(task_count < 1024);
    tasks[task_count++] = t;
    return 1;
}
static void producer(void *m) {
    if (!async_needed) {
        ++native_saves;
        return;
    }
    if (fail_allocate) return;
    struct Task *t = calloc(1, sizeof(struct Task));
    t->vptr = (uintptr_t) (task_table);
    t->manager = m;
    post_entry(((char *) (m)) + 8, t);
}
static void *install(uintptr_t address, void *hook, void **original) {
    *original = ((void *) (address));
    if (address == (uintptr_t) (native_post)) post_entry = ((uint64_t (*)(void *, void *))(hook));
    else if (address == (uintptr_t) (native_exec)) exec_entry = ((void (*)(void *))(hook));
    else if (address == (uintptr_t) (native_dtor)) dtor_entry = ((void (*)(void *))(hook));
    else assert(false);
    return hook;
}
static bool load_current = true;
static int delivery_claim = AZ_OK;
int azsdk_exit_delivery_claim(const struct azexit_request *request, uint64_t clock) {
    assert(request && clock);
    return delivery_claim;
}
static int current(unsigned deck __attribute__((unused)), uint64_t generation, void *context __attribute__((unused))) { return load_current && generation == 42; }
static int execute(const struct azexit_cue_request *request, void *m, struct azexit_result *out, void *context __attribute__((unused))) {
    assert(m == manager || m == replacement);
    ++executions;
    if (atomic_load_explicit(&(block_execute), memory_order_seq_cst)) {
        atomic_store_explicit(&(entered), true, memory_order_seq_cst);
        while (!atomic_load_explicit(&(unblock), memory_order_seq_cst)) sched_yield();
    }
    out->token = request->identity.token;
    out->load_generation = request->identity.load_generation;
    out->outcome = AZ_COMMAND_APPLIED;
    out->reason = AZ_OK;
    return pending ? AZ_PENDING : callback_result;
}
static struct azexit_cue_request request(uint64_t token) {
    struct azexit_cue_request r = {0};
    r.size = sizeof r;
    r.identity.token = token;
    r.identity.load_generation = 42;
    r.identity.deadline_ns = UINT64_MAX;
    r.operation = AZ_CMD_HOT_SET;
    return r;
}
static void *reentry_worker(void *unused __attribute__((unused))) {
    struct azexit_cue_request r = request(9000);
    r.identity.deck = 1;
    atomic_store(&reentry_result, azsdk_cue_actor_enqueue(&r));
    return NULL;
}
static void probe_reentry(void) {
    pthread_t foreign;
    assert(pthread_create(&foreign, NULL, reentry_worker, NULL) == 0);
    assert(pthread_join(foreign, NULL) == 0);
}
static struct azexit_result poll(uint64_t token) {
    struct azexit_result r = {0};
    assert(azsdk_cue_actor_poll(0, &r) == AZ_OK);
    assert(r.token == token);
    return r;
}
static void release(uint64_t token) { assert(azsdk_cue_actor_release(0, token) == AZ_OK); }
static void drain() {
    struct Task *queue[1024];
    unsigned count = task_count;
    memcpy(queue, tasks, count * sizeof(*queue));
    task_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct Task *t = queue[i];
        exec_entry(t);
        dtor_entry(t);
        free(t);
    }
}
static _Atomic(bool) posted, resume_returned;
static _Atomic(int) resume_result = AZ_PENDING;
static void *drain_worker(void *unused __attribute__((unused))) {
    drain();
    return NULL;
}
static void *receipt_worker(void *unused __attribute__((unused))) {
    atomic_store(&resume_result, azsdk_cue_actor_resume(0, 11));
    atomic_store(&resume_returned, true);
    return NULL;
}
static void *post_worker(void *context) {
    assert(post_entry((char *) replacement + 8, context) == 1);
    atomic_store(&posted, true);
    return NULL;
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t) ts.tv_sec * UINT64_C(1000000000) + (uint64_t) ts.tv_nsec;
}
int main(void) {
    box_table[5] = (uintptr_t) (needs);
    manager[0] = replacement[0] = (uintptr_t) (manager_table);
    manager[1] = replacement[1] = (uintptr_t) (box_table);
    owner[0] = (uintptr_t) (owner_table);
    manager[6] = replacement[6] = (uintptr_t) (owner); // manager+8 box+0x28
    struct azcue_actor_profile p = {(uintptr_t) (manager_table), (uintptr_t) (box_table), (uintptr_t) (owner_table), (uintptr_t) (task_table),
                                    (uintptr_t) (producer), (uintptr_t) (native_post), (uintptr_t) (native_exec), (uintptr_t) (native_dtor)};
    struct azcue_actor_callbacks cb = {execute, current, NULL};
    assert(azsdk_cue_actor_configure(&p, install, &cb) == AZ_OK);
    assert(azsdk_cue_actor_register(0, manager, 1) == AZ_OK);
    __auto_type r = request(1);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    r.operation = AZ_CMD_MEMORY_DELETE; // producer must receive actor-owned copy
    azsdk_cue_actor_drive(0);
    drain();
    assert(poll(1).outcome == AZ_COMMAND_APPLIED);
    release(1);
    assert(executions == 1 && native_saves == 0);
    r = request(2);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(azsdk_cue_actor_cancel(0, 2) == AZ_OK);
    release(2);
    r = request(3);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    drain();
    assert(executions == 2 && native_saves == 0);
    poll(3);
    release(3);
    r = request(4);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    azsdk_cue_actor_retire(0, manager);
    assert(poll(4).reason == AZ_STALE);
    release(4);
    assert(azsdk_cue_actor_register(0, replacement, 2) == AZ_OK);
    azsdk_cue_actor_retire(0, manager); // stale destructor must not retire replacement
    drain();
    assert(executions == 2 && native_saves == 0);
    async_needed = false;
    r = request(5);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(5).reason == AZ_UNSUPPORTED);
    release(5);
    assert(native_saves == 0);
    async_needed = true;
    fail_post = true;
    r = request(6);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(6).reason == AZ_CANCELLED);
    release(6);
    fail_post = false;
    inline_post = true;
    r = request(7);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    poll(7);
    release(7);
    inline_post = false;
    fail_allocate = true;
    r = request(8);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(8).reason == AZ_UNSUPPORTED);
    release(8);
    fail_allocate = false;
    pending = true;
    r = request(9);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    drain();
    struct azexit_result result = {0};
    result.token = 9;
    result.load_generation = 42;
    result.outcome = AZ_COMMAND_APPLIED;
    assert(azsdk_cue_actor_complete(0, 9, &result) == AZ_OK);
    poll(9);
    release(9);
    pending = false;
    pending = true;
    r = request(10);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    drain();
    assert(azsdk_cue_actor_resume(0, 10) == AZ_OK);
    pending = false;
    azsdk_cue_actor_drive(0);
    drain();
    poll(10);
    release(10);
    // Public plugin endpoints remain bounded while the native owner is working.
    r = request(11);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    atomic_store_explicit(&(block_execute), true, memory_order_seq_cst);
    pthread_t owner_worker;
    assert(pthread_create(&owner_worker, NULL, drain_worker, NULL) == 0);
    while (!atomic_load_explicit(&(entered), memory_order_seq_cst)) sched_yield();
    __auto_type busy = request(12);
    assert(azsdk_cue_actor_enqueue(&busy) == AZ_BUSY);
    assert(azsdk_cue_actor_cancel(0, 11) == AZ_PENDING);
    assert(azsdk_cue_actor_poll(0, &result) == AZ_PENDING);
    assert(azsdk_cue_actor_release(0, 11) == AZ_BUSY);
    struct Task *foreign_task = calloc(1, sizeof(struct Task));
    foreign_task->vptr = (uintptr_t) (task_table);
    foreign_task->manager = replacement;
    pthread_t receipt_notifier, foreign_poster;
    assert(pthread_create(&receipt_notifier, NULL, receipt_worker, NULL) == 0);
    assert(pthread_create(&foreign_poster, NULL, post_worker, foreign_task) == 0);
    uint64_t limit = monotonic_ns() + UINT64_C(500000000);
    while ((!atomic_load(&posted) || !atomic_load(&resume_returned)) && monotonic_ns() < limit) sched_yield();
    bool unblocked_stock_post = atomic_load_explicit(&(posted), memory_order_seq_cst);
    bool unblocked_receipt = atomic_load_explicit(&(resume_returned), memory_order_seq_cst);
    atomic_store_explicit(&(unblock), true, memory_order_seq_cst);
    assert(pthread_join(foreign_poster, NULL) == 0);
    assert(pthread_join(receipt_notifier, NULL) == 0);
    assert(pthread_join(owner_worker, NULL) == 0);
    assert(unblocked_stock_post);
    assert(unblocked_receipt && atomic_load_explicit(&(resume_result), memory_order_seq_cst) == AZ_BUSY);
    drain(); // The untagged foreign task still performs the original native save.

    atomic_store_explicit(&(block_execute), false, memory_order_seq_cst);
    poll(11);
    release(11);
    // A failed continuation cannot erase the effects of already dispatched work.
    pending = true;
    r = request(13);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    drain();
    pending = false;
    callback_result = AZ_BUSY;
    assert(azsdk_cue_actor_resume(0, 13) == AZ_OK);
    azsdk_cue_actor_drive(0);
    drain();
    assert(poll(13).outcome == AZ_COMMAND_UNCERTAIN);
    release(13);
    callback_result = AZ_OK;
    pending = true;
    r = request(14);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    drain();
    azsdk_cue_actor_retire(0, replacement);
    assert(poll(14).outcome == AZ_COMMAND_UNCERTAIN);
    release(14);
    assert(azsdk_cue_actor_register(0, replacement, 3) == AZ_OK);
    pending = false;
    // Cancel/release never reclaims a native envelope, even across SDK requests.
    for (unsigned i = 0; i < 512; ++i) {
        r = request(100 + i);
        assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
        azsdk_cue_actor_drive(0);
        assert(azsdk_cue_actor_cancel(0, 100 + i) == AZ_OK);
        release(100 + i);
    }
    r = request(900);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(900).reason == AZ_BUSY);
    release(900);
    unsigned before = executions;
    drain();
    assert(executions == before && native_saves == 1);
    // Untagged physical/native MemoryCue remains unchanged.
    struct Task physical = {0};
    physical.vptr = (uintptr_t) (task_table);
    physical.manager = replacement;
    assert(azsdk_cue_actor_register(1, replacement, 4) == AZ_OK);
    atomic_store_explicit(&(body_reentry), 1, memory_order_seq_cst);
    exec_entry(&physical);
    assert(atomic_load_explicit(&(reentry_result), memory_order_seq_cst) == AZ_PENDING);
    assert(azsdk_cue_actor_cancel(1, 9000) == AZ_OK);
    assert(azsdk_cue_actor_release(1, 9000) == AZ_OK);
    atomic_store_explicit(&(body_reentry), 2, memory_order_seq_cst);
    dtor_entry(&physical);
    assert(atomic_load_explicit(&(reentry_result), memory_order_seq_cst) == AZ_PENDING);
    assert(azsdk_cue_actor_cancel(1, 9000) == AZ_OK);
    assert(azsdk_cue_actor_release(1, 9000) == AZ_OK);
    atomic_store_explicit(&(body_reentry), 0, memory_order_seq_cst);
    azsdk_cue_actor_retire(1, replacement);
    assert(native_saves == 2);
    assert(destroyed == 527);
    // Admission is rechecked at both queue dispatch and native execution.
    unsigned before_executions = executions, before_saves = native_saves;
    r = request(910);
    r.identity.load_generation = 43;
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(910).outcome == AZ_COMMAND_REJECTED && poll(910).reason == AZ_STALE);
    release(910);
    r = request(911);
    r.identity.deadline_ns = 1;
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    assert(poll(911).reason == AZ_STALE);
    release(911);
    r = request(912);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    load_current = false;
    drain();
    assert(poll(912).reason == AZ_STALE);
    release(912);
    load_current = true;
    r = request(913);
    r.identity.deadline_ns = monotonic_ns() + UINT64_C(50000000);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    struct timespec delay = {.tv_nsec = 60000000};
    assert(!nanosleep(&delay, NULL));
    drain();
    assert(poll(913).reason == AZ_STALE);
    release(913);
    r = request(914);
    assert(azsdk_cue_actor_enqueue(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
    delivery_claim = AZ_CANCELLED;
    drain();
    assert(poll(914).outcome == AZ_COMMAND_CANCELLED && poll(914).reason == AZ_CANCELLED);
    release(914);
    assert(executions == before_executions && native_saves == before_saves);
}

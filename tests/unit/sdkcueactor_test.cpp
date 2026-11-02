/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcueactor.h"
#include "../../src/azmod/azmod-mod.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdint>
#include <time.h>
#include <atomic>
#include <thread>
#include <chrono>
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
static std::vector<Task *> tasks;
static unsigned native_saves, executions, destroyed;
static std::atomic<bool> block_execute {false}, entered {false}, unblock {false};
static std::atomic<unsigned> body_reentry {0};
static std::atomic<int> reentry_result {AZ_FAILED};
static void probe_reentry();
static int callback_result = AZ_OK;
static bool async_needed = true, fail_post, inline_post, fail_allocate, pending;
static bool needs(void *) { return async_needed; }
static void native_exec(void *) {
    ++native_saves;
    if (body_reentry.load() == 1) probe_reentry();
}
static void native_dtor(void *) {
    ++destroyed;
    if (body_reentry.load() == 2) probe_reentry();
}
static uint64_t native_post(void *, void *p) {
    Task *t = static_cast<Task *>(p);
    if (fail_post) {
        dtor_entry(t);
        delete t;
        return 0;
    }
    if (inline_post) {
        exec_entry(t);
        dtor_entry(t);
        delete t;
        return 1;
    }
    tasks.push_back(t);
    return 1;
}
static void producer(void *m) {
    if (!async_needed) {
        ++native_saves;
        return;
    }
    if (fail_allocate) return;
    Task *t = new Task();
    t->vptr = uintptr_t(task_table);
    t->manager = m;
    post_entry(static_cast<char *>(m) + 8, t);
}
static void *install(uintptr_t address, void *hook, void **original) {
    *original = reinterpret_cast<void *>(address);
    if (address == uintptr_t(native_post)) post_entry = reinterpret_cast<uint64_t (*)(void *, void *)>(hook);
    else if (address == uintptr_t(native_exec)) exec_entry = reinterpret_cast<void (*)(void *)>(hook);
    else if (address == uintptr_t(native_dtor)) dtor_entry = reinterpret_cast<void (*)(void *)>(hook);
    else assert(false);
    return hook;
}
static int current(unsigned, uint64_t generation, void *) { return generation == 42; }
static int execute(const azexit_cue_request *request, void *m, azexit_result *out, void *) {
    assert(m == manager || m == replacement);
    ++executions;
    if (block_execute.load()) {
        entered.store(true);
        while (!unblock.load()) std::this_thread::yield();
    }
    out->token = request->identity.token;
    out->load_generation = request->identity.load_generation;
    out->outcome = AZ_COMMAND_APPLIED;
    out->reason = AZ_OK;
    return pending ? AZ_PENDING : callback_result;
}
static azexit_cue_request request(uint64_t token) {
    azexit_cue_request r = {};
    r.size = sizeof r;
    r.identity.token = token;
    r.identity.load_generation = 42;
    r.identity.deadline_ns = UINT64_MAX;
    r.operation = AZ_CMD_HOT_SET;
    return r;
}
static void probe_reentry() {
    std::thread foreign([] {
        auto r = request(9000);
        r.identity.deck = 1;
        reentry_result.store(azsdk_cue_actor_enqueue(&r));
    });
    foreign.join();
}
static azexit_result poll(uint64_t token) {
    azexit_result r = {};
    assert(azsdk_cue_actor_poll(0, &r) == AZ_OK);
    assert(r.token == token);
    return r;
}
static void release(uint64_t token) { assert(azsdk_cue_actor_release(0, token) == AZ_OK); }
static void drain() {
    auto queue = tasks;
    tasks.clear();
    for (Task *t : queue) {
        exec_entry(t);
        dtor_entry(t);
        delete t;
    }
}
int main() {
    box_table[5] = uintptr_t(needs);
    manager[0] = replacement[0] = uintptr_t(manager_table);
    manager[1] = replacement[1] = uintptr_t(box_table);
    owner[0] = uintptr_t(owner_table);
    manager[6] = replacement[6] = uintptr_t(owner); // manager+8 box+0x28
    azcue_actor_profile p = {uintptr_t(manager_table), uintptr_t(box_table), uintptr_t(owner_table), uintptr_t(task_table),
                             uintptr_t(producer), uintptr_t(native_post), uintptr_t(native_exec), uintptr_t(native_dtor)};
    azcue_actor_callbacks cb = {execute, current, nullptr};
    assert(azsdk_cue_actor_configure(&p, install, &cb) == AZ_OK);
    assert(azsdk_cue_actor_register(0, manager, 1) == AZ_OK);
    auto r = request(1);
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
    azexit_result result = {};
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
    block_execute.store(true);
    std::thread owner_worker(drain);
    while (!entered.load()) std::this_thread::yield();
    auto busy = request(12);
    assert(azsdk_cue_actor_enqueue(&busy) == AZ_BUSY);
    assert(azsdk_cue_actor_cancel(0, 11) == AZ_PENDING);
    assert(azsdk_cue_actor_poll(0, &result) == AZ_PENDING);
    assert(azsdk_cue_actor_release(0, 11) == AZ_BUSY);
    Task *foreign_task = new Task();
    foreign_task->vptr = uintptr_t(task_table);
    foreign_task->manager = replacement;
    std::atomic<bool> posted {false};
    std::atomic<bool> resume_returned {false};
    std::atomic<int> resume_result {AZ_PENDING};
    std::thread receipt_notifier([&] { resume_result.store(azsdk_cue_actor_resume(0,11)); resume_returned.store(true); });
    std::thread foreign_poster([&] { assert(post_entry(reinterpret_cast<char *>(replacement)+8,foreign_task)==1); posted.store(true); });
    auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while ((!posted.load() || !resume_returned.load()) && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    bool unblocked_stock_post = posted.load();
    bool unblocked_receipt = resume_returned.load();
    unblock.store(true);
    foreign_poster.join();
    receipt_notifier.join();
    owner_worker.join();
    assert(unblocked_stock_post);
    assert(unblocked_receipt && resume_result.load()==AZ_BUSY);
    drain(); // The untagged foreign task still performs the original native save.

    block_execute.store(false);
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
    Task physical = {};
    physical.vptr = uintptr_t(task_table);
    physical.manager = replacement;
    assert(azsdk_cue_actor_register(1, replacement, 4) == AZ_OK);
    body_reentry.store(1);
    exec_entry(&physical);
    assert(reentry_result.load() == AZ_PENDING);
    assert(azsdk_cue_actor_cancel(1, 9000) == AZ_OK);
    assert(azsdk_cue_actor_release(1, 9000) == AZ_OK);
    body_reentry.store(2);
    dtor_entry(&physical);
    assert(reentry_result.load() == AZ_PENDING);
    assert(azsdk_cue_actor_cancel(1, 9000) == AZ_OK);
    assert(azsdk_cue_actor_release(1, 9000) == AZ_OK);
    body_reentry.store(0);
    azsdk_cue_actor_retire(1, replacement);
    assert(native_saves == 2);
    assert(destroyed == 527);
}

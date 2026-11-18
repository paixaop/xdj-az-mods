/* SPDX-License-Identifier: GPL-3.0-only */
#include "azcuerecall.h"
#include "azmod-mod.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#define RAW(size)                              \
    struct {                                   \
        _Alignas(8) unsigned char bytes[size]; \
    }
#define put(p, at, v)                                               \
    do {                                                            \
        __auto_type value = (v);                                    \
        memcpy((unsigned char *) (p) + (at), &value, sizeof value); \
    } while (0)
#define get(type, p, at) ({ type value; memcpy(&value,(const unsigned char *)(p)+(at),sizeof value); value; })
static uintptr_t manager_table[2], loader_table[2], load_table[4], property_table[8], box_table[6];
static RAW(0x180) manager;
static RAW(0x1d0) loader;
static RAW(0x90) load, control;
static RAW(0x200) core;
static RAW(0x400) context;
static RAW(0x10) available;
static unsigned copies, destroys, dispatches, references = 1, submissions;
static bool foreign_thread;
static _Atomic(bool) dispatch_hold = false, dispatch_inside = false, bridge_quiescent = true;
static RAW(0xf8) hot_target;
static unsigned assignments;
static void native_assign(void *self, const void *source) {
    assert(self == hot_target.bytes);
    memcpy(hot_target.bytes + 8, ((const unsigned char *) (source)) + 8, 0xf0);
    ++assignments;
}
static unsigned cancel_calls;
static int submit_result = AZ_PENDING;
static uint64_t submitted_token;
static void native_copy(void *out, const void *in) {
    memcpy(out, in, 0xf8);
    assert(get(uintptr_t, in, 0x58) == (uintptr_t) (&references));
    ++references;
    ++copies;
}
static void native_destroy(void *p) {
    assert(get(uintptr_t, p, 0x58) == (uintptr_t) (&references) && references);
    --references;
    ++destroys;
}
static bool native_affinity(void *box) {
    assert(box == core.bytes + 0x110);
    return foreign_thread;
}
static void native_call(void *self, int kind, const void *property) {
    assert(self == load.bytes && kind == 0 && !foreign_thread);
    assert(get(uint64_t, property, 0x40) == 96000);
    ++dispatches;
    atomic_store_explicit(&(dispatch_inside), true, memory_order_release);
    while (atomic_load_explicit(&(dispatch_hold), memory_order_acquire)) sched_yield();
}
int azsdk_exit_recall_submit(const struct azexit_cue_request *r) {
    ++submissions;
    submitted_token = r->identity.token;
    return submit_result;
}
int azsdk_exit_recall_cancel(unsigned unused, uint64_t token) {
    (void) unused;
    assert(token == submitted_token);
    ++cancel_calls;
    return AZ_PENDING;
}
int azsdk_exit_recall_quiescent(unsigned unused, uint64_t unused2) {
    (void) unused;
    (void) unused2;
    return atomic_load_explicit(&(bridge_quiescent), memory_order_acquire);
}
static struct azexit_cue_request request(uint64_t token) {
    struct azexit_cue_request r = {0};
    r.size = sizeof r;
    r.operation = AZ_CMD_SAVED_RECALL;
    r.identity = (struct azexit_request) {token, 10, UINT64_MAX, 0, 0};
    return r;
}
static void bindings() {
    put(manager.bytes, 0, (uintptr_t) (manager_table));
    put(manager.bytes, 0x130, ((uintptr_t) loader.bytes));
    put(loader.bytes, 0, (uintptr_t) (loader_table));
    put(loader.bytes, 0x1b8, ((uintptr_t) load.bytes));
    put(load.bytes, 0, (uintptr_t) (load_table));
    put(load.bytes, 0x10, ((uintptr_t) core.bytes) + 0x110);
    put(load.bytes, 0x20, ((uintptr_t) available.bytes));
    put(load.bytes, 0x60, ((uintptr_t) context.bytes));
    put(control.bytes, 0x18, ((uintptr_t) core.bytes) + 0x110);
    put(control.bytes, 0x28, ((uintptr_t) available.bytes));
    put(control.bytes, 0x68, ((uintptr_t) context.bytes));
    put(core.bytes, 0x110, (uintptr_t) (box_table));
    put(available.bytes, 8, (uint8_t) 1);
    foreign_thread = false;
}
static _Atomic bool retire_started, retire_done;
static void *dsp_run(void *unused) {
    (void) unused;
    assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_OK);
    return NULL;
}
static void *retire_run(void *unused) {
    (void) unused;
    retire_started = true;
    azsdk_cue_recall_retire(manager.bytes);
    retire_done = true;
    return NULL;
}
int main() {
    load_table[3] = (uintptr_t) (native_call);
    property_table[7] = (uintptr_t) (native_assign);
    box_table[5] = (uintptr_t) (native_affinity);
    struct azcue_recall_profile profile = {(uintptr_t) (manager_table), (uintptr_t) (loader_table), (uintptr_t) (load_table), (uintptr_t) (property_table),
                                           (uintptr_t) (native_call), (uintptr_t) (native_copy), (uintptr_t) (native_destroy), (uintptr_t) (native_affinity), 0, (uintptr_t) (native_assign), 0};
    azsdk_cue_recall_test_profile(&profile);
    assert(azsdk_cue_recall_configure(0, 0) == AZ_INVALID);
    assert(azsdk_cue_recall_configure(1, 0) == AZ_OK);
    bindings();
    RAW(0xf8)
    property;
    put(property.bytes, 0, (uintptr_t) (property_table));
    put(property.bytes, 0x58, (uintptr_t) (&references));
    put(property.bytes, 0x40, (uint64_t) (96000));
    __auto_type r = request(1);
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    assert(copies == 1 && references == 2 && submitted_token == 1);
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_BUSY);
    assert(!azsdk_cue_recall_property(0, 2));
    const void *owned = azsdk_cue_recall_property(0, 1);
    assert(owned && owned != property.bytes);
    native_destroy(property.bytes); /* caller's temporary can disappear before DSP execution */
    assert(references == 1 && get(uint64_t, owned, 0x40) == 96000);
    assert(azsdk_cue_recall_dispatch(0, 2, core.bytes, control.bytes) == AZ_STALE);
    assert(azsdk_cue_recall_dispatch(0, 1, core.bytes, control.bytes) == AZ_OK && dispatches == 1);
    assert(azsdk_cue_recall_dispatch(0, 1, core.bytes, control.bytes) == AZ_STALE);
    azsdk_cue_recall_release(0, 2);
    assert(references == 1);
    azsdk_cue_recall_release(0, 1);
    assert(references == 0 && !azsdk_cue_recall_property(0, 1));
    azsdk_cue_recall_release(0, 1);
    assert(destroys == 2);
    /* Native submission alone never fabricates an applied receipt. Completion is
     * exclusively the sdk_exit bridge's correlated seek and settled graph. */
    assert(submissions == 1 && submit_result == AZ_PENDING);
    references = 1;
    for (unsigned fault = 0; fault < 7; ++fault) {
        bindings();
        r = request(10 + fault);
        assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
        if (fault == 0) foreign_thread = true;
        if (fault == 1) put(load.bytes, 0x10, ((uintptr_t) core.bytes));
        if (fault == 2) put(control.bytes, 0x18, ((uintptr_t) core.bytes));
        if (fault == 3) put(load.bytes, 0x60, ((uintptr_t) context.bytes) + 8);
        if (fault == 4) put(load.bytes, 0x20, ((uintptr_t) available.bytes) + 1);
        if (fault == 5) put(available.bytes, 8, (uint8_t) 0);
        if (fault == 6) put(load.bytes, 0, (uintptr_t) (property_table));
        assert(azsdk_cue_recall_dispatch(0, r.identity.token, core.bytes, control.bytes) == AZ_UNSUPPORTED);
        assert(dispatches == 1);
        azsdk_cue_recall_release(0, r.identity.token);
        assert(references == 1);
    }
    bindings();
    r = request(30);
    unsigned before = copies;
    put(loader.bytes, 0x1b8, (uintptr_t) (0));
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_UNSUPPORTED && copies == before);
    bindings();
    submit_result = AZ_BUSY;
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_BUSY && references == 1);
    assert(!azsdk_cue_recall_property(0, r.identity.token));
    submit_result = AZ_PENDING;
    r.operation = AZ_CMD_MEMORY_RECALL;
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    azsdk_cue_recall_release(0, r.identity.token);
    assert(references == 1);
    // Arbitrary Hot hydration invokes only the typed property update. The
    // general CueLoadFacade path would alter unrelated playback state.
    unsigned recalls_before = dispatches;
    const unsigned operations[] = {AZ_CMD_CUE_POINT_SET, AZ_CMD_CUE_LOOP_SET};
    for (unsigned oi = 0; oi < 2; ++oi) {
        unsigned operation = operations[oi];
        bindings();
        // Stock CueLoader starts with an empty lazy recall cache. Neither new
        // Hot operation needs that wrapper, even before any stock cue recall.
        put(loader.bytes, 0x1b8, (uintptr_t) (0));
        const unsigned generics[] = {AZ_CMD_MEMORY_RECALL, AZ_CMD_SAVED_RECALL};
        for (unsigned gi = 0; gi < 2; ++gi) {
            unsigned generic = generics[gi];
            r = request(35);
            r.operation = generic;
            assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_UNSUPPORTED);
        }
        put(hot_target.bytes, 0, (uintptr_t) (property_table));
        put(context.bytes, 0x18 + 3 * 8, ((uintptr_t) hot_target.bytes));
        r = request(35);
        r.operation = operation;
        r.classification = AZ_CUE_HOT;
        r.entry_id = 3;
        assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
        unsigned assignments_before = assignments;
        assert(azsdk_cue_recall_dispatch(0, 35, core.bytes, control.bytes) == AZ_OK);
        assert(assignments == assignments_before + 1 && dispatches == recalls_before);
        assert(!memcmp(hot_target.bytes + 8, property.bytes + 8, 0xf0));
        azsdk_cue_recall_release(0, 35);
        assert(references == 1);
    }
    // Excluding the unused recall cache must retain the actual Core owner,
    // available source context, affinity and target type/method checks.
    for (unsigned fault = 0; fault < 6; ++fault) {
        bindings();
        put(loader.bytes, 0x1b8, (uintptr_t) (0));
        put(hot_target.bytes, 0, (uintptr_t) (property_table));
        put(context.bytes, 0x18 + 3 * 8, ((uintptr_t) hot_target.bytes));
        r = request(36);
        r.operation = AZ_CMD_CUE_POINT_SET;
        r.classification = AZ_CUE_HOT;
        r.entry_id = 3;
        assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
        unsigned assignments_before = assignments;
        if (fault == 0) foreign_thread = true;
        if (fault == 1) put(control.bytes, 0x18, ((uintptr_t) core.bytes));
        if (fault == 2) put(control.bytes, 0x68, (uintptr_t) (0));
        if (fault == 3) put(available.bytes, 8, (uint8_t) 0);
        if (fault == 4) put(hot_target.bytes, 0, (uintptr_t) (loader_table));
        if (fault == 5) property_table[7] = (uintptr_t) (native_call);
        assert(azsdk_cue_recall_dispatch(0, 36, core.bytes, control.bytes) == AZ_UNSUPPORTED);
        assert(assignments == assignments_before && dispatches == recalls_before);
        property_table[7] = (uintptr_t) (native_assign);
        azsdk_cue_recall_release(0, 36);
        assert(references == 1);
    }
    /* Manager destruction invalidates the borrowed facade before a queued DSP
     * dispatch, while keeping its independent owned property until settlement. */
    bindings();
    r = request(40);
    bridge_quiescent = false;
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    unsigned calls_before_retire = dispatches;
    azsdk_cue_recall_retire(manager.bytes);
    assert(cancel_calls == 1 && references == 2);
    memset(load.bytes, 0, sizeof load.bytes); /* emulate destroyed native facade */
    assert(azsdk_cue_recall_dispatch(0, 40, core.bytes, control.bytes) == AZ_STALE);
    assert(dispatches == calls_before_retire);
    azsdk_cue_recall_release(0, 40);
    assert(references == 2 && azsdk_cue_recall_property(0, 40));
    bridge_quiescent = true;
    azsdk_cue_recall_release(0, 40);
    assert(references == 1 && !azsdk_cue_recall_property(0, 40));
    /* A replacement lifecycle can reuse the allocation; the previous token
     * cannot dispatch that replacement's facade. */
    bindings();
    r = request(41);
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    assert(azsdk_cue_recall_dispatch(0, 40, core.bytes, control.bytes) == AZ_STALE);
    assert(azsdk_cue_recall_dispatch(0, 41, core.bytes, control.bytes) == AZ_OK);
    azsdk_cue_recall_release(0, 41);
    assert(references == 1);
    /* Retirement waits for the synchronous native call to release its borrow.
     * DSP-facing access uses a try-lock and cannot wait behind that call. */
    bindings();
    r = request(42);
    bridge_quiescent = false;
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    dispatch_hold = true;
    dispatch_inside = false;
    pthread_t dsp;
    assert(!pthread_create(&dsp, NULL, dsp_run, NULL));
    while (!atomic_load_explicit(&(dispatch_inside), memory_order_acquire)) sched_yield();
    assert(!azsdk_cue_recall_property(0, 42));
    assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_BUSY);
    pthread_t destructor;
    assert(!pthread_create(&destructor, NULL, retire_run, NULL));
    while (!atomic_load_explicit(&(retire_started), memory_order_seq_cst)) sched_yield();
    assert(!atomic_load_explicit(&(retire_done), memory_order_seq_cst) && references == 2);
    dispatch_hold = false;
    assert(!pthread_join(dsp, NULL));
    assert(!pthread_join(destructor, NULL));
    assert(atomic_load_explicit(&(retire_done), memory_order_seq_cst) && cancel_calls == 2 && references == 2);
    assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_STALE);
    azsdk_cue_recall_release(0, 42);
    assert(references == 2);
    bridge_quiescent = true;
    azsdk_cue_recall_release(0, 42);
    assert(references == 1);
}

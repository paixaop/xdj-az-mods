/* SPDX-License-Identifier: GPL-3.0-only */
#include "azcuerecall.h"
#include "azmod-mod.h"
#include <cassert>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
template <unsigned N>
struct alignas(8) Raw {
    unsigned char bytes[N] = {};
    uintptr_t address() { return uintptr_t(bytes); }
};
template <class T>
static void put(void *p, unsigned at, T v) { std::memcpy(static_cast<unsigned char *>(p) + at, &v, sizeof v); }
template <class T>
static T get(const void *p, unsigned at) {
    T v;
    std::memcpy(&v, static_cast<const unsigned char *>(p) + at, sizeof v);
    return v;
}
static uintptr_t manager_table[2], loader_table[2], load_table[4], property_table[2], box_table[6];
static Raw<0x180> manager;
static Raw<0x1d0> loader;
static Raw<0x90> load, control;
static Raw<0x200> core;
static Raw<0x400> context;
static Raw<0x10> available;
static unsigned copies, destroys, dispatches, references = 1, submissions;
static bool foreign_thread;
static std::atomic<bool> dispatch_hold {false}, dispatch_inside {false}, bridge_quiescent {true};
static unsigned cancel_calls;
static int submit_result = AZ_PENDING;
static uint64_t submitted_token;
static void native_copy(void *out, const void *in) {
    std::memcpy(out, in, 0xf8);
    assert(get<uintptr_t>(in, 0x58) == uintptr_t(&references));
    ++references;
    ++copies;
}
static void native_destroy(void *p) {
    assert(get<uintptr_t>(p, 0x58) == uintptr_t(&references) && references);
    --references;
    ++destroys;
}
static bool native_affinity(void *box) {
    assert(box == core.bytes + 0x110);
    return foreign_thread;
}
static void native_call(void *self, int kind, const void *property) {
    assert(self == load.bytes && kind == 0 && !foreign_thread);
    assert(get<uint64_t>(property, 0x40) == 96000);
    ++dispatches;
    dispatch_inside.store(true, std::memory_order_release);
    while (dispatch_hold.load(std::memory_order_acquire)) std::this_thread::yield();
}
extern "C" int azsdk_exit_recall_submit(const azexit_cue_request *r) noexcept {
    ++submissions;
    submitted_token = r->identity.token;
    return submit_result;
}
extern "C" int azsdk_exit_recall_cancel(unsigned, uint64_t token) noexcept {
    assert(token == submitted_token);
    ++cancel_calls;
    return AZ_PENDING;
}
extern "C" int azsdk_exit_recall_quiescent(unsigned, uint64_t) noexcept {
    return bridge_quiescent.load(std::memory_order_acquire);
}
static azexit_cue_request request(uint64_t token) {
    azexit_cue_request r = {};
    r.size = sizeof r;
    r.operation = AZ_CMD_SAVED_RECALL;
    r.identity = {token, 10, UINT64_MAX, 0, 0};
    return r;
}
static void bindings() {
    put(manager.bytes, 0, uintptr_t(manager_table));
    put(manager.bytes, 0x130, loader.address());
    put(loader.bytes, 0, uintptr_t(loader_table));
    put(loader.bytes, 0x1b8, load.address());
    put(load.bytes, 0, uintptr_t(load_table));
    put(load.bytes, 0x10, core.address() + 0x110);
    put(load.bytes, 0x20, available.address());
    put(load.bytes, 0x60, context.address());
    put(control.bytes, 0x18, core.address() + 0x110);
    put(control.bytes, 0x28, available.address());
    put(control.bytes, 0x68, context.address());
    put(core.bytes, 0x110, uintptr_t(box_table));
    put(available.bytes, 8, uint8_t(1));
    foreign_thread = false;
}
int main() {
    load_table[3] = uintptr_t(native_call);
    box_table[5] = uintptr_t(native_affinity);
    azcue_recall_profile profile = {uintptr_t(manager_table), uintptr_t(loader_table), uintptr_t(load_table), uintptr_t(property_table),
                                    uintptr_t(native_call), uintptr_t(native_copy), uintptr_t(native_destroy), uintptr_t(native_affinity)};
    azsdk_cue_recall_test_profile(&profile);
    assert(azsdk_cue_recall_configure(0, 0) == AZ_INVALID);
    assert(azsdk_cue_recall_configure(1, 0) == AZ_OK);
    bindings();
    Raw<0xf8> property;
    put(property.bytes, 0, uintptr_t(property_table));
    put(property.bytes, 0x58, uintptr_t(&references));
    put(property.bytes, 0x40, uint64_t(96000));
    auto r = request(1);
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    assert(copies == 1 && references == 2 && submitted_token == 1);
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_BUSY);
    assert(!azsdk_cue_recall_property(0, 2));
    const void *owned = azsdk_cue_recall_property(0, 1);
    assert(owned && owned != property.bytes);
    native_destroy(property.bytes); /* caller's temporary can disappear before DSP execution */
    assert(references == 1 && get<uint64_t>(owned, 0x40) == 96000);
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
        if (fault == 1) put(load.bytes, 0x10, core.address());
        if (fault == 2) put(control.bytes, 0x18, core.address());
        if (fault == 3) put(load.bytes, 0x60, context.address() + 8);
        if (fault == 4) put(load.bytes, 0x20, available.address() + 1);
        if (fault == 5) put(available.bytes, 8, uint8_t(0));
        if (fault == 6) put(load.bytes, 0, uintptr_t(property_table));
        assert(azsdk_cue_recall_dispatch(0, r.identity.token, core.bytes, control.bytes) == AZ_UNSUPPORTED);
        assert(dispatches == 1);
        azsdk_cue_recall_release(0, r.identity.token);
        assert(references == 1);
    }
    bindings();
    r = request(30);
    unsigned before = copies;
    put(loader.bytes, 0x1b8, uintptr_t(0));
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
    /* Manager destruction invalidates the borrowed facade before a queued DSP
     * dispatch, while keeping its independent owned property until settlement. */
    bindings();
    r = request(40);
    bridge_quiescent = false;
    assert(azsdk_cue_recall_prepare(&r, manager.bytes, property.bytes) == AZ_PENDING);
    unsigned calls_before_retire = dispatches;
    azsdk_cue_recall_retire(manager.bytes);
    assert(cancel_calls == 1 && references == 2);
    std::memset(load.bytes, 0, sizeof load.bytes); /* emulate destroyed native facade */
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
    std::thread dsp([&] { assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_OK); });
    while (!dispatch_inside.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(!azsdk_cue_recall_property(0, 42));
    assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_BUSY);
    std::atomic<bool> retire_started {false}, retire_done {false};
    std::thread destructor([&] {
        retire_started = true;
        azsdk_cue_recall_retire(manager.bytes);
        retire_done = true;
    });
    while (!retire_started.load()) std::this_thread::yield();
    assert(!retire_done.load() && references == 2);
    dispatch_hold = false;
    dsp.join();
    destructor.join();
    assert(retire_done.load() && cancel_calls == 2 && references == 2);
    assert(azsdk_cue_recall_dispatch(0, 42, core.bytes, control.bytes) == AZ_STALE);
    azsdk_cue_recall_release(0, 42);
    assert(references == 2);
    bridge_quiescent = true;
    azsdk_cue_recall_release(0, 42);
    assert(references == 1);
}

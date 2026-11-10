/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azcuerecall.h"
#include "../common/azcuerecallbridge.h"
#include "../common/azcueprofile.h"
#include "../azmod/azmod-mod.h"
#include <cstring>
#include <mutex>
namespace {
#ifndef AZSDK_CUE_RECALL_TESTING
struct azcue_recall_profile {
    uintptr_t manager_table, loader_table, load_table, property_table;
    uintptr_t load_call, copy_property, destroy_property, affinity;
    uintptr_t locked_property_table, assign_property, assign_locked;
};
#endif
struct Slot {
    std::mutex mutex;
    alignas(8) unsigned char property[0xf8] = {};
    uint64_t token = 0;
    uintptr_t facade = 0, manager = 0;
    unsigned operation = 0, kind = 0;
    bool retired = false;
    bool dispatched = false;
};
struct State {
    std::mutex mutex;
    azcue_recall_profile profile = {};
    Slot slots[4];
    bool configured = false;
};
State &state() {
    static State s;
    return s;
}
template <class T>
T read(uintptr_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof v);
    return v;
}
uintptr_t word(uintptr_t a) { return read<uintptr_t>(a); }
bool nominal(uintptr_t object, uintptr_t table) { return object && word(object) == table; }
bool hot_install(unsigned operation) {
    return operation == AZ_CMD_CUE_POINT_SET || operation == AZ_CMD_CUE_LOOP_SET;
}
void reset(Slot &slot) {
    if (slot.token) reinterpret_cast<void (*)(void *)>(state().profile.destroy_property)(slot.property);
    slot.token = 0;
    slot.facade = slot.manager = 0;
    slot.retired = false;
    slot.dispatched = false;
    std::memset(slot.property, 0, sizeof slot.property);
}
} // namespace
#ifdef AZSDK_CUE_RECALL_TESTING
extern "C" void azsdk_cue_recall_test_profile(const azcue_recall_profile *p) noexcept {
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (p && !s.configured) s.profile = *p;
}
#endif
extern "C" int azsdk_cue_recall_configure(int build, uintptr_t bias) noexcept {
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (build != 1 && build != 2) return AZ_INVALID;
    if (s.configured) return AZ_OK;
#if defined(__aarch64__) && !defined(AZSDK_CUE_RECALL_TESTING)
    const bool newer = build == 2;
#define NATIVE(field, old_address, new_address) s.profile.field = bias + (newer ? new_address : old_address)
    NATIVE(manager_table, 0x2614318, 0x294e7a8);
    NATIVE(loader_table, 0x2616030, 0x29504c0);
    NATIVE(load_table, 0x28770b8, 0x2bc9300);
    NATIVE(property_table, 0x288eeb0, 0x2be12b0);
    NATIVE(load_call, 0x13726c8, 0x142da18);
    NATIVE(copy_property, 0x80aac0, 0x8504a8);
    NATIVE(destroy_property, 0x13e7e98, 0x14a32d8);
    NATIVE(affinity, 0x957408, 0x99ca38);
    NATIVE(locked_property_table, 0x288f0d0, 0x2be14d0);
    azcue_profile verified {};
    if (!azsdk_cue_main_profile(build, &verified)) return AZ_UNSUPPORTED;
    s.profile.assign_property = verified.store[13].address;
    s.profile.assign_locked = verified.store[14].address;
#undef NATIVE
#else
    (void) bias;
#endif
    const auto &p = s.profile;
    if (!p.manager_table || !p.loader_table || !p.load_table || !p.property_table ||
        !p.load_call || !p.copy_property || !p.destroy_property || !p.affinity ||
        word(p.load_table + 3 * sizeof(uintptr_t)) != p.load_call) return AZ_UNSUPPORTED;
    s.configured = true;
    return AZ_OK;
}
extern "C" int azsdk_cue_recall_prepare(const azexit_cue_request *r, void *manager,
                                        const void *property) noexcept {
    if (!r || r->size < sizeof *r || r->identity.deck >= 4 || !r->identity.token ||
        !r->identity.load_generation || !r->identity.deadline_ns || r->identity.reserved ||
        (r->operation != AZ_CMD_MEMORY_RECALL && r->operation != AZ_CMD_SAVED_RECALL &&
         !((r->operation == AZ_CMD_CUE_POINT_SET || r->operation == AZ_CMD_CUE_LOOP_SET) &&
           r->classification == AZ_CUE_HOT && r->entry_id >= 1 && r->entry_id <= 8))) return AZ_INVALID;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto &p = s.profile;
    if (!s.configured || !nominal(reinterpret_cast<uintptr_t>(manager), p.manager_table) ||
        !nominal(reinterpret_cast<uintptr_t>(property), p.property_table)) return AZ_UNSUPPORTED;
    Slot &slot = s.slots[r->identity.deck];
    std::unique_lock<std::mutex> slot_lock(slot.mutex);
    if (slot.token) return AZ_BUSY;
    uintptr_t facade = 0;
    if (!hot_install(r->operation)) {
        const uintptr_t loader = word(reinterpret_cast<uintptr_t>(manager) + 0x130);
        if (!nominal(loader, p.loader_table)) return AZ_UNSUPPORTED;
        facade = word(loader + 0x1b8);
        if (!nominal(facade, p.load_table)) return AZ_UNSUPPORTED;
    }
    try {
        reinterpret_cast<void (*)(void *, const void *)>(p.copy_property)(slot.property, property);
    } catch (...) { return AZ_FAILED; }
    slot.operation = r->operation;
    slot.kind = r->entry_id;
    slot.token = r->identity.token;
    slot.facade = facade;
    slot.manager = reinterpret_cast<uintptr_t>(manager);
    /* The caller holds the manager lifecycle gate. Publish the complete owned
     * slot before making its DSP mailbox runnable. */
    slot_lock.unlock();
    int rc = azsdk_exit_recall_submit(r);
    if (rc != AZ_PENDING) {
        slot_lock.lock();
        reset(slot);
    }
    return rc;
}
extern "C" const void *azsdk_cue_recall_property(unsigned deck, uint64_t token) noexcept {
    State &s = state();
    if (deck >= 4 || !token) return nullptr;
    Slot &slot = s.slots[deck];
    std::unique_lock<std::mutex> lock(slot.mutex, std::try_to_lock);
    if (!lock.owns_lock() || slot.token != token) return nullptr;
    return slot.property;
}
extern "C" int azsdk_cue_recall_dispatch(unsigned deck, uint64_t token, void *core,
                                         void *control_facade) noexcept {
    State &s = state();
    if (!s.configured || deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = s.slots[deck];
    std::unique_lock<std::mutex> lock(slot.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_BUSY;
    const auto &p = s.profile;
    if (slot.token != token || slot.dispatched || slot.retired) return AZ_STALE;
    const uintptr_t c = reinterpret_cast<uintptr_t>(core), control = reinterpret_cast<uintptr_t>(control_facade);
    const uintptr_t facade = slot.facade;
    if (!c || !control || word(control + 0x18) != c + 0x110 ||
        !word(control + 0x68) || !word(control + 0x28) ||
        !read<uint8_t>(word(control + 0x28) + 8) ||
        word(word(c + 0x110) + 5 * sizeof(uintptr_t)) != p.affinity ||
        reinterpret_cast<bool (*)(void *)>(p.affinity)(reinterpret_cast<void *>(c + 0x110))) return AZ_UNSUPPORTED;
    if (hot_install(slot.operation)) {
        // sdk_exit already joined this registered unit/Core/ControlFacade,
        // native load generation and the owned property's source ID under its
        // DSP lifecycle lease. Stock CueLoader's lazy recall cache can be NULL;
        // the direct typed Hot update neither borrows nor invokes that wrapper.
        // CueLoadFacade::call also cancels unrelated control state. Use its
        // inner typed Hot property update instead, on this same DSP owner.
        uintptr_t context = word(control + 0x68);
        uintptr_t target = word(context + 0x18 + slot.kind * 8);
        uintptr_t assign = nominal(target, p.property_table) ? p.assign_property :
                           nominal(target, p.locked_property_table) ? p.assign_locked : 0;
        if (!assign || word(word(target) + 7 * sizeof(uintptr_t)) != assign) return AZ_UNSUPPORTED;
        slot.dispatched = true;
        try { reinterpret_cast<void (*)(void *, const void *)>(assign)(reinterpret_cast<void *>(target), slot.property); }
        catch (...) { return AZ_FAILED; }
        return AZ_OK;
    }
    if (!nominal(facade, p.load_table) || word(facade + 0x10) != c + 0x110 ||
        word(facade + 0x60) != word(control + 0x68) ||
        word(facade + 0x20) != word(control + 0x28)) return AZ_UNSUPPORTED;
    /* Native root copies the owned property again. The affinity check above
     * forces its synchronous branch, preserving sdk_exit's causal node scope. */
    slot.dispatched = true;
    try {
        reinterpret_cast<void (*)(void *, int, const void *)>(p.load_call)(reinterpret_cast<void *>(facade), 0, slot.property);
    } catch (...) { return AZ_FAILED; }
    return AZ_OK; /* Submission only: sdk_exit requires the correlated seek and settled work. */
}
extern "C" void azsdk_cue_recall_release(unsigned deck, uint64_t token) noexcept {
    State &s = state();
    if (deck >= 4 || !token) return;
    Slot &slot = s.slots[deck];
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (slot.token == token && azsdk_exit_recall_quiescent(deck, token)) reset(slot);
}

extern "C" void azsdk_cue_recall_retire(void *manager) noexcept {
    if (!manager) return;
    State &s = state();
    for (unsigned deck = 0; deck < 4; ++deck) {
        Slot &slot = s.slots[deck];
        /* Management waits for a native dispatch that already borrowed the
         * facade. DSP never waits for this gate or another deck's copy. */
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.token || slot.manager != reinterpret_cast<uintptr_t>(manager)) continue;
        slot.retired = true;
        slot.facade = 0;
        (void) azsdk_exit_recall_cancel(deck, slot.token);
        /* Retain native-owned members until the DSP bridge is quiescent. A
         * dispatch may have finished but its child graph/postcondition has not. */
    }
}

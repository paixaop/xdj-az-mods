/* SPDX-License-Identifier: GPL-3.0-only */
/* Asynchronous native EXIT/RELOOP. The mailbox owns copied SDK values; native command
 * incarnations carry the request through Quantize, Slip and subsequent chains. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/aztimestamp.h"
#include "../common/azexit.h"
#include "../common/azcuerecallbridge.h"
#include "../common/azexitprofile.h"
#ifndef AZSDK_EXIT_TESTING
#include "../common/azcuecommands.h"
#endif
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
#include "../common/azcuerecall.h"
#endif
#include "../common/aznativecommands.h"
#include "../common/azloadobserver.h"
#include "../azmod/azmod-mod.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <dlfcn.h>
#include <exception>
#include <limits>
#include <mutex>
#include <time.h>
#if defined(__linux__)
#include <link.h>
#endif
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_EXIT_TESTING)
#include "sdk_vtable_patch.h"
#endif
#include "sdk_exit_profiles.h"

namespace {
enum {
    IDLE,
    WRITING,
    QUEUED,
    RUNNING,
    READY,
    READING
};
static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
              "EXIT owner callbacks require lock-free mailboxes");
struct LoopPoints {
    bool in_valid = false, out_valid = false;
    uint64_t in = 0, out = 0, source = 0;
    uint32_t rate = 0, beat = 0, mode = 0;
    unsigned char identity[12] = {};
};
struct Slot {
    azsdk_native::OwnerGate gate;
    std::atomic<uintptr_t> core {0}; /* comparison only before gate admission */
    uintptr_t unit = 0, facade = 0;
    std::atomic<unsigned> phase {IDLE};
    azexit_request request = {};
    uint32_t operation = AZ_CMD_LOOP_EXIT;
    azexit_loop_request arguments = {};
    azexit_cue_request cue_arguments = {};
    unsigned char cue_source_before[12] = {};
    bool cue_seek_seen = false;
    bool cue_playing_before = false;
    bool cue_aux_before = false;
    unsigned cue_rgb_before = 0;
    std::atomic<bool> internal_recall {false};
    LoopPoints before, retained_before;
    uintptr_t retained = 0;
    azexit_cue_stage_sample staged_cue = {};
    uint64_t expected_in = 0, expected_out = 0;
    unsigned move_stage = 0;
    azexit_result result = {};
    uint64_t revision = 0; /* owner frame only; never reset on replacement */
    std::atomic<uint64_t> token {0}, lost {0}, failed {0};
    std::atomic<unsigned> loss_overflow {0};
    azsdk_load_sample load = {};
    bool untracked = false;
    bool caused = false;
    bool initially_engaged = false;
    bool maintenance_seen = false;
    unsigned captured = 0;
};
struct Node {
    /* Zero is free, one is reserved. Fields are atomic because disposal can run
     * outside the processing thread. No saved pointer is dereferenced. */
    std::atomic<uintptr_t> address {0};
    std::atomic<uint64_t> token {0}, generation {0}, incarnation {0};
    /* required: 0 borrowed/superseded, 1 finite effect, 2 proven initial loop setup. */
    std::atomic<unsigned> deck {0}, executed {0}, borrowed {0}, required {0}, type {0};
    std::atomic<unsigned> maintenance {0};
};
constexpr unsigned NODE_COUNT = 512;
struct State {
    Slot decks[4];
    Node nodes[NODE_COUNT];
    std::atomic<uint64_t> incarnation {0};
    std::mutex management;
    std::atomic<unsigned> mask {0}, enabled {0};
    azexit_profile profile = {};
    uintptr_t bias = 0;
    uintptr_t beat_exit = 0, beat_reset = 0;
    int build = 0;
    void *original = nullptr, *pre_original = nullptr;
    void *manager_original = nullptr, *quantize_original = nullptr;
    void *cancel_original = nullptr, *core_jump_original = nullptr;
    bool configured = false;
};
struct Scope {
    Slot *slot;
    unsigned deck;
    uint64_t token, generation;
    bool maintenance;
    bool setup;
    uintptr_t command;
};
thread_local Scope causal = {};
thread_local Slot *processing = nullptr;
struct CausalScope {
    Scope previous;
    explicit CausalScope(const Scope &value) : previous(causal) { causal = value; }
    ~CausalScope() { causal = previous; }
};
bool future_chain(Slot &, uintptr_t, uintptr_t *, unsigned &) noexcept;
bool loop_schedule_matches(Slot &, const void *, const void *, int, const void *, double) noexcept;
bool settled_work(Slot &, uint64_t) noexcept;
void supersede_setup(Slot &) noexcept;
bool initial_request(Slot &, const void *) noexcept;
bool initial_chain(Slot &, uintptr_t) noexcept;
uintptr_t future_hidden(Slot &, uintptr_t) noexcept;
/* ELF constructors can call configure before default-priority C++ initialization. */
State &state() {
    static State value;
    return value;
}
template <class Function>
Function trampoline(void *const *source) noexcept {
    /* Installation publishes the original before the entry patch. A reached
     * hook without its original violates that startup invariant; never swallow
     * the native call or continue with partially initialized state. */
    void *entry = __atomic_load_n(source, __ATOMIC_ACQUIRE);
    if (!entry) std::terminate();
    return reinterpret_cast<Function>(entry);
}
uint64_t now() noexcept {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = static_cast<uint64_t>(ts.tv_sec);
    return seconds <= (UINT64_MAX - uint64_t(ts.tv_nsec)) / UINT64_C(1000000000)
               ? seconds * UINT64_C(1000000000) + uint64_t(ts.tv_nsec)
               : 0;
}
uintptr_t word(uintptr_t p) noexcept {
    uintptr_t v;
    std::memcpy(&v, reinterpret_cast<void *>(p), sizeof v);
    return v;
}
int32_t integer(uintptr_t p) noexcept {
    int32_t v;
    std::memcpy(&v, reinterpret_cast<void *>(p), sizeof v);
    return v;
}
unsigned byte(uintptr_t p) noexcept { return *reinterpret_cast<const unsigned char *>(p); }
bool pointer(uintptr_t p) noexcept { return p >= 0x10000 && !(p & 7) && p <= UINT64_C(0x0000ffffffffffff); }
bool table(uintptr_t object, uintptr_t expected) noexcept {
    return pointer(object) && word(object) == expected;
}
/* These fields belong to the constructed unit, protected by its actual teardown gate.
 * Admission alone is intentionally NOT an owner-thread claim. Affinity is checked after
 * the original preProcess has bound the native boxes to its current processing thread. */
bool admitted(void *) { return true; }
bool manager_binding(uintptr_t manager) noexcept {
    if (!pointer(manager)) return false;
    uintptr_t adapter = word(manager + 0x10);
    if (!adapter) return true;
    const auto &p = state().profile;
    /* Stock request() recognizes this stateless reference-merging adapter and
     * keeps the chain in the same manager. Other adapters may retain it elsewhere. */
    bool valid = table(adapter, p.merger_table) && word(p.merger_table + 16) == p.merger_request;
    return valid;
}
bool coherent(Slot &slot) noexcept {
    State &s = state();
    const auto &p = s.profile;
    uintptr_t core = slot.core.load(std::memory_order_acquire);
    uintptr_t bundle = word(slot.unit + 0x190), facade = slot.facade;
    return table(slot.unit, p.unit) && table(core, p.core) && pointer(bundle) &&
           word(slot.unit + 0x128) == core && word(bundle + 0x40) == facade &&
           table(facade, p.facade) && word(facade + 0x18) == core + 0x110 &&
           word(facade + 0x40) == core && table(core + 0x110, p.box) &&
           word(p.facade + 3 * 8) == p.exit && word(p.facade + 4 * 8) == p.reloop &&
           word(p.facade + 6 * 8) == p.control_jump && word(p.box + 5 * 8) == p.affinity;
}
bool bindings(Slot &slot) noexcept {
    const auto &p = state().profile;
    uintptr_t f = slot.facade, core = slot.core.load(std::memory_order_relaxed);
    uintptr_t beat = word(f + 0x70), quantize = word(f + 0x50), slip = word(f + 0xa8);
    uintptr_t manager = word(core + 0xa0), available = word(f + 0x28);
    if (!table(beat, p.beat) || word(p.beat + 16 * 8) != state().beat_exit ||
        word(p.beat + 15 * 8) != state().beat_reset || !table(quantize, p.quantize) || !table(slip, p.slip) ||
        !manager_binding(manager) || !pointer(available) || !byte(available + 8)) {
        return false;
    }
    /* The native objects must be the same UnitFunctionSet children, rather than
     * unrelated objects with matching nominal types. */
    uintptr_t functions = word(slot.unit + 0x188);
    uintptr_t basis = word(slot.unit + 0x180);
    if (!pointer(basis)) return false;
    uintptr_t jump = word(basis + 0x38);
    bool valid = table(jump, p.jump_table) && word(basis + 0x30) == core && word(jump + 0x10) == core &&
           word(quantize + 0x20) == jump && word(beat + 0x38) == basis && word(beat + 0x48) == core &&
           word(beat + 0x70) == word(f + 0x68) && word(beat + 0x80) == quantize &&
           pointer(functions) && word(functions + 0x20) == quantize &&
           word(functions + 0x30) == slip && word(slip + 0x78) == quantize;
    return valid;
}
bool quantize_idle(Slot &slot) noexcept {
    uintptr_t quantize = word(slot.facade + 0x50);
    if (integer(quantize + 0x98) != -1) return false;
    const unsigned holders[] = {0x30, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8};
    for (unsigned offset : holders)
        if (word(quantize + offset)) return false;
    return true;
}
bool idle(Slot &slot) noexcept {
    uintptr_t manager = word(slot.core.load(std::memory_order_relaxed) + 0xa0);
    return !word(manager) && !word(manager + 8) && quantize_idle(slot);
}
bool same_load(unsigned deck, Slot &slot) noexcept {
    azsdk_load_sample current = {};
    return azsdk_load_read(int(deck), &current) && current.load_generation == slot.load.load_generation &&
           current.unit_generation == slot.load.unit_generation &&
           current.invalidation_epoch == slot.load.invalidation_epoch;
}
void mark_lost(Slot &slot, uint64_t token) noexcept {
    /* Dispatch tokens increase process-wide. An old tombstone must not erase
     * a newer request's failure when it is finally executed or destroyed. */
    uint64_t observed = slot.lost.load(std::memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (observed >= token || slot.lost.compare_exchange_strong(observed, token, std::memory_order_release, std::memory_order_relaxed)) return;
    }
    slot.loss_overflow.store(1, std::memory_order_release);
}
int command_type(uintptr_t object) noexcept {
    if (!pointer(object)) return -1;
    uintptr_t vtable = word(object);
    for (unsigned i = 0; i < AZEXIT_COMMAND_TYPES; ++i)
        if (vtable == state().profile.commands[i].table) return int(i);
    return -1;
}
Node *find_node(uintptr_t address) noexcept {
    for (auto &node : state().nodes)
        if (node.address.load(std::memory_order_acquire) == address) return &node;
    return nullptr;
}
Node *capture(uintptr_t address, bool borrowed = false) noexcept {
    if (!address || !causal.slot) return nullptr;
    if (Node *node = find_node(address)) {
        if (node->token.load(std::memory_order_acquire) != causal.token ||
            node->generation.load(std::memory_order_acquire) != causal.generation) {
            if (borrowed && node->borrowed.load(std::memory_order_acquire) &&
                node->type.load(std::memory_order_acquire) == AZEXIT_QUANTIZE_PENDING &&
                node->generation.load(std::memory_order_acquire) == causal.generation) {
                /* A later EXIT can reuse the same live native infrastructure
                 * after the prior token has settled. The object incarnation is
                 * unchanged; the new native request gets its own token. */
                node->token.store(causal.token, std::memory_order_release);
                node->executed.store(0, std::memory_order_release);
                node->required.store(0, std::memory_order_release);
            } else if (!borrowed) causal.slot->untracked = true;
        }
        if (causal.setup && !borrowed && node->token.load(std::memory_order_acquire) == causal.token &&
            node->generation.load(std::memory_order_acquire) == causal.generation && !node->executed.load(std::memory_order_acquire))
            node->required.store(2, std::memory_order_release);
        if (causal.maintenance && node->token.load(std::memory_order_acquire) == causal.token &&
            node->generation.load(std::memory_order_acquire) == causal.generation) {
            node->maintenance.store(1, std::memory_order_release);
            causal.slot->maintenance_seen = true;
        }
        return node;
    }
    int type = command_type(address);
    if (type < 0) {
        causal.slot->untracked = true;
        return nullptr;
    }
    for (auto &node : state().nodes) {
        uintptr_t empty = 0;
        if (!node.address.compare_exchange_strong(empty, 1, std::memory_order_acq_rel)) continue;
        uint64_t incarnation = state().incarnation.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!incarnation) std::terminate();
        node.token.store(causal.token, std::memory_order_relaxed);
        node.generation.store(causal.generation, std::memory_order_relaxed);
        node.deck.store(causal.deck, std::memory_order_relaxed);
        node.executed.store(0, std::memory_order_relaxed);
        node.borrowed.store(borrowed, std::memory_order_relaxed);
        node.required.store(borrowed ? 0 : causal.setup ? 2 : 1, std::memory_order_relaxed);
        node.type.store(unsigned(type), std::memory_order_relaxed);
        node.maintenance.store(causal.maintenance, std::memory_order_relaxed);
        node.incarnation.store(incarnation, std::memory_order_relaxed);
        node.address.store(address, std::memory_order_release);
        if (!borrowed) ++causal.slot->captured;
        if (causal.maintenance) causal.slot->maintenance_seen = true;
        return &node;
    }
    causal.slot->untracked = true;
    return nullptr;
}
void capture_list(uintptr_t address, bool borrowed = false) noexcept {
    unsigned count = 0;
    while (address && count++ < NODE_COUNT) {
        if (!capture(address, borrowed)) return;
        address = word(address + 0x18);
    }
    if (address && causal.slot) causal.slot->untracked = true;
}
bool pending_nodes(uint64_t token) noexcept {
    for (auto &node : state().nodes)
        if (node.address.load(std::memory_order_acquire) > 1 && node.token.load(std::memory_order_acquire) == token) return true;
    return false;
}
unsigned free_nodes() noexcept {
    unsigned count = 0;
    for (auto &node : state().nodes) count += node.address.load(std::memory_order_acquire) == 0;
    return count;
}
/* EXIT's cancel/reset can change an older QuantizePending without submitting a
 * new chain. Adopt that owner's pending infrastructure and affected callback
 * references; execution supplies the scope for the changed Quantize state. */
void adopt_pending(Slot &slot) noexcept {
    uintptr_t quantize = word(slot.facade + 0x50);
    uintptr_t manager = word(slot.core.load(std::memory_order_relaxed) + 0xa0);
    for (unsigned offset : {0x30u, 0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u})
        capture_list(word(quantize + offset), true);
    for (unsigned offset : {0u, 8u}) {
        uintptr_t chain = word(manager + offset);
        if (!chain) continue;
        if (!table(chain, state().profile.chain_table)) {
            slot.untracked = true;
            continue;
        }
        uintptr_t node = word(chain + 0x18);
        for (unsigned n = 0; node && n < NODE_COUNT; ++n) {
            int type = command_type(node);
            if (type < 0) {
                slot.untracked = true;
                break;
            }
            if (type == AZEXIT_QUANTIZE_PENDING && word(node + 0x20) == quantize) (void) capture(node, true);
            node = word(node + 0x18);
        }
    }
}
void require_pending(const Scope &scope) noexcept {
    for (auto &node : state().nodes)
        if (node.address.load(std::memory_order_acquire) > 1 &&
            node.token.load(std::memory_order_acquire) == scope.token &&
            node.generation.load(std::memory_order_acquire) == scope.generation &&
            node.type.load(std::memory_order_acquire) == AZEXIT_QUANTIZE_PENDING)
            node.required.store(1, std::memory_order_release);
}
void quantize_cancel_hook(void *self) {
    bool owned = causal.slot && word(causal.slot->facade + 0x50) == reinterpret_cast<uintptr_t>(self);
    if (owned) {
        causal.slot->caused = true;
        adopt_pending(*causal.slot);
    }
    trampoline<void (*)(void *)> (&state().cancel_original)(self);
    /* Older work intentionally destroyed by EXIT was borrowed, not a failed
     * new command. A retained QuantizePending now carries EXIT's changed state
     * and must execute; replacement before execution is not completion. */
    if (owned) require_pending(causal);
}
void transfer_pending(void *quantize) noexcept {
    for (unsigned d = 0; d < 4; ++d) {
        Slot &slot = state().decks[d];
        if (!slot.core.load(std::memory_order_acquire)) continue;
        azsdk_native::OwnerGate::Lease lease;
        if (!slot.gate.held_by_current_thread() && slot.gate.enter(lease) != AZ_OK) continue;
        if (word(slot.facade + 0x50) != reinterpret_cast<uintptr_t>(quantize)) continue;
        uint64_t generation = slot.gate.generation();
        for (auto &node : state().nodes) {
            uintptr_t address = node.address.load(std::memory_order_acquire);
            if (address <= 1 || node.deck.load(std::memory_order_acquire) != d ||
                node.generation.load(std::memory_order_acquire) != generation ||
                !node.borrowed.load(std::memory_order_acquire) ||
                node.type.load(std::memory_order_acquire) != AZEXIT_QUANTIZE_PENDING) continue;
            if (node.address.compare_exchange_strong(address, 1, std::memory_order_acq_rel)) {
                mark_lost(slot, node.token.load(std::memory_order_acquire));
                node.address.store(0, std::memory_order_release);
            }
        }
        return;
    }
}
/* The native manager and callbacks retain actual Command references. Capture
 * them before the original can execute or dispose them, never after return. */
void manager_hook(void *self, const void *reference) {
    if (causal.slot && word(causal.slot->core.load(std::memory_order_relaxed) + 0xa0) == reinterpret_cast<uintptr_t>(self)) {
        uintptr_t chain = word(reinterpret_cast<uintptr_t>(reference));
        if (table(chain, state().profile.chain_table)) {
            unsigned count = 0;
            Scope accepted = causal;
            accepted.maintenance = causal.maintenance && future_chain(*causal.slot, chain, nullptr, count);
            accepted.setup = causal.setup && initial_chain(*causal.slot, chain);
            CausalScope scope(accepted);
            capture_list(word(chain + 0x18));
            trampoline<void (*)(void *, const void *)> (&state().manager_original)(self, reference);
            return;
        }
        else causal.slot->untracked = true;
    }
    trampoline<void (*)(void *, const void *)> (&state().manager_original)(self, reference);
}
int quantize_hook(void *self, int selector, const void *reference, const void *options, const void *property, double timing) {
    bool owned = causal.slot && word(causal.slot->facade + 0x50) == reinterpret_cast<uintptr_t>(self);
    if (owned) {
        /* RELOOP can replace a callback on an existing QuantizePending without
         * invoking EXIT's cancel helper or submitting a fresh manager chain. */
        if (causal.slot->operation != AZ_CMD_LOOP_EXIT) adopt_pending(*causal.slot);
        capture_list(word(reinterpret_cast<uintptr_t>(reference)));
    }
    int result = trampoline<int (*)(void *, int, const void *, const void *, const void *, double)>(&state().quantize_original)(self, selector, reference, options, property, timing);
    if (owned && causal.slot->operation != AZ_CMD_LOOP_EXIT && result != -1) require_pending(causal);
    /* A new native request can reuse an existing QuantizePending allocation.
     * Its bookkeeping then belongs to that request. Only an accepted handoff
     * removes the old SDK scope; SDK-created effect callbacks stay tagged. */
    if (!causal.slot && result != -1) transfer_pending(self);
    return result;
}
void jump_hook(void *self, const void *property, int mode, const void *reference, bool option, double timing) {
    Scope next = causal;
    next.setup = causal.slot && word(word(causal.slot->unit + 0x180) + 0x38) == reinterpret_cast<uintptr_t>(self) &&
        initial_request(*causal.slot, reference);
    CausalScope scope(next);
    if (next.slot && word(word(next.slot->unit + 0x180) + 0x38) == reinterpret_cast<uintptr_t>(self))
        capture_list(word(reinterpret_cast<uintptr_t>(reference)));
    reinterpret_cast<void (*)(void *, const void *, int, const void *, bool, double)>(state().profile.jump_request)(self, property, mode, reference, option, timing);
}
void loop_schedule_hook(void *self, const void *forward, const void *reverse, int mode,
                        const void *reference, bool option, double timing) {
    using Schedule = void (*)(void *, const void *, const void *, int, const void *, bool, double);
    Scope next = causal;
    next.setup = false;
    next.maintenance = causal.slot &&
        word(word(causal.slot->unit + 0x180) + 0x38) == reinterpret_cast<uintptr_t>(self) &&
        loop_schedule_matches(*causal.slot, forward, reverse, mode, reference, timing);
    CausalScope scope(next);
    if (next.maintenance) capture_list(future_hidden(*next.slot, word(reinterpret_cast<uintptr_t>(reference))));
    reinterpret_cast<Schedule>(state().profile.jump_schedule)(self, forward, reverse, mode, reference, option, timing);
}
bool hot_edit(uint32_t operation) noexcept {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    return operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR;
#else
    (void) operation;
    return false;
#endif
}
bool recall_operation(uint32_t operation) noexcept {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    return operation == AZ_CMD_MEMORY_RECALL || operation == AZ_CMD_SAVED_RECALL;
#else
    (void) operation;
    return false;
#endif
}
bool cue_operation(uint32_t operation) noexcept {
    return hot_edit(operation) || recall_operation(operation) || operation == AZ_CMD_MAIN_SET || operation == AZ_CMD_MAIN_RECALL || operation == AZ_CMD_HOT_TRIGGER;
}
bool cue_seek_operation(uint32_t operation) noexcept {
    return recall_operation(operation) || operation == AZ_CMD_MAIN_RECALL || operation == AZ_CMD_HOT_TRIGGER;
}
bool cue_position_matches(const Slot &slot) noexcept {
    uintptr_t core = slot.core.load(std::memory_order_relaxed);
    return slot.retained_before.in_valid &&
        !std::memcmp(reinterpret_cast<const void *>(core + 0x18), slot.retained_before.identity, 12) &&
        compare_timestamps(word(core + 0x28), slot.retained_before.in,
                           slot.retained_before.rate, slot.retained_before.rate, 1) == AZ_TIME_EQUAL;
}
void core_jump_hook(void *self, const void *position) {
    Slot *slot = causal.slot;
    uint64_t target = word(reinterpret_cast<uintptr_t>(position) + 0x18);
    uint64_t window = 1;
    bool planned = true;
    /* JumpController produces a source-rate-dependent forward/reverse seek
     * interval while playing. Both stock builds store that native window in
     * the nominal JumpCommand's ReversiblePositionWithSourceInfo at +0x28.
     * Borrow it only while this exact request-tagged command executes. */
    if (slot && slot->operation == AZ_CMD_HOT_TRIGGER && causal.command) {
        uintptr_t command = causal.command;
        planned = table(command, state().profile.commands[0].table) &&
            word(command + 0x20) == reinterpret_cast<uintptr_t>(self) &&
            table(command + 0x28, state().profile.interval_source_table) &&
            !std::memcmp(reinterpret_cast<const void *>(command + 0x30), slot->retained_before.identity, 12);
        if (planned) {
            uint64_t forward = word(command + 0x40), reverse = word(command + 0x48);
            const uint64_t rate = slot->retained_before.rate;
            int order = compare_timestamps(forward, reverse, rate, rate, 1);
            planned = forward <= UINT64_C(9007199254740992) && reverse <= UINT64_C(9007199254740992) && order != AZ_TIME_INVALID;
            if (planned) {
                window = (order == AZ_TIME_BEFORE ? reverse - forward : forward - reverse) + 1;
                planned = compare_timestamps(slot->retained_before.in, forward, rate, rate, window) == AZ_TIME_EQUAL &&
                    compare_timestamps(slot->retained_before.in, reverse, rate, rate, window) == AZ_TIME_EQUAL &&
                    compare_timestamps(target, forward, rate, rate, window) == AZ_TIME_EQUAL &&
                    compare_timestamps(target, reverse, rate, rate, window) == AZ_TIME_EQUAL;
            }
        }
    }
    bool owned = slot && cue_seek_operation(slot->operation) &&
        slot->core.load(std::memory_order_relaxed) == reinterpret_cast<uintptr_t>(self) &&
        slot->token.load(std::memory_order_acquire) == causal.token &&
        slot->phase.load(std::memory_order_acquire) == RUNNING &&
        !std::memcmp(reinterpret_cast<const unsigned char *>(position) + 8, slot->retained_before.identity, 12) &&
        planned && compare_timestamps(target, slot->retained_before.in,
                                      slot->retained_before.rate, slot->retained_before.rate, window) == AZ_TIME_EQUAL;
    trampoline<void (*)(void *, const void *)>(&state().core_jump_original)(self, position);
    if (owned && same_load(causal.deck, *slot) &&
        !std::memcmp(reinterpret_cast<const void *>(slot->core.load(std::memory_order_relaxed) + 0x18), slot->retained_before.identity, 12) &&
        compare_timestamps(word(slot->core.load(std::memory_order_relaxed) + 0x28), target,
                           slot->retained_before.rate, slot->retained_before.rate, window) == AZ_TIME_EQUAL) slot->cue_seek_seen = true;
}
uint32_t jump_cancel_hook(void *self) {
    if (causal.slot && word(word(causal.slot->unit + 0x180) + 0x38) == reinterpret_cast<uintptr_t>(self))
        supersede_setup(*causal.slot);
    return reinterpret_cast<uint32_t (*)(void *)>(state().profile.jump_cancel)(self);
}
bool command_hook(void *self) {
    uintptr_t address = reinterpret_cast<uintptr_t>(self);
    int type = command_type(address);
    if (type < 0) std::terminate(); /* only verified nominal slots are patched */
    Node *node = find_node(address);
    if (!node && causal.slot) node = capture(address);
    if (!node) return reinterpret_cast<bool (*)(void *)>(state().profile.commands[type].execute)(self);
    uint64_t incarnation = node->incarnation.load(std::memory_order_acquire);
    unsigned deck = node->deck.load(std::memory_order_acquire);
    uint64_t token = node->token.load(std::memory_order_acquire), generation = node->generation.load(std::memory_order_acquire);
    if (node->address.load(std::memory_order_acquire) != address ||
        node->incarnation.load(std::memory_order_acquire) != incarnation) return false;
    Slot &slot = state().decks[deck];
    azsdk_native::OwnerGate::Lease lease;
    bool held = slot.gate.held_by_current_thread();
    if ((!held && slot.gate.enter(lease) != AZ_OK) || slot.gate.generation() != generation ||
        slot.token.load(std::memory_order_acquire) != token || slot.phase.load(std::memory_order_acquire) != RUNNING ||
        !same_load(deck, slot)) {
        mark_lost(slot, token);
        return false; /* a retired incarnation never follows raw native owners */
    }
    uint64_t time = now();
    if (!time || compare_timestamps(time, slot.request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL ||
        reinterpret_cast<bool (*)(void *)>(state().profile.affinity)(reinterpret_cast<void *>(slot.core.load(std::memory_order_relaxed) + 0x110))) {
        mark_lost(slot, token);
        return false;
    }
    node->executed.store(1, std::memory_order_release);
    CausalScope scope({&slot, deck, token, generation, false, false, address});
    try {
        return reinterpret_cast<bool (*)(void *)>(state().profile.commands[type].execute)(self);
    } catch (...) {
        slot.failed.store(token, std::memory_order_release);
        throw; /* preserve the original native exception contract */
    }
}
void destroy_command(void *self, bool deleting) {
    uintptr_t address = reinterpret_cast<uintptr_t>(self);
    int type = command_type(address);
    if (type < 0) std::terminate();
    if (Node *node = find_node(address)) {
        uintptr_t expected = address;
        if (node->address.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            uint64_t token = node->token.load(std::memory_order_acquire);
            Slot &slot = state().decks[node->deck.load(std::memory_order_acquire)];
            if (!node->executed.load(std::memory_order_acquire) && node->required.load(std::memory_order_acquire) &&
                !node->maintenance.load(std::memory_order_acquire))
            {
                mark_lost(slot, token);
            }
            node->address.store(0, std::memory_order_release); /* before pool reuse */
        }
    }
    const auto &p = state().profile.commands[type];
    reinterpret_cast<void (*)(void *)>(deleting ? p.deleting : p.destroy)(self);
}
void destroy_hook(void *self) { destroy_command(self, false); }
void deleting_hook(void *self) { destroy_command(self, true); }
/* Native CueProperty::isActiveLoopPlay, exactly mirrored on both stock ELFs:
 * 13ec228 / 14a7668, over the working property at context+160. */
bool engaged(Slot &slot, bool &value) noexcept {
    uintptr_t context = word(slot.facade + 0x68);
    if (!pointer(context)) return false;
    uintptr_t p = context + 0x160;
    value = byte(p + 0x30) && (byte(p + 0x34) & 0x1f) && byte(p + 0x36) &&
            integer(p + 0x38) != 0 && word(p + 0x40) != UINT64_C(0x7fffffffffffffff);
    return true;
}

bool extended(uint32_t operation) noexcept {
    return operation >= AZ_CMD_LOOP_IN && operation <= AZ_CMD_LOOP_ADJUST_OUT &&
           operation != AZ_CMD_LOOP_EXIT && operation != AZ_CMD_RELOOP;
}
bool entering(uint32_t operation) noexcept { return operation == AZ_CMD_RELOOP || operation == AZ_CMD_LOOP_ENTER; }
bool set_bindings(Slot &slot) noexcept {
    const auto &p = state().profile;
    uintptr_t setter = slot.facade + 0xc8;
    return table(setter, p.set_table) &&
           word(setter + 0x10) == slot.core.load(std::memory_order_relaxed) + 0x110 &&
           word(setter + 0x20) == word(slot.facade + 0x28) &&
           word(setter + 0x48) == word(slot.facade + 0x50) &&
           word(setter + 0x60) == word(slot.facade + 0x68) && word(p.set_table + 2 * 8) == p.set_point &&
           word(p.set_table + 5 * 8) == p.clear_cue && word(p.set_table + 9 * 8) == p.loop_out &&
           word(p.set_table + 14 * 8) == p.adjust_in && word(p.set_table + 15 * 8) == p.adjust_out &&
           word(p.set_table + 16 * 8) == p.length_beat && word(p.set_table + 17 * 8) == p.length_square;
}
bool valid_position(uintptr_t source) noexcept {
    return byte(source) && (byte(source + 4) & 0x1f) && byte(source + 6) &&
           integer(source + 8) != 0 && word(source + 16) <= UINT64_C(9007199254740992);
}
bool empty_position(uintptr_t source) noexcept {
    /* Native empty constructor values are absence markers, not timestamps.
     * A malformed or foreign populated source must still refuse HOT_CLEAR. */
    return !word(source) && !integer(source + 8) &&
        word(source + 16) == UINT64_C(0x7fffffffffffffff) &&
        !word(source + 0x20) && integer(source + 0x30) == 0x7fffffff &&
        word(source + 0x38) == UINT64_C(0x7fffffffffffffff);
}
LoopPoints points(uintptr_t property) noexcept {
    LoopPoints result;
    std::memcpy(result.identity, reinterpret_cast<const void *>(property + 0x30), 12);
    result.in_valid = valid_position(property + 0x30);
    result.out_valid = valid_position(property + 0x88);
    result.in = word(property + 0x40);
    result.out = word(property + 0x98);
    result.rate = unsigned(integer(property + 0x60));
    result.source = word(property + 0x50);
    result.beat = unsigned(integer(property + 0xd8));
    result.mode = unsigned(integer(property + 0xc8));
    if (!result.rate || result.rate >= 0x7fffffffu || !result.source) result.in_valid = result.out_valid = false;
    /* Stored-cue reconstruction can allocate a separate Source object for
     * each endpoint. SourceInfo and rate identify the same native source;
     * the two counted object addresses need not be identical. */
    if (std::memcmp(result.identity, reinterpret_cast<const void *>(property + 0x88), 12) ||
        unsigned(integer(property + 0xb8)) != result.rate || !word(property + 0xa8))
        result.out_valid = false;
    return result;
}
uintptr_t working(Slot &slot) noexcept { return word(slot.facade + 0x68) + 0x160; }
bool complete_points(const LoopPoints &p) noexcept { return p.in_valid && p.out_valid && compare_timestamps(p.out, p.in, p.rate, p.rate, 1) == AZ_TIME_AFTER; }
bool same_source(const LoopPoints &a, const LoopPoints &b) noexcept { return a.rate == b.rate && a.source == b.source && !std::memcmp(a.identity, b.identity, 12); }
struct LoopView {
    uintptr_t property, holder;
    LoopPoints points;
};
bool loop_view(Slot &slot, LoopView &view) noexcept {
    bool active = false;
    if (!engaged(slot, active) || !active) return false;
    view.property = working(slot);
    view.points = points(view.property);
    view.holder = word(view.property + 0x58);
    return complete_points(view.points);
}
bool interval_matches(uintptr_t object, const LoopView &view, bool reverse, bool sourced) noexcept {
    const auto &p = state().profile;
    return table(object, sourced ? p.interval_source_table : p.interval_table) &&
           !std::memcmp(reinterpret_cast<void *>(object + 8), view.points.identity, 12) &&
           compare_timestamps(word(object + 0x18), reverse ? view.points.out : view.points.in, view.points.rate, view.points.rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(word(object + 0x20), reverse ? view.points.in : view.points.out, view.points.rate, view.points.rate, 1) == AZ_TIME_EQUAL &&
           (!sourced || word(object + 0x28) == view.holder);
}
bool loop_callback(Slot &slot, uintptr_t loop, const LoopView &view, uintptr_t &play) noexcept {
    const auto &p = state().profile;
    if (!table(loop, p.commands[3].table) || word(loop + 0x20) != word(slot.unit + 0x180) ||
        word(loop + 0x28) != view.property || !interval_matches(loop + 0x38, view, false, true) ||
        !interval_matches(loop + 0x68, view, true, false)) return false;
    play = word(loop + 0x18);
    return !play || (table(play, p.commands[1].table) &&
                    word(play + 0x20) == slot.core.load(std::memory_order_relaxed) && !word(play + 0x18));
}
/* Quantize's interval request wraps an owner-held Loop callback. The visible
 * wrapper and its hidden callback are both stock future playback work; the
 * selector and exact embedded owner distinguish them from a pending SDK effect. */
uintptr_t future_hidden(Slot &slot, uintptr_t wrapper) noexcept {
    const auto &p = state().profile;
    uintptr_t quantize = word(slot.facade + 0x50), embedded = quantize + 0x20;
    if (!table(wrapper, p.commands[13].table) || word(wrapper + 0x20) != embedded ||
        integer(wrapper + 0x28) != 2 || integer(embedded + 8) != 2 ||
        word(embedded) != word(word(slot.unit + 0x180) + 0x38) || word(embedded + 0x20) != quantize) return 0;
    uintptr_t play = word(wrapper + 0x18);
    if (play && (!table(play, p.commands[1].table) ||
                 word(play + 0x20) != slot.core.load(std::memory_order_relaxed) || word(play + 0x18))) return 0;
    LoopView view = {};
    uintptr_t hidden = word(embedded + 0x10), hidden_play = 0;
    return loop_view(slot, view) && loop_callback(slot, hidden, view, hidden_play) && !hidden_play ? hidden : 0;
}
bool loop_schedule_matches(Slot &slot, const void *forward, const void *reverse, int mode,
                           const void *reference, double timing) noexcept {
    LoopView view = {};
    uintptr_t play = 0;
    return mode == 0 && timing == std::numeric_limits<double>::max() && loop_view(slot, view) &&
           interval_matches(reinterpret_cast<uintptr_t>(forward), view, false, true) &&
           interval_matches(reinterpret_cast<uintptr_t>(reverse), view, true, false) &&
           pointer(reinterpret_cast<uintptr_t>(reference)) &&
           (loop_callback(slot, word(reinterpret_cast<uintptr_t>(reference)), view, play) ||
            future_hidden(slot, word(reinterpret_cast<uintptr_t>(reference))));
}
bool future_chain(Slot &slot, uintptr_t chain, uintptr_t *addresses, unsigned &count) noexcept {
    const auto &p = state().profile;
    LoopView view = {};
    if (!table(chain, p.chain_table) || !loop_view(slot, view) ||
        !interval_matches(chain + 0x20, view, true, false) ||
        (byte(chain + 0x48) != 0 && byte(chain + 0x48) != 3) || byte(chain + 0x49)) return false;
    double timing;
    std::memcpy(&timing, reinterpret_cast<void *>(chain + 0x50), sizeof timing);
    if (timing != std::numeric_limits<double>::max()) return false;
    uintptr_t jump = word(chain + 0x18), play = 0;
    if (!table(jump, p.commands[0].table) || word(jump + 0x20) != slot.core.load(std::memory_order_relaxed) ||
        !interval_matches(jump + 0x28, view, false, true)) return false;
    uintptr_t callback = word(jump + 0x18), hidden = 0;
    if (!loop_callback(slot, callback, view, play)) {
        hidden = future_hidden(slot, callback);
        if (!hidden) return false;
        play = word(callback + 0x18);
    }
    for (uintptr_t address : {jump, callback, play, hidden}) {
        if (!address) continue;
        if (count >= 8) return false;
        if (addresses) addresses[count] = address;
        ++count;
    }
    return true;
}
bool initial_request(Slot &slot, const void *reference) noexcept {
    LoopView view = {};
    uintptr_t play = 0;
    return pointer(reinterpret_cast<uintptr_t>(reference)) && loop_view(slot, view) &&
        loop_callback(slot, word(reinterpret_cast<uintptr_t>(reference)), view, play);
}
bool initial_chain(Slot &slot, uintptr_t chain) noexcept {
    const auto &p = state().profile;
    LoopView view = {};
    if (!table(chain, p.chain_table) || byte(chain + 0x49) || !loop_view(slot, view)) return false;
    double timing;
    std::memcpy(&timing, reinterpret_cast<void *>(chain + 0x50), sizeof timing);
    if (timing != std::numeric_limits<double>::max() ||
        (byte(chain + 0x48) != 0 && byte(chain + 0x48) != 3)) return false;
    uintptr_t jump = word(chain + 0x18), play = 0;
    if (!table(jump, p.commands[0].table) || word(jump + 0x20) != slot.core.load(std::memory_order_relaxed) ||
        !table(jump + 0x28, p.interval_source_table) ||
        std::memcmp(reinterpret_cast<void *>(jump + 0x30), view.points.identity, 12) ||
        word(jump + 0x40) > UINT64_C(9007199254740992) || word(jump + 0x48) > UINT64_C(9007199254740992) ||
        word(jump + 0x50) != view.holder || !table(chain + 0x20, p.interval_table) ||
        std::memcmp(reinterpret_cast<void *>(chain + 0x28), view.points.identity, 12) ||
        word(chain + 0x38) > UINT64_C(9007199254740992) || word(chain + 0x40) > UINT64_C(9007199254740992)) return false;
    return loop_callback(slot, word(jump + 0x18), view, play);
}
/* Native OUT initialization can replace an initial position-jump chain before
 * its first execution. Only its exact Loop callback proves this is loop setup;
 * an ordinary one-shot Jump remains a required effect. The replacement must
 * subsequently produce a verified recurring chain before we can acknowledge it. */
void supersede_setup(Slot &slot) noexcept {
    if (slot.phase.load(std::memory_order_acquire) != RUNNING ||
        causal.token != slot.token.load(std::memory_order_acquire) ||
        causal.generation != slot.gate.generation() ||
        !(extended(slot.operation) || entering(slot.operation) || cue_operation(slot.operation))) return;
    uintptr_t manager = word(slot.core.load(std::memory_order_relaxed) + 0xa0);
    for (unsigned offset : {0u, 8u}) {
        uintptr_t chain = word(manager + offset);
        if (!initial_chain(slot, chain)) continue;
        uintptr_t jump = word(chain + 0x18), loop = word(jump + 0x18), play = word(loop + 0x18);
        Node *nodes[3] = {};
        unsigned count = 0;
        bool valid = true;
        for (uintptr_t address : {jump, loop, play}) {
            if (!address) continue;
            Node *node = find_node(address);
            if (!node || node->token.load(std::memory_order_acquire) != causal.token ||
                node->generation.load(std::memory_order_acquire) != causal.generation ||
                node->executed.load(std::memory_order_acquire) || node->borrowed.load(std::memory_order_acquire) ||
                node->required.load(std::memory_order_acquire) != 2) {
                valid = false;
                break;
            }
            nodes[count++] = node;
        }
        if (!valid) continue;
        for (unsigned i = 0; i < count; ++i) nodes[i]->required.store(0, std::memory_order_release);
        slot.maintenance_seen = true;
    }
}
bool settled_work(Slot &slot, uint64_t token) noexcept {
    uintptr_t quantize = word(slot.facade + 0x50);
    if (integer(quantize + 0x98) != -1) return false;
    for (unsigned offset : {0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u})
        if (word(quantize + offset)) return false;
    uintptr_t manager = word(slot.core.load(std::memory_order_relaxed) + 0xa0);
    uintptr_t addresses[8] = {};
    unsigned count = 0;
    for (unsigned offset : {0u, 8u}) {
        uintptr_t chain = word(manager + offset);
        if (chain && !future_chain(slot, chain, addresses, count)) return false;
    }
    bool wrapped = false;
    for (unsigned i = 0; i < count; ++i) wrapped |= table(addresses[i], state().profile.commands[13].table);
    if (word(quantize + 0x30) && !wrapped) return false;
    if (!count) {
        // A Memory point recall deliberately cancels the active loop's setup.
        // Unlike OUT/RELOOP, it must not produce a replacement wrap. Keep the
        // empty-manager/node proof; cue_postcondition still requires this
        // request's seek, source-bound point, disengagement and transport pause.
        bool point_recall = slot.operation == AZ_CMD_MEMORY_RECALL &&
            slot.retained_before.in_valid && !slot.retained_before.out_valid;
        return (!slot.maintenance_seen || point_recall) && !pending_nodes(token) && idle(slot);
    }
    uint64_t generation = slot.gate.generation();
    for (auto &node : state().nodes) {
        uintptr_t address = node.address.load(std::memory_order_acquire);
        if (address <= 1 || node.token.load(std::memory_order_acquire) != token) continue;
        if (node.generation.load(std::memory_order_acquire) != generation || !node.maintenance.load(std::memory_order_acquire)) return false;
        bool owned = false;
        for (unsigned i = 0; i < count; ++i) owned |= addresses[i] == address;
        if (!owned) return false;
    }
    /* These exact continuations are held by the constructed unit's native
     * manager. The SDK adds no references: return ongoing playback to stock
     * ownership instead of extending a one-shot request through every wrap. */
    for (auto &node : state().nodes) {
        uintptr_t address = node.address.load(std::memory_order_acquire);
        if (address <= 1 || node.token.load(std::memory_order_acquire) != token ||
            node.generation.load(std::memory_order_acquire) != generation) continue;
        if (!node.address.compare_exchange_strong(address, 1, std::memory_order_acq_rel)) return false;
        node.address.store(0, std::memory_order_release);
    }
    slot.maintenance_seen = false;
    return true;
}
bool shifted(uint64_t value, double seconds, uint32_t rate, uint64_t &out) noexcept {
    /* Mirror the two verified FCVTZS expression orders; reject ranges that
     * could overflow native signed arithmetic before the firmware sees them. */
    double milliseconds = seconds * 1000.0;
    double samples = state().build == 1 ? (milliseconds * rate) / 1000.0 : (milliseconds * 0.001) * rate;
    if (!std::isfinite(samples) || std::fabs(samples) > 9007199254740992.0) return false;
    int64_t delta = static_cast<int64_t>(samples);
    int64_t target = static_cast<int64_t>(value) + delta;
    if (!delta || target < 0 || target > INT64_C(9007199254740992)) return false;
    out = uint64_t(target);
    return true;
}
void adjust(Slot &slot, bool in, double seconds) {
    void *setter = reinterpret_cast<void *>(slot.facade + 0xc8);
    const auto &p = state().profile;
    /* Verified policy0 selects the native 13.33ms minimum. LoopType1 skips
     * copying working points into the selected retained cue; no saved edit. */
    if (in) reinterpret_cast<void (*)(void *, double, int)>(p.adjust_in)(setter, seconds * 1000.0, 0);
    else reinterpret_cast<void (*)(void *, double, int, int)>(p.adjust_out)(setter, seconds * 1000.0, 1, 0);
}
bool loop_context_safe(Slot &slot) noexcept {
    if (slot.operation != AZ_CMD_LOOP_ADJUST_IN && slot.operation != AZ_CMD_LOOP_MOVE) return true;
    uintptr_t context = word(slot.facade + 0x68);
    /* Native IN adjustment also copies working points into the selected cue.
     * Restrict that effect to the transient default cue in the ordinary mode. */
    return integer(context + 0x14) == 0 && !byte(context + 0x380);
}
bool prepare_loop(Slot &slot) noexcept {
    if (!set_bindings(slot) || !loop_context_safe(slot)) return false;
    slot.before = points(working(slot));
    uintptr_t context = word(slot.facade + 0x68);
    slot.retained = word(context + 0x18); /* native CueKind0, separate from working */
    if (!table(slot.retained, state().profile.cue_table)) return false;
    slot.retained_before = points(slot.retained);
    slot.expected_in = slot.before.in;
    slot.expected_out = slot.before.out;
    slot.move_stage = 0;
    switch (slot.operation) {
    case AZ_CMD_LOOP_OUT: return !slot.initially_engaged && slot.retained_before.in_valid;
    case AZ_CMD_LOOP_IN:
    case AZ_CMD_LOOP_CLEAR:
    case AZ_CMD_LOOP_ENTER: return true;
    default: break;
    }
    if (!slot.initially_engaged || !complete_points(slot.before)) return false;
    if (slot.operation == AZ_CMD_LOOP_MOVE || slot.operation == AZ_CMD_LOOP_ADJUST_IN)
        if (!shifted(slot.before.in, slot.arguments.position_s, slot.before.rate, slot.expected_in)) return false;
    if (slot.operation == AZ_CMD_LOOP_MOVE || slot.operation == AZ_CMD_LOOP_ADJUST_OUT)
        if (!shifted(slot.before.out, slot.operation == AZ_CMD_LOOP_MOVE ? slot.arguments.position_s : slot.arguments.end_s,
                     slot.before.rate, slot.expected_out)) return false;
    return compare_timestamps(slot.expected_out, slot.expected_in, slot.before.rate, slot.before.rate, 1) == AZ_TIME_AFTER;
}
bool cue_playing(const Slot &slot, bool &playing, bool *auxiliary = nullptr) noexcept {
    uintptr_t pause = word(slot.facade + 0x58), bundle = word(slot.facade + 0x78);
    uintptr_t core = slot.core.load(std::memory_order_relaxed);
    if (!table(pause, state().profile.play_pause_table) || !pointer(bundle) ||
        word(pause + 0x58) != core || word(bundle) != core ||
        word(bundle + 8) != word(slot.facade + 0x68) || word(bundle + 0xa8) != pause) return false;
    unsigned value = byte(pause + 8), other = byte(pause + 9);
    if (value > 1 || other > 1) return false;
    playing = value != 0;
    if (auxiliary) *auxiliary = other != 0;
    return true;
}
bool prepare_cue(Slot &slot) noexcept {
    const auto &p = state().profile;
    if (!cue_operation(slot.operation)) return false;
    if ((slot.operation == AZ_CMD_MAIN_SET || hot_edit(slot.operation)) && !set_bindings(slot)) return false;
    uintptr_t context = word(slot.facade + 0x68);
    unsigned kind = slot.operation == AZ_CMD_HOT_TRIGGER || hot_edit(slot.operation) ? slot.cue_arguments.entry_id : 0;
    if (kind > 8) return false;
    slot.retained = word(context + 0x18 + kind * 8);
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    if (recall_operation(slot.operation))
        slot.retained = reinterpret_cast<uintptr_t>(azsdk_cue_recall_property(unsigned(&slot - state().decks), slot.request.token));
#endif
    if (!table(slot.retained, p.cue_table) && !table(slot.retained, p.cue_base_table)) return false;
    slot.before = points(working(slot));
    slot.retained_before = points(slot.retained);
    std::memcpy(slot.cue_source_before, reinterpret_cast<const void *>(slot.core.load(std::memory_order_relaxed) + 0x18), 12);
    slot.cue_seek_seen = false;
    slot.staged_cue = {};
    slot.cue_rgb_before = (byte(slot.retained + 0x10) << 16) | (byte(slot.retained + 0x11) << 8) | byte(slot.retained + 0x12);
    if (!cue_playing(slot, slot.cue_playing_before, &slot.cue_aux_before)) return false;
    if (slot.operation == AZ_CMD_HOT_TRIGGER && integer(word(slot.facade + 0x70) + 0x104)) return false;
    if (slot.operation == AZ_CMD_HOT_SET && slot.initially_engaged && !complete_points(slot.before)) return false;
    return slot.operation == AZ_CMD_MAIN_SET || slot.operation == AZ_CMD_HOT_SET ||
        (slot.operation == AZ_CMD_HOT_CLEAR && empty_position(slot.retained + 0x30) && empty_position(slot.retained + 0x88)) ||
        (slot.retained_before.in_valid && !std::memcmp(slot.retained_before.identity, slot.cue_source_before, 12));
}
void dispatch_cue(Slot &slot) {
    const auto &p = state().profile;
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    if (recall_operation(slot.operation)) {
        if (azsdk_cue_recall_dispatch(unsigned(&slot - state().decks), slot.request.token,
                reinterpret_cast<void *>(slot.core.load(std::memory_order_relaxed)), reinterpret_cast<void *>(slot.facade)) != AZ_OK) throw std::exception();
        return;
    }
#endif
    if (slot.operation == AZ_CMD_HOT_SET) {
        azexit_native_color color = {static_cast<unsigned char>(slot.cue_arguments.rgb >> 16), static_cast<unsigned char>(slot.cue_arguments.rgb >> 8), static_cast<unsigned char>(slot.cue_arguments.rgb)};
        if (!p.set_hot || word(p.set_table + 4 * 8) != p.set_hot) throw std::exception();
        reinterpret_cast<void (*)(void *, int, azexit_native_color, int)>(p.set_hot)(reinterpret_cast<void *>(slot.facade + 0xc8), int(slot.cue_arguments.entry_id), color, 1);
    } else if (slot.operation == AZ_CMD_HOT_CLEAR)
        reinterpret_cast<void (*)(void *, int)>(p.clear_cue)(reinterpret_cast<void *>(slot.facade + 0xc8), int(slot.cue_arguments.entry_id));
    else if (slot.operation == AZ_CMD_MAIN_SET)
        reinterpret_cast<void (*)(void *, int, int)>(p.set_point)(reinterpret_cast<void *>(slot.facade + 0xc8), 0, 1);
    else if (slot.operation == AZ_CMD_MAIN_RECALL)
        reinterpret_cast<void (*)(void *, int)>(p.control_backcue)(reinterpret_cast<void *>(slot.facade), 0);
    else if (slot.operation == AZ_CMD_HOT_TRIGGER)
        reinterpret_cast<void (*)(void *, int, int, int)>(p.control_jump)(reinterpret_cast<void *>(slot.facade), int(slot.cue_arguments.entry_id), 0, 0);
    else throw std::exception();
}
bool cue_postcondition(Slot &slot) noexcept {
    bool playing = false, auxiliary = false;
    if (!cue_playing(slot, playing, &auxiliary)) return false;
    if (recall_operation(slot.operation)) {
        LoopPoints current = points(word(word(slot.facade + 0x68) + 0x18));
        bool engaged_now = false;
        bool loop_ok = slot.operation != AZ_CMD_MEMORY_RECALL ||
            (engaged(slot, engaged_now) && !engaged_now);
        if (complete_points(slot.retained_before)) {
            LoopPoints active = points(working(slot));
            loop_ok = engaged(slot, engaged_now) && engaged_now && complete_points(active) &&
                same_source(active, slot.retained_before) && compare_timestamps(active.in, slot.retained_before.in, active.rate, active.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(active.out, slot.retained_before.out, active.rate, active.rate, 1) == AZ_TIME_EQUAL;
        }
        return slot.cue_seek_seen && current.in_valid && compare_timestamps(current.in, slot.retained_before.in, current.rate, current.rate, 1) == AZ_TIME_EQUAL &&
            same_source(current, slot.retained_before) && !playing && !auxiliary && loop_ok &&
            current.out_valid == slot.retained_before.out_valid &&
            (!current.out_valid || compare_timestamps(current.out, slot.retained_before.out, current.rate, current.rate, 1) == AZ_TIME_EQUAL);
    }
    unsigned kind = slot.operation == AZ_CMD_HOT_TRIGGER || hot_edit(slot.operation) ? slot.cue_arguments.entry_id : 0;
    if (word(word(slot.facade + 0x68) + 0x18 + kind * 8) != slot.retained) return false;
    LoopPoints retained = points(slot.retained);
    if ((hot_edit(slot.operation) || slot.operation == AZ_CMD_MAIN_SET) &&
        (playing != slot.cue_playing_before || auxiliary != slot.cue_aux_before)) return false;
    if (slot.operation == AZ_CMD_HOT_CLEAR) return !retained.in_valid && !retained.out_valid;
    if (slot.operation == AZ_CMD_HOT_SET) {
        unsigned rgb = (byte(slot.retained + 0x10) << 16) | (byte(slot.retained + 0x11) << 8) | byte(slot.retained + 0x12);
        bool loop_matches = slot.initially_engaged ? complete_points(retained) && same_source(retained, slot.before) &&
            compare_timestamps(retained.in, slot.before.in, retained.rate, retained.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(retained.out, slot.before.out, retained.rate, retained.rate, 1) == AZ_TIME_EQUAL : !retained.out_valid;
        bool matches = loop_matches && retained.in_valid && !std::memcmp(retained.identity, slot.cue_source_before, 12) && rgb == slot.cue_arguments.rgb &&
            (!slot.retained_before.in_valid || compare_timestamps(retained.in, slot.retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL ||
             retained.out_valid != slot.retained_before.out_valid || (retained.out_valid && compare_timestamps(retained.out, slot.retained_before.out, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL) ||
             rgb != slot.cue_rgb_before || !same_source(retained, slot.retained_before));
        if (matches) {
            slot.staged_cue.in_sample = retained.in;
            slot.staged_cue.out_sample = retained.out;
            slot.staged_cue.sample_rate = retained.rate;
            slot.staged_cue.out_valid = retained.out_valid;
            std::memcpy(slot.staged_cue.source_id, retained.identity, 12);
        }
        return matches;
    }
    if (slot.operation == AZ_CMD_MAIN_SET)
        return retained.in_valid && !std::memcmp(retained.identity, slot.cue_source_before, 12) &&
            (!slot.retained_before.in_valid || compare_timestamps(retained.in, slot.retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL);
    if (!retained.in_valid || compare_timestamps(retained.in, slot.retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL || !same_source(retained, slot.retained_before)) return false;
    if (slot.operation == AZ_CMD_MAIN_RECALL)
        return slot.cue_seek_seen && cue_position_matches(slot) && !playing && !auxiliary;
    return slot.cue_seek_seen && playing;
}
void dispatch_loop(Slot &slot) {
    const auto &p = state().profile;
    void *setter = reinterpret_cast<void *>(slot.facade + 0xc8);
    switch (slot.operation) {
    case AZ_CMD_LOOP_IN: reinterpret_cast<void (*)(void *, int, int)>(p.set_point)(setter, 0, 1); break;
    case AZ_CMD_LOOP_OUT: reinterpret_cast<void (*)(void *, int, int)>(p.loop_out)(setter, 0, 0); break;
    case AZ_CMD_LOOP_CLEAR: reinterpret_cast<void (*)(void *, int)>(p.clear_cue)(setter, 0); break;
    case AZ_CMD_LOOP_ENTER: reinterpret_cast<void (*)(void *, int)>(p.reloop)(reinterpret_cast<void *>(slot.facade), 0); break;
    case AZ_CMD_LOOP_DOUBLE:
    case AZ_CMD_LOOP_HALVE:
        reinterpret_cast<void (*)(void *, int, int, int)>(p.length_square)(setter, slot.operation == AZ_CMD_LOOP_DOUBLE ? 1 : -1, 1, 0);
        break;
    case AZ_CMD_LOOP_LENGTH: {
        uint32_t beat = 0;
        if (!azexit_pack_beat(slot.arguments.beats, &beat)) throw std::exception();
        reinterpret_cast<void (*)(void *, uint32_t, int, int)>(p.length_beat)(setter, beat, 1, 0);
        break;
    }
    case AZ_CMD_LOOP_ADJUST_IN: adjust(slot, true, slot.arguments.position_s); break;
    case AZ_CMD_LOOP_ADJUST_OUT: adjust(slot, false, slot.arguments.end_s); break;
    case AZ_CMD_LOOP_MOVE:
        /* Expand outward before moving the other edge. Each stage must settle
         * before the next starts. This is explicit sequencing, not rollback. */
        slot.move_stage = 1;
        adjust(slot, compare_timestamps(slot.arguments.position_s, 0., 1, slot.before.rate, 1) == AZ_TIME_BEFORE, slot.arguments.position_s);
        break;
    default: throw std::exception();
    }
}
bool beat_equal(uint32_t actual, uint32_t expected, unsigned numerator_scale = 1, unsigned denominator_scale = 1) noexcept {
    uint64_t an = actual & 0xffffu, ad = actual >> 16;
    uint64_t en = expected & 0xffffu, ed = expected >> 16;
    return an && ad && en && ed && an * ed * denominator_scale == en * ad * numerator_scale;
}
bool loop_postcondition(Slot &slot) noexcept {
    if (word(word(slot.facade + 0x68) + 0x18) != slot.retained || !table(slot.retained, state().profile.cue_table)) return false;
    LoopPoints after = points(working(slot)), retained = points(slot.retained);
    switch (slot.operation) {
    case AZ_CMD_LOOP_IN:
        return retained.in_valid && (!slot.retained_before.in_valid || compare_timestamps(retained.in, slot.retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL);
    case AZ_CMD_LOOP_OUT: {
        bool active = false;
        return engaged(slot, active) && active && complete_points(after) &&
               compare_timestamps(after.in, slot.retained_before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && same_source(after, slot.retained_before) &&
               (!slot.before.out_valid || compare_timestamps(after.out, slot.before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL);
    }
    case AZ_CMD_LOOP_CLEAR:
        return !retained.in_valid && !retained.out_valid;
    default: break;
    }
    if (!complete_points(after) || !same_source(slot.before, after)) return false;
    switch (slot.operation) {
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN:
    case AZ_CMD_LOOP_ADJUST_OUT:
        return compare_timestamps(after.in, slot.expected_in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(after.out, slot.expected_out, after.rate, after.rate, 1) == AZ_TIME_EQUAL;
    case AZ_CMD_LOOP_DOUBLE:
        return compare_timestamps(after.in, slot.before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(after.out, slot.before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL &&
               (slot.before.mode == 1 ? after.mode == 1 && beat_equal(after.beat, slot.before.beat, 2, 1)
                                      : compare_timestamps(after.out - after.in, 2 * (slot.before.out - slot.before.in), after.rate, after.rate, 1) == AZ_TIME_EQUAL);
    case AZ_CMD_LOOP_HALVE:
        return compare_timestamps(after.in, slot.before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(after.out, slot.before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL &&
               (slot.before.mode == 1 ? after.mode == 1 && beat_equal(after.beat, slot.before.beat, 1, 2)
                                      : compare_timestamps(after.out - after.in, (slot.before.out - slot.before.in) / 2, after.rate, after.rate, 1) == AZ_TIME_EQUAL);
    case AZ_CMD_LOOP_LENGTH: {
        uint32_t beat = 0;
        return azexit_pack_beat(slot.arguments.beats, &beat) && after.mode == 1 && beat_equal(after.beat, beat) &&
               compare_timestamps(after.in, slot.before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && compare_timestamps(after.out, slot.before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL;
    }
    default: return false;
    }
}

void finish(Slot &slot, unsigned outcome, int reason, uint64_t revision = 0) noexcept {
    slot.result = {slot.request.token, slot.request.load_generation, revision, outcome, reason};
    slot.phase.store(READY, std::memory_order_release);
}
void execute_move_tail(unsigned deck, Slot &slot) noexcept {
    uint64_t time = now(), token = slot.request.token;
    if (slot.untracked) return;
    if (slot.failed.load(std::memory_order_acquire) == token || slot.lost.load(std::memory_order_acquire) == token ||
        slot.loss_overflow.load(std::memory_order_acquire)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        return;
    }
    if (!time || compare_timestamps(time, slot.request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL || !same_load(deck, slot)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, time ? AZ_STALE : AZ_FAILED);
        return;
    }
    try {
        if (!coherent(slot) || !bindings(slot) || !set_bindings(slot) || !loop_context_safe(slot) ||
            reinterpret_cast<bool (*)(void *)>(state().profile.affinity)(reinterpret_cast<void *>(slot.core.load(std::memory_order_relaxed) + 0x110))) {
            finish(slot, AZ_COMMAND_UNCERTAIN, AZ_UNSUPPORTED);
            return;
        }
        if (!settled_work(slot, token)) return;
        LoopPoints current = points(working(slot));
        bool positive = compare_timestamps(slot.arguments.position_s, 0., 1, slot.before.rate, 1) == AZ_TIME_AFTER;
        if (!complete_points(current) || !same_source(slot.before, current) ||
            compare_timestamps(current.in, positive ? slot.before.in : slot.expected_in, current.rate, current.rate, 1) != AZ_TIME_EQUAL ||
            compare_timestamps(current.out, positive ? slot.expected_out : slot.before.out, current.rate, current.rate, 1) != AZ_TIME_EQUAL) {
            finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            return;
        }
        CausalScope scope({&slot, deck, token, slot.gate.generation(), false, false, 0});
        slot.move_stage = 3;
        adjust(slot, positive, slot.arguments.position_s);
    } catch (...) {
        slot.failed.store(token, std::memory_order_release);
        slot.move_stage = 3;
    }
}
void execute(unsigned deck, Slot &slot) noexcept {
    if (slot.phase.load(std::memory_order_acquire) == RUNNING &&
        slot.operation == AZ_CMD_LOOP_MOVE && slot.move_stage == 2) {
        execute_move_tail(deck, slot);
        return;
    }
    unsigned expected = QUEUED;
    if (!slot.phase.compare_exchange_strong(expected, RUNNING, std::memory_order_acq_rel)) return;
    if (!(state().mask.load(std::memory_order_acquire) & (1u << deck))) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    uint64_t time = now();
    if (!time || compare_timestamps(time, slot.request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        finish(slot, AZ_COMMAND_CANCELLED, time ? AZ_STALE : AZ_FAILED);
        return;
    }
    if (!azsdk_load_read(int(deck), &slot.load) || slot.load.load_generation != slot.request.load_generation) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    try {
        bool owner_coherent = coherent(slot);
        bool owner_bindings = owner_coherent && bindings(slot);
        bool owner_wrong_thread = owner_bindings && reinterpret_cast<bool (*)(void *)>(state().profile.affinity)(reinterpret_cast<void *>(slot.core.load(std::memory_order_relaxed) + 0x110));
        if (!owner_coherent || !owner_bindings || owner_wrong_thread) {
            finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            return;
        }
        if (slot.revision == UINT64_MAX || free_nodes() < 64) {
            finish(slot, AZ_COMMAND_REJECTED, slot.revision == UINT64_MAX ? AZ_LIMIT : AZ_BUSY);
            return;
        }
        time = now();
        if (!time || compare_timestamps(time, slot.request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL || !same_load(deck, slot)) {
            finish(slot, AZ_COMMAND_REJECTED, time ? AZ_STALE : AZ_FAILED);
            return;
        }
        slot.untracked = false;
        slot.maintenance_seen = false;
        slot.captured = 0;
        slot.caused = false;
        bool before;
        if (!engaged(slot, before)) {
            finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            return;
        }
        slot.initially_engaged = before;
        if ((extended(slot.operation) && !prepare_loop(slot)) || (cue_operation(slot.operation) && !prepare_cue(slot))) {
            finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            return;
        }
        CausalScope scope({&slot, deck, slot.request.token, slot.gate.generation(), false, false, 0});
        if (cue_operation(slot.operation)) dispatch_cue(slot);
        else if (extended(slot.operation)) dispatch_loop(slot);
        else if (slot.operation == AZ_CMD_RELOOP)
            reinterpret_cast<void (*)(void *, int)>(state().profile.reloop)(reinterpret_cast<void *>(slot.facade), 0);
        else reinterpret_cast<void (*)(void *)>(state().profile.exit)(reinterpret_cast<void *>(slot.facade));
        bool after = true;
        if (!engaged(slot, after)) slot.failed.store(slot.request.token, std::memory_order_release);
        slot.caused = (extended(slot.operation) || cue_operation(slot.operation)) ? true : slot.operation == AZ_CMD_RELOOP ? (!before && after) || slot.captured != 0
                                                                                        : slot.caused || !before || !after || slot.captured != 0;
        /* Do not complete here. The native operation may have retained commands
         * in Quantize/Slip, including descendants produced in a later block. */
    } catch (...) {
        slot.failed.store(slot.request.token, std::memory_order_release);
    }
}
void settle(unsigned deck, Slot &slot) noexcept {
    if (slot.phase.load(std::memory_order_acquire) != RUNNING) return;
    uint64_t token = slot.request.token;
    /* An unknown handoff cannot acknowledge settlement. Even retirement cannot
     * prove an unobserved callback dead; retain the SDK fence for this process. */
    if (slot.untracked) return;
    if (!same_load(deck, slot)) {
        /* Live native references remain tagged with this old token. Their
         * execute wrappers now suppress them, even after result release. */
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_STALE);
        return;
    }
    uint64_t time = now();
    if (!time || compare_timestamps(time, slot.request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        finish(slot, AZ_COMMAND_UNCERTAIN, time ? AZ_STALE : AZ_FAILED);
        return;
    }
    if (!coherent(slot) || !bindings(slot)) {
        slot.failed.store(token, std::memory_order_release);
        return; /* no invented settlement while native ownership is unknown */
    }
    if (slot.operation == AZ_CMD_HOT_TRIGGER) {
        int pending = integer(word(slot.facade + 0x70) + 0x104);
        if (pending < 0 || pending > 3) {
            slot.untracked = true;
            return;
        }
        if (pending) return;
    }
    if (!settled_work(slot, token)) return;
    if (slot.operation == AZ_CMD_LOOP_MOVE && slot.move_stage == 1 &&
        slot.failed.load(std::memory_order_acquire) != token && slot.lost.load(std::memory_order_acquire) != token) {
        LoopPoints current = points(working(slot));
        bool positive = compare_timestamps(slot.arguments.position_s, 0., 1, slot.before.rate, 1) == AZ_TIME_AFTER;
        if (!set_bindings(slot) || !loop_context_safe(slot) || !complete_points(current) || !same_source(slot.before, current) ||
            compare_timestamps(current.in, positive ? slot.before.in : slot.expected_in, current.rate, current.rate, 1) != AZ_TIME_EQUAL ||
            compare_timestamps(current.out, positive ? slot.expected_out : slot.before.out, current.rate, current.rate, 1) != AZ_TIME_EQUAL) {
            finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            return;
        }
        /* Arm the second edit for the NEXT successful preProcess. A facade
         * call after postProcess could post a raw asynchronous task instead of
         * taking the verified owner-thread direct route. */
        slot.move_stage = 2;
        return;
    }
    if (slot.operation == AZ_CMD_LOOP_MOVE && slot.move_stage == 2) return;
    bool loop = true;
    bool verified = cue_operation(slot.operation) ? cue_postcondition(slot) : extended(slot.operation) && !entering(slot.operation)
                        ? set_bindings(slot) && loop_postcondition(slot)
                        : engaged(slot, loop) && loop == entering(slot.operation) &&
                              !(entering(slot.operation) && slot.initially_engaged);
    /* Engagement alone cannot confirm a seek when RELOOP started engaged.
     * There is no request-correlated native position receipt for that case. */
    if (!verified || slot.untracked || !slot.caused ||
        slot.failed.load(std::memory_order_acquire) == token || slot.lost.load(std::memory_order_acquire) == token ||
        slot.loss_overflow.load(std::memory_order_acquire)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        return;
    }
    finish(slot, AZ_COMMAND_APPLIED, AZ_OK, ++slot.revision);
}
__attribute__((unused)) bool preprocess_hook(void *self, const void *frames) {
    using Preprocess = bool (*)(void *, const void *);
    bool result = trampoline<Preprocess>(&state().pre_original)(self, frames);
    if (result && processing && processing->core.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(self) && state().enabled.load(std::memory_order_acquire)) {
        unsigned deck = unsigned(processing - state().decks);
        execute(deck, *processing);
    }
    return result;
}
__attribute__((unused)) void process_hook(void *self, const void *frames) {
    State &s = state();
    using Process = void (*)(void *, const void *);
    auto original = trampoline<Process>(&s.original);
    for (unsigned d = 0; d < 4; ++d) {
        Slot &slot = s.decks[d];
        if (slot.core.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(self)) continue;
        azsdk_native::OwnerGate::Lease lease;
        if (slot.gate.enter(lease) != AZ_OK) return;
        struct OwnerFrame {
            Slot *previous;
            explicit OwnerFrame(Slot *current) : previous(processing) { processing = current; }
            ~OwnerFrame() { processing = previous; }
        } owner(&slot);
        try {
            /* Original includes preProcess, rendering, all manager executions
             * and postprocessing. Teardown waits until the entire frame ends. */
            original(self, frames);
            if (s.enabled.load(std::memory_order_acquire)) settle(d, slot);
        } catch (...) {
            slot.failed.store(slot.token.load(std::memory_order_acquire), std::memory_order_release);
            throw;
        }
        return;
    }
    original(self, frames);
}
unsigned deck_mask() { return state().enabled.load(std::memory_order_acquire) ? state().mask.load(std::memory_order_acquire) : 0; }
/* Preserve the original endpoint's EXIT-only capability meaning for old peers. */
unsigned mask() { return state().enabled.load(std::memory_order_acquire) & AZEXIT_ENABLE_EXIT ? deck_mask() : 0; }
uint64_t commands() {
    if (!deck_mask()) return 0;
    unsigned enabled = state().enabled.load(std::memory_order_acquire);
    return (enabled & AZEXIT_ENABLE_EXIT ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0) |
           (enabled & AZEXIT_ENABLE_RELOOP ? UINT64_C(1) << AZ_CMD_RELOOP : 0) |
           (enabled & AZEXIT_ENABLE_LOOP ? (((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2) &
                                            ~((UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP)))
                                         : 0) |
           (enabled & AZEXIT_ENABLE_CUE ? (UINT64_C(1) << AZ_CMD_MAIN_SET) |
                                         (UINT64_C(1) << AZ_CMD_MAIN_RECALL) |
                                         (UINT64_C(1) << AZ_CMD_HOT_TRIGGER) : 0)
#ifndef AZSDK_EXIT_TESTING
           | azsdk_cue_commands_commands()
#endif
           ;
}
int target(unsigned deck, uint64_t *out) {
    if (!out || deck >= 4) return AZ_INVALID;
    if (!(deck_mask() & (1u << deck))) return AZ_UNSUPPORTED;
    azsdk_load_sample load = {};
    if (!azsdk_load_read(int(deck), &load)) return AZ_STALE;
    *out = load.load_generation;
    return AZ_OK;
}
int submit_command(const azexit_request *r, uint32_t operation) {
    if (!r || r->deck >= 4 || !r->token || !r->load_generation || !r->deadline_ns || r->reserved) return AZ_INVALID;
    if (operation != AZ_CMD_LOOP_EXIT && operation != AZ_CMD_RELOOP) return AZ_UNSUPPORTED;
    if (!(commands() & (UINT64_C(1) << operation)) || !(deck_mask() & (1u << r->deck))) return AZ_UNSUPPORTED;
    Slot &slot = state().decks[r->deck];
    unsigned expected = IDLE;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel)) return AZ_BUSY;
    slot.request = *r;
    slot.operation = operation;
    slot.token.store(r->token, std::memory_order_release);
    slot.phase.store(QUEUED, std::memory_order_release);
    return AZ_PENDING;
}
int submit_loop(const azexit_loop_request *r) {
    if (!r || r->size < sizeof *r || !extended(r->operation)) return AZ_INVALID;
    const auto &identity = r->identity;
    if (identity.deck >= 4 || !identity.token || !identity.load_generation || !identity.deadline_ns || identity.reserved ||
        !std::isfinite(r->position_s) || !std::isfinite(r->end_s) || !std::isfinite(r->beats)) return AZ_INVALID;
    bool valid = false;
    uint32_t beat = 0;
    switch (r->operation) {
    case AZ_CMD_LOOP_LENGTH: valid = r->position_s == 0 && r->end_s == 0 && azexit_pack_beat(r->beats, &beat); break;
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN:
        valid = r->position_s != 0 && std::isfinite(r->position_s * 1000) && r->end_s == 0 && r->beats == 0;
        break;
    case AZ_CMD_LOOP_ADJUST_OUT:
        valid = r->end_s != 0 && std::isfinite(r->end_s * 1000) && r->position_s == 0 && r->beats == 0;
        break;
    default: valid = r->position_s == 0 && r->end_s == 0 && r->beats == 0;
    }
    if (!valid) return AZ_INVALID;
    if (!(commands() & (UINT64_C(1) << r->operation)) || !(deck_mask() & (1u << identity.deck))) return AZ_UNSUPPORTED;
    Slot &slot = state().decks[identity.deck];
    unsigned expected = IDLE;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel)) return AZ_BUSY;
    slot.request = identity;
    slot.arguments = *r;
    slot.operation = r->operation;
    slot.token.store(identity.token, std::memory_order_release);
    slot.phase.store(QUEUED, std::memory_order_release);
    return AZ_PENDING;
}
int target_cue(unsigned deck, azexit_cue_target *out) {
    uint64_t generation = 0;
    if (!out || deck >= 4) return AZ_INVALID;
    int rc = target(deck, &generation);
    if (rc != AZ_OK) return rc;
    *out = {};
#ifndef AZSDK_EXIT_TESTING
    azexit_cue_target native = {};
    rc = azsdk_cue_commands_target_cue(deck, &native);
    if (rc == AZ_BUSY || rc == AZ_PENDING) return rc;
    if (rc == AZ_OK) *out = native;
#endif
    return AZ_OK;
}
int submit_cue(const azexit_cue_request *r) {
    if (!r || r->size < sizeof *r) return AZ_INVALID;
#ifndef AZSDK_EXIT_TESTING
    if (hot_edit(r->operation) || recall_operation(r->operation) || !cue_operation(r->operation)) return azsdk_cue_commands_submit(r);
#else
    if (!cue_operation(r->operation)) return AZ_INVALID;
#endif
    const auto &identity = r->identity;
    if (identity.deck >= 4 || !identity.token || !identity.load_generation || !identity.deadline_ns || identity.reserved ||
        r->expected_revision || r->flags != AZ_COMMAND_NATIVE_LOAD || r->rgb || r->comment[0] ||
        !std::memchr(r->comment, 0, sizeof r->comment) || r->position_s != 0 || r->end_s != 0 || r->beats != 0) return AZ_INVALID;
    if (r->operation == AZ_CMD_HOT_TRIGGER ? (r->classification != AZ_CUE_HOT || r->entry_id < 1 || r->entry_id > 8) :
        (r->classification != AZ_CUE_MAIN || r->entry_id)) return AZ_INVALID;
    if (!(commands() & (UINT64_C(1) << r->operation)) || !(deck_mask() & (1u << identity.deck))) return AZ_UNSUPPORTED;
    Slot &slot = state().decks[identity.deck];
    unsigned expected = IDLE;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel)) return AZ_BUSY;
    slot.request = identity;
    slot.cue_arguments = *r;
    slot.operation = r->operation;
    slot.token.store(identity.token, std::memory_order_release);
    slot.phase.store(QUEUED, std::memory_order_release);
    return AZ_PENDING;
}
int submit(const azexit_request *r) { return submit_command(r, AZ_CMD_LOOP_EXIT); }
int cancel(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = state().decks[deck];
#ifndef AZSDK_EXIT_TESTING
    if (slot.phase.load(std::memory_order_acquire) == IDLE) return azsdk_cue_commands_cancel(deck, token);
#endif
    unsigned expected = QUEUED;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel))
        return expected == IDLE ? AZ_STALE : AZ_PENDING;
    if (slot.request.token != token) {
        slot.phase.store(QUEUED, std::memory_order_release);
        return AZ_STALE;
    }
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    return AZ_OK;
}
int poll(unsigned deck, azexit_result *out) {
    if (deck >= 4 || !out) return AZ_INVALID;
#ifndef AZSDK_EXIT_TESTING
    if (state().decks[deck].internal_recall.load(std::memory_order_acquire)) return azsdk_cue_commands_poll(deck, out);
#endif
    Slot &slot = state().decks[deck];
    unsigned expected = READY;
    if (!slot.phase.compare_exchange_strong(expected, READING, std::memory_order_acq_rel)) {
#ifndef AZSDK_EXIT_TESTING
        if (expected == IDLE) return azsdk_cue_commands_poll(deck, out);
#endif
        return AZ_PENDING;
    }
    *out = slot.result;
    slot.phase.store(READY, std::memory_order_release);
    return AZ_OK;
}
int release(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
#ifndef AZSDK_EXIT_TESTING
    if (state().decks[deck].internal_recall.load(std::memory_order_acquire)) return azsdk_cue_commands_release(deck, token);
#endif
    Slot &slot = state().decks[deck];
    unsigned expected = READY;
    if (!slot.phase.compare_exchange_strong(expected, READING, std::memory_order_acq_rel)) {
#ifndef AZSDK_EXIT_TESTING
        if (expected == IDLE) return azsdk_cue_commands_release(deck, token);
#endif
        return AZ_BUSY;
    }
    if (slot.result.token != token) {
        slot.phase.store(READY, std::memory_order_release);
        return AZ_STALE;
    }
    slot.phase.store(IDLE, std::memory_order_release);
    return AZ_OK;
}
#if defined(__linux__) && !defined(AZSDK_EXIT_TESTING)
int image(dl_phdr_info *info, std::size_t, void *context) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    auto *s = static_cast<State *>(context);
    s->bias = info->dlpi_addr;
    return 1;
}
#endif
__attribute__((unused)) bool patch(uintptr_t address, uintptr_t original, void *replacement) {
#ifdef AZSDK_EXIT_TESTING
    return __atomic_compare_exchange_n(reinterpret_cast<uintptr_t *>(address), &original,
                                       reinterpret_cast<uintptr_t>(replacement), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#elif defined(__linux__) && defined(__aarch64__)
    void *entry = reinterpret_cast<void *>(original);
    return sdk_patch_slot(address, replacement, &entry) == 1;
#else
    (void) address;
    (void) original;
    (void) replacement;
    return false;
#endif
}
} // namespace

extern "C" const azexit_service *azstem_exit_service_v1(void) noexcept {
#ifndef AZSDK_EXIT_TESTING
    /* Hand the mutating private endpoint only to azmod, which authenticates
     * public mod ownership. Resolve once during community_init, never on audio. */
    Dl_info caller = {}, core = {};
    void *anchor = dlsym(RTLD_DEFAULT, "azmod_trackid_path");
    if (!anchor || !dladdr(anchor, &core) ||
        !dladdr(__builtin_extract_return_addr(__builtin_return_address(0)), &caller) ||
        !core.dli_fbase || caller.dli_fbase != core.dli_fbase) return nullptr;
#endif
    static const azexit_service api = {sizeof api, 1, mask, target, submit, cancel, poll, release, commands, submit_command, submit_loop, submit_cue, target_cue,
#ifndef AZSDK_EXIT_TESTING
        azsdk_cue_commands_tick, azsdk_cue_commands_native_cues
#else
        nullptr, nullptr
#endif
    };
    return &api;
}
extern "C" int azsdk_exit_configure(int on, int build, azexit_install_fn install) noexcept {
    State &s = state();
    if (!on) {
        s.enabled.store(0, std::memory_order_release);
#ifndef AZSDK_EXIT_TESTING
        azsdk_cue_commands_configure(0, build, install);
#endif
        return 0;
    }
    if (build < 1 || build > 2 || !install || (unsigned(on) & ~(AZEXIT_ENABLE_EXIT | AZEXIT_ENABLE_RELOOP | AZEXIT_ENABLE_LOOP | AZEXIT_ENABLE_CUE))) return 0;
#if !(defined(__linux__) && defined(__aarch64__)) && !defined(AZSDK_EXIT_TESTING)
    return 0;
#else
    try {
        std::lock_guard<std::mutex> lock(s.management);
        if (s.configured) return 0;
        s.build = build;
#ifndef AZSDK_EXIT_TESTING
        dl_iterate_phdr(image, &s);
        s.profile = exit_profiles[build - 1];
        const uintptr_t beat_methods[2][2] = {{0x13de978, 0x13de948}, {0x1499a50, 0x1499a20}};
        s.beat_exit = s.bias + beat_methods[build - 1][0];
        s.beat_reset = s.bias + beat_methods[build - 1][1];
        uintptr_t *fields[] = {&s.profile.unit, &s.profile.core, &s.profile.facade, &s.profile.beat,
                               &s.profile.quantize, &s.profile.slip, &s.profile.box, &s.profile.process,
                               &s.profile.preprocess, &s.profile.exit, &s.profile.affinity, &s.profile.reloop, &s.profile.control_jump,
                               &s.profile.manager_submit.address, &s.profile.quantize_submit.address,
                               &s.profile.quantize_jump.address, &s.profile.quantize_cancel.address, &s.profile.chain_table,
                               &s.profile.chain_destroy, &s.profile.chain_deleting,
                               &s.profile.jump_table, &s.profile.jump_request,
                               &s.profile.set_table, &s.profile.set_point, &s.profile.clear_cue, &s.profile.loop_out,
                               &s.profile.adjust_in, &s.profile.adjust_out, &s.profile.length_beat, &s.profile.length_square, &s.profile.cue_table,
                               &s.profile.merger_table, &s.profile.merger_request,
                               &s.profile.jump_schedule, &s.profile.interval_table, &s.profile.interval_source_table, &s.profile.jump_cancel, &s.profile.control_backcue,
                               &s.profile.cue_base_table, &s.profile.core_jump.address, &s.profile.set_hot, &s.profile.play_pause_table};
        for (auto field : fields) {
            if (*field > UINTPTR_MAX - s.bias) return 0;
            *field += s.bias;
        }
        for (auto &command : s.profile.commands) {
            uintptr_t *values[] = {&command.table, &command.destroy, &command.deleting, &command.execute};
            for (auto value : values) {
                if (*value > UINTPTR_MAX - s.bias) return 0;
                *value += s.bias;
            }
        }
#endif
        if (!s.profile.preprocess || std::memcmp(reinterpret_cast<void *>(s.profile.preprocess), s.profile.prologue, 16) ||
            !s.profile.process || std::memcmp(reinterpret_cast<void *>(s.profile.process), s.profile.process_prologue, 16) ||
            !s.profile.manager_submit.address || std::memcmp(reinterpret_cast<void *>(s.profile.manager_submit.address), s.profile.manager_submit.prologue, 16) ||
            !s.profile.quantize_submit.address || std::memcmp(reinterpret_cast<void *>(s.profile.quantize_submit.address), s.profile.quantize_submit.prologue, 16) ||
            !s.profile.quantize_cancel.address || std::memcmp(reinterpret_cast<void *>(s.profile.quantize_cancel.address), s.profile.quantize_cancel.prologue, 16) ||
            word(s.profile.core + 24) != s.profile.process || word(s.profile.facade + 24) != s.profile.exit ||
            !s.profile.reloop || word(s.profile.facade + 32) != s.profile.reloop ||
            !s.profile.control_jump || word(s.profile.facade + 48) != s.profile.control_jump ||
            word(s.profile.box + 40) != s.profile.affinity || !s.profile.jump_table ||
            !s.profile.jump_cancel || word(s.profile.jump_table + 40) != s.profile.jump_cancel ||
            word(s.profile.jump_table + 16) != s.profile.jump_request || !s.profile.jump_schedule ||
            word(s.profile.jump_table + 32) != s.profile.jump_schedule ||
            !s.profile.interval_table || !s.profile.interval_source_table) return 0;
        if ((unsigned(on) & AZEXIT_ENABLE_CUE) &&
            (!s.profile.control_backcue || word(s.profile.facade + 8 * 8) != s.profile.control_backcue ||
             !s.profile.cue_base_table || !s.profile.core_jump.address ||
             std::memcmp(reinterpret_cast<void *>(s.profile.core_jump.address), s.profile.core_jump.prologue, 16) ||
             !s.profile.play_pause_table)) return 0;
        for (const auto &command : s.profile.commands) {
            if (!command.table || !command.destroy || !command.deleting || !command.execute ||
                word(command.table) != command.destroy || word(command.table + 8) != command.deleting ||
                word(command.table + 16) != command.execute) return 0;
        }
        s.configured = true; /* Never try to patch a process that already reached this hook. */
        if (!install(s.profile.process, reinterpret_cast<void *>(process_hook), &s.original) || !s.original ||
            !install(s.profile.preprocess, reinterpret_cast<void *>(preprocess_hook), &s.pre_original) || !s.pre_original ||
            !install(s.profile.manager_submit.address, reinterpret_cast<void *>(manager_hook), &s.manager_original) || !s.manager_original ||
            !install(s.profile.quantize_submit.address, reinterpret_cast<void *>(quantize_hook), &s.quantize_original) || !s.quantize_original ||
            !install(s.profile.quantize_cancel.address, reinterpret_cast<void *>(quantize_cancel_hook), &s.cancel_original) || !s.cancel_original) return 0;
        for (const auto &command : s.profile.commands) {
            if (!patch(command.table, command.destroy, reinterpret_cast<void *>(destroy_hook)) ||
                !patch(command.table + 8, command.deleting, reinterpret_cast<void *>(deleting_hook)) ||
                !patch(command.table + 16, command.execute, reinterpret_cast<void *>(command_hook))) return 0;
        }
        if (!patch(s.profile.jump_table + 16, s.profile.jump_request, reinterpret_cast<void *>(jump_hook))) return 0;
        if (!patch(s.profile.jump_table + 32, s.profile.jump_schedule, reinterpret_cast<void *>(loop_schedule_hook))) return 0;
        if (!patch(s.profile.jump_table + 40, s.profile.jump_cancel, reinterpret_cast<void *>(jump_cancel_hook))) return 0;
        if ((unsigned(on) & AZEXIT_ENABLE_CUE) &&
            (!install(s.profile.core_jump.address, reinterpret_cast<void *>(core_jump_hook), &s.core_jump_original) || !s.core_jump_original)) return 0;
#ifndef AZSDK_EXIT_TESTING
        if (on & AZEXIT_ENABLE_CUE) azsdk_cue_commands_configure(1, build, install);
#endif
        s.enabled.store(unsigned(on), std::memory_order_release);
        return 1;
    } catch (...) { return 0; }
#endif
}
extern "C" int azsdk_exit_register(int deck, void *unit) noexcept {
    if (deck < 0 || deck >= 4 || !state().enabled.load(std::memory_order_acquire)) return 0;
    State &s = state();
    Slot &slot = s.decks[deck];
    try {
        std::lock_guard<std::mutex> lock(s.management);
        if (slot.gate.generation() || !table(reinterpret_cast<uintptr_t>(unit), s.profile.unit)) return 0;
        uintptr_t core = word(reinterpret_cast<uintptr_t>(unit) + 0x128);
        for (unsigned d = 0; d < 4; ++d)
            if (s.decks[d].core.load(std::memory_order_acquire) == core) return 0;
        uintptr_t bundle = word(reinterpret_cast<uintptr_t>(unit) + 0x190);
        if (!pointer(bundle) || !table(core, s.profile.core)) return 0;
        slot.unit = reinterpret_cast<uintptr_t>(unit);
        slot.facade = word(bundle + 0x40);
        slot.core.store(core, std::memory_order_release);
        if (!coherent(slot)) {
            slot.core.store(0, std::memory_order_release);
            return 0;
        }
        uint64_t generation;
        if (slot.gate.publish(&slot, admitted, &generation) != AZ_OK) {
            slot.core.store(0, std::memory_order_release);
            return 0;
        }
        s.mask.fetch_or(1u << unsigned(deck), std::memory_order_release);
        return 1;
    } catch (...) { return 0; }
}
extern "C" void azsdk_exit_retire(int deck) noexcept {
    if (deck < 0 || deck >= 4) return;
    State &s = state();
    Slot &slot = s.decks[deck];
    if (slot.gate.held_by_current_thread()) std::terminate();
    std::lock_guard<std::mutex> lock(s.management);
    s.mask.fetch_and(~(1u << unsigned(deck)), std::memory_order_acq_rel);
    uint64_t generation = slot.gate.generation();
    /* A native destructor must never free storage admitted by an in-flight owner
     * frame. Self-destruction is a violated native lifecycle, not permission to free. */
    if (generation && slot.gate.retire(generation) != AZ_OK) std::terminate();
    slot.core.store(0, std::memory_order_release);
    unsigned expected = QUEUED;
    if (slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel))
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
    else if (expected == RUNNING && !slot.untracked)
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_STALE);
}
#ifdef AZSDK_EXIT_TESTING
extern "C" void azsdk_exit_test_profile(const azexit_profile *profile) noexcept {
    if (profile && !state().configured) {
        state().profile = *profile;
        state().beat_exit = word(profile->beat + 16 * 8);
        state().beat_reset = word(profile->beat + 15 * 8);
    }
}
#endif

#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
extern "C" int azsdk_exit_recall_submit(const azexit_cue_request *r) noexcept {
    if (!r || r->identity.deck >= 4 || !(recall_operation(r->operation) || hot_edit(r->operation))) return AZ_INVALID;
    Slot &slot = state().decks[r->identity.deck];
    unsigned expected = IDLE;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel)) return AZ_BUSY;
    slot.internal_recall.store(true, std::memory_order_release);
    slot.request = r->identity;
    slot.cue_arguments = *r;
    slot.operation = r->operation;
    slot.token.store(r->identity.token, std::memory_order_release);
    slot.phase.store(QUEUED, std::memory_order_release);
    return AZ_PENDING;
}
extern "C" int azsdk_exit_cue_stage_submit(const azexit_cue_request *r) noexcept {
    return azsdk_exit_recall_submit(r);
}
extern "C" int azsdk_exit_cue_stage_capture(unsigned deck, uint64_t token, azexit_cue_stage_sample *out) noexcept {
    if (deck >= 4 || !token || !out) return AZ_INVALID;
    Slot &slot = state().decks[deck];
    unsigned expected = READY;
    if (!slot.internal_recall.load(std::memory_order_acquire) ||
        !slot.phase.compare_exchange_strong(expected, READING, std::memory_order_acq_rel)) return AZ_PENDING;
    int result = AZ_STALE;
    if (slot.result.token == token && slot.result.outcome == AZ_COMMAND_APPLIED && hot_edit(slot.operation)) {
        *out = slot.staged_cue;
        result = AZ_OK;
    }
    slot.phase.store(READY, std::memory_order_release);
    return result;
}
extern "C" int azsdk_exit_recall_cancel(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = state().decks[deck];
    unsigned expected = QUEUED;
    if (!slot.internal_recall.load(std::memory_order_acquire) || slot.token.load(std::memory_order_acquire) != token) return AZ_STALE;
    if (!slot.phase.compare_exchange_strong(expected, WRITING, std::memory_order_acq_rel)) return AZ_PENDING;
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    return AZ_OK;
}
extern "C" int azsdk_exit_recall_quiescent(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4 || !token) return 0;
    Slot &slot = state().decks[deck];
    unsigned phase = slot.phase.load(std::memory_order_acquire);
    return phase == IDLE || !slot.internal_recall.load(std::memory_order_acquire) ||
        slot.token.load(std::memory_order_acquire) != token || phase == READY || phase == READING;
}
extern "C" int azsdk_exit_recall_poll(unsigned deck, azexit_result *out) noexcept {
    if (deck >= 4 || !out) return AZ_INVALID;
    Slot &slot = state().decks[deck];
    unsigned expected = READY;
    if (!slot.internal_recall.load(std::memory_order_acquire) ||
        !slot.phase.compare_exchange_strong(expected, READING, std::memory_order_acq_rel)) return AZ_PENDING;
    *out = slot.result;
    slot.phase.store(READY, std::memory_order_release);
    return AZ_OK;
}
extern "C" int azsdk_exit_recall_release(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = state().decks[deck];
    unsigned expected = READY;
    if (!slot.internal_recall.load(std::memory_order_acquire) ||
        !slot.phase.compare_exchange_strong(expected, READING, std::memory_order_acq_rel)) return AZ_BUSY;
    if (slot.result.token != token) { slot.phase.store(READY, std::memory_order_release); return AZ_STALE; }
    slot.internal_recall.store(false, std::memory_order_release);
    slot.phase.store(IDLE, std::memory_order_release);
    return AZ_OK;
}
#endif

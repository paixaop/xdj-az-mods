/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcueactor.h"
#include "../azmod/azmod-mod.h"
#include <cstring>
#include <mutex>
#include <exception>
#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <cstdio>
#include <atomic>
#endif
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_CUE_ACTOR_TESTING)
#include "sdk_vtable_patch.h"
#endif

extern "C" __attribute__((weak)) int azsdk_exit_delivery_publish(const azexit_receipt *) noexcept {
    return AZ_UNSUPPORTED;
}
extern "C" __attribute__((weak)) void azsdk_cue_commands_notify(unsigned) noexcept {}

namespace {
constexpr unsigned TAGS = 512;
enum Phase {
    Idle,
    Queued,
    Posted,
    Executing,
    Waiting,
    Ready
};
struct Slot {
    void *manager = nullptr;
    uint64_t generation = 0, epoch = 0;
    Phase phase = Idle;
    bool continuation = false, work_started = false;
    azexit_cue_request request = {};
    azexit_result result = {};
};
struct Tag {
    bool reserved = false, cancelled = false, executed = false;
    void *task = nullptr, *manager = nullptr;
    unsigned deck = 0;
    uint64_t epoch = 0;
    azexit_cue_request request = {};
};
struct State {
    std::recursive_mutex mutex;
    Slot slots[4];
    Tag tags[TAGS];
    azcue_actor_profile profile = {};
    azcue_actor_callbacks callbacks = {};
    void *post = nullptr, *execute = nullptr, *dtor = nullptr;
    bool configured = false, installation_attempted = false;
};
State &state() {
    static State s;
    return s;
}
thread_local Tag *capture = nullptr;
uintptr_t word(const void *object, unsigned offset = 0) {
    uintptr_t value;
    std::memcpy(&value, static_cast<const char *>(object) + offset, sizeof value);
    return value;
}
uint64_t now() {
    timespec t = {};
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return uint64_t(t.tv_sec) * UINT64_C(1000000000) + uint64_t(t.tv_nsec);
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
void actor_diagnostic(unsigned deck, unsigned group, unsigned stage, const char *name, const azexit_cue_request &request, bool started) {
    static std::atomic<unsigned> last[2][4];
    static std::atomic<uint64_t> tokens[2][4];
    if (deck >= 4 || group >= 2 || !request.operation) return;
    bool changed = tokens[group][deck].exchange(request.identity.token, std::memory_order_relaxed) != request.identity.token;
    unsigned marker = stage * 2 + unsigned(started) + 1;
    if (last[group][deck].exchange(marker, std::memory_order_relaxed) == marker && !changed) return;
    std::fprintf(stderr, "AZCUE actor deck=%u stage=%s token=%llu load=%llu clock=%llu deadline=%llu started=%u\n",
                 deck, name, (unsigned long long) request.identity.token, (unsigned long long) request.identity.load_generation,
                 (unsigned long long) now(), (unsigned long long) request.identity.deadline_ns, unsigned(started));
}
#define ACTOR_DIAG(deck, group, stage, name, request, started) actor_diagnostic(deck, group, stage, name, request, started)
#else
#define ACTOR_DIAG(...) ((void) 0)
#endif
Tag *find(void *task) {
    for (auto &tag : state().tags)
        if (tag.reserved && tag.task == task) return &tag;
    return nullptr;
}
void finish(Slot &slot, unsigned outcome, int reason) {
    slot.result = {};
    slot.result.token = slot.request.identity.token;
    slot.result.load_generation = slot.request.identity.load_generation;
    slot.result.outcome = outcome == AZ_COMMAND_REJECTED && slot.work_started ? unsigned(AZ_COMMAND_UNCERTAIN) : outcome;
    slot.result.reason = reason;
    slot.phase = Ready;
    if (!(slot.request.identity.token & (UINT64_C(1) << 63))) {
        azexit_receipt receipt = {};
        receipt.size = sizeof receipt;
        receipt.version = AZEXIT_DELIVERY_VERSION;
        receipt.kind = AZEXIT_RECEIPT_TERMINAL;
        receipt.settled = 1;
        receipt.deck = slot.request.identity.deck;
        receipt.token = slot.result.token;
        receipt.load_generation = slot.result.load_generation;
        receipt.revision = slot.result.revision;
        receipt.observed_ns = now();
        receipt.outcome = slot.result.outcome;
        receipt.reason = slot.result.reason;
        (void) azsdk_exit_delivery_publish(&receipt);
    }
    azsdk_cue_commands_notify(slot.request.identity.deck);
}
bool matches(const Tag &tag, const Slot &slot) {
    return tag.epoch == slot.epoch && tag.manager == slot.manager &&
           tag.request.identity.token == slot.request.identity.token;
}
uint64_t post_hook(void *box, void *task) {
    State &s = state();
    /* Shared stock notification posts never enter the SDK management lock. */
    if (!capture) return reinterpret_cast<uint64_t (*)(void *, void *)>(__atomic_load_n(&s.post, __ATOMIC_ACQUIRE))(box, task);
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (capture) {
        Tag &tag = *capture;
        /* Only the original guarded producer is called while capture is set.
         * An ABI/profile violation must never release an untagged memory save. */
        if (tag.task || box != static_cast<char *>(tag.manager) + 8 || !task ||
            word(task) != s.profile.task_vptr || word(task, 0x18) != uintptr_t(tag.manager)) std::terminate();
        tag.task = task;
        Slot &slot = s.slots[tag.deck];
        if (matches(tag, slot)) slot.phase = Posted;
        capture = nullptr; /* no inherited capture during native reentry */
    }
    return reinterpret_cast<uint64_t (*)(void *, void *)>(s.post)(box, task);
}
void execute_hook(void *task) {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex);
    Tag *tag = find(task);
    if (!tag) {
        void *original = s.execute;
        lock.unlock();
        reinterpret_cast<void (*)(void *)>(original)(task);
        return;
    }
    /* A tombstone suppresses the original operation until native destruction,
     * including cancellation, owner retirement, and released SDK mailboxes. */
    if (tag->executed) return;
    tag->executed = true;
    Slot &slot = s.slots[tag->deck];
    if (tag->cancelled || !matches(*tag, slot) || slot.phase != Posted) return;
    if (!slot.work_started && !s.callbacks.current(tag->deck, tag->request.identity.load_generation, s.callbacks.context)) {
        ACTOR_DIAG(tag->deck, 1, 1, "execute-current", tag->request, slot.work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    uint64_t clock = now();
    if (!slot.work_started && (!clock || compare_timestamps(clock, tag->request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL)) {
        ACTOR_DIAG(tag->deck, 1, 2, "execute-deadline", tag->request, slot.work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    ACTOR_DIAG(tag->deck, 1, 0, "execute-admitted", tag->request, slot.work_started);
    slot.phase = Executing;
    azexit_result result = {};
    int rc = s.callbacks.execute(&tag->request, tag->manager, &result, s.callbacks.context);
    if (slot.phase != Executing) return; /* callback may synchronously complete */
    if (rc == AZ_PENDING) {
        slot.work_started = true;
        slot.phase = slot.continuation ? Queued : Waiting;
        slot.continuation = false;
    } else if (rc == AZ_OK && result.token == tag->request.identity.token &&
               result.load_generation == tag->request.identity.load_generation) {
        slot.result = result;
        slot.phase = Ready;
        if (!(slot.request.identity.token & (UINT64_C(1) << 63))) {
            azexit_receipt receipt = {};
            receipt.size = sizeof receipt;
            receipt.version = AZEXIT_DELIVERY_VERSION;
            receipt.kind = AZEXIT_RECEIPT_TERMINAL;
            receipt.settled = 1;
            receipt.deck = slot.request.identity.deck;
            receipt.token = result.token;
            receipt.load_generation = result.load_generation;
            receipt.revision = result.revision;
            receipt.observed_ns = now();
            receipt.outcome = result.outcome;
            receipt.reason = result.reason;
            (void) azsdk_exit_delivery_publish(&receipt);
        }
    } else finish(slot, AZ_COMMAND_REJECTED, rc == AZ_OK ? AZ_INVALID : rc);
}
void dtor_hook(void *task) {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex);
    Tag *tag = find(task);
    if (tag) {
        Slot &slot = s.slots[tag->deck];
        if (matches(*tag, slot) && !tag->executed && slot.phase == Posted)
            finish(slot, AZ_COMMAND_REJECTED, AZ_CANCELLED);
        /* Native dtor owns token/watchdog accounting; allocator runs after it. */
        reinterpret_cast<void (*)(void *)>(s.dtor)(task);
        *tag = {};
        return;
    }
    void *original = s.dtor;
    lock.unlock();
    reinterpret_cast<void (*)(void *)>(original)(task);
}
} // namespace
extern "C" int azsdk_cue_actor_configure(const azcue_actor_profile *p, azexit_install_fn install,
                                         const azcue_actor_callbacks *callbacks) noexcept {
    State &s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!p || !install || !callbacks || !callbacks->execute || !callbacks->current) return AZ_INVALID;
    if (s.configured || s.installation_attempted) return AZ_BUSY;
    if (!p->manager_vptr || !p->box_vptr || !p->owner_vptr || !p->task_vptr ||
        !p->memory_producer || !p->post || !p->execute || !p->complete_dtor) return AZ_UNSUPPORTED;
    s.profile = *p;
    s.callbacks = *callbacks;
    s.installation_attempted = true;
    /* Installer publishes each original before its patch becomes reachable. */
    if (!install(p->post, reinterpret_cast<void *>(post_hook), &s.post) || !s.post) return AZ_UNSUPPORTED;
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_CUE_ACTOR_TESTING)
    s.execute = reinterpret_cast<void *>(p->execute);
    if (sdk_patch_slot(p->task_vptr + 16, reinterpret_cast<void *>(execute_hook), &s.execute) != 1) return AZ_UNSUPPORTED;
#else
    if (!install(p->execute, reinterpret_cast<void *>(execute_hook), &s.execute) || !s.execute) return AZ_UNSUPPORTED;
#endif
    if (!install(p->complete_dtor, reinterpret_cast<void *>(dtor_hook), &s.dtor) || !s.dtor) return AZ_UNSUPPORTED;
    s.configured = true;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_register(unsigned deck, void *manager, uint64_t generation) noexcept {
    State &s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (deck >= 4 || !manager || !generation) return AZ_INVALID;
    if (!s.configured || word(manager) != s.profile.manager_vptr ||
        word(manager, 8) != s.profile.box_vptr) return AZ_UNSUPPORTED;
    Slot &slot = s.slots[deck];
    if (slot.manager) return AZ_BUSY;
    if (++slot.epoch == 0) std::terminate();
    slot.manager = manager;
    slot.generation = generation;
    return AZ_OK;
}
extern "C" void azsdk_cue_actor_retire(unsigned deck, void *manager) noexcept {
    State &s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (deck >= 4) return;
    Slot &slot = s.slots[deck];
    if (slot.manager != manager) return;
    for (auto &tag : s.tags)
        if (tag.reserved && tag.deck == deck && tag.epoch == slot.epoch) tag.cancelled = true;
    if (slot.phase != Idle && slot.phase != Ready) finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
    slot.manager = nullptr;
    slot.generation = 0;
}
extern "C" int azsdk_cue_actor_enqueue(const azexit_cue_request *request) noexcept {
    if (!request || request->size < sizeof *request || request->identity.deck >= 4 ||
        !request->identity.token || !request->identity.load_generation || !request->identity.deadline_ns ||
        request->identity.reserved || !std::memchr(request->comment, 0, sizeof request->comment)) return AZ_INVALID;
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_BUSY;
    Slot &slot = s.slots[request->identity.deck];
    if (!s.configured || !slot.manager) return AZ_UNSUPPORTED;
    if (slot.phase != Idle) return AZ_BUSY;
    for (const auto &tag : s.tags)
        if (tag.reserved && tag.deck == request->identity.deck &&
            tag.request.identity.token == request->identity.token) return AZ_STALE;
    slot.request = *request;
    slot.work_started = false;
    slot.continuation = false;
    slot.phase = Queued;
    return AZ_PENDING;
}
extern "C" void azsdk_cue_actor_drive(unsigned deck) noexcept {
    State &s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (deck >= 4) return;
    Slot &slot = s.slots[deck];
    if (slot.phase != Queued) return;
    if (!slot.manager || (!slot.work_started && !s.callbacks.current(deck, slot.request.identity.load_generation, s.callbacks.context))) {
        ACTOR_DIAG(deck, 0, 1, "drive-current-or-manager", slot.request, slot.work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    uint64_t clock = now();
    if (!slot.work_started && (!clock || compare_timestamps(clock, slot.request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL)) {
        ACTOR_DIAG(deck, 0, 2, "drive-deadline", slot.request, slot.work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    ACTOR_DIAG(deck, 0, 0, "drive-admitted", slot.request, slot.work_started);
    void *box = static_cast<char *>(slot.manager) + 8;
    uintptr_t owner = word(box, 0x28);
    if (word(slot.manager) != s.profile.manager_vptr || word(box) != s.profile.box_vptr ||
        !owner || word(reinterpret_cast<void *>(owner)) != s.profile.owner_vptr ||
        !reinterpret_cast<bool (*)(void *)>(word(reinterpret_cast<void *>(s.profile.box_vptr), 0x28))(box)) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        return;
    }
    Tag *tag = nullptr;
    for (auto &candidate : s.tags)
        if (!candidate.reserved) {
            tag = &candidate;
            break;
        }
    if (!tag) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_BUSY);
        return;
    }
    tag->reserved = true;
    tag->deck = deck;
    tag->manager = slot.manager;
    tag->epoch = slot.epoch;
    tag->request = slot.request;
    if (capture) std::terminate();
    capture = tag;
    reinterpret_cast<void (*)(void *)>(s.profile.memory_producer)(slot.manager);
    capture = nullptr;
    if (!tag->task && tag->reserved) {
        *tag = {};
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    }
}
extern "C" int azsdk_cue_actor_cancel(unsigned deck, uint64_t token) noexcept {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_PENDING;
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = s.slots[deck];
    if (slot.phase == Idle || slot.request.identity.token != token) return AZ_STALE;
    if (slot.phase == Ready) return AZ_OK;
    for (auto &tag : s.tags)
        if (tag.reserved && tag.deck == deck && matches(tag, slot)) tag.cancelled = true;
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_peek(unsigned deck, azexit_result *result) noexcept {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_PENDING;
    if (deck >= 4 || !result) return AZ_INVALID;
    if (s.slots[deck].phase != Ready) return AZ_PENDING;
    *result = s.slots[deck].result;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_take(unsigned deck, azexit_result *result) noexcept {
    return azsdk_cue_actor_peek(deck, result);
}
extern "C" int azsdk_cue_actor_poll(unsigned deck, azexit_result *result) noexcept {
    return azsdk_cue_actor_peek(deck, result);
}
extern "C" int azsdk_cue_actor_release(unsigned deck, uint64_t token) noexcept {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_BUSY;
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = s.slots[deck];
    if (slot.phase != Ready) return AZ_BUSY;
    if (slot.result.token != token) return AZ_STALE;
    slot.phase = Idle;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_complete(unsigned deck, uint64_t token, const azexit_result *result) noexcept {
    State &s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (deck >= 4 || !token || !result) return AZ_INVALID;
    Slot &slot = s.slots[deck];
    if ((slot.phase != Waiting && slot.phase != Executing) || slot.request.identity.token != token ||
        result->token != token || result->load_generation != slot.request.identity.load_generation) return AZ_STALE;
    slot.result = *result;
    slot.phase = Ready;
    if (!(token & (UINT64_C(1) << 63))) {
        azexit_receipt receipt = {};
        receipt.size = sizeof receipt;
        receipt.version = AZEXIT_DELIVERY_VERSION;
        receipt.kind = AZEXIT_RECEIPT_TERMINAL;
        receipt.settled = 1;
        receipt.deck = deck;
        receipt.token = result->token;
        receipt.load_generation = result->load_generation;
        receipt.revision = result->revision;
        receipt.observed_ns = now();
        receipt.outcome = result->outcome;
        receipt.reason = result->reason;
        (void) azsdk_exit_delivery_publish(&receipt);
    }
    return AZ_OK;
}

extern "C" int azsdk_cue_actor_resume(unsigned deck, uint64_t token) noexcept {
    State &s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_BUSY;
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot &slot = s.slots[deck];
    if (slot.request.identity.token != token) return AZ_STALE;
    if (slot.phase == Executing) {
        slot.continuation = true;
        return AZ_OK;
    }
    if (slot.phase != Waiting) return AZ_STALE;
    slot.phase = Queued;
    return AZ_OK;
}

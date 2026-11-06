/* SPDX-License-Identifier: GPL-3.0-only */
/* Slot8 CueUpdate weak observer. Only indexed revision-change receipts, never
 * cue kinds, edit operations, persistence, positions, source or load identity.
 * Logical deck comes from the caller's owned Unit registration. Callback3's
 * unsigned argument is compared to the opaque scalar captured from that same
 * server's embedded channel+0x48; it is not interpreted as a deck number.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azcuecommands.h"
#include "../common/azcueobserver.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <time.h>
#if defined(__linux__)
#if !defined(__GLIBCXX__)
#error "Native cue weak listeners require GNU libstdc++"
#endif
#include <link.h>
#endif

#ifdef AZSDK_CUE_TESTING
extern "C" __attribute__((weak)) void azsdk_cue_commands_collection_changed(unsigned) noexcept {}
#endif

namespace {
constexpr unsigned decks = 4, slot_limit = 64, mask_bits = 9, cue_mask = 0x1ff;
constexpr std::uint64_t max_generation = UINT64_MAX >> mask_bits;
constexpr unsigned read_flag = 4, execute_flag = 1;
constexpr std::uintptr_t unit_publisher = 0x1a0, publisher_server = 0x1f28;
constexpr std::uintptr_t cue_channel = 0x1508, channel_route = 0x48;
constexpr unsigned observer_slot = 8;
static_assert(sizeof(std::uintptr_t) == 8 && sizeof(unsigned) == 4, "native cue scalar ABI");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
              "native callbacks must use lock-free scalar atomics");
struct Notification; // u32 native index, padding, then native CueProperty copy
class Listener {
public:
    virtual ~Listener() {}
    virtual void callback2(const Notification &) noexcept = 0;
    virtual void callback3(const Notification &, unsigned route) noexcept = 0;
};
static_assert(sizeof(std::weak_ptr<Listener>) == 16, "native by-value GNU weak owner");
struct Range { std::uintptr_t begin, end; unsigned flags; };
struct Pending {
    // Each slot is assigned once and never reused. Old strong callback leases
    // can OR only their old slot, even across retirement/replacement races.
    std::atomic<std::uint64_t> word {0};
};
struct Published {
    std::atomic<bool> collecting {false};
    std::atomic<std::uint64_t> sequence {0}, generation {0}, revision {0}, observed {0};
    std::atomic<unsigned> mask {0};
#ifdef AZSDK_CUE_TESTING
    std::atomic<bool> clock_failure {false};
#endif
};
struct State {
    Range image[32] = {};
    unsigned image_count = 0;
    std::once_flag image_once;
    std::mutex registration;
    std::atomic<unsigned> enabled {0};
    std::atomic<unsigned> active[decks] = {}, acknowledged[decks] = {};
    unsigned bound_build = 0, used = 0; // registration mutex
    std::uint64_t next_generation = 0;
    Pending pending[slot_limit];
    Published published[decks];
    std::shared_ptr<Listener> owners[decks];
    unsigned owner_slot[decks] = {};
    State() noexcept {
        for (unsigned d = 0; d != decks; ++d) { active[d].store(0); acknowledged[d].store(0); }
    }
#ifdef AZSDK_CUE_TESTING
    using Hook = void (*)(int);
    std::atomic<Hook> before_pending {nullptr}, after_collect {nullptr};
#endif
};
State &state() noexcept {
    // azmod startup can precede TU dynamic constructors. Preserve the bounded
    // state through native/static teardown too; no native owners live here.
    struct Storage {
        alignas(State) unsigned char bytes[sizeof(State)];
        State *value;
        Storage() noexcept : value(::new (static_cast<void *>(bytes)) State) {}
    };
    static Storage storage;
    return *storage.value;
}
bool deck_ok(int deck) noexcept { return deck >= 0 && unsigned(deck) < decks; }
bool canonical(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
template<class T> T scalar(std::uintptr_t p) noexcept {
    T value; std::memcpy(&value, reinterpret_cast<const void *>(p), sizeof value); return value;
}
bool contains(std::uintptr_t p, std::size_t bytes, unsigned flags) noexcept {
    if (bytes > UINTPTR_MAX - p) return false;
    for (unsigned i = 0; i != state().image_count; ++i) {
        const auto &range = state().image[i];
        if ((range.flags & flags) == flags && p >= range.begin && p + bytes <= range.end) return true;
    }
    return false;
}
#if defined(__linux__)
int main_image(dl_phdr_info *info, std::size_t, void *) noexcept {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    auto &s = state();
    for (unsigned i = 0; i != info->dlpi_phnum; ++i) {
        const auto &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_R)) continue;
        const std::uintptr_t base = info->dlpi_addr;
        if (s.image_count == 32 || header.p_vaddr > UINTPTR_MAX - base ||
            header.p_memsz > UINTPTR_MAX - base - header.p_vaddr) { s.image_count = 0; return 1; }
        s.image[s.image_count++] = {base + header.p_vaddr, base + header.p_vaddr + header.p_memsz, header.p_flags};
    }
    return 1;
}
void prepare_image() { dl_iterate_phdr(main_image, nullptr); }
#endif
std::uintptr_t checked_vtable(std::uintptr_t object, const char *name, unsigned slots) noexcept {
    if (!canonical(object)) return 0;
    const auto table = scalar<std::uintptr_t>(object);
    if (!canonical(table) || table < 16 || !contains(table - 16, (slots + 2u) * 8u, read_flag) ||
        scalar<std::uintptr_t>(table - 16)) return 0;
    const auto type = scalar<std::uintptr_t>(table - 8);
    if (!canonical(type) || !contains(type, 16, read_flag)) return 0;
    auto text = scalar<std::uintptr_t>(type + 8);
    if (!contains(text, 1, read_flag)) return 0;
    if (*reinterpret_cast<const char *>(text) == '*') ++text;
    for (std::size_t i = 0;; ++i) {
        if (text > UINTPTR_MAX - i || !contains(text + i, 1, read_flag)) return 0;
        const auto actual = *reinterpret_cast<const volatile char *>(text + i);
        if (actual != name[i]) return 0;
        if (!actual) return table;
    }
}
void invalidate(unsigned deck) noexcept {
    state().active[deck].store(0, std::memory_order_release);
    state().acknowledged[deck].store(0, std::memory_order_release);
}
void invalidate_slot(unsigned deck, unsigned slot) noexcept {
    auto &s = state();
    auto expected = slot;
    s.active[deck].compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    expected = slot;
    s.acknowledged[deck].compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}
class CueListener final : public Listener {
    const unsigned deck_, slot_, route_;
public:
    CueListener(unsigned deck, unsigned slot, unsigned route) noexcept : deck_(deck), slot_(slot), route_(route) {}
    void callback2(const Notification &) noexcept override {} // avoid duplicate/unrouted publication
    void callback3(const Notification &value, unsigned route) noexcept override {
        auto &s = state();
        if (route != route_ || !s.enabled.load(std::memory_order_acquire) ||
            s.active[deck_].load(std::memory_order_acquire) != slot_) return;
        std::uint32_t index;
        std::memcpy(&index, &value, sizeof index);
        if (index >= mask_bits) return;
#ifdef AZSDK_CUE_TESTING
        auto hook = s.before_pending.load(std::memory_order_acquire);
        if (hook) hook(int(deck_));
#endif
        // One nonblocking RMW, no finite retry/lost wakeup. The immutable slot
        // belongs to this registration even if retirement races this store.
        s.pending[slot_ - 1].word.fetch_or(std::uint64_t(1u << index), std::memory_order_release);
        azsdk_cue_commands_collection_changed(deck_);
    }
};
std::uint64_t collection_now(unsigned deck) noexcept {
#ifdef AZSDK_CUE_TESTING
    if (state().published[deck].clock_failure.load(std::memory_order_acquire)) return 0;
#else
    (void)deck;
#endif
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return 0;
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > (UINT64_MAX - static_cast<unsigned long>(value.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + static_cast<unsigned long>(value.tv_nsec);
}
struct CollectionGuard {
    std::atomic<bool> &busy;
    explicit CollectionGuard(std::atomic<bool> &value) noexcept : busy(value) {}
    ~CollectionGuard() { busy.store(false, std::memory_order_release); }
};
} // namespace

extern "C" void azsdk_cue_configure(int on, int build) noexcept {
    auto &s = state();
    s.enabled.store(0, std::memory_order_release);
    for (unsigned d = 0; d != decks; ++d) invalidate(d);
    try {
        std::lock_guard<std::mutex> lock(s.registration);
        for (unsigned d = 0; d != decks; ++d) { invalidate(d); s.owners[d].reset(); s.owner_slot[d] = 0; }
#if defined(__linux__)
        if (!on || (build != 1 && build != 2) || (s.bound_build && s.bound_build != unsigned(build))) return;
        std::call_once(s.image_once, prepare_image);
        if (!s.image_count) return;
        s.bound_build = unsigned(build);
        s.enabled.store(1, std::memory_order_release);
#else
        (void)on; (void)build;
#endif
    } catch (...) {} // stays disabled, core listener ownership only
}
extern "C" int azsdk_cue_register(int deck, void *owned_unit) noexcept {
    auto &s = state();
    if (!deck_ok(deck) || !owned_unit || !s.enabled.load(std::memory_order_acquire)) return 0;
    unsigned slot = 0;
    std::uintptr_t server = 0, entry = 0;
    std::shared_ptr<Listener> listener;
    try {
        {
            std::lock_guard<std::mutex> lock(s.registration);
            if (!s.enabled.load(std::memory_order_acquire) || s.owners[deck] || s.used == slot_limit ||
                s.next_generation == max_generation) return 0;
            const auto unit = reinterpret_cast<std::uintptr_t>(owned_unit);
            if (!canonical(unit)) return 0;
            const auto publisher = scalar<std::uintptr_t>(unit + unit_publisher);
            if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
            server = scalar<std::uintptr_t>(publisher + publisher_server);
            const auto table = checked_vtable(server, "N9dj_player23StateNotificationServerE", 26);
            if (!table) return 0;
            entry = scalar<std::uintptr_t>(table + observer_slot * 8);
            if (!contains(entry, 4, execute_flag)) return 0;
            // Synchronous post-Unit-ctor window only. The native complete server
            // constructors initialize this u32 from their fourth argument:
            // 1.30 STR at0x148a3f8;2.00 STR at0x1545988. Copy it once while the
            // caller owns the constructed chain; no later native field read.
            // Same embedded channel as registration vector+0x1508 and dispatch
            // X0. No deck namespace or global immutability is assigned to it.
            const auto route = scalar<std::uint32_t>(server + cue_channel + channel_route);
            slot = ++s.used;
            const auto generation = ++s.next_generation;
            s.pending[slot - 1].word.store(generation << mask_bits, std::memory_order_release);
            listener = std::make_shared<CueListener>(unsigned(deck), slot, route);
            s.owners[deck] = listener; s.owner_slot[deck] = slot;
            s.active[deck].store(slot, std::memory_order_release);
        }
        // Native registration may call back/reenter or throw. Keep its local
        // core listener owner alive, but hold no SDK lock across native work.
        try {
            using Register = void (*)(void *, std::weak_ptr<Listener>);
            reinterpret_cast<Register>(entry)(reinterpret_cast<void *>(server), std::weak_ptr<Listener>(listener));
        } catch (...) {
            invalidate_slot(unsigned(deck), slot);
            std::lock_guard<std::mutex> lock(s.registration);
            if (s.owner_slot[deck] == slot) { s.owners[deck].reset(); s.owner_slot[deck] = 0; }
            return 0;
        }
        std::lock_guard<std::mutex> lock(s.registration);
        if (!s.enabled.load(std::memory_order_acquire) || s.active[deck].load(std::memory_order_acquire) != slot) {
            invalidate_slot(unsigned(deck), slot);
            if (s.owner_slot[deck] == slot) { s.owners[deck].reset(); s.owner_slot[deck] = 0; }
            return 0;
        }
        s.acknowledged[deck].store(slot, std::memory_order_release);
        return 1;
    } catch (...) {
        if (slot) invalidate_slot(unsigned(deck), slot);
        return 0;
    }
}
extern "C" void azsdk_cue_retire(int deck) noexcept {
    if (!deck_ok(deck)) return;
    const auto slot = state().active[deck].load(std::memory_order_acquire);
    if (slot) invalidate_slot(unsigned(deck), slot);
    try {
        std::lock_guard<std::mutex> lock(state().registration);
        if (slot && state().owner_slot[deck] == slot) {
            invalidate_slot(unsigned(deck), slot);
            state().owners[deck].reset(); state().owner_slot[deck] = 0;
        }
    } catch (...) {} // any escaped strong callback lease remains inactive
}
extern "C" int azsdk_cue_take(int deck, azsdk_cue_sample *out) noexcept {
    auto &s = state();
    if (!deck_ok(deck) || !out || !s.enabled.load(std::memory_order_acquire)) return 0;
    const auto slot = s.active[deck].load(std::memory_order_acquire);
    if (!slot || slot > slot_limit || s.acknowledged[deck].load(std::memory_order_acquire) != slot) return 0;
    auto &p = s.published[deck];
    bool free = false;
    if (!p.collecting.compare_exchange_strong(free, true, std::memory_order_acq_rel, std::memory_order_relaxed)) return 0;
    CollectionGuard guard(p.collecting);
    const auto sequence = p.sequence.load(std::memory_order_acquire);
    const auto revision = p.revision.load(std::memory_order_relaxed);
    if ((sequence & 1u) || sequence > UINT64_MAX - 2 || revision == UINT64_MAX ||
        !s.enabled.load(std::memory_order_acquire) || s.active[deck].load(std::memory_order_acquire) != slot ||
        s.acknowledged[deck].load(std::memory_order_acquire) != slot ||
        !(s.pending[slot - 1].word.load(std::memory_order_acquire) & cue_mask)) return 0;
    const auto now = collection_now(unsigned(deck));
    if (!now) return 0; // preserve pending on clock failure
    const auto batch = s.pending[slot - 1].word.fetch_and(~std::uint64_t(cue_mask), std::memory_order_acq_rel);
    const auto mask = static_cast<unsigned>(batch & cue_mask);
    if (!mask) return 0;
#ifdef AZSDK_CUE_TESTING
    auto hook = s.after_collect.load(std::memory_order_acquire);
    if (hook) hook(deck);
#endif
    if (!s.enabled.load(std::memory_order_acquire) || s.active[deck].load(std::memory_order_acquire) != slot ||
        s.acknowledged[deck].load(std::memory_order_acquire) != slot) return 0;
    const azsdk_cue_sample value {batch >> mask_bits, revision + 1, now, mask, 0};
    p.sequence.store(sequence + 1, std::memory_order_release);
    p.generation.store(value.unit_generation, std::memory_order_relaxed);
    p.revision.store(value.revision, std::memory_order_relaxed);
    p.observed.store(now, std::memory_order_relaxed);
    p.mask.store(mask, std::memory_order_relaxed);
    p.sequence.store(sequence + 2, std::memory_order_release);
    if (!s.enabled.load(std::memory_order_acquire) || s.active[deck].load(std::memory_order_acquire) != slot ||
        s.acknowledged[deck].load(std::memory_order_acquire) != slot) return 0;
    *out = value; return 1;
}
extern "C" int azsdk_cue_read(int deck, azsdk_cue_sample *out) noexcept {
    auto &s = state();
    if (!deck_ok(deck) || !out || !s.enabled.load(std::memory_order_acquire)) return 0;
    const auto slot = s.active[deck].load(std::memory_order_acquire);
    if (!slot || slot > slot_limit || s.acknowledged[deck].load(std::memory_order_acquire) != slot) return 0;
    const auto generation = s.pending[slot - 1].word.load(std::memory_order_acquire) >> mask_bits;
    auto &p = s.published[deck];
    const auto sequence = p.sequence.load(std::memory_order_acquire);
    if (sequence & 1u) return 0;
    const azsdk_cue_sample value {p.generation.load(std::memory_order_relaxed), p.revision.load(std::memory_order_relaxed),
        p.observed.load(std::memory_order_relaxed), p.mask.load(std::memory_order_relaxed), 0};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (p.sequence.load(std::memory_order_relaxed) != sequence || !value.unit_generation || value.unit_generation != generation ||
        !value.revision || !value.observed_ns || !value.changed_index_mask || !s.enabled.load(std::memory_order_acquire) ||
        s.active[deck].load(std::memory_order_acquire) != slot ||
        s.acknowledged[deck].load(std::memory_order_acquire) != slot) return 0;
    *out = value; return 1;
}
extern "C" unsigned azsdk_cue_capabilities() noexcept {
    auto &s = state(); unsigned result = 0;
    if (!s.enabled.load(std::memory_order_acquire)) return 0;
    for (unsigned d = 0; d != decks; ++d) {
        const auto slot = s.active[d].load(std::memory_order_acquire);
        if (slot && s.acknowledged[d].load(std::memory_order_acquire) == slot) result |= 1u << d;
    }
    return s.enabled.load(std::memory_order_acquire) ? result : 0;
}

#ifdef AZSDK_CUE_TESTING
extern "C" int azsdk_cue_test_control(int deck, unsigned kind, std::uint64_t value) noexcept {
    if (!deck_ok(deck)) return 0;
    auto &s = state(); auto &p = s.published[deck];
    if (kind == 1) {
        if (!value) { p.collecting.store(false, std::memory_order_release); return 1; }
        bool free = false; return p.collecting.compare_exchange_strong(free, true);
    }
    if (kind == 2) { p.clock_failure.store(value != 0, std::memory_order_release); return 1; }
    if (kind == 3) { p.revision.store(value, std::memory_order_release); return 1; }
    if (kind == 4) {
        const auto slot = s.active[deck].load(std::memory_order_acquire);
        return slot && slot <= slot_limit ? int(s.pending[slot - 1].word.load(std::memory_order_acquire) & cue_mask) : 0;
    }
    if (kind == 5) {
        std::lock_guard<std::mutex> lock(s.registration);
        if (value < s.next_generation || value > max_generation) return 0;
        s.next_generation = value; return 1;
    }
    return 0;
}
extern "C" void azsdk_cue_test_pending_hook(void (*hook)(int)) noexcept {
    state().before_pending.store(hook, std::memory_order_release);
}
extern "C" void azsdk_cue_test_collect_hook(void (*hook)(int)) noexcept {
    state().after_collect.store(hook, std::memory_order_release);
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* Native virtual entry/receipt tap. No source decode, native tasks or code patch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azcuecommands.h"
#include "../common/azloadobserver.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <time.h>
#if defined(__linux__)
#include <link.h>
#endif

#ifdef AZSDK_LOAD_TESTING
extern "C" __attribute__((weak)) void azsdk_cue_commands_notify(unsigned) noexcept {}
extern "C" __attribute__((weak)) void azsdk_exit_load_changed(unsigned) noexcept {}
#endif

namespace {
constexpr unsigned kDecks = 4, kRead = 4, kExecute = 1;
constexpr std::uintptr_t kUnitBundle = 0x190, kUnloadThis = 8;
constexpr std::intptr_t kCompleteThis = -0x150;
static_assert(sizeof(std::uintptr_t) == 8 && sizeof(unsigned) == 4, "native scalar ABI");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
              "bounded native observations require lock-free scalars");
struct Range {
    std::uintptr_t begin, end;
    unsigned flags;
};
Range image[32];
unsigned image_count;
std::uintptr_t image_base;
std::mutex management;
std::atomic<unsigned> enabled {0};
std::atomic<azsdk_load_patch_fn> patcher {nullptr};
std::atomic_flag setup = ATOMIC_FLAG_INIT;
unsigned installed_build;
/* Immutable first-empty reservations: aligned listener identity | (deck+1).
 * A failed strong CAS must inspect that same entry before proceeding, so two
 * contenders for one identity cannot reserve different entries. Never cleared. */
std::atomic<std::uintptr_t> association_history[16] = {};
/* Exact ELF virtual addresses, compared only against owned vtable entries.
 * No direct absolute native call is made. */
const std::uintptr_t native_entries[2][4] = {
    {0x13b9790, 0x13b9720, 0x13b9a38, 0xa35690},
    {0x1474e10, 0x1474da0, 0x14750c0, 0xa7b4a0}};
std::uint64_t next_unit;
std::atomic<std::uint64_t> next_load {0};
/* Parent installer reads these prepublished originals; hooks read atomically.
 * They are never reset, including after a declined or partial installation. */
void *original_load, *original_unload, *original_secondary_unload, *original_complete;
std::atomic<unsigned> load_installed {0}, unload_installed {0}, secondary_installed {0}, complete_installed {0};
struct Deck {
    std::atomic_flag capture = ATOMIC_FLAG_INIT;
    std::atomic<std::uintptr_t> unit {0}, facade {0}, listener {0};
    std::atomic<std::uint64_t> unit_generation {0}, epoch {1}, history {0};
    std::atomic<std::uint64_t> pending_epoch {0}, pending_unit {0}, pending_id {0};
    std::atomic<unsigned> ready {0}, poison {0}, valid {0};
    std::atomic<std::uint64_t> sequence {0}, sample_unit {0}, sample_load {0}, sample_epoch {0}, observed {0}, sample_id {0};
};
Deck decks[kDecks];
#ifdef AZSDK_LOAD_TESTING
void (*before_publish)(int);
void (*before_association)(int);
void (*configure_hook)(int, int);
std::uintptr_t fixture_profile[4];
unsigned fixture_build;
#endif
bool deck_ok(int deck) noexcept { return deck >= 0 && unsigned(deck) < kDecks; }
bool canonical(std::uintptr_t p) noexcept { return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u); }
bool contains(std::uintptr_t p, std::size_t n, unsigned flags) noexcept {
    if (n > std::numeric_limits<std::uintptr_t>::max() - p) return false;
    for (unsigned i = 0; i < image_count; ++i)
        if ((image[i].flags & flags) == flags && p >= image[i].begin && p + n <= image[i].end) return true;
    return false;
}
std::uintptr_t word(std::uintptr_t p) noexcept {
    std::uintptr_t v;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof v);
    return v;
}
#if defined(__linux__)
int main_image(dl_phdr_info *info, std::size_t, void *) noexcept {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    image_base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto &h = info->dlpi_phdr[i];
        if (h.p_type != PT_LOAD || !(h.p_flags & PF_R)) continue;
        std::uintptr_t base = info->dlpi_addr;
        if (image_count == sizeof image / sizeof *image || h.p_vaddr > UINTPTR_MAX - base || h.p_memsz > UINTPTR_MAX - base - h.p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = {base + h.p_vaddr, base + h.p_vaddr + h.p_memsz, h.p_flags};
    }
    return 1;
}
#endif
/* Only the caller's genuine owned object may be read. Mapped RTTI cannot make a
 * stale object pointer safe. Native names stop at first mismatch/NUL. */
std::uintptr_t nominal(std::uintptr_t object, const char *expected, std::intptr_t top, unsigned slots) noexcept {
    if (!canonical(object)) return 0;
    std::uintptr_t table = word(object);
    if (!canonical(table) || table < 16 || !contains(table - 16, (slots + 2u) * 8u, kRead) ||
        static_cast<std::intptr_t>(word(table - 16)) != top) return 0;
    std::uintptr_t type = word(table - 8);
    if (!canonical(type) || !contains(type, 16, kRead)) return 0;
    std::uintptr_t name = word(type + 8);
    if (!contains(name, 1, kRead)) return 0;
    if (*reinterpret_cast<const char *>(name) == '*') ++name;
    std::size_t bytes = std::strlen(expected) + 1;
    if (!contains(name, bytes, kRead)) return 0;
    const volatile char *actual = reinterpret_cast<const volatile char *>(name);
    for (std::size_t i = 0; i < bytes; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
bool native_id(const void *borrowed, std::uint32_t &out) noexcept {
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(borrowed);
    if (!nominal(p, "N9dj_player13LoadRequestIDE", 0, 2)) return false;
    out = __atomic_load_n(reinterpret_cast<const std::uint32_t *>(p + 8), __ATOMIC_ACQUIRE);
    return true;
}
std::uint64_t now() noexcept {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    std::uint64_t s = static_cast<std::uint64_t>(t.tv_sec);
    if (s > (UINT64_MAX - static_cast<unsigned long>(t.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + static_cast<unsigned long>(t.tv_nsec);
}
std::uint64_t invalidate(Deck &d) noexcept {
    d.valid.store(0, std::memory_order_release);
    azsdk_exit_load_changed(unsigned(&d - decks));
    std::uint64_t e = d.epoch.load(std::memory_order_acquire);
    for (unsigned n = 0; n < 8; ++n) {
        if (e == UINT64_MAX) break;
        if (d.epoch.compare_exchange_strong(e, e + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return e + 1;
    }
    d.poison.store(1, std::memory_order_release); /* never reuse an invalidation epoch */
    return 0;
}
bool fresh_id(Deck &d, std::uint32_t id) noexcept {
    std::uint64_t value = std::uint64_t(id) + 1, previous = d.history.load(std::memory_order_acquire);
    for (unsigned n = 0; n < 8; ++n) {
        if (value <= previous) return false;
        if (d.history.compare_exchange_strong(previous, value, std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
    d.poison.store(1, std::memory_order_release); /* an unrecorded ID cannot later be reused */
    return false;
}
std::uint64_t load_generation() noexcept {
    std::uint64_t value = next_load.load(std::memory_order_relaxed);
    for (unsigned n = 0; n < 8; ++n) {
        if (value == UINT64_MAX) return 0;
        if (next_load.compare_exchange_strong(value, value + 1, std::memory_order_relaxed, std::memory_order_relaxed)) return value + 1;
    }
    return 0;
}
void unlock(Deck &d) noexcept { d.capture.clear(std::memory_order_release); }
using Load = void (*)(void *, const void *, const void *, const void *, const void *, void *);
using Unload = void (*)(void *, const void *, void *);
using Complete = void (*)(void *, const void *, std::uint32_t, const void *);
void load_hook(void *, const void *, const void *, const void *, const void *, void *);
void unload_hook(void *, const void *, void *);
void secondary_unload_hook(void *, const void *, void *);
void complete_hook(void *, const void *, std::uint32_t, const void *);
std::uintptr_t verified_entry(unsigned kind) noexcept {
    if (!installed_build || installed_build > 2 || kind >= 4) return 0;
#ifdef AZSDK_LOAD_TESTING
    if (fixture_build == installed_build) return fixture_profile[kind];
#endif
    std::uintptr_t value = native_entries[installed_build - 1][kind];
    return value > UINTPTR_MAX - image_base ? 0 : image_base + value;
}
bool install(std::uintptr_t slot, void *hook, void **original, std::atomic<unsigned> &acknowledged, unsigned kind) noexcept {
    void *current = __atomic_load_n(reinterpret_cast<void **>(slot), __ATOMIC_ACQUIRE);
    void *expected = __atomic_load_n(original, __ATOMIC_ACQUIRE);
    void *verified = reinterpret_cast<void *>(verified_entry(kind));
    if (!verified || !contains(reinterpret_cast<std::uintptr_t>(verified), 4, kExecute)) return false;
    if ((current != hook && current != verified) || (expected && expected != verified)) return false;
    if (current == hook) return acknowledged.load(std::memory_order_acquire) && expected && contains(reinterpret_cast<std::uintptr_t>(expected), 4, kExecute);
    if (!contains(reinterpret_cast<std::uintptr_t>(current), 4, kExecute) || (expected && expected != current)) return false;
    if (!expected) {
        expected = current;
        __atomic_store_n(original, expected, __ATOMIC_RELEASE);
    }
    auto patch = patcher.load(std::memory_order_acquire);
    if (!patch) return false;
    int rc = 0;
    try {
        rc = patch(slot, hook, original);
    } catch (...) { return false; }
    bool ok = rc == 1 && __atomic_load_n(original, __ATOMIC_ACQUIRE) == expected &&
              __atomic_load_n(reinterpret_cast<void **>(slot), __ATOMIC_ACQUIRE) == hook;
    if (ok) acknowledged.store(1, std::memory_order_release);
    return ok;
}
bool completion_ready(std::uintptr_t listener) noexcept {
    std::uintptr_t table = nominal(listener, "N7usecase4load14LoadControllerE", kCompleteThis, 3);
    if (!table) return false;
    void *hook = reinterpret_cast<void *>(complete_hook);
    if (__atomic_load_n(reinterpret_cast<void **>(table + 16), __ATOMIC_ACQUIRE) == hook)
        return complete_installed.load(std::memory_order_acquire) && __atomic_load_n(&original_complete, __ATOMIC_ACQUIRE) != nullptr;
    if (setup.test_and_set(std::memory_order_acquire)) return false;
    bool ok = install(table + 16, hook, &original_complete, complete_installed, 3);
    setup.clear(std::memory_order_release);
    return ok;
}
int facade_deck(std::uintptr_t facade) noexcept {
    for (unsigned n = 0; n < kDecks; ++n)
        if (decks[n].facade.load(std::memory_order_acquire) == facade && decks[n].unit_generation.load(std::memory_order_acquire)) return static_cast<int>(n);
    return -1;
}
bool reserve_listener(unsigned deck, std::uintptr_t listener) noexcept {
    for (auto &record : association_history) {
        std::uintptr_t value = record.load(std::memory_order_acquire);
        if (!value && record.compare_exchange_strong(value, listener | (deck + 1u), std::memory_order_acq_rel, std::memory_order_acquire)) {
#ifdef AZSDK_LOAD_TESTING
            if (before_association) before_association(deck);
#endif
            return true;
        }
        if ((value & ~std::uintptr_t(7)) != listener) continue;
        unsigned owner = unsigned(value & 7u) - 1u;
        if (owner == deck) {
#ifdef AZSDK_LOAD_TESTING
            if (before_association) before_association(deck);
#endif
            return true;
        }
        if (owner < kDecks) {
            decks[owner].poison.store(1, std::memory_order_release);
            invalidate(decks[owner]);
        }
        decks[deck].poison.store(1, std::memory_order_release);
        invalidate(decks[deck]);
        return false;
    }
    decks[deck].poison.store(1, std::memory_order_release);
    invalidate(decks[deck]);
    return false;
}
void begin_load(std::uintptr_t facade, const void *option, void *listener) noexcept {
    if (!enabled.load(std::memory_order_acquire)) return;
    int deck = facade_deck(facade);
    if (deck < 0) return;
    Deck &d = decks[deck];
    std::uint64_t epoch = invalidate(d), unit = d.unit_generation.load(std::memory_order_acquire);
    std::uint32_t id;
    if (!epoch || !unit || d.poison.load(std::memory_order_acquire)) return;
    if (!native_id(option, id)) {
        d.poison.store(1, std::memory_order_release);
        return;
    }
    if (!fresh_id(d, id) ||
        !d.ready.load(std::memory_order_acquire) || !completion_ready(reinterpret_cast<std::uintptr_t>(listener))) return;
    if (!reserve_listener(unsigned(deck), reinterpret_cast<std::uintptr_t>(listener)) ||
        d.poison.load(std::memory_order_acquire) || d.capture.test_and_set(std::memory_order_acquire)) return;
    if (enabled.load(std::memory_order_acquire) && d.epoch.load(std::memory_order_acquire) == epoch &&
        d.unit_generation.load(std::memory_order_acquire) == unit && !d.poison.load(std::memory_order_acquire)) {
        d.listener.store(reinterpret_cast<std::uintptr_t>(listener), std::memory_order_release);
        d.pending_unit.store(unit, std::memory_order_relaxed);
        d.pending_id.store(std::uint64_t(id) + 1, std::memory_order_relaxed);
        d.pending_epoch.store(epoch, std::memory_order_release);
    }
    unlock(d);
}
void begin_unload(std::uintptr_t facade, const void *option) noexcept {
    if (!enabled.load(std::memory_order_acquire)) return;
    int deck = facade_deck(facade);
    if (deck < 0) return;
    Deck &d = decks[deck];
    invalidate(d);
    std::uint32_t id;
    if (native_id(option, id)) (void) fresh_id(d, id);
    else d.poison.store(1, std::memory_order_release);
    if (d.capture.test_and_set(std::memory_order_acquire)) return;
    d.pending_epoch.store(0, std::memory_order_release);
    d.pending_id.store(0, std::memory_order_release);
    unlock(d);
}
void complete(std::uintptr_t listener, const void *request, std::uint32_t result) noexcept {
    if (!enabled.load(std::memory_order_acquire) || !nominal(listener, "N7usecase4load14LoadControllerE", kCompleteThis, 3)) return;
    std::uint32_t id;
    if (!native_id(request, id)) return;
    for (unsigned deck = 0; deck < kDecks; ++deck) {
        Deck &d = decks[deck];
        std::uint64_t epoch = d.epoch.load(std::memory_order_acquire), unit = d.unit_generation.load(std::memory_order_acquire);
        if (!unit || d.listener.load(std::memory_order_acquire) != listener || d.pending_epoch.load(std::memory_order_acquire) != epoch ||
            d.pending_unit.load(std::memory_order_acquire) != unit || d.pending_id.load(std::memory_order_acquire) != std::uint64_t(id) + 1) continue;
        if (d.capture.test_and_set(std::memory_order_acquire)) {
            invalidate(d);
            return;
        }
        if (enabled.load(std::memory_order_acquire) && !d.poison.load(std::memory_order_acquire) &&
            d.epoch.load(std::memory_order_acquire) == epoch && d.unit_generation.load(std::memory_order_acquire) == unit &&
            d.listener.load(std::memory_order_acquire) == listener && d.pending_epoch.load(std::memory_order_acquire) == epoch &&
            d.pending_id.load(std::memory_order_acquire) == std::uint64_t(id) + 1) {
            d.pending_id.store(0, std::memory_order_release);
            std::uint64_t generation = result == 0 ? load_generation() : 0, observed = generation ? now() : 0;
            std::uint64_t sequence = d.sequence.load(std::memory_order_relaxed);
#ifdef AZSDK_LOAD_TESTING
            if (before_publish) before_publish(static_cast<int>(deck));
#endif
            if (generation && observed && sequence <= UINT64_MAX - 2 && d.epoch.load(std::memory_order_acquire) == epoch &&
                d.unit_generation.load(std::memory_order_acquire) == unit) {
                d.sequence.store(sequence + 1, std::memory_order_release);
                /* Acquiring any field of a newer write must also observe its
                 * preceding odd sequence at the final sequence check. */
                d.sample_unit.store(unit, std::memory_order_release);
                d.sample_load.store(generation, std::memory_order_release);
                d.sample_epoch.store(epoch, std::memory_order_release);
                d.sample_id.store(id, std::memory_order_release);
                d.observed.store(observed, std::memory_order_release);
                d.sequence.store(sequence + 2, std::memory_order_release);
                d.valid.store(1, std::memory_order_release);
                if (d.epoch.load(std::memory_order_acquire) != epoch || d.unit_generation.load(std::memory_order_acquire) != unit)
                    d.valid.store(0, std::memory_order_release);
            }
        }
        unlock(d);
        azsdk_cue_commands_notify(deck);
        return;
    }
}
void load_hook(void *self, const void *track, const void *option, const void *where, const void *speed, void *listener) {
    begin_load(reinterpret_cast<std::uintptr_t>(self), option, listener);
    auto original = reinterpret_cast<Load>(__atomic_load_n(&original_load, __ATOMIC_ACQUIRE));
    if (original) original(self, track, option, where, speed, listener);
}
void unload_hook(void *self, const void *option, void *listener) {
    begin_unload(reinterpret_cast<std::uintptr_t>(self), option);
    auto original = reinterpret_cast<Unload>(__atomic_load_n(&original_unload, __ATOMIC_ACQUIRE));
    if (original) original(self, option, listener);
}
void secondary_unload_hook(void *self, const void *option, void *listener) {
    std::uintptr_t receiver = reinterpret_cast<std::uintptr_t>(self);
    if (receiver >= kUnloadThis) begin_unload(receiver - kUnloadThis, option);
    auto original = reinterpret_cast<Unload>(__atomic_load_n(&original_secondary_unload, __ATOMIC_ACQUIRE));
    if (original) original(self, option, listener);
}
void complete_hook(void *self, const void *request, std::uint32_t result, const void *where) {
    complete(reinterpret_cast<std::uintptr_t>(self), request, result);
    auto original = reinterpret_cast<Complete>(__atomic_load_n(&original_complete, __ATOMIC_ACQUIRE));
    if (original) original(self, request, result, where);
}
} // namespace

extern "C" void azsdk_load_configure(int on, int build, azsdk_load_patch_fn patch) noexcept {
    enabled.store(0, std::memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) {
        invalidate(decks[d]);
        decks[d].unit_generation.store(0, std::memory_order_release);
    }
#ifdef AZSDK_LOAD_TESTING
    if (configure_hook) configure_hook(0, on);
#endif
    try {
        std::lock_guard<std::mutex> lock(management);
        enabled.store(0, std::memory_order_release);
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(decks[d]);
            decks[d].unit_generation.store(0, std::memory_order_release);
            decks[d].facade.store(0, std::memory_order_release);
            decks[d].unit.store(0, std::memory_order_release);
            decks[d].ready.store(0, std::memory_order_release);
        }
#if defined(__linux__)
        if (!on || !patch || (build != 1 && build != 2) || (installed_build && installed_build != unsigned(build))) return;
        if (!image_count) dl_iterate_phdr(main_image, nullptr);
        if (!image_count) return;
        if (!installed_build) installed_build = unsigned(build); /* immutable before first enabled publication */
        patcher.store(patch, std::memory_order_release);
#ifdef AZSDK_LOAD_TESTING
        if (configure_hook) configure_hook(1, on);
#endif
        enabled.store(1, std::memory_order_release);
#else
        (void) on;
        (void) build;
        (void) patch;
        (void) installed_build;
#endif
    } catch (...) {}
}
extern "C" int azsdk_load_register(int deck, void *owned_unit) noexcept {
    if (!deck_ok(deck) || !enabled.load(std::memory_order_acquire) || !owned_unit) return 0;
    try {
        std::lock_guard<std::mutex> lock(management);
        Deck &d = decks[deck];
        if (!enabled.load(std::memory_order_acquire) || d.unit_generation.load(std::memory_order_acquire) || d.poison.load(std::memory_order_acquire)) return 0;
        std::uintptr_t unit = reinterpret_cast<std::uintptr_t>(owned_unit);
        if (!nominal(unit, "N9dj_player12DjPlayerUnitE", 0, 2)) return 0;
        std::uintptr_t bundle = word(unit + kUnitBundle);
        if (!canonical(bundle)) return 0;
        std::uintptr_t facade = word(bundle), primary = nominal(facade, "N9dj_player15TrackLoadFacadeE", 0, 4);
        if (!primary) return 0;
        for (unsigned n = 0; n < kDecks; ++n)
            if (n != unsigned(deck) && decks[n].unit_generation.load(std::memory_order_acquire) &&
                (decks[n].unit.load(std::memory_order_acquire) == unit || decks[n].facade.load(std::memory_order_acquire) == facade)) return 0;
        std::uintptr_t secondary = nominal(facade + kUnloadThis, "N9dj_player15TrackLoadFacadeE", -8, 3);
        if (!secondary || next_unit == UINT64_MAX || setup.test_and_set(std::memory_order_acquire)) return 0;
        bool ok = install(primary + 16, reinterpret_cast<void *>(load_hook), &original_load, load_installed, 0) &&
                  install(primary + 24, reinterpret_cast<void *>(unload_hook), &original_unload, unload_installed, 1) &&
                  install(secondary + 16, reinterpret_cast<void *>(secondary_unload_hook), &original_secondary_unload, secondary_installed, 2);
        setup.clear(std::memory_order_release);
        if (!ok || d.capture.test_and_set(std::memory_order_acquire)) return 0;
        std::uint64_t epoch = invalidate(d);
        if (!epoch || !enabled.load(std::memory_order_acquire)) {
            unlock(d);
            return 0;
        }
        d.listener.store(0, std::memory_order_release);
        d.pending_epoch.store(0, std::memory_order_release);
        d.pending_id.store(0, std::memory_order_release);
        d.unit.store(unit, std::memory_order_release);
        d.facade.store(facade, std::memory_order_release);
        d.ready.store(1, std::memory_order_release);
        d.unit_generation.store(++next_unit, std::memory_order_release);
        unlock(d);
        return 1;
    } catch (...) { return 0; }
}
extern "C" void azsdk_load_retire(int deck) noexcept {
    if (!deck_ok(deck)) return;
    Deck &d = decks[deck];
    invalidate(d);
    d.unit_generation.store(0, std::memory_order_release);
    d.facade.store(0, std::memory_order_release);
    d.unit.store(0, std::memory_order_release);
    d.ready.store(0, std::memory_order_release);
}
extern "C" int azsdk_load_read(int deck, azsdk_load_sample *out) noexcept {
    if (!deck_ok(deck) || !out || !enabled.load(std::memory_order_acquire)) return 0;
    Deck &d = decks[deck];
    if (d.poison.load(std::memory_order_acquire) || !d.valid.load(std::memory_order_acquire)) return 0;
    std::uint64_t epoch = d.epoch.load(std::memory_order_acquire), sequence = d.sequence.load(std::memory_order_acquire);
    if (!sequence || (sequence & 1u)) return 0;
    azsdk_load_sample sample = {d.sample_unit.load(std::memory_order_acquire), d.sample_load.load(std::memory_order_acquire),
                                d.sample_epoch.load(std::memory_order_acquire), d.observed.load(std::memory_order_acquire),
                                static_cast<std::uint32_t>(d.sample_id.load(std::memory_order_acquire)), 0};
    if (d.sequence.load(std::memory_order_acquire) != sequence || !d.valid.load(std::memory_order_acquire) ||
        d.poison.load(std::memory_order_acquire) || d.epoch.load(std::memory_order_acquire) != epoch || sample.invalidation_epoch != epoch ||
        d.unit_generation.load(std::memory_order_acquire) != sample.unit_generation || !sample.unit_generation || !sample.load_generation ||
        !enabled.load(std::memory_order_acquire)) return 0;
    *out = sample;
    return 1;
}
#ifdef AZSDK_LOAD_TESTING
extern "C" void azsdk_load_test_hold(int deck, int held) noexcept {
    if (!deck_ok(deck)) return;
    if (held) decks[deck].capture.test_and_set(std::memory_order_acquire);
    else decks[deck].capture.clear(std::memory_order_release);
}
extern "C" void azsdk_load_test_before_publish(void (*hook)(int)) noexcept { before_publish = hook; }
extern "C" void azsdk_load_test_before_association(void (*hook)(int)) noexcept { before_association = hook; }
extern "C" void azsdk_load_test_configure_hook(void (*hook)(int, int)) noexcept { configure_hook = hook; }
extern "C" void azsdk_load_test_profile(int build, void *load, void *unload, void *secondary, void *completion) noexcept {
    if (enabled.load(std::memory_order_acquire) || installed_build || (build != 1 && build != 2)) return;
    fixture_build = unsigned(build);
    fixture_profile[0] = reinterpret_cast<std::uintptr_t>(load);
    fixture_profile[1] = reinterpret_cast<std::uintptr_t>(unload);
    fixture_profile[2] = reinterpret_cast<std::uintptr_t>(secondary);
    fixture_profile[3] = reinterpret_cast<std::uintptr_t>(completion);
}
extern "C" void azsdk_load_test_exhaust(int deck, int which) noexcept {
    if (!deck_ok(deck)) return;
    if (which == 1) decks[deck].epoch.store(UINT64_MAX, std::memory_order_release);
    else if (which == 2) next_load.store(UINT64_MAX, std::memory_order_release);
    else if (which == 3) decks[deck].sequence.store(UINT64_MAX - 1, std::memory_order_release);
    else if (which == 4) next_unit = UINT64_MAX;
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded GNU weak observer bridge. Shared owning chain with sdk_loop_observer.cpp:
 * unit+0x1a0 -> UnitStateServer+0x1f28 -> StateNotificationServer. Virtual slots 15/19/21/2
 * select play-with-state, quantize, key-lock and Sync observers on both supported builds.
 * Only borrowed notification bytes are copied; no native object or source pointer is kept.
 * Signatures/layout evidence and unresolved semantic normalization are documented in
 * CORE-SDK-LIVE-READERS.md. Registration and receipt never establish committed load identity. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azliveobserver.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <time.h>
#if defined(__linux__)
#if !defined(__GLIBCXX__)
#error "Native live observers require GNU libstdc++, never libc++"
#endif
#include <link.h>
#endif

namespace {
constexpr unsigned kDecks = 4, kLegacyStreams = 3, kSync = 3, kTempoLock = 4, kStreams = 5, kRead = 4, kExecute = 1;
constexpr std::size_t kUnitPublisher = 0x1a0, kPublisherServer = 0x1f28;
constexpr unsigned slots[kStreams] = {15, 19, 21, 2, 5};
static_assert(sizeof(std::uintptr_t) == 8 && sizeof(unsigned) == 4, "native scalar ABI");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
              "live callbacks must publish without locks");
struct Notification;
class Listener {
   public:
    virtual ~Listener() {}
    virtual void callback2(const Notification &) noexcept = 0;
    virtual void callback3(const Notification &, unsigned player) noexcept = 0;
};
static_assert(sizeof(std::weak_ptr<Listener>) == 16, "GNU native weak_ptr pair");
struct ImageRange {
    std::uintptr_t begin, end;
    unsigned flags;
};
ImageRange image[32];
unsigned image_count;
std::mutex registration_mutex;
std::atomic<unsigned> enabled {0}, submitted {0};
std::uint64_t next_generation;
/* Separate streams avoid one kind of callback blocking another kind's publication. */
struct Component {
    std::atomic<std::uint64_t> sequence {0}, generation {0}, revision {0}, observed {0};
    std::atomic<unsigned> valid {0}, a {0}, b {0}, c {0};
};
struct Published {
    std::atomic<std::uint64_t> active_generation {0};
    Component components[kStreams];
};
Published published[kDecks];
std::shared_ptr<Listener> owners[kDecks][kStreams];
bool deck_ok(int d) noexcept { return d >= 0 && static_cast<unsigned>(d) < kDecks; }
bool canonical(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
bool image_contains(std::uintptr_t p, std::size_t n, unsigned flags) noexcept {
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
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto &h = info->dlpi_phdr[i];
        if (h.p_type != PT_LOAD || !(h.p_flags & PF_R)) continue;
        std::uintptr_t base = info->dlpi_addr;
        if (image_count == sizeof image / sizeof *image ||
            h.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - base ||
            h.p_memsz > std::numeric_limits<std::uintptr_t>::max() - base - h.p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = {base + h.p_vaddr, base + h.p_vaddr + h.p_memsz, h.p_flags};
    }
    return 1;
}
#endif
std::uintptr_t checked_vtable(std::uintptr_t object, const char *expected, unsigned count) noexcept {
    if (!canonical(object)) return 0;
    std::uintptr_t table = word(object);
    if (!canonical(table) || table < 16 || !image_contains(table - 16, (count + 2u) * 8u, kRead) || word(table - 16)) return 0;
    std::uintptr_t type = word(table - 8);
    if (!canonical(type) || !image_contains(type, 16, kRead)) return 0;
    std::uintptr_t name = word(type + 8);
    if (!image_contains(name, 1, kRead)) return 0;
    if (*reinterpret_cast<const char *>(name) == '*') ++name;
    std::size_t bytes = std::strlen(expected) + 1;
    if (!image_contains(name, bytes, kRead)) return 0;
    /* A different RTTI name may occupy a shorter C string in the same ELF segment.
     * Stop at its first mismatch/NUL; a fixed-size memcmp crosses that object's end. */
    const volatile char *actual = reinterpret_cast<const volatile char *>(name);
    for (std::size_t i = 0; i < bytes; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
std::uint64_t observed_now() noexcept {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    std::uint64_t s = static_cast<std::uint64_t>(t.tv_sec);
    if (s > (UINT64_MAX - static_cast<unsigned long>(t.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + static_cast<unsigned long>(t.tv_nsec);
}
void publish(unsigned deck, unsigned stream, std::uint64_t generation, const Notification &n) noexcept {
    Published &p = published[deck];
    if (p.active_generation.load(std::memory_order_acquire) != generation) return;
    unsigned a = 0, b = 0, c = 0;
    if (stream == 0) {
        unsigned char bytes[3];
        std::memcpy(bytes, &n, sizeof bytes);
        a = bytes[0];
        b = bytes[1];
        c = bytes[2];
    } else {
        /* Quantize/key-lock zero-extend a byte; Sync carries a full 32-bit enum. */
        std::uint32_t value;
        std::memcpy(&value, &n, sizeof value);
        a = value;
    }
    bool valid = a <= 1 && b <= 1 && c <= 1;
    if (stream == kSync) valid = a == 0 || a == 2;
    Component &v = p.components[stream];
    std::uint64_t seq = v.sequence.load(std::memory_order_acquire);
    if ((seq & 1u) || seq > UINT64_MAX - 2 || !v.sequence.compare_exchange_strong(seq, seq + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return;
    if (p.active_generation.load(std::memory_order_acquire) != generation) {
        v.sequence.store(seq + 2, std::memory_order_release);
        return;
    }
    std::uint64_t now = observed_now(), revision = v.revision.load(std::memory_order_relaxed);
    if (!now || revision == UINT64_MAX) {
        v.sequence.store(seq + 2, std::memory_order_release);
        return;
    }
    v.generation.store(generation, std::memory_order_relaxed);
    v.valid.store(valid, std::memory_order_relaxed);
    v.a.store(a, std::memory_order_relaxed);
    v.b.store(b, std::memory_order_relaxed);
    v.c.store(c, std::memory_order_relaxed);
    v.observed.store(now, std::memory_order_relaxed);
    v.revision.store(revision + 1, std::memory_order_relaxed);
    v.sequence.store(seq + 2, std::memory_order_release);
}
class CaptureListener final : public Listener {
    const unsigned deck, stream;
    const std::uint64_t generation;
   public:
    CaptureListener(unsigned d, unsigned s, std::uint64_t g) noexcept : deck(d), stream(s), generation(g) {}
    void callback2(const Notification &) noexcept override {}
    void callback3(const Notification &n, unsigned player) noexcept override {
        if (player != deck) return;
        publish(deck, stream, generation, n);
    }
};
struct ScalarReceipt {
    std::uint64_t unit_generation = 0, revision = 0, observed_ns = 0;
    std::int32_t raw = 0;
    unsigned valid = 0;
};
bool read_scalar(int deck, unsigned stream, ScalarReceipt &value) noexcept {
    if (!deck_ok(deck) || !enabled.load(std::memory_order_acquire)) return false;
    Published &p = published[deck];
    std::uint64_t active = p.active_generation.load(std::memory_order_acquire);
    if (!active) return false;
    Component &v = p.components[stream];
    std::uint64_t seq = v.sequence.load(std::memory_order_acquire);
    if (seq & 1u) return false;

    value.unit_generation = v.generation.load(std::memory_order_relaxed);
    value.revision = v.revision.load(std::memory_order_relaxed);
    value.observed_ns = v.observed.load(std::memory_order_relaxed);
    unsigned raw = v.a.load(std::memory_order_relaxed);
    value.valid = v.valid.load(std::memory_order_relaxed);
    std::memcpy(&value.raw, &raw, sizeof value.raw);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (v.sequence.load(std::memory_order_relaxed) != seq || value.unit_generation != active ||
        !value.revision || !value.observed_ns || p.active_generation.load(std::memory_order_acquire) != active) return false;
    return true;
}
void invalidate(unsigned deck) noexcept {
    published[deck].active_generation.store(0, std::memory_order_release);
    submitted.fetch_and(~(1u << deck), std::memory_order_acq_rel);
}
} // namespace

extern "C" void azsdk_live_configure(int on, int build) noexcept {
    enabled.store(0, std::memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) invalidate(d);
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(d);
            for (unsigned s = 0; s < kStreams; ++s) owners[d][s].reset();
        }
#if defined(__linux__)
        if (on && (build == 1 || build == 2)) {
            if (!image_count) dl_iterate_phdr(main_image, nullptr);
            if (image_count) enabled.store(1, std::memory_order_release);
        }
#else
        (void) on;
        (void) build;
#endif
    } catch (...) {}
}
extern "C" int azsdk_live_register(int deck, void *owned_unit) noexcept {
    if (!deck_ok(deck) || !owned_unit || !enabled.load(std::memory_order_acquire)) return 0;
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        if (!enabled.load(std::memory_order_acquire) || owners[deck][0]) return 0;
        std::uintptr_t unit = reinterpret_cast<std::uintptr_t>(owned_unit);
        if (!canonical(unit)) return 0;
        std::uintptr_t publisher = word(unit + kUnitPublisher);
        if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
        std::uintptr_t server = word(publisher + kPublisherServer);
        std::uintptr_t table = checked_vtable(server, "N9dj_player23StateNotificationServerE", 26);
        if (!table) return 0;
        std::uintptr_t entries[kStreams];
        for (unsigned s = 0; s < kStreams; ++s) {
            entries[s] = word(table + slots[s] * 8);
            if (!image_contains(entries[s], 4, kExecute)) return 0;
        }
        if (next_generation == UINT64_MAX) return 0;
        std::uint64_t generation = ++next_generation;
        /* Allocate before publishing an active generation; a throw cannot leave a half set. */
        std::shared_ptr<Listener> pending[kStreams];
        for (unsigned s = 0; s < kStreams; ++s) pending[s] = std::make_shared<CaptureListener>(static_cast<unsigned>(deck), s, generation);
        for (unsigned s = 0; s < kStreams; ++s) owners[deck][s] = pending[s];
        published[deck].active_generation.store(generation, std::memory_order_release);
        try {
            using Register = void (*)(void *, std::weak_ptr<Listener>);
            for (unsigned s = 0; s < kStreams; ++s)
                reinterpret_cast<Register>(entries[s])(reinterpret_cast<void *>(server), std::weak_ptr<Listener>(owners[deck][s]));
        } catch (...) {
            invalidate(static_cast<unsigned>(deck));
            for (unsigned s = 0; s < kStreams; ++s) owners[deck][s].reset();
            return 0;
        }
        submitted.fetch_or(1u << deck, std::memory_order_release);
        return 1;
    } catch (...) {
        /* Allocation/locking failed before publishing owners. Locals unwind while any
         * acquired registration lock is still held; shared state needs no cleanup. */
        return 0;
    }
}
extern "C" void azsdk_live_retire(int deck) noexcept {
    if (!deck_ok(deck)) return;
    invalidate(static_cast<unsigned>(deck));
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        invalidate(static_cast<unsigned>(deck));
        for (unsigned s = 0; s < kStreams; ++s) owners[deck][s].reset();
    } catch (...) {}
}
extern "C" int azsdk_live_read(int deck, azsdk_live_sample *out) noexcept {
    if (!deck_ok(deck) || !out || !enabled.load(std::memory_order_acquire)) return 0;
    Published &p = published[deck];
    std::uint64_t active = p.active_generation.load(std::memory_order_acquire);
    if (!active) return 0;
    azsdk_live_sample value = {};
    value.unit_generation = active;
    value.mode_is_2 = value.play_pause = value.play_aux = value.quantize = value.key_lock = -1;
    for (unsigned s = 0; s < kLegacyStreams; ++s) {
        Component &v = p.components[s];
        std::uint64_t seq = v.sequence.load(std::memory_order_acquire);
        if (seq & 1u) continue;
        std::uint64_t gen = v.generation.load(std::memory_order_relaxed);
        std::uint64_t rev = v.revision.load(std::memory_order_relaxed);
        std::uint64_t time = v.observed.load(std::memory_order_relaxed);
        unsigned valid = v.valid.load(std::memory_order_relaxed);
        unsigned a = v.a.load(std::memory_order_relaxed), b = v.b.load(std::memory_order_relaxed), c = v.c.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (v.sequence.load(std::memory_order_relaxed) != seq || gen != active || !rev || !time) continue;
        value.observed_fields |= 1u << s;
        value.revisions[s] = rev;
        value.observed_ns[s] = time;
        if (!valid) continue;
        value.valid_fields |= 1u << s;
        if (s == 0) {
            value.mode_is_2 = static_cast<int>(a);
            value.play_pause = static_cast<int>(b);
            value.play_aux = static_cast<int>(c);
        } else if (s == 1) value.quantize = static_cast<int>(a);
        else value.key_lock = static_cast<int>(a);
    }
    if (!value.observed_fields || p.active_generation.load(std::memory_order_acquire) != active) return 0;
    *out = value;
    return 1;
}
extern "C" int azsdk_sync_read(int deck, azsdk_sync_sample *out) noexcept {
    if (!out) return 0;
    ScalarReceipt value;
    if (!read_scalar(deck, kSync, value)) return 0;
    *out = {value.unit_generation, value.revision, value.observed_ns, value.raw, value.valid};
    return 1;
}
extern "C" unsigned azsdk_live_capabilities(void) noexcept {
    return enabled.load(std::memory_order_acquire) ? submitted.load(std::memory_order_acquire) : 0;
}
extern "C" int azsdk_tempo_lock_read(int deck, azsdk_tempo_lock_sample *out) noexcept {
    if (!out) return 0;
    ScalarReceipt value;
    if (!read_scalar(deck, kTempoLock, value)) return 0;
    *out = {value.unit_generation, value.revision, value.observed_ns, value.raw, value.valid};
    return 1;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* GNU C++ ABI reconstruction isolated here: weak_ptr by value and a four-slot listener.
 * Native interface source names are not recovered. Both builds corroborate this bounded
 * route: unit+0x1a0 -> UnitStateServer+0x1f28 -> StateNotificationServer virtual slot 11.
 * Basis: DjPlayerUnit constructors 1.30 0x14a2560 / 2.00 0x155d9f0; observer wrappers
 * 1.30 0x14824b8 / 2.00 0x153da48; dispatchers 1.30 0x148d480 / 2.00 0x1548a08.
 * These are evidence anchors only, never addresses resolved or called by this module.
 * Registration borrows an owned native object. Callback3 carries its constructor's player
 * number and a borrowed current working CueProperty copy at notification+8. Only its
 * verified scalar validity predicate and checked source coordinates are captured;
 * native pointers are not retained. Source coordinates do not establish load identity.
 * Unknown initial replay, async registration completion and source freshness remain unknown.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/aztimestamp.h"
#include "../common/azloopobserver.h"
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <time.h>
#if defined(__linux__)
#if !defined(__GLIBCXX__)
#error "The native loop listener requires GNU libstdc++, never libc++"
#endif
#include <link.h>
#endif

namespace {
constexpr unsigned kDecks = 4;
constexpr unsigned kRead = 4, kExecute = 1;
constexpr std::size_t kUnitPublisher = 0x1a0, kPublisherServer = 0x1f28;
constexpr std::size_t kObserverSlot = 11;
static_assert(sizeof(std::uintptr_t) == 8, "native listener requires a 64-bit ABI");
static_assert(sizeof(unsigned) == 4, "native player number requires a 32-bit ABI");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
              "loop callback publication must be lock-free");

struct Notification; /* Borrowed 8-byte header followed by a 0xf8-byte CueProperty copy. */
class Listener {
   public:
    virtual ~Listener() {}
    virtual void callback2(const Notification &) noexcept = 0;
    virtual void callback3(const Notification &, unsigned player) noexcept = 0;
};
static_assert(sizeof(std::weak_ptr<Listener>) == 16, "native weak_ptr pair must be two words");

struct ImageRange {
    std::uintptr_t begin, end;
    unsigned flags;
};
/* The main ELF mappings live for this process. Initialize before the first listener and
 * never rewrite them: borrowed callbacks may still be in flight during reconfiguration. */
ImageRange image[32];
unsigned image_count;
std::mutex registration_mutex;
std::atomic<unsigned> enabled {0}, submitted {0};
std::uint64_t next_generation; /* Registration mutex; never wrap and reuse an identity. */
struct Published {
    std::atomic<std::uint64_t> sequence {0}, active_generation {0};
    std::atomic<std::uint64_t> generation {0}, revision {0}, observed {0};
    std::atomic<int> engaged {0};
    std::atomic<int> bounds_valid {0}, bounds_reason {1};
    std::atomic<std::uint64_t> in_bits {0}, out_bits {0};
    std::atomic<std::uint64_t> check_in_samples {0}, check_out_samples {0};
    std::atomic<std::uint64_t> check_in_cached_us {0}, check_out_cached_us {0};
    std::atomic<std::uint32_t> check_rate {0};
};
Published published[kDecks];
std::shared_ptr<Listener> owners[kDecks]; /* Registration paths only, protected by mutex. */

bool deck_ok(int deck) noexcept { return deck >= 0 && static_cast<unsigned>(deck) < kDecks; }
bool canonical(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
bool image_contains(std::uintptr_t p, std::size_t n, unsigned flags) noexcept {
    if (n > std::numeric_limits<std::uintptr_t>::max() - p) return false;
    for (unsigned i = 0; i < image_count; ++i)
        if ((image[i].flags & flags) == flags && p >= image[i].begin && p + n <= image[i].end)
            return true;
    return false;
}
std::uintptr_t word(std::uintptr_t p) noexcept {
    std::uintptr_t out;
    std::memcpy(&out, reinterpret_cast<const void *>(p), sizeof out);
    return out;
}
#if defined(__linux__)
int main_image(dl_phdr_info *info, std::size_t, void *) noexcept {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto &h = info->dlpi_phdr[i];
        if (h.p_type != PT_LOAD || !(h.p_flags & PF_R)) continue;
        if (image_count == sizeof image / sizeof *image) {
            image_count = 0;
            return 1;
        }
        std::uintptr_t base = info->dlpi_addr;
        if (h.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - base ||
            h.p_memsz > std::numeric_limits<std::uintptr_t>::max() - base - h.p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = {base + h.p_vaddr, base + h.p_vaddr + h.p_memsz, h.p_flags};
    }
    return 1;
}
#endif
/* Object liveness comes from the caller's native constructor scope, not these range checks.
 * ELF bounds constrain vtable/typeinfo/name/code reads to the main executable only. */
std::uintptr_t checked_vtable(std::uintptr_t object, const char *expected, unsigned slots) noexcept {
    if (!canonical(object)) return 0;
    std::uintptr_t table = word(object);
    if (!canonical(table) || table < 16 ||
        !image_contains(table - 16, (slots + 2u) * 8u, kRead) || word(table - 16)) return 0;
    std::uintptr_t type = word(table - 8);
    if (!canonical(type) || !image_contains(type, 16, kRead)) return 0;
    std::uintptr_t name = word(type + 8);
    /* GNU type_info::name strips '*' on an unmerged name. It denotes the same type. */
    if (!image_contains(name, 1, kRead)) return 0;
    if (*reinterpret_cast<const char *>(name) == '*') ++name;
    std::size_t length = std::strlen(expected) + 1;
    if (!image_contains(name, length, kRead)) return 0;
    /* A different RTTI name may occupy a shorter C string in the same ELF segment.
     * Stop at its first mismatch/NUL; a fixed-size memcmp crosses that object's end. */
    const volatile char *actual = reinterpret_cast<const volatile char *>(name);
    for (std::size_t i = 0; i < length; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
std::uint64_t observed_now() noexcept {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L) return 0;
    std::uint64_t seconds = static_cast<std::uint64_t>(ts.tv_sec);
    if (seconds > (UINT64_MAX - static_cast<unsigned long>(ts.tv_nsec)) / UINT64_C(1000000000))
        return 0;
    return seconds * UINT64_C(1000000000) + static_cast<unsigned long>(ts.tv_nsec);
}

/* Exact native InstantDoublesTargetOnLocalPlayer::isActiveLoopPlay policy:
 * 1.30 a609f0 -> 1492ae8 -> 13ec228; 2.00 aa69c8 -> 154e058 -> 14a7668.
 * Both publishers copy the CURRENT working property, which EXIT clears independently of
 * retained cue slots. Byte 0 describes a separate mode and is not loop engagement.
 * Copy only scalars while the native callback owns its full 0x100-byte notification. */
bool working_loop_engaged(const Notification &notification) noexcept {
    unsigned char scalar[0x18];
    const auto *bytes = reinterpret_cast<const unsigned char *>(&notification);
    std::memcpy(scalar, bytes + 0x38, sizeof scalar);
    std::uint32_t nonzero = 0;
    for (unsigned i = 0; i < 4; ++i) nonzero |= static_cast<std::uint32_t>(scalar[8 + i]) << (i * 8);
    std::uint64_t position = 0;
    for (unsigned i = 0; i < 8; ++i) position |= static_cast<std::uint64_t>(scalar[16 + i]) << (i * 8);
    return scalar[0] != 0 && (scalar[4] & 0x1f) != 0 && scalar[6] != 0 && nonzero != 0 &&
           position != UINT64_C(0x7fffffffffffffff);
}

std::uint32_t scalar32(const unsigned char *p) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return value;
}
std::uint64_t scalar64(const unsigned char *p) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return value;
}
std::uint64_t double_bits(double value) noexcept {
    std::uint64_t bits;
    static_assert(sizeof bits == sizeof value, "loop coordinates require binary64 storage");
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}
double bits_double(std::uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
bool source_position_valid(const unsigned char *source) noexcept {
    return source[0] && (source[4] & 0x1f) && source[6] && scalar32(source + 8) &&
           scalar64(source + 16) != UINT64_C(0x7fffffffffffffff);
}
struct Bounds {
    int reason = 1; /* Internal first-failure diagnostic; never a native pointer or SDK field. */
    bool valid = false;
    double in_s = 0, out_s = 0;
    std::uint64_t check_in_samples = 0, check_out_samples = 0;
    std::uint64_t check_in_cached_us = 0, check_out_cached_us = 0;
    std::uint32_t check_rate = 0;
};
Bounds rejected_bounds(int reason) noexcept {
    Bounds result;
    result.reason = reason;
    return result;
}
/* SDK consistency policy: permit one source sample of rounding/truncation and
 * half a microsecond for the integer cache. It does not describe every native
 * setter. Cross multiplication keeps the boundary exact, even above double's
 * integer precision; the largest intermediate is below 2^97 for u64/u32 inputs.
 * Source samples, not the cache, remain the published coordinate authority. */
bool source_time_consistent_us(std::uint64_t samples, std::uint32_t rate, std::uint64_t us) noexcept {
    if (!rate) return false;
    using Wide = unsigned __int128;
    Wide source = static_cast<Wide>(samples) * 1000000u;
    Wide cached = static_cast<Wide>(us) * rate;
    return compare_timestamps(2u * source, 2u * cached, 1, 1, UINT64_C(2000000) + rate + 1) == AZ_TIME_EQUAL;
}
/* Both builds: CueProperty IN/OUT at +0x18/+0x70, PositionWithSourceInfo at point+0x10.
 * Setter 1.30 13ef020 / 2.00 14aa460 stores signed samples/Hertz and caches microseconds
 * using FCVTAS: nearest, ties away from zero, after (1e6 / rate) * samples.
 * This is a bounded borrowed-record read ending at notification+0xd0. SourceInfo words
 * are compared only as opaque associations, never followed or retained. Point+8 is an
 * independent native setter bool; neither native position predicate requires it. */
Bounds working_loop_bounds(const Notification &notification, bool engaged) noexcept {
    Bounds result;
    if (!engaged) return result;
    std::uintptr_t property = reinterpret_cast<std::uintptr_t>(&notification) + 8;
    if (!checked_vtable(property, "N9dj_player11CuePropertyE", 1)) return rejected_bounds(2);
    if (!checked_vtable(property + 0x18, "N9dj_player13PointPropertyE", 1)) return rejected_bounds(3);
    if (!checked_vtable(property + 0x70, "N9dj_player13PointPropertyE", 1)) return rejected_bounds(4);
    if (!checked_vtable(property + 0x28, "N12player_basis22PositionWithSourceInfoE", 1)) return rejected_bounds(5);
    if (!checked_vtable(property + 0x80, "N12player_basis22PositionWithSourceInfoE", 1)) return rejected_bounds(6);
    unsigned char bytes[0xc8];
    std::memcpy(bytes, reinterpret_cast<const void *>(property), sizeof bytes);
    if (!source_position_valid(bytes + 0x30) || !source_position_valid(bytes + 0x88)) return rejected_bounds(7);
    if (std::memcmp(bytes + 0x30, bytes + 0x88, 12)) return rejected_bounds(8);
    std::uint32_t rate = scalar32(bytes + 0x60);
    if (!rate || rate >= UINT32_C(0x7fffffff) || rate != scalar32(bytes + 0xb8)) return rejected_bounds(9);
    /* Native stored-cue conversion creates independently owned Source objects
     * for IN and OUT. Their checked SourceInfo/rate above define association,
     * and each opaque source reference must remain present. Neither is followed. */
    if (!scalar64(bytes + 0x50) || !scalar64(bytes + 0xa8)) return rejected_bounds(10);
    std::uint64_t in = scalar64(bytes + 0x40), out = scalar64(bytes + 0x98);
    std::uint64_t in_us = scalar64(bytes + 0x68), out_us = scalar64(bytes + 0xc0);
    /* Technical precision bound, not a media-duration policy. It also rejects negative
     * signed values and INT64_MAX without implementation-defined unsigned-to-signed casts. */
    constexpr std::uint64_t exact_integer = UINT64_C(9007199254740992);
    if (in > exact_integer || out > exact_integer || in_us > exact_integer ||
        out_us > exact_integer || compare_timestamps(out, in, rate, rate, 1) != AZ_TIME_AFTER) return rejected_bounds(11);
    result.check_in_samples = in;
    result.check_out_samples = out;
    result.check_in_cached_us = in_us;
    result.check_out_cached_us = out_us;
    result.check_rate = rate;
    if (!source_time_consistent_us(in, rate, in_us) || !source_time_consistent_us(out, rate, out_us)) {
        result.reason = 12;
        return result;
    }
    result.in_s = static_cast<double>(in) / static_cast<double>(rate);
    result.out_s = static_cast<double>(out) / static_cast<double>(rate);
    if (!std::isfinite(result.in_s) || !std::isfinite(result.out_s) || result.out_s <= result.in_s) {
        result.in_s = result.out_s = 0;
        result.reason = 13;
        return result;
    }
    result.valid = true;
    result.reason = 0;
    return result;
}

class LoopListener final : public Listener {
    const unsigned deck;
    const std::uint64_t generation;
   public:
    LoopListener(unsigned d, std::uint64_t g) noexcept : deck(d), generation(g) {}
    void callback2(const Notification &) noexcept override {}
    void callback3(const Notification &notification, unsigned player) noexcept override {
        if (player != deck) return;
        Published &p = published[deck];
        if (p.active_generation.load(std::memory_order_acquire) != generation) return;
        bool engaged = working_loop_engaged(notification);
        Bounds bounds = working_loop_bounds(notification, engaged);
        std::uint64_t seq = p.sequence.load(std::memory_order_acquire);
        if ((seq & 1u) || !p.sequence.compare_exchange_strong(seq, seq + 1,
                                                              std::memory_order_acq_rel, std::memory_order_relaxed)) return;
        if (p.active_generation.load(std::memory_order_acquire) != generation) {
            p.sequence.store(seq + 2, std::memory_order_release);
            return;
        }
        std::uint64_t now = observed_now();
        if (!now) {
            p.sequence.store(seq + 2, std::memory_order_release);
            return;
        }
        std::uint64_t revision = p.revision.load(std::memory_order_relaxed);
        if (revision == UINT64_MAX) {
            p.sequence.store(seq + 2, std::memory_order_release);
            return;
        }
        p.generation.store(generation, std::memory_order_relaxed);
        p.engaged.store(engaged, std::memory_order_relaxed);
        p.bounds_valid.store(bounds.valid, std::memory_order_relaxed);
        p.bounds_reason.store(bounds.reason, std::memory_order_relaxed);
        p.in_bits.store(double_bits(bounds.in_s), std::memory_order_relaxed);
        p.out_bits.store(double_bits(bounds.out_s), std::memory_order_relaxed);
        p.check_in_samples.store(bounds.check_in_samples, std::memory_order_relaxed);
        p.check_out_samples.store(bounds.check_out_samples, std::memory_order_relaxed);
        p.check_in_cached_us.store(bounds.check_in_cached_us, std::memory_order_relaxed);
        p.check_out_cached_us.store(bounds.check_out_cached_us, std::memory_order_relaxed);
        p.check_rate.store(bounds.check_rate, std::memory_order_relaxed);
        p.observed.store(now, std::memory_order_relaxed);
        p.revision.store(revision + 1, std::memory_order_relaxed);
        p.sequence.store(seq + 2, std::memory_order_release);
    }
};
void invalidate(unsigned deck) noexcept {
    published[deck].active_generation.store(0, std::memory_order_release);
    submitted.fetch_and(~(1u << deck), std::memory_order_acq_rel);
}
} // namespace

extern "C" void azsdk_loop_configure(int on, int build) noexcept {
    enabled.store(0, std::memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) invalidate(d);
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(d); /* Also cover a registration already inside the mutex. */
            owners[d].reset();
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
    } catch (...) { /* Disabled and generations invalid even if a registration lock fails. */
    }
}
extern "C" int azsdk_loop_register(int deck, void *owned_unit) noexcept {
    if (!deck_ok(deck) || !owned_unit || !enabled.load(std::memory_order_acquire)) return 0;
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        if (!enabled.load(std::memory_order_acquire) || owners[deck]) return 0;
        std::uintptr_t unit = reinterpret_cast<std::uintptr_t>(owned_unit);
        if (!canonical(unit)) return 0;
        std::uintptr_t publisher = word(unit + kUnitPublisher);
        if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
        std::uintptr_t server = word(publisher + kPublisherServer);
        std::uintptr_t table = checked_vtable(server, "N9dj_player23StateNotificationServerE", 26);
        if (!table) return 0;
        std::uintptr_t entry = word(table + kObserverSlot * 8);
        if (!image_contains(entry, 4, kExecute)) return 0;
        if (next_generation == UINT64_MAX) return 0;
        std::uint64_t generation = ++next_generation;
        auto listener = std::make_shared<LoopListener>(static_cast<unsigned>(deck), generation);
        owners[deck] = listener;
        published[deck].active_generation.store(generation, std::memory_order_release);
        try {
            using Register = void (*)(void *, std::weak_ptr<Listener>);
            reinterpret_cast<Register>(entry)(reinterpret_cast<void *>(server), std::weak_ptr<Listener>(listener));
        } catch (...) {
            invalidate(static_cast<unsigned>(deck));
            owners[deck].reset();
            return 0;
        }
        submitted.fetch_or(1u << deck, std::memory_order_release);
        return 1;
    } catch (...) { return 0; }
}
extern "C" void azsdk_loop_retire(int deck) noexcept {
    if (!deck_ok(deck)) return;
    invalidate(static_cast<unsigned>(deck));
    try {
        std::lock_guard<std::mutex> lock(registration_mutex);
        invalidate(static_cast<unsigned>(deck));
        owners[deck].reset();
    } catch (...) { /* Any retained core owner remains invalid; it is never repurposed. */
    }
}
extern "C" int azsdk_loop_read(int deck, azsdk_loop_sample *out) noexcept {
    if (!deck_ok(deck) || !out || !enabled.load(std::memory_order_acquire)) return 0;
    Published &p = published[deck];
    std::uint64_t active = p.active_generation.load(std::memory_order_acquire);
    if (!active) return 0;
    std::uint64_t seq = p.sequence.load(std::memory_order_acquire);
    if (seq & 1u) return 0;
    azsdk_loop_sample value = {};
    value.unit_generation = p.generation.load(std::memory_order_relaxed);
    value.revision = p.revision.load(std::memory_order_relaxed);
    value.observed_ns = p.observed.load(std::memory_order_relaxed);
    value.engaged = p.engaged.load(std::memory_order_relaxed);
    value.bounds_valid = p.bounds_valid.load(std::memory_order_relaxed);
    value.reserved = p.bounds_reason.load(std::memory_order_relaxed);
    value.in_s = bits_double(p.in_bits.load(std::memory_order_relaxed));
    value.out_s = bits_double(p.out_bits.load(std::memory_order_relaxed));
    value.check_in_samples = p.check_in_samples.load(std::memory_order_relaxed);
    value.check_out_samples = p.check_out_samples.load(std::memory_order_relaxed);
    value.check_in_cached_us = p.check_in_cached_us.load(std::memory_order_relaxed);
    value.check_out_cached_us = p.check_out_cached_us.load(std::memory_order_relaxed);
    value.check_rate = p.check_rate.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (p.sequence.load(std::memory_order_relaxed) != seq || value.unit_generation != active ||
        !value.revision || !value.observed_ns || p.active_generation.load(std::memory_order_acquire) != active)
        return 0;
    *out = value;
    return 1;
}
extern "C" unsigned azsdk_loop_capabilities(void) noexcept {
    return enabled.load(std::memory_order_acquire) ? submitted.load(std::memory_order_acquire) : 0;
}

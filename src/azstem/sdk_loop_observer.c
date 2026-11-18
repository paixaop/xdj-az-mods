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
#include <stdatomic.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../common/aznative_listener.h"
#include "../common/aznative_sync.h"
#include <time.h>
#if defined(__linux__)
#include <link.h>
#endif


enum {
    kDecks = 4
};
static const unsigned kRead = 4, kExecute = 1;
static const size_t kUnitPublisher = 0x1a0, kPublisherServer = 0x1f28;
static const size_t kObserverSlot = 11;
_Static_assert(sizeof(uintptr_t) == 8, "native listener requires a 64-bit ABI");
_Static_assert(sizeof(unsigned) == 4, "native player number requires a 32-bit ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "loop callback publication must be lock-free");

struct ImageRange {
    uintptr_t begin, end;
    unsigned flags;
};
/* The main ELF mappings live for this process. Initialize before the first listener and
 * never rewrite them: borrowed callbacks may still be in flight during reconfiguration. */
static struct ImageRange image[32];
static unsigned image_count;
static pthread_mutex_t registration_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(unsigned) enabled, submitted;
static uint64_t next_generation; /* Registration mutex; never wrap and reuse an identity. */
struct Published {
    _Atomic(uint64_t) sequence, active_generation;
    _Atomic(uint64_t) generation, revision, observed;
    _Atomic(int) engaged;
    _Atomic(int) bounds_valid, bounds_reason;
    _Atomic(uint64_t) in_bits, out_bits;
    _Atomic(uint64_t) check_in_samples, check_out_samples;
    _Atomic(uint64_t) check_in_cached_us, check_out_cached_us;
    _Atomic(uint32_t) check_rate;
};
static struct Published published[kDecks] = {
    {.bounds_reason = 1}, {.bounds_reason = 1}, {.bounds_reason = 1}, {.bounds_reason = 1}};
static struct az_native_owner owners[kDecks]; /* Registration paths only, protected by mutex. */

static bool deck_ok(int deck) { return deck >= 0 && ((unsigned) (deck)) < kDecks; }
static bool canonical(uintptr_t p) {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
static bool image_contains(uintptr_t p, size_t n, unsigned flags) {
    if (n > UINTPTR_MAX - p) return false;
    for (unsigned i = 0; i < image_count; ++i)
        if ((image[i].flags & flags) == flags && p >= image[i].begin && p + n <= image[i].end)
            return true;
    return false;
}
static uintptr_t word(uintptr_t p) {
    uintptr_t out;
    memcpy(&out, ((const void *) (p)), sizeof out);
    return out;
}
#if defined(__linux__)
static int main_image(struct dl_phdr_info *info, size_t size __attribute__((unused)), void *context __attribute__((unused))) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type h = &(info->dlpi_phdr[i]);
        if (h->p_type != PT_LOAD || !(h->p_flags & PF_R)) continue;
        if (image_count == sizeof image / sizeof *image) {
            image_count = 0;
            return 1;
        }
        uintptr_t base = info->dlpi_addr;
        if (h->p_vaddr > UINTPTR_MAX - base ||
            h->p_memsz > UINTPTR_MAX - base - h->p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = (struct ImageRange) {base + h->p_vaddr, base + h->p_vaddr + h->p_memsz, h->p_flags};
    }
    return 1;
}
#endif
/* Object liveness comes from the caller's native constructor scope, not these range checks.
 * ELF bounds constrain vtable/typeinfo/name/code reads to the main executable only. */
static uintptr_t checked_vtable(uintptr_t object, const char *expected, unsigned slots) {
    if (!canonical(object)) return 0;
    uintptr_t table = word(object);
    if (!canonical(table) || table < 16 ||
        !image_contains(table - 16, (slots + 2u) * 8u, kRead) || word(table - 16)) return 0;
    uintptr_t type = word(table - 8);
    if (!canonical(type) || !image_contains(type, 16, kRead)) return 0;
    uintptr_t name = word(type + 8);
    /* GNU type_info::name strips '*' on an unmerged name. It denotes the same type. */
    if (!image_contains(name, 1, kRead)) return 0;
    if (*((const char *) (name)) == '*') ++name;
    size_t length = strlen(expected) + 1;
    if (!image_contains(name, length, kRead)) return 0;
    /* A different RTTI name may occupy a shorter C string in the same ELF segment.
     * Stop at its first mismatch/NUL; a fixed-size memcmp crosses that object's end. */
    const volatile char *actual = ((const volatile char *) (name));
    for (size_t i = 0; i < length; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
static uint64_t observed_now() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = ((uint64_t) (ts.tv_sec));
    if (seconds > (UINT64_MAX - ((unsigned long) (ts.tv_nsec))) / UINT64_C(1000000000))
        return 0;
    return seconds * UINT64_C(1000000000) + ((unsigned long) (ts.tv_nsec));
}

/* Exact native InstantDoublesTargetOnLocalPlayer::isActiveLoopPlay policy:
 * 1.30 a609f0 -> 1492ae8 -> 13ec228; 2.00 aa69c8 -> 154e058 -> 14a7668.
 * Both publishers copy the CURRENT working property, which EXIT clears independently of
 * retained cue slots. Byte 0 describes a separate mode and is not loop engagement.
 * Copy only scalars while the native callback owns its full 0x100-byte notification. */
static bool working_loop_engaged(const void *notification) {
    unsigned char scalar[0x18];
    const unsigned char *bytes = ((const unsigned char *) (notification));
    memcpy(scalar, bytes + 0x38, sizeof scalar);
    uint32_t nonzero = 0;
    for (unsigned i = 0; i < 4; ++i) nonzero |= ((uint32_t) (scalar[8 + i])) << (i * 8);
    uint64_t position = 0;
    for (unsigned i = 0; i < 8; ++i) position |= ((uint64_t) (scalar[16 + i])) << (i * 8);
    return scalar[0] != 0 && (scalar[4] & 0x1f) != 0 && scalar[6] != 0 && nonzero != 0 &&
           position != UINT64_C(0x7fffffffffffffff);
}

static uint32_t scalar32(const unsigned char *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= ((uint32_t) (p[i])) << (8 * i);
    return value;
}
static uint64_t scalar64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= ((uint64_t) (p[i])) << (8 * i);
    return value;
}
static uint64_t double_bits(double value) {
    uint64_t bits;
    _Static_assert(sizeof bits == sizeof value, "loop coordinates require binary64 storage");
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
static double bits_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
static bool source_position_valid(const unsigned char *source) {
    return source[0] && (source[4] & 0x1f) && source[6] && scalar32(source + 8) &&
           scalar64(source + 16) != UINT64_C(0x7fffffffffffffff);
}
struct Bounds {
    int reason; /* Internal first-failure diagnostic; never a native pointer or SDK field. */
    bool valid;
    double in_s, out_s;
    uint64_t check_in_samples, check_out_samples;
    uint64_t check_in_cached_us, check_out_cached_us;
    uint32_t check_rate;
};
static struct Bounds rejected_bounds(int reason) {
    struct Bounds result = {.reason = 1};
    result.reason = reason;
    return result;
}
/* SDK consistency policy: permit one source sample of rounding/truncation and
 * half a microsecond for the integer cache. It does not describe every native
 * setter. Cross multiplication keeps the boundary exact, even above double's
 * integer precision; the largest intermediate is below 2^97 for u64/u32 inputs.
 * Source samples, not the cache, remain the published coordinate authority. */
static bool source_time_consistent_us(uint64_t samples, uint32_t rate, uint64_t us) {
    if (!rate) return false;
    typedef unsigned __int128 Wide;
    Wide source = ((Wide) (samples)) * 1000000u;
    Wide cached = ((Wide) (us)) * rate;
    return compare_timestamps(2u * source, 2u * cached, 1, 1, UINT64_C(2000000) + rate + 1) == AZ_TIME_EQUAL;
}
/* Both builds: CueProperty IN/OUT at +0x18/+0x70, PositionWithSourceInfo at point+0x10.
 * Setter 1.30 13ef020 / 2.00 14aa460 stores signed samples/Hertz and caches microseconds
 * using FCVTAS: nearest, ties away from zero, after (1e6 / rate) * samples.
 * This is a bounded borrowed-record read ending at notification+0xd0. SourceInfo words
 * are compared only as opaque associations, never followed or retained. Point+8 is an
 * independent native setter bool; neither native position predicate requires it. */
static struct Bounds working_loop_bounds(const void *notification, bool engaged) {
    struct Bounds result = {.reason = 1};
    if (!engaged) return result;
    uintptr_t property = ((uintptr_t) (notification)) + 8;
    if (!checked_vtable(property, "N9dj_player11CuePropertyE", 1)) return rejected_bounds(2);
    if (!checked_vtable(property + 0x18, "N9dj_player13PointPropertyE", 1)) return rejected_bounds(3);
    if (!checked_vtable(property + 0x70, "N9dj_player13PointPropertyE", 1)) return rejected_bounds(4);
    if (!checked_vtable(property + 0x28, "N12player_basis22PositionWithSourceInfoE", 1)) return rejected_bounds(5);
    if (!checked_vtable(property + 0x80, "N12player_basis22PositionWithSourceInfoE", 1)) return rejected_bounds(6);
    unsigned char bytes[0xc8];
    memcpy(bytes, ((const void *) (property)), sizeof bytes);
    if (!source_position_valid(bytes + 0x30) || !source_position_valid(bytes + 0x88)) return rejected_bounds(7);
    if (memcmp(bytes + 0x30, bytes + 0x88, 12)) return rejected_bounds(8);
    uint32_t rate = scalar32(bytes + 0x60);
    if (!rate || rate >= UINT32_C(0x7fffffff) || rate != scalar32(bytes + 0xb8)) return rejected_bounds(9);
    /* Native stored-cue conversion creates independently owned Source objects
     * for IN and OUT. Their checked SourceInfo/rate above define association,
     * and each opaque source reference must remain present. Neither is followed. */
    if (!scalar64(bytes + 0x50) || !scalar64(bytes + 0xa8)) return rejected_bounds(10);
    uint64_t in = scalar64(bytes + 0x40), out = scalar64(bytes + 0x98);
    uint64_t in_us = scalar64(bytes + 0x68), out_us = scalar64(bytes + 0xc0);
    /* Technical precision bound, not a media-duration policy. It also rejects negative
     * signed values and INT64_MAX without implementation-defined unsigned-to-signed casts. */
    const uint64_t exact_integer = UINT64_C(9007199254740992);
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
    result.in_s = ((double) (in)) / ((double) (rate));
    result.out_s = ((double) (out)) / ((double) (rate));
    if (!isfinite(result.in_s) || !isfinite(result.out_s) || result.out_s <= result.in_s) {
        result.in_s = result.out_s = 0;
        result.reason = 13;
        return result;
    }
    result.valid = true;
    result.reason = 0;
    return result;
}

struct LoopListener {
    const void *const *vtable;
    unsigned deck;
    uint64_t generation;
};
static void listener_destroy(void *opaque) { (void) opaque; }
static void callback2(void *opaque, const void *notification) {
    (void) opaque;
    (void) notification;
}
static void callback3(void *opaque, const void *notification, unsigned player) {
    const struct LoopListener *listener = opaque;
    const unsigned deck = listener->deck;
    const uint64_t generation = listener->generation;
    if (player != deck) return;
    struct Published *p = &(published[deck]);
    if (atomic_load_explicit(&(p->active_generation), memory_order_acquire) != generation) return;
    bool engaged = working_loop_engaged(notification);
    struct Bounds bounds = working_loop_bounds(notification, engaged);
    uint64_t seq = atomic_load_explicit(&(p->sequence), memory_order_acquire);
    if ((seq & 1u) || !atomic_compare_exchange_strong_explicit(&(p->sequence), &(seq), seq + 1, memory_order_acq_rel, memory_order_relaxed)) return;
    if (atomic_load_explicit(&(p->active_generation), memory_order_acquire) != generation) {
        atomic_store_explicit(&(p->sequence), seq + 2, memory_order_release);
        return;
    }
    uint64_t now = observed_now();
    if (!now) {
        atomic_store_explicit(&(p->sequence), seq + 2, memory_order_release);
        return;
    }
    uint64_t revision = atomic_load_explicit(&(p->revision), memory_order_relaxed);
    if (revision == UINT64_MAX) {
        atomic_store_explicit(&(p->sequence), seq + 2, memory_order_release);
        return;
    }
    atomic_store_explicit(&(p->generation), generation, memory_order_relaxed);
    atomic_store_explicit(&(p->engaged), engaged, memory_order_relaxed);
    atomic_store_explicit(&(p->bounds_valid), bounds.valid, memory_order_relaxed);
    atomic_store_explicit(&(p->bounds_reason), bounds.reason, memory_order_relaxed);
    atomic_store_explicit(&(p->in_bits), double_bits(bounds.in_s), memory_order_relaxed);
    atomic_store_explicit(&(p->out_bits), double_bits(bounds.out_s), memory_order_relaxed);
    atomic_store_explicit(&(p->check_in_samples), bounds.check_in_samples, memory_order_relaxed);
    atomic_store_explicit(&(p->check_out_samples), bounds.check_out_samples, memory_order_relaxed);
    atomic_store_explicit(&(p->check_in_cached_us), bounds.check_in_cached_us, memory_order_relaxed);
    atomic_store_explicit(&(p->check_out_cached_us), bounds.check_out_cached_us, memory_order_relaxed);
    atomic_store_explicit(&(p->check_rate), bounds.check_rate, memory_order_relaxed);
    atomic_store_explicit(&(p->observed), now, memory_order_relaxed);
    atomic_store_explicit(&(p->revision), revision + 1, memory_order_relaxed);
    atomic_store_explicit(&(p->sequence), seq + 2, memory_order_release);
}
static const void *const listener_vtable[] = {
    (void *) listener_destroy, (void *) listener_destroy, (void *) callback2, (void *) callback3};
static void invalidate(unsigned deck) {
    atomic_store_explicit(&(published[deck].active_generation), 0, memory_order_release);
    atomic_fetch_and_explicit(&(submitted), ~(1u << deck), memory_order_acq_rel);
}


void azsdk_loop_configure(int on, int build) {
    atomic_store_explicit(&(enabled), 0, memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) invalidate(d);
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return;
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(d); /* Also cover a registration already inside the mutex. */
            az_native_owner_reset(&owners[d]);
        }
#if defined(__linux__)
        if (on && (build == 1 || build == 2)) {
            if (!image_count) dl_iterate_phdr(main_image, NULL);
            if (image_count) atomic_store_explicit(&(enabled), 1, memory_order_release);
        }
#else
        (void) on;
        (void) build;
#endif
    }
}
/* A provisional listener must not survive an interrupted registration. The
 * caller still owns registration_mutex while this cleanup runs. */
static void registration_drop(int *deck) {
    if (*deck < 0) return;
    invalidate((unsigned) *deck);
    az_native_owner_reset(&owners[*deck]);
}
int azsdk_loop_register(int deck, void *owned_unit) {
    if (!deck_ok(deck) || !owned_unit || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return 0;
        if (!atomic_load_explicit(&(enabled), memory_order_acquire) || owners[deck].object) return 0;
        uintptr_t unit = ((uintptr_t) (owned_unit));
        if (!canonical(unit)) return 0;
        uintptr_t publisher = word(unit + kUnitPublisher);
        if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
        uintptr_t server = word(publisher + kPublisherServer);
        uintptr_t table = checked_vtable(server, "N9dj_player23StateNotificationServerE", 26);
        if (!table) return 0;
        uintptr_t entry = word(table + kObserverSlot * 8);
        if (!image_contains(entry, 4, kExecute)) return 0;
        if (next_generation == UINT64_MAX) return 0;
        uint64_t generation = ++next_generation;
        struct az_native_owner listener __attribute__((cleanup(az_native_owner_reset))) =
            az_listener_create(sizeof(struct LoopListener), listener_vtable);
        if (!listener.object) return 0;
        struct LoopListener *object = listener.object;
        object->deck = (unsigned) deck;
        object->generation = generation;
        az_native_owner_copy(&owners[deck], &listener);
        atomic_store_explicit(&(published[deck].active_generation), generation, memory_order_release);
        int pending_deck __attribute__((cleanup(registration_drop))) = deck;
        az_listener_register(entry, server, &listener);
        pending_deck = -1;
        atomic_fetch_or_explicit(&(submitted), 1u << deck, memory_order_release);
        return 1;
    }
}
void azsdk_loop_retire(int deck) {
    if (!deck_ok(deck)) return;
    invalidate(((unsigned) (deck)));
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return;
        invalidate(((unsigned) (deck)));
        az_native_owner_reset(&owners[deck]);
    }
}
int azsdk_loop_read(int deck, struct azsdk_loop_sample *out) {
    if (!deck_ok(deck) || !out || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    struct Published *p = &(published[deck]);
    uint64_t active = atomic_load_explicit(&(p->active_generation), memory_order_acquire);
    if (!active) return 0;
    uint64_t seq = atomic_load_explicit(&(p->sequence), memory_order_acquire);
    if (seq & 1u) return 0;
    struct azsdk_loop_sample value = {0};
    value.unit_generation = atomic_load_explicit(&(p->generation), memory_order_relaxed);
    value.revision = atomic_load_explicit(&(p->revision), memory_order_relaxed);
    value.observed_ns = atomic_load_explicit(&(p->observed), memory_order_relaxed);
    value.engaged = atomic_load_explicit(&(p->engaged), memory_order_relaxed);
    value.bounds_valid = atomic_load_explicit(&(p->bounds_valid), memory_order_relaxed);
    value.reserved = atomic_load_explicit(&(p->bounds_reason), memory_order_relaxed);
    value.in_s = bits_double(atomic_load_explicit(&(p->in_bits), memory_order_relaxed));
    value.out_s = bits_double(atomic_load_explicit(&(p->out_bits), memory_order_relaxed));
    value.check_in_samples = atomic_load_explicit(&(p->check_in_samples), memory_order_relaxed);
    value.check_out_samples = atomic_load_explicit(&(p->check_out_samples), memory_order_relaxed);
    value.check_in_cached_us = atomic_load_explicit(&(p->check_in_cached_us), memory_order_relaxed);
    value.check_out_cached_us = atomic_load_explicit(&(p->check_out_cached_us), memory_order_relaxed);
    value.check_rate = atomic_load_explicit(&(p->check_rate), memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&(p->sequence), memory_order_relaxed) != seq || value.unit_generation != active ||
        !value.revision || !value.observed_ns || atomic_load_explicit(&(p->active_generation), memory_order_acquire) != active)
        return 0;
    *out = value;
    return 1;
}
unsigned azsdk_loop_capabilities(void) {
    return atomic_load_explicit(&(enabled), memory_order_acquire) ? atomic_load_explicit(&(submitted), memory_order_acquire) : 0;
}

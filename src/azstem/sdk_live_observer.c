/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded GNU weak observer bridge. Shared owning chain with sdk_loop_observer.c:
 * unit+0x1a0 -> UnitStateServer+0x1f28 -> StateNotificationServer. Virtual slots 15/19/21/2
 * select play-with-state, quantize, key-lock and Sync observers on both supported builds.
 * Only borrowed notification bytes are copied; no native object or source pointer is kept.
 * Signatures/layout evidence and unresolved semantic normalization are documented in
 * CORE-SDK-LIVE-READERS.md. Registration and receipt never establish committed load identity. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azliveobserver.h"
#ifdef AZ_STATUS_NATIVE_PRODUCER
#include "../common/azstatus_native.h"
#include "../azmod/azmod-mod.h"
#endif
#include <stdatomic.h>
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
    kDecks = 4,
    kLegacyStreams = 3,
    kSync = 3,
    kTempoLock = 4,
    kStreams = 5,
    kRead = 4,
    kExecute = 1
};
static const size_t kUnitPublisher = 0x1a0, kPublisherServer = 0x1f28;
static const unsigned slots[kStreams] = {15, 19, 21, 2, 5};
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(unsigned) == 4, "native scalar ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "live callbacks must publish without locks");
struct ImageRange {
    uintptr_t begin, end;
    unsigned flags;
};
static struct ImageRange image[32];
static unsigned image_count;
static pthread_mutex_t registration_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(unsigned) enabled, submitted;
static uint64_t next_generation;
/* Separate streams avoid one kind of callback blocking another kind's publication. */
struct Component {
    _Atomic(uint64_t) sequence, generation, revision, observed;
    _Atomic(unsigned) valid, a, b, c;
};
struct Published {
    _Atomic(uint64_t) active_generation;
#ifdef AZ_STATUS_NATIVE_PRODUCER
    _Atomic(uintptr_t) unit;
#endif
    struct Component components[kStreams];
};
static struct Published published[kDecks];
static struct az_native_owner owners[kDecks][kStreams];
static bool deck_ok(int d) { return d >= 0 && ((unsigned) (d)) < kDecks; }
static bool canonical(uintptr_t p) {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
static bool image_contains(uintptr_t p, size_t n, unsigned flags) {
    if (n > UINTPTR_MAX - p) return false;
    for (unsigned i = 0; i < image_count; ++i)
        if ((image[i].flags & flags) == flags && p >= image[i].begin && p + n <= image[i].end) return true;
    return false;
}
static uintptr_t word(uintptr_t p) {
    uintptr_t v;
    memcpy(&v, ((const void *) (p)), sizeof v);
    return v;
}
#if defined(__linux__)
static int main_image(struct dl_phdr_info *info, size_t size __attribute__((unused)), void *context __attribute__((unused))) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type h = &(info->dlpi_phdr[i]);
        if (h->p_type != PT_LOAD || !(h->p_flags & PF_R)) continue;
        uintptr_t base = info->dlpi_addr;
        if (image_count == sizeof image / sizeof *image ||
            h->p_vaddr > UINTPTR_MAX - base ||
            h->p_memsz > UINTPTR_MAX - base - h->p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = (struct ImageRange) {base + h->p_vaddr, base + h->p_vaddr + h->p_memsz, h->p_flags};
    }
    return 1;
}
#endif
static uintptr_t checked_type_name(uintptr_t table, const char *expected) {
    uintptr_t type = word(table - 8);
    if (!canonical(type) || !image_contains(type, 16, kRead)) return 0;
    uintptr_t name = word(type + 8);
    if (!image_contains(name, 1, kRead)) return 0;
    if (*((const char *) (name)) == '*') ++name;
    size_t bytes = strlen(expected) + 1;
    if (!image_contains(name, bytes, kRead)) return 0;
    /* A different RTTI name may occupy a shorter C string in the same ELF segment.
     * Stop at its first mismatch/NUL; a fixed-size memcmp crosses that object's end. */
    const volatile char *actual = ((const volatile char *) (name));
    for (size_t i = 0; i < bytes; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
static uintptr_t checked_vtable(uintptr_t object, const char *expected, unsigned count) {
    if (!canonical(object)) return 0;
    uintptr_t table = word(object);
    if (!canonical(table) || table < 16 || !image_contains(table - 16, (count + 2u) * 8u, kRead) || word(table - 16)) return 0;
    return checked_type_name(table, expected);
}
static uint64_t observed_now() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    uint64_t s = ((uint64_t) (t.tv_sec));
    if (s > (UINT64_MAX - ((unsigned long) (t.tv_nsec))) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + ((unsigned long) (t.tv_nsec));
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
static _Atomic(bool) status_enabled;
static void raw_play(unsigned deck, uint64_t unit, uint64_t revision, uint64_t now,
                     unsigned a, unsigned b, unsigned c, int valid, uint64_t lost) {
    if (!atomic_load_explicit(&(status_enabled), memory_order_acquire)) return;
    struct azstatus_native_receipt r = {0};
    r.kind = AZN_PLAY;
    r.deck = deck;
    r.unit = unit;
    r.observed_ns = now;
    r.revision = revision;
    r.a = (int) (a);
    r.b = (int) (b);
    r.c = (int) (c);
    r.valid = valid;
    r.lost_topics = lost;
    azn_emit(&r);
}
#endif
struct Notification {
    unsigned a, b, c;
    bool valid;
};
static struct Notification read_notification(unsigned stream, const void *n) {
    unsigned a = 0, b = 0, c = 0;
    if (stream == 0) {
        unsigned char bytes[3];
        memcpy(bytes, n, sizeof bytes);
        a = bytes[0];
        b = bytes[1];
        c = bytes[2];
    } else {
        /* Quantize/key-lock zero-extend a byte; Sync carries a full 32-bit enum. */
        uint32_t value;
        memcpy(&value, n, sizeof value);
        a = value;
    }
    bool valid = a <= 1 && b <= 1 && c <= 1;
    if (stream == kSync) valid = a == 0 || a == 2;
    return (struct Notification) {a, b, c, valid};
}
static bool claim_component(struct Component *v, unsigned deck, unsigned stream, uint64_t generation, uint64_t *seq) {
#ifndef AZ_STATUS_NATIVE_PRODUCER
    (void) deck;
    (void) stream;
    (void) generation;
#endif
    *seq = atomic_load_explicit(&(v->sequence), memory_order_acquire);
    if ((*seq & 1u) || *seq > UINT64_MAX - 2 || !atomic_compare_exchange_strong_explicit(&(v->sequence), seq, *seq + 1, memory_order_acq_rel, memory_order_relaxed)) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
        if (!stream) raw_play(deck, generation, 0, observed_now(), 0, 0, 0, 0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
#endif
        return false;
    }
    return true;
}
static void publish(unsigned deck, unsigned stream, uint64_t generation, const void *n) {
    struct Published *p = &(published[deck]);
    if (atomic_load_explicit(&(p->active_generation), memory_order_acquire) != generation) return;
    struct Notification notification = read_notification(stream, n);
    unsigned a = notification.a, b = notification.b, c = notification.c;
    bool valid = notification.valid;
    struct Component *v = &(p->components[stream]);
    uint64_t seq;
    if (!claim_component(v, deck, stream, generation, &seq)) return;
    if (atomic_load_explicit(&(p->active_generation), memory_order_acquire) != generation) {
        atomic_store_explicit(&(v->sequence), seq + 2, memory_order_release);
        return;
    }
    uint64_t now = observed_now(), revision = atomic_load_explicit(&(v->revision), memory_order_relaxed);
    if (!now || revision == UINT64_MAX) {
        atomic_store_explicit(&(v->sequence), seq + 2, memory_order_release);
        return;
    }
    atomic_store_explicit(&(v->generation), generation, memory_order_relaxed);
    atomic_store_explicit(&(v->valid), valid, memory_order_relaxed);
    atomic_store_explicit(&(v->a), a, memory_order_relaxed);
    atomic_store_explicit(&(v->b), b, memory_order_relaxed);
    atomic_store_explicit(&(v->c), c, memory_order_relaxed);
    atomic_store_explicit(&(v->observed), now, memory_order_relaxed);
    atomic_store_explicit(&(v->revision), revision + 1, memory_order_relaxed);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    /* Capture every authenticated notification while its native borrow is
     * valid, before releasing the latest-value slot to a seed reader. */
    if (!stream && atomic_load_explicit(&(status_enabled), memory_order_acquire)) {
        azn_raw_state(deck, generation, 0);
        raw_play(deck, generation, revision + 1, now, a, b, c, valid, 0);
    }
#endif
    atomic_store_explicit(&(v->sequence), seq + 2, memory_order_release);
}
struct CaptureListener {
    const void *const *vtable;
    unsigned deck, stream;
    uint64_t generation;
};
static void listener_destroy(void *opaque) { (void) opaque; }
static void callback2(void *opaque, const void *n) {
    (void) opaque;
    (void) n;
}
static void callback3(void *opaque, const void *n, unsigned player) {
    const struct CaptureListener *listener = opaque;
    if (player != listener->deck) return;
    publish(listener->deck, listener->stream, listener->generation, n);
}
static const void *const listener_vtable[] = {
    (void *) listener_destroy, (void *) listener_destroy, (void *) callback2, (void *) callback3};
static void pending_release(struct az_native_owner (*pending)[kStreams]) {
    for (unsigned s = 0; s < kStreams; ++s) az_native_owner_reset(&(*pending)[s]);
}
struct ScalarReceipt {
    uint64_t unit_generation, revision, observed_ns;
    int32_t raw;
    unsigned valid;
};
static bool scalar_current(struct Published *p, struct Component *v, uint64_t seq, uint64_t active, const struct ScalarReceipt *value) {
    if (atomic_load_explicit(&(v->sequence), memory_order_relaxed) != seq || value->unit_generation != active ||
        !value->revision || !value->observed_ns || atomic_load_explicit(&(p->active_generation), memory_order_acquire) != active) return false;
    return true;
}
static bool read_scalar(int deck, unsigned stream, struct ScalarReceipt *value) {
    if (!deck_ok(deck) || !atomic_load_explicit(&(enabled), memory_order_acquire)) return false;
    struct Published *p = &(published[deck]);
    uint64_t active = atomic_load_explicit(&(p->active_generation), memory_order_acquire);
    if (!active) return false;
    struct Component *v = &(p->components[stream]);
    uint64_t seq = atomic_load_explicit(&(v->sequence), memory_order_acquire);
    if (seq & 1u) return false;

    value->unit_generation = atomic_load_explicit(&(v->generation), memory_order_relaxed);
    value->revision = atomic_load_explicit(&(v->revision), memory_order_relaxed);
    value->observed_ns = atomic_load_explicit(&(v->observed), memory_order_relaxed);
    unsigned raw = atomic_load_explicit(&(v->a), memory_order_relaxed);
    value->valid = atomic_load_explicit(&(v->valid), memory_order_relaxed);
    memcpy(&value->raw, &raw, sizeof value->raw);
    atomic_thread_fence(memory_order_acquire);
    if (!scalar_current(p, v, seq, active, value)) return false;
    return true;
}
static void invalidate(unsigned deck) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_store_explicit(&(published[deck].unit), 0, memory_order_release);
    __auto_type prior = atomic_exchange_explicit(&(published[deck].active_generation), 0, memory_order_acq_rel);
    if (prior && atomic_load_explicit(&(status_enabled), memory_order_acquire)) {
        azn_raw_state(deck, prior, 1);
        raw_play(deck, prior, 0, observed_now(), 0, 0, 0, -1, 0);
    }
#else
    atomic_store_explicit(&(published[deck].active_generation), 0, memory_order_release);
#endif
    atomic_fetch_and_explicit(&(submitted), ~(1u << deck), memory_order_acq_rel);
}


static void enable_live_profile(int on, int build) {
#if defined(__linux__)
    if (on && (build == 1 || build == 2)) {
        if (!image_count) dl_iterate_phdr(main_image, NULL);
        if (image_count) atomic_store_explicit(&(enabled), on == 2 ? 2 : 1, memory_order_release);
    }
#else
    (void) on;
    (void) build;
#endif
}
void azsdk_live_configure(int on, int build) {
    atomic_store_explicit(&(enabled), 0, memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) invalidate(d);
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return;
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(d);
            for (unsigned s = 0; s < kStreams; ++s) az_native_owner_reset(&owners[d][s]);
        }
        enable_live_profile(on, build);
    }
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
void azsdk_live_status_start(int build, int legacy_active) {
    if (!legacy_active) azsdk_live_configure(2, build);
    atomic_store_explicit(&(status_enabled), true, memory_order_release);
}
#endif
/* A provisional listener must not survive an interrupted registration. The
 * caller still owns registration_mutex while this cleanup runs. */
static void registration_drop(int *deck) {
    if (*deck < 0) return;
    invalidate((unsigned) *deck);
    for (unsigned i = 0; i < kStreams; ++i) az_native_owner_reset(&owners[*deck][i]);
}
static int live_binding(uintptr_t unit, uintptr_t *server, uintptr_t *table) {

    if (!canonical(unit)) return 0;
    uintptr_t publisher = word(unit + kUnitPublisher);
    if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
    *server = word(publisher + kPublisherServer);
    *table = checked_vtable(*server, "N9dj_player23StateNotificationServerE", 26);
    if (!*table) return 0;
    return 1;
}
static int live_entries(uintptr_t table, unsigned mask, uintptr_t entries[kStreams]) {
    for (unsigned s = 0; s < kStreams; ++s) {
        if (!(mask & (1u << s))) continue;
        entries[s] = word(table + slots[s] * 8);
        if (!image_contains(entries[s], 4, kExecute)) return 0;
    }
    return 1;
}
static int allocate_live_listeners(int deck, uint64_t generation, unsigned mask, struct az_native_owner pending[kStreams]) {
    for (unsigned s = 0; s < kStreams; ++s) {
        if (!(mask & (1u << s))) continue;
        pending[s] = az_listener_create(sizeof(struct CaptureListener), listener_vtable);
        if (!pending[s].object) return 0;
        struct CaptureListener *listener = pending[s].object;
        listener->deck = (unsigned) deck;
        listener->stream = s;
        listener->generation = generation;
    }
    return 1;
}
static int register_live_listeners(int deck, uintptr_t unit, uintptr_t server, uintptr_t table) {
#ifndef AZ_STATUS_NATIVE_PRODUCER
    (void) unit;
#endif
    const bool passive = atomic_load_explicit(&(enabled), memory_order_acquire) == 2;
    const unsigned mask = passive ? (1u | (1u << kSync)) : (1u << kStreams) - 1;
    uintptr_t entries[kStreams];
    if (!live_entries(table, mask, entries)) return 0;
    if (next_generation == UINT64_MAX) return 0;
    uint64_t generation = ++next_generation;
        /* Allocate every selected listener before publishing a generation. */
    struct az_native_owner pending[kStreams] __attribute__((cleanup(pending_release))) = {0};
    // NOLINTNEXTLINE(clang-analyzer-unix.Malloc): pending_release cleans the array on failure and native unwind.
    if (!allocate_live_listeners(deck, generation, mask, pending)) return 0;
    for (unsigned s = 0; s < kStreams; ++s) az_native_owner_copy(&owners[deck][s], &pending[s]);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_store_explicit(&(published[deck].unit), unit, memory_order_release);
#endif
    atomic_store_explicit(&(published[deck].active_generation), generation, memory_order_release);
    int pending_deck __attribute__((cleanup(registration_drop))) = deck;
    for (unsigned s = 0; s < kStreams; ++s) {
        if (!(mask & (1u << s))) continue;
        az_listener_register(entries[s], server, &owners[deck][s]);
    }
    pending_deck = -1;
    (void) pending_deck; /* Successful registration disarms the cleanup rollback. */
    atomic_fetch_or_explicit(&(submitted), 1u << deck, memory_order_release);
    return 1;
}
int azsdk_live_register(int deck, void *owned_unit) {
    if (!deck_ok(deck) || !owned_unit || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return 0;
        if (!atomic_load_explicit(&(enabled), memory_order_acquire) || owners[deck][0].object) return 0;
        uintptr_t unit = (uintptr_t) owned_unit, server, table;
        if (!live_binding(unit, &server, &table)) return 0;
        return register_live_listeners(deck, unit, server, table);
    }
}
void azsdk_live_retire(int deck) {
    if (!deck_ok(deck)) return;
    invalidate(((unsigned) (deck)));
    {
        AZ_MUTEX_GUARD(lock, registration_mutex, false);
        if (lock.error) return;
        invalidate(((unsigned) (deck)));
        for (unsigned s = 0; s < kStreams; ++s) az_native_owner_reset(&owners[deck][s]);
    }
}
static void apply_legacy_stream(unsigned s, unsigned valid, unsigned a, unsigned b, unsigned c, struct azsdk_live_sample *value) {
    if (!valid) return;
    value->valid_fields |= 1u << s;
    if (s == 0) {
        value->mode_is_2 = ((int) (a));
        value->play_pause = ((int) (b));
        value->play_aux = ((int) (c));
    } else if (s == 1) value->quantize = ((int) (a));
    else value->key_lock = ((int) (a));
}
static void read_legacy_stream(struct Component *v, uint64_t active, unsigned s, struct azsdk_live_sample *value) {
    uint64_t seq = atomic_load_explicit(&(v->sequence), memory_order_acquire);
    if (seq & 1u) return;
    uint64_t gen = atomic_load_explicit(&(v->generation), memory_order_relaxed);
    uint64_t rev = atomic_load_explicit(&(v->revision), memory_order_relaxed);
    uint64_t time = atomic_load_explicit(&(v->observed), memory_order_relaxed);
    unsigned valid = atomic_load_explicit(&(v->valid), memory_order_relaxed);
    unsigned a = atomic_load_explicit(&(v->a), memory_order_relaxed), b = atomic_load_explicit(&(v->b), memory_order_relaxed), c = atomic_load_explicit(&(v->c), memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&(v->sequence), memory_order_relaxed) != seq || gen != active || !rev || !time) return;
    value->observed_fields |= 1u << s;
    value->revisions[s] = rev;
    value->observed_ns[s] = time;
    apply_legacy_stream(s, valid, a, b, c, value);
}
int azsdk_live_read(int deck, struct azsdk_live_sample *out) {
    if (!deck_ok(deck) || !out || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    struct Published *p = &(published[deck]);
    uint64_t active = atomic_load_explicit(&(p->active_generation), memory_order_acquire);
    if (!active) return 0;
    struct azsdk_live_sample value = {0};
    value.unit_generation = active;
    value.mode_is_2 = value.play_pause = value.play_aux = value.quantize = value.key_lock = -1;
    for (unsigned s = 0; s < kLegacyStreams; ++s) {
        read_legacy_stream(&p->components[s], active, s, &value);
    }
    if (!value.observed_fields || atomic_load_explicit(&(p->active_generation), memory_order_acquire) != active) return 0;
    *out = value;
    return 1;
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
int azn_sync_seed(unsigned deck, uintptr_t unit, int *raw) {
    if (deck >= kDecks || !unit || !raw || atomic_load_explicit(&(published[deck].unit), memory_order_acquire) != unit) return 0;
    struct ScalarReceipt value = {0};
    if (!read_scalar((int) deck, kSync, &value) || !value.valid ||
        atomic_load_explicit(&(published[deck].unit), memory_order_acquire) != unit ||
        atomic_load_explicit(&(published[deck].active_generation), memory_order_acquire) != value.unit_generation) return 0;
    *raw = (int) (value.raw);
    return 1;
}
#endif
int azsdk_sync_read(int deck, struct azsdk_sync_sample *out) {
    if (!out) return 0;
    struct ScalarReceipt value = {0};
    if (!read_scalar(deck, kSync, &value)) return 0;
    *out = (struct azsdk_sync_sample) {value.unit_generation, value.revision, value.observed_ns, value.raw, value.valid};
    return 1;
}
unsigned azsdk_live_capabilities(void) {
    return atomic_load_explicit(&(enabled), memory_order_acquire) ? atomic_load_explicit(&(submitted), memory_order_acquire) : 0;
}
int azsdk_tempo_lock_read(int deck, struct azsdk_tempo_lock_sample *out) {
    if (!out) return 0;
    struct ScalarReceipt value = {0};
    if (!read_scalar(deck, kTempoLock, &value)) return 0;
    *out = (struct azsdk_tempo_lock_sample) {value.unit_generation, value.revision, value.observed_ns, value.raw, value.valid};
    return 1;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Native virtual entry/receipt tap. No source decode, native tasks or code patch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azcuecommands.h"
#include "../common/azloadobserver.h"
#ifdef AZ_STATUS_NATIVE_PRODUCER
#include "../common/azstatus_source_kind.h"
#endif
#ifdef AZ_STATUS_NATIVE_PRODUCER
#include "../common/azstatus_native.h"
#ifdef AZ_STATUS_MEMORY_PRODUCER
#include "../common/azstatus_memory.h"
#endif
#endif
#include <stdatomic.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <float.h>
#include "../common/aznative_sync.h"
#include "../common/aznative_c.h"
#include <time.h>
#if defined(__linux__)
#include <link.h>
#endif

#ifdef AZSDK_LOAD_TESTING
__attribute__((weak)) void azsdk_cue_commands_notify(unsigned deck __attribute__((unused))) {}
__attribute__((weak)) void azsdk_exit_load_changed(unsigned deck __attribute__((unused))) {}
#endif


enum {
    kDecks = 4,
    kRead = 4,
    kExecute = 1
};
const uintptr_t kUnitBundle = 0x190, kUnloadThis = 8;
const intptr_t kCompleteThis = -0x150;
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(unsigned) == 4, "native scalar ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
               "bounded native observations require lock-free scalars");
struct Range {
    uintptr_t begin, end;
    unsigned flags;
};
static struct Range image[32];
static unsigned image_count;
static uintptr_t image_base;
static pthread_mutex_t management = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(unsigned) enabled;
static _Atomic(azsdk_load_patch_fn) patcher;
static atomic_flag setup = ATOMIC_FLAG_INIT;
static unsigned installed_build;
/* Immutable first-empty reservations: aligned listener identity | (deck+1).
 * A failed strong CAS must inspect that same entry before proceeding, so two
 * contenders for one identity cannot reserve different entries. Never cleared. */
static _Atomic(uintptr_t) association_history[16] = {0};
/* Exact ELF virtual addresses, compared only against owned vtable entries.
 * No direct absolute native call is made. */
static const uintptr_t native_entries[2][4] = {
    {0x13b9790, 0x13b9720, 0x13b9a38, 0xa35690},
    {0x1474e10, 0x1474da0, 0x14750c0, 0xa7b4a0}};
static uint64_t next_unit;
static _Atomic(uint64_t) next_load;
/* Parent installer reads these prepublished originals; hooks read atomically.
 * They are never reset, including after a declined or partial installation. */
static void *original_load, *original_unload, *original_secondary_unload, *original_complete;
static _Atomic(unsigned) load_installed, unload_installed, secondary_installed, complete_installed;
struct Deck {
    atomic_flag capture;
    _Atomic(uintptr_t) unit, facade, listener;
    _Atomic(uint64_t) unit_generation, epoch, history;
    _Atomic(uint64_t) pending_epoch, pending_unit, pending_id;
#ifdef AZ_STATUS_NATIVE_PRODUCER
    _Atomic(uint64_t) awaiting_empty, pending_source_pin, sample_source_pin;
    _Atomic(uint64_t) confirmed_empty;
    _Atomic(uintptr_t) pending_wherefrom;
    unsigned char pending_track[12]; /* capture flag owns pending writes/consumption */
    _Atomic(uint64_t) pending_track_lo;
    _Atomic(uint32_t) pending_track_hi;
    double pending_original_bpm;
    unsigned pending_bpm_state;
    _Atomic(uint64_t) sample_original_bpm;
    _Atomic(unsigned) sample_bpm_state;
    _Atomic(uint64_t) sample_track_lo;
    _Atomic(uint32_t) sample_track_hi;
    _Atomic(unsigned) sample_source_kind;
#endif
    _Atomic(unsigned) ready, poison, valid;
    _Atomic(uint64_t) sequence, sample_unit, sample_load, sample_epoch, observed, sample_id;
};
#define DECK_INITIALIZER {.capture = ATOMIC_FLAG_INIT, .epoch = 1}
static struct Deck decks[kDecks] = {DECK_INITIALIZER, DECK_INITIALIZER, DECK_INITIALIZER, DECK_INITIALIZER};
#ifdef AZSDK_LOAD_TESTING
static void (*before_publish)(int);
static void (*before_association)(int);
static void (*configure_hook)(int, int);
static uintptr_t fixture_profile[4];
static unsigned fixture_build;
#endif
static bool deck_ok(int deck) { return deck >= 0 && (unsigned) (deck) < kDecks; }
static bool canonical(uintptr_t p) { return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u); }
static bool contains(uintptr_t p, size_t n, unsigned flags) {
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
    image_base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type h = &(info->dlpi_phdr[i]);
        if (h->p_type != PT_LOAD || !(h->p_flags & PF_R)) continue;
        uintptr_t base = info->dlpi_addr;
        if (image_count == sizeof image / sizeof *image || h->p_vaddr > UINTPTR_MAX - base || h->p_memsz > UINTPTR_MAX - base - h->p_vaddr) {
            image_count = 0;
            return 1;
        }
        image[image_count++] = (struct Range) {base + h->p_vaddr, base + h->p_vaddr + h->p_memsz, h->p_flags};
    }
    return 1;
}
#endif
/* Only the caller's genuine owned object may be read. Mapped RTTI cannot make a
 * stale object pointer safe. Native names stop at first mismatch/NUL. */
static uintptr_t nominal(uintptr_t object, const char *expected, intptr_t top, unsigned slots) {
    if (!canonical(object)) return 0;
    uintptr_t table = word(object);
    if (!canonical(table) || table < 16 || !contains(table - 16, (slots + 2u) * 8u, kRead) ||
        ((intptr_t) (word(table - 16))) != top) return 0;
    uintptr_t type = word(table - 8);
    if (!canonical(type) || !contains(type, 16, kRead)) return 0;
    uintptr_t name = word(type + 8);
    if (!contains(name, 1, kRead)) return 0;
    if (*((const char *) (name)) == '*') ++name;
    size_t bytes = strlen(expected) + 1;
    if (!contains(name, bytes, kRead)) return 0;
    const volatile char *actual = ((const volatile char *) (name));
    for (size_t i = 0; i < bytes; ++i)
        if (actual[i] != expected[i]) return 0;
    return table;
}
static bool native_id(const void *borrowed, uint32_t *out) {
    uintptr_t p = ((uintptr_t) (borrowed));
    if (!nominal(p, "N9dj_player13LoadRequestIDE", 0, 2)) return false;
    *out = __atomic_load_n(((const uint32_t *) (p + 8)), __ATOMIC_ACQUIRE);
    return true;
}
static uint64_t now() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    uint64_t s = ((uint64_t) (t.tv_sec));
    if (s > (UINT64_MAX - ((unsigned long) (t.tv_nsec))) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + ((unsigned long) (t.tv_nsec));
}
static uint64_t invalidate(struct Deck *d) {
    atomic_store_explicit(&d->valid, 0, memory_order_release);
    azsdk_exit_load_changed((unsigned) (d - decks));
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_store_explicit(&(d->pending_wherefrom), 0, memory_order_release);
    atomic_store_explicit(&(d->pending_source_pin), 0, memory_order_release);
#endif
    uint64_t e = atomic_load_explicit(&(d->epoch), memory_order_acquire);
    for (unsigned n = 0; n < 8; ++n) {
        if (e == UINT64_MAX) break;
        if (atomic_compare_exchange_strong_explicit(&(d->epoch), &(e), e + 1, memory_order_acq_rel, memory_order_acquire)) return e + 1;
    }
    atomic_store_explicit(&(d->poison), 1, memory_order_release); /* never reuse an invalidation epoch */
    return 0;
}
static bool fresh_id(struct Deck *d, uint32_t id) {
    uint64_t value = (uint64_t) (id) + 1, previous = atomic_load_explicit(&(d->history), memory_order_acquire);
    for (unsigned n = 0; n < 8; ++n) {
        if (value <= previous) return false;
        if (atomic_compare_exchange_strong_explicit(&(d->history), &(previous), value, memory_order_acq_rel, memory_order_acquire)) return true;
    }
    atomic_store_explicit(&(d->poison), 1, memory_order_release); /* an unrecorded ID cannot later be reused */
    return false;
}
static uint64_t load_generation() {
    uint64_t value = atomic_load_explicit(&(next_load), memory_order_relaxed);
    for (unsigned n = 0; n < 8; ++n) {
        if (value == UINT64_MAX) return 0;
        if (atomic_compare_exchange_strong_explicit(&(next_load), &(value), value + 1, memory_order_relaxed, memory_order_relaxed)) return value + 1;
    }
    return 0;
}
static void unlock(struct Deck *d) { atomic_flag_clear_explicit(&(d->capture), memory_order_release); }
typedef void (*Load)(void *, const void *, const void *, const void *, const void *, void *);
typedef void (*Unload)(void *, const void *, void *);
typedef void (*Complete)(void *, const void *, uint32_t, const void *);
static void load_hook(void *, const void *, const void *, const void *, const void *, void *);
static void unload_hook(void *, const void *, void *);
static void secondary_unload_hook(void *, const void *, void *);
static void complete_hook(void *, const void *, uint32_t, const void *);
static uintptr_t verified_entry(unsigned kind) {
    if (!installed_build || installed_build > 2 || kind >= 4) return 0;
#ifdef AZSDK_LOAD_TESTING
    if (fixture_build == installed_build) return fixture_profile[kind];
#endif
    uintptr_t value = native_entries[installed_build - 1][kind];
    return value > UINTPTR_MAX - image_base ? 0 : image_base + value;
}
struct patch_call {
    azsdk_load_patch_fn patch;
    uintptr_t slot;
    void *hook;
    void **original;
    int result;
};
static void invoke_patch(void *opaque) {
    struct patch_call *call = opaque;
    call->result = call->patch(call->slot, call->hook, call->original);
}
static bool install(uintptr_t slot, void *hook, void **original, _Atomic(unsigned) *acknowledged, unsigned kind) {
    void *current = __atomic_load_n(((void **) (slot)), __ATOMIC_ACQUIRE);
    void *expected = __atomic_load_n(original, __ATOMIC_ACQUIRE);
    void *verified = ((void *) (verified_entry(kind)));
    if (!verified || !contains(((uintptr_t) (verified)), 4, kExecute)) return false;
    if ((current != hook && current != verified) || (expected && expected != verified)) return false;
    if (current == hook) return atomic_load_explicit(acknowledged, memory_order_acquire) && expected && contains(((uintptr_t) (expected)), 4, kExecute);
    if (!contains(((uintptr_t) (current)), 4, kExecute) || (expected && expected != current)) return false;
    if (!expected) {
        expected = current;
        __atomic_store_n(original, expected, __ATOMIC_RELEASE);
    }
    __auto_type patch = atomic_load_explicit(&(patcher), memory_order_acquire);
    if (!patch) return false;
    int rc = 0;
    struct patch_call call = {patch, slot, hook, original, 0};
    invoke_patch(&call);
    rc = call.result;
    bool ok = rc == 1 && __atomic_load_n(original, __ATOMIC_ACQUIRE) == expected &&
              __atomic_load_n(((void **) (slot)), __ATOMIC_ACQUIRE) == hook;
    if (ok) atomic_store_explicit(acknowledged, 1, memory_order_release);
    return ok;
}
static bool completion_ready(uintptr_t listener) {
    uintptr_t table = nominal(listener, "N7usecase4load14LoadControllerE", kCompleteThis, 3);
    if (!table) return false;
    void *hook = ((void *) (complete_hook));
    if (__atomic_load_n(((void **) (table + 16)), __ATOMIC_ACQUIRE) == hook)
        return atomic_load_explicit(&(complete_installed), memory_order_acquire) && __atomic_load_n(&original_complete, __ATOMIC_ACQUIRE) != NULL;
    if (atomic_flag_test_and_set_explicit(&(setup), memory_order_acquire)) return false;
    bool ok = install(table + 16, hook, &original_complete, &complete_installed, 3);
    atomic_flag_clear_explicit(&(setup), memory_order_release);
    return ok;
}
static int facade_deck(uintptr_t facade) {
    for (unsigned n = 0; n < kDecks; ++n)
        if (atomic_load_explicit(&(decks[n].facade), memory_order_acquire) == facade && atomic_load_explicit(&(decks[n].unit_generation), memory_order_acquire)) return ((int) (n));
    return -1;
}
static bool reserve_listener(unsigned deck, uintptr_t listener) {
    for (unsigned i = 0; i < sizeof association_history / sizeof *association_history; ++i) {
        uintptr_t value = atomic_load_explicit(&(association_history[i]), memory_order_acquire);
        if (!value && atomic_compare_exchange_strong_explicit(&(association_history[i]), &(value), listener | (deck + 1u), memory_order_acq_rel, memory_order_acquire)) {
#ifdef AZSDK_LOAD_TESTING
            if (before_association) before_association(deck);
#endif
            return true;
        }
        if ((value & ~(uintptr_t) (7)) != listener) continue;
        unsigned owner = (unsigned) (value & 7u) - 1u;
        if (owner == deck) {
#ifdef AZSDK_LOAD_TESTING
            if (before_association) before_association(deck);
#endif
            return true;
        }
        if (owner < kDecks) {
            atomic_store_explicit(&(decks[owner].poison), 1, memory_order_release);
            invalidate(&decks[owner]);
        }
        atomic_store_explicit(&(decks[deck].poison), 1, memory_order_release);
        invalidate(&decks[deck]);
        return false;
    }
    atomic_store_explicit(&(decks[deck].poison), 1, memory_order_release);
    invalidate(&decks[deck]);
    return false;
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
static _Atomic(azn_source_capture_fn) source_capture;
static uintptr_t contained_wherefrom(const void *borrowed) {
    uintptr_t ref = ((uintptr_t) (borrowed));
    return canonical(ref) ? word(ref) : 0;
}
void status_load(unsigned deck, uint64_t unit, uint64_t epoch, int state,
                 uint64_t attempt, bool known_id, uint64_t load_generation, uint64_t source_pin,
                 unsigned source_kind, double bpm, unsigned bpm_state) {
    if (!unit || !epoch) return;
    struct azstatus_native_receipt r = {0};
    r.deck = deck;
    r.kind = AZN_LOAD;
    r.unit = unit;
    r.epoch = epoch;
    r.a = state;
    r.attempt = attempt;
    r.valid = known_id;
    r.load_generation = load_generation;
    r.source_pin = source_pin;
    r.source_kind = source_kind;
    r.original_bpm = bpm;
    r.original_bpm_state = bpm_state;
    azn_emit(&r);
}
#endif
static void begin_load(uintptr_t facade, const void *option, void *listener, const void *track, const void *where) {
    (void) track;
    (void) where;
    if (!atomic_load_explicit(&(enabled), memory_order_acquire)) return;
    int deck = facade_deck(facade);
    if (deck < 0) return;
    struct Deck *d = &(decks[deck]);
    uint64_t epoch = invalidate(d), unit = atomic_load_explicit(&(d->unit_generation), memory_order_acquire);
    uint32_t id;
    if (!epoch || !unit || atomic_load_explicit(&(d->poison), memory_order_acquire)) return;
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_store_explicit(&(d->awaiting_empty), 0, memory_order_release);
#ifdef AZ_STATUS_MEMORY_PRODUCER
    azn_memory_retire(deck, epoch);
#endif
#endif
    if (!native_id(option, &id)) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
        status_load(deck, unit, epoch, AZN_UNKNOWN, 0, false, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
#endif
        atomic_store_explicit(&(d->poison), 1, memory_order_release);
        return;
    }
#ifdef AZ_STATUS_NATIVE_PRODUCER
    status_load(deck, unit, epoch, AZN_LOADING, id, true, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
#endif
    if (!fresh_id(d, id) ||
        !atomic_load_explicit(&(d->ready), memory_order_acquire) || !completion_ready(((uintptr_t) (listener)))) return;
    if (!reserve_listener((unsigned) (deck), ((uintptr_t) (listener))) ||
        atomic_load_explicit(&(d->poison), memory_order_acquire) || atomic_flag_test_and_set_explicit(&(d->capture), memory_order_acquire)) return;
    if (atomic_load_explicit(&(enabled), memory_order_acquire) && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
        atomic_load_explicit(&(d->unit_generation), memory_order_acquire) == unit && !atomic_load_explicit(&(d->poison), memory_order_acquire)) {
        atomic_store_explicit(&(d->listener), ((uintptr_t) (listener)), memory_order_release);
#ifdef AZ_STATUS_NATIVE_PRODUCER
        uint64_t pin = 0;
        uintptr_t object = 0;
        memset(d->pending_track, 0, sizeof d->pending_track);
        if (track) memcpy(d->pending_track, track, sizeof d->pending_track);
        uint64_t pending_lo;
        uint32_t pending_hi;
        memcpy(&pending_lo, d->pending_track, 8);
        memcpy(&pending_hi, d->pending_track + 8, 4);
        atomic_store_explicit(&(d->pending_track_lo), pending_lo, memory_order_release);
        atomic_store_explicit(&(d->pending_track_hi), pending_hi, memory_order_release);
        /* Both admitted firmware loaders copy the complete 0x38-byte
         * TrackInfoToLoad into their queued task. Its +0x30 double is copied
         * unchanged into SourceInfo+0x60, unlike the position-dependent grid
         * BPM. Copy only while the original load argument is borrowed. */
        d->pending_original_bpm = 0;
        d->pending_bpm_state = AZN_BPM_UNAVAILABLE;
        if (track) {
            double bpm;
            memcpy(&bpm, ((const unsigned char *) (track)) + 0x30, sizeof bpm);
            if (bpm == DBL_MAX) d->pending_bpm_state = AZN_BPM_EMPTY;
            else if (isfinite(bpm) && bpm > 0) {
                d->pending_original_bpm = bpm;
                d->pending_bpm_state = AZN_BPM_READY;
            }
        }
        __auto_type capture_source = atomic_load_explicit(&(source_capture), memory_order_acquire);
        if (track && where) {
            object = contained_wherefrom(where);
            if (canonical(object)) {
                if (capture_source) pin = capture_source((unsigned) (deck), unit, epoch, object, d->pending_track);
            }
        }
        atomic_store_explicit(&(d->pending_source_pin), pin, memory_order_relaxed);
        atomic_store_explicit(&(d->pending_wherefrom), object, memory_order_relaxed);
#endif
        atomic_store_explicit(&(d->pending_unit), unit, memory_order_relaxed);
        atomic_store_explicit(&(d->pending_id), (uint64_t) (id) + 1, memory_order_relaxed);
        atomic_store_explicit(&(d->pending_epoch), epoch, memory_order_release);
    }
    unlock(d);
}
static void begin_unload(uintptr_t facade, const void *option) {
    if (!atomic_load_explicit(&(enabled), memory_order_acquire)) return;
    int deck = facade_deck(facade);
    if (deck < 0) return;
    struct Deck *d = &(decks[deck]);
    __auto_type epoch = invalidate(d);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_store_explicit(&(d->awaiting_empty), epoch, memory_order_release);
#ifdef AZ_STATUS_MEMORY_PRODUCER
    azn_memory_retire(deck, epoch);
#endif
    // Entry invalidates the previous load. It is not an unload completion.
    status_load(deck, atomic_load_explicit(&(d->unit_generation), memory_order_acquire), epoch, AZN_UNKNOWN, 0, false, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
#else
    (void) epoch;
#endif
    uint32_t id;
    if (native_id(option, &id)) (void) fresh_id(d, id);
    else atomic_store_explicit(&(d->poison), 1, memory_order_release);
    if (atomic_flag_test_and_set_explicit(&(d->capture), memory_order_acquire)) return;
    atomic_store_explicit(&(d->pending_epoch), 0, memory_order_release);
    atomic_store_explicit(&(d->pending_id), 0, memory_order_release);
    unlock(d);
}
static void complete(uintptr_t listener, const void *request, uint32_t result, const void *where) {
    (void) where;
    if (!atomic_load_explicit(&(enabled), memory_order_acquire) || !nominal(listener, "N7usecase4load14LoadControllerE", kCompleteThis, 3)) return;
    uint32_t id;
    if (!native_id(request, &id)) return;
    for (unsigned deck = 0; deck < kDecks; ++deck) {
        struct Deck *d = &(decks[deck]);
        uint64_t epoch = atomic_load_explicit(&(d->epoch), memory_order_acquire), unit = atomic_load_explicit(&(d->unit_generation), memory_order_acquire);
        if (!unit || atomic_load_explicit(&(d->listener), memory_order_acquire) != listener || atomic_load_explicit(&(d->pending_epoch), memory_order_acquire) != epoch ||
            atomic_load_explicit(&(d->pending_unit), memory_order_acquire) != unit || atomic_load_explicit(&(d->pending_id), memory_order_acquire) != (uint64_t) (id) + 1) continue;
        if (atomic_flag_test_and_set_explicit(&(d->capture), memory_order_acquire)) {
            invalidate(d);
            return;
        }
        if (atomic_load_explicit(&(enabled), memory_order_acquire) && !atomic_load_explicit(&(d->poison), memory_order_acquire) &&
            atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch && atomic_load_explicit(&(d->unit_generation), memory_order_acquire) == unit &&
            atomic_load_explicit(&(d->listener), memory_order_acquire) == listener && atomic_load_explicit(&(d->pending_epoch), memory_order_acquire) == epoch &&
            atomic_load_explicit(&(d->pending_id), memory_order_acquire) == (uint64_t) (id) + 1) {
            atomic_store_explicit(&(d->pending_id), 0, memory_order_release);
            uint64_t generation = result == 0 ? load_generation() : 0, observed = generation ? now() : 0;
#ifdef AZ_STATUS_NATIVE_PRODUCER
            if (result) status_load(deck, unit, epoch, AZN_FAILED, id, true, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
#endif
            uint64_t sequence = atomic_load_explicit(&(d->sequence), memory_order_relaxed);
#ifdef AZSDK_LOAD_TESTING
            if (before_publish) before_publish(((int) (deck)));
#endif
            if (generation && observed && sequence <= UINT64_MAX - 2 && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
                atomic_load_explicit(&(d->unit_generation), memory_order_acquire) == unit) {
                atomic_store_explicit(&(d->sequence), sequence + 1, memory_order_release);
                /* Acquiring any field of a newer write must also observe its
                 * preceding odd sequence at the final sequence check. */
#ifdef AZ_STATUS_NATIVE_PRODUCER
                uint64_t pin = atomic_exchange_explicit(&(d->pending_source_pin), 0, memory_order_relaxed);
                uintptr_t origin = atomic_exchange_explicit(&(d->pending_wherefrom), 0, memory_order_relaxed);
                if (!origin || !where || contained_wherefrom(where) != origin) {
                    pin = 0;
                    origin = 0;
                }
                unsigned kind = origin ? (unsigned) (azn_source_kind((int) (installed_build), d->pending_track)) : 0;
                atomic_store_explicit(&(d->sample_source_kind), kind, memory_order_release);
                atomic_store_explicit(&(d->sample_source_pin), pin, memory_order_release);
                unsigned bpm_state = origin ? d->pending_bpm_state : (unsigned) (AZN_BPM_UNAVAILABLE);
                double bpm = bpm_state == AZN_BPM_READY ? d->pending_original_bpm : 0;
                uint64_t bpm_bits;
                memcpy(&bpm_bits, &bpm, sizeof bpm_bits);
                atomic_store_explicit(&(d->sample_original_bpm), bpm_bits, memory_order_release);
                atomic_store_explicit(&(d->sample_bpm_state), bpm_state, memory_order_release);
                uint64_t lo;
                uint32_t hi;
                memcpy(&lo, d->pending_track, 8);
                memcpy(&hi, d->pending_track + 8, 4);
                atomic_store_explicit(&(d->sample_track_lo), lo, memory_order_release);
                atomic_store_explicit(&(d->sample_track_hi), hi, memory_order_release);
#endif
                atomic_store_explicit(&(d->sample_unit), unit, memory_order_release);
                atomic_store_explicit(&(d->sample_load), generation, memory_order_release);
                atomic_store_explicit(&(d->sample_epoch), epoch, memory_order_release);
                atomic_store_explicit(&(d->sample_id), id, memory_order_release);
                atomic_store_explicit(&(d->observed), observed, memory_order_release);
                atomic_store_explicit(&(d->sequence), sequence + 2, memory_order_release);
#ifdef AZ_STATUS_NATIVE_PRODUCER
                // Reserve the load barrier before this matched generation is visible to owner writers.
                if (atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch && atomic_load_explicit(&(d->unit_generation), memory_order_acquire) == unit)
                    status_load(deck, unit, epoch, AZN_LOADED, id, true, generation, pin, kind, bpm, bpm_state);
#endif
                atomic_store_explicit(&(d->valid), 1, memory_order_release);
                if (atomic_load_explicit(&(d->epoch), memory_order_acquire) != epoch || atomic_load_explicit(&(d->unit_generation), memory_order_acquire) != unit)
                    atomic_store_explicit(&(d->valid), 0, memory_order_release);
#ifdef AZ_STATUS_MEMORY_PRODUCER
                else if (origin) azn_memory_bind(deck, unit, epoch, generation, origin, d->pending_track);
#endif
            }
        }
        unlock(d);
        azsdk_cue_commands_notify(deck);
        return;
    }
}
static void load_hook(void *self, const void *track, const void *option, const void *where, const void *speed, void *listener) {
    begin_load(((uintptr_t) (self)), option, listener, track, where);
    __auto_type original = ((Load) (__atomic_load_n(&original_load, __ATOMIC_ACQUIRE)));
    if (original) original(self, track, option, where, speed, listener);
}
static void unload_hook(void *self, const void *option, void *listener) {
    begin_unload(((uintptr_t) (self)), option);
    __auto_type original = ((Unload) (__atomic_load_n(&original_unload, __ATOMIC_ACQUIRE)));
    if (original) original(self, option, listener);
}
static void secondary_unload_hook(void *self, const void *option, void *listener) {
    uintptr_t receiver = ((uintptr_t) (self));
    if (receiver >= kUnloadThis) begin_unload(receiver - kUnloadThis, option);
    __auto_type original = ((Unload) (__atomic_load_n(&original_secondary_unload, __ATOMIC_ACQUIRE)));
    if (original) original(self, option, listener);
}
static void complete_hook(void *self, const void *request, uint32_t result, const void *where) {
    complete(((uintptr_t) (self)), request, result, where);
    __auto_type original = ((Complete) (__atomic_load_n(&original_complete, __ATOMIC_ACQUIRE)));
    if (original) original(self, request, result, where);
}


#ifdef AZ_STATUS_NATIVE_PRODUCER
int azn_register_source_capture(azn_source_capture_fn callback) {
    if (!callback) return 0;
    azn_source_capture_fn empty = NULL;
    return atomic_compare_exchange_strong_explicit(&(source_capture), &(empty), callback, memory_order_acq_rel, memory_order_acquire) || empty == callback;
}
uint64_t azn_load_source_pin(unsigned deck, uint64_t epoch, uint64_t load) {
    if (deck >= kDecks) return 0;
    struct Deck *d = &(decks[deck]);
    uint64_t seq = atomic_load_explicit(&(d->sequence), memory_order_acquire);
    if ((seq & 1u) || atomic_load_explicit(&(d->sample_epoch), memory_order_acquire) != epoch ||
        atomic_load_explicit(&(d->sample_load), memory_order_acquire) != load) return 0;
    uint64_t pin = atomic_load_explicit(&(d->sample_source_pin), memory_order_acquire);
    return seq == atomic_load_explicit(&(d->sequence), memory_order_acquire) && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
                   atomic_load_explicit(&(d->valid), memory_order_acquire)
               ? pin
               : 0;
}
/* A scalar-only pre-completion witness. Never take capture here: completion
 * intentionally invalidates a contended writer, and a reader must not cause it.
 * Each new request has a new epoch; acquiring any newly copied field carries
 * that invalidation to the final epoch check. Completion clears pending_id
 * before publishing its matched load, including on failure. */
uint64_t azn_load_pending(unsigned deck, uint64_t epoch, const unsigned char source[12]) {
    if (deck >= kDecks || !epoch || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    __auto_type d = &(decks[deck]);
    uint64_t request = atomic_load_explicit(&(d->pending_id), memory_order_acquire);
    uint64_t unit = atomic_load_explicit(&(d->pending_unit), memory_order_acquire);
    if (!request || !unit || atomic_load_explicit(&(d->pending_epoch), memory_order_acquire) != epoch) return 0;
    if (source) {
        uint64_t lo;
        uint32_t hi;
        memcpy(&lo, source, 8);
        memcpy(&hi, source + 8, 4);
        if (!hi || atomic_load_explicit(&(d->pending_track_lo), memory_order_acquire) != lo ||
            atomic_load_explicit(&(d->pending_track_hi), memory_order_acquire) != hi) return 0;
    }
    return atomic_load_explicit(&(d->pending_id), memory_order_acquire) == request &&
                   atomic_load_explicit(&(d->pending_epoch), memory_order_acquire) == epoch &&
                   atomic_load_explicit(&(d->unit_generation), memory_order_acquire) == unit &&
                   !atomic_load_explicit(&(d->poison), memory_order_acquire) && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
                   atomic_load_explicit(&(enabled), memory_order_acquire)
               ? request
               : 0;
}
int azn_load_track_matches(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char source[12]) {
    if (deck >= kDecks || !source || !epoch || !load) return 0;
    __auto_type d = &(decks[deck]);
    uint64_t seq = atomic_load_explicit(&(d->sequence), memory_order_acquire);
    if (!seq || (seq & 1)) return 0;
    uint64_t lo;
    uint32_t hi;
    memcpy(&lo, source, 8);
    memcpy(&hi, source + 8, 4);
    return hi && atomic_load_explicit(&(d->sample_track_lo), memory_order_acquire) == lo && atomic_load_explicit(&(d->sample_track_hi), memory_order_acquire) == hi &&
           atomic_load_explicit(&(d->sample_load), memory_order_acquire) == load && atomic_load_explicit(&(d->sample_epoch), memory_order_acquire) == epoch &&
           atomic_load_explicit(&(d->valid), memory_order_acquire) && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
           !atomic_load_explicit(&(d->poison), memory_order_acquire) && atomic_load_explicit(&(d->sequence), memory_order_acquire) == seq;
}
unsigned azn_load_kind(unsigned deck, uint64_t epoch, uint64_t load) {
    if (deck >= kDecks || !epoch || !load) return 0;
    __auto_type d = &(decks[deck]);
    uint64_t seq = atomic_load_explicit(&(d->sequence), memory_order_acquire);
    unsigned kind = atomic_load_explicit(&(d->sample_source_kind), memory_order_acquire);
    return seq && !(seq & 1) && atomic_load_explicit(&(d->sample_load), memory_order_acquire) == load &&
                   atomic_load_explicit(&(d->sample_epoch), memory_order_acquire) == epoch && atomic_load_explicit(&(d->epoch), memory_order_acquire) == epoch &&
                   atomic_load_explicit(&(d->valid), memory_order_acquire) && atomic_load_explicit(&(d->sequence), memory_order_acquire) == seq
               ? kind
               : 0;
}
unsigned azn_load_original_bpm(unsigned deck, uint64_t unit, uint64_t epoch, uint64_t load, double *out) {
    if (deck >= kDecks || !out || !unit || !epoch || !load) return AZN_BPM_UNKNOWN;
    __auto_type d = &(decks[deck]);
    uint64_t seq = atomic_load_explicit(&(d->sequence), memory_order_acquire);
    uint64_t bits = atomic_load_explicit(&(d->sample_original_bpm), memory_order_acquire);
    unsigned state = atomic_load_explicit(&(d->sample_bpm_state), memory_order_acquire);
    if (!seq || (seq & 1) || atomic_load_explicit(&(d->sample_load), memory_order_acquire) != load ||
        atomic_load_explicit(&(d->sample_epoch), memory_order_acquire) != epoch || atomic_load_explicit(&(d->sample_unit), memory_order_acquire) != unit ||
        atomic_load_explicit(&(d->epoch), memory_order_acquire) != epoch || atomic_load_explicit(&(d->unit_generation), memory_order_acquire) != unit ||
        !atomic_load_explicit(&(d->valid), memory_order_acquire) || atomic_load_explicit(&(d->poison), memory_order_acquire) ||
        !atomic_load_explicit(&(enabled), memory_order_acquire) || atomic_load_explicit(&(d->sequence), memory_order_acquire) != seq) return AZN_BPM_UNKNOWN;
    memcpy(out, &bits, sizeof bits);
    return state;
}
uint64_t azn_lifecycle_fence(unsigned deck) {
    return deck < kDecks && atomic_load_explicit(&(decks[deck].unit_generation), memory_order_acquire)
               ? atomic_load_explicit(&(decks[deck].epoch), memory_order_acquire)
               : 0;
}
int azn_load_history_revoked(unsigned deck, uint64_t epoch) {
    if (deck >= kDecks || !epoch) return 0;
    __auto_type d = &(decks[deck]);
    __auto_type current = atomic_load_explicit(&(d->epoch), memory_order_acquire);
    /* An invalidated incarnation with no active replacement has no playback
     * history to capture. A failed load_read alone does not prove this. */
    return current > epoch && !atomic_load_explicit(&(d->valid), memory_order_acquire) &&
           atomic_load_explicit(&(d->epoch), memory_order_acquire) == current;
}
uint64_t azn_empty_fence(unsigned deck) {
    if (deck >= kDecks) return 0;
    __auto_type d = &(decks[deck]);
    uint64_t f = atomic_load_explicit(&(d->awaiting_empty), memory_order_acquire);
    return f && atomic_load_explicit(&(d->epoch), memory_order_acquire) == f ? f : 0;
}
void azn_empty_commit(unsigned deck, uint64_t fence) {
    if (!fence || deck >= kDecks || azn_empty_fence(deck) != fence) return;
    __auto_type d = &(decks[deck]);
    __auto_type unit = atomic_load_explicit(&(d->unit_generation), memory_order_acquire);
    uint64_t old = atomic_load_explicit(&(d->confirmed_empty), memory_order_acquire);
    if (old < fence && atomic_compare_exchange_strong_explicit(&(d->confirmed_empty), &(old), fence, memory_order_acq_rel, memory_order_acquire) &&
        azn_empty_fence(deck) == fence) status_load(deck, unit, fence, AZN_EMPTY, 0, false, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
}
int azn_empty_seed(unsigned deck, struct azstatus_native_receipt *out) {
    uint64_t fence = azn_empty_fence(deck);
    if (!fence || !out || atomic_load_explicit(&(decks[deck].confirmed_empty), memory_order_acquire) != fence) return 0;
    __auto_type unit = atomic_load_explicit(&(decks[deck].unit_generation), memory_order_acquire);
    if (!unit || azn_empty_fence(deck) != fence) return 0;
    *out = (struct azstatus_native_receipt) {0};
    out->size = sizeof *out;
    out->version = 1;
    out->kind = AZN_LOAD;
    out->a = AZN_EMPTY;
    out->deck = deck;
    out->unit = unit;
    out->epoch = fence;
    return 1;
}
#endif
void azsdk_load_configure(int on, int build, azsdk_load_patch_fn patch) {
    atomic_store_explicit(&(enabled), 0, memory_order_release);
    for (unsigned d = 0; d < kDecks; ++d) {
        invalidate(&decks[d]);
        atomic_store_explicit(&(decks[d].unit_generation), 0, memory_order_release);
    }
#ifdef AZSDK_LOAD_TESTING
    if (configure_hook) configure_hook(0, on);
#endif
    {
        AZ_MUTEX_GUARD(lock, management, false);
        if (lock.error) return;
        atomic_store_explicit(&(enabled), 0, memory_order_release);
        for (unsigned d = 0; d < kDecks; ++d) {
            invalidate(&decks[d]);
            atomic_store_explicit(&(decks[d].unit_generation), 0, memory_order_release);
            atomic_store_explicit(&(decks[d].facade), 0, memory_order_release);
            atomic_store_explicit(&(decks[d].unit), 0, memory_order_release);
            atomic_store_explicit(&(decks[d].ready), 0, memory_order_release);
        }
#if defined(__linux__)
        if (!on || !patch || (build != 1 && build != 2) || (installed_build && installed_build != (unsigned) (build))) return;
        if (!image_count) dl_iterate_phdr(main_image, NULL);
        if (!image_count) return;
        if (!installed_build) installed_build = (unsigned) (build); /* immutable before first enabled publication */
        atomic_store_explicit(&(patcher), patch, memory_order_release);
#ifdef AZSDK_LOAD_TESTING
        if (configure_hook) configure_hook(1, on);
#endif
        atomic_store_explicit(&(enabled), 1, memory_order_release);
#else
        (void) on;
        (void) build;
        (void) patch;
        (void) installed_build;
#endif
    }
}
int azsdk_load_register(int deck, void *owned_unit) {
    if (!deck_ok(deck) || !atomic_load_explicit(&(enabled), memory_order_acquire) || !owned_unit) return 0;
    {
        AZ_MUTEX_GUARD(lock, management, false);
        if (lock.error) return 0;
        struct Deck *d = &(decks[deck]);
        if (!atomic_load_explicit(&(enabled), memory_order_acquire) || atomic_load_explicit(&(d->unit_generation), memory_order_acquire) || atomic_load_explicit(&(d->poison), memory_order_acquire)) return 0;
        uintptr_t unit = ((uintptr_t) (owned_unit));
        if (!nominal(unit, "N9dj_player12DjPlayerUnitE", 0, 2)) return 0;
        uintptr_t bundle = word(unit + kUnitBundle);
        if (!canonical(bundle)) return 0;
        uintptr_t facade = word(bundle), primary = nominal(facade, "N9dj_player15TrackLoadFacadeE", 0, 4);
        if (!primary) return 0;
        for (unsigned n = 0; n < kDecks; ++n)
            if (n != (unsigned) (deck) && atomic_load_explicit(&(decks[n].unit_generation), memory_order_acquire) &&
                (atomic_load_explicit(&(decks[n].unit), memory_order_acquire) == unit || atomic_load_explicit(&(decks[n].facade), memory_order_acquire) == facade)) return 0;
        uintptr_t secondary = nominal(facade + kUnloadThis, "N9dj_player15TrackLoadFacadeE", -8, 3);
        if (!secondary || next_unit == UINT64_MAX || atomic_flag_test_and_set_explicit(&(setup), memory_order_acquire)) return 0;
        bool ok = install(primary + 16, ((void *) (load_hook)), &original_load, &load_installed, 0) &&
                  install(primary + 24, ((void *) (unload_hook)), &original_unload, &unload_installed, 1) &&
                  install(secondary + 16, ((void *) (secondary_unload_hook)), &original_secondary_unload, &secondary_installed, 2);
        atomic_flag_clear_explicit(&(setup), memory_order_release);
        if (!ok || atomic_flag_test_and_set_explicit(&(d->capture), memory_order_acquire)) return 0;
        uint64_t epoch = invalidate(d);
        if (!epoch || !atomic_load_explicit(&(enabled), memory_order_acquire)) {
            unlock(d);
            return 0;
        }
        atomic_store_explicit(&(d->listener), 0, memory_order_release);
        atomic_store_explicit(&(d->pending_epoch), 0, memory_order_release);
        atomic_store_explicit(&(d->pending_id), 0, memory_order_release);
        atomic_store_explicit(&(d->unit), unit, memory_order_release);
        atomic_store_explicit(&(d->facade), facade, memory_order_release);
        atomic_store_explicit(&(d->ready), 1, memory_order_release);
        atomic_store_explicit(&(d->unit_generation), ++next_unit, memory_order_release);
#ifdef AZ_STATUS_NATIVE_PRODUCER
        atomic_store_explicit(&(d->awaiting_empty), epoch, memory_order_release);
        azn_capability(deck, AZN_CAP_LOAD | AZN_CAP_ORIGINAL_BPM, 1);
#endif
        unlock(d);
        return 1;
    }
}
void azsdk_load_retire(int deck) {
    if (!deck_ok(deck)) return;
    struct Deck *d = &(decks[deck]);
    __auto_type epoch = invalidate(d);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    status_load(deck, atomic_load_explicit(&(d->unit_generation), memory_order_acquire), epoch, AZN_UNKNOWN, 0, false, 0, 0, 0, 0, AZN_BPM_UNKNOWN);
    azn_capability(deck, AZN_CAP_LOAD | AZN_CAP_ORIGINAL_BPM, 0);
#ifdef AZ_STATUS_MEMORY_PRODUCER
    azn_memory_retire(deck, epoch);
#endif
#else
    (void) epoch;
#endif
    atomic_store_explicit(&(d->unit_generation), 0, memory_order_release);
    atomic_store_explicit(&(d->facade), 0, memory_order_release);
    atomic_store_explicit(&(d->unit), 0, memory_order_release);
    atomic_store_explicit(&(d->ready), 0, memory_order_release);
}
int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    if (!deck_ok(deck) || !out || !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    struct Deck *d = &(decks[deck]);
    if (atomic_load_explicit(&(d->poison), memory_order_acquire) || !atomic_load_explicit(&(d->valid), memory_order_acquire)) return 0;
    uint64_t epoch = atomic_load_explicit(&(d->epoch), memory_order_acquire), sequence = atomic_load_explicit(&(d->sequence), memory_order_acquire);
    if (!sequence || (sequence & 1u)) return 0;
    struct azsdk_load_sample sample = {atomic_load_explicit(&(d->sample_unit), memory_order_acquire), atomic_load_explicit(&(d->sample_load), memory_order_acquire),
                                       atomic_load_explicit(&(d->sample_epoch), memory_order_acquire), atomic_load_explicit(&(d->observed), memory_order_acquire),
                                       ((uint32_t) (atomic_load_explicit(&(d->sample_id), memory_order_acquire))), 0};
    if (atomic_load_explicit(&(d->sequence), memory_order_acquire) != sequence || !atomic_load_explicit(&(d->valid), memory_order_acquire) ||
        atomic_load_explicit(&(d->poison), memory_order_acquire) || atomic_load_explicit(&(d->epoch), memory_order_acquire) != epoch || sample.invalidation_epoch != epoch ||
        atomic_load_explicit(&(d->unit_generation), memory_order_acquire) != sample.unit_generation || !sample.unit_generation || !sample.load_generation ||
        !atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    *out = sample;
    return 1;
}
#ifdef AZSDK_LOAD_TESTING
void azsdk_load_test_hold(int deck, int held) {
    if (!deck_ok(deck)) return;
    if (held) atomic_flag_test_and_set_explicit(&(decks[deck].capture), memory_order_acquire);
    else atomic_flag_clear_explicit(&(decks[deck].capture), memory_order_release);
}
void azsdk_load_test_before_publish(void (*hook)(int)) { before_publish = hook; }
void azsdk_load_test_before_association(void (*hook)(int)) { before_association = hook; }
void azsdk_load_test_configure_hook(void (*hook)(int, int)) { configure_hook = hook; }
void azsdk_load_test_profile(int build, void *load, void *unload, void *secondary, void *completion) {
    if (atomic_load_explicit(&(enabled), memory_order_acquire) || installed_build || (build != 1 && build != 2)) return;
    fixture_build = (unsigned) (build);
    fixture_profile[0] = ((uintptr_t) (load));
    fixture_profile[1] = ((uintptr_t) (unload));
    fixture_profile[2] = ((uintptr_t) (secondary));
    fixture_profile[3] = ((uintptr_t) (completion));
}
void azsdk_load_test_exhaust(int deck, int which) {
    if (!deck_ok(deck)) return;
    if (which == 1) atomic_store_explicit(&(decks[deck].epoch), UINT64_MAX, memory_order_release);
    else if (which == 2) atomic_store_explicit(&(next_load), UINT64_MAX, memory_order_release);
    else if (which == 3) atomic_store_explicit(&(decks[deck].sequence), UINT64_MAX - 1, memory_order_release);
    else if (which == 4) next_unit = UINT64_MAX;
}
#endif

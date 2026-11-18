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

#ifdef AZSDK_CUE_TESTING
__attribute__((weak)) void azsdk_cue_commands_collection_changed(unsigned deck __attribute__((unused))) {}
#endif


enum {
    decks = 4,
    slot_limit = 64,
    mask_bits = 9,
    cue_mask = 0x1ff
};
static const uint64_t max_generation = UINT64_MAX >> mask_bits;
static const unsigned read_flag = 4, execute_flag = 1;
static const uintptr_t unit_publisher = 0x1a0, publisher_server = 0x1f28;
static const uintptr_t cue_channel = 0x1508, channel_route = 0x48;
static const unsigned observer_slot = 8;
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(unsigned) == 4, "native cue scalar ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "native callbacks must use lock-free scalar atomics");
struct Range {
    uintptr_t begin, end;
    unsigned flags;
};
struct Pending {
    // Each slot is assigned once and never reused. Old strong callback leases
    // can OR only their old slot, even across retirement/replacement races.
    _Atomic(uint64_t) word;
};
struct Published {
    _Atomic(bool) collecting;
    _Atomic(uint64_t) sequence, generation, revision, observed;
    _Atomic(unsigned) mask;
#ifdef AZSDK_CUE_TESTING
    _Atomic(bool) clock_failure;
#endif
};
struct State {
    struct Range image[32];
    unsigned image_count;
    pthread_once_t image_once;
    pthread_mutex_t registration;
    _Atomic(unsigned) enabled;
    _Atomic(unsigned) active[decks], acknowledged[decks];
    unsigned bound_build, used;
    uint64_t next_generation;
    struct Pending pending[slot_limit];
    struct Published published[decks];
    struct az_native_owner owners[decks];
    unsigned owner_slot[decks];
#ifdef AZSDK_CUE_TESTING
    _Atomic(void (*)(int)) before_pending, after_collect;
#endif
};
/* Static C initialization is available before native constructor hooks and
 * remains valid throughout native teardown. Only core listener owners live here. */
static struct State storage = {.image_once = PTHREAD_ONCE_INIT, .registration = PTHREAD_MUTEX_INITIALIZER};
static struct State *state(void) { return &storage; }
static bool deck_ok(int deck) { return deck >= 0 && (unsigned) (deck) < decks; }
static bool canonical(uintptr_t p) {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
static uintptr_t scalar_word(uintptr_t p) {
    uintptr_t value;
    memcpy(&value, (const void *) p, sizeof value);
    return value;
}
static uint32_t scalar_u32(uintptr_t p) {
    uint32_t value;
    memcpy(&value, (const void *) p, sizeof value);
    return value;
}
static bool contains(uintptr_t p, size_t bytes, unsigned flags) {
    if (bytes > UINTPTR_MAX - p) return false;
    for (unsigned i = 0; i != state()->image_count; ++i) {
        __auto_type range = &(state()->image[i]);
        if ((range->flags & flags) == flags && p >= range->begin && p + bytes <= range->end) return true;
    }
    return false;
}
#if defined(__linux__)
static int main_image(struct dl_phdr_info *info, size_t size __attribute__((unused)), void *context __attribute__((unused))) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    struct State *s = state();
    for (unsigned i = 0; i != info->dlpi_phnum; ++i) {
        __auto_type header = &(info->dlpi_phdr[i]);
        if (header->p_type != PT_LOAD || !(header->p_flags & PF_R)) continue;
        const uintptr_t base = info->dlpi_addr;
        if (s->image_count == 32 || header->p_vaddr > UINTPTR_MAX - base ||
            header->p_memsz > UINTPTR_MAX - base - header->p_vaddr) {
            s->image_count = 0;
            return 1;
        }
        s->image[s->image_count++] = (struct Range) {base + header->p_vaddr, base + header->p_vaddr + header->p_memsz, header->p_flags};
    }
    return 1;
}
static void prepare_image() { dl_iterate_phdr(main_image, NULL); }
#endif
static uintptr_t checked_vtable(uintptr_t object, const char *name, unsigned slots) {
    if (!canonical(object)) return 0;
    __auto_type table = scalar_word(object);
    if (!canonical(table) || table < 16 || !contains(table - 16, (slots + 2u) * 8u, read_flag) ||
        scalar_word(table - 16)) return 0;
    __auto_type type = scalar_word(table - 8);
    if (!canonical(type) || !contains(type, 16, read_flag)) return 0;
    __auto_type text = scalar_word(type + 8);
    if (!contains(text, 1, read_flag)) return 0;
    if (*((const char *) (text)) == '*') ++text;
    for (size_t i = 0;; ++i) {
        if (text > UINTPTR_MAX - i || !contains(text + i, 1, read_flag)) return 0;
        __auto_type actual = *((const volatile char *) (text + i));
        if (actual != name[i]) return 0;
        if (!actual) return table;
    }
}
static void invalidate(unsigned deck) {
    atomic_store_explicit(&(state()->active[deck]), 0, memory_order_release);
    atomic_store_explicit(&(state()->acknowledged[deck]), 0, memory_order_release);
}
static void invalidate_slot(unsigned deck, unsigned slot) {
    struct State *s = state();
    __auto_type expected = slot;
    atomic_compare_exchange_strong_explicit(&(s->active[deck]), &(expected), 0, memory_order_acq_rel, memory_order_acquire);
    expected = slot;
    atomic_compare_exchange_strong_explicit(&(s->acknowledged[deck]), &(expected), 0, memory_order_acq_rel, memory_order_acquire);
}
struct CueListener {
    const void *const *vtable;
    unsigned deck, slot, route;
};
static void listener_destroy(void *opaque) { (void) opaque; }
static void callback2(void *opaque, const void *value) {
    (void) opaque;
    (void) value;
}
static void callback3(void *opaque, const void *value, unsigned route) {
    const struct CueListener *listener = opaque;
    struct State *s = state();
    if (route != listener->route || !atomic_load_explicit(&(s->enabled), memory_order_acquire) ||
        atomic_load_explicit(&(s->active[listener->deck]), memory_order_acquire) != listener->slot) return;
    uint32_t index;
    memcpy(&index, value, sizeof index);
    if (index >= mask_bits) return;
#ifdef AZSDK_CUE_TESTING
    __auto_type hook = atomic_load_explicit(&(s->before_pending), memory_order_acquire);
    if (hook) hook((int) (listener->deck));
#endif
        // One nonblocking RMW, no finite retry/lost wakeup. The immutable slot
        // belongs to this registration even if retirement races this store.
    atomic_fetch_or_explicit(&(s->pending[listener->slot - 1].word), (uint64_t) (1u << index), memory_order_release);
    azsdk_cue_commands_collection_changed(listener->deck);
}
static const void *const listener_vtable[] = {
    (void *) listener_destroy, (void *) listener_destroy, (void *) callback2, (void *) callback3};
static uint64_t collection_now(unsigned deck) {
#ifdef AZSDK_CUE_TESTING
    if (atomic_load_explicit(&(state()->published[deck].clock_failure), memory_order_acquire)) return 0;
#else
    (void) deck;
#endif
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return 0;
    __auto_type seconds = ((uint64_t) (value.tv_sec));
    if (seconds > (UINT64_MAX - ((unsigned long) (value.tv_nsec))) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + ((unsigned long) (value.tv_nsec));
}
static void collection_release(_Atomic(bool) **busy) {
    atomic_store_explicit(*busy, false, memory_order_release);
}

void azsdk_cue_configure(int on, int build) {
    struct State *s = state();
    atomic_store_explicit(&(s->enabled), 0, memory_order_release);
    for (unsigned d = 0; d != decks; ++d) invalidate(d);
    AZ_MUTEX_GUARD(lock, s->registration, false);
    if (lock.error) return;
    for (unsigned d = 0; d != decks; ++d) {
        invalidate(d);
        az_native_owner_reset(&s->owners[d]);
        s->owner_slot[d] = 0;
    }
#if defined(__linux__)
    if (!on || (build != 1 && build != 2) || (s->bound_build && s->bound_build != (unsigned) (build))) return;
    if (pthread_once(&s->image_once, prepare_image) || !s->image_count) return;
    s->bound_build = (unsigned) (build);
    atomic_store_explicit(&(s->enabled), 1, memory_order_release);
#else
    (void) on;
    (void) build;
#endif
}
int azsdk_cue_register(int deck, void *owned_unit) {
    struct State *s = state();
    if (!deck_ok(deck) || !owned_unit || !atomic_load_explicit(&s->enabled, memory_order_acquire)) return 0;
    unsigned slot = 0;
    uintptr_t server = 0, entry = 0;
    struct az_native_owner listener __attribute__((cleanup(az_native_owner_reset))) = {0};
    {
        AZ_MUTEX_GUARD(lock, s->registration, false);
        if (lock.error) return 0;
        if (!atomic_load_explicit(&s->enabled, memory_order_acquire) || s->owners[deck].object || s->used == slot_limit ||
            s->next_generation == max_generation) return 0;
        const uintptr_t unit = (uintptr_t) owned_unit;
        if (!canonical(unit)) return 0;
        const uintptr_t publisher = scalar_word(unit + unit_publisher);
        if (!checked_vtable(publisher, "N9dj_player15UnitStateServerE", 2)) return 0;
        server = scalar_word(publisher + publisher_server);
        const uintptr_t table = checked_vtable(server, "N9dj_player23StateNotificationServerE", 26);
        if (!table) return 0;
        entry = scalar_word(table + observer_slot * 8);
        if (!contains(entry, 4, execute_flag)) return 0;
        /* Synchronous post-Unit-ctor window only. The native complete server
         * constructors initialize this u32 from their fourth argument:
         * 1.30 STR at0x148a3f8;2.00 STR at0x1545988. Copy it once while the
         * caller owns the constructed chain; no later native field read.
         * Same embedded channel as registration vector+0x1508 and dispatch
         * X0. No deck namespace or global immutability is assigned to it. */
        const uint32_t route = scalar_u32(server + cue_channel + channel_route);
        slot = ++s->used;
        const uint64_t generation = ++s->next_generation;
        atomic_store_explicit(&s->pending[slot - 1].word, generation << mask_bits, memory_order_release);
        listener = az_listener_create(sizeof(struct CueListener), listener_vtable);
        if (!listener.object) {
            invalidate_slot((unsigned) deck, slot);
            return 0;
        }
        struct CueListener *object = listener.object;
        object->deck = (unsigned) deck;
        object->slot = slot;
        object->route = route;
        az_native_owner_copy(&s->owners[deck], &listener);
        s->owner_slot[deck] = slot;
        atomic_store_explicit(&s->active[deck], slot, memory_order_release);
    }
    /* Native registration may reenter or unwind. Retain the local core listener
     * without holding any SDK lock while native registration is running. */
    if (!az_listener_register(entry, server, &listener)) {
        invalidate_slot((unsigned) deck, slot);
        AZ_MUTEX_GUARD(lock, s->registration, false);
        if (!lock.error && s->owner_slot[deck] == slot) {
            az_native_owner_reset(&s->owners[deck]);
            s->owner_slot[deck] = 0;
        }
        return 0;
    }
    AZ_MUTEX_GUARD(lock, s->registration, false);
    if (lock.error) {
        invalidate_slot((unsigned) deck, slot);
        return 0;
    }
    if (!atomic_load_explicit(&s->enabled, memory_order_acquire) ||
        atomic_load_explicit(&s->active[deck], memory_order_acquire) != slot) {
        invalidate_slot((unsigned) deck, slot);
        if (s->owner_slot[deck] == slot) {
            az_native_owner_reset(&s->owners[deck]);
            s->owner_slot[deck] = 0;
        }
        return 0;
    }
    atomic_store_explicit(&s->acknowledged[deck], slot, memory_order_release);
    return 1;
}
void azsdk_cue_retire(int deck) {
    if (!deck_ok(deck)) return;
    struct State *s = state();
    const unsigned slot = atomic_load_explicit(&s->active[deck], memory_order_acquire);
    if (slot) invalidate_slot((unsigned) deck, slot);
    AZ_MUTEX_GUARD(lock, s->registration, false);
    if (!lock.error && slot && s->owner_slot[deck] == slot) {
        invalidate_slot((unsigned) deck, slot);
        az_native_owner_reset(&s->owners[deck]);
        s->owner_slot[deck] = 0;
    }
}

int azsdk_cue_take(int deck, struct azsdk_cue_sample *out) {
    struct State *s = state();
    if (!deck_ok(deck) || !out || !atomic_load_explicit(&(s->enabled), memory_order_acquire)) return 0;
    __auto_type slot = atomic_load_explicit(&(s->active[deck]), memory_order_acquire);
    if (!slot || slot > slot_limit || atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot) return 0;
    __auto_type p = &(s->published[deck]);
    bool free = false;
    if (!atomic_compare_exchange_strong_explicit(&(p->collecting), &(free), true, memory_order_acq_rel, memory_order_relaxed)) return 0;
    _Atomic(bool) *guard __attribute__((cleanup(collection_release))) = &p->collecting;
    __auto_type sequence = atomic_load_explicit(&(p->sequence), memory_order_acquire);
    __auto_type revision = atomic_load_explicit(&(p->revision), memory_order_relaxed);
    if ((sequence & 1u) || sequence > UINT64_MAX - 2 || revision == UINT64_MAX ||
        !atomic_load_explicit(&(s->enabled), memory_order_acquire) || atomic_load_explicit(&(s->active[deck]), memory_order_acquire) != slot ||
        atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot ||
        !(atomic_load_explicit(&(s->pending[slot - 1].word), memory_order_acquire) & cue_mask)) return 0;
    __auto_type now = collection_now((unsigned) (deck));
    if (!now) return 0; // preserve pending on clock failure
    __auto_type batch = atomic_fetch_and_explicit(&(s->pending[slot - 1].word), ~(uint64_t) (cue_mask), memory_order_acq_rel);
    __auto_type mask = ((unsigned) (batch & cue_mask));
    if (!mask) return 0;
#ifdef AZSDK_CUE_TESTING
    __auto_type hook = atomic_load_explicit(&(s->after_collect), memory_order_acquire);
    if (hook) hook(deck);
#endif
    if (!atomic_load_explicit(&(s->enabled), memory_order_acquire) || atomic_load_explicit(&(s->active[deck]), memory_order_acquire) != slot ||
        atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot) return 0;
    const struct azsdk_cue_sample value = {batch >> mask_bits, revision + 1, now, mask, 0};
    atomic_store_explicit(&(p->sequence), sequence + 1, memory_order_release);
    atomic_store_explicit(&(p->generation), value.unit_generation, memory_order_relaxed);
    atomic_store_explicit(&(p->revision), value.revision, memory_order_relaxed);
    atomic_store_explicit(&(p->observed), now, memory_order_relaxed);
    atomic_store_explicit(&(p->mask), mask, memory_order_relaxed);
    atomic_store_explicit(&(p->sequence), sequence + 2, memory_order_release);
    if (!atomic_load_explicit(&(s->enabled), memory_order_acquire) || atomic_load_explicit(&(s->active[deck]), memory_order_acquire) != slot ||
        atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot) return 0;
    *out = value;
    return 1;
}
int azsdk_cue_read(int deck, struct azsdk_cue_sample *out) {
    struct State *s = state();
    if (!deck_ok(deck) || !out || !atomic_load_explicit(&(s->enabled), memory_order_acquire)) return 0;
    __auto_type slot = atomic_load_explicit(&(s->active[deck]), memory_order_acquire);
    if (!slot || slot > slot_limit || atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot) return 0;
    __auto_type generation = atomic_load_explicit(&(s->pending[slot - 1].word), memory_order_acquire) >> mask_bits;
    __auto_type p = &(s->published[deck]);
    __auto_type sequence = atomic_load_explicit(&(p->sequence), memory_order_acquire);
    if (sequence & 1u) return 0;
    const struct azsdk_cue_sample value = {atomic_load_explicit(&(p->generation), memory_order_relaxed), atomic_load_explicit(&(p->revision), memory_order_relaxed),
                                           atomic_load_explicit(&(p->observed), memory_order_relaxed), atomic_load_explicit(&(p->mask), memory_order_relaxed), 0};
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&(p->sequence), memory_order_relaxed) != sequence || !value.unit_generation || value.unit_generation != generation ||
        !value.revision || !value.observed_ns || !value.changed_index_mask || !atomic_load_explicit(&(s->enabled), memory_order_acquire) ||
        atomic_load_explicit(&(s->active[deck]), memory_order_acquire) != slot ||
        atomic_load_explicit(&(s->acknowledged[deck]), memory_order_acquire) != slot) return 0;
    *out = value;
    return 1;
}
unsigned azsdk_cue_capabilities() {
    struct State *s = state();
    unsigned result = 0;
    if (!atomic_load_explicit(&(s->enabled), memory_order_acquire)) return 0;
    for (unsigned d = 0; d != decks; ++d) {
        __auto_type slot = atomic_load_explicit(&(s->active[d]), memory_order_acquire);
        if (slot && atomic_load_explicit(&(s->acknowledged[d]), memory_order_acquire) == slot) result |= 1u << d;
    }
    return atomic_load_explicit(&(s->enabled), memory_order_acquire) ? result : 0;
}

#ifdef AZSDK_CUE_TESTING
int azsdk_cue_test_control(int deck, unsigned kind, uint64_t value) {
    if (!deck_ok(deck)) return 0;
    struct State *s = state();
    __auto_type p = &(s->published[deck]);
    if (kind == 1) {
        if (!value) {
            atomic_store_explicit(&(p->collecting), false, memory_order_release);
            return 1;
        }
        bool free = false;
        return atomic_compare_exchange_strong_explicit(&(p->collecting), &(free), true, memory_order_seq_cst, memory_order_seq_cst);
    }
    if (kind == 2) {
        atomic_store_explicit(&(p->clock_failure), value != 0, memory_order_release);
        return 1;
    }
    if (kind == 3) {
        atomic_store_explicit(&(p->revision), value, memory_order_release);
        return 1;
    }
    if (kind == 4) {
        __auto_type slot = atomic_load_explicit(&(s->active[deck]), memory_order_acquire);
        return slot && slot <= slot_limit ? (int) (atomic_load_explicit(&(s->pending[slot - 1].word), memory_order_acquire) & cue_mask) : 0;
    }
    if (kind == 5) {
        AZ_MUTEX_LOCK(lock, s->registration);
        if (value < s->next_generation || value > max_generation) return 0;
        s->next_generation = value;
        return 1;
    }
    return 0;
}
void azsdk_cue_test_pending_hook(void (*hook)(int)) {
    atomic_store_explicit(&(state()->before_pending), hook, memory_order_release);
}
void azsdk_cue_test_collect_hook(void (*hook)(int)) {
    atomic_store_explicit(&(state()->after_collect), hook, memory_order_release);
}
#endif

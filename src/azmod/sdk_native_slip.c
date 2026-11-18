/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azslipobserver.h"
#include "../common/aznative_c.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
struct azsdk_slip_debug_v1 azsdk_slip_observation_v1[4];
struct azsdk_slip_getter_debug_v1 azsdk_slip_getter_observation_v1[4];
_Static_assert(sizeof(struct azsdk_slip_debug_v1) == 56, "internal scalar observation ABI");
_Static_assert(sizeof(struct azsdk_slip_getter_debug_v1) == 56, "internal getter observation ABI");
enum {
    limit = 64
};
typedef struct Slot Slot;
struct Slot {
    _Atomic unsigned stage;
    uintptr_t source, source_control, parent_control;
    unsigned deck;
    struct az_native_control *source_owner, *parent_owner;
    struct az_native_owner listener;
    _Atomic bool writing;
    _Atomic uint64_t stamp, revision, observed, callbacks;
    _Atomic unsigned raw;
    _Atomic uintptr_t getter[3];
    _Atomic uint64_t getter_ns;
    _Atomic unsigned getter_seen, getter_conflict;
    uint64_t next_revision;
};
typedef struct State {
    struct az_slip_profile profile;
    _Atomic bool configured;
    _Atomic unsigned used, current[4];
    Slot slots[limit];
} State;
/* Process lifetime, including native callbacks during process teardown. */
static State storage;
static State *state(void) { return &storage; }
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "native callbacks use lock-free scalar atomics");
struct Pair {
    uintptr_t source, control;
};
#define scalar(type, address) ({ type value; memcpy(&value, (const void *)(address), sizeof value); value; })
static bool pointer(uintptr_t p) { return p >= 0x10000 && !(p & 7) && p <= UINT64_C(0x0000ffffffffffff); }
static unsigned used() {
    unsigned n = atomic_load_explicit(&(state()->used), memory_order_acquire);
    return n < limit ? n : limit;
}
static unsigned number(const Slot *slot) { return (unsigned) (slot - state()->slots) + 1; }
static bool current(const Slot *slot) {
    return atomic_load_explicit(&(slot->stage), memory_order_acquire) == 2 && atomic_load_explicit(&(state()->current[slot->deck]), memory_order_acquire) == number(slot) &&
           az_native_alive(slot->parent_owner) && az_native_alive(slot->source_owner);
}
static void retire(Slot *slot) {
    if (atomic_exchange_explicit(&(slot->stage), 3, memory_order_acq_rel) == 3) return;
    unsigned expected = number(slot);
    atomic_compare_exchange_strong_explicit(&(state()->current[slot->deck]), &(expected), 0, memory_order_acq_rel, memory_order_acquire);
    az_native_owner_reset(&slot->listener);
}
static void capture(Slot *, unsigned);
struct OwnedListener {
    const void *const *vtable;
    Slot *slot;
};
struct ListenerControl {
    struct az_native_control base;
    struct OwnedListener receiver;
};
static void listener_destroy(void *object) { (void) object; }
static void listener_delete(void *object) { (void) object; }
static void listener_changed(void *object, unsigned raw) {
    AZ_NATIVE_ERRNO(preserve);
    capture(((struct OwnedListener *) object)->slot, raw);
}
static const void *const listener_vtable[] = {
    (void *) listener_destroy, (void *) listener_delete, (void *) listener_changed};
static void control_dispose(void *control) { (void) control; }
static void control_destroy(void *control) { free(control); }
static void *control_deleter(void *control, const void *type) {
    (void) control;
    (void) type;
    return NULL;
}
static const void *const control_vtable[] = {
    (void *) control_dispose, (void *) control_destroy, (void *) control_dispose,
    (void *) control_destroy, (void *) control_deleter};
static void listener_registration_drop(Slot **slot) {
    if (*slot) retire(*slot);
}
static bool make_listener(Slot *slot) {
    struct ListenerControl *control = calloc(1, sizeof *control);
    if (!control) return false;
    control->base = (struct az_native_control) {control_vtable, 1, 1};
    control->receiver = (struct OwnedListener) {listener_vtable, slot};
    slot->listener = (struct az_native_owner) {&control->receiver, &control->base};
    /* A const weak reference borrows the same two-word carrier during the call;
     * native code owns every weak copy it retains. The strong owner pins it. */
    Slot *pending __attribute__((cleanup(listener_registration_drop))) = slot;
    state()->profile.registration((void *) slot->source, &slot->listener);
    pending = NULL;
    return true;
}
static void constructed(const struct azce_call *call) {
    State *s = state();
    __auto_type object = (uintptr_t) (call->args[0]);
    const unsigned deck = (unsigned) (call->args[2]);
    if (call->caller_link != s->profile.construct_return || deck >= 4 || !pointer(object) || object < 16 ||
        scalar(uintptr_t, object - 16) != s->profile.source_control_vtable ||
        __atomic_load_n(&((struct az_native_control *) (object - 16))->strong, __ATOMIC_RELAXED) <= 0 || scalar(unsigned, object + 0x134) != deck) return;
    for (unsigned i = 0; i < used(); i++) {
        const Slot *prior = &(s->slots[i]);
        __auto_type stage = atomic_load_explicit(&(prior->stage), memory_order_acquire);
        if (stage && prior->source_control == object - 16 && (stage != 3 || prior->source_owner)) return;
    }
    unsigned n = atomic_load_explicit(&(s->used), memory_order_relaxed);
    do {
        if (n == limit) return;
    } while (!atomic_compare_exchange_weak_explicit(&(s->used), &(n), n + 1, memory_order_acq_rel, memory_order_acquire));
    Slot *slot = &(s->slots[n]);
    slot->source = object;
    slot->source_control = object - 16;
    slot->deck = deck;
    atomic_store_explicit(&(slot->stage), 1, memory_order_release);
}
static void published(const struct azce_call *call) {
    State *s = state();
    const unsigned deck = (unsigned) (call->args[5]);
    __auto_type parent = (uintptr_t) (call->args[0]);
    if (deck >= 4 || call->caller_link != s->profile.parent_returns[deck] || !pointer(parent) || parent < 16 ||
        scalar(uintptr_t, parent - 16) != s->profile.parent_control_vtable) return;
    /* The successful actual PlayerInfoState constructor has published this real
     * shared_ptr<gui::SlipState> into its owned member. It is not an SDK caller
     * inventing an owner from an address; the exact native caller holds the new
     * inplace PlayerInfoState owner throughout this constructor return. */
    __auto_type pair = scalar(struct Pair, parent + 0x1120);
    for (unsigned i = 0; i < used(); i++) {
        const Slot *prior = &(s->slots[i]);
        __auto_type stage = atomic_load_explicit(&(prior->stage), memory_order_acquire);
        if ((stage == 2 || stage == 3) && prior->parent_control == parent - 16 &&
            prior->source == pair.source && prior->source_control == pair.control) return;
    }
    atomic_store_explicit(&(s->current[deck]), 0, memory_order_release);
    for (unsigned i = used(); i; i--) {
        Slot *slot = &(s->slots[i - 1]);
        if (atomic_load_explicit(&(slot->stage), memory_order_acquire) != 1 || slot->deck != deck || slot->source != pair.source || slot->source_control != pair.control) continue;
        slot->source_owner = (struct az_native_control *) pair.control;
        az_native_weak_add(slot->source_owner);
        slot->parent_owner = (struct az_native_control *) (parent - 16);
        az_native_weak_add(slot->parent_owner);
        slot->parent_control = parent - 16;
        if (!make_listener(slot)) {
            retire(slot);
            return;
        }
        atomic_store_explicit(&(slot->stage), 2, memory_order_release);
        atomic_store_explicit(&(s->current[deck]), i, memory_order_release);
        return;
    }
}
static void disposing(uintptr_t control, bool parent) {
    if (!control) return;
    for (unsigned i = 0; i < used(); i++) {
        Slot *slot = &(state()->slots[i]);
        __auto_type stage = atomic_load_explicit(&(slot->stage), memory_order_acquire);
        if ((stage == 1 || stage == 2) && (parent ? slot->parent_control : slot->source_control) == control) retire(slot);
    }
}
static uint64_t now() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    uint64_t sec = (uint64_t) (t.tv_sec), ns = (uint64_t) (t.tv_nsec);
    return sec > (UINT64_MAX - ns) / UINT64_C(1000000000) ? 0 : sec * UINT64_C(1000000000) + ns;
}
static void capture(Slot *value, unsigned raw) {
    Slot *slot = value;
    if (!current(slot)) return;
    bool idle = false;
    if (!atomic_compare_exchange_strong_explicit(&(slot->writing), &(idle), true, memory_order_acquire, memory_order_acquire)) return;
    __auto_type ns = now();
    __auto_type sequence = atomic_load_explicit(&(slot->stamp), memory_order_relaxed);
    if (sequence > UINT64_MAX - 2) {
        retire(slot);
        atomic_store_explicit(&(slot->writing), false, memory_order_release);
        return;
    }
    __auto_type calls = atomic_load_explicit(&(slot->callbacks), memory_order_relaxed);
    if (calls == UINT64_MAX) {
        retire(slot);
        atomic_store_explicit(&(slot->writing), false, memory_order_release);
        return;
    }
    atomic_store_explicit(&(slot->callbacks), calls + 1, memory_order_release);
    atomic_store_explicit(&(slot->stamp), sequence + 1, memory_order_seq_cst);
    if (ns && slot->next_revision != UINT64_MAX) {
        atomic_store_explicit(&(slot->raw), raw, memory_order_seq_cst);
        atomic_store_explicit(&(slot->observed), ns, memory_order_seq_cst);
        atomic_store_explicit(&(slot->revision), ++slot->next_revision, memory_order_seq_cst);
    } else {
        atomic_store_explicit(&(slot->revision), 0, memory_order_seq_cst);
        atomic_store_explicit(&(slot->observed), 0, memory_order_seq_cst);
    }
    atomic_store_explicit(&(slot->stamp), sequence + 2, memory_order_seq_cst);
    atomic_store_explicit(&(slot->writing), false, memory_order_release);
}
int az_slip_configure(const struct az_slip_profile *value) {
    if (!value->construct_return || !value->registration || !value->source_control_vtable || !value->parent_control_vtable) return false;
    for (unsigned i = 0; i < 4; i++)
        if (!value->parent_returns[i]) return false;
    State *s = state();
    if (atomic_load_explicit(&(s->configured), memory_order_seq_cst)) return false;
    s->profile = *value;
    atomic_store_explicit(&(s->configured), true, memory_order_release);
    return true;
}
void az_slip_forward(struct azce_call *call, void (*original)(struct azce_call *)) {
    if (!call || !original) return;
    bool reserved = call->reserved != 0;
#if AZCE_SIZE >= 320
    reserved = reserved || call->caller_reserved != 0;
#endif
    if (!atomic_load_explicit(&(state()->configured), memory_order_acquire) || reserved) {
        original(call);
        return;
    }
    switch (call->kind) {
    case AZ_SLIP_CONSTRUCT:
        original(call);
        {
            AZ_NATIVE_ERRNO(preserve);
            constructed(call);
        }
        return;
    case AZ_SLIP_PUBLISH_PARENT:
        original(call);
        {
            AZ_NATIVE_ERRNO(preserve);
            published(call);
        }
        return;
    case AZ_SLIP_DISPOSE_SOURCE:
    case AZ_SLIP_DISPOSE_PARENT: {
        AZ_NATIVE_ERRNO(preserve);
        disposing(call->args[0], call->kind == AZ_SLIP_DISPOSE_PARENT);
    }
        original(call);
        return;
    default: original(call); return;
    }
}
int az_slip_read(unsigned deck, struct az_slip_receipt *out) {
    AZ_NATIVE_ERRNO(preserve);
    if (deck >= 4 || !out) return false;
    State *s = state();
    const unsigned id = atomic_load_explicit(&(s->current[deck]), memory_order_acquire);
    if (!id || id > used()) return false;
    Slot *slot = &(s->slots[id - 1]);
    if (!current(slot)) return false;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        __auto_type before = atomic_load_explicit(&(slot->stamp), memory_order_seq_cst);
        if (before & 1) continue;
        struct az_slip_receipt value = {id, atomic_load_explicit(&(slot->revision), memory_order_seq_cst), atomic_load_explicit(&(slot->observed), memory_order_seq_cst), atomic_load_explicit(&(slot->raw), memory_order_seq_cst), 0};
        if (before == atomic_load_explicit(&(slot->stamp), memory_order_seq_cst) && value.revision && value.observed_ns && current(slot)) {
            *out = value;
            return true;
        }
    }
    return false;
}
void azsdk_slip_capture_getter(unsigned site, uintptr_t source, uintptr_t target) {
    AZ_NATIVE_ERRNO(preserve);
    if (site >= 3 || target < 0x10000 || target > UINT64_C(0x0000ffffffffffff) || (target & 3)) return;
    for (unsigned i = 0; i < used(); i++) {
        Slot *slot = &(state()->slots[i]);
        if (!current(slot) || slot->source != source) continue;
        bool idle = false;
        if (!atomic_compare_exchange_strong_explicit(&(slot->writing), &(idle), true, memory_order_acquire, memory_order_acquire)) return;
        uint64_t stamp = atomic_load_explicit(&(slot->stamp), memory_order_relaxed), ns = now();
        if (stamp <= UINT64_MAX - 2 && ns && current(slot)) {
            atomic_store_explicit(&(slot->stamp), stamp + 1, memory_order_seq_cst);
            __auto_type prior = atomic_load_explicit(&(slot->getter[site]), memory_order_seq_cst);
            const unsigned bit = 1u << site;
            if (prior && prior != target) atomic_fetch_or_explicit(&(slot->getter_conflict), bit, memory_order_seq_cst);
            else {
                atomic_store_explicit(&(slot->getter[site]), target, memory_order_seq_cst);
                atomic_fetch_or_explicit(&(slot->getter_seen), bit, memory_order_seq_cst);
            }
            atomic_store_explicit(&(slot->getter_ns), ns, memory_order_seq_cst);
            atomic_store_explicit(&(slot->stamp), stamp + 2, memory_order_seq_cst);
        }
        atomic_store_explicit(&(slot->writing), false, memory_order_release);
        return;
    }
}
void azsdk_slip_collect_v1(void) {
    AZ_NATIVE_ERRNO(preserve);
    static _Atomic(bool) collecting;
    bool idle = false;
    if (!atomic_compare_exchange_strong_explicit(&(collecting), &(idle), true, memory_order_acquire, memory_order_acquire)) return;
    for (unsigned d = 0; d < 4; d++) {
        struct az_slip_receipt r = {0};
        bool valid = az_slip_read(d, &r);
        __auto_type id = atomic_load_explicit(&(state()->current[d]), memory_order_acquire);
        const bool registered = id && id <= used() && current(&state()->slots[id - 1]);
        if (!registered || (valid && r.generation != id)) valid = false;
        struct azsdk_slip_debug_v1 *out = &azsdk_slip_observation_v1[d];
        __auto_type seq = __atomic_load_n(&out->sequence, __ATOMIC_RELAXED);
        if (seq > UINT64_MAX - 2) continue;
        __atomic_store_n(&out->sequence, seq + 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->generation, registered ? (uint64_t) (id) : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->revision, valid ? r.revision : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->observed_ns, valid ? r.observed_ns : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->callbacks, registered ? atomic_load_explicit(&(state()->slots[id - 1].callbacks), memory_order_seq_cst) : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->registered, registered ? 1u : 0u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->valid, valid ? 1u : 0u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->raw, valid ? r.raw : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->reserved, 0u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&out->sequence, seq + 2, __ATOMIC_SEQ_CST);
        struct azsdk_slip_getter_debug_v1 *get = &azsdk_slip_getter_observation_v1[d];
        struct azsdk_slip_getter_debug_v1 g = (struct azsdk_slip_getter_debug_v1) {0};
        if (registered)
            for (unsigned attempt = 0; attempt < 3; attempt++) {
                Slot *slot = &(state()->slots[id - 1]);
                __auto_type before = atomic_load_explicit(&(slot->stamp), memory_order_seq_cst);
                if (before & 1) continue;
                g.generation = id;
                g.observed_ns = atomic_load_explicit(&(slot->getter_ns), memory_order_seq_cst);
                g.target_slot4 = atomic_load_explicit(&(slot->getter[0]), memory_order_seq_cst);
                g.target_slot13 = atomic_load_explicit(&(slot->getter[1]), memory_order_seq_cst);
                g.target_slot14 = atomic_load_explicit(&(slot->getter[2]), memory_order_seq_cst);
                g.seen_mask = atomic_load_explicit(&(slot->getter_seen), memory_order_seq_cst);
                g.conflict_mask = atomic_load_explicit(&(slot->getter_conflict), memory_order_seq_cst);
                if (before == atomic_load_explicit(&(slot->stamp), memory_order_seq_cst) && current(slot)) break;
                g = (struct azsdk_slip_getter_debug_v1) {0};
            }
        __auto_type gs = __atomic_load_n(&get->sequence, __ATOMIC_RELAXED);
        if (gs > UINT64_MAX - 2) continue;
        __atomic_store_n(&get->sequence, gs + 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->generation, g.generation, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->observed_ns, g.observed_ns, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->target_slot4, g.target_slot4, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->target_slot13, g.target_slot13, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->target_slot14, g.target_slot14, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->seen_mask, g.seen_mask, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->conflict_mask, g.conflict_mask, __ATOMIC_SEQ_CST);
        __atomic_store_n(&get->sequence, gs + 2, __ATOMIC_SEQ_CST);
    }
    atomic_store_explicit(&(collecting), false, memory_order_release);
}

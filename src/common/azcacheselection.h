/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHESELECTION_H
#define AZCACHESELECTION_H
#include "aznative_c.h"
#include <stdatomic.h>
#include <string.h>
/* The caller already owns and serializes the complete native objects. Fences
 * qualify observations; they never acquire a native object's lifetime. */
enum {
    AZSEL_OK = 0,
    AZSEL_UNAVAILABLE = 1,
    AZSEL_GUARD = 2,
    AZSEL_CHANGED = 3,
    AZSEL_LIMIT = 4
};
enum {
    AZSEL_SELECTION_CHANGE = 1,
    AZSEL_SCOPE_LOSS = 2
};
struct azsel_position {
    uint32_t outer, selector;
};
struct azsel_borrow {
    unsigned build;
    const void *root;
    void *context;
    bool (*already_owned)(void *);
    bool (*supported_sqlite_mount)(void *, const void *);
};
struct azsel_state {
    unsigned limit;
    _Atomic unsigned active, mutating;
    _Atomic uint64_t revision, next;
    _Atomic bool saturated;
};
#define AZSEL_STATE_INIT {.limit = 32, .revision = 1}
static inline void azsel_state_init(struct azsel_state *s, unsigned depth, uint64_t revision, uint64_t next) {
    *s = (struct azsel_state) {.limit = depth, .revision = revision, .next = next, .saturated = !depth || depth > 32 || !revision};
}
static inline bool azsel_stamp(const struct azsel_state *s, uint64_t *revision) {
    if (atomic_load_explicit(&s->saturated, memory_order_acquire) || atomic_load_explicit(&s->mutating, memory_order_acquire)) return false;
    *revision = atomic_load_explicit(&s->revision, memory_order_acquire);
    return *revision && !atomic_load_explicit(&s->mutating, memory_order_acquire) && !atomic_load_explicit(&s->saturated, memory_order_acquire) && atomic_load_explicit(&s->revision, memory_order_acquire) == *revision;
}
static inline bool azsel_stamp_is(const struct azsel_state *s, uint64_t revision) {
    uint64_t current = 0;
    return azsel_stamp(s, &current) && current == revision;
}
struct azsel_invocation {
    struct azsel_state *state;
    uint64_t id, revision;
    bool entered;
};
static inline void azsel_invocation_init(struct azsel_invocation *i, struct azsel_state *s) {
    *i = (struct azsel_invocation) {.state = s};
    if (!azsel_stamp(s, &i->revision)) return;
    unsigned active = atomic_load_explicit(&s->active, memory_order_acquire);
    while (active < s->limit) {
        if (atomic_compare_exchange_weak_explicit(&s->active, &active, active + 1, memory_order_acq_rel, memory_order_acquire)) {
            uint64_t next = atomic_load_explicit(&s->next, memory_order_acquire);
            while (next != UINT64_MAX) {
                if (atomic_compare_exchange_weak_explicit(&s->next, &next, next + 1, memory_order_acq_rel, memory_order_acquire)) {
                    i->id = next + 1;
                    if (azsel_stamp_is(s, i->revision)) {
                        i->entered = true;
                        return;
                    }
                    atomic_fetch_sub_explicit(&s->active, 1, memory_order_release);
                    return;
                }
            }
            atomic_store_explicit(&s->saturated, true, memory_order_release);
            atomic_fetch_sub_explicit(&s->active, 1, memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&s->saturated, true, memory_order_release);
}
static inline void azsel_invocation_drop(struct azsel_invocation *i) {
    if (i->entered) {
        i->entered = false;
        atomic_fetch_sub_explicit(&i->state->active, 1, memory_order_release);
    }
}
static inline bool azsel_ready(const struct azsel_invocation *i) { return i->entered && azsel_stamp_is(i->state, i->revision); }
struct azsel_mutation {
    struct azsel_state *state;
    bool entered;
};
static inline void azsel_mutation_init(struct azsel_mutation *m, struct azsel_state *s, unsigned event) {
    *m = (struct azsel_mutation) {.state = s};
    if (event != AZSEL_SELECTION_CHANGE && event != AZSEL_SCOPE_LOSS) atomic_store_explicit(&s->saturated, true, memory_order_release);
    unsigned depth = atomic_load_explicit(&s->mutating, memory_order_acquire);
    while (depth < s->limit)
        if (atomic_compare_exchange_weak_explicit(&s->mutating, &depth, depth + 1, memory_order_acq_rel, memory_order_acquire)) {
            m->entered = true;
            break;
        }
    if (!m->entered) atomic_store_explicit(&s->saturated, true, memory_order_release);
    uint64_t revision = atomic_load_explicit(&s->revision, memory_order_acquire);
    for (;;) {
        if (revision == UINT64_MAX) {
            atomic_store_explicit(&s->saturated, true, memory_order_release);
            break;
        }
        if (atomic_compare_exchange_weak_explicit(&s->revision, &revision, revision + 1, memory_order_acq_rel, memory_order_acquire)) break;
    }
}
static inline void azsel_mutation_drop(struct azsel_mutation *m) {
    if (m->entered) {
        m->entered = false;
        atomic_fetch_sub_explicit(&m->state->mutating, 1, memory_order_release);
    }
}
static inline bool azsel_add(uintptr_t base, uintptr_t offset, uintptr_t *out) {
    if (offset > UINTPTR_MAX - base) return false;
    *out = base + offset;
    return true;
}
struct azsel_vector {
    uintptr_t begin, end, capacity;
};
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(struct azsel_vector) == 24, "native GNU vector prefix");
static inline bool azsel_count(const struct azsel_vector *v, uintptr_t stride, uintptr_t *out) {
    if (!stride) return false;
    if (!v->begin) {
        *out = 0;
        return !v->end && !v->capacity;
    }
    if ((v->begin | v->end | v->capacity) & 7u || v->end < v->begin || v->capacity < v->end || (v->end - v->begin) % stride || (v->capacity - v->begin) % stride) return false;
    *out = (v->end - v->begin) / stride;
    return true;
}
static inline struct azsel_vector azsel_view(uintptr_t address) {
    struct azsel_vector v;
    memcpy(&v, (const void *) address, sizeof v);
    return v;
}
static inline bool azsel_same(const struct az_native_owner *a, const struct az_native_owner *b) { return a->object && b->object && a->object == b->object && a->control == b->control; }
struct azsel_proof {
    const struct azsel_borrow *borrow;
    const void *mount;
    bool result;
};
static inline void azsel_owned_proof(void *opaque) {
    struct azsel_proof *p = opaque;
    p->result = p->borrow->already_owned(p->borrow->context);
}
static inline void azsel_mount_proof(void *opaque) {
    struct azsel_proof *p = opaque;
    p->result = p->borrow->supported_sqlite_mount(p->borrow->context, p->mount);
}
static inline bool azsel_allowed(const struct azsel_invocation *i, const struct azsel_borrow *b) {
    if (!azsel_ready(i) || (b->build != 1 && b->build != 2) || !b->root || ((uintptr_t) b->root & 7u) || !b->already_owned || !b->supported_sqlite_mount) return false;
    struct azsel_proof p = {.borrow = b};
    azsel_owned_proof(&p);
    return p.result && azsel_ready(i);
}
struct azsel_pins {
    struct az_native_owner mount, database, collector, cache;
};
static inline void azsel_pins_drop(struct azsel_pins *p) {
    az_native_owner_reset(&p->cache);
    az_native_owner_reset(&p->collector);
    az_native_owner_reset(&p->database);
    az_native_owner_reset(&p->mount);
}
struct azsel_resolution {
    int status;
    const struct azsel_state *state;
    uint64_t invocation, revision;
    const void *root;
    struct azsel_position position;
    struct azsel_pins pins;
};
static inline void azsel_resolution_drop(struct azsel_resolution *r) { azsel_pins_drop(&r->pins); }
static inline bool azsel_root_addresses(const void *root, uintptr_t *mount, uintptr_t *collector, uintptr_t *outer) {
    return azsel_add((uintptr_t) root, 0x28, mount) && azsel_add((uintptr_t) root, 0x40, collector) && azsel_add((uintptr_t) root, 0x50, outer);
}
static inline bool azsel_pin_source(const struct azsel_invocation *i, const struct azsel_borrow *b,
                                    struct azsel_pins *p, uintptr_t *outer, int *status) {
    uintptr_t mount = 0, collector = 0, database = 0;
    *status = AZSEL_GUARD;
    if (!azsel_root_addresses(b->root, &mount, &collector, outer)) return false;
    az_native_owner_copy(&p->mount, (const struct az_native_owner *) mount);
    az_native_owner_copy(&p->collector, (const struct az_native_owner *) collector);
    *status = AZSEL_UNAVAILABLE;
    if (!p->mount.object || !p->collector.object || !azsel_ready(i)) return false;
    struct azsel_proof proof = {b, p->mount.object, false};
    *status = AZSEL_GUARD;
    azsel_mount_proof(&proof);
    if (!proof.result) return false;
    *status = AZSEL_CHANGED;
    if (!azsel_allowed(i, b)) return false;
    *status = AZSEL_GUARD;
    if (!azsel_add((uintptr_t) p->mount.object, 0x30, &database)) return false;
    az_native_owner_copy(&p->database, (const struct az_native_owner *) database);
    return true;
}
static inline bool azsel_vector_entry(uintptr_t address, uintptr_t stride, uintptr_t slot, uintptr_t *entry, int *status) {
    struct azsel_vector vector = azsel_view(address);
    uintptr_t count = 0;
    *status = AZSEL_UNAVAILABLE;
    if (!azsel_count(&vector, stride, &count) || slot >= count) return false;
    *status = AZSEL_GUARD;
    return slot <= (UINTPTR_MAX - vector.begin) / stride && azsel_add(vector.begin, slot * stride, entry);
}
static inline bool azsel_cache_address(uintptr_t outer, struct azsel_position pos, uintptr_t *cache, int *status) {
    uintptr_t inner = 0;
    if (!azsel_vector_entry(outer, 24, pos.outer, &inner, status)) return false;
    return azsel_vector_entry(inner, 16, pos.selector - 1, cache, status);
}
/* Zero-initialize output; returned pins stay within the already-owned callback. */
static inline void azsel_publish_resolution(const struct azsel_invocation *i, const struct azsel_borrow *b,
                                            struct azsel_position pos, struct azsel_pins *pins, struct azsel_resolution *r) {
    *r = (struct azsel_resolution) {AZSEL_OK, i->state, i->entered ? i->id : 0, i->revision, b->root, pos, *pins};
    *pins = (struct azsel_pins) {0};
}
static inline void azsel_resolve_owned(const struct azsel_invocation *i, const struct azsel_borrow *b, struct azsel_position pos, struct azsel_resolution *r) {
    azsel_resolution_drop(r);
    *r = (struct azsel_resolution) {.status = AZSEL_GUARD};
    if (!azsel_allowed(i, b)) return;
    r->status = AZSEL_UNAVAILABLE;
    if (pos.selector < 1 || pos.selector > 9) return;
    struct azsel_pins p __attribute__((cleanup(azsel_pins_drop))) = {0};
    uintptr_t outer = 0, cache = 0;
    if (!azsel_pin_source(i, b, &p, &outer, &r->status)) return;
    if (!azsel_cache_address(outer, pos, &cache, &r->status)) return;
    az_native_owner_copy(&p.cache, (const struct az_native_owner *) cache);
    r->status = AZSEL_CHANGED;
    if (!p.database.object || !p.cache.object || !azsel_allowed(i, b)) return;
    azsel_publish_resolution(i, b, pos, &p, r);
}
static inline bool azsel_same_invocation(const struct azsel_invocation *i, const struct azsel_borrow *b, const struct azsel_resolution *selected) {
    return selected->status == AZSEL_OK && selected->state == i->state && selected->invocation == (i->entered ? i->id : 0) &&
           selected->revision == i->revision && selected->root == b->root;
}
static inline bool azsel_same_position(struct azsel_position a, struct azsel_position b) {
    return a.outer == b.outer && a.selector == b.selector;
}
static inline bool azsel_validate_fresh(const struct azsel_invocation *i, const struct azsel_borrow *b, struct azsel_position p, const struct azsel_resolution *selected) {
    if (!azsel_same_invocation(i, b, selected) || !azsel_same_position(p, selected->position) || !azsel_allowed(i, b)) return false;
    struct azsel_resolution fresh __attribute__((cleanup(azsel_resolution_drop))) = {0};
    azsel_resolve_owned(i, b, p, &fresh);
    return fresh.status == AZSEL_OK && azsel_same(&selected->pins.mount, &fresh.pins.mount) && azsel_same(&selected->pins.database, &fresh.pins.database) && azsel_same(&selected->pins.collector, &fresh.pins.collector) && azsel_same(&selected->pins.cache, &fresh.pins.cache) && azsel_allowed(i, b);
}
#endif

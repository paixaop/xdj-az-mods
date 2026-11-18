/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEBIRTH_H
#define AZCACHEBIRTH_H
#include "aznative_c.h"
/* Private C borrow/forward contract, separate from native entry prototypes and
 * the public SDK. Every owner below uses the actual GNU alias/control ABI. */
struct azce_call;
struct azcb_source_borrow {
    void *server;
    unsigned build;
    uint64_t source_epoch;
    const struct az_native_owner *mount, *database;
};
struct azcb_forward {
    void (*call)(void *);
    void *context;
};
#define AZ_CACHE_PRIVATE __attribute__((visibility("hidden")))
AZ_CACHE_PRIVATE void azcb_forward_library_owned(unsigned, void *, struct azcb_forward);
AZ_CACHE_PRIVATE void azcb_observe_entry_owned(struct azce_call *, struct azcb_forward);
struct azcb_birth_borrow {
    struct azcb_source_borrow source;
    const struct az_native_owner *collector, *creator_database;
    const void *receiver_collector, *receiver_creator, *borrowed_pool;
    /* Original constructs this native strong owner in the caller's result
     * storage. Read it only after the original returns normally. */
    const struct az_native_owner *native_result;
};
enum azcb_factory_kind {
    AZCB_PRIMARY_FACTORY = 1,
    AZCB_STANDARD_FALLBACK_FACTORY = 2
};
struct azcb_factory_borrow {
    const void *receiver_creator;
    unsigned kind;
    const void *borrowed_pool;
    const void *const *native_result;
};
enum azcb_write_kind {
    AZCB_SETUP_WRITE = 1,
    AZCB_LAYOUT_WRITE = 2
};
struct azcb_write_borrow {
    const void *receiver_cache;
    unsigned kind, argument;
    const void *borrowed_pool;
    const int *native_status;
};
/* Primary construction permits no mutation. Standard mode0 requires setup16,
 * then successful setters0..15 on its newly owned raw result in this invocation.
 * Unsupported/nested calls still forward, but cannot publish a birth. */
AZ_CACHE_PRIVATE void azcb_observe_factory_owned(const struct azcb_factory_borrow *, struct azcb_forward);
AZ_CACHE_PRIVATE void azcb_observe_construction_write_owned(const struct azcb_write_borrow *, struct azcb_forward);
struct azcb_query_borrow {
    struct azcb_source_borrow source;
    const struct az_native_owner *collector, *selected_cache;
    const void *receiver_cache;
};
/* Copies a scalar only, after successful native forwarding and within that
 * owning callback. Never retain a native row/container or a later-use pointer. */
struct azcb_copy_row {
    bool (*copy)(void *, uint32_t *);
    void *context;
};
struct azcb_observation {
    uint64_t birth_epoch, layout_generation, source_epoch;
    struct az_native_owner mount, database, collector, cache; /* weak owners */
    uint32_t row_value;
    unsigned factory_kind;
};
static inline void azcb_observation_drop(struct azcb_observation *o) {
    az_native_weak_reset(&o->cache);
    az_native_weak_reset(&o->collector);
    az_native_weak_reset(&o->database);
    az_native_weak_reset(&o->mount);
}
static inline void azcb_observation_copy(struct azcb_observation *dst, const struct azcb_observation *src) {
    if (dst == src) return;
    az_native_weak_copy(&dst->mount, &src->mount);
    az_native_weak_copy(&dst->database, &src->database);
    az_native_weak_copy(&dst->collector, &src->collector);
    az_native_weak_copy(&dst->cache, &src->cache);
    dst->birth_epoch = src->birth_epoch;
    dst->layout_generation = src->layout_generation;
    dst->source_epoch = src->source_epoch;
    dst->row_value = src->row_value;
    dst->factory_kind = src->factory_kind;
}
/* Forward once without constructing/freeing native result objects. On failure,
 * leave Observation unchanged. Borrowed holder storage must remain live or a
 * preceding mutation fence must reject before rereading it; an owner pin alone
 * cannot preserve a mutable vector's element storage. Only weak owners persist.
 * Birth requires proved fresh Collector creation, never a cache hit. */
AZ_CACHE_PRIVATE int azcb_observe_birth_owned(const struct azcb_birth_borrow *, struct azcb_forward, struct azcb_observation *);
AZ_CACHE_PRIVATE int azcb_observe_query_owned(const struct azcb_query_borrow *, struct azcb_forward, struct azcb_copy_row, struct azcb_observation *);
/* Retirement and selected-storage fencing precede native work and span unwind.
 * Neither acquires liveness or dereferences an unowned native receiver. */
AZ_CACHE_PRIVATE void azcb_retire_forward(struct azcb_forward);
AZ_CACHE_PRIVATE void azcb_selection_forward(struct azcb_forward);
#undef AZ_CACHE_PRIVATE
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCONNECTIONEPOCH_H
#define AZCONNECTIONEPOCH_H
#include "aznative_c.h"
#include <stdatomic.h>
#include <string.h>

/* Private bookkeeping for observed connection operations. A receipt is an
 * incarnation, never a sqlite3 owner or lease. Preexisting connections remain
 * unknown. The native output stays caller-owned throughout forwarding. */
struct az_connection_receipt {
    uint64_t generation;
};
struct az_connection_entry {
    const void *connection;
    uint64_t generation;
};
#define AZ_CONNECTION_CAPACITY 32
struct az_connection_state {
    pthread_mutex_t mutex;
    _Atomic(bool) saturated;
    struct az_connection_entry entries[AZ_CONNECTION_CAPACITY], status_entries[AZ_CONNECTION_CAPACITY];
    uint64_t next_generation, sequence, next_status_generation, last_close;
    unsigned active, closing;
    size_t capacity;
    uint64_t max_generation, max_sequence;
    unsigned max_active;
};
/* Bounds are immutable for a state's lifetime. Smaller policies exercise
 * exhaustion without resetting or reusing any allocated generation. */
#define AZ_CONNECTION_STATE_BOUNDS(cap, generation, sequence, active) \
    {.mutex = PTHREAD_MUTEX_INITIALIZER, .capacity = (cap), .max_generation = (generation), .max_sequence = (sequence), .max_active = (active)}
#define AZ_CONNECTION_STATE_INIT AZ_CONNECTION_STATE_BOUNDS(AZ_CONNECTION_CAPACITY, UINT64_MAX, UINT64_MAX, 32)
struct az_connection_operation {
    struct az_connection_state *state;
    uint64_t sequence;
    bool tracked, eligible, status_eligible, close;
};
static inline void az_connection_state_drop(struct az_connection_state *s) { pthread_mutex_destroy(&s->mutex); }
static inline bool az_connection_lock(struct az_connection_state *s) {
    if (!pthread_mutex_lock(&s->mutex)) return true;
    atomic_store_explicit(&s->saturated, true, memory_order_release);
    return false;
}
static inline void az_connection_unlock(struct az_connection_state *s) {
    if (pthread_mutex_unlock(&s->mutex)) atomic_store_explicit(&s->saturated, true, memory_order_release);
}
static inline void az_connection_retire_locked(struct az_connection_state *s, const void *connection) {
    if (!connection) return;
    for (size_t i = 0; i < s->capacity; ++i) {
        if (s->entries[i].connection == connection) s->entries[i] = (struct az_connection_entry) {0};
        if (s->status_entries[i].connection == connection) s->status_entries[i] = (struct az_connection_entry) {0};
    }
}
static inline bool az_connection_has_capacity(const struct az_connection_state *s) {
    return s->capacity && s->capacity <= AZ_CONNECTION_CAPACITY && s->max_generation && s->max_sequence && s->max_active &&
           s->sequence != s->max_sequence && s->active != s->max_active;
}
static inline void az_connection_enter(struct az_connection_state *s, struct az_connection_operation *op,
                                       const void *connection, bool closing) {
    AZ_NATIVE_ERRNO(saved_errno);
    *op = (struct az_connection_operation) {.state = s};
    if (atomic_load_explicit(&s->saturated, memory_order_acquire)) return;
    if (!az_connection_lock(s)) return;
    if (closing) az_connection_retire_locked(s, connection);
    if (!atomic_load_explicit(&s->saturated, memory_order_acquire)) {
        if (!az_connection_has_capacity(s)) {
            atomic_store_explicit(&s->saturated, true, memory_order_release);
        } else {
            op->eligible = !closing && s->active == 0;
            op->sequence = ++s->sequence;
            op->status_eligible = !closing && !s->closing;
            op->close = closing;
            if (closing) {
                ++s->closing;
                s->last_close = op->sequence;
            }
            op->tracked = true;
            ++s->active;
        }
    }
    az_connection_unlock(s);
}
/* These helpers run with the state lock held. Clearing duplicates precedes
 * both birth contracts, including when native open returned an error. */
static inline bool az_connection_clear_duplicate(struct az_connection_state *s, const void *connection) {
    bool duplicate = false;
    for (size_t i = 0; i < s->capacity; ++i) {
        struct az_connection_entry *entry = &s->entries[i];
        if (entry->connection == connection) {
            /* Two births without retirement are ambiguous. */
            *entry = (struct az_connection_entry) {0};
            duplicate = true;
        }
    }
    return duplicate;
}
static inline bool az_connection_legacy_eligible(const struct az_connection_operation *op) {
    const struct az_connection_state *s = op->state;
    return op->eligible && s->active == 1 && s->sequence == op->sequence &&
           !atomic_load_explicit(&s->saturated, memory_order_acquire);
}
static inline void az_connection_publish_legacy(struct az_connection_state *s, const void *connection, struct az_connection_receipt *receipt) {
    struct az_connection_entry *free_entry = NULL;
    for (size_t i = 0; i < s->capacity; ++i)
        if (!s->entries[i].generation) {
            free_entry = &s->entries[i];
            break;
        }
    /* Capacity refusal never evicts another live incarnation. */
    if (!free_entry) return;
    if (s->next_generation == s->max_generation) atomic_store_explicit(&s->saturated, true, memory_order_release);
    else {
        uint64_t generation = ++s->next_generation;
        *free_entry = (struct az_connection_entry) {connection, generation};
        if (receipt) *receipt = (struct az_connection_receipt) {generation};
    }
}
static inline struct az_connection_entry *az_connection_status_slot(struct az_connection_state *s, const void *connection) {
    struct az_connection_entry *slot = NULL;
    bool duplicate = false;
    for (size_t i = 0; i < s->capacity; ++i) {
        struct az_connection_entry *entry = &s->status_entries[i];
        if (entry->connection == connection) {
            *entry = (struct az_connection_entry) {0};
            duplicate = true;
        }
        if (!entry->connection && !slot) slot = entry;
    }
    return duplicate ? NULL : slot;
}
static inline bool az_connection_status_eligible(const struct az_connection_operation *op) {
    const struct az_connection_state *s = op->state;
    return op->status_eligible && !s->closing && s->last_close < op->sequence &&
           s->next_status_generation < s->max_generation && !atomic_load_explicit(&s->saturated, memory_order_acquire);
}
static inline void az_connection_publish_status(const struct az_connection_operation *op, int result, const void *connection) {
    /* Independent native workers may open distinct libraries concurrently.
     * Status accepts those distinct births, but never an overlapping close.
     * This namespace is separate from the stricter legacy receipt contract. */
    struct az_connection_state *s = op->state;
    struct az_connection_entry *slot = az_connection_status_slot(s, connection);
    if (slot && !result && az_connection_status_eligible(op))
        *slot = (struct az_connection_entry) {connection, ++s->next_status_generation};
}
static inline void az_connection_finish_locked(struct az_connection_operation *op) {
    struct az_connection_state *s = op->state;
    if (op->close) --s->closing;
    --s->active;
    op->tracked = false;
    az_connection_unlock(s);
}
/* Return with the registry locked only for a balanced tracked operation. */
static inline bool az_connection_begin_completion(struct az_connection_operation *op) {
    if (!op->tracked) return false;
    struct az_connection_state *s = op->state;
    if (!az_connection_lock(s)) {
        op->tracked = false;
        return false;
    }
    if (!s->active) {
        atomic_store_explicit(&s->saturated, true, memory_order_release);
        op->tracked = false;
        az_connection_unlock(s);
        return false;
    }
    return true;
}
static inline void az_connection_complete(struct az_connection_operation *op, bool offer, int result,
                                          const void *connection, struct az_connection_receipt *receipt) {
    AZ_NATIVE_ERRNO(saved_errno);
    if (!az_connection_begin_completion(op)) return;
    struct az_connection_state *s = op->state;
    if (offer && connection) {
        bool duplicate = az_connection_clear_duplicate(s, connection);
        if (!result && !duplicate && az_connection_legacy_eligible(op)) az_connection_publish_legacy(s, connection, receipt);
        az_connection_publish_status(op, result, connection);
    }
    az_connection_finish_locked(op);
}
static inline void az_connection_operation_drop(struct az_connection_operation *op) {
    az_connection_complete(op, false, -1, NULL, NULL);
}
/* Native callbacks own their exact argument/result contract. No lock remains
 * held across them. C cleanup releases operation accounting during native
 * unwinding without replacing its exception or errno. */
static inline int az_connection_forward_open(struct az_connection_state *s, const void *native_output,
                                             int (*original)(void *), void *context, struct az_connection_receipt *receipt) {
    struct az_connection_operation op __attribute__((cleanup(az_connection_operation_drop)));
    az_connection_enter(s, &op, NULL, false);
    int result = original(context), native_errno = errno;
    const void *connection = NULL;
    if (native_output) memcpy(&connection, native_output, sizeof connection);
    az_connection_complete(&op, true, result, connection, receipt);
    errno = native_errno;
    return result;
}
/* Entry retirement is unconditional. BUSY/error/zombie/exception returns never
 * revive it. Completion only touches this operation's counter. */
static inline int az_connection_forward_close(struct az_connection_state *s, const void *connection,
                                              int (*original)(void *), void *context) {
    struct az_connection_operation op __attribute__((cleanup(az_connection_operation_drop)));
    az_connection_enter(s, &op, connection, true);
    int result = original(context), native_errno = errno;
    az_connection_complete(&op, false, result, NULL, NULL);
    errno = native_errno;
    return result;
}
/* Scalar observation acquires no native liveness. Unknown, active, retired and
 * saturated reads leave the output unchanged. No read creates a birth. */
static inline bool az_connection_read(struct az_connection_state *s, const void *connection, struct az_connection_receipt *receipt) {
    AZ_NATIVE_ERRNO(saved_errno);
    if (!connection || atomic_load_explicit(&s->saturated, memory_order_acquire) || !az_connection_lock(s)) return false;
    bool found = false;
    if (!s->active && !atomic_load_explicit(&s->saturated, memory_order_acquire))
        for (size_t i = 0; i < s->capacity; ++i)
            if (s->entries[i].generation && s->entries[i].connection == connection) {
                *receipt = (struct az_connection_receipt) {s->entries[i].generation};
                found = true;
                break;
            }
    az_connection_unlock(s);
    return found;
}
static inline uint64_t az_connection_status_generation(struct az_connection_state *s, const void *connection) {
    AZ_NATIVE_ERRNO(saved_errno);
    if (!connection || atomic_load_explicit(&s->saturated, memory_order_acquire) || !az_connection_lock(s)) return 0;
    uint64_t generation = 0;
    if (!atomic_load_explicit(&s->saturated, memory_order_acquire))
        for (size_t i = 0; i < s->capacity; ++i)
            if (s->status_entries[i].connection == connection) {
                generation = s->status_entries[i].generation;
                break;
            }
    az_connection_unlock(s);
    return generation;
}
#endif

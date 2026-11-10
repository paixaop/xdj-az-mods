/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded command policy, without native entry points. Drive scmd_step only on an
 * adapter's verified owning thread. Public operations never wait or allocate. */
#ifndef AZMOD_SDK_COMMAND_ENGINE_H
#define AZMOD_SDK_COMMAND_ENGINE_H
#include "../common/aztimestamp.h"
#include "../common/azexit.h"
#include "azmod-mod.h"
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define SCMD_DECKS 4
#define SCMD_OWNERS 32
#define SCMD_SLOTS 32
#define SCMD_PER_OWNER 4
#define SCMD_EVENTS 64
#define SCMD_ALL_COMMANDS ((UINT64_C(1) << (AZ_CMD_CUE_LOOP_SET + 1)) - 2)
#define SCMD_TARGET_LOAD 1u
#define SCMD_TARGET_REVISION 2u
#ifdef __cplusplus
static_assert(AZ_CMD_CUE_LOOP_SET < 63, "command IDs must fit the capability mask");
static_assert(offsetof(az_command_request, comment) + sizeof(((az_command_request *) 0)->comment) == sizeof(az_command_request), "command request grew: update validation and retry comparison");
#else
_Static_assert(AZ_CMD_CUE_LOOP_SET < 63, "command IDs must fit the capability mask");
_Static_assert(offsetof(az_command_request, comment) + sizeof(((az_command_request *) 0)->comment) == sizeof(az_command_request), "command request grew: update validation and retry comparison");
#endif

enum scmd_detail {
    SCMD_DETAIL_NONE,
    SCMD_DETAIL_ARGUMENTS,
    SCMD_DETAIL_ID_CONFLICT,
    SCMD_DETAIL_ID_ORDER,
    SCMD_DETAIL_LOAD,
    SCMD_DETAIL_REVISION,
    SCMD_DETAIL_CAPABILITY,
    SCMD_DETAIL_PERSISTENCE,
    SCMD_DETAIL_DEADLINE,
    SCMD_DETAIL_QUOTA,
    SCMD_DETAIL_CLOCK,
    SCMD_DETAIL_BACKEND
};
enum scmd_phase {
    SCMD_FREE,
    SCMD_QUEUED,
    SCMD_PREPARING,
    SCMD_DISPATCHED,
    SCMD_FINAL
};

/* A copied, operation-appropriate observation. begin must keep its native source
 * alive and coherent until end; a cached pointer or an LTI revision is insufficient. */
struct scmd_target {
    uint64_t load_generation, revision, commands;
    uint64_t revision_commands, persistent_commands;
    uint32_t valid;
    int32_t loaded;
};
struct scmd_completion {
    uint32_t outcome;
    int32_t reason;
    uint64_t load_generation, revision;
    uint32_t settled; /* 1 proves this token can no longer execute in the native queue. */
};
struct scmd_adapter {
    void *context;
    /* Pure, bounded validation; called with the engine lock held on either thread. */
    int (*arguments)(void *, const az_command_request *, const struct scmd_target *);
    /* AZ_OK acquires the adapter endpoint lease, paired with exactly one end.
     * Deferred adapters queue scalars only and acquire the actual native
     * owner/load lifetime at execution before following native pointers. */
    int (*begin)(void *, const az_command_request *, struct scmd_target *);
    /* AZ_PENDING means dispatched without proven completion. AZ_OK supplies explicit
     * completion evidence. Other returns are uncertain and keep the deck barrier. */
    int (*dispatch)(void *, const az_command_request *, uint64_t, struct scmd_completion *);
    void (*end)(void *, uint32_t);
};
struct scmd_record {
    uint64_t token, execution_revision, owner_lifetime, native_lifetime;
    int owner, phase, pinned, awaiting;
    int asynchronous, acknowledged, terminal_delivered, callback_active;
    int retired, native_submitted, settled, native_retired, deadline_fired;
    az_command_completion_fn callback;
    void *callback_user;
    enum scmd_detail detail;
    az_command_request request;
    az_command_result result;
};
/* A released notification can outlive the primary command record. This is a
 * reservation identity, not a second command slot: it keeps the callback,
 * user pointer, immutable result and owner/ID retry identity until RELEASED
 * returns. The bounded array is part of the admission quota. */
struct scmd_pending_delivery {
    int occupied, owner, acknowledged, callback_active, released_delivered;
    uint64_t token, owner_lifetime;
    az_command_request request;
    az_command_result result;
    az_command_completion_fn callback;
    void *callback_user;
};
struct scmd_change {
    uint32_t size, version;
    uint64_t sequence, token;
    uint32_t missed, settled;
    az_command_result result;
};
struct scmd_journal_entry {
    int owner;
    uint64_t owner_lifetime;
    struct scmd_change value;
};
struct scmd_engine {
    pthread_mutex_t mu;
    uint64_t next_token, sequence, highest_id[SCMD_OWNERS], active[SCMD_DECKS];
    uint64_t (*clock)(void *); /* Process lifetime; bounded, no waiting or engine reentry. */
    void *clock_context;
    const struct azexit_delivery_v2 *delivery;
    uint64_t owner_lifetime[SCMD_OWNERS];
    unsigned char owner_closed[SCMD_OWNERS], owner_initializing[SCMD_OWNERS];
    struct scmd_record records[SCMD_SLOTS];
    struct scmd_pending_delivery pending[SCMD_SLOTS];
    struct scmd_journal_entry events[SCMD_EVENTS];
};
struct scmd_notification {
    az_command_completion_fn callback;
    void *user;
    uint64_t token;
    uint32_t kind;
    az_command_completion completion;
};
static inline uint64_t scmd_monotonic(void *unused) {
    (void) unused;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) now.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) now.tv_nsec;
}
#define SCMD_ENGINE_INIT {.mu = PTHREAD_MUTEX_INITIALIZER, .clock = scmd_monotonic}
static inline uint32_t scmd_size(const void *record) {
    uint32_t size;
    memcpy(&size, record, sizeof size);
    return size;
}
static inline int scmd_owner_ok(int owner) { return owner >= 0 && owner < SCMD_OWNERS; }
static inline int scmd_error_status(int status) { return status >= AZ_NOT_FOUND && status <= AZ_UNSUPPORTED ? status : AZ_FAILED; }
static inline int scmd_output_ok(const az_command_result *out) { return out && scmd_size(out) >= sizeof *out; }
static inline uint64_t scmd_now(struct scmd_engine *e) { return e->clock ? e->clock(e->clock_context) : 0; }
static inline void scmd_copy_result(az_command_result *out, const az_command_result *value) {
    az_command_result copy = *value;
    copy.size = scmd_size(out);
    memcpy(out, &copy, sizeof copy);
}
static inline int scmd_request_valid(const az_command_request *r) {
    return r && scmd_size(r) >= sizeof *r && r->version == AZ_DATA_VERSION &&
           r->operation >= AZ_CMD_LOOP_IN && r->operation <= AZ_CMD_CUE_LOOP_SET && r->deck < SCMD_DECKS &&
           r->request_id && r->expected_load_generation && r->deadline_ns && r->quantization <= AZ_QUANTIZE_ON &&
           !(r->flags & ~(AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS)) &&
           (!(r->flags & AZ_COMMAND_NATIVE_CUE_IDS) || ((r->flags & AZ_COMMAND_NATIVE_LOAD) && r->operation > AZ_CMD_LOOP_ADJUST_OUT && r->expected_revision)) &&
           (!(r->flags & AZ_COMMAND_NATIVE_LOAD) ||
            (r->operation > AZ_CMD_LOOP_ADJUST_OUT ||
             (!(r->flags & AZ_COMMAND_PERSIST) && !r->expected_revision))) &&
           !r->reserved && r->classification <= AZ_CUE_MEMORY &&
           !(r->rgb & UINT32_C(0xff000000)) && isfinite(r->position_s) && isfinite(r->end_s) && isfinite(r->beats) &&
           memchr(r->comment, 0, sizeof r->comment);
}
static inline void scmd_canonical_request(az_command_request *out, const az_command_request *r) {
    *out = *r;
    out->size = sizeof *out;
    size_t n = strlen(out->comment);
    memset(out->comment + n, 0, sizeof out->comment - n);
}
/* Request IDs bind canonical encoded arguments before a source clock exists.
 * Preserve finite binary64 inputs and normalize the two zero encodings. The
 * native adapter separately checks temporal effects using its actual clock. */
static inline uint64_t scmd_argument_encoding(double value) {
    uint64_t encoded;
    memcpy(&encoded, &value, sizeof encoded);
    if (!(encoded & UINT64_C(0x7fffffffffffffff))) encoded = 0;
    return encoded;
}
/* Compare the declared request fields, excluding padding and unknown tails. */
static inline int scmd_request_equal(const az_command_request *a, const az_command_request *b) {
    int deadline_order = compare_timestamps(a->deadline_ns, b->deadline_ns, 1000000000, 1000000000, 1);
    if (deadline_order == AZ_TIME_INVALID) return 0;
    return a->operation == b->operation && a->deck == b->deck && a->request_id == b->request_id &&
           a->expected_load_generation == b->expected_load_generation && a->expected_revision == b->expected_revision &&
           deadline_order == AZ_TIME_EQUAL && a->quantization == b->quantization && a->flags == b->flags &&
           a->classification == b->classification && a->entry_id == b->entry_id &&
           scmd_argument_encoding(a->position_s) == scmd_argument_encoding(b->position_s) &&
           scmd_argument_encoding(a->end_s) == scmd_argument_encoding(b->end_s) &&
           a->beats == b->beats && a->rgb == b->rgb && !strcmp(a->comment, b->comment);
}
static inline struct scmd_record *scmd_find_id(struct scmd_engine *e, int owner, uint64_t id) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE && e->records[i].owner == owner &&
            e->records[i].owner_lifetime == e->owner_lifetime[owner] && e->records[i].request.request_id == id) return &e->records[i];
    return NULL;
}
static inline struct scmd_pending_delivery *scmd_find_pending_id(struct scmd_engine *e, int owner, uint64_t id) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->pending[i].occupied && e->pending[i].owner == owner &&
            e->pending[i].owner_lifetime == e->owner_lifetime[owner] && e->pending[i].request.request_id == id) return &e->pending[i];
    return NULL;
}
static inline struct scmd_pending_delivery *scmd_find_pending_token(struct scmd_engine *e, uint64_t token) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->pending[i].occupied && e->pending[i].token == token) return &e->pending[i];
    return NULL;
}
static inline unsigned scmd_owner_reservations(struct scmd_engine *e, int owner) {
    unsigned count = 0;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        if (e->records[i].phase != SCMD_FREE && e->records[i].owner == owner) count++;
        if (e->pending[i].occupied && e->pending[i].owner == owner) count++;
    }
    return count;
}
static inline unsigned scmd_total_reservations(struct scmd_engine *e) {
    unsigned count = 0;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        if (e->records[i].phase != SCMD_FREE) count++;
        if (e->pending[i].occupied) count++;
    }
    return count;
}
static inline struct scmd_pending_delivery *scmd_pending_free(struct scmd_engine *e) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (!e->pending[i].occupied) return &e->pending[i];
    return NULL;
}
static inline struct scmd_record *scmd_find_token(struct scmd_engine *e, uint64_t token) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE && e->records[i].token == token) return &e->records[i];
    return NULL;
}
static inline void scmd_clear_barrier(struct scmd_engine *e, const struct scmd_record *r) {
    if (!r->pinned && !r->awaiting && e->active[r->request.deck] == r->token) e->active[r->request.deck] = 0;
}
static inline void scmd_publish(struct scmd_engine *e, const struct scmd_record *r) {
    uint64_t sequence = ++e->sequence;
    struct scmd_journal_entry *entry = &e->events[(sequence - 1) % SCMD_EVENTS];
    memset(entry, 0, sizeof *entry);
    entry->owner = r->owner;
    entry->owner_lifetime = r->owner_lifetime;
    entry->value.size = sizeof entry->value;
    entry->value.version = AZ_DATA_VERSION;
    entry->value.sequence = sequence;
    entry->value.token = r->token;
    entry->value.settled = r->phase == SCMD_FINAL && (e->delivery ? r->settled : !r->awaiting);
    entry->value.result = r->result;
}
static inline void scmd_finish(struct scmd_engine *e, struct scmd_record *r, uint32_t outcome, int reason,
                               uint64_t revision, enum scmd_detail detail, uint64_t now, int awaiting) {
    r->phase = SCMD_FINAL;
    r->awaiting = awaiting;
    r->detail = detail;
    r->result.outcome = outcome;
    r->result.reason = reason;
    r->result.revision = revision;
    r->result.observed_ns = now;
    scmd_clear_barrier(e, r);
    scmd_publish(e, r);
}
static inline int scmd_target_valid(const az_command_request *r, const struct scmd_target *t,
                                    const struct scmd_adapter *adapter, enum scmd_detail *detail) {
    uint64_t bit = UINT64_C(1) << r->operation;
    if (!t || !(t->commands & bit)) {
        *detail = SCMD_DETAIL_CAPABILITY;
        return AZ_UNSUPPORTED;
    }
    if ((t->persistent_commands & bit) && !(r->flags & AZ_COMMAND_PERSIST)) {
        *detail = SCMD_DETAIL_PERSISTENCE;
        return AZ_INVALID;
    }
    if (!(t->valid & SCMD_TARGET_LOAD) || t->loaded != 1 || !t->load_generation || t->load_generation != r->expected_load_generation) {
        *detail = SCMD_DETAIL_LOAD;
        return AZ_STALE;
    }
    if ((t->revision_commands & bit) && !r->expected_revision) {
        *detail = SCMD_DETAIL_REVISION;
        return AZ_INVALID;
    }
    if (r->expected_revision && (!(t->valid & SCMD_TARGET_REVISION) || !t->revision || t->revision != r->expected_revision)) {
        *detail = SCMD_DETAIL_REVISION;
        return AZ_STALE;
    }
    int rc = adapter->arguments(adapter->context, r, t);
    if (rc != AZ_OK) {
        *detail = SCMD_DETAIL_ARGUMENTS;
        return scmd_error_status(rc);
    }
    *detail = SCMD_DETAIL_NONE;
    return AZ_OK;
}
/* A rejection was never accepted, so it neither consumes the ID nor needs retention. */
static inline int scmd_refuse(az_command_result *out, const az_command_request *r, int reason, uint64_t now) {
    az_command_result value = {sizeof value, AZ_DATA_VERSION, AZ_COMMAND_REJECTED, reason, r->request_id, 0, now};
    scmd_copy_result(out, &value);
    return reason;
}
static inline int scmd_admit(struct scmd_engine *e, int owner, const az_command_request *request,
                             az_command_completion_fn callback, void *user,
                             const struct scmd_target *target, const struct scmd_adapter *adapter,
                             az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (detail_out) *detail_out = SCMD_DETAIL_ARGUMENTS;
    if (!e || !scmd_owner_ok(owner) || !scmd_output_ok(out)) return AZ_INVALID;
    az_command_request r;
    scmd_canonical_request(&r, request);
    if (e->delivery ? pthread_mutex_lock(&e->mu) : pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    if (e->owner_closed[owner]) {
        pthread_mutex_unlock(&e->mu);
        return AZ_INVALID;
    }
    struct scmd_record *existing = scmd_find_id(e, owner, r.request_id);
    struct scmd_pending_delivery *pending = existing ? NULL : scmd_find_pending_id(e, owner, r.request_id);
    if (existing || pending) {
        const az_command_request *saved = existing ? &existing->request : &pending->request;
        az_command_completion_fn saved_callback = existing ? existing->callback : pending->callback;
        void *saved_user = existing ? existing->callback_user : pending->callback_user;
        if (!scmd_request_equal(saved, &r) || saved_callback != callback || saved_user != user) {
            if (detail_out) *detail_out = SCMD_DETAIL_ID_CONFLICT;
            pthread_mutex_unlock(&e->mu);
            return AZ_INVALID;
        }
        if (detail_out) *detail_out = existing ? existing->detail : SCMD_DETAIL_NONE;
        scmd_copy_result(out, existing ? &existing->result : &pending->result);
        pthread_mutex_unlock(&e->mu);
        return AZ_OK;
    }
    uint64_t now = scmd_now(e);
    int deadline_order = compare_timestamps(r.deadline_ns, now, 1000000000, 1000000000, 1);
    enum scmd_detail detail = SCMD_DETAIL_NONE;
    int rc = AZ_OK;
    if (r.request_id <= e->highest_id[owner]) {
        rc = AZ_STALE;
        detail = SCMD_DETAIL_ID_ORDER;
    } else if (!adapter || !adapter->arguments || !adapter->begin || !adapter->dispatch || !adapter->end) {
        rc = AZ_UNSUPPORTED;
        detail = SCMD_DETAIL_BACKEND;
    } else if (!now || deadline_order == AZ_TIME_INVALID) {
        rc = AZ_FAILED;
        detail = SCMD_DETAIL_CLOCK;
    } else if (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL) {
        rc = AZ_STALE;
        detail = SCMD_DETAIL_DEADLINE;
    }
    struct scmd_target observed;
    if (rc == AZ_OK && e->delivery && !target) {
        memset(&observed, 0, sizeof observed);
        int status = adapter->begin(adapter->context, &r, &observed);
        target = &observed;
        target_pending = status == AZ_BUSY || status == AZ_PENDING;
        if (status != AZ_OK && !target_pending) {
            rc = scmd_error_status(status);
            detail = status == AZ_STALE ? SCMD_DETAIL_LOAD : SCMD_DETAIL_BACKEND;
        }
    }
    if (rc == AZ_OK && target_pending) {
        /* A busy native lookup supplies no revision evidence. Retain the copied
         * request, then validate a fresh target before dispatch, using the same
         * deadline. The advertised capability still gates admission. */
        if (!target || !(target->commands & (UINT64_C(1) << r.operation))) {
            rc = AZ_UNSUPPORTED;
            detail = SCMD_DETAIL_CAPABILITY;
        } else if ((target->valid & SCMD_TARGET_LOAD) &&
                   (target->loaded != 1 || !target->load_generation || target->load_generation != r.expected_load_generation)) {
            rc = AZ_STALE;
            detail = SCMD_DETAIL_LOAD;
        }
    } else if (rc == AZ_OK) rc = scmd_target_valid(&r, target, adapter, &detail);
    struct scmd_record *slot = NULL;
    if (rc == AZ_OK) {
        unsigned count = 0;
        for (unsigned i = 0; i < SCMD_SLOTS; i++) {
            if (e->records[i].phase == SCMD_FREE) slot = &e->records[i];
        }
        count = scmd_owner_reservations(e, owner);
        /* Leave enough journal sequence values for every outstanding terminal event. */
        if (!slot || count >= SCMD_PER_OWNER || scmd_total_reservations(e) >= SCMD_SLOTS || e->next_token >= (UINT64_MAX >> 3) || e->sequence >= UINT64_MAX - SCMD_SLOTS) {
            rc = AZ_LIMIT;
            detail = SCMD_DETAIL_QUOTA;
        }
    }
    if (rc == AZ_OK && e->delivery) {
        struct azexit_request identity = {e->next_token + 1, r.expected_load_generation, r.deadline_ns, r.deck, 0};
        rc = e->delivery->reserve(&identity);
    }
    if (rc != AZ_OK) {
        if (detail_out) *detail_out = detail;
        if (rc != AZ_INVALID && rc != AZ_BUSY) scmd_refuse(out, &r, rc, now);
        pthread_mutex_unlock(&e->mu);
        return rc;
    }
    memset(slot, 0, sizeof *slot);
    slot->token = ++e->next_token;
    slot->owner = owner;
    slot->owner_lifetime = e->owner_lifetime[owner];
    slot->phase = SCMD_QUEUED;
    slot->awaiting = e->delivery != NULL;
    slot->asynchronous = callback != NULL;
    slot->callback = callback;
    slot->callback_user = user;
    slot->request = r;
    slot->result = (az_command_result) {sizeof slot->result, AZ_DATA_VERSION, AZ_COMMAND_ACCEPTED, AZ_PENDING, r.request_id, 0, now};
    e->highest_id[owner] = r.request_id;
    scmd_publish(e, slot);
    scmd_copy_result(out, &slot->result);
    if (detail_out) *detail_out = SCMD_DETAIL_NONE;
    pthread_mutex_unlock(&e->mu);
    return AZ_OK;
}
static inline int scmd_move_to_pending_locked(struct scmd_engine *e, struct scmd_record *r) {
    if (!e || !r || r->phase != SCMD_FINAL || !r->asynchronous || !r->terminal_delivered ||
        !r->acknowledged || r->awaiting || r->pinned || r->callback_active || r->retired) return 0;
    struct scmd_pending_delivery *p = scmd_pending_free(e);
    if (!p) return 0;
    memset(p, 0, sizeof *p);
    p->occupied = 1;
    p->owner = r->owner;
    p->owner_lifetime = r->owner_lifetime;
    p->acknowledged = 1;
    p->token = r->token;
    p->request = r->request;
    p->result = r->result;
    p->callback = r->callback;
    p->callback_user = r->callback_user;
    memset(r, 0, sizeof *r);
    return 1;
}
static inline int scmd_submit_target(struct scmd_engine *e, int owner, const az_command_request *request,
                                     const struct scmd_target *target, const struct scmd_adapter *adapter,
                                     az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (!scmd_request_valid(request)) {
        if (detail_out) *detail_out = SCMD_DETAIL_ARGUMENTS;
        return AZ_INVALID;
    }
    return scmd_admit(e, owner, request, NULL, NULL, target, adapter, out, detail_out, target_pending);
}
static inline int scmd_submit(struct scmd_engine *e, int owner, const az_command_request *request,
                              const struct scmd_target *target, const struct scmd_adapter *adapter,
                              az_command_result *out, enum scmd_detail *detail_out) {
    return scmd_submit_target(e, owner, request, target, adapter, out, detail_out, 0);
}
static inline int scmd_submit_async(struct scmd_engine *e, int owner, const az_command_request *request,
                                    az_command_completion_fn callback, void *user,
                                    const struct scmd_target *target, const struct scmd_adapter *adapter,
                                    az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (!callback || !scmd_request_valid(request)) return AZ_INVALID;
    return scmd_admit(e, owner, request, callback, user, target, adapter, out, detail_out, target_pending);
}
static inline int scmd_owner_deliverable(struct scmd_engine *e, int owner, uint64_t lifetime) {
    return !e->owner_closed[owner] && !e->owner_initializing[owner] &&
           lifetime == e->owner_lifetime[owner];
}
static inline int scmd_acknowledge(struct scmd_engine *e, int owner, uint64_t id) {
    if (!e || !scmd_owner_ok(owner) || !id) return AZ_INVALID;
    /* A valid terminal acknowledgement is a lifecycle transition, not a
     * best-effort query.  Serialize briefly so it cannot report AZ_BUSY and
     * strand the RELEASED obligation. */
    pthread_mutex_lock(&e->mu);
    struct scmd_record *r = scmd_find_id(e, owner, id);
    int rc = AZ_STALE;
    if (r) {
        if (!r->asynchronous || r->phase != SCMD_FINAL) rc = AZ_INVALID;
        else {
            r->acknowledged = 1;
            (void) scmd_move_to_pending_locked(e, r);
            rc = AZ_OK;
        }
    } else if (scmd_find_pending_id(e, owner, id)) rc = AZ_OK;
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* Reserve the next callback identity while holding the store lock, then invoke it
 * after the lock is released.  This is the only path that turns native evidence
 * into a public callback. */
static inline int scmd_next_notification(struct scmd_engine *e, struct scmd_notification *out) {
    if (!e || !out) return AZ_INVALID;
    pthread_mutex_lock(&e->mu);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &e->records[i];
        if (r->phase != SCMD_FINAL || !r->asynchronous || !r->callback || r->callback_active ||
            r->terminal_delivered || r->retired || !scmd_owner_deliverable(e, r->owner, r->owner_lifetime))
            continue;
        memset(out, 0, sizeof *out);
        out->callback = r->callback;
        out->user = r->callback_user;
        out->token = r->token;
        out->kind = AZ_COMMAND_COMPLETION_TERMINAL;
        out->completion.size = sizeof out->completion;
        out->completion.version = AZ_DATA_VERSION;
        out->completion.kind = AZ_COMMAND_COMPLETION_TERMINAL;
        out->completion.settled = e->delivery ? r->settled : !r->awaiting;
        out->completion.result = r->result;
        r->terminal_delivered = 1;
        r->callback_active = 1;
        pthread_mutex_unlock(&e->mu);
        return AZ_OK;
    }
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_pending_delivery *p = &e->pending[i];
        if (!p->occupied || !p->callback || p->callback_active || p->released_delivered ||
            !scmd_owner_deliverable(e, p->owner, p->owner_lifetime)) continue;
        p->released_delivered = 1;
        p->callback_active = 1;
        memset(out, 0, sizeof *out);
        out->callback = p->callback;
        out->user = p->callback_user;
        out->token = p->token;
        out->kind = AZ_COMMAND_COMPLETION_RELEASED;
        out->completion.size = sizeof out->completion;
        out->completion.version = AZ_DATA_VERSION;
        out->completion.kind = AZ_COMMAND_COMPLETION_RELEASED;
        out->completion.settled = 1;
        out->completion.result = p->result;
        pthread_mutex_unlock(&e->mu);
        return AZ_OK;
    }
    pthread_mutex_unlock(&e->mu);
    return AZ_EMPTY;
}
static inline void scmd_notification_done(struct scmd_engine *e, const struct scmd_notification *notification) {
    if (!e || !notification) return;
    pthread_mutex_lock(&e->mu);
    struct scmd_record *r = scmd_find_token(e, notification->token);
    if (r) {
        r->callback_active = 0;
        (void) scmd_move_to_pending_locked(e, r);
    } else {
        struct scmd_pending_delivery *p = scmd_find_pending_token(e, notification->token);
        if (p && notification->kind == AZ_COMMAND_COMPLETION_RELEASED) {
            p->callback_active = 0;
            if (p->released_delivered) memset(p, 0, sizeof *p);
        }
    }
    pthread_mutex_unlock(&e->mu);
}
static inline int scmd_poll(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
    if (!e || !scmd_owner_ok(owner) || !id || !scmd_output_ok(out)) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, id);
    int found = r != NULL;
    if (r) scmd_copy_result(out, &r->result);
    else {
        struct scmd_pending_delivery *p = scmd_find_pending_id(e, owner, id);
        if (p) {
            scmd_copy_result(out, &p->result);
            found = 1;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return found ? AZ_OK : AZ_STALE;
}
static inline int scmd_cancel(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
    if (!e || !scmd_owner_ok(owner) || !id || !scmd_output_ok(out)) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, id);
    int rc = AZ_STALE;
    if (r) {
        if (r->phase == SCMD_QUEUED || r->phase == SCMD_PREPARING) {
            uint64_t now = scmd_now(e);
            if (!now) {
                pthread_mutex_unlock(&e->mu);
                return AZ_FAILED;
            }
            scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0, SCMD_DETAIL_NONE, now, 0);
            rc = AZ_OK;
        } else if (r->phase == SCMD_DISPATCHED || r->awaiting) rc = AZ_PENDING;
        else if (r->result.outcome == AZ_COMMAND_CANCELLED) rc = AZ_OK;
        scmd_copy_result(out, &r->result);
    } else {
        struct scmd_pending_delivery *p = scmd_find_pending_id(e, owner, id);
        if (p) {
            scmd_copy_result(out, &p->result);
            rc = p->result.outcome == AZ_COMMAND_CANCELLED ? AZ_OK : AZ_STALE;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* Explicit acknowledgement: retained retries stay exact until release. A released
 * ID remains below the owner's high-water mark and can never execute again. */
static inline int scmd_release(struct scmd_engine *e, int owner, uint64_t id) {
    if (!e || !scmd_owner_ok(owner) || !id) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, id);
    int rc = !r ? (scmd_find_pending_id(e, owner, id) ? AZ_INVALID : AZ_STALE) : r->asynchronous                                  ? AZ_INVALID
                                                                             : r->phase != SCMD_FINAL || r->pinned || r->awaiting ? AZ_BUSY
                                                                                                                                  : AZ_OK;
    if (rc == AZ_OK) memset(r, 0, sizeof *r);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static inline int scmd_completion_valid(const struct scmd_completion *c) {
    if (!c || c->settled > 1) return 0;
    if (c->outcome == AZ_COMMAND_APPLIED) return c->reason == AZ_OK && c->revision && c->load_generation && c->settled;
    if (c->revision) return 0;
    if (c->outcome == AZ_COMMAND_CANCELLED) return c->reason == AZ_CANCELLED && c->settled;
    if (c->outcome == AZ_COMMAND_REJECTED) return c->reason >= AZ_NOT_FOUND && c->reason <= AZ_UNSUPPORTED && c->reason != AZ_CANCELLED && c->settled;
    return c->outcome == AZ_COMMAND_UNCERTAIN && c->reason >= AZ_NOT_FOUND && c->reason <= AZ_UNSUPPORTED;
}
static inline int scmd_complete_locked(struct scmd_engine *e, struct scmd_record *r, const struct scmd_completion *c, uint64_t now) {
    if (r->phase == SCMD_FINAL) {
        if (r->result.outcome != AZ_COMMAND_UNCERTAIN || !r->awaiting) return AZ_STALE;
        if (!scmd_completion_valid(c)) return AZ_INVALID;
        if (!c->settled) return AZ_PENDING;
        r->awaiting = 0;
        scmd_clear_barrier(e, r);
        (void) scmd_move_to_pending_locked(e, r);
        return AZ_OK; /* The published terminal uncertainty remains immutable. */
    }
    if (r->phase != SCMD_DISPATCHED) return AZ_STALE;
    if (!now || !scmd_completion_valid(c)) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0, now ? SCMD_DETAIL_BACKEND : SCMD_DETAIL_CLOCK, now, 1);
        return AZ_INVALID;
    }
    if (c->outcome == AZ_COMMAND_APPLIED && c->load_generation != r->request.expected_load_generation) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_STALE, 0, SCMD_DETAIL_LOAD, now, 0);
        return AZ_STALE;
    }
    if (c->outcome == AZ_COMMAND_APPLIED && c->revision < r->execution_revision) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_STALE, 0, SCMD_DETAIL_REVISION, now, 0);
        return AZ_STALE;
    }
    scmd_finish(e, r, c->outcome, c->reason, c->revision,
                c->outcome == AZ_COMMAND_UNCERTAIN ? SCMD_DETAIL_BACKEND : SCMD_DETAIL_NONE,
                now, !c->settled);
    return AZ_OK;
}
/* Trusted adapter completion only. A module consumer must never receive this entry. */
static inline int scmd_complete(struct scmd_engine *e, uint64_t token, const struct scmd_completion *completion) {
    if (!e || !token || !completion) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_token(e, token);
    int rc = r ? scmd_complete_locked(e, r, completion, scmd_now(e)) : AZ_STALE;
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* One request for one deck. begin/end and dispatch are outside the lock; pinned
 * prevents release, and active prevents both native overlap and FIFO overtaking. */
static inline int scmd_step(struct scmd_engine *e, unsigned deck, const struct scmd_adapter *adapter) {
    if (!e || deck >= SCMD_DECKS || !adapter || !adapter->arguments || !adapter->begin || !adapter->dispatch || !adapter->end) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    if (e->active[deck]) {
        pthread_mutex_unlock(&e->mu);
        return AZ_BUSY;
    }
    struct scmd_record *r = NULL;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        struct scmd_record *candidate = &e->records[i];
        if (candidate->phase == SCMD_QUEUED && candidate->request.deck == deck && (!r || candidate->token < r->token)) r = candidate;
    }
    if (!r) {
        pthread_mutex_unlock(&e->mu);
        return AZ_EMPTY;
    }
    uint64_t now = scmd_now(e);
    if (!now) {
        pthread_mutex_unlock(&e->mu);
        return AZ_FAILED;
    }
    int deadline_order = compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1);
    if (deadline_order == AZ_TIME_INVALID) {
        pthread_mutex_unlock(&e->mu);
        return AZ_FAILED;
    }
    if (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL) {
        scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, 0);
        pthread_mutex_unlock(&e->mu);
        return AZ_OK;
    }
    r->phase = SCMD_PREPARING;
    r->pinned = 1;
    e->active[deck] = r->token;
    az_command_request request = r->request;
    uint64_t token = r->token;
    pthread_mutex_unlock(&e->mu);
    struct scmd_target target;
    memset(&target, 0, sizeof target);
    int rc = adapter->begin(adapter->context, &request, &target);
    int leased = rc == AZ_OK, send = 0;
    pthread_mutex_lock(&e->mu);
    r = scmd_find_token(e, token); /* A pinned record cannot have been released. */
    if (r->phase != SCMD_FINAL) {
        if (rc == AZ_BUSY || rc == AZ_PENDING || rc == AZ_INVALID) r->phase = SCMD_QUEUED;
        else if (rc != AZ_OK) scmd_finish(e, r, AZ_COMMAND_REJECTED, scmd_error_status(rc), 0, SCMD_DETAIL_BACKEND, scmd_now(e), 0);
        else {
            enum scmd_detail detail;
            rc = scmd_target_valid(&request, &target, adapter, &detail);
            now = scmd_now(e); /* Recheck after owner-thread validation, not before it. */
            deadline_order = compare_timestamps(request.deadline_ns, now, 1000000000, 1000000000, 1);
            if (!now || deadline_order == AZ_TIME_INVALID) {
                rc = AZ_FAILED;
                detail = SCMD_DETAIL_CLOCK;
            }
            if (now && (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL)) scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, 0);
            else if (rc != AZ_OK) scmd_finish(e, r, AZ_COMMAND_REJECTED, rc, 0, detail, now, 0);
            else {
                r->phase = SCMD_DISPATCHED;
                r->awaiting = 1;
                r->execution_revision = target.valid & SCMD_TARGET_REVISION ? target.revision : 0;
                send = 1; /* Cancellation after this claim cannot promise no action. */
            }
        }
    }
    pthread_mutex_unlock(&e->mu);
    struct scmd_completion completion;
    memset(&completion, 0, sizeof completion);
    int dispatched = send ? adapter->dispatch(adapter->context, &request, token, &completion) : AZ_OK;
    if (leased) adapter->end(adapter->context, deck);
    pthread_mutex_lock(&e->mu);
    r = scmd_find_token(e, token);
    if (send) {
        if (dispatched == AZ_OK) (void) scmd_complete_locked(e, r, &completion, scmd_now(e));
        else if (dispatched != AZ_PENDING && r->phase != SCMD_FINAL)
            scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, scmd_error_status(dispatched), 0, SCMD_DETAIL_BACKEND, scmd_now(e), 1);
    }
    r->pinned = 0;
    if (r->phase == SCMD_QUEUED || (r->phase == SCMD_FINAL && !r->awaiting)) scmd_clear_barrier(e, r);
    int pending = r->phase == SCMD_DISPATCHED;
    int queued = r->phase == SCMD_QUEUED;
    pthread_mutex_unlock(&e->mu);
    return send ? pending ? AZ_PENDING : AZ_OK : queued ? rc
                                                        : AZ_OK;
}
/* Clock-driven safety sweep, never a native action. In-flight timeout is uncertainty,
 * and its deck stays fenced until the adapter acknowledges settled execution. */
static inline int scmd_expire(struct scmd_engine *e) {
    if (!e) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    uint64_t now = scmd_now(e);
    if (!now) {
        pthread_mutex_unlock(&e->mu);
        return AZ_FAILED;
    }
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        struct scmd_record *r = &e->records[i];
        if (r->phase == SCMD_FREE || r->phase == SCMD_FINAL) continue;
        int order = compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1);
        if (order == AZ_TIME_INVALID) {
            pthread_mutex_unlock(&e->mu);
            return AZ_FAILED;
        }
        if (order == AZ_TIME_AFTER) continue;
        int dispatched = r->phase == SCMD_DISPATCHED;
        scmd_finish(e, r, dispatched ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, dispatched);
    }
    pthread_mutex_unlock(&e->mu);
    return AZ_OK;
}
static inline int scmd_event(struct scmd_engine *e, int owner, uint64_t after, struct scmd_change *out) {
    if (!e || !scmd_owner_ok(owner) || !out || scmd_size(out) < sizeof *out || after == UINT64_MAX) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    if (after > e->sequence) {
        pthread_mutex_unlock(&e->mu);
        return AZ_INVALID;
    }
    uint64_t first = e->sequence >= SCMD_EVENTS ? e->sequence - SCMD_EVENTS + 1 : 1;
    int missed = after < first - 1;
    for (uint64_t sequence = after < first ? first : after + 1; sequence && sequence <= e->sequence; sequence++) {
        const struct scmd_journal_entry *entry = &e->events[(sequence - 1) % SCMD_EVENTS];
        if (entry->owner == owner && entry->owner_lifetime == e->owner_lifetime[owner]) {
            struct scmd_change copy = entry->value;
            copy.size = scmd_size(out);
            copy.missed = missed;
            memcpy(out, &copy, sizeof copy);
            pthread_mutex_unlock(&e->mu);
            return AZ_OK;
        }
    }
    if (missed) {
        struct scmd_change recovery;
        memset(&recovery, 0, sizeof recovery);
        recovery.size = scmd_size(out);
        recovery.version = AZ_DATA_VERSION;
        recovery.sequence = e->sequence;
        recovery.missed = 1;
        memcpy(out, &recovery, sizeof recovery);
    }
    pthread_mutex_unlock(&e->mu);
    return missed ? AZ_OK : AZ_EMPTY;
}
/* Only after consumers and owner-thread work have stopped. */
static inline int scmd_destroy(struct scmd_engine *e) {
    if (!e) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE) {
            pthread_mutex_unlock(&e->mu);
            return AZ_BUSY;
        }
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->pending[i].occupied) {
            pthread_mutex_unlock(&e->mu);
            return AZ_BUSY;
        }
    pthread_mutex_unlock(&e->mu);
    return pthread_mutex_destroy(&e->mu) ? AZ_BUSY : AZ_OK;
}
#endif

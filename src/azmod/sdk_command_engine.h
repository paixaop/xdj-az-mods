/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded command policy, without native entry points. Drive scmd_step only on an
 * adapter's verified owning thread. Public operations never wait or allocate. */
#ifndef AZMOD_SDK_COMMAND_ENGINE_H
#define AZMOD_SDK_COMMAND_ENGINE_H
#include "../common/aztimestamp.h"
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
#define SCMD_ALL_COMMANDS ((UINT64_C(1) << (AZ_CMD_CUE_COLOR + 1)) - 2)
#define SCMD_TARGET_LOAD 1u
#define SCMD_TARGET_REVISION 2u
#ifdef __cplusplus
static_assert(AZ_CMD_CUE_COLOR < 63, "command IDs must fit the capability mask");
static_assert(offsetof(az_command_request, comment) + sizeof(((az_command_request *) 0)->comment) == sizeof(az_command_request), "command request grew: update validation and retry comparison");
#else
_Static_assert(AZ_CMD_CUE_COLOR < 63, "command IDs must fit the capability mask");
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
    uint64_t token, execution_revision;
    int owner, phase, pinned, awaiting;
    enum scmd_detail detail;
    az_command_request request;
    az_command_result result;
};
struct scmd_change {
    uint32_t size, version;
    uint64_t sequence, token;
    uint32_t missed, settled;
    az_command_result result;
};
struct scmd_journal_entry {
    int owner;
    struct scmd_change value;
};
struct scmd_engine {
    pthread_mutex_t mu;
    uint64_t next_token, sequence, highest_id[SCMD_OWNERS], active[SCMD_DECKS];
    uint64_t (*clock)(void *); /* Process lifetime; bounded, no waiting or engine reentry. */
    void *clock_context;
    struct scmd_record records[SCMD_SLOTS];
    struct scmd_journal_entry events[SCMD_EVENTS];
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
           r->operation >= AZ_CMD_LOOP_IN && r->operation <= AZ_CMD_CUE_COLOR && r->deck < SCMD_DECKS &&
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
/* Compare fields, not C padding or a future caller's unknown tail. Signed zeros
 * and the unused bytes after a comment's NUL have the same argument meaning. */
static inline int scmd_request_equal(const az_command_request *a, const az_command_request *b) {
    return a->operation == b->operation && a->deck == b->deck && a->request_id == b->request_id &&
           a->expected_load_generation == b->expected_load_generation && a->expected_revision == b->expected_revision &&
           compare_timestamps(a->deadline_ns, b->deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && a->quantization == b->quantization && a->flags == b->flags &&
           a->classification == b->classification && a->entry_id == b->entry_id && compare_timestamps(a->position_s, b->position_s, 1, 1000000000, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(a->end_s, b->end_s, 1, 1000000000, 1) == AZ_TIME_EQUAL && a->beats == b->beats && a->rgb == b->rgb && !strcmp(a->comment, b->comment);
}
static inline struct scmd_record *scmd_find_id(struct scmd_engine *e, int owner, uint64_t id) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE && e->records[i].owner == owner && e->records[i].request.request_id == id) return &e->records[i];
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
    entry->value.size = sizeof entry->value;
    entry->value.version = AZ_DATA_VERSION;
    entry->value.sequence = sequence;
    entry->value.token = r->token;
    entry->value.settled = r->phase == SCMD_FINAL && !r->awaiting;
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
static inline int scmd_submit_target(struct scmd_engine *e, int owner, const az_command_request *request,
                              const struct scmd_target *target, const struct scmd_adapter *adapter,
                              az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (detail_out) *detail_out = SCMD_DETAIL_ARGUMENTS;
    if (!e || !scmd_owner_ok(owner) || !scmd_output_ok(out) || !scmd_request_valid(request)) return AZ_INVALID;
    az_command_request r;
    scmd_canonical_request(&r, request);
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *existing = scmd_find_id(e, owner, r.request_id);
    if (existing) {
        if (!scmd_request_equal(&existing->request, &r)) {
            if (detail_out) *detail_out = SCMD_DETAIL_ID_CONFLICT;
            pthread_mutex_unlock(&e->mu);
            return AZ_INVALID;
        }
        if (detail_out) *detail_out = existing->detail;
        scmd_copy_result(out, &existing->result);
        pthread_mutex_unlock(&e->mu);
        return AZ_OK;
    }
    uint64_t now = scmd_now(e);
    enum scmd_detail detail = SCMD_DETAIL_NONE;
    int rc = AZ_OK;
    if (!now) {
        rc = AZ_FAILED;
        detail = SCMD_DETAIL_CLOCK;
    } else if (r.request_id <= e->highest_id[owner]) {
        rc = AZ_STALE;
        detail = SCMD_DETAIL_ID_ORDER;
    } else if (compare_timestamps(r.deadline_ns, now, 1000000000, 1000000000, 1) <= AZ_TIME_EQUAL) {
        rc = AZ_STALE;
        detail = SCMD_DETAIL_DEADLINE;
    } else if (!adapter || !adapter->arguments || !adapter->begin || !adapter->dispatch || !adapter->end) {
        rc = AZ_UNSUPPORTED;
        detail = SCMD_DETAIL_BACKEND;
    } else if (target_pending) {
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
    } else rc = scmd_target_valid(&r, target, adapter, &detail);
    struct scmd_record *slot = NULL;
    if (rc == AZ_OK) {
        unsigned count = 0;
        for (unsigned i = 0; i < SCMD_SLOTS; i++) {
            if (e->records[i].phase == SCMD_FREE) slot = &e->records[i];
            else if (e->records[i].owner == owner) count++;
        }
        /* Leave enough journal sequence values for every outstanding terminal event. */
        if (!slot || count >= SCMD_PER_OWNER || e->next_token == UINT64_MAX || e->sequence >= UINT64_MAX - SCMD_SLOTS) {
            rc = AZ_LIMIT;
            detail = SCMD_DETAIL_QUOTA;
        }
    }
    if (rc != AZ_OK) {
        if (detail_out) *detail_out = detail;
        if (rc != AZ_INVALID) scmd_refuse(out, &r, rc, now);
        pthread_mutex_unlock(&e->mu);
        return rc;
    }
    memset(slot, 0, sizeof *slot);
    slot->token = ++e->next_token;
    slot->owner = owner;
    slot->phase = SCMD_QUEUED;
    slot->request = r;
    slot->result = (az_command_result) {sizeof slot->result, AZ_DATA_VERSION, AZ_COMMAND_ACCEPTED, AZ_PENDING, r.request_id, 0, now};
    e->highest_id[owner] = r.request_id;
    scmd_publish(e, slot);
    scmd_copy_result(out, &slot->result);
    if (detail_out) *detail_out = SCMD_DETAIL_NONE;
    pthread_mutex_unlock(&e->mu);
    return AZ_OK;
}
static inline int scmd_submit(struct scmd_engine *e, int owner, const az_command_request *request,
                              const struct scmd_target *target, const struct scmd_adapter *adapter,
                              az_command_result *out, enum scmd_detail *detail_out) {
    return scmd_submit_target(e, owner, request, target, adapter, out, detail_out, 0);
}
static inline int scmd_poll(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
    if (!e || !scmd_owner_ok(owner) || !id || !scmd_output_ok(out)) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, id);
    if (r) scmd_copy_result(out, &r->result);
    pthread_mutex_unlock(&e->mu);
    return r ? AZ_OK : AZ_STALE;
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
    int rc = !r ? AZ_STALE : r->phase != SCMD_FINAL || r->pinned || r->awaiting ? AZ_BUSY
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
    scmd_finish(e, r, c->outcome, c->reason, c->revision, c->outcome == AZ_COMMAND_UNCERTAIN ? SCMD_DETAIL_BACKEND : SCMD_DETAIL_NONE, now, !c->settled);
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
    if (compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1) <= AZ_TIME_EQUAL) {
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
            if (!now) {
                rc = AZ_FAILED;
                detail = SCMD_DETAIL_CLOCK;
            }
            if (now && compare_timestamps(request.deadline_ns, now, 1000000000, 1000000000, 1) <= AZ_TIME_EQUAL) scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, 0);
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
        if (r->phase == SCMD_FREE || r->phase == SCMD_FINAL || compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1) == AZ_TIME_AFTER) continue;
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
        if (entry->owner == owner) {
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
    pthread_mutex_unlock(&e->mu);
    return pthread_mutex_destroy(&e->mu) ? AZ_BUSY : AZ_OK;
}
#endif

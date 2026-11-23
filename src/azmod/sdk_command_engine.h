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

#pragma GCC visibility push(hidden)

#define SCMD_DECKS 4
#define SCMD_OWNERS 32
#define SCMD_SLOTS 32
#define SCMD_PER_OWNER 4
#define SCMD_EVENTS 64
#define SCMD_ALL_COMMANDS ((UINT64_C(1) << (AZ_CMD_CUE_LOOP_SET + 1)) - 2)
#define SCMD_TARGET_LOAD 1u
#define SCMD_TARGET_REVISION 2u
_Static_assert(AZ_CMD_CUE_LOOP_SET < 63, "command IDs must fit the capability mask");
_Static_assert(offsetof(az_command_request, comment) + sizeof(((az_command_request *) 0)->comment) == sizeof(az_command_request), "command request grew: update validation and retry comparison");

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
struct scmd_cue_batch {
    az_cue_batch_request plan;
    uint64_t native_token, revision;
    unsigned created, changed, creating, stop;
    struct azexit_receipt terminal;
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
    struct scmd_cue_batch batch;
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
    az_cue_batch_request batch;
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
uint64_t scmd_monotonic(void *unused);
#define SCMD_ENGINE_INIT {.mu = PTHREAD_MUTEX_INITIALIZER, .clock = scmd_monotonic}
uint32_t scmd_size(const void *record);
int scmd_owner_ok(int owner);
int scmd_error_status(int status);
int scmd_output_ok(const az_command_result *out);
uint64_t scmd_now(struct scmd_engine *e);
void scmd_copy_result(az_command_result *out, const az_command_result *value);

int scmd_request_valid(const az_command_request *r);

int scmd_request_equal(const az_command_request *a, const az_command_request *b);
struct scmd_record *scmd_find_id(struct scmd_engine *e, int owner, uint64_t id);
struct scmd_pending_delivery *scmd_find_pending_id(struct scmd_engine *e, int owner, uint64_t id);
struct scmd_pending_delivery *scmd_find_pending_token(struct scmd_engine *e, uint64_t token);

struct scmd_record *scmd_find_token(struct scmd_engine *e, uint64_t token);
void scmd_clear_barrier(struct scmd_engine *e, const struct scmd_record *r);

void scmd_finish(struct scmd_engine *e, struct scmd_record *r, uint32_t outcome, int reason,
                 uint64_t revision, enum scmd_detail detail, uint64_t now, int awaiting);

int scmd_target_valid(const az_command_request *r, const struct scmd_target *t,
                      const struct scmd_adapter *adapter, enum scmd_detail *detail);

int scmd_admit(struct scmd_engine *e, int owner, const az_command_request *request,
               az_command_completion_fn callback, void *user,
               const struct scmd_target *target, const struct scmd_adapter *adapter,
               az_command_result *out, enum scmd_detail *detail_out, int target_pending);
int scmd_admit_batch(struct scmd_engine *, int, const az_cue_batch_request *,
                     az_command_completion_fn, void *, const struct scmd_adapter *, az_command_result *);

int scmd_move_to_pending_locked(struct scmd_engine *e, struct scmd_record *r);

int scmd_submit(struct scmd_engine *e, int owner, const az_command_request *request,
                const struct scmd_target *target, const struct scmd_adapter *adapter,
                az_command_result *out, enum scmd_detail *detail_out);
int scmd_submit_async(struct scmd_engine *e, int owner, const az_command_request *request,
                      az_command_completion_fn callback, void *user,
                      const struct scmd_target *target, const struct scmd_adapter *adapter,
                      az_command_result *out, enum scmd_detail *detail_out, int target_pending);
int scmd_owner_deliverable(struct scmd_engine *e, int owner, uint64_t lifetime);
int scmd_acknowledge(struct scmd_engine *e, int owner, uint64_t id);
/* Reserve the next callback identity while holding the store lock, then invoke it
 * after the lock is released.  This is the only path that turns native evidence
 * into a public callback. */

int scmd_next_notification(struct scmd_engine *e, struct scmd_notification *out);
void scmd_notification_done(struct scmd_engine *e, const struct scmd_notification *notification);
int scmd_poll(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out);

int scmd_cancel(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out);
/* Explicit acknowledgement: retained retries stay exact until release. A released
 * ID remains below the owner's high-water mark and can never execute again. */

int scmd_release(struct scmd_engine *e, int owner, uint64_t id);

int scmd_completion_valid(const struct scmd_completion *c);

/* Trusted adapter completion only. A module consumer must never receive this entry. */
int scmd_complete(struct scmd_engine *e, uint64_t token, const struct scmd_completion *completion);

int scmd_step(struct scmd_engine *e, unsigned deck, const struct scmd_adapter *adapter);
/* Clock-driven safety sweep, never a native action. In-flight timeout is uncertainty,
 * and its deck stays fenced until the adapter acknowledges settled execution. */

int scmd_expire(struct scmd_engine *e);

int scmd_event(struct scmd_engine *e, int owner, uint64_t after, struct scmd_change *out);
/* Only after consumers and owner-thread work have stopped. */
int scmd_destroy(struct scmd_engine *e);
#pragma GCC visibility pop
#endif

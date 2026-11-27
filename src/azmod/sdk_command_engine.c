/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded command policy. The host owns the single engine instance and authenticates
 * callers before entering this module; native adapters supply qualified execution. */
#define _POSIX_C_SOURCE 200809L
#include "sdk_command_engine.h"

uint64_t scmd_monotonic(void *unused) {
    (void) unused;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) now.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) now.tv_nsec;
}
uint32_t scmd_size(const void *record) {
    uint32_t size;
    memcpy(&size, record, sizeof size);
    return size;
}
int scmd_owner_ok(int owner) { return owner >= 0 && owner < SCMD_OWNERS; }
int scmd_error_status(int status) { return status >= AZ_NOT_FOUND && status <= AZ_UNSUPPORTED ? status : AZ_FAILED; }
int scmd_output_ok(const az_command_result *out) { return out && scmd_size(out) >= sizeof *out; }
uint64_t scmd_now(struct scmd_engine *e) { return e->clock ? e->clock(e->clock_context) : 0; }
void scmd_copy_result(az_command_result *out, const az_command_result *value) {
    az_command_result copy = *value;
    copy.size = scmd_size(out);
    memcpy(out, &copy, sizeof copy);
}
static int scmd_request_envelope(const az_command_request *r) {
    return r && scmd_size(r) >= sizeof *r && r->version == AZ_DATA_VERSION &&
           r->operation >= AZ_CMD_LOOP_IN && r->operation <= AZ_CMD_CUE_LOOP_SET && r->deck < SCMD_DECKS &&
           r->request_id && r->expected_load_generation && r->deadline_ns;
}
static int scmd_request_cue_ids(const az_command_request *r) {
    return !(r->flags & AZ_COMMAND_NATIVE_CUE_IDS) ||
           ((r->flags & AZ_COMMAND_NATIVE_LOAD) && r->operation > AZ_CMD_LOOP_ADJUST_OUT && r->expected_revision);
}
static int scmd_request_native_load(const az_command_request *r) {
    return !(r->flags & AZ_COMMAND_NATIVE_LOAD) || r->operation > AZ_CMD_LOOP_ADJUST_OUT ||
           (!(r->flags & AZ_COMMAND_PERSIST) && !r->expected_revision);
}
static int scmd_request_values(const az_command_request *r) {
    return r->quantization <= AZ_QUANTIZE_ON && !r->reserved && r->classification <= AZ_CUE_MEMORY &&
           !(r->rgb & UINT32_C(0xff000000)) && isfinite(r->position_s) && isfinite(r->end_s) && isfinite(r->beats) &&
           memchr(r->comment, 0, sizeof r->comment);
}
int scmd_request_valid(const az_command_request *r) {
    return scmd_request_envelope(r) &&
           !(r->flags & ~(AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS)) &&
           scmd_request_cue_ids(r) && scmd_request_native_load(r) && scmd_request_values(r);
}
static void scmd_canonical_request(az_command_request *out, const az_command_request *r) {
    *out = *r;
    out->size = sizeof *out;
    size_t n = strlen(out->comment);
    memset(out->comment + n, 0, sizeof out->comment - n);
}
/* Request IDs bind canonical encoded arguments before a source clock exists.
 * Preserve finite binary64 inputs and normalize the two zero encodings. The
 * native adapter separately checks temporal effects using its actual clock. */
static uint64_t scmd_argument_encoding(double value) {
    uint64_t encoded;
    memcpy(&encoded, &value, sizeof encoded);
    if (!(encoded & UINT64_C(0x7fffffffffffffff))) encoded = 0;
    return encoded;
}
/* Compare the declared request fields, excluding padding and unknown tails. */
static int scmd_request_identity_equal(const az_command_request *a, const az_command_request *b) {
    return a->operation == b->operation && a->deck == b->deck && a->request_id == b->request_id &&
           a->expected_load_generation == b->expected_load_generation && a->expected_revision == b->expected_revision;
}
static int scmd_request_policy_equal(const az_command_request *a, const az_command_request *b) {
    return a->quantization == b->quantization && a->flags == b->flags && a->classification == b->classification &&
           a->entry_id == b->entry_id && a->rgb == b->rgb;
}
static int scmd_request_values_equal(const az_command_request *a, const az_command_request *b) {
    return scmd_argument_encoding(a->position_s) == scmd_argument_encoding(b->position_s) &&
           scmd_argument_encoding(a->end_s) == scmd_argument_encoding(b->end_s) &&
           a->beats == b->beats && !strcmp(a->comment, b->comment);
}
int scmd_request_equal(const az_command_request *a, const az_command_request *b) {
    int deadline_order = compare_timestamps(a->deadline_ns, b->deadline_ns, 1000000000, 1000000000, 1);
    if (deadline_order == AZ_TIME_INVALID) return 0;
    return scmd_request_identity_equal(a, b) && deadline_order == AZ_TIME_EQUAL &&
           scmd_request_policy_equal(a, b) && scmd_request_values_equal(a, b);
}
struct scmd_record *scmd_find_id(struct scmd_engine *e, int owner, uint64_t id) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE && e->records[i].owner == owner &&
            e->records[i].owner_lifetime == e->owner_lifetime[owner] && e->records[i].request.request_id == id) return &e->records[i];
    return NULL;
}
struct scmd_pending_delivery *scmd_find_pending_id(struct scmd_engine *e, int owner, uint64_t id) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->pending[i].occupied && e->pending[i].owner == owner &&
            e->pending[i].owner_lifetime == e->owner_lifetime[owner] && e->pending[i].request.request_id == id) return &e->pending[i];
    return NULL;
}
struct scmd_pending_delivery *scmd_find_pending_token(struct scmd_engine *e, uint64_t token) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->pending[i].occupied && e->pending[i].token == token) return &e->pending[i];
    return NULL;
}
static unsigned scmd_owner_reservations(struct scmd_engine *e, int owner) {
    unsigned count = 0;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        if (e->records[i].phase != SCMD_FREE && e->records[i].owner == owner) count++;
        if (e->pending[i].occupied && e->pending[i].owner == owner) count++;
    }
    return count;
}
static unsigned scmd_total_reservations(struct scmd_engine *e) {
    unsigned count = 0;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        if (e->records[i].phase != SCMD_FREE) count++;
        if (e->pending[i].occupied) count++;
    }
    return count;
}
static struct scmd_pending_delivery *scmd_pending_free(struct scmd_engine *e) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (!e->pending[i].occupied) return &e->pending[i];
    return NULL;
}
struct scmd_record *scmd_find_token(struct scmd_engine *e, uint64_t token) {
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (e->records[i].phase != SCMD_FREE && e->records[i].token == token) return &e->records[i];
    return NULL;
}
void scmd_clear_barrier(struct scmd_engine *e, const struct scmd_record *r) {
    if (!r->pinned && !r->awaiting && e->active[r->request.deck] == r->token) e->active[r->request.deck] = 0;
}
static void scmd_publish(struct scmd_engine *e, const struct scmd_record *r) {
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
void scmd_finish(struct scmd_engine *e, struct scmd_record *r, uint32_t outcome, int reason,
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
static int scmd_target_load_current(const az_command_request *r, const struct scmd_target *t) {
    return t->loaded == 1 && t->load_generation && t->load_generation == r->expected_load_generation;
}
static int scmd_target_revision_valid(const az_command_request *r, const struct scmd_target *t, enum scmd_detail *detail) {
    uint64_t bit = UINT64_C(1) << r->operation;
    if ((t->revision_commands & bit) && !r->expected_revision) {
        *detail = SCMD_DETAIL_REVISION;
        return AZ_INVALID;
    }
    if (r->expected_revision && (!(t->valid & SCMD_TARGET_REVISION) || !t->revision || t->revision != r->expected_revision)) {
        *detail = SCMD_DETAIL_REVISION;
        return AZ_STALE;
    }
    return AZ_OK;
}
int scmd_target_valid(const az_command_request *r, const struct scmd_target *t,
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
    if (!(t->valid & SCMD_TARGET_LOAD) || !scmd_target_load_current(r, t)) {
        *detail = SCMD_DETAIL_LOAD;
        return AZ_STALE;
    }
    int revision = scmd_target_revision_valid(r, t, detail);
    if (revision != AZ_OK) return revision;
    int rc = adapter->arguments(adapter->context, r, t);
    if (rc != AZ_OK) {
        *detail = SCMD_DETAIL_ARGUMENTS;
        return scmd_error_status(rc);
    }
    *detail = SCMD_DETAIL_NONE;
    return AZ_OK;
}
/* A rejection was never accepted, so it neither consumes the ID nor needs retention. */
static int scmd_refuse(az_command_result *out, const az_command_request *r, int reason, uint64_t now) {
    az_command_result value = {sizeof value, AZ_DATA_VERSION, AZ_COMMAND_REJECTED, reason, r->request_id, 0, now};
    scmd_copy_result(out, &value);
    return reason;
}
struct scmd_admission {
    struct scmd_engine *engine;
    int owner, target_pending;
    az_command_request request;
    az_command_completion_fn callback;
    void *user;
    const struct scmd_target *target;
    struct scmd_target observed;
    const struct scmd_adapter *adapter;
    az_command_result *output;
    enum scmd_detail detail;
    uint64_t now;
    struct scmd_record *slot;
    const az_cue_batch_request *batch;
};
struct scmd_retry {
    const az_command_request *request;
    const az_command_result *result;
    az_command_completion_fn callback;
    void *user;
    enum scmd_detail detail;
    const az_cue_batch_request *batch;
};
static int scmd_find_retry(struct scmd_admission *admission, struct scmd_retry *retry) {
    struct scmd_record *existing = scmd_find_id(admission->engine, admission->owner, admission->request.request_id);
    if (existing) {
        *retry = (struct scmd_retry) {&existing->request, &existing->result, existing->callback, existing->callback_user, existing->detail, &existing->batch.plan};
        return 1;
    }
    struct scmd_pending_delivery *pending = scmd_find_pending_id(admission->engine, admission->owner, admission->request.request_id);
    if (!pending) return 0;
    *retry = (struct scmd_retry) {&pending->request, &pending->result, pending->callback, pending->callback_user, SCMD_DETAIL_NONE, &pending->batch};
    return 1;
}
static int scmd_batch_entry_equal(const az_cue_batch_entry *a, const az_cue_batch_entry *b) {
    return a->classification == b->classification && a->type == b->type && a->slot == b->slot &&
           a->rgb == b->rgb && scmd_argument_encoding(a->position_s) == scmd_argument_encoding(b->position_s) &&
           scmd_argument_encoding(a->end_s) == scmd_argument_encoding(b->end_s) &&
           !strcmp(a->comment, b->comment);
}
static int scmd_batch_equal(const az_cue_batch_request *stored, const az_cue_batch_request *incoming) {
    if (!incoming) return !stored->count;
    if (stored->count != incoming->count || stored->mode != incoming->mode) return 0;
    for (unsigned i = 0; i < stored->count; ++i)
        if (!scmd_batch_entry_equal(&stored->entries[i], &incoming->entries[i])) return 0;
    return 1;
}
static int scmd_retry_admission(struct scmd_admission *admission, const struct scmd_retry *retry) {
    if (!scmd_request_equal(retry->request, &admission->request) || retry->callback != admission->callback ||
        retry->user != admission->user || !scmd_batch_equal(retry->batch, admission->batch)) {
        admission->detail = SCMD_DETAIL_ID_CONFLICT;
        return AZ_INVALID;
    }
    admission->detail = retry->detail;
    scmd_copy_result(admission->output, retry->result);
    return AZ_OK;
}
static void scmd_copy_batch(struct scmd_record *record, const az_cue_batch_request *batch) {
    record->batch.plan = *batch;
    record->batch.native_token = record->token;
    record->batch.revision = batch->expected_revision;
}
static int scmd_adapter_valid(const struct scmd_adapter *adapter) {
    return adapter && adapter->arguments && adapter->begin && adapter->dispatch && adapter->end;
}
static int scmd_admission_error(struct scmd_admission *admission, int status, enum scmd_detail detail) {
    admission->detail = detail;
    return status;
}
static int scmd_admission_policy(struct scmd_admission *admission) {
    struct scmd_engine *e = admission->engine;
    const az_command_request *r = &admission->request;
    int deadline_order = compare_timestamps(r->deadline_ns, admission->now, 1000000000, 1000000000, 1);
    admission->detail = SCMD_DETAIL_NONE;
    if (r->request_id <= e->highest_id[admission->owner]) return scmd_admission_error(admission, AZ_STALE, SCMD_DETAIL_ID_ORDER);
    if (!scmd_adapter_valid(admission->adapter)) return scmd_admission_error(admission, AZ_UNSUPPORTED, SCMD_DETAIL_BACKEND);
    if (!admission->now || deadline_order == AZ_TIME_INVALID) return scmd_admission_error(admission, AZ_FAILED, SCMD_DETAIL_CLOCK);
    if (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL) return scmd_admission_error(admission, AZ_STALE, SCMD_DETAIL_DEADLINE);
    return AZ_OK;
}
static int scmd_observe_admission(struct scmd_admission *admission) {
    if (!admission->engine->delivery || admission->target) return AZ_OK;
    memset(&admission->observed, 0, sizeof admission->observed);
    int status = admission->adapter->begin(admission->adapter->context, &admission->request, &admission->observed);
    admission->target = &admission->observed;
    admission->target_pending = status == AZ_BUSY || status == AZ_PENDING;
    if (status != AZ_OK && !admission->target_pending)
        return scmd_admission_error(admission, scmd_error_status(status), status == AZ_STALE ? SCMD_DETAIL_LOAD : SCMD_DETAIL_BACKEND);
    return AZ_OK;
}
static int scmd_pending_target_valid(struct scmd_admission *admission) {
    const struct scmd_target *target = admission->target;
    /* A busy native lookup supplies no revision evidence. Retain the request
     * for fresh validation at dispatch under the same advertised capability. */
    if (!target || !(target->commands & (UINT64_C(1) << admission->request.operation)))
        return scmd_admission_error(admission, AZ_UNSUPPORTED, SCMD_DETAIL_CAPABILITY);
    if ((target->valid & SCMD_TARGET_LOAD) && !scmd_target_load_current(&admission->request, target))
        return scmd_admission_error(admission, AZ_STALE, SCMD_DETAIL_LOAD);
    return AZ_OK;
}
static int scmd_admission_target(struct scmd_admission *admission) {
    if (admission->target_pending) return scmd_pending_target_valid(admission);
    return scmd_target_valid(&admission->request, admission->target, admission->adapter, &admission->detail);
}
static int scmd_admission_slot(struct scmd_admission *admission) {
    struct scmd_engine *engine = admission->engine;
    for (unsigned i = 0; i < SCMD_SLOTS; i++)
        if (engine->records[i].phase == SCMD_FREE) admission->slot = &engine->records[i];
    unsigned count = scmd_owner_reservations(engine, admission->owner);
    /* Leave enough journal sequence values for every outstanding terminal event. */
    if (!admission->slot || count >= SCMD_PER_OWNER || scmd_total_reservations(engine) >= SCMD_SLOTS ||
        engine->next_token >= (UINT64_MAX / UINT64_C(8)) || engine->sequence >= UINT64_MAX - SCMD_SLOTS)
        return scmd_admission_error(admission, AZ_LIMIT, SCMD_DETAIL_QUOTA);
    return AZ_OK;
}
static int scmd_admission_reserve(struct scmd_admission *admission) {
    struct scmd_engine *engine = admission->engine;
    if (!engine->delivery) return AZ_OK;
    const az_command_request *request = &admission->request;
    struct azexit_request identity = {engine->next_token + 1, request->expected_load_generation, request->deadline_ns, request->deck, 0};
    return engine->delivery->reserve(&identity);
}
static int scmd_admission_validate(struct scmd_admission *admission) {
    int rc = scmd_admission_policy(admission);
    if (rc != AZ_OK) return rc;
    rc = scmd_observe_admission(admission);
    if (rc != AZ_OK) return rc;
    rc = scmd_admission_target(admission);
    if (rc != AZ_OK) return rc;
    rc = scmd_admission_slot(admission);
    if (rc != AZ_OK) return rc;
    return scmd_admission_reserve(admission);
}
static void scmd_admission_commit(struct scmd_admission *admission) {
    struct scmd_engine *engine = admission->engine;
    struct scmd_record *slot = admission->slot;
    memset(slot, 0, sizeof *slot);
    slot->token = ++engine->next_token;
    slot->owner = admission->owner;
    slot->owner_lifetime = engine->owner_lifetime[admission->owner];
    slot->phase = SCMD_QUEUED;
    slot->awaiting = engine->delivery != NULL;
    slot->asynchronous = admission->callback != NULL;
    slot->callback = admission->callback;
    slot->callback_user = admission->user;
    slot->request = admission->request;
    if (admission->batch) scmd_copy_batch(slot, admission->batch);
    slot->result = (az_command_result) {sizeof slot->result, AZ_DATA_VERSION, AZ_COMMAND_ACCEPTED, AZ_PENDING,
                                        admission->request.request_id, 0, admission->now};
    engine->highest_id[admission->owner] = admission->request.request_id;
    scmd_publish(engine, slot);
    scmd_copy_result(admission->output, &slot->result);
    admission->detail = SCMD_DETAIL_NONE;
}
static int scmd_admit_locked(struct scmd_admission *admission);
int scmd_admit_batch(struct scmd_engine *e, int owner, const az_cue_batch_request *batch,
                     az_command_completion_fn callback, void *user,
                     const struct scmd_adapter *adapter, az_command_result *out) {
    struct scmd_admission admission = {
        .engine = e, .owner = owner, .callback = callback, .user = user, .output = out, .adapter = adapter, .batch = batch, .detail = SCMD_DETAIL_ARGUMENTS, .request = {.size = sizeof(az_command_request), .version = AZ_DATA_VERSION, .operation = AZ_CMD_CUE_POINT_SET, .deck = batch->deck, .request_id = batch->request_id, .expected_load_generation = batch->expected_load_generation, .expected_revision = batch->expected_revision, .deadline_ns = batch->deadline_ns, .flags = AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS}};
    pthread_mutex_lock(&e->mu);
    int rc = scmd_admit_locked(&admission);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static int scmd_admit_locked(struct scmd_admission *admission) {
    if (admission->engine->owner_closed[admission->owner]) return AZ_INVALID;
    struct scmd_retry retry;
    if (scmd_find_retry(admission, &retry)) return scmd_retry_admission(admission, &retry);
    admission->now = scmd_now(admission->engine);
    int rc = scmd_admission_validate(admission);
    if (rc != AZ_OK) {
        if (rc != AZ_INVALID && rc != AZ_BUSY) scmd_refuse(admission->output, &admission->request, rc, admission->now);
        return rc;
    }
    scmd_admission_commit(admission);
    return AZ_OK;
}
int scmd_admit(struct scmd_engine *e, int owner, const az_command_request *request,
               az_command_completion_fn callback, void *user,
               const struct scmd_target *target, const struct scmd_adapter *adapter,
               az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (detail_out) *detail_out = SCMD_DETAIL_ARGUMENTS;
    if (!e || !scmd_owner_ok(owner) || !scmd_output_ok(out)) return AZ_INVALID;
    struct scmd_admission admission = {.engine = e, .owner = owner, .target_pending = target_pending, .callback = callback, .user = user, .target = target, .adapter = adapter, .output = out, .detail = SCMD_DETAIL_ARGUMENTS};
    scmd_canonical_request(&admission.request, request);
    if (e->delivery ? pthread_mutex_lock(&e->mu) : pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    int rc = scmd_admit_locked(&admission);
    if (detail_out) *detail_out = admission.detail;
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static int scmd_released_notification_ready(const struct scmd_record *r) {
    return r->phase == SCMD_FINAL && r->asynchronous && r->terminal_delivered && r->acknowledged &&
           !r->awaiting && !r->pinned && !r->callback_active && !r->retired;
}
int scmd_move_to_pending_locked(struct scmd_engine *e, struct scmd_record *r) {
    if (!e || !r || !scmd_released_notification_ready(r)) return 0;
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
    p->batch = r->batch.plan;
    memset(r, 0, sizeof *r);
    return 1;
}
static int scmd_submit_target(struct scmd_engine *e, int owner, const az_command_request *request,
                              const struct scmd_target *target, const struct scmd_adapter *adapter,
                              az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (!scmd_request_valid(request)) {
        if (detail_out) *detail_out = SCMD_DETAIL_ARGUMENTS;
        return AZ_INVALID;
    }
    return scmd_admit(e, owner, request, NULL, NULL, target, adapter, out, detail_out, target_pending);
}
int scmd_submit(struct scmd_engine *e, int owner, const az_command_request *request,
                const struct scmd_target *target, const struct scmd_adapter *adapter,
                az_command_result *out, enum scmd_detail *detail_out) {
    return scmd_submit_target(e, owner, request, target, adapter, out, detail_out, 0);
}
int scmd_submit_async(struct scmd_engine *e, int owner, const az_command_request *request,
                      az_command_completion_fn callback, void *user,
                      const struct scmd_target *target, const struct scmd_adapter *adapter,
                      az_command_result *out, enum scmd_detail *detail_out, int target_pending) {
    if (!callback || !scmd_request_valid(request)) return AZ_INVALID;
    return scmd_admit(e, owner, request, callback, user, target, adapter, out, detail_out, target_pending);
}
int scmd_owner_deliverable(struct scmd_engine *e, int owner, uint64_t lifetime) {
    return !e->owner_closed[owner] && !e->owner_initializing[owner] &&
           lifetime == e->owner_lifetime[owner];
}
int scmd_acknowledge(struct scmd_engine *e, int owner, uint64_t id) {
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
static int scmd_terminal_notification_ready(struct scmd_engine *e, const struct scmd_record *r) {
    return r->phase == SCMD_FINAL && r->asynchronous && r->callback && !r->callback_active &&
           !r->terminal_delivered && !r->retired && scmd_owner_deliverable(e, r->owner, r->owner_lifetime);
}
static void scmd_terminal_notification(struct scmd_engine *e, struct scmd_record *r, struct scmd_notification *out) {
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
}
static int scmd_claim_terminal_notification(struct scmd_engine *e, struct scmd_notification *out) {
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *record = &e->records[i];
        if (!scmd_terminal_notification_ready(e, record)) continue;
        scmd_terminal_notification(e, record, out);
        return AZ_OK;
    }
    return AZ_EMPTY;
}
static int scmd_pending_notification_ready(struct scmd_engine *e, const struct scmd_pending_delivery *p) {
    return p->occupied && p->callback && !p->callback_active && !p->released_delivered &&
           scmd_owner_deliverable(e, p->owner, p->owner_lifetime);
}
static void scmd_released_notification(struct scmd_pending_delivery *p, struct scmd_notification *out) {
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
}
static int scmd_claim_released_notification(struct scmd_engine *e, struct scmd_notification *out) {
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_pending_delivery *pending = &e->pending[i];
        if (!scmd_pending_notification_ready(e, pending)) continue;
        scmd_released_notification(pending, out);
        return AZ_OK;
    }
    return AZ_EMPTY;
}
int scmd_next_notification(struct scmd_engine *e, struct scmd_notification *out) {
    if (!e || !out) return AZ_INVALID;
    pthread_mutex_lock(&e->mu);
    int rc = scmd_claim_terminal_notification(e, out);
    if (rc == AZ_EMPTY) rc = scmd_claim_released_notification(e, out);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
void scmd_notification_done(struct scmd_engine *e, const struct scmd_notification *notification) {
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
int scmd_poll(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
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
static int scmd_cancel_record(struct scmd_engine *e, struct scmd_record *r, az_command_result *out) {
    int rc = AZ_STALE;
    if (r->phase == SCMD_QUEUED || r->phase == SCMD_PREPARING) {
        uint64_t now = scmd_now(e);
        if (!now) return AZ_FAILED;
        scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0, SCMD_DETAIL_NONE, now, 0);
        rc = AZ_OK;
    } else if (r->phase == SCMD_DISPATCHED || r->awaiting) rc = AZ_PENDING;
    else if (r->result.outcome == AZ_COMMAND_CANCELLED) rc = AZ_OK;
    scmd_copy_result(out, &r->result);
    return rc;
}
static int scmd_cancel_pending(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
    const struct scmd_pending_delivery *pending = scmd_find_pending_id(e, owner, id);
    if (!pending) return AZ_STALE;
    scmd_copy_result(out, &pending->result);
    return pending->result.outcome == AZ_COMMAND_CANCELLED ? AZ_OK : AZ_STALE;
}
int scmd_cancel(struct scmd_engine *e, int owner, uint64_t id, az_command_result *out) {
    if (!e || !scmd_owner_ok(owner) || !id || !scmd_output_ok(out)) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *record = scmd_find_id(e, owner, id);
    int rc = record ? scmd_cancel_record(e, record, out) : scmd_cancel_pending(e, owner, id, out);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* Explicit acknowledgement: retained retries stay exact until release. A released
 * ID remains below the owner's high-water mark and can never execute again. */
static int scmd_release_record_status(const struct scmd_record *r) {
    if (r->asynchronous) return AZ_INVALID;
    return r->phase != SCMD_FINAL || r->pinned || r->awaiting ? AZ_BUSY : AZ_OK;
}
int scmd_release(struct scmd_engine *e, int owner, uint64_t id) {
    if (!e || !scmd_owner_ok(owner) || !id) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, id);
    int rc = r ? scmd_release_record_status(r) : scmd_find_pending_id(e, owner, id) ? AZ_INVALID
                                                                                    : AZ_STALE;
    if (rc == AZ_OK) memset(r, 0, sizeof *r);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static int scmd_applied_completion(const struct scmd_completion *c) {
    return c->reason == AZ_OK && c->revision && c->load_generation && c->settled;
}
static int scmd_completion_error(const struct scmd_completion *c) {
    return c->reason >= AZ_NOT_FOUND && c->reason <= AZ_UNSUPPORTED;
}
static int scmd_rejected_completion(const struct scmd_completion *c) {
    return scmd_completion_error(c) && c->reason != AZ_CANCELLED && c->settled;
}
int scmd_completion_valid(const struct scmd_completion *c) {
    if (!c || c->settled > 1) return 0;
    if (c->outcome == AZ_COMMAND_APPLIED) return scmd_applied_completion(c);
    if (c->revision) return 0;
    if (c->outcome == AZ_COMMAND_CANCELLED) return c->reason == AZ_CANCELLED && c->settled;
    if (c->outcome == AZ_COMMAND_REJECTED) return scmd_rejected_completion(c);
    return c->outcome == AZ_COMMAND_UNCERTAIN && scmd_completion_error(c);
}
static int scmd_settle_uncertainty(struct scmd_engine *e, struct scmd_record *r, const struct scmd_completion *c) {
    if (r->result.outcome != AZ_COMMAND_UNCERTAIN || !r->awaiting) return AZ_STALE;
    if (!scmd_completion_valid(c)) return AZ_INVALID;
    if (!c->settled) return AZ_PENDING;
    r->awaiting = 0;
    scmd_clear_barrier(e, r);
    (void) scmd_move_to_pending_locked(e, r);
    return AZ_OK; /* The published terminal uncertainty remains immutable. */
}
static int scmd_completion_matches(struct scmd_engine *e, struct scmd_record *r, const struct scmd_completion *c, uint64_t now) {
    if (c->outcome != AZ_COMMAND_APPLIED) return AZ_OK;
    if (c->load_generation != r->request.expected_load_generation) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_STALE, 0, SCMD_DETAIL_LOAD, now, 0);
        return AZ_STALE;
    }
    if (c->revision < r->execution_revision) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_STALE, 0, SCMD_DETAIL_REVISION, now, 0);
        return AZ_STALE;
    }
    return AZ_OK;
}
static int scmd_complete_locked(struct scmd_engine *e, struct scmd_record *r, const struct scmd_completion *c, uint64_t now) {
    if (r->phase == SCMD_FINAL) return scmd_settle_uncertainty(e, r, c);
    if (r->phase != SCMD_DISPATCHED) return AZ_STALE;
    if (!now || !scmd_completion_valid(c)) {
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0, now ? SCMD_DETAIL_BACKEND : SCMD_DETAIL_CLOCK, now, 1);
        return AZ_INVALID;
    }
    int matched = scmd_completion_matches(e, r, c, now);
    if (matched != AZ_OK) return matched;
    scmd_finish(e, r, c->outcome, c->reason, c->revision,
                c->outcome == AZ_COMMAND_UNCERTAIN ? SCMD_DETAIL_BACKEND : SCMD_DETAIL_NONE,
                now, !c->settled);
    return AZ_OK;
}
/* Trusted adapter completion only. A module consumer must never receive this entry. */
int scmd_complete(struct scmd_engine *e, uint64_t token, const struct scmd_completion *completion) {
    if (!e || !token || !completion) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_token(e, token);
    int rc = r ? scmd_complete_locked(e, r, completion, scmd_now(e)) : AZ_STALE;
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* One request for one deck. begin/end and dispatch are outside the lock; pinned
 * prevents release, and active prevents both native overlap and FIFO overtaking. */
struct scmd_execution {
    struct scmd_engine *engine;
    unsigned deck;
    const struct scmd_adapter *adapter;
    struct scmd_record *record;
    az_command_request request;
    struct scmd_target target;
    uint64_t token;
    int status, send;
};
static struct scmd_record *scmd_oldest_queued(struct scmd_engine *e, unsigned deck) {
    struct scmd_record *r = NULL;
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        struct scmd_record *candidate = &e->records[i];
        if (candidate->phase == SCMD_QUEUED && candidate->request.deck == deck && (!r || candidate->token < r->token)) r = candidate;
    }
    return r;
}
static int scmd_claim_execution(struct scmd_execution *execution) {
    struct scmd_engine *e = execution->engine;
    unsigned deck = execution->deck;
    if (e->active[deck]) return AZ_BUSY;
    struct scmd_record *r = scmd_oldest_queued(e, deck);
    if (!r) return AZ_EMPTY;
    uint64_t now = scmd_now(e);
    if (!now) return AZ_FAILED;
    int deadline_order = compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1);
    if (deadline_order == AZ_TIME_INVALID) return AZ_FAILED;
    if (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL) {
        scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, 0);
        return AZ_OK;
    }
    r->phase = SCMD_PREPARING;
    r->pinned = 1;
    e->active[deck] = r->token;
    execution->request = r->request;
    execution->token = r->token;
    return AZ_OK;
}
static void scmd_validate_execution(struct scmd_execution *execution) {
    struct scmd_engine *e = execution->engine;
    struct scmd_record *r = execution->record;
    enum scmd_detail detail;
    int rc = scmd_target_valid(&execution->request, &execution->target, execution->adapter, &detail);
    uint64_t now = scmd_now(e); /* Recheck after owner-thread validation, not before it. */
    int deadline_order = compare_timestamps(execution->request.deadline_ns, now, 1000000000, 1000000000, 1);
    if (!now || deadline_order == AZ_TIME_INVALID) {
        rc = AZ_FAILED;
        detail = SCMD_DETAIL_CLOCK;
    }
    if (now && (deadline_order == AZ_TIME_BEFORE || deadline_order == AZ_TIME_EQUAL)) scmd_finish(e, r, AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, 0);
    else if (rc != AZ_OK) scmd_finish(e, r, AZ_COMMAND_REJECTED, rc, 0, detail, now, 0);
    else {
        r->phase = SCMD_DISPATCHED;
        r->awaiting = 1;
        r->execution_revision = execution->target.valid & SCMD_TARGET_REVISION ? execution->target.revision : 0;
        execution->send = 1; /* Cancellation after this claim cannot promise no action. */
    }
    execution->status = rc;
}
static void scmd_execution_begun(struct scmd_execution *execution) {
    struct scmd_engine *e = execution->engine;
    struct scmd_record *r = scmd_find_token(e, execution->token); /* The pinned record cannot have been released. */
    execution->record = r;
    if (r->phase == SCMD_FINAL) return;
    int rc = execution->status;
    if (rc == AZ_BUSY || rc == AZ_PENDING || rc == AZ_INVALID) r->phase = SCMD_QUEUED;
    else if (rc != AZ_OK) scmd_finish(e, r, AZ_COMMAND_REJECTED, scmd_error_status(rc), 0, SCMD_DETAIL_BACKEND, scmd_now(e), 0);
    else scmd_validate_execution(execution);
}
static void scmd_dispatch_completed(struct scmd_execution *execution, const struct scmd_completion *completion, int dispatched) {
    if (!execution->send) return;
    struct scmd_engine *e = execution->engine;
    struct scmd_record *r = execution->record;
    if (dispatched == AZ_OK) (void) scmd_complete_locked(e, r, completion, scmd_now(e));
    else if (dispatched != AZ_PENDING && r->phase != SCMD_FINAL)
        scmd_finish(e, r, AZ_COMMAND_UNCERTAIN, scmd_error_status(dispatched), 0, SCMD_DETAIL_BACKEND, scmd_now(e), 1);
}
static int scmd_release_execution(struct scmd_execution *execution) {
    struct scmd_record *r = execution->record;
    r->pinned = 0;
    if (r->phase == SCMD_QUEUED || (r->phase == SCMD_FINAL && !r->awaiting)) scmd_clear_barrier(execution->engine, r);
    int pending = r->phase == SCMD_DISPATCHED;
    int queued = r->phase == SCMD_QUEUED;
    return execution->send ? pending ? AZ_PENDING : AZ_OK : queued ? execution->status
                                                                   : AZ_OK;
}
int scmd_step(struct scmd_engine *e, unsigned deck, const struct scmd_adapter *adapter) {
    if (!e || deck >= SCMD_DECKS || !scmd_adapter_valid(adapter)) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_execution execution = {.engine = e, .deck = deck, .adapter = adapter};
    int claimed = scmd_claim_execution(&execution);
    pthread_mutex_unlock(&e->mu);
    if (claimed != AZ_OK || !execution.token) return claimed;
    execution.status = adapter->begin(adapter->context, &execution.request, &execution.target);
    int leased = execution.status == AZ_OK;
    pthread_mutex_lock(&e->mu);
    scmd_execution_begun(&execution);
    pthread_mutex_unlock(&e->mu);
    struct scmd_completion completion = {0};
    int dispatched = execution.send ? adapter->dispatch(adapter->context, &execution.request, execution.token, &completion) : AZ_OK;
    if (leased) adapter->end(adapter->context, deck);
    pthread_mutex_lock(&e->mu);
    execution.record = scmd_find_token(e, execution.token);
    scmd_dispatch_completed(&execution, &completion, dispatched);
    int result = scmd_release_execution(&execution);
    pthread_mutex_unlock(&e->mu);
    return result;
}
/* Clock-driven safety sweep, never a native action. In-flight timeout is uncertainty,
 * and its deck stays fenced until the adapter acknowledges settled execution. */
static int scmd_expire_record(struct scmd_engine *e, struct scmd_record *r, uint64_t now) {
    if (r->phase == SCMD_FREE || r->phase == SCMD_FINAL) return AZ_OK;
    int order = compare_timestamps(r->request.deadline_ns, now, 1000000000, 1000000000, 1);
    if (order == AZ_TIME_INVALID) return AZ_FAILED;
    if (order == AZ_TIME_AFTER) return AZ_OK;
    int dispatched = r->phase == SCMD_DISPATCHED;
    scmd_finish(e, r, dispatched ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_CANCELLED, AZ_STALE, 0, SCMD_DETAIL_DEADLINE, now, dispatched);
    return AZ_OK;
}
int scmd_expire(struct scmd_engine *e) {
    if (!e) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    uint64_t now = scmd_now(e);
    if (!now) {
        pthread_mutex_unlock(&e->mu);
        return AZ_FAILED;
    }
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        int rc = scmd_expire_record(e, &e->records[i], now);
        if (rc != AZ_OK) {
            pthread_mutex_unlock(&e->mu);
            return rc;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return AZ_OK;
}
static int scmd_event_locked(struct scmd_engine *e, int owner, uint64_t after, struct scmd_change *out) {
    uint64_t first = e->sequence >= SCMD_EVENTS ? e->sequence - SCMD_EVENTS + 1 : 1;
    int missed = after < first - 1;
    for (uint64_t sequence = after < first ? first : after + 1; sequence && sequence <= e->sequence; sequence++) {
        const struct scmd_journal_entry *entry = &e->events[(sequence - 1) % SCMD_EVENTS];
        if (entry->owner == owner && entry->owner_lifetime == e->owner_lifetime[owner]) {
            struct scmd_change copy = entry->value;
            copy.size = scmd_size(out);
            copy.missed = missed;
            memcpy(out, &copy, sizeof copy);
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
    return missed ? AZ_OK : AZ_EMPTY;
}
int scmd_event(struct scmd_engine *e, int owner, uint64_t after, struct scmd_change *out) {
    if (!e || !scmd_owner_ok(owner) || !out || scmd_size(out) < sizeof *out || after == UINT64_MAX) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    int rc = after > e->sequence ? AZ_INVALID : scmd_event_locked(e, owner, after, out);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
/* Only after consumers and owner-thread work have stopped. */
int scmd_destroy(struct scmd_engine *e) {
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

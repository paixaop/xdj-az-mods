/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuetext.h"
#include "../common/azcuecommands.h"
#include "../common/azcueactor.h"
#include "../common/azcueobserver.h"
#include "../common/azcuestore.h"
#include "../common/azloadobserver.h"
#include "../common/azcueprofile.h"
#include "../common/azcuepersist.h"
#include "../common/azcuerecall.h"
#include "../common/azcuerecallbridge.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include "../common/aznative_sync.h"
#include <string.h>
#include <math.h>
#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <stdio.h>
#endif

const uint64_t INTERNAL = UINT64_C(1) << 63;
static uint64_t bit(unsigned operation) { return UINT64_C(1) << operation; }
typedef struct Cache Cache;
struct Cache {
    pthread_mutex_t mutex;
    _Atomic(bool) waiter;
    struct az_native_cue_snapshot value;
    uint64_t observed;
    _Atomic(uint64_t) refresh, actor_finished;
    azcs_revision_state revision;
    /* One copied public command owns admission through release, including
     * contention with another deck's owner frame or an internal snapshot query.
     * 0 empty,1 exclusive write/read,2 queued,3 terminal,4 management posting,
     * 5 active public command in the actor. */
    _Atomic(unsigned) waiting;
    struct azexit_cue_request waiting_request;
    struct azexit_result waiting_result;
};
typedef struct AutoPlan AutoPlan;
struct AutoPlan {
    bool active, final, effect_seen;
    unsigned steps;
    uint64_t token, revision;
    struct az_cue_entry target;
    uint32_t target_rate;
};
typedef struct Pending Pending;
struct Pending {
    _Atomic(uint64_t) cookie, token;
    _Atomic(uint64_t) resume, persist_ready, bridge_ready;
    _Atomic(uint64_t) actor_retired, retired_load, persist_retired, stop_resumed;
    struct azexit_result actor_completion; // management dispatcher only
    _Atomic(uintptr_t) manager;
    struct az_cue_entry expected;
    uint32_t expected_rate;
    azcs_property retained;
    _Atomic(uint64_t) bridge, bridge_generation, bridge_revision;
    _Atomic(unsigned) stage;
    _Atomic(uint64_t) stage_token;
    struct azexit_cue_stage_sample staged_cue;
    uint64_t before_revision;
    struct azexit_cue_request active_request;
    // Borrowed only inside confirm: persistence pins all five baseline refs
    // until that callback returns, including the original immutable contents.
    struct azcuepersist_witness before_witness;
    AutoPlan automatic;
};
typedef struct State State;
struct State {
    _Atomic(bool) enabled;
    _Atomic(uint64_t) capabilities, serial;
    Cache decks[4];
    Pending pending[4];
    azcs_profile store;
    int build;
    uintptr_t bias;
    _Atomic(unsigned) notifications, collections;
};
static State command_state = {.decks = {[0 ... 3] = {.mutex = PTHREAD_MUTEX_INITIALIZER}}};
#define state() (command_state)
#ifdef AZSDK_CUE_COMMANDS_TESTING
void azsdk_exit_delivery_notify(unsigned unused) { (void) unused; }
int azsdk_exit_delivery_continue(const struct azexit_request *r, uint64_t clock) {
    (void) r;
    (void) clock;
    return AZ_OK;
}
int azsdk_cue_actor_peek(unsigned deck, struct azexit_result *result) {
    return azsdk_cue_actor_poll(deck, result);
}
int azsdk_cue_actor_take(unsigned deck, struct azexit_result *result) {
    return azsdk_cue_actor_peek(deck, result);
}
int azsdk_cue_persist_take(const struct azexit_cue_request *request, void *manager,
                           struct azexit_result *result) {
    return azsdk_cue_persist_poll(request, manager, result);
}
int azsdk_exit_recall_take(unsigned deck, struct azexit_result *result) {
    return azsdk_exit_recall_poll(deck, result);
}
#endif
static bool public_actor_finished(State *s, unsigned deck) {
    (void) s;
#ifdef AZSDK_CUE_COMMANDS_TESTING
    struct azexit_result result = {0};
    if (azsdk_cue_actor_peek(deck, &result) != AZ_OK || !result.token ||
        (result.token & INTERNAL) || result.outcome == AZ_COMMAND_ACCEPTED) return false;
#else
    if (atomic_load_explicit(&(s->decks[deck].actor_finished), memory_order_acquire) !=
        s->decks[deck].waiting_request.identity.token) return false;
#endif
    return true;
}
static bool unfinished_public_work() {
    State *s = &(state());
    for (unsigned deck = 0; deck < 4; ++deck) {
        unsigned waiting = atomic_load_explicit(&(s->decks[deck].waiting), memory_order_acquire);
        if (!waiting || waiting == 3) continue;
        if (waiting != 5) return true;
        if (!public_actor_finished(s, deck)) return true;
    }
    return false;
}
static void retire_internal_reads() {
    State *s = &(state());
    for (unsigned deck = 0; deck < 4; ++deck) {
        Cache *cache = &(s->decks[deck]);
        uint64_t refresh = atomic_load_explicit(&(cache->refresh), memory_order_acquire);
        if ((refresh & INTERNAL) && azsdk_cue_actor_cancel(deck, refresh) == AZ_OK &&
            azsdk_cue_actor_release(deck, refresh) == AZ_OK)
            atomic_compare_exchange_strong_explicit(&(cache->refresh), &(refresh), 0, memory_order_acq_rel, memory_order_acquire);
    }
}
#ifdef AZSDK_CUE_COMMANDS_TESTING
_Atomic(uint64_t) test_clock = 0;
#endif
static uint64_t now() {
#ifdef AZSDK_CUE_COMMANDS_TESTING
    uint64_t injected = atomic_load_explicit(&(test_clock), memory_order_relaxed);
    if (injected) return injected;
#endif
    struct timespec t = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return (uint64_t) (t.tv_sec) * UINT64_C(1000000000) + (uint64_t) (t.tv_nsec);
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
static void command_diagnostic(const struct azexit_cue_request *request, unsigned stage, const char *name) {
    static _Atomic(unsigned) last[4];
    static _Atomic(uint64_t) tokens[4];
    unsigned deck = request->identity.deck;
    if (deck >= 4 || !request->operation) return;
    bool changed = atomic_exchange_explicit(&(tokens[deck]), request->identity.token, memory_order_relaxed) != request->identity.token;
    if (atomic_exchange_explicit(&(last[deck]), stage + 1, memory_order_relaxed) == stage + 1 && !changed) return;
    fprintf(stderr, "AZCUE command deck=%u stage=%s op=%u token=%llu load=%llu clock=%llu deadline=%llu\n",
            deck, name, request->operation, (unsigned long long) request->identity.token, (unsigned long long) request->identity.load_generation,
            (unsigned long long) now(), (unsigned long long) request->identity.deadline_ns);
}
#define COMMAND_DIAG(request, stage, name) command_diagnostic(&(request), stage, name)
#else
#define COMMAND_DIAG(...) ((void) 0)
#endif
static int current(unsigned deck, uint64_t generation, void *unused) {
    (void) unused;
    struct azsdk_load_sample sample = {0};
    return atomic_load_explicit(&(state().enabled), memory_order_acquire) && azsdk_load_read((int) (deck), &sample) &&
           sample.load_generation == generation;
}
static void ready(unsigned deck, uint64_t token, void *unused) {
    (void) unused;
    if (deck >= 4 || !token) return;
    Pending *pending = &(state().pending[deck]);
    atomic_store_explicit(&(pending->persist_ready), token, memory_order_release);
    atomic_store_explicit(&(pending->resume), token, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
static void publish_waiting_result(unsigned deck, const struct azexit_result *result) {
    struct azexit_receipt value = {
        sizeof value, AZEXIT_DELIVERY_VERSION, AZEXIT_RECEIPT_TERMINAL, 1, deck,
        result->token, result->load_generation, 0, result->revision, now(), result->outcome, result->reason};
    (void) azsdk_exit_delivery_publish(&value);
}
#define read(T, p, offset) ({ T read_value; memcpy(&read_value, (const char *)(p) + (offset), sizeof read_value); read_value; })
static bool valid_position(const unsigned char *p) {
    return p[0] && (p[4] & 0x1f) && p[6] && read(int32_t, p, 8) != 0 &&
           read(uint64_t, p, 16) <= UINT64_C(9007199254740992) && read(uint32_t, p, 0x30) && read(uintptr_t, p, 0x20);
}
static bool property_scalar(const azcs_native_property *property, struct az_cue_entry *out) {
    const unsigned char *p = property->bytes;
    if (!valid_position(p + 0x30)) return false;
    out->start_s = (double) (read(uint64_t, p, 0x40)) / read(uint32_t, p, 0x60);
    out->valid |= AZ_CUE_START;
    out->type = AZ_CUE_POINT;
    if (valid_position(p + 0x88) && !memcmp(p + 0x30, p + 0x88, 12) &&
        read(uint32_t, p, 0x60) == read(uint32_t, p, 0xb8) &&
        compare_timestamps(read(uint64_t, p, 0x98), read(uint64_t, p, 0x40),
                           read(uint32_t, p, 0x60), read(uint32_t, p, 0x60), 1) == AZ_TIME_AFTER) {
        out->end_s = (double) (read(uint64_t, p, 0x98)) / read(uint32_t, p, 0xb8);
        out->valid |= AZ_CUE_END;
        out->type = AZ_CUE_LOOP;
    }
    return true;
}

/* -1 malformed, 0 native absent HOT slot, 1 populated source-qualified row. */
static bool absent_hot_cue(const azcs_owned_cue *cue) {
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    return cue->native.bytes[0] == 0xff || read(uint64_t, &cue->native, 8) == absent || read(uint64_t, &cue->native, 0x10) == absent;
}
static bool valid_cue_kind(bool hot_collection, unsigned kind) {
    return hot_collection ? kind >= 1 && kind <= 8 : kind == 0;
}
static void scalar_metadata(const azcs_owned_cue *cue, unsigned kind, size_t ordinal, struct az_cue_entry *out) {
    (*out) = (__typeof__((*out))) {};
    out->size = sizeof(*out);
    out->version = 1;
    out->saved = 1;
    out->valid = AZ_CUE_SAVED;
    out->classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
    out->slot = kind;
    out->entry_id = kind ? kind : (uint32_t) (ordinal + 1);
    out->rgb = read(uint32_t, &cue->native, 0x78) & 0xffffff;
    out->valid |= AZ_CUE_COLOR;
    out->auto_active = !!cue->native.bytes[0x28];
    out->valid |= AZ_CUE_AUTO_ACTIVE;
    uintptr_t text = read(uintptr_t, &cue->native, 0x70);
    if (text) {
        azcue_copy_utf8_prefix(out->comment, ((const char *) (text)));
        out->valid |= AZ_CUE_COMMENT;
    }
}
static int scalar(azcs_snapshot *snapshot, size_t ordinal, struct az_cue_entry *out, azcs_property *selected_property) {
    azcs_owned_cue cue __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    azcs_property temporary __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_property *property = selected_property ? selected_property : &temporary;
    if (!azcs_snapshot_row(snapshot, ordinal, &(cue))) return -1;
    unsigned kind = cue.native.bytes[0];
    const bool hot_collection = ordinal >= azcs_snapshot_memory_count(snapshot);
    /* Exact native validStoredCue guard. Only its fixed HOT collection may
     * contain absent slots; populated conversion/source failures are errors. */
    if (hot_collection && absent_hot_cue(&cue)) return 0;
    if (!valid_cue_kind(hot_collection, kind) || !azcs_snapshot_property(snapshot, (uint32_t) (ordinal + 1), azcs_snapshot_revision(snapshot), property)) return -1;
    scalar_metadata(&cue, kind, ordinal, out);
    return property_scalar(&(property->native), &((*out))) ? 1 : -1;
}
static bool snapshot_copy(azcs_snapshot *snapshot, unsigned deck, uint64_t generation, struct az_native_cue_snapshot *out) {
    (*out) = (__typeof__((*out))) {};
    out->size = sizeof(*out);
    out->version = 1;
    out->deck = deck;
    out->native_load_generation = generation;
    out->revision = azcs_snapshot_revision(snapshot);
    out->source_rate_hz = azcs_snapshot_source_rate(snapshot);
    for (size_t i = 0; i < azcs_snapshot_count(snapshot); ++i) {
        int status = scalar(snapshot, i, &(out->entries[out->count]), NULL);
        if (status < 0) return false;
        if (status > 0) ++out->count;
    }
    return true;
}
static void publish(unsigned deck, const struct az_native_cue_snapshot *snapshot) {
    Cache *cache = &(state().decks[deck]);
    AZ_MUTEX_LOCK(lock, cache->mutex);
    cache->value = (*snapshot);
    cache->observed = now();
}
static int validate_persist(const struct azexit_cue_request *request, void *manager, uint64_t cookie, void *unused) {
    (void) unused;
    if (request->identity.deck >= 4 || !current(request->identity.deck, request->identity.load_generation, NULL)) {
        COMMAND_DIAG(*request, 6, "persist-current");
        return 0;
    }
    Pending *pending = &(state().pending[request->identity.deck]);
    return cookie && atomic_load_explicit(&(pending->cookie), memory_order_acquire) == cookie &&
           atomic_load_explicit(&(pending->token), memory_order_acquire) == request->identity.token &&
           atomic_load_explicit(&(pending->manager), memory_order_acquire) == (uintptr_t) (manager) && atomic_load_explicit(&(pending->cookie), memory_order_acquire) == cookie;
}
static bool same_point(const struct az_cue_entry *a, const struct az_cue_entry *b, uint32_t rate) {
    return a->classification == b->classification && a->slot == b->slot && a->type == b->type &&
           compare_timestamps(a->start_s, b->start_s, 1, rate, 1) == AZ_TIME_EQUAL &&
           (!(a->valid & AZ_CUE_END) || compare_timestamps(a->end_s, b->end_s, 1, rate, 1) == AZ_TIME_EQUAL);
}
static bool staged_point(const azcs_native_property *property, const struct az_cue_entry *entry, const struct azexit_cue_stage_sample *captured) {
    const unsigned char *p = property->bytes;
    return captured->sample_rate && read(uint32_t, p, 0x60) == captured->sample_rate &&
           !memcmp(p + 0x30, captured->source_id, 12) &&
           (bool) (entry->valid & AZ_CUE_END) == (bool) (captured->out_valid) &&
           compare_timestamps(read(uint64_t, p, 0x40), captured->in_sample, captured->sample_rate, captured->sample_rate, 1) == AZ_TIME_EQUAL &&
           (!captured->out_valid || (read(uint32_t, p, 0xb8) == captured->sample_rate &&
                                     !memcmp(p + 0x88, captured->source_id, 12) &&
                                     compare_timestamps(read(uint64_t, p, 0x98), captured->out_sample, captured->sample_rate, captured->sample_rate, 1) == AZ_TIME_EQUAL));
}
static bool creating(unsigned operation);
typedef struct CueConfirmation {
    const struct azexit_cue_request *request;
    void *manager;
    uint64_t cookie;
    unsigned deck;
    Pending *pending;
    azcs_snapshot *snapshot;
    struct az_native_cue_snapshot *copied;
} CueConfirmation;
static bool confirm_metadata(CueConfirmation *c) {
    const struct azexit_cue_request *r = c->request;
    struct azcuepersist_witness fresh = {0};
    unsigned kind = r->classification == AZ_CUE_HOT ? r->entry_id : 0;
    return azcs_snapshot_content_witness(c->snapshot, azcs_snapshot_revision(c->snapshot), &fresh, false) &&
           azsdk_cue_persist_confirm_publication(r, &fresh) &&
           azcs_snapshot_metadata_only_changed(c->snapshot, &c->pending->before_witness, kind, r->entry_id,
                                               r->operation == AZ_CMD_CUE_COMMENT ? r->comment : NULL, r->rgb, c->pending->expected_rate) &&
           validate_persist(r, c->manager, c->cookie, NULL);
}
static bool one_automatic_target(CueConfirmation *c) {
    unsigned active = 0;
    for (unsigned i = 0; i < c->copied->count; ++i) {
        const struct az_cue_entry *row = &c->copied->entries[i];
        if (row->classification == AZ_CUE_MEMORY && row->type == AZ_CUE_LOOP && row->auto_active) {
            if (row->entry_id != c->pending->automatic.target.entry_id) return false;
            ++active;
        }
    }
    return active == 1;
}
static bool confirm_automatic(CueConfirmation *c) {
    const struct azexit_cue_request *r = c->request;
    struct azcuepersist_witness fresh = {0};
    if (!azcs_snapshot_content_witness(c->snapshot, azcs_snapshot_revision(c->snapshot), &fresh, false) ||
        !azsdk_cue_persist_confirm_publication(r, &fresh) ||
        !azcs_snapshot_auto_only_changed(c->snapshot, &c->pending->before_witness, r->entry_id, r->position_s != 0, c->pending->expected_rate)) return false;
    AutoPlan *plan = &c->pending->automatic;
    return !(plan->active && plan->token == r->identity.token && plan->final) || one_automatic_target(c);
}
static bool confirm_creation(CueConfirmation *c) {
    const struct azexit_cue_request *r = c->request;
    struct azcuepersist_witness fresh = {0};
    unsigned kind = r->classification == AZ_CUE_HOT ? r->entry_id : 0;
    return azcs_snapshot_content_witness(c->snapshot, azcs_snapshot_revision(c->snapshot), &fresh, true) &&
           azsdk_cue_persist_confirm_publication(r, &fresh) &&
           azcs_snapshot_creation_only_changed(c->snapshot, &c->pending->before_witness, kind, &c->pending->retained, c->pending->expected_rate, r->comment, r->rgb);
}
static bool confirmation_target(CueConfirmation *c, const struct az_cue_entry *row) {
    const struct az_cue_entry *expected = &c->pending->expected;
    return expected->classification == AZ_CUE_HOT ? row->classification == AZ_CUE_HOT && row->slot == expected->slot : same_point(row, expected, c->pending->expected_rate);
}
static bool confirmation_effect(CueConfirmation *c, const struct az_cue_entry *row) {
    const struct azexit_cue_request *r = c->request;
    if (r->operation == AZ_CMD_CUE_COMMENT) return !strcmp(row->comment, r->comment);
    if (r->operation == AZ_CMD_CUE_COLOR) return row->rgb == r->rgb;
    if (r->operation == AZ_CMD_SAVED_AUTO) return row->auto_active == (int) r->position_s;
    if (r->operation == AZ_CMD_HOT_SET) return same_point(row, &c->pending->expected, c->pending->expected_rate) && row->rgb == r->rgb;
    return same_point(row, &c->pending->expected, c->pending->expected_rate);
}
static bool confirm_effect(CueConfirmation *c) {
    bool exists = false, effect = false;
    for (unsigned i = 0; i < c->copied->count; ++i) {
        const struct az_cue_entry *row = &c->copied->entries[i];
        if (!confirmation_target(c, row)) continue;
        exists = true;
        effect = confirmation_effect(c, row);
    }
    unsigned op = c->request->operation;
    bool deletion = op == AZ_CMD_SAVED_DELETE || op == AZ_CMD_MEMORY_DELETE || op == AZ_CMD_HOT_CLEAR;
    return deletion ? !exists : effect;
}
static bool confirm_copied(CueConfirmation *c) {
    if (!snapshot_copy(c->snapshot, c->deck, c->request->identity.load_generation, c->copied) || !validate_persist(c->request, c->manager, c->cookie, NULL)) return false;
    COMMAND_DIAG(*c->request, 16, "confirm-copied");
    if (c->request->operation == AZ_CMD_SAVED_AUTO && !confirm_automatic(c)) return false;
    return creating(c->request->operation) ? confirm_creation(c) : confirm_effect(c);
}
static int confirm_persist(const struct azexit_cue_request *request, void *manager, uint64_t cookie, uint64_t *revision, void *unused) {
    (void) unused;
    if (!validate_persist(request, manager, cookie, NULL)) return 0;
    State *s = &state();
    unsigned deck = request->identity.deck;
    azcs_snapshot snapshot __attribute__((cleanup(azcs_snapshot_reset))) = {0};
    struct az_native_cue_snapshot copied;
    if (!azcs_snapshot_open(&snapshot, &s->store, manager, request->identity.load_generation, NULL, &s->decks[deck].revision)) return 0;
    COMMAND_DIAG(*request, 15, "confirm-open");
    CueConfirmation c = {request, manager, cookie, deck, &s->pending[deck], &snapshot, &copied};
    if (request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) {
        if (!confirm_metadata(&c)) return 0;
        // The typed collection and receipt prove the edit independently of public position conversion.
        COMMAND_DIAG(*request, 17, "confirm-metadata");
        *revision = azcs_snapshot_revision(&snapshot);
        return 1;
    }
    if (!confirm_copied(&c)) return 0;
    publish(deck, &copied);
    *revision = copied.revision;
    return 1;
}
static bool creating(unsigned operation) { return operation == AZ_CMD_CUE_POINT_SET || operation == AZ_CMD_CUE_LOOP_SET; }
static bool selector(unsigned operation) {
    return operation == AZ_CMD_SAVED_RECALL || operation == AZ_CMD_SAVED_DELETE || operation == AZ_CMD_SAVED_AUTO ||
           operation == AZ_CMD_MEMORY_RECALL || operation == AZ_CMD_MEMORY_DELETE ||
           operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR;
}
static bool hot(unsigned operation) { return operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR; }
static bool recall(unsigned operation) { return operation == AZ_CMD_SAVED_RECALL || operation == AZ_CMD_MEMORY_RECALL; }
static int command_fail(Pending *pending, const struct azexit_cue_request *request, struct azexit_result *result, int reason) {
    AutoPlan *plan = &(pending->automatic);
    if (plan->active && plan->token == request->identity.token && plan->effect_seen) {
        *result = (__typeof__(*result)) {request->identity.token, request->identity.load_generation, 0, AZ_COMMAND_UNCERTAIN, reason};
        plan->active = false;
        return AZ_OK;
    }
    return reason;
}
static int resume_hot_completion(unsigned deck, const struct azexit_cue_request *request) {
    (void) deck;
    (void) request;

        // The native PlayerState copy and the setter's automatic store work
        // can lag DSP completion. Keep the original deadline and capture.
#ifdef AZSDK_CUE_COMMANDS_TESTING
    ready(deck, request->identity.token, NULL);
#endif
    return AZ_PENDING;
}
typedef struct CommandStep {
    bool done;
    int status;
} CommandStep;
typedef struct CueExecution {
    State *state;
    Pending *pending;
    unsigned deck, kind;
    bool awaiting_hot_view;
    const struct azexit_cue_request *request, *native_request;
    void *manager;
    struct azexit_result *result;
    azcs_snapshot *snapshot;
    azcs_property *property, *canonical;
    azcs_owned_cue *selected_stored;
    struct az_cue_entry *expected;
    struct azexit_cue_request *automatic_step;
    struct azcuepersist_witness *witness;
    const struct azcuepersist_witness *selected_witness;
} CueExecution;
static CommandStep complete_step(int status) { return (CommandStep) {true, status}; }
static CommandStep hydrate_created_hot(CueExecution *e) {
    // Storage completion proves persistence; hydrate the playable Hot bank on the existing DSP handoff.
    const uint64_t saved_revision = e->result->revision;
    int next = azsdk_cue_recall_prepare(e->request, e->manager, &e->pending->retained.native);
    azcs_property_reset(&e->pending->retained);
    if (next == AZ_PENDING) {
        e->pending->active_request = *e->request;
        atomic_store_explicit(&e->pending->bridge_generation, e->request->identity.load_generation, memory_order_relaxed);
        atomic_store_explicit(&e->pending->bridge_revision, saved_revision, memory_order_relaxed);
        atomic_store_explicit(&e->pending->bridge, e->request->identity.token, memory_order_release);
        if (atomic_load_explicit(&e->pending->bridge_ready, memory_order_acquire) == e->request->identity.token) azsdk_cue_commands_notify(e->deck);
        return complete_step(AZ_PENDING);
    }
    *e->result = (__typeof__(*e->result)) {e->request->identity.token, e->request->identity.load_generation, 0, AZ_COMMAND_UNCERTAIN, next};
    return complete_step(AZ_OK);
}
static CommandStep resume_automatic_receipt(CueExecution *e, int rc) {
    AutoPlan *plan = &(e->pending->automatic);
    if (!plan->active || plan->token != e->request->identity.token) return complete_step(rc);
    if (rc != AZ_OK) return complete_step(command_fail(e->pending, e->request, e->result, rc));
    if (e->result->outcome != AZ_COMMAND_APPLIED || !e->result->revision) {
        if (plan->effect_seen) e->result->outcome = AZ_COMMAND_UNCERTAIN;
        plan->active = false;
        return complete_step(AZ_OK);
    }
    if (plan->final) {
        plan->active = false;
        return complete_step(AZ_OK);
    }
    plan->effect_seen = true;
    plan->revision = e->result->revision;
        // Continue on this owned frame only after this exact native receipt.
        // The next step must still have the original public deadline/source.

    return (CommandStep) {0};
}
static bool created_hot_applied(CueExecution *e, int rc) {
    return creating(e->request->operation) && e->request->classification == AZ_CUE_HOT && rc == AZ_OK && e->result->outcome == AZ_COMMAND_APPLIED;
}
static CommandStep resume_persistence(CueExecution *e) {
    int rc = azsdk_cue_persist_take(&e->pending->active_request, e->manager, e->result);
    if (rc == AZ_PENDING) return complete_step(rc);
    atomic_store_explicit(&e->pending->persist_ready, 0, memory_order_release);
    atomic_store_explicit(&e->pending->cookie, 0, memory_order_release);
    atomic_store_explicit(&e->pending->stage, 0, memory_order_release);
    if (created_hot_applied(e, rc)) return hydrate_created_hot(e);
    azcs_property_reset(&e->pending->retained);
    return resume_automatic_receipt(e, rc);
}
static CommandStep admit_execution(CueExecution *e) {
    uint64_t clock = now();
    if (!clock || compare_timestamps(clock, e->request->identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        COMMAND_DIAG(*e->request, 1, "execute-deadline");
        return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    }
    if (!current(e->deck, e->request->identity.load_generation, NULL)) {
        COMMAND_DIAG(*e->request, 2, "execute-current");
        return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    }
    if (e->request->operation) {
        int allowed = azsdk_exit_delivery_continue(&e->request->identity, clock);
        if (allowed != AZ_OK) return complete_step(command_fail(e->pending, e->request, e->result, allowed));
    }

    return (CommandStep) {0};
}
static CommandStep refresh_cached_snapshot(CueExecution *e) {
    Cache *cache = &(e->state->decks[e->deck]);
    AZ_MUTEX_LOCK(lock, cache->mutex);
    if (cache->value.native_load_generation == e->request->identity.load_generation &&
        cache->value.revision && cache->value.revision == azcs_snapshot_revision(e->snapshot) &&
        cache->value.source_rate_hz == azcs_snapshot_source_rate(e->snapshot)) {
        if (!current(e->deck, e->request->identity.load_generation, NULL)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
        cache->observed = now();
        *e->result = (__typeof__(*e->result)) {e->request->identity.token, e->request->identity.load_generation, azcs_snapshot_revision(e->snapshot), AZ_COMMAND_APPLIED, AZ_OK};
        return complete_step(AZ_OK);
    }
    return (CommandStep) {0};
}
static CommandStep publish_snapshot_query(CueExecution *e) {
    CommandStep step = refresh_cached_snapshot(e);
    if (step.done) return step;
    struct az_native_cue_snapshot copied;
    if (!snapshot_copy(e->snapshot, e->deck, e->request->identity.load_generation, &(copied))) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (!current(e->deck, e->request->identity.load_generation, NULL)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    publish(e->deck, &(copied));
    e->result->token = e->request->identity.token;
    e->result->load_generation = e->request->identity.load_generation;
    e->result->revision = azcs_snapshot_revision(e->snapshot);
    e->result->outcome = AZ_COMMAND_APPLIED;
    e->result->reason = AZ_OK;
    return complete_step(AZ_OK);
}
static CommandStep open_execution_snapshot(CueExecution *e) {
    if (!azcs_snapshot_open(e->snapshot, &(e->state->store), e->manager, e->request->identity.load_generation, NULL, &(e->state->decks[e->deck].revision))) {
        COMMAND_DIAG(*e->request, 3, "snapshot-open");
        if (e->awaiting_hot_view) return complete_step(resume_hot_completion(e->deck, e->request));
        return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    }
    if (!current(e->deck, e->request->identity.load_generation, NULL)) {
        COMMAND_DIAG(*e->request, 4, "snapshot-after-current");
        return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    }
    COMMAND_DIAG(*e->request, 0, "snapshot-admitted");
    if (e->request->operation == 0) return publish_snapshot_query(e);
    return (CommandStep) {0};
}
static void find_automatic_target(AutoPlan *plan, const struct az_native_cue_snapshot *cues, uint32_t entry) {
    for (unsigned i = 0; i < cues->count; ++i) {
        const struct az_cue_entry *row = &cues->entries[i];
        if (row->classification == AZ_CUE_MEMORY && row->entry_id == entry) {
            plan->target = *row;
            break;
        }
    }
}
static CommandStep initialize_automatic_plan(CueExecution *e, const struct az_native_cue_snapshot *cues) {
    AutoPlan *plan = &e->pending->automatic;
    if (azcs_snapshot_revision(e->snapshot) != e->request->expected_revision) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    *plan = (AutoPlan) {0};
    find_automatic_target(plan, cues, e->request->entry_id);
    if (plan->target.type != AZ_CUE_LOOP || !(plan->target.valid & AZ_CUE_END)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_INVALID));
    azcs_property target_property __attribute__((cleanup(azcs_property_reset))) = {0};
    if (!azcs_snapshot_property(e->snapshot, plan->target.entry_id, azcs_snapshot_revision(e->snapshot), &target_property)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    plan->target_rate = read(uint32_t, &target_property.native, 0x60);
    plan->active = true;
    plan->token = e->request->identity.token;
    plan->revision = azcs_snapshot_revision(e->snapshot);
    return (CommandStep) {0};
}
static bool automatic_target_matches(const struct az_cue_entry *row, const AutoPlan *plan) {
    return same_point(row, &plan->target, plan->target_rate) && row->rgb == plan->target.rgb && !strcmp(row->comment, plan->target.comment);
}
static bool automatic_other(const struct az_native_cue_snapshot *cues, const AutoPlan *plan, unsigned *other) {
    bool target_present = false;
    for (unsigned i = 0; i < cues->count; i++) {
        const struct az_cue_entry *row = &cues->entries[i];
        if (row->classification != AZ_CUE_MEMORY || row->type != AZ_CUE_LOOP) continue;
        if (row->entry_id == plan->target.entry_id) target_present = automatic_target_matches(row, plan);
        else if (row->auto_active && !*other) *other = row->entry_id;
    }
    return target_present;
}
static CommandStep select_automatic_step(CueExecution *e, const struct az_native_cue_snapshot *cues) {
    AutoPlan *plan = &e->pending->automatic;
    if (azcs_snapshot_revision(e->snapshot) != plan->revision) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    unsigned other = 0;
    if (!automatic_other(cues, plan, &other)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    if (plan->steps >= AZ_NATIVE_CUE_LIMIT) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    *e->automatic_step = *e->request;
    e->automatic_step->entry_id = other ? other : plan->target.entry_id;
    e->automatic_step->expected_revision = plan->revision;
    e->automatic_step->position_s = other ? 0 : 1;
    plan->final = !other;
    e->native_request = e->automatic_step;
    return (CommandStep) {0};
}
static CommandStep prepare_automatic_step(CueExecution *e) {
    if (e->request->operation != AZ_CMD_SAVED_AUTO || e->request->position_s != 1) return (CommandStep) {0};
    AutoPlan *plan = &e->pending->automatic;
    struct az_native_cue_snapshot cues = {0};
    if (!snapshot_copy(e->snapshot, e->deck, e->request->identity.load_generation, &cues)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (!plan->active || plan->token != e->request->identity.token) {
        CommandStep step = initialize_automatic_plan(e, &cues);
        if (step.done) return step;
    }
    return select_automatic_step(e, &cues);
}
static bool hot_stage_complete(CueExecution *e) {
    return atomic_load_explicit(&e->pending->stage_token, memory_order_acquire) == e->request->identity.token &&
           atomic_load_explicit(&e->pending->stage, memory_order_acquire) == 2;
}
static bool creation_times_valid(const struct azexit_cue_request *r, uint32_t rate) {
    const int start = compare_timestamps(r->position_s, 0., 1, rate, 1);
    const int end = compare_timestamps(r->end_s, r->position_s, 1, rate, 1);
    const int unused = compare_timestamps(r->end_s, 0., 1, rate, 1);
    if (start == AZ_TIME_INVALID || start == AZ_TIME_BEFORE) return false;
    if (r->operation == AZ_CMD_CUE_LOOP_SET) return end != AZ_TIME_INVALID && end == AZ_TIME_AFTER;
    return unused != AZ_TIME_INVALID && unused == AZ_TIME_EQUAL;
}
static int creation_row_available(CueExecution *e, const struct az_cue_entry *row, uint32_t rate, unsigned *memory) {
    if (e->kind && row->classification == AZ_CUE_HOT && row->slot == e->kind) return AZ_LIMIT;
    if (row->classification != AZ_CUE_MEMORY) return AZ_OK;
    ++*memory;
    int equal = compare_timestamps(row->start_s, e->request->position_s, 1, rate, 1);
    if (equal == AZ_TIME_INVALID) return AZ_UNSUPPORTED;
    return !e->kind && equal == AZ_TIME_EQUAL ? AZ_LIMIT : AZ_OK;
}
static int creation_space(CueExecution *e, uint32_t rate) {
    azcs_property live_hot __attribute__((cleanup(azcs_property_reset))) = {0};
    if (e->kind && azcs_snapshot_current_property(e->snapshot, e->kind, &live_hot)) return AZ_LIMIT;
    struct az_native_cue_snapshot stored = {0};
    if (!snapshot_copy(e->snapshot, e->deck, e->request->identity.load_generation, &stored)) return AZ_UNSUPPORTED;
    unsigned memory = 0;
    for (unsigned i = 0; i < stored.count; ++i) {
        int rc = creation_row_available(e, &stored.entries[i], rate, &memory);
        if (rc != AZ_OK) return rc;
    }
    return !e->kind && memory >= 10 ? AZ_LIMIT : AZ_OK;
}
static CommandStep prepare_creation_property(CueExecution *e) {
    if (azcs_snapshot_revision(e->snapshot) != e->request->expected_revision) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    uint32_t rate = azcs_snapshot_source_rate(e->snapshot);
    if (!rate) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (!creation_times_valid(e->request, rate)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_INVALID));
    int rc = creation_space(e, rate);
    if (rc != AZ_OK) return complete_step(command_fail(e->pending, e->request, e->result, rc));
    if (!azcs_snapshot_timed_property(e->snapshot, e->kind, e->request->position_s, e->request->end_s,
                                      e->request->operation == AZ_CMD_CUE_LOOP_SET, e->request->rgb, e->request->comment, e->property) ||
        !property_scalar(&e->property->native, e->expected)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    e->expected->classification = e->request->classification;
    e->expected->slot = e->kind;
    return (CommandStep) {0};
}
static CommandStep prepare_staged_property(CueExecution *e) {
    if (e->request->operation == AZ_CMD_HOT_CLEAR) {
        if (!azcs_property_copy_to(&e->pending->retained, e->property)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
        *e->expected = e->pending->expected;
    } else {
        if (!azcs_snapshot_current_property(e->snapshot, e->kind, e->property) || !property_scalar(&e->property->native, e->expected) ||
            !staged_point(&e->property->native, e->expected, &e->pending->staged_cue)) return complete_step(resume_hot_completion(e->deck, e->request));
        e->expected->classification = AZ_CUE_HOT;
        e->expected->slot = e->kind;
    }
    return (CommandStep) {0};
}
static int selected_ordinal(CueExecution *e, size_t *ordinal) {
    // Memory IDs are revision-scoped ordinals; Hot IDs are native slot bytes.
    // Resolve the identity before performing source seek work for one property.
    *ordinal = azcs_snapshot_count(e->snapshot);
    if (e->native_request->classification == AZ_CUE_MEMORY) {
        if (e->native_request->entry_id && e->native_request->entry_id <= azcs_snapshot_memory_count(e->snapshot)) *ordinal = e->native_request->entry_id - 1;
    } else {
        for (size_t i = azcs_snapshot_memory_count(e->snapshot); i < azcs_snapshot_count(e->snapshot); ++i) {
            azcs_owned_cue row __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
            if (!azcs_snapshot_row(e->snapshot, i, &row)) return AZ_STALE;
            if (row.native.bytes[0] == e->native_request->entry_id) {
                *ordinal = i;
                break;
            }
        }
    }
    return *ordinal == azcs_snapshot_count(e->snapshot) ? AZ_STALE : AZ_OK;
}
static bool modifies_selected(unsigned operation) {
    return operation == AZ_CMD_SAVED_AUTO || operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR;
}
static bool requires_saved_loop(unsigned operation) {
    return operation == AZ_CMD_SAVED_RECALL || operation == AZ_CMD_SAVED_DELETE || operation == AZ_CMD_SAVED_AUTO;
}
static CommandStep validate_selected_property(CueExecution *e, size_t ordinal) {
    int status = scalar(e->snapshot, ordinal, e->expected, e->property);
    if (status < 0) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (!status || e->expected->classification != e->native_request->classification || e->expected->entry_id != e->native_request->entry_id) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    COMMAND_DIAG(*e->request, 11, "selected-property");
    if (modifies_selected(e->request->operation) && !azcs_snapshot_row(e->snapshot, ordinal, e->selected_stored)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    e->kind = e->expected->slot;
    if (requires_saved_loop(e->request->operation) && !(e->expected->valid & AZ_CUE_END)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_INVALID));
    return (CommandStep) {0};
}
static CommandStep prepare_selected_property(CueExecution *e) {
    if (selector(e->request->operation) && e->native_request->expected_revision != azcs_snapshot_revision(e->snapshot)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    size_t ordinal;
    int rc = selected_ordinal(e, &ordinal);
    if (rc != AZ_OK) return complete_step(command_fail(e->pending, e->request, e->result, rc));
    return validate_selected_property(e, ordinal);
}
static CommandStep prepare_current_property(CueExecution *e) {
    if (!azcs_snapshot_current_property(e->snapshot, 0, e->property) || !property_scalar(&e->property->native, e->expected)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    e->expected->classification = e->kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
    e->expected->slot = e->kind;
    return (CommandStep) {0};
}
static CommandStep choose_execution_property(CueExecution *e) {
    if (creating(e->request->operation)) return prepare_creation_property(e);
    if (hot(e->request->operation) && hot_stage_complete(e)) return prepare_staged_property(e);
    if (e->request->operation == AZ_CMD_HOT_SET) {
        azcs_property_reset(&e->pending->retained); // DSP captures its playhead independently of the main cue.
        return (CommandStep) {0};
    }
    if (selector(e->request->operation) || e->request->operation == AZ_CMD_HOT_CLEAR) return prepare_selected_property(e);
    return prepare_current_property(e);
}
static CommandStep prepare_execution_property(CueExecution *e) {
    if (e->request->operation == AZ_CMD_HOT_CLEAR && (e->request->flags & AZ_COMMAND_NATIVE_CUE_IDS) &&
        !hot_stage_complete(e) && azcs_snapshot_revision(e->snapshot) != e->request->expected_revision) return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
    return choose_execution_property(e);
}
static CommandStep dispatch_hot_stage(CueExecution *e) {
    if (e->request->operation == AZ_CMD_HOT_CLEAR && !azcs_property_copy_to(e->property, &(e->pending->retained))) return complete_step(command_fail(e->pending, e->request, e->result, AZ_FAILED));
    e->pending->expected = (*e->expected);
    int rc = azsdk_exit_cue_stage_submit(e->request);
    if (rc == AZ_PENDING) {
        e->pending->staged_cue = (__typeof__(e->pending->staged_cue)) {};
        e->pending->active_request = *e->request; // publish the operation with this exact bridge token
        atomic_store_explicit(&(e->pending->bridge_generation), e->request->identity.load_generation, memory_order_relaxed);
        atomic_store_explicit(&(e->pending->bridge_revision), 0, memory_order_relaxed);
        atomic_store_explicit(&(e->pending->stage_token), e->request->identity.token, memory_order_release);
        atomic_store_explicit(&(e->pending->stage), 1, memory_order_release);
        atomic_store_explicit(&(e->pending->bridge), e->request->identity.token, memory_order_release);
        if (atomic_load_explicit(&(e->pending->bridge_ready), memory_order_acquire) == e->request->identity.token)
            azsdk_cue_commands_notify(e->deck);
    }
    return complete_step(rc);
}
static CommandStep dispatch_selected_recall(CueExecution *e) {
    int rc = azsdk_cue_recall_prepare(e->request, e->manager, &e->property->native);
    if (rc == AZ_PENDING) {
            /* DSP serials are a separate namespace. The selected, source-qualified
             * copied property belongs to this exact store snapshot revision. */
        e->pending->active_request = *e->request; // publish the operation with this exact bridge token
        atomic_store_explicit(&(e->pending->bridge_generation), e->request->identity.load_generation, memory_order_relaxed);
        atomic_store_explicit(&(e->pending->bridge_revision), azcs_snapshot_revision(e->snapshot), memory_order_relaxed);
        atomic_store_explicit(&(e->pending->bridge), e->request->identity.token, memory_order_release);
        if (atomic_load_explicit(&(e->pending->bridge_ready), memory_order_acquire) == e->request->identity.token)
            azsdk_cue_commands_notify(e->deck);
    }
    return complete_step(rc);
}
static CommandStep dispatch_execution_action(CueExecution *e) {
    if (hot(e->request->operation) && !hot_stage_complete(e)) return dispatch_hot_stage(e);
    if (e->request->operation == AZ_CMD_SAVED_SAVE && !(e->expected->valid & AZ_CUE_END)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_INVALID));
    if (recall(e->request->operation)) return dispatch_selected_recall(e);
    return (CommandStep) {0};
}
static bool canonical_times_match(CueExecution *e) {
    const uint32_t rate = azcs_snapshot_source_rate(e->snapshot);
    const int start = compare_timestamps(e->expected->start_s, e->request->position_s, 1, rate, 1);
    const int end = compare_timestamps(e->expected->end_s, e->request->end_s, 1, rate, 1);
    if (start == AZ_TIME_INVALID || start != AZ_TIME_EQUAL ||
        (e->request->operation == AZ_CMD_CUE_LOOP_SET && (end == AZ_TIME_INVALID || end != AZ_TIME_EQUAL))) return false;
    return true;
}
static bool canonical_native_matches(CueExecution *e) {
    const uint32_t rate = azcs_snapshot_source_rate(e->snapshot);
        // Keep the original native label for live Hot hydration: the stock
        // Property-to-Cue serializer intentionally emits an empty comment.
    const int same_in = compare_timestamps(read(uint64_t, &e->property->native, 0x40), read(uint64_t, &e->canonical->native, 0x40), rate, rate, 1);
    const int same_out = e->request->operation == AZ_CMD_CUE_LOOP_SET ? compare_timestamps(read(uint64_t, &e->property->native, 0x98), read(uint64_t, &e->canonical->native, 0x98), rate, rate, 1) : AZ_TIME_EQUAL;
    if (same_in == AZ_TIME_INVALID || same_in != AZ_TIME_EQUAL ||
        (e->request->operation == AZ_CMD_CUE_LOOP_SET && (same_out == AZ_TIME_INVALID || same_out != AZ_TIME_EQUAL))) return false;
    return true;
}
static CommandStep canonicalize_execution_property(CueExecution *e) {
    if (!azcs_snapshot_canonical_property(e->snapshot, e->kind, e->property, e->canonical)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    COMMAND_DIAG(*e->request, 12, "canonical-property");
    e->expected->valid &= ~(AZ_CUE_START | AZ_CUE_END);
    e->expected->start_s = e->expected->end_s = 0;
    if (!property_scalar(&(e->canonical->native), e->expected)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (e->request->operation == AZ_CMD_SAVED_SAVE && !(e->expected->valid & AZ_CUE_END)) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (creating(e->request->operation) && (!canonical_times_match(e) || !canonical_native_matches(e))) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    return (CommandStep) {0};
}
static CommandStep prepare_execution_witness(CueExecution *e) {
    if (modifies_selected(e->request->operation) || e->request->operation == AZ_CMD_HOT_SET || creating(e->request->operation)) {
        uint64_t revision = e->request->operation == AZ_CMD_HOT_SET ? azcs_snapshot_revision(e->snapshot) : e->native_request->expected_revision;
        if (!azcs_snapshot_content_witness(e->snapshot, revision, e->witness, creating(e->request->operation))) {
            COMMAND_DIAG(*e->request, 5, "snapshot-witness");
            // HOT_SET's own asynchronous store can publish between the DSP
            // capture and this witness. No repository request exists yet;
            // refresh the view without repeating the setter or its deadline.
            if (e->awaiting_hot_view) return complete_step(resume_hot_completion(e->deck, e->request));
            return complete_step(command_fail(e->pending, e->request, e->result, AZ_STALE));
        }
        if (e->request->operation != AZ_CMD_HOT_SET && !creating(e->request->operation)) e->witness->selected_cue = ((uintptr_t) (&e->selected_stored->native));
        e->selected_witness = e->witness;
        COMMAND_DIAG(*e->request, 13, "content-witness");
    }

    return (CommandStep) {0};
}
static bool persistence_can_retry(unsigned operation, int status) {
    return hot(operation) && (status == AZ_BUSY || (operation == AZ_CMD_HOT_SET && status == AZ_STALE));
}
static CommandStep persistence_submitted(CueExecution *e, int rc) {
    if (rc != AZ_PENDING) {
        atomic_store_explicit(&(e->pending->cookie), 0, memory_order_release);
        // Busy or stale preflight produced no repository request. A native
        // automatic store can invalidate the captured content witness between
        // preparation and admission. Recheck the load, captured property and
        // fresh witness on the next owned frame, keeping this DSP stage and
        // the original deadline rather than repeating the setter.
        if (persistence_can_retry(e->request->operation, rc)) return complete_step(resume_hot_completion(e->deck, e->request));
        return complete_step(command_fail(e->pending, e->request, e->result, rc));
    }
    if (e->pending->automatic.active && e->pending->automatic.token == e->request->identity.token) ++e->pending->automatic.steps;
    return complete_step(rc);
}
static CommandStep submit_execution_persistence(CueExecution *e) {
    uint64_t cookie = atomic_fetch_add_explicit(&(e->state->serial), 1, memory_order_relaxed) + 1;
    if (!cookie || cookie >= INTERNAL) return complete_step(command_fail(e->pending, e->request, e->result, AZ_UNSUPPORTED));
    if (creating(e->request->operation) && !azcs_property_copy_to(e->property, &(e->pending->retained))) return complete_step(command_fail(e->pending, e->request, e->result, AZ_FAILED));
    e->pending->expected = (*e->expected);
    e->pending->active_request = *e->native_request;
    e->pending->expected_rate = read(uint32_t, &e->canonical->native, 0x60);
    e->pending->before_witness = (*e->witness);
    e->pending->before_revision = azcs_snapshot_revision(e->snapshot);
    atomic_store_explicit(&(e->pending->token), e->request->identity.token, memory_order_relaxed);
    atomic_store_explicit(&(e->pending->manager), (uintptr_t) (e->manager), memory_order_relaxed);
    atomic_store_explicit(&(e->pending->cookie), cookie, memory_order_release);
    uintptr_t maintainer = read(uintptr_t, e->manager, 0x140);
    COMMAND_DIAG(*e->request, 14, "persist-submit");
    int rc = azsdk_cue_persist_submit(&e->pending->active_request, e->manager, ((void *) (maintainer)), e->kind, &e->property->native, cookie, e->selected_witness);
    return persistence_submitted(e, rc);
}
static int execute_prepared(CueExecution *e) {
    CommandStep step = canonicalize_execution_property(e);
    if (step.done) return step.status;
    step = prepare_execution_witness(e);
    if (step.done) return step.status;
    return submit_execution_persistence(e).status;
}
static int execute_frame(CueExecution *e) {
    CommandStep step = admit_execution(e);
    if (step.done) return step.status;
    e->awaiting_hot_view = e->request->operation == AZ_CMD_HOT_SET &&
                           atomic_load_explicit(&e->pending->stage_token, memory_order_acquire) == e->request->identity.token &&
                           atomic_load_explicit(&e->pending->stage, memory_order_acquire) == 2;
    step = open_execution_snapshot(e);
    if (step.done) return step.status;
    step = prepare_automatic_step(e);
    if (step.done) return step.status;
    step = prepare_execution_property(e);
    if (step.done) return step.status;
    step = dispatch_execution_action(e);
    if (step.done) return step.status;
    return execute_prepared(e);
}
static int execute(const struct azexit_cue_request *request, void *manager, struct azexit_result *result, void *unused) {
    (void) unused;
    State *s = &state();
    unsigned deck = request->identity.deck;
    Pending *pending = &s->pending[deck];
    azcs_snapshot snapshot __attribute__((cleanup(azcs_snapshot_reset))) = {0};
    struct azexit_cue_request automatic_step = {0};
    azcs_property property __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_owned_cue selected_stored __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    struct az_cue_entry expected = {0};
    azcs_property canonical __attribute__((cleanup(azcs_property_reset))) = {0};
    struct azcuepersist_witness witness = {0};
    unsigned kind = (hot(request->operation) || (creating(request->operation) && request->classification == AZ_CUE_HOT)) ? request->entry_id : 0;
    CueExecution e = {.state = s, .pending = pending, .deck = deck, .kind = kind, .request = request, .native_request = request, .manager = manager, .result = result, .snapshot = &snapshot, .property = &property, .canonical = &canonical, .selected_stored = &selected_stored, .expected = &expected, .automatic_step = &automatic_step, .witness = &witness};
    if (atomic_load_explicit(&pending->token, memory_order_acquire) == request->identity.token && atomic_load_explicit(&pending->cookie, memory_order_acquire)) {
        CommandStep step = resume_persistence(&e);
        if (step.done) return step.status;
    }
    return execute_frame(&e);
}

// Unused coordinate fields are canonical argument encodings, not observations
// in a source clock. Check their zero representation before source admission.
static bool unused_coordinate(double value) {
    uint64_t encoded = 0;
    memcpy(&encoded, &value, sizeof encoded);
    return !(encoded & UINT64_C(0x7fffffffffffffff));
}
static bool metadata_operation(unsigned operation) {
    return operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR;
}
static bool valid_command_identity(const struct azexit_cue_request *r) {
    return r && r->size >= sizeof *r && r->identity.deck < 4 && r->identity.token &&
           !(r->identity.token & INTERNAL) && r->identity.load_generation && r->identity.deadline_ns && !r->identity.reserved;
}
static bool valid_command_payload(const struct azexit_cue_request *r) {
    return r->operation < 64 && memchr(r->comment, 0, sizeof r->comment) && isfinite(r->position_s) && isfinite(r->end_s) &&
           (creating(r->operation) || unused_coordinate(r->end_s)) && r->beats == 0;
}
static bool memory_palette_color(uint32_t rgb) {
    const uint32_t colors[] = {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu};
    for (unsigned i = 0; i < sizeof colors / sizeof *colors; i++)
        if (colors[i] == rgb) return true;
    return false;
}
static bool valid_creation_kind(const struct azexit_cue_request *r) {
    return r->classification == AZ_CUE_HOT ? r->entry_id >= 1 && r->entry_id <= 8 : r->classification == AZ_CUE_MEMORY && !r->entry_id;
}
static int command_current(const struct azexit_cue_request *r) {
    return current(r->identity.deck, r->identity.load_generation, NULL) ? AZ_OK : AZ_STALE;
}
static int valid_creation(const struct azexit_cue_request *r) {
    if (r->flags != (AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS) ||
        !r->expected_revision || !valid_creation_kind(r) || r->rgb > 0xffffff) return AZ_INVALID;
    if (r->classification == AZ_CUE_MEMORY && !memory_palette_color(r->rgb)) return AZ_UNSUPPORTED;
    return command_current(r);
}
static bool valid_selected_flags(const struct azexit_cue_request *r, bool selected) {
    uint32_t flags = AZ_COMMAND_NATIVE_LOAD | (recall(r->operation) ? 0 : AZ_COMMAND_PERSIST) | (selected ? AZ_COMMAND_NATIVE_CUE_IDS : 0);
    if (r->flags != flags) return false;
    return selected ? r->expected_revision && r->entry_id && r->entry_id <= AZ_NATIVE_CUE_LIMIT : !r->expected_revision;
}
static bool valid_selected_classification(const struct azexit_cue_request *r, bool selected) {
    bool classified = r->classification == AZ_CUE_MEMORY || (metadata_operation(r->operation) && r->classification == AZ_CUE_HOT);
    return classified && (selected || !r->entry_id) && (r->classification != AZ_CUE_HOT || r->entry_id <= 8);
}
static bool valid_existing_classification(const struct azexit_cue_request *r, bool selected) {
    if (hot(r->operation)) return r->classification == AZ_CUE_HOT && r->entry_id >= 1 && r->entry_id <= 8;
    return valid_selected_classification(r, selected);
}
static bool valid_command_position(const struct azexit_cue_request *r) {
    if (r->operation == AZ_CMD_SAVED_AUTO) return r->position_s == 0 || r->position_s == 1;
    return unused_coordinate(r->position_s);
}
static int valid_command_color(const struct azexit_cue_request *r) {
    if (r->operation == AZ_CMD_HOT_SET || r->operation == AZ_CMD_CUE_COLOR) {
        if (r->rgb > 0xffffff) return AZ_INVALID;
        if (r->operation == AZ_CMD_CUE_COLOR && r->classification == AZ_CUE_MEMORY && !memory_palette_color(r->rgb)) return AZ_UNSUPPORTED;
    } else if (r->rgb) return AZ_INVALID;
    return AZ_OK;
}
static int valid_existing_command(const struct azexit_cue_request *r) {
    bool fenced_clear = r->operation == AZ_CMD_HOT_CLEAR && (r->flags & AZ_COMMAND_NATIVE_CUE_IDS);
    bool selected = selector(r->operation) || fenced_clear;
    if (!valid_selected_flags(r, selected) || !valid_existing_classification(r, selected)) return AZ_INVALID;
    if (!valid_command_position(r)) return AZ_INVALID;
    if (r->operation != AZ_CMD_CUE_COMMENT && r->comment[0]) return AZ_INVALID;
    int rc = valid_command_color(r);
    return rc == AZ_OK ? command_current(r) : rc;
}
static int valid(const struct azexit_cue_request *r) {
    if (!r) return AZ_INVALID;
    if (!valid_command_identity(r) || !valid_command_payload(r)) return AZ_INVALID;
    if (!(azsdk_cue_commands_ready(r->identity.deck) & bit(r->operation))) return AZ_UNSUPPORTED;
    return creating(r->operation) ? valid_creation(r) : valid_existing_command(r);
}

static int configure_backing_services(State *s, const struct azcue_profile *profile, int build, azexit_install_fn install) {
    struct azcuepersist_callbacks persistence = {validate_persist, confirm_persist, ready, NULL};
    if (!azsdk_cue_persist_configure(&profile->persist, install, &persistence)) return AZ_UNSUPPORTED;
    int rc = azsdk_cue_recall_configure(build, s->bias);
    if (rc != AZ_OK) return rc;
    if (!azsdk_cue_lifecycle_configure(build, install)) return AZ_UNSUPPORTED;
    return AZ_OK;
}
int azsdk_cue_commands_configure(int on, int build, azexit_install_fn install) {
    State *s = &(state());
    if (!on) {
        atomic_store_explicit(&(s->enabled), false, memory_order_release);
        return AZ_OK;
    }
    if (atomic_load_explicit(&(s->enabled), memory_order_acquire)) return AZ_BUSY;
    struct azcue_profile profile = {0};
    if (!install || !azsdk_cue_main_profile(build, &profile)) return AZ_UNSUPPORTED;
    s->build = build;
    s->bias = profile.actor.memory_producer - (build == 1 ? 0x81db60 : 0x863460);
    s->store = azcs_native_profile(build, s->bias);
    struct azcue_actor_callbacks callbacks = {execute, current, NULL};
    int rc = azsdk_cue_actor_configure(&profile.actor, install, &callbacks);
    if (rc != AZ_OK) return rc;
    rc = configure_backing_services(s, &profile, build, install);
    if (rc != AZ_OK) return rc;
    atomic_store_explicit(&(s->capabilities), azsdk_cue_persist_capabilities() | bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL), memory_order_release);
    atomic_store_explicit(&(s->enabled), true, memory_order_release);
    return AZ_OK;
}
uint64_t azsdk_cue_commands_ready(unsigned deck) {
    if (deck >= 4 || !atomic_load_explicit(&(state().enabled), memory_order_acquire)) return 0;
    uint64_t mask = atomic_load_explicit(&(state().capabilities), memory_order_acquire);
#ifndef AZSDK_CUE_COMMANDS_TESTING
    unsigned bit_deck = 1u << deck;
    if (!(azsdk_cue_actor_mask() & bit_deck)) return 0;
    if (!(azsdk_cue_capabilities() & bit_deck))
        mask &= bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL);
#endif
    return mask;
}
uint64_t azsdk_cue_commands_commands(void) {
    uint64_t mask = 0;
    for (unsigned deck = 0; deck < 4; ++deck) mask |= azsdk_cue_commands_ready(deck);
    return mask;
}
static int copy_cached_cues(unsigned deck, Cache *cache, struct az_native_cue_snapshot *out) {
    int rc = AZ_STALE;
    if (cache->value.revision && current(deck, cache->value.native_load_generation, NULL)) {
        const uint32_t size = out->size;
        struct az_native_cue_snapshot value = cache->value;
        value.size = size;
        memcpy(out, &value, size < sizeof value ? size : sizeof value);
        rc = AZ_OK;
    }
    return rc;
}
int azsdk_cue_commands_native_cues(unsigned deck, struct az_native_cue_snapshot *out) {
    if (deck >= 4 || !out || out->size < AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    AZ_MUTEX_GUARD(lock, cache->mutex, true);
    if (!lock.held) {
        atomic_store_explicit(&(cache->waiter), true, memory_order_release);
        if (!(lock = az_mutex_acquire(lock.mutex, true)).held) return AZ_BUSY;
    }
    int rc = copy_cached_cues(deck, cache, out);
    az_mutex_release(&lock);
    if (atomic_exchange_explicit(&(cache->waiter), false, memory_order_acq_rel)) azsdk_exit_delivery_notify(deck);
    return rc;
}
int azsdk_cue_commands_target_cue(unsigned deck, struct azexit_cue_target *out) {
    if (deck >= 4 || !out) return AZ_INVALID;
    struct azexit_cue_target target = {0};
    target.persistent_commands = azsdk_cue_commands_commands() & ~(bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL));
    struct az_native_cue_snapshot snapshot = {0};
    snapshot.size = sizeof snapshot;
    int rc = azsdk_cue_commands_native_cues(deck, &snapshot);
    if (rc == AZ_BUSY) return AZ_BUSY;
    if (rc == AZ_OK) {
        target.revision = snapshot.revision;
        target.revision_valid = 1;
        for (unsigned operation = 1; operation < 64; ++operation)
            if (selector(operation) || creating(operation)) target.revision_commands |= azsdk_cue_commands_commands() & bit(operation);
    }
    struct azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (deck), &load)) return AZ_STALE;
    *out = target;
    return AZ_OK;
}
static int queue_admitted(Cache *cache, const struct azexit_cue_request *request, int rc) {
    COMMAND_DIAG(*request, rc == AZ_BUSY ? 8 : 9, rc == AZ_BUSY ? "public-queued" : "actor-enqueued");
    atomic_store_explicit(&(cache->waiting), rc == AZ_PENDING ? 5 : rc == AZ_BUSY ? 2
                                                                                  : 0,
                          memory_order_release);
    if (rc == AZ_PENDING || rc == AZ_BUSY) azsdk_cue_commands_notify(request->identity.deck);
    return rc == AZ_BUSY ? AZ_PENDING : rc;
}
int azsdk_cue_commands_submit(const struct azexit_cue_request *request) {
    if (!request) return AZ_INVALID;
    if (request->size >= sizeof *request) COMMAND_DIAG(*request, 10, "public-entry");
    int rc = valid(request);
    if (rc != AZ_OK) return rc;
    COMMAND_DIAG(*request, 7, "public-submit");
    Cache *cache = &(state().decks[request->identity.deck]);
    unsigned empty = 0;
    if (!atomic_compare_exchange_strong_explicit(&(cache->waiting), &(empty), 1, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    cache->waiting_request = *request;
    // All decks share the actor's owner lock. Retire queued internal reads on
    // every deck; their native tasks keep tombstones until destruction. A running
    // owner frame stays protected, and contention uses the copied queue below.
    retire_internal_reads();
    rc = azsdk_cue_actor_enqueue(&cache->waiting_request);
    return queue_admitted(cache, request, rc);
}
static int cancel_queued(Cache *cache, unsigned deck, uint64_t token) {
    if (cache->waiting_request.identity.token != token) {
        atomic_store_explicit(&(cache->waiting), 2, memory_order_release);
        return AZ_STALE;
    }
    cache->waiting_result = (__typeof__(cache->waiting_result)) {token, cache->waiting_request.identity.load_generation, 0, AZ_COMMAND_CANCELLED, AZ_CANCELLED};
    atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
    publish_waiting_result(deck, &(cache->waiting_result));
    return AZ_OK;
}
static bool persistence_active(Pending *pending, uint64_t token) {
    return (atomic_load_explicit(&pending->cookie, memory_order_acquire) && atomic_load_explicit(&pending->token, memory_order_acquire) == token) ||
           (atomic_load_explicit(&pending->stage_token, memory_order_acquire) == token && atomic_load_explicit(&pending->stage, memory_order_acquire) == 2);
}
static void resume_stopping(Pending *pending, unsigned deck, uint64_t token) {
    if ((atomic_load_explicit(&pending->persist_ready, memory_order_acquire) == token || !atomic_load_explicit(&pending->cookie, memory_order_acquire)) &&
        atomic_exchange_explicit(&pending->stop_resumed, token, memory_order_acq_rel) != token) {
        atomic_store_explicit(&pending->resume, token, memory_order_release);
        azsdk_cue_commands_notify(deck);
    }
}
static int cancel_active(Pending *pending, unsigned deck, uint64_t token) {
    if (atomic_load_explicit(&(pending->bridge), memory_order_acquire) == token) {
        int rc = azsdk_exit_recall_cancel(deck, token);
        if (rc != AZ_OK) return rc;
        if (creating(pending->active_request.operation)) {
            // Its storage receipt already proved an effect. Keep the actor
            // until the cancelled Hot handoff is collected as UNCERTAIN.
            azsdk_cue_commands_notify(deck);
            return AZ_PENDING;
        }
    }
    if (persistence_active(pending, token)) {
        resume_stopping(pending, deck, token);
        return AZ_PENDING;
    }
    int rc = azsdk_cue_actor_cancel(deck, token);
    if (rc == AZ_OK && atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == token) atomic_store_explicit(&(pending->stage), 0, memory_order_release);
    return rc;
}
int azsdk_cue_commands_cancel(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache *cache = &state().decks[deck];
    unsigned queued = 2;
    if (atomic_compare_exchange_strong_explicit(&cache->waiting, &queued, 1, memory_order_acq_rel, memory_order_acquire)) return cancel_queued(cache, deck, token);
    if (queued && queued != 5) return AZ_PENDING;
    return cancel_active(&state().pending[deck], deck, token);
}
int azsdk_cue_commands_poll(unsigned deck, struct azexit_result *result) {
    if (deck >= 4 || !result) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    unsigned ready = 3;
    if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(ready), 1, memory_order_acq_rel, memory_order_acquire)) {
        *result = cache->waiting_result;
        atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
        return AZ_OK;
    }
    if ((ready && ready != 5) || atomic_load_explicit(&(cache->refresh), memory_order_acquire)) return AZ_PENDING;
    return azsdk_cue_actor_take(deck, result);
}
static int release_queued(Cache *cache, uint64_t token) {
    if (cache->waiting_result.token != token) {
        atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
        return AZ_STALE;
    }
    atomic_store_explicit(&(cache->waiting), 0, memory_order_release);
    return AZ_OK;
}
static void release_active(Cache *cache, unsigned deck, uint64_t token, unsigned ready) {
    if (ready == 5) atomic_store_explicit(&(cache->waiting), 0, memory_order_release);
    if (atomic_load_explicit(&(state().pending[deck].stage_token), memory_order_acquire) == token)
        atomic_store_explicit(&(state().pending[deck].stage), 0, memory_order_release);
}
int azsdk_cue_commands_release(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    unsigned ready = 3;
    if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(ready), 1, memory_order_acq_rel, memory_order_acquire)) {
        return release_queued(cache, token);
    }
    if (ready && ready != 5) return AZ_BUSY;
    int rc = azsdk_cue_actor_release(deck, token);
    if (rc == AZ_OK) release_active(cache, deck, token, ready);
    return rc;
}
static void qualify_bridge_result(Pending *pending, unsigned deck, uint64_t bridge, struct azexit_result *result) {
    uint64_t generation = atomic_load_explicit(&(pending->bridge_generation), memory_order_acquire);
    if (result->load_generation != generation || !current(deck, generation, NULL)) {
        result->load_generation = generation;
        result->revision = 0;
        result->outcome = AZ_COMMAND_UNCERTAIN;
        result->reason = AZ_STALE;
    } else if (creating(pending->active_request.operation) && result->outcome != AZ_COMMAND_APPLIED) {
        result->outcome = AZ_COMMAND_UNCERTAIN;
    } else if (result->outcome == AZ_COMMAND_APPLIED) {
        uint64_t revision = atomic_load_explicit(&(pending->bridge_revision), memory_order_acquire);
        if (revision) result->revision = revision;
        if (atomic_load_explicit(&(pending->stage), memory_order_acquire) == 1 &&
            azsdk_exit_cue_stage_capture(deck, bridge, &pending->staged_cue) != AZ_OK) {
            result->revision = 0;
            result->outcome = AZ_COMMAND_UNCERTAIN;
            result->reason = AZ_FAILED;
        }
    }
}
static bool collect_bridge(Pending *pending, unsigned deck) {
    uint64_t bridge = atomic_load_explicit(&(pending->bridge), memory_order_acquire);
    if (bridge
#ifndef AZSDK_CUE_COMMANDS_TESTING
        && atomic_load_explicit(&(pending->bridge_ready), memory_order_acquire) == bridge
#endif
    ) {
        struct azexit_result result = {0};
        if (azsdk_exit_recall_take(deck, &result) == AZ_OK && result.token == bridge) {
            qualify_bridge_result(pending, deck, bridge, &result);
            if (azsdk_exit_recall_release(deck, bridge) != AZ_OK) return false;
            azsdk_cue_recall_release(deck, bridge);
            atomic_store_explicit(&(pending->bridge_ready), 0, memory_order_release);
            atomic_store_explicit(&(pending->bridge), 0, memory_order_release);
            if (atomic_load_explicit(&(pending->stage), memory_order_acquire) == 1 && result.outcome == AZ_COMMAND_APPLIED) {
                atomic_store_explicit(&(pending->stage), 2, memory_order_release);
                atomic_store_explicit(&(pending->resume), bridge, memory_order_release);
                azsdk_cue_commands_notify(deck);
            } else {
                atomic_store_explicit(&(pending->stage), 0, memory_order_release);
                pending->actor_completion = result;
            }
        }
    }
    return true;
}
static void collect_actor(Pending *pending, unsigned deck) {
    if (pending->actor_completion.token &&
        azsdk_cue_actor_complete(deck, pending->actor_completion.token, &pending->actor_completion) != AZ_BUSY)
        pending->actor_completion = (__typeof__(pending->actor_completion)) {};
    uint64_t retired = atomic_load_explicit(&(pending->actor_retired), memory_order_acquire);
    if (retired) {
        if (atomic_load_explicit(&(pending->persist_retired), memory_order_acquire) == retired) {
            atomic_store_explicit(&(pending->persist_ready), 0, memory_order_release);
            atomic_store_explicit(&(pending->cookie), 0, memory_order_release);
            atomic_store_explicit(&(pending->stage), 0, memory_order_release);
        }
        if (!atomic_load_explicit(&(pending->cookie), memory_order_acquire) &&
            !atomic_load_explicit(&(pending->bridge), memory_order_acquire) &&
            atomic_load_explicit(&(pending->stage), memory_order_acquire) != 2) {
            struct azexit_receipt value = {
                sizeof value, AZEXIT_DELIVERY_VERSION, AZEXIT_RECEIPT_SETTLED, 1, deck,
                retired, atomic_load_explicit(&(pending->retired_load), memory_order_acquire), 0, 0, now(), 0, 0};
            atomic_store_explicit(&(pending->actor_retired), 0, memory_order_release);
            (void) azsdk_exit_delivery_publish(&value);
        }
    }
}
static void resume_notified(Pending *pending, unsigned deck, unsigned collections) {
    uint64_t notice = atomic_exchange_explicit(&(pending->resume), 0, memory_order_acq_rel);
    if (!notice && (collections & (1u << deck))) {
        notice = atomic_load_explicit(&(pending->persist_ready), memory_order_acquire);
        if (!notice && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2)
            notice = atomic_load_explicit(&(pending->stage_token), memory_order_acquire);
    }
    if (notice && azsdk_cue_actor_resume(deck, notice) == AZ_BUSY) {
        uint64_t empty = 0;
        atomic_compare_exchange_strong_explicit(&(pending->resume), &(empty), notice, memory_order_acq_rel, memory_order_acquire);
    }
}
static void collect_refresh(Cache *cache, unsigned deck) {
    uint64_t token = atomic_load_explicit(&(cache->refresh), memory_order_acquire);
    if (token
#ifndef AZSDK_CUE_COMMANDS_TESTING
        && atomic_load_explicit(&(cache->actor_finished), memory_order_acquire) == token
#endif
    ) {
        struct azexit_result result = {0};
        if (azsdk_cue_actor_take(deck, &result) == AZ_OK && result.token == token) {
            if (azsdk_cue_actor_release(deck, token) == AZ_OK) atomic_store_explicit(&(cache->refresh), 0, memory_order_release);
        }
    }
}
static bool post_waiting_request(Cache *cache, unsigned deck, uint64_t clock) {
    const struct azexit_cue_request *request = &(cache->waiting_request);
    int rc = compare_timestamps(clock, request->identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL ? AZ_STALE : valid(request);
    if (rc == AZ_OK) rc = azsdk_cue_actor_enqueue(request);
    if (rc == AZ_PENDING) {
        COMMAND_DIAG((*request), 9, "actor-enqueued");
        atomic_store_explicit(&(cache->waiting), 5, memory_order_release);
        return true;
    } else if (rc == AZ_BUSY) atomic_store_explicit(&(cache->waiting), 2, memory_order_release);
    else {
        cache->waiting_result = (__typeof__(cache->waiting_result)) {request->identity.token, request->identity.load_generation, 0, AZ_COMMAND_REJECTED, rc};
        atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
        publish_waiting_result(deck, &(cache->waiting_result));
    }
    return false;
}
static bool post_public(Cache *cache, unsigned deck, uint64_t clock) {
    bool posted_public = false;
    if (!atomic_load_explicit(&(cache->refresh), memory_order_acquire)) {
        unsigned queued = 2;
        if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(queued), 4, memory_order_acq_rel, memory_order_acquire)) {
            posted_public = post_waiting_request(cache, deck, clock);
        }
    }
    return posted_public;
}
static bool schedule_refresh(State *s, Cache *cache, unsigned deck, uint64_t clock) {
    struct azsdk_load_sample load = {0};
    bool refresh = false;
    if (azsdk_load_read((int) (deck), &load)) {
        {
            AZ_MUTEX_LOCK(lock, cache->mutex);
            refresh = cache->value.native_load_generation != load.load_generation || compare_timestamps(clock, cache->observed, 1000000000, 10, 1) != AZ_TIME_EQUAL;
        }
        if (refresh) {
            uint64_t serial = atomic_fetch_add_explicit(&(s->serial), 1, memory_order_relaxed) + 1;
            if (serial >= INTERNAL) {
                atomic_store_explicit(&(s->enabled), false, memory_order_release);
                return false;
            }
            struct azexit_cue_request request = {0};
            request.size = sizeof request;
            request.identity.deck = deck;
            request.identity.token = INTERNAL | serial;
            request.identity.load_generation = load.load_generation;
            request.identity.deadline_ns = clock + UINT64_C(1000000000);
            if (azsdk_cue_actor_enqueue(&request) == AZ_PENDING) {
                atomic_store_explicit(&(cache->refresh), request.identity.token, memory_order_release);
                        // Cover a public admission that raced this publication.
                if (unfinished_public_work()) retire_internal_reads();
            }
        }
    }
    return true;
}
static bool refresh_idle(State *s, Cache *cache, unsigned deck, uint64_t clock, bool posted_public) {
    unsigned waiting = atomic_load_explicit(&(cache->waiting), memory_order_acquire);
    if (!posted_public && (!waiting || waiting == 3) && !atomic_load_explicit(&(cache->refresh), memory_order_acquire) && !unfinished_public_work()) {
        if (!schedule_refresh(s, cache, deck, clock)) return false;
    }
    return true;
}
void azsdk_cue_commands_tick(void) {
    State *s = &state();
    if (!atomic_load_explicit(&s->enabled, memory_order_acquire)) return;
    uint64_t clock = now();
    if (!clock) return;
    if (unfinished_public_work()) retire_internal_reads();
    unsigned collections = atomic_exchange_explicit(&s->collections, 0, memory_order_acq_rel);
    for (unsigned deck = 0; deck < 4; ++deck) {
        Pending *pending = &s->pending[deck];
        if (!collect_bridge(pending, deck)) continue;
        collect_actor(pending, deck);
        resume_notified(pending, deck, collections);
        Cache *cache = &s->decks[deck];
        collect_refresh(cache, deck);
        bool posted_public = post_public(cache, deck, clock);
        if (!refresh_idle(s, cache, deck, clock, posted_public)) return;
        azsdk_cue_actor_drive(deck);
    }
}

void azsdk_cue_commands_notify(unsigned deck) {
    if (deck >= 4) return;
    atomic_fetch_or_explicit(&(state().notifications), 1u << deck, memory_order_release);
    azsdk_exit_delivery_notify(deck);
}
void azsdk_cue_commands_actor_finished(unsigned deck, uint64_t token) {
    if (deck >= 4) return;
    atomic_store_explicit(&(state().decks[deck].actor_finished), token, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void azsdk_cue_commands_actor_retired(unsigned deck, uint64_t token, uint64_t load) {
    if (deck >= 4) return;
    Pending *pending = &(state().pending[deck]);
    azcs_property_reset(&(pending->retained)); // manager's retirement still holds the actor lease
    atomic_store_explicit(&(pending->retired_load), load, memory_order_relaxed);
    atomic_store_explicit(&(pending->actor_retired), token, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void azsdk_cue_commands_persist_retired(unsigned deck, uint64_t token) {
    if (deck >= 4) return;
    atomic_store_explicit(&(state().pending[deck].persist_retired), token, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void azsdk_cue_commands_recall_finished(unsigned deck, uint64_t token) {
    if (deck >= 4) return;
    atomic_store_explicit(&(state().pending[deck].bridge_ready), token, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void azsdk_cue_commands_collection_changed(unsigned deck) {
    if (deck >= 4) return;
    atomic_fetch_or_explicit(&(state().collections), 1u << deck, memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void azsdk_cue_commands_drive(void) {
    if (!atomic_exchange_explicit(&(state().notifications), 0, memory_order_acq_rel)) return;
    azsdk_cue_commands_tick();
}

#ifdef AZSDK_CUE_COMMANDS_TESTING
void azsdk_cue_commands_test_time(uint64_t value) { atomic_store_explicit(&(test_clock), value, memory_order_relaxed); }
void azsdk_cue_commands_test_cache_lock(unsigned deck, void (*held)(void *), void *context) {
    AZ_MUTEX_LOCK(lock, state().decks[deck].mutex);
    held(context);
}
#endif

static __attribute__((destructor)) void command_state_drop(void) {
    for (unsigned i = 4; i > 0; --i) azcs_property_reset(&command_state.pending[i - 1].retained);
    for (unsigned i = 4; i > 0; --i) pthread_mutex_destroy(&command_state.decks[i - 1].mutex);
}

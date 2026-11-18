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
int azsdk_exit_delivery_publish(const struct azexit_receipt *r) {
    (void) r;
    return AZ_UNSUPPORTED;
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
static bool unfinished_public_work() {
    State *s = &(state());
    for (unsigned deck = 0; deck < 4; ++deck) {
        unsigned waiting = atomic_load_explicit(&(s->decks[deck].waiting), memory_order_acquire);
        if (!waiting || waiting == 3) continue;
        if (waiting != 5) return true;
#ifdef AZSDK_CUE_COMMANDS_TESTING
        struct azexit_result result = {0};
        if (azsdk_cue_actor_peek(deck, &result) != AZ_OK || !result.token ||
            (result.token & INTERNAL) || result.outcome == AZ_COMMAND_ACCEPTED) return true;
#else
        if (atomic_load_explicit(&(s->decks[deck].actor_finished), memory_order_acquire) !=
            s->decks[deck].waiting_request.identity.token) return true;
#endif
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
static int scalar(azcs_snapshot *snapshot, size_t ordinal, struct az_cue_entry *out, azcs_property *selected_property) {
    azcs_owned_cue cue __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    azcs_property temporary __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_property *property = selected_property ? selected_property : &temporary;
    if (!azcs_snapshot_row(snapshot, ordinal, &(cue))) return -1;
    unsigned kind = cue.native.bytes[0];
    const bool hot_collection = ordinal >= azcs_snapshot_memory_count(snapshot);
    /* Exact native validStoredCue guard. Only its fixed HOT collection may
     * contain absent slots; populated conversion/source failures are errors. */
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    if (hot_collection && (kind == 0xff || read(uint64_t, &cue.native, 8) == absent || read(uint64_t, &cue.native, 0x10) == absent)) return 0;
    if ((hot_collection ? kind < 1 || kind > 8 : kind != 0) ||
        !azcs_snapshot_property(snapshot, (uint32_t) (ordinal + 1), azcs_snapshot_revision(snapshot), &((*property)))) return -1;
    (*out) = (__typeof__((*out))) {};
    out->size = sizeof(*out);
    out->version = 1;
    out->saved = 1;
    out->valid = AZ_CUE_SAVED;
    out->classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
    out->slot = kind;
    out->entry_id = kind ? kind : (uint32_t) (ordinal + 1);
    out->rgb = read(uint32_t, &cue.native, 0x78) & 0xffffff;
    out->valid |= AZ_CUE_COLOR;
    out->auto_active = !!cue.native.bytes[0x28];
    out->valid |= AZ_CUE_AUTO_ACTIVE;
    uintptr_t text = read(uintptr_t, &cue.native, 0x70);
    if (text) {
        azcue_copy_utf8_prefix(out->comment, ((const char *) (text)));
        out->valid |= AZ_CUE_COMMENT;
    }
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
static int confirm_persist(const struct azexit_cue_request *request, void *manager, uint64_t cookie, uint64_t *revision, void *unused) {
    (void) unused;
    if (!validate_persist(request, manager, cookie, NULL)) return 0;
    State *s = &(state());
    unsigned deck = request->identity.deck;
    azcs_snapshot snapshot __attribute__((cleanup(azcs_snapshot_reset))) = {0};
    struct az_native_cue_snapshot copied;
    if (!azcs_snapshot_open(&(snapshot), &(s->store), manager, request->identity.load_generation, NULL, &(s->decks[deck].revision))) return 0;
    COMMAND_DIAG(*request, 15, "confirm-open");
    Pending *pending = &(s->pending[deck]);
    if (request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) {
        struct azcuepersist_witness fresh = {0};
        const unsigned kind = request->classification == AZ_CUE_HOT ? request->entry_id : 0;
        if (!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &fresh, false) ||
            !azsdk_cue_persist_confirm_publication(request, &fresh) ||
            !azcs_snapshot_metadata_only_changed(&(snapshot), &(pending->before_witness), kind, request->entry_id,
                                                 request->operation == AZ_CMD_CUE_COMMENT ? request->comment : NULL, request->rgb, pending->expected_rate) ||
            !validate_persist(request, manager, cookie, NULL)) return 0;
        // The exact receipt and full typed native collection prove the edit.
        // Public position conversion is an independent cache refresh, outside
        // this already-dispatched write's original completion budget.
        COMMAND_DIAG(*request, 17, "confirm-metadata");
        *revision = azcs_snapshot_revision(&(snapshot));
        return 1;
    }
    if (!snapshot_copy(&(snapshot), deck, request->identity.load_generation, &(copied)) || !validate_persist(request, manager, cookie, NULL)) return 0;
    COMMAND_DIAG(*request, 16, "confirm-copied");
    if (request->operation == AZ_CMD_SAVED_AUTO) {
        struct azcuepersist_witness fresh = {0};
        if (!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &fresh, false) ||
            !azsdk_cue_persist_confirm_publication(request, &fresh) ||
            !azcs_snapshot_auto_only_changed(&(snapshot), &(pending->before_witness), request->entry_id, request->position_s != 0, pending->expected_rate)) return 0;
        if (pending->automatic.active && pending->automatic.token == request->identity.token && pending->automatic.final) {
            unsigned active = 0;
            for (unsigned i = 0; i < copied.count; ++i) {
                const struct az_cue_entry *row = &(copied.entries[i]);
                if (row->classification == AZ_CUE_MEMORY && row->type == AZ_CUE_LOOP && row->auto_active) {
                    if (row->entry_id != pending->automatic.target.entry_id) return 0;
                    ++active;
                }
            }
            if (active != 1) return 0;
        }
    }
    if (creating(request->operation)) {
        struct azcuepersist_witness fresh = {0};
        const unsigned kind = request->classification == AZ_CUE_HOT ? request->entry_id : 0;
        if (!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &fresh, true) ||
            !azsdk_cue_persist_confirm_publication(request, &fresh) ||
            !azcs_snapshot_creation_only_changed(&(snapshot), &(pending->before_witness), kind, &(pending->retained), pending->expected_rate, request->comment, request->rgb)) return 0;
        publish(deck, &(copied));
        *revision = copied.revision;
        return 1;
    }
    const struct az_cue_entry *expected = &(pending->expected);
    bool exists = false, effect = false;
    for (unsigned i = 0; i < copied.count; ++i) {
        const struct az_cue_entry *row = &(copied.entries[i]);
        bool target = expected->classification == AZ_CUE_HOT ? row->classification == AZ_CUE_HOT && row->slot == expected->slot : same_point(row, &((*expected)), pending->expected_rate);
        if (!target) continue;
        exists = true;
        if (request->operation == AZ_CMD_CUE_COMMENT) effect = !strcmp(row->comment, request->comment);
        else if (request->operation == AZ_CMD_CUE_COLOR) effect = row->rgb == request->rgb;
        else if (request->operation == AZ_CMD_SAVED_AUTO) effect = row->auto_active == (int) (request->position_s);
        else if (request->operation == AZ_CMD_HOT_SET) effect = same_point(row, &((*expected)), pending->expected_rate) && row->rgb == request->rgb;
        else effect = same_point(row, &((*expected)), pending->expected_rate);
    }
    bool deletion = request->operation == AZ_CMD_SAVED_DELETE || request->operation == AZ_CMD_MEMORY_DELETE || request->operation == AZ_CMD_HOT_CLEAR;
    if (!(deletion ? !exists : effect)) return 0;
    publish(deck, &(copied));
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
static int execute(const struct azexit_cue_request *request, void *manager, struct azexit_result *result, void *unused) {
    (void) unused;
    State *s = &(state());
    const unsigned deck = request->identity.deck;
    Pending *pending = &(s->pending[deck]);

    if (atomic_load_explicit(&(pending->token), memory_order_acquire) == request->identity.token && atomic_load_explicit(&(pending->cookie), memory_order_acquire)) {
        int rc = azsdk_cue_persist_take(&pending->active_request, manager, result);
        if (rc == AZ_PENDING) return rc;
        atomic_store_explicit(&(pending->persist_ready), 0, memory_order_release);
        atomic_store_explicit(&(pending->cookie), 0, memory_order_release);
        atomic_store_explicit(&(pending->stage), 0, memory_order_release);
        if (creating(request->operation) && request->classification == AZ_CUE_HOT && rc == AZ_OK && result->outcome == AZ_COMMAND_APPLIED) {
            // Storage completion is necessary but does not prove the playable
            // Hot bank. Hydrate that slot on the existing DSP owner handoff.
            const uint64_t saved_revision = result->revision;
            int next = azsdk_cue_recall_prepare(request, manager, &pending->retained.native);
            azcs_property_reset(&(pending->retained));
            if (next == AZ_PENDING) {
                pending->active_request = *request; // publish the operation with this exact bridge token
                atomic_store_explicit(&(pending->bridge_generation), request->identity.load_generation, memory_order_relaxed);
                atomic_store_explicit(&(pending->bridge_revision), saved_revision, memory_order_relaxed);
                atomic_store_explicit(&(pending->bridge), request->identity.token, memory_order_release);
                if (atomic_load_explicit(&(pending->bridge_ready), memory_order_acquire) == request->identity.token) azsdk_cue_commands_notify(deck);
                return AZ_PENDING;
            }
            *result = (__typeof__(*result)) {request->identity.token, request->identity.load_generation, 0, AZ_COMMAND_UNCERTAIN, next};
            return AZ_OK;
        }
        azcs_property_reset(&(pending->retained));
        AutoPlan *plan = &(pending->automatic);
        if (!plan->active || plan->token != request->identity.token) return rc;
        if (rc != AZ_OK) return command_fail(pending, request, result, rc);
        if (result->outcome != AZ_COMMAND_APPLIED || !result->revision) {
            if (plan->effect_seen) result->outcome = AZ_COMMAND_UNCERTAIN;
            plan->active = false;
            return AZ_OK;
        }
        if (plan->final) {
            plan->active = false;
            return AZ_OK;
        }
        plan->effect_seen = true;
        plan->revision = result->revision;
        // Continue on this owned frame only after this exact native receipt.
        // The next step must still have the original public deadline/source.
    }
    uint64_t clock = now();
    if (!clock || compare_timestamps(clock, request->identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        COMMAND_DIAG(*request, 1, "execute-deadline");
        return command_fail(pending, request, result, AZ_STALE);
    }
    if (!current(deck, request->identity.load_generation, NULL)) {
        COMMAND_DIAG(*request, 2, "execute-current");
        return command_fail(pending, request, result, AZ_STALE);
    }
    if (request->operation) {
        int allowed = azsdk_exit_delivery_continue(&request->identity, clock);
        if (allowed != AZ_OK) return command_fail(pending, request, result, allowed);
    }
    bool awaiting_hot_view = request->operation == AZ_CMD_HOT_SET &&
                             atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == request->identity.token && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2;

    azcs_snapshot snapshot __attribute__((cleanup(azcs_snapshot_reset))) = {0};
    if (!azcs_snapshot_open(&(snapshot), &(s->store), manager, request->identity.load_generation, NULL, &(s->decks[deck].revision))) {
        COMMAND_DIAG(*request, 3, "snapshot-open");
        if (awaiting_hot_view) return resume_hot_completion(deck, request);
        return command_fail(pending, request, result, AZ_STALE);
    }
    if (!current(deck, request->identity.load_generation, NULL)) {
        COMMAND_DIAG(*request, 4, "snapshot-after-current");
        return command_fail(pending, request, result, AZ_STALE);
    }
    COMMAND_DIAG(*request, 0, "snapshot-admitted");
    if (request->operation == 0) {
        // Snapshot::open revalidates the source and fingerprints both native
        // immutable collections. Their positions need conversion only after
        // that exact load/revision changes, rather than on every 100ms read.
        {
            Cache *cache = &(s->decks[deck]);
            AZ_MUTEX_LOCK(lock, cache->mutex);
            if (cache->value.native_load_generation == request->identity.load_generation &&
                cache->value.revision && cache->value.revision == azcs_snapshot_revision(&(snapshot)) &&
                cache->value.source_rate_hz == azcs_snapshot_source_rate(&(snapshot))) {
                if (!current(deck, request->identity.load_generation, NULL)) return command_fail(pending, request, result, AZ_STALE);
                cache->observed = now();
                *result = (__typeof__(*result)) {request->identity.token, request->identity.load_generation, azcs_snapshot_revision(&(snapshot)), AZ_COMMAND_APPLIED, AZ_OK};
                return AZ_OK;
            }
        }
        struct az_native_cue_snapshot copied;
        if (!snapshot_copy(&(snapshot), deck, request->identity.load_generation, &(copied))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        if (!current(deck, request->identity.load_generation, NULL)) return command_fail(pending, request, result, AZ_STALE);
        publish(deck, &(copied));
        result->token = request->identity.token;
        result->load_generation = request->identity.load_generation;
        result->revision = azcs_snapshot_revision(&(snapshot));
        result->outcome = AZ_COMMAND_APPLIED;
        result->reason = AZ_OK;
        return AZ_OK;
    }
    struct azexit_cue_request automatic_step = {0};
    const struct azexit_cue_request *native_request = request;
    if (request->operation == AZ_CMD_SAVED_AUTO && request->position_s == 1) {
        AutoPlan *plan = &(pending->automatic);
        struct az_native_cue_snapshot cues = {0};
        if (!snapshot_copy(&(snapshot), deck, request->identity.load_generation, &(cues))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        if (!plan->active || plan->token != request->identity.token) {
            if (azcs_snapshot_revision(&(snapshot)) != request->expected_revision) return command_fail(pending, request, result, AZ_STALE);
            (*plan) = (__typeof__((*plan))) {};
            for (unsigned i = 0; i < cues.count; ++i) {
                __auto_type row = &(cues.entries[i]);
                if (row->classification == AZ_CUE_MEMORY && row->entry_id == request->entry_id) {
                    plan->target = (*row);
                    break;
                }
            }
            if (plan->target.type != AZ_CUE_LOOP || !(plan->target.valid & AZ_CUE_END)) return command_fail(pending, request, result, AZ_INVALID);
            azcs_property target_property __attribute__((cleanup(azcs_property_reset))) = {0};
            if (!azcs_snapshot_property(&(snapshot), plan->target.entry_id, azcs_snapshot_revision(&(snapshot)), &(target_property))) return command_fail(pending, request, result, AZ_STALE);
            plan->target_rate = read(uint32_t, &target_property.native, 0x60);
            plan->active = true;
            plan->token = request->identity.token;
            plan->revision = azcs_snapshot_revision(&(snapshot));
        }
        if (azcs_snapshot_revision(&(snapshot)) != plan->revision) return command_fail(pending, request, result, AZ_STALE);
        bool target_present = false;
        unsigned other = 0;
        for (unsigned i = 0; i < cues.count; ++i) {
            __auto_type row = &(cues.entries[i]);
            if (row->classification != AZ_CUE_MEMORY || row->type != AZ_CUE_LOOP) continue;
            if (row->entry_id == plan->target.entry_id) {
                target_present = same_point(row, &(plan->target), plan->target_rate) && row->rgb == plan->target.rgb && !strcmp(row->comment, plan->target.comment);
            } else if (row->auto_active && !other) other = row->entry_id;
        }
        if (!target_present) return command_fail(pending, request, result, AZ_STALE);
        if (plan->steps >= AZ_NATIVE_CUE_LIMIT) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        automatic_step = *request;
        automatic_step.entry_id = other ? other : plan->target.entry_id;
        automatic_step.expected_revision = plan->revision;
        automatic_step.position_s = other ? 0 : 1;
        plan->final = !other;
        native_request = &automatic_step;
    }
    azcs_property property __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_owned_cue selected_stored __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    struct az_cue_entry expected = {0};
    unsigned kind = (hot(request->operation) || (creating(request->operation) && request->classification == AZ_CUE_HOT)) ? request->entry_id : 0;
    if (request->operation == AZ_CMD_HOT_CLEAR && (request->flags & AZ_COMMAND_NATIVE_CUE_IDS) &&
        !(atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == request->identity.token && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2) &&
        azcs_snapshot_revision(&(snapshot)) != request->expected_revision) return command_fail(pending, request, result, AZ_STALE);
    if (creating(request->operation)) {
        if (azcs_snapshot_revision(&(snapshot)) != request->expected_revision) return command_fail(pending, request, result, AZ_STALE);
        const uint32_t rate = azcs_snapshot_source_rate(&(snapshot));
        if (!rate) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        const int start = compare_timestamps(request->position_s, 0., 1, rate, 1);
        const int end = compare_timestamps(request->end_s, request->position_s, 1, rate, 1);
        const int unused = compare_timestamps(request->end_s, 0., 1, rate, 1);
        if (start == AZ_TIME_INVALID || start == AZ_TIME_BEFORE ||
            (request->operation == AZ_CMD_CUE_LOOP_SET && (end == AZ_TIME_INVALID || end != AZ_TIME_AFTER)) ||
            (request->operation == AZ_CMD_CUE_POINT_SET && (unused == AZ_TIME_INVALID || unused != AZ_TIME_EQUAL))) return command_fail(pending, request, result, AZ_INVALID);
        azcs_property live_hot __attribute__((cleanup(azcs_property_reset))) = {0};
        if (kind && azcs_snapshot_current_property(&(snapshot), kind, &(live_hot))) return command_fail(pending, request, result, AZ_LIMIT);
        struct az_native_cue_snapshot stored = {0};
        if (!snapshot_copy(&(snapshot), deck, request->identity.load_generation, &(stored))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        unsigned memory = 0;
        for (unsigned i = 0; i < stored.count; ++i) {
            const struct az_cue_entry *row = &(stored.entries[i]);
            if (kind && row->classification == AZ_CUE_HOT && row->slot == kind) return command_fail(pending, request, result, AZ_LIMIT);
            if (row->classification == AZ_CUE_MEMORY) {
                ++memory;
                int equal = compare_timestamps(row->start_s, request->position_s, 1, rate, 1);
                if (equal == AZ_TIME_INVALID) return command_fail(pending, request, result, AZ_UNSUPPORTED);
                if (!kind && equal == AZ_TIME_EQUAL) return command_fail(pending, request, result, AZ_LIMIT);
            }
        }
        if (!kind && memory >= 10) return command_fail(pending, request, result, AZ_LIMIT);
        if (!azcs_snapshot_timed_property(&(snapshot), kind, request->position_s, request->end_s,
                                          request->operation == AZ_CMD_CUE_LOOP_SET, request->rgb, request->comment, &(property)) ||
            !property_scalar(&(property.native), &(expected))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        expected.classification = request->classification;
        expected.slot = kind;
    } else if (hot(request->operation) && atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == request->identity.token && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2) {
        if (request->operation == AZ_CMD_HOT_CLEAR) {
            if (!azcs_property_copy_to(&(pending->retained), &(property))) return command_fail(pending, request, result, AZ_STALE);
            expected = pending->expected;
        } else {
            if (!azcs_snapshot_current_property(&(snapshot), kind, &(property)) || !property_scalar(&(property.native), &(expected)) ||
                !staged_point(&(property.native), &(expected), &(pending->staged_cue))) return resume_hot_completion(deck, request);
            expected.classification = AZ_CUE_HOT;
            expected.slot = kind;
        }
    } else if (request->operation == AZ_CMD_HOT_SET) {
        azcs_property_reset(&(pending->retained)); // DSP stage captures its playhead, independent of an existing main cue.
    } else if (selector(request->operation) || request->operation == AZ_CMD_HOT_CLEAR) {
        if (selector(request->operation) && native_request->expected_revision != azcs_snapshot_revision(&(snapshot))) return command_fail(pending, request, result, AZ_STALE);
        size_t ordinal = azcs_snapshot_count(&(snapshot));
        // Memory IDs are ordinals in this exact admitted revision. Hot IDs are
        // native slot bytes. Resolve those identities before converting points:
        // conversion can perform source seek work and is needed only once for
        // the selected property, never for every preceding row.
        if (native_request->classification == AZ_CUE_MEMORY) {
            if (native_request->entry_id && native_request->entry_id <= azcs_snapshot_memory_count(&(snapshot)))
                ordinal = native_request->entry_id - 1;
        } else {
            for (size_t i = azcs_snapshot_memory_count(&(snapshot)); i < azcs_snapshot_count(&(snapshot)); ++i) {
                azcs_owned_cue row __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
                if (!azcs_snapshot_row(&(snapshot), i, &(row))) return command_fail(pending, request, result, AZ_STALE);
                if (row.native.bytes[0] == native_request->entry_id) {
                    ordinal = i;
                    break;
                }
            }
        }
        if (ordinal == azcs_snapshot_count(&(snapshot))) return command_fail(pending, request, result, AZ_STALE);
        int status = scalar(&(snapshot), ordinal, &(expected), &property);
        if (status < 0) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        if (!status || expected.classification != native_request->classification || expected.entry_id != native_request->entry_id) return command_fail(pending, request, result, AZ_STALE);
        COMMAND_DIAG(*request, 11, "selected-property");
        if ((request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) &&
            !azcs_snapshot_row(&(snapshot), ordinal, &(selected_stored))) return command_fail(pending, request, result, AZ_STALE);
        kind = expected.slot;
        if ((request->operation == AZ_CMD_SAVED_RECALL || request->operation == AZ_CMD_SAVED_DELETE || request->operation == AZ_CMD_SAVED_AUTO) &&
            !(expected.valid & AZ_CUE_END)) return command_fail(pending, request, result, AZ_INVALID);
    } else {
        if (!azcs_snapshot_current_property(&(snapshot), 0, &(property)) || !property_scalar(&(property.native), &(expected))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        expected.classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
        expected.slot = kind;
    }
    if (hot(request->operation) && !(atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == request->identity.token && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2)) {
        if (request->operation == AZ_CMD_HOT_CLEAR && !azcs_property_copy_to(&(property), &(pending->retained))) return command_fail(pending, request, result, AZ_FAILED);
        pending->expected = expected;
        int rc = azsdk_exit_cue_stage_submit(request);
        if (rc == AZ_PENDING) {
            pending->staged_cue = (__typeof__(pending->staged_cue)) {};
            pending->active_request = *request; // publish the operation with this exact bridge token
            atomic_store_explicit(&(pending->bridge_generation), request->identity.load_generation, memory_order_relaxed);
            atomic_store_explicit(&(pending->bridge_revision), 0, memory_order_relaxed);
            atomic_store_explicit(&(pending->stage_token), request->identity.token, memory_order_release);
            atomic_store_explicit(&(pending->stage), 1, memory_order_release);
            atomic_store_explicit(&(pending->bridge), request->identity.token, memory_order_release);
            if (atomic_load_explicit(&(pending->bridge_ready), memory_order_acquire) == request->identity.token)
                azsdk_cue_commands_notify(deck);
        }
        return rc;
    }
    if (request->operation == AZ_CMD_SAVED_SAVE && !(expected.valid & AZ_CUE_END)) return command_fail(pending, request, result, AZ_INVALID);
    if (recall(request->operation)) {
        int rc = azsdk_cue_recall_prepare(request, manager, &property.native);
        if (rc == AZ_PENDING) {
            /* DSP serials are a separate namespace. The selected, source-qualified
             * copied property belongs to this exact store snapshot revision. */
            pending->active_request = *request; // publish the operation with this exact bridge token
            atomic_store_explicit(&(pending->bridge_generation), request->identity.load_generation, memory_order_relaxed);
            atomic_store_explicit(&(pending->bridge_revision), azcs_snapshot_revision(&(snapshot)), memory_order_relaxed);
            atomic_store_explicit(&(pending->bridge), request->identity.token, memory_order_release);
            if (atomic_load_explicit(&(pending->bridge_ready), memory_order_acquire) == request->identity.token)
                azsdk_cue_commands_notify(deck);
        }
        return rc;
    }
    /* Native persistence serializes microseconds, then reconstructs samples.
     * Its exact owned round trip defines the observable saved position; the
     * original property still goes unchanged to the real native producer. */
    azcs_property canonical __attribute__((cleanup(azcs_property_reset))) = {0};
    if (!azcs_snapshot_canonical_property(&(snapshot), kind, &(property), &(canonical))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
    COMMAND_DIAG(*request, 12, "canonical-property");
    expected.valid &= ~(AZ_CUE_START | AZ_CUE_END);
    expected.start_s = expected.end_s = 0;
    if (!property_scalar(&(canonical.native), &(expected))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
    if (request->operation == AZ_CMD_SAVED_SAVE && !(expected.valid & AZ_CUE_END)) return command_fail(pending, request, result, AZ_UNSUPPORTED);
    if (creating(request->operation)) {
        const uint32_t rate = azcs_snapshot_source_rate(&(snapshot));
        const int start = compare_timestamps(expected.start_s, request->position_s, 1, rate, 1);
        const int end = compare_timestamps(expected.end_s, request->end_s, 1, rate, 1);
        if (start == AZ_TIME_INVALID || start != AZ_TIME_EQUAL ||
            (request->operation == AZ_CMD_CUE_LOOP_SET && (end == AZ_TIME_INVALID || end != AZ_TIME_EQUAL))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
        // Keep the original native label for live Hot hydration: the stock
        // Property-to-Cue serializer intentionally emits an empty comment.
        const int same_in = compare_timestamps(read(uint64_t, &property.native, 0x40), read(uint64_t, &canonical.native, 0x40), rate, rate, 1);
        const int same_out = request->operation == AZ_CMD_CUE_LOOP_SET ? compare_timestamps(read(uint64_t, &property.native, 0x98), read(uint64_t, &canonical.native, 0x98), rate, rate, 1) : AZ_TIME_EQUAL;
        if (same_in == AZ_TIME_INVALID || same_in != AZ_TIME_EQUAL ||
            (request->operation == AZ_CMD_CUE_LOOP_SET && (same_out == AZ_TIME_INVALID || same_out != AZ_TIME_EQUAL))) return command_fail(pending, request, result, AZ_UNSUPPORTED);
    }
    struct azcuepersist_witness witness = {0};
    const struct azcuepersist_witness *selected_witness = NULL;
    if (request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR || request->operation == AZ_CMD_HOT_SET || creating(request->operation)) {
        uint64_t revision = request->operation == AZ_CMD_HOT_SET ? azcs_snapshot_revision(&(snapshot)) : native_request->expected_revision;
        if (!azcs_snapshot_content_witness(&(snapshot), revision, &witness, creating(request->operation))) {
            COMMAND_DIAG(*request, 5, "snapshot-witness");
            // HOT_SET's own asynchronous store can publish between the DSP
            // capture and this witness. No repository request exists yet;
            // refresh the view without repeating the setter or its deadline.
            if (awaiting_hot_view) return resume_hot_completion(deck, request);
            return command_fail(pending, request, result, AZ_STALE);
        }
        if (request->operation != AZ_CMD_HOT_SET && !creating(request->operation)) witness.selected_cue = ((uintptr_t) (&selected_stored.native));
        selected_witness = &witness;
        COMMAND_DIAG(*request, 13, "content-witness");
    }
    uint64_t cookie = atomic_fetch_add_explicit(&(s->serial), 1, memory_order_relaxed) + 1;
    if (!cookie || cookie >= INTERNAL) return command_fail(pending, request, result, AZ_UNSUPPORTED);
    if (creating(request->operation) && !azcs_property_copy_to(&(property), &(pending->retained))) return command_fail(pending, request, result, AZ_FAILED);
    pending->expected = expected;
    pending->active_request = *native_request;
    pending->expected_rate = read(uint32_t, &canonical.native, 0x60);
    pending->before_witness = witness;
    pending->before_revision = azcs_snapshot_revision(&(snapshot));
    atomic_store_explicit(&(pending->token), request->identity.token, memory_order_relaxed);
    atomic_store_explicit(&(pending->manager), (uintptr_t) (manager), memory_order_relaxed);
    atomic_store_explicit(&(pending->cookie), cookie, memory_order_release);
    uintptr_t maintainer = read(uintptr_t, manager, 0x140);
    COMMAND_DIAG(*request, 14, "persist-submit");
    int rc = azsdk_cue_persist_submit(&pending->active_request, manager, ((void *) (maintainer)), kind, &property.native, cookie, selected_witness);
    if (rc != AZ_PENDING) {
        atomic_store_explicit(&(pending->cookie), 0, memory_order_release);
        // Busy or stale preflight produced no repository request. A native
        // automatic store can invalidate the captured content witness between
        // preparation and admission. Recheck the load, captured property and
        // fresh witness on the next owned frame, keeping this DSP stage and
        // the original deadline rather than repeating the setter.
        if (hot(request->operation) && (rc == AZ_BUSY || (request->operation == AZ_CMD_HOT_SET && rc == AZ_STALE))) return resume_hot_completion(deck, request);
        return command_fail(pending, request, result, rc);
    }
    if (pending->automatic.active && pending->automatic.token == request->identity.token) ++pending->automatic.steps;
    return rc;
}

// Unused coordinate fields are canonical argument encodings, not observations
// in a source clock. Check their zero representation before source admission.
static bool unused_coordinate(double value) {
    uint64_t encoded = 0;
    memcpy(&encoded, &value, sizeof encoded);
    return !(encoded & UINT64_C(0x7fffffffffffffff));
}
static int valid(const struct azexit_cue_request *r) {
    if (!r || r->size < sizeof *r || r->identity.deck >= 4 || !r->identity.token ||
        (r->identity.token & INTERNAL) || !r->identity.load_generation || !r->identity.deadline_ns || r->identity.reserved || r->operation >= 64 ||
        !memchr(r->comment, 0, sizeof r->comment) || !isfinite(r->position_s) || !isfinite(r->end_s) || (!creating(r->operation) && !unused_coordinate(r->end_s)) || r->beats != 0) return AZ_INVALID;
    if (!(azsdk_cue_commands_ready(r->identity.deck) & bit(r->operation))) return AZ_UNSUPPORTED;
    if (creating(r->operation)) {
        if (r->flags != (AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS) || !r->expected_revision ||
            (r->classification == AZ_CUE_HOT ? (r->entry_id < 1 || r->entry_id > 8) : (r->classification != AZ_CUE_MEMORY || r->entry_id)) || r->rgb > 0xffffff) return AZ_INVALID;
        if (r->classification == AZ_CUE_MEMORY) {
            bool palette = false;
            {
                unsigned range_values_1[] = {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu};
                for (unsigned range_index_1 = 0; range_index_1 < sizeof(range_values_1) / sizeof(range_values_1)[0]; ++range_index_1) {
                    unsigned rgb = range_values_1[range_index_1];
                    palette = palette || r->rgb == rgb;
                }
            }
            if (!palette) return AZ_UNSUPPORTED;
        }
        return current(r->identity.deck, r->identity.load_generation, NULL) ? AZ_OK : AZ_STALE;
    }
    const bool fenced_clear = r->operation == AZ_CMD_HOT_CLEAR && (r->flags & AZ_COMMAND_NATIVE_CUE_IDS);
    const bool selected = selector(r->operation) || fenced_clear;
    uint32_t flags = AZ_COMMAND_NATIVE_LOAD | (recall(r->operation) ? 0 : AZ_COMMAND_PERSIST) |
                     (selected ? AZ_COMMAND_NATIVE_CUE_IDS : 0);
    if (r->flags != flags || (selected ? (!r->expected_revision || !r->entry_id || r->entry_id > AZ_NATIVE_CUE_LIMIT) : r->expected_revision != 0)) return AZ_INVALID;
    if (hot(r->operation)) {
        if (r->classification != AZ_CUE_HOT || r->entry_id < 1 || r->entry_id > 8) return AZ_INVALID;
    } else if ((r->classification != AZ_CUE_MEMORY &&
                !((r->operation == AZ_CMD_CUE_COMMENT || r->operation == AZ_CMD_CUE_COLOR) && r->classification == AZ_CUE_HOT)) ||
               (!selected && r->entry_id) || (r->classification == AZ_CUE_HOT && r->entry_id > 8)) return AZ_INVALID;
    if (r->operation == AZ_CMD_SAVED_AUTO) {
        if (r->position_s != 0 && r->position_s != 1) return AZ_INVALID;
    } else if (!unused_coordinate(r->position_s)) return AZ_INVALID;
    if (r->operation != AZ_CMD_CUE_COMMENT && r->comment[0]) return AZ_INVALID;
    if (r->operation == AZ_CMD_HOT_SET || r->operation == AZ_CMD_CUE_COLOR) {
        if (r->rgb > 0xffffff) return AZ_INVALID;
        if (r->operation == AZ_CMD_CUE_COLOR && r->classification == AZ_CUE_MEMORY) {
            switch (r->rgb) {
            case 0x000000:
            case 0xff00c6:
            case 0xff0000:
            case 0xff7100:
            case 0xffff00:
            case 0x00e300:
            case 0x00c3ff:
            case 0x0051ff:
            case 0x9c08ff: break;
            default: return AZ_UNSUPPORTED;
            }
        }
    } else if (r->rgb) return AZ_INVALID;
    return current(r->identity.deck, r->identity.load_generation, NULL) ? AZ_OK : AZ_STALE;
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
    struct azcuepersist_callbacks persistence = {validate_persist, confirm_persist, ready, NULL};
    if (!azsdk_cue_persist_configure(&profile.persist, install, &persistence)) return AZ_UNSUPPORTED;
    rc = azsdk_cue_recall_configure(build, s->bias);
    if (rc != AZ_OK) return rc;
    if (!azsdk_cue_lifecycle_configure(build, install)) return AZ_UNSUPPORTED;
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
int azsdk_cue_commands_native_cues(unsigned deck, struct az_native_cue_snapshot *out) {
    if (deck >= 4 || !out || out->size < AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    AZ_MUTEX_GUARD(lock, cache->mutex, true);
    if (!lock.held) {
        atomic_store_explicit(&(cache->waiter), true, memory_order_release);
        if (!(lock = az_mutex_acquire(lock.mutex, true)).held) return AZ_BUSY;
    }
    int rc = AZ_STALE;
    if (cache->value.revision && current(deck, cache->value.native_load_generation, NULL)) {
        const uint32_t size = out->size;
        struct az_native_cue_snapshot value = cache->value;
        value.size = size;
        memcpy(out, &value, size < sizeof value ? size : sizeof value);
        rc = AZ_OK;
    }
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
int azsdk_cue_commands_submit(const struct azexit_cue_request *request) {
    if (request && request->size >= sizeof *request) COMMAND_DIAG(*request, 10, "public-entry");
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
    COMMAND_DIAG(*request, rc == AZ_BUSY ? 8 : 9, rc == AZ_BUSY ? "public-queued" : "actor-enqueued");
    atomic_store_explicit(&(cache->waiting), rc == AZ_PENDING ? 5 : rc == AZ_BUSY ? 2
                                                                                  : 0,
                          memory_order_release);
    if (rc == AZ_PENDING || rc == AZ_BUSY) azsdk_cue_commands_notify(request->identity.deck);
    return rc == AZ_BUSY ? AZ_PENDING : rc;
}
int azsdk_cue_commands_cancel(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    unsigned queued = 2;
    if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(queued), 1, memory_order_acq_rel, memory_order_acquire)) {
        if (cache->waiting_request.identity.token != token) {
            atomic_store_explicit(&(cache->waiting), 2, memory_order_release);
            return AZ_STALE;
        }
        cache->waiting_result = (__typeof__(cache->waiting_result)) {token, cache->waiting_request.identity.load_generation, 0, AZ_COMMAND_CANCELLED, AZ_CANCELLED};
        atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
        publish_waiting_result(deck, &(cache->waiting_result));
        return AZ_OK;
    }
    if (queued && queued != 5) return AZ_PENDING;
    Pending *pending = &(state().pending[deck]);
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
    if ((atomic_load_explicit(&(pending->cookie), memory_order_acquire) && atomic_load_explicit(&(pending->token), memory_order_acquire) == token) ||
        (atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == token && atomic_load_explicit(&(pending->stage), memory_order_acquire) == 2)) {
        if ((atomic_load_explicit(&(pending->persist_ready), memory_order_acquire) == token ||
             !atomic_load_explicit(&(pending->cookie), memory_order_acquire)) &&
            atomic_exchange_explicit(&(pending->stop_resumed), token, memory_order_acq_rel) != token) {
            atomic_store_explicit(&(pending->resume), token, memory_order_release);
            azsdk_cue_commands_notify(deck);
        }
        return AZ_PENDING;
    }
    int rc = azsdk_cue_actor_cancel(deck, token);
    if (rc == AZ_OK && atomic_load_explicit(&(pending->stage_token), memory_order_acquire) == token) atomic_store_explicit(&(pending->stage), 0, memory_order_release);
    return rc;
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
int azsdk_cue_commands_release(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache *cache = &(state().decks[deck]);
    unsigned ready = 3;
    if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(ready), 1, memory_order_acq_rel, memory_order_acquire)) {
        if (cache->waiting_result.token != token) {
            atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
            return AZ_STALE;
        }
        atomic_store_explicit(&(cache->waiting), 0, memory_order_release);
        return AZ_OK;
    }
    if (ready && ready != 5) return AZ_BUSY;
    int rc = azsdk_cue_actor_release(deck, token);
    if (rc == AZ_OK && ready == 5) atomic_store_explicit(&(cache->waiting), 0, memory_order_release);
    if (rc == AZ_OK && deck < 4 && atomic_load_explicit(&(state().pending[deck].stage_token), memory_order_acquire) == token)
        atomic_store_explicit(&(state().pending[deck].stage), 0, memory_order_release);
    return rc;
}
void azsdk_cue_commands_tick(void) {
    State *s = &(state());
    if (!atomic_load_explicit(&(s->enabled), memory_order_acquire)) return;
    uint64_t clock = now();
    if (!clock) return;
    if (unfinished_public_work()) retire_internal_reads();
    unsigned collections = atomic_exchange_explicit(&(s->collections), 0, memory_order_acq_rel);
    for (unsigned deck = 0; deck < 4; ++deck) {
        Pending *pending = &(s->pending[deck]);
        uint64_t bridge = atomic_load_explicit(&(pending->bridge), memory_order_acquire);
        if (bridge
#ifndef AZSDK_CUE_COMMANDS_TESTING
            && atomic_load_explicit(&(pending->bridge_ready), memory_order_acquire) == bridge
#endif
        ) {
            struct azexit_result result = {0};
            if (azsdk_exit_recall_take(deck, &result) == AZ_OK && result.token == bridge) {
                uint64_t generation = atomic_load_explicit(&(pending->bridge_generation), memory_order_acquire);
                if (result.load_generation != generation || !current(deck, generation, NULL)) {
                    result.load_generation = generation;
                    result.revision = 0;
                    result.outcome = AZ_COMMAND_UNCERTAIN;
                    result.reason = AZ_STALE;
                } else if (creating(pending->active_request.operation) && result.outcome != AZ_COMMAND_APPLIED) {
                    result.outcome = AZ_COMMAND_UNCERTAIN;
                } else if (result.outcome == AZ_COMMAND_APPLIED) {
                    uint64_t revision = atomic_load_explicit(&(pending->bridge_revision), memory_order_acquire);
                    if (revision) result.revision = revision;
                    if (atomic_load_explicit(&(pending->stage), memory_order_acquire) == 1 &&
                        azsdk_exit_cue_stage_capture(deck, bridge, &pending->staged_cue) != AZ_OK) {
                        result.revision = 0;
                        result.outcome = AZ_COMMAND_UNCERTAIN;
                        result.reason = AZ_FAILED;
                    }
                }
                if (azsdk_exit_recall_release(deck, bridge) != AZ_OK) continue;
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
        Cache *cache = &(s->decks[deck]);
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
        bool posted_public = false;
        if (!atomic_load_explicit(&(cache->refresh), memory_order_acquire)) {
            unsigned queued = 2;
            if (atomic_compare_exchange_strong_explicit(&(cache->waiting), &(queued), 4, memory_order_acq_rel, memory_order_acquire)) {
                const struct azexit_cue_request *request = &(cache->waiting_request);
                int rc = compare_timestamps(clock, request->identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL ? AZ_STALE : valid(request);
                if (rc == AZ_OK) rc = azsdk_cue_actor_enqueue(request);
                if (rc == AZ_PENDING) {
                    COMMAND_DIAG((*request), 9, "actor-enqueued");
                    atomic_store_explicit(&(cache->waiting), 5, memory_order_release);
                    posted_public = true;
                } else if (rc == AZ_BUSY) atomic_store_explicit(&(cache->waiting), 2, memory_order_release);
                else {
                    cache->waiting_result = (__typeof__(cache->waiting_result)) {request->identity.token, request->identity.load_generation, 0, AZ_COMMAND_REJECTED, rc};
                    atomic_store_explicit(&(cache->waiting), 3, memory_order_release);
                    publish_waiting_result(deck, &(cache->waiting_result));
                }
            }
        }
        unsigned waiting = atomic_load_explicit(&(cache->waiting), memory_order_acquire);
        if (!posted_public && (!waiting || waiting == 3) && !atomic_load_explicit(&(cache->refresh), memory_order_acquire) && !unfinished_public_work()) {
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
                        return;
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
        }
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

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuepersist.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <string.h>
#include "../common/aznative_calls.h"

#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <stdio.h>
#endif

/* Standalone persistence fixtures do not link the command actor. */
void azsdk_cue_commands_persist_retired(unsigned, uint64_t) __attribute__((weak));


enum {
    FREE,
    WRITING,
    ACTIVE,
    RECEIPT,
    POLLING,
    HELPING
};
typedef struct Slot Slot;
struct Slot {
    _Atomic(unsigned) phase, invalid, mutated, retired;
    _Atomic(uintptr_t) handler, receiver;
    struct azexit_cue_request request;
    _Atomic(void *) manager;
    void *maintainer;
    uint64_t cookie;
    unsigned kind;
    unsigned char property_identity[12];
    uint64_t property_in;
    _Atomic(int) native_result;
    _Atomic(unsigned) receipt_seen;
    _Atomic(unsigned) diagnostic, no_effect, request_entered;
    _Atomic(uint64_t) submit_time, request_time, handler_time;
    uintptr_t property_source;
    uint32_t property_rate;
    uintptr_t request_source;
    uint32_t request_rate;
    struct azcuepersist_witness witness;
    uintptr_t own_content;
    uintptr_t reply_memory_set, reply_hot_set, reply_memory_content, reply_hot_content;
    _Atomic(unsigned) reply_captured, metadata_applied;
    uint32_t native_serial;
    unsigned char native_track[12];
    struct azcue_native_timestamps native_times;
    _Atomic(unsigned) helper_claim;
};
typedef struct Receipt Receipt;
struct Receipt {
    _Atomic(unsigned) phase;
    uint64_t sequence, token;
    uintptr_t maintainer;
    uint32_t tag;
    int result;
    unsigned deck;
    unsigned char track[16];
};
typedef struct State State;
struct State {
    struct azcuepersist_profile profile;
    struct azcuepersist_callbacks callbacks;
    Slot slots[4];
    Receipt receipts[64];
    _Atomic(uint64_t) sequence;
    _Atomic(unsigned) enabled, overflow, attempted;
    void *request_original, *handler_original;
    void *reply_original, *registered_original, *memory_original, *hot_original;
    void *memory_notice_original;
};
static State persist_state = {.slots = {[0 ... 3] = {.native_result = -1}}, .receipts = {[0 ... 63] = {.result = -1, .deck = 4}}};
#define state() (persist_state)
_Thread_local Slot *submitting = NULL;
typedef struct Scope Scope;
struct Scope {
    Slot *previous;
};
/* Interrupted native work invalidates its provisional receipt without catching
 * the exception. A normal return keeps the explicit result checks below. */
struct pending_call {
    Slot *slot;
    unsigned diagnostic;
    bool returned;
};
static void pending_call_drop(struct pending_call *call) {
    if (call->returned) return;
    if (call->diagnostic) atomic_store_explicit(&call->slot->diagnostic, call->diagnostic, memory_order_release);
    atomic_store_explicit(&call->slot->invalid, 1, memory_order_release);
}
static void polling_drop(Slot **slot) {
    if (*slot) atomic_store_explicit(&(*slot)->phase, RECEIPT, memory_order_release);
}
static void submitting_scope_init(Scope *ctx, Slot *slot);
static void submitting_scope_drop(Scope *ctx);
static void submitting_scope_init(Scope *ctx, Slot *slot) {
    (void) ctx;
    *ctx = (Scope) {.previous = submitting};
    submitting = slot;
}
static void submitting_scope_drop(Scope *ctx) {
    (void) ctx;
    submitting = ctx->previous;
}

static uintptr_t word(uintptr_t address) {
    uintptr_t out;
    memcpy(&out, ((const void *) (address)), sizeof out);
    return out;
}
static uint32_t u32(uintptr_t address) {
    uint32_t out;
    memcpy(&out, ((const void *) (address)), sizeof out);
    return out;
}
static bool pointer(uintptr_t address) { return address >= 0x10000 && !(address & 7); }
static uint64_t now() {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) (value.tv_sec);
    return seconds <= (UINT64_MAX - (uint64_t) (value.tv_nsec)) / UINT64_C(1000000000)
               ? seconds * UINT64_C(1000000000) + (uint64_t) (value.tv_nsec)
               : 0;
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
static void persist_diagnostic(unsigned deck, uint64_t token, const char *stage, int result) {
    fprintf(stderr, "AZCUE persist deck=%u token=%llu stage=%s clock=%llu result=%d\n", deck,
            (unsigned long long) token, stage, (unsigned long long) now(), result);
}
#define PERSIST_DIAG(...) persist_diagnostic(__VA_ARGS__)
#else
#define PERSIST_DIAG(...) ((void) 0)
#endif
static bool creating(unsigned operation) { return operation == AZ_CMD_CUE_POINT_SET || operation == AZ_CMD_CUE_LOOP_SET; }
static bool supported(unsigned operation) {
    const unsigned operations[] = {AZ_CMD_HOT_SET, AZ_CMD_HOT_CLEAR, AZ_CMD_MEMORY_SET, AZ_CMD_MEMORY_DELETE,
                                   AZ_CMD_SAVED_SAVE, AZ_CMD_SAVED_DELETE, AZ_CMD_SAVED_AUTO, AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR,
                                   AZ_CMD_CUE_POINT_SET, AZ_CMD_CUE_LOOP_SET};
    for (unsigned i = 0; i < sizeof operations / sizeof *operations; ++i)
        if (operation == operations[i]) return true;
    return false;
}
static bool deleting(unsigned operation) { return operation == AZ_CMD_HOT_CLEAR || operation == AZ_CMD_MEMORY_DELETE || operation == AZ_CMD_SAVED_DELETE; }
static unsigned utf8_head(unsigned char c, unsigned *minimum, unsigned *value) {
    if (c >= 0xc2 && c <= 0xdf) {
        *minimum = 0x80;
        *value = c & 0x1f;
        return 1;
    }
    if (c >= 0xe0 && c <= 0xef) {
        *minimum = 0x800;
        *value = c & 0x0f;
        return 2;
    }
    if (c >= 0xf0 && c <= 0xf4) {
        *minimum = 0x10000;
        *value = c & 7;
        return 3;
    }
    return 0;
}
static bool utf8_sequence(const char *text, size_t length, size_t *i) {
    unsigned char c = (unsigned char) text[(*i)++];
    if (c < 0x80) return true;
    unsigned minimum, value;
    unsigned count = utf8_head(c, &minimum, &value);
    if (!count || count > length - *i) return false;
    while (count--) {
        unsigned char tail = (unsigned char) text[(*i)++];
        if ((tail & 0xc0) != 0x80) return false;
        value = (value << 6) | (tail & 0x3f);
    }
    return value >= minimum && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
}
static bool utf8(const char *text, size_t *length) {
    *length = 0;
    while (*length < 128 && text[*length]) ++*length;
    if (*length == 128) return false;
    for (size_t i = 0; i < *length;)
        if (!utf8_sequence(text, *length, &i)) return false;
    return true;
}
struct ValidateCall {
    Slot *slot;
    bool result;
};
static void validate_call(void *opaque) {
    struct ValidateCall *c = opaque;
    Slot *slot = &(*c->slot);
    c->result = state().callbacks.validate(&slot->request, atomic_load_explicit(&(slot->manager), memory_order_acquire), slot->cookie, state().callbacks.context) != 0;
}
static bool valid(Slot *slot) {
    uint64_t time = now();
    if (atomic_load_explicit(&(slot->invalid), memory_order_acquire) || !time || compare_timestamps(time, slot->request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) return false;
    struct ValidateCall call = {slot, false};
    validate_call(&call);
    return call.result;
}
static void *original_address(void *const *p) {
    void *entry = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (!entry) abort();
    return entry;
}
#define original(F, p) ((F) original_address(p))
Slot *for_handler(uintptr_t handler) {
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(state().slots) / sizeof(state().slots)[0]; ++range_index_1) {
            __auto_type slot = &(state().slots[range_index_1]);

            unsigned phase = atomic_load_explicit(&(slot->phase), memory_order_acquire);
            if ((phase == ACTIVE || phase == RECEIPT) && atomic_load_explicit(&(slot->handler), memory_order_acquire) == handler) return slot;
        }
    }
    return NULL;
}
/* Nontrivial return selects the native AArch64 hidden-X8 aggregate ABI. Native
 * allocation creates the actual counted string header; never synthesize it.
 * This local destructor is deliberately empty; the native destructor owns it. */
typedef struct az_native_string NativeString;
struct StringRelease {
    NativeString *text;
    az_native_string_destroy_fn destroy;
};
static void string_release_drop(struct StringRelease *r) { r->destroy(r->text); }
static bool mutate_row(Slot *slot, uintptr_t row) {
    __auto_type profile = &(state().profile);
    if (slot->request.operation == AZ_CMD_CUE_COMMENT || creating(slot->request.operation)) {
        size_t length = 0;
        if (!utf8(slot->request.comment, &(length))) return false;
        NativeString text = az_native_string_allocate((az_native_string_allocate_fn) profile->string_allocate, length + 1);
        if (!text.data) return false;
        struct StringRelease release __attribute__((cleanup(string_release_drop))) = {&text, (az_native_string_destroy_fn) profile->string_destroy};
        (void) release; /* cleanup attribute owns the scope exit */
        memcpy(text.data, slot->request.comment, length + 1);
        ((az_native_string_assign_fn) profile->string_assign)((struct az_native_string *) (row + 0x70), &text);
    }
    if (slot->request.operation == AZ_CMD_CUE_COLOR || slot->request.operation == AZ_CMD_HOT_SET || creating(slot->request.operation)) {
        unsigned rgb = slot->request.rgb;
        ((void (*)(void *, unsigned, unsigned, unsigned))(profile->color_construct))(
            ((void *) (row + 0x78)), (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }
    if (slot->request.operation == AZ_CMD_SAVED_AUTO) {
        unsigned char automatic = slot->request.position_s == 1.0;
        memcpy(((void *) (row + 0x28)), &automatic, 1);
    }
    return true;
}

static bool metadata(unsigned operation) { return operation == AZ_CMD_SAVED_AUTO || operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR || operation == AZ_CMD_HOT_SET || creating(operation); }
static bool vector_bounds(uintptr_t content, uintptr_t *begin, uintptr_t *end, uint64_t *count) {
    (*begin) = (*end) = (*count) = 0;
    if (!content) return true;
    if (!pointer(content)) return false;
    (*begin) = word(content + 0x28);
    (*end) = word(content + 0x30);
    if ((*end) < (*begin) || ((*end) - (*begin)) % 0x80 || ((*end) - (*begin)) / 0x80 > 10 || ((*end) != (*begin) && !pointer((*begin)))) return false;
    (*count) = ((*end) - (*begin)) / 0x80;
    return true;
}
static void mix(uint64_t *hash, const void *p, size_t n) {
    __auto_type bytes = ((const unsigned char *) (p));
    for (size_t i = 0; i < n; ++i) (*hash) = ((*hash) ^ bytes[i]) * UINT64_C(1099511628211);
}
static uint64_t witness_hash(const struct azcuepersist_witness *w) {
    uint64_t count, hot_count;
    uintptr_t begin, end, hot_begin, hot_end;
    if (!vector_bounds(w->memory_content, &(begin), &(end), &(count)) || !vector_bounds(w->hot_content, &(hot_begin), &(hot_end), &(hot_count)) || hot_count > 8) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);

    mix(&(hash), w->source_id, 12);
    mix(&(hash), &count, sizeof count);
    mix(&(hash), ((const void *) (begin)), end - begin);
    mix(&(hash), ((const void *) (hot_begin)), hot_end - hot_begin);
    return hash;
}
static bool publication_matches(uintptr_t root, uintptr_t content) {
    return root ? word(root + 0x28) == content : !content;
}
static bool witness_identity_current(const Slot *s) {
    const struct azcuepersist_witness *w = &s->witness;
    return w->source_info && (creating(s->request.operation) || (w->memory_set && w->hot_set)) && w->revision &&
           (s->request.operation == AZ_CMD_HOT_SET || w->revision == s->request.expected_revision) && w->fingerprint;
}
static bool witness_current(const Slot *s) {
    const struct azcuepersist_witness *w = &s->witness;
    return witness_identity_current(s) && publication_matches(w->memory_set, w->memory_content) &&
           publication_matches(w->hot_set, w->hot_content) && witness_hash(w) == w->fingerprint;
}
static void release_witness(Slot *slot) {
    uintptr_t *reply_refs[] = {&slot->reply_memory_set, &slot->reply_hot_set, &slot->reply_memory_content, &slot->reply_hot_content};
    {
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(reply_refs) / sizeof(reply_refs)[0]; ++range_index_2) {
            __auto_type ref = reply_refs[range_index_2];
            if (*ref) {
                ((void (*)(void *))(state().profile.content_release))(ref);
                *ref = 0;
            }
        }
    }
    if (slot->own_content) {
        ((void (*)(void *))(state().profile.content_release))(&slot->own_content);
        slot->own_content = 0;
    }
    uintptr_t *refs[] = {&slot->witness.source_info, &slot->witness.memory_set, &slot->witness.hot_set, &slot->witness.memory_content, &slot->witness.hot_content};
    {
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(refs) / sizeof(refs)[0]; ++range_index_3) {
            __auto_type ref = refs[range_index_3];
            if (*ref) {
                ((void (*)(void *))(state().profile.content_release))(ref);
                *ref = 0;
            }
        }
    }
}
static void retire_receipt(Slot *slot) {
    unsigned deck = slot->request.identity.deck;
    uint64_t token = slot->request.identity.token;
    release_witness(slot);
    atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
    if (azsdk_cue_commands_persist_retired) azsdk_cue_commands_persist_retired(deck, token);
}
static void complete_receipt(Slot *slot) {
    if (!atomic_load_explicit(&(slot->receipt_seen), memory_order_acquire)) return;
    unsigned active = ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(active), RECEIPT, memory_order_acq_rel, memory_order_acquire)) return;
    if (atomic_load_explicit(&(slot->retired), memory_order_acquire)) {
        unsigned receipt = RECEIPT;
        if (atomic_compare_exchange_strong_explicit(&(slot->phase), &(receipt), POLLING, memory_order_acq_rel, memory_order_acquire)) {
            retire_receipt(slot);
        }
    } else state().callbacks.ready(slot->request.identity.deck, slot->request.identity.token, state().callbacks.context);
}
static void leave_helper(Slot *slot) {
    atomic_store_explicit(&(slot->phase), ACTIVE, memory_order_release);
    complete_receipt(slot);
}
static void leave_helper_drop(Slot **slot) { leave_helper(&(**slot)); }
/* This helper executes on the native repository queue, after doRequest returns.
 * Match its copied native RequestID serial and TrackID, never thread-local state. */
struct VectorCleanup {
    uintptr_t *retained, *built, *vector;
};
static void vector_cleanup(struct VectorCleanup *c) {
    const struct azcuepersist_profile *p = &(state().profile);
    ((void (*)(void *)) p->vector_destroy)(c->vector);
    if (*c->built) ((void (*)(void *)) p->content_release)(c->built);
    if (*c->retained) ((void (*)(void *)) p->content_release)(c->retained);
}
static bool native_row_matches(Slot *slot, uintptr_t row) {
    return *(const unsigned char *) row == slot->kind &&
           azcue_timestamps_equal(azcue_read_timestamps((const void *) row), slot->native_times, slot->property_rate);
}
static bool hydrate_hot_ticket(Slot *slot, uintptr_t begin, uintptr_t end, const void *cue) {
    uintptr_t original_row = 0;
    for (uintptr_t row = begin; row != end; row += 0x80)
        if (native_row_matches(slot, row)) original_row = row;
    if (!original_row) {
        atomic_store_explicit(&slot->invalid, 1, memory_order_release);
        return false;
    }
    // registerCue owns this copy and builds its native write ticket after this helper returns.
    void *owned_cue = (void *) cue;
    ((void (*)(void *)) state().profile.cue_destroy)(owned_cue);
    ((void (*)(void *, const void *)) state().profile.cue_copy)(owned_cue, (const void *) original_row);
    return mutate_row(slot, (uintptr_t) owned_cue);
}
static bool append_metadata_rows(Slot *slot, uintptr_t *vector, uintptr_t begin, uintptr_t end) {
    unsigned matches = 0;
    for (uintptr_t at = begin; at != end; at += 0x80) {
        bool target = native_row_matches(slot, at);
        ((void (*)(void *, const void *)) state().profile.vector_append)(vector, (const void *) at);
        if (target) {
            ++matches;
            if (!mutate_row(slot, vector[1] - 0x80)) return false;
        }
    }
    return matches == 1 && vector[1] - vector[0] == end - begin;
}
static bool metadata_source_current(Slot *slot, uintptr_t collection, uintptr_t retained) {
    return valid(slot) && word(collection + 0x28) == retained && witness_current(slot);
}
static bool publish_metadata(Slot *slot, bool hot, uintptr_t collection, uintptr_t *built) {
    ((void (*)(void *, const void *)) state().profile.content_copy)(&slot->own_content, built);
    if (slot->own_content != *built) return false;
    uintptr_t publish = hot ? state().profile.hot_content_publish : state().profile.content_publish;
    if (!((bool (*)(void *, const void *, void *)) publish)((void *) collection, built, NULL) || word(collection + 0x28) != *built) return false;
    atomic_store_explicit(&slot->metadata_applied, 1, memory_order_release);
    atomic_store_explicit(&slot->diagnostic, 6, memory_order_release);
    return true;
}
static bool build_metadata_vector(Slot *slot, bool hot, uintptr_t collection, uintptr_t retained, uintptr_t *built, uintptr_t *vector) {
    uintptr_t begin = word(retained + 0x28), end = word(retained + 0x30);
    if (!append_metadata_rows(slot, vector, begin, end) || !metadata_source_current(slot, collection, retained)) return false;
    uintptr_t build = hot ? state().profile.hot_content_from_vector : state().profile.content_from_vector;
    ((void (*)(void *, const void *)) build)(built, vector);
    if (!*built || word(*built + 0x30) - word(*built + 0x28) != end - begin || !metadata_source_current(slot, collection, retained)) return false;
    return publish_metadata(slot, hot, collection, built);
}
static bool build_metadata(Slot *slot, bool hot, uintptr_t collection, uintptr_t target_begin, uintptr_t target_end, const void *cue) {
    uintptr_t retained = 0, built = 0, vector[3] = {0};
    struct VectorCleanup cleanup __attribute__((cleanup(vector_cleanup))) = {&retained, &built, vector};
    (void) cleanup; /* cleanup attribute owns the scope exit */
    if (!pointer(collection)) return false;
    ((void (*)(void *, const void *)) state().profile.content_copy)(&retained, (const void *) (collection + 0x28));
    if (retained != (hot ? slot->witness.hot_content : slot->witness.memory_content) || !witness_current(slot)) return false;
    if (slot->request.operation == AZ_CMD_HOT_SET && !hydrate_hot_ticket(slot, target_begin, target_end, cue)) return false;
    return build_metadata_vector(slot, hot, collection, retained, &built, vector);
}
struct BuildCall {
    Slot *slot;
    bool hot;
    uintptr_t collection, begin, end;
    const void *cue;
    bool result;
};
static void build_call(void *opaque) {
    struct BuildCall *c = opaque;
    c->result = build_metadata(c->slot, c->hot, c->collection, c->begin, c->end, c->cue);
}
struct HelperCall {
    void *function, *cache;
    const void *request, *track, *cue, *set_ref;
    unsigned priority;
    int result;
};
static void helper_call(void *opaque) {
    struct HelperCall *c = opaque;
    c->result = ((int (*)(void *, const void *, const void *, const void *, const void *, unsigned)) c->function)(c->cache, c->request, c->track, c->cue, c->set_ref, c->priority);
}
typedef struct MetadataWork {
    bool hot;
    void *cache;
    const void *request, *track, *cue, *set_ref;
    unsigned priority;
    Slot *slot;
    uintptr_t begin, end;
    uint64_t count;
} MetadataWork;
static int native_metadata(MetadataWork *w) {
    typedef int (*Helper)(void *, const void *, const void *, const void *, const void *, unsigned);
    return original(Helper, w->hot ? &state().hot_original : &state().memory_original)(w->cache, w->request, w->track, w->cue, w->set_ref, w->priority);
}
static int invalid_metadata(Slot *slot) {
    atomic_store_explicit(&slot->invalid, 1, memory_order_release);
    return 1;
}
static bool metadata_candidate(Slot *candidate, MetadataWork *w) {
    return atomic_load_explicit(&candidate->mutated, memory_order_acquire) == 2 && metadata(candidate->request.operation) &&
           (bool) candidate->kind == w->hot && candidate->native_serial && candidate->native_serial == u32((uintptr_t) w->request + 8) &&
           !memcmp(candidate->native_track, w->track, 12);
}
static bool select_metadata_slot(MetadataWork *w) {
    for (unsigned i = 0; i < sizeof state().slots / sizeof state().slots[0]; ++i) {
        Slot *candidate = &state().slots[i];
        unsigned active = ACTIVE;
        if (!atomic_compare_exchange_strong_explicit(&candidate->phase, &active, HELPING, memory_order_acq_rel, memory_order_acquire)) continue;
        if (!metadata_candidate(candidate, w)) {
            leave_helper(candidate);
            continue;
        }
        if (w->slot) {
            invalid_metadata(w->slot);
            invalid_metadata(candidate);
            leave_helper(candidate);
            leave_helper(w->slot);
            return false;
        }
        w->slot = candidate;
    }
    return true;
}
static bool metadata_identity(MetadataWork *w) {
    Slot *slot = w->slot;
    return ((const unsigned char *) w->cue)[0] == slot->kind &&
           azcue_timestamps_equal(azcue_read_timestamps(w->cue), slot->native_times, slot->property_rate) &&
           !memcmp(slot->native_track, slot->witness.source_id, 12) &&
           (creating(slot->request.operation) || word((uintptr_t) w->set_ref) == (w->hot ? slot->witness.hot_set : slot->witness.memory_set));
}
static bool claim_metadata(MetadataWork *w) {
    unsigned expected = 0;
    return atomic_compare_exchange_strong_explicit(&w->slot->helper_claim, &expected, 1, memory_order_acq_rel, memory_order_acquire) &&
           valid(w->slot) && witness_current(w->slot) && metadata_identity(w);
}
static bool bind_empty_publication(Slot *slot, uintptr_t actual, uintptr_t *root, uintptr_t *content) {
    // Bind a newly constructed publication only while it is empty and this exact helper lease is held.
    uintptr_t begin = 0, end = 0;
    uint64_t count = 0;
    uintptr_t actual_content = actual ? word(actual + 0x28) : 0;
    if (!actual || !vector_bounds(actual_content, &begin, &end, &count) || count) return false;
    __auto_type copy = ((void (*)(void *, const void *)) state().profile.content_copy);
    copy(root, &actual);
    if (actual_content) copy(content, &actual_content);
    return *root == actual && *content == actual_content && witness_current(slot);
}
static bool bind_creation_publication(MetadataWork *w) {
    Slot *slot = w->slot;
    if (!creating(slot->request.operation)) return true;
    uintptr_t actual = word((uintptr_t) w->set_ref);
    uintptr_t *root = w->hot ? &slot->witness.hot_set : &slot->witness.memory_set;
    uintptr_t *content = w->hot ? &slot->witness.hot_content : &slot->witness.memory_content;
    return *root ? actual == *root : bind_empty_publication(slot, actual, root, content);
}
static bool metadata_creation_collision(MetadataWork *w, uintptr_t row) {
    unsigned kind = *(const unsigned char *) row;
    struct azcue_native_timestamps times = azcue_read_timestamps((const void *) row);
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    if (kind == 0xff || times.in_us == absent || times.in_sample == absent) return false;
    int collision = compare_timestamps(times.in_us, w->slot->native_times.in_us, 1000000, w->slot->property_rate, 1);
    return (w->hot && kind == w->slot->kind) || (!w->hot && (collision == AZ_TIME_INVALID || collision == AZ_TIME_EQUAL));
}
static int create_metadata(MetadataWork *w) {
    if (!w->hot && w->count >= 10) return invalid_metadata(w->slot);
    for (uintptr_t row = w->begin; row != w->end; row += 0x80)
        if (metadata_creation_collision(w, row)) return invalid_metadata(w->slot);
    int result = native_metadata(w);
    if (result == 0) atomic_store_explicit(&w->slot->metadata_applied, 1, memory_order_release);
    else invalid_metadata(w->slot);
    return result;
}
static bool metadata_target_unique(MetadataWork *w) {
    unsigned matches = 0;
    for (uintptr_t row = w->begin; row != w->end; row += 0x80)
        if (native_row_matches(w->slot, row)) ++matches;
    return matches <= 1 && (w->slot->request.operation == AZ_CMD_HOT_SET || matches == 1);
}
static int update_metadata(MetadataWork *w) {
    Slot *slot = w->slot;
    if (!metadata_target_unique(w)) return invalid_metadata(slot);
    struct HelperCall call = {original_address(w->hot ? &state().hot_original : &state().memory_original), w->cache, w->request, w->track, w->cue, w->set_ref, w->priority, 1};
    struct pending_call pending __attribute__((cleanup(pending_call_drop))) = {slot, 0, false};
    helper_call(&call);
    pending.returned = true;
    int result = call.result;
    if (slot->request.operation == AZ_CMD_HOT_SET && result == 0) {
        atomic_store_explicit(&slot->metadata_applied, 1, memory_order_release);
        return result;
    }
    if (result != 1 || !valid(slot) || !witness_current(slot)) {
        invalid_metadata(slot);
        return result;
    }
    struct BuildCall build = {slot, w->hot, word((uintptr_t) w->set_ref), w->begin, w->end, w->cue, false};
    pending.returned = false;
    build_call(&build);
    pending.returned = true;
    if (build.result) return 0;
    invalid_metadata(slot);
    return result;
}
static int owned_metadata(MetadataWork *w) {
    Slot *leave __attribute__((cleanup(leave_helper_drop))) = w->slot;
    (void) leave; /* cleanup attribute owns the scope exit */
    if (!claim_metadata(w) || !bind_creation_publication(w)) return invalid_metadata(w->slot);
    uintptr_t target = w->hot ? w->slot->witness.hot_content : w->slot->witness.memory_content;
    if (!vector_bounds(target, &w->begin, &w->end, &w->count)) return invalid_metadata(w->slot);
    return creating(w->slot->request.operation) ? create_metadata(w) : update_metadata(w);
}
static int metadata_hook(bool hot, void *cache, const void *request, const void *track, const void *cue, const void *set_ref, unsigned priority) {
    MetadataWork w = {.hot = hot, .cache = cache, .request = request, .track = track, .cue = cue, .set_ref = set_ref, .priority = priority};
    if (atomic_load_explicit(&state().enabled, memory_order_acquire) && request && track && cue && set_ref)
        if (!select_metadata_slot(&w)) return 1;
    return w.slot ? owned_metadata(&w) : native_metadata(&w);
}
static int memory_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(false, a, b, c, d, e, f); }
static int hot_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(true, a, b, c, d, e, f); }
static unsigned request_rejection(Slot *slot, const void *record) {
    uintptr_t input = (uintptr_t) record;
    if (!pointer(input)) return 11;
    if (((const unsigned char *) record)[0] != (unsigned) !deleting(slot->request.operation)) return 12;
    if (u32(input + 4) != slot->kind) return 13;
    if (memcmp((const void *) (input + 8 + 0x30), slot->property_identity, 12)) return 14;
    if (compare_timestamps(word(input + 8 + 0x40), slot->property_in, slot->property_rate, slot->property_rate, 1) != AZ_TIME_EQUAL) return 15;
    return valid(slot) ? 0 : 16;
}
static void request_hook(void *maintainer, const void *record) {
    Slot *slot = submitting;
    if (slot && slot->maintainer == maintainer) {
        uintptr_t input = ((uintptr_t) (record));
        atomic_store_explicit(&(slot->request_time), now(), memory_order_release);
        if (pointer(input)) {
            slot->request_source = word(input + 8 + 0x50);
            slot->request_rate = u32(input + 8 + 0x60);
        }
        unsigned reason = request_rejection(slot, record);
        if (reason) {
            atomic_store_explicit(&(slot->diagnostic), reason, memory_order_release);
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            atomic_store_explicit(&(slot->no_effect), 1, memory_order_release);
            return; /* exact intercepted request was suppressed before repository work */
        }
        atomic_store_explicit(&(slot->diagnostic), 2, memory_order_release);
        atomic_store_explicit(&(slot->request_entered), 1, memory_order_release);
    } else slot = NULL;
    Scope scope __attribute__((cleanup(submitting_scope_drop)));
    submitting_scope_init(&scope, slot);
    original(void (*)(void *, const void *), &state().request_original)(maintainer, record);
    if (slot) {
        unsigned entered = 2;
        atomic_compare_exchange_strong_explicit(&(slot->diagnostic), &(entered), 5, memory_order_acq_rel, memory_order_acquire);
    }
}
static bool mutate_handler(Slot *slot, uintptr_t handler) {
    if (slot->request.operation == AZ_CMD_SAVED_AUTO || slot->request.operation == AZ_CMD_CUE_COMMENT || slot->request.operation == AZ_CMD_CUE_COLOR) {
        if (!slot->witness.selected_cue || !witness_current(slot)) return false;
        ((void (*)(void *))(state().profile.cue_destroy))(((void *) (handler + 0x38)));
        ((void (*)(void *, const void *))(state().profile.cue_copy))(((void *) (handler + 0x38)), ((const void *) (slot->witness.selected_cue)));
    }
    slot->native_serial = u32(handler + 0x30);
    memcpy(slot->native_track, ((const void *) (handler + 0x10)), 12);
    slot->native_times = azcue_read_timestamps(((const void *) (handler + 0x38)));
    if (!mutate_row(slot, handler + 0x38)) return false;
    atomic_store_explicit(&(slot->mutated), 2, memory_order_release);
    atomic_store_explicit(&(slot->diagnostic), 4, memory_order_release);
    return true;
}
struct MutationCall {
    Slot *slot;
    uintptr_t handler;
    bool result;
};
static void mutation_call(void *opaque) {
    struct MutationCall *c = opaque;
    c->result = mutate_handler(c->slot, c->handler);
}
static Slot *bind_submitting_handler(uintptr_t handler) {
    Slot *candidate = submitting;
    uintptr_t maintainer = ((uintptr_t) (candidate->maintainer));
    if (word(handler) == state().profile.handler_table && word(maintainer + 0x160) == handler) {
        uintptr_t receiver = word(handler + 0xc0);
        if (pointer(receiver) && word(receiver) == state().profile.reception_table) {
            atomic_store_explicit(&(candidate->receiver), receiver, memory_order_release);
            atomic_store_explicit(&(candidate->handler), handler, memory_order_release);
            return candidate;
        }
    }
    return NULL;
}
static bool prepare_handler(Slot *slot, uintptr_t handler) {
    atomic_store_explicit(&(slot->handler_time), now(), memory_order_release);
    atomic_store_explicit(&(slot->diagnostic), 3, memory_order_release);
    if (!valid(slot) || atomic_load_explicit(&(state().overflow), memory_order_acquire)) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return false; /* suppress late retries as well */
    }
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&(slot->mutated), &(expected), 1, memory_order_acq_rel, memory_order_acquire)) {
        struct MutationCall call = {slot, handler, false};
        struct pending_call pending __attribute__((cleanup(pending_call_drop))) = {slot, 0, false};
        mutation_call(&call);
        pending.returned = true;
        if (!call.result) {
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            return false;
        }
    } else if (expected != 2) return false;
    return true;
}
static bool handler_hook(void *self) {
    if (!atomic_load_explicit(&state().enabled, memory_order_acquire)) return original(bool (*)(void *), &state().handler_original)(self);
    uintptr_t handler = (uintptr_t) self;
    Slot *slot = for_handler(handler);
    if (!slot && submitting) slot = bind_submitting_handler(handler);
    if (slot && !prepare_handler(slot, handler)) return false;
    return original(bool (*)(void *), &state().handler_original)(self);
}
static bool reference_present(const void *ref) { return ref && word((uintptr_t) ref); }
static bool capture_reply(Slot *owner, const void *memory, const void *hot) {
    __auto_type copy = ((void (*)(void *, const void *))(state().profile.content_copy));
    bool memory_present = reference_present(memory);
    bool hot_present = reference_present(hot);
    copy(&owner->reply_memory_set, memory_present ? memory : &owner->witness.memory_set);
    copy(&owner->reply_hot_set, hot_present ? hot : &owner->witness.hot_set);
    if (!creating(owner->request.operation) && (!owner->reply_memory_set || !owner->reply_hot_set)) return false;
    if (memory_present) copy(&owner->reply_memory_content, ((const void *) (owner->reply_memory_set + 0x28)));
    if (hot_present) copy(&owner->reply_hot_content, ((const void *) (owner->reply_hot_set + 0x28)));
    atomic_store_explicit(&(owner->reply_captured), 2, memory_order_release);
    return true;
}
struct ReplyCall {
    Slot *owner;
    const void *memory, *hot;
    bool result;
};
static void reply_call(void *opaque) {
    struct ReplyCall *c = opaque;
    c->result = capture_reply(c->owner, c->memory, c->hot);
}
static Slot *reply_owner(uintptr_t receiver) {
    Slot *owner = NULL;
    {
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(state().slots) / sizeof(state().slots)[0]; ++range_index_5) {
            __auto_type slot = &(state().slots[range_index_5]);

            if ((atomic_load_explicit(&(slot->phase), memory_order_acquire) == ACTIVE || atomic_load_explicit(&(slot->phase), memory_order_acquire) == HELPING) && atomic_load_explicit(&(slot->receiver), memory_order_acquire) == receiver) {
                owner = slot;
                break;
            }
        }
    }
    return owner;
}
static void capture_owned_reply(Slot *owner, const void *request_id, const void *track, const void *memory, const void *hot) {
    unsigned absent = 0;
    if (!request_id || !track || !atomic_load_explicit(&(owner->metadata_applied), memory_order_acquire) ||
        u32(((uintptr_t) (request_id)) + 8) != owner->native_serial || memcmp(track, owner->native_track, 12) ||
        !atomic_compare_exchange_strong_explicit(&(owner->reply_captured), &(absent), 1, memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&(owner->invalid), 1, memory_order_release);
    } else {
        struct ReplyCall call = {owner, memory, hot, false};
        struct pending_call pending __attribute__((cleanup(pending_call_drop))) = {owner, 0, false};
        reply_call(&call);
        pending.returned = true;
        if (!call.result) atomic_store_explicit(&(owner->invalid), 1, memory_order_release);
    }
}
static Receipt *reserve_receipt(uintptr_t receiver, const void *track, const int *result) {
    Receipt *receipt = NULL;
    if (track && result && pointer(receiver)) {
        {
            for (unsigned range_index_6 = 0; range_index_6 < sizeof(state().receipts) / sizeof(state().receipts)[0]; ++range_index_6) {
                __auto_type entry = &(state().receipts[range_index_6]);

                unsigned free = FREE;
                if (atomic_compare_exchange_strong_explicit(&(entry->phase), &(free), WRITING, memory_order_acq_rel, memory_order_acquire)) {
                    receipt = entry;
                    break;
                }
            }
        }
    }
    return receipt;
}
static void publish_native_receipt(Receipt *receipt, Slot *owner, uintptr_t receiver, const void *track, const int *result) {
    uint64_t sequence = atomic_fetch_add_explicit(&(state().sequence), 1, memory_order_acq_rel);
    if (sequence == UINT64_MAX) atomic_store_explicit(&(state().overflow), 1, memory_order_release);
    receipt->sequence = sequence;
    receipt->maintainer = word(receiver + 0x10);
    receipt->tag = u32(receiver + 8);
    receipt->result = *result;
    memcpy(receipt->track, track, 16);
    receipt->token = owner ? owner->request.identity.token : 0;
    receipt->deck = owner ? owner->request.identity.deck : 4;
    atomic_store_explicit(&(receipt->phase), ACTIVE, memory_order_release);
}
static bool metadata_reply(Slot *owner, const int *result) {
    return owner && metadata(owner->request.operation) && result && (*result == 0 || *result == 5);
}
static void reply_hook(void *self, const void *request_id, const void *track, const void *memory, const void *hot, const int *result) {
    if (!atomic_load_explicit(&state().enabled, memory_order_acquire)) {
        original(void (*)(void *, const void *, const void *, const void *, const void *, const int *), &state().reply_original)(self, request_id, track, memory, hot, result);
        return;
    }
    uintptr_t receiver = (uintptr_t) self;
    Slot *owner = reply_owner(receiver);
    if (metadata_reply(owner, result)) capture_owned_reply(owner, request_id, track, memory, hot);
    if (owner) PERSIST_DIAG(owner->request.identity.deck, owner->request.identity.token, "reply", result ? *result : -1);
    Receipt *receipt = reserve_receipt(receiver, track, result);
    if (receipt) publish_native_receipt(receipt, owner, receiver, track, result);
    else atomic_store_explicit(&state().overflow, 1, memory_order_release);
    original(void (*)(void *, const void *, const void *, const void *, const void *, const int *), &state().reply_original)(self, request_id, track, memory, hot, result);
}
static Receipt *registered_receipt(uintptr_t p) {
    Receipt *receipt = NULL;
    uint64_t sequence = UINT64_MAX;
    {
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(state().receipts) / sizeof(state().receipts)[0]; ++range_index_7) {
            __auto_type entry = &(state().receipts[range_index_7]);

            unsigned expected = ACTIVE;
            if (!atomic_compare_exchange_strong_explicit(&(entry->phase), &(expected), POLLING, memory_order_acq_rel, memory_order_acquire)) continue;
            if (entry->maintainer == word(p) && entry->tag == u32(p + 12) && entry->result == (int) (u32(p + 8)) &&
                !memcmp(entry->track, ((const void *) (p + 16)), 16) && entry->sequence < sequence) {
                if (receipt) atomic_store_explicit(&(receipt->phase), ACTIVE, memory_order_release);
                receipt = entry;
                sequence = entry->sequence;
            } else atomic_store_explicit(&(entry->phase), ACTIVE, memory_order_release);
        }
    }
    return receipt;
}
static Slot *registered_slot(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return NULL;
    Slot *slot = &(state().slots[deck]);
    unsigned phase = atomic_load_explicit(&(slot->phase), memory_order_acquire);
    if (phase != ACTIVE && phase != HELPING) return NULL;
    return slot->request.identity.token == token ? slot : NULL;
}
static void settle_registered(Receipt *receipt) {
    if (!receipt) return;
    unsigned deck = receipt->deck;
    uint64_t token = receipt->token;
    int result = receipt->result;
    atomic_store_explicit(&(receipt->phase), FREE, memory_order_release);
    Slot *slot = registered_slot(deck, token);
    if (!slot) return;
    atomic_store_explicit(&(slot->native_result), result, memory_order_relaxed);
    atomic_store_explicit(&(slot->receipt_seen), 1, memory_order_release);
    complete_receipt(slot);
}
static _Thread_local const void *quiet_memory_track;
struct MemoryNoticeScope {
    const void *previous;
};
static void memory_notice_scope_init(struct MemoryNoticeScope *scope, const void *track) {
    scope->previous = quiet_memory_track;
    quiet_memory_track = track;
}
static void memory_notice_scope_drop(struct MemoryNoticeScope *scope) {
    quiet_memory_track = scope->previous;
}
static void memory_notice_hook(void *self, const void *track) {
    if (track && track == quiet_memory_track) return;
    original(void (*)(void *, const void *), &state().memory_notice_original)(self, track);
}
static void call_registered(void *payload, const Receipt *receipt) {
    struct MemoryNoticeScope scope __attribute__((cleanup(memory_notice_scope_drop)));
    Slot *slot = receipt ? registered_slot(receipt->deck, receipt->token) : NULL;
    /* The native registration task hands its TrackID at +0x10 to the
     * success caution. Keep storage, observer updates and manual feedback. */
    memory_notice_scope_init(&scope, slot && !slot->kind && creating(slot->request.operation) ? (char *) payload + 0x10 : NULL);
    original(void (*)(void *), &state().registered_original)(payload);
}
static void registered_hook(void *payload) {
    if (!atomic_load_explicit(&(state().enabled), memory_order_acquire)) {
        original(void (*)(void *), &state().registered_original)(payload);
        return;
    }
    Receipt *receipt = registered_receipt((uintptr_t) payload);
    /* Actual native task disposes its active request/handler before notifying
     * our actor continuation. Merely seeing admission never completes a command. */
#if defined(AZSDK_CUE_DIAGNOSTIC)
    unsigned diagnostic_deck = receipt ? receipt->deck : 4;
    uint64_t diagnostic_token = receipt ? receipt->token : 0;
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-enter", receipt->result);
#endif
    call_registered(payload, receipt);
#if defined(AZSDK_CUE_DIAGNOSTIC)
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-return", receipt->result);
#endif
    settle_registered(receipt);
}

struct InstallCall {
    const struct azcuepersist_profile *profile;
    azexit_install_fn install;
    bool result;
};
static void install_call(void *opaque) {
    struct InstallCall *c = opaque;
    const struct azcuepersist_profile *profile = c->profile;
    azexit_install_fn install = c->install;

    if (!install(profile->request, ((void *) (request_hook)), &state().request_original) ||
        !install(profile->handler_request, ((void *) (handler_hook)), &state().handler_original) ||
        !install(profile->reception_reply, ((void *) (reply_hook)), &state().reply_original) ||
        !install(profile->registered_task, ((void *) (registered_hook)), &state().registered_original) ||
        !install(profile->memory_helper, ((void *) (memory_hook)), &state().memory_original) ||
        !install(profile->hot_helper, ((void *) (hot_hook)), &state().hot_original)) return;
    if (profile->memory_notice) (void) install(profile->memory_notice, (void *) memory_notice_hook, &state().memory_notice_original);
    c->result = true;
}
static bool persist_profile_ready(const struct azcuepersist_profile *p) {
    const uintptr_t entries[] = {p->save, p->erase, p->request, p->handler_request, p->reception_reply,
                                 p->registered_task, p->handler_table, p->reception_table, p->string_allocate, p->string_assign,
                                 p->string_destroy, p->color_construct, p->memory_helper, p->content_copy, p->content_release,
                                 p->vector_append, p->vector_destroy, p->content_from_vector, p->content_publish, p->hot_helper,
                                 p->hot_content_from_vector, p->hot_content_publish, p->cue_copy, p->cue_destroy};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; ++i)
        if (!entries[i]) return false;
    return true;
}
static bool configure_input_valid(const struct azcuepersist_profile *profile, azexit_install_fn install, const struct azcuepersist_callbacks *callbacks) {
    return profile && callbacks && install && callbacks->validate && callbacks->confirm && callbacks->ready && persist_profile_ready(profile);
}
int azsdk_cue_persist_configure(const struct azcuepersist_profile *profile, azexit_install_fn install,
                                const struct azcuepersist_callbacks *callbacks) {
    if (!configure_input_valid(profile, install, callbacks)) return 0;
    unsigned attempted = 0;
    if (!atomic_compare_exchange_strong_explicit(&(state().attempted), &(attempted), 1, memory_order_acq_rel, memory_order_acquire)) return 0;
    state().profile = *profile;
    state().callbacks = *callbacks;
    struct InstallCall call = {profile, install, false};
    install_call(&call);
    if (!call.result) return 0;
    atomic_store_explicit(&(state().enabled), 1, memory_order_release);
    return 1;
}
uint64_t azsdk_cue_persist_capabilities(void) {
    if (!atomic_load_explicit(&(state().enabled), memory_order_acquire) || atomic_load_explicit(&(state().overflow), memory_order_acquire)) return 0;
    uint64_t result = 0;
    for (unsigned operation = AZ_CMD_SAVED_SAVE; operation <= AZ_CMD_CUE_LOOP_SET; ++operation)
        if (supported(operation)) result |= UINT64_C(1) << operation;
    return result;
}
struct ProducerCall {
    Slot *slot;
    const struct azexit_cue_request *request;
    void *maintainer;
    unsigned kind;
    const void *property;
};
static void producer_call(void *opaque) {
    struct ProducerCall *c = opaque;
    Slot *slot = &(*c->slot);
    const struct azexit_cue_request *request = c->request;
    void *maintainer = c->maintainer;
    unsigned kind = c->kind;
    const void *property = c->property;

    atomic_store_explicit(&(slot->diagnostic), 20, memory_order_release);
    uintptr_t producer = deleting(request->operation) ? state().profile.erase : state().profile.save;
    PERSIST_DIAG(request->identity.deck, request->identity.token, "producer-enter", 0);
    ((void (*)(void *, unsigned, const void *))(producer))(maintainer, kind, property);
    PERSIST_DIAG(request->identity.deck, request->identity.token, "producer-return", 0);
}
static bool submit_identity_valid(const struct azexit_cue_request *r) {
    return r && r->size >= sizeof *r && r->identity.deck < 4 && r->identity.token &&
           r->identity.load_generation && r->identity.deadline_ns && !r->identity.reserved;
}
static bool submit_color_supported(uint32_t rgb) {
    const unsigned colors[] = {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu};
    for (unsigned i = 0; i < sizeof colors / sizeof *colors; ++i)
        if (rgb == colors[i]) return true;
    return false;
}
static bool submit_arguments_valid(const struct azexit_cue_request *r, unsigned kind) {
    if ((r->operation == AZ_CMD_HOT_SET || r->operation == AZ_CMD_HOT_CLEAR) && (!kind || kind != r->entry_id)) return false;
    return (r->flags & AZ_COMMAND_PERSIST) && r->rgb <= 0xffffffu &&
           (r->operation != AZ_CMD_SAVED_AUTO || r->position_s == 0.0 || r->position_s == 1.0);
}
static int submit_operation_valid(const struct azexit_cue_request *r, unsigned kind) {
    if (!supported(r->operation) || !(azsdk_cue_persist_capabilities() & (UINT64_C(1) << r->operation))) return AZ_UNSUPPORTED;
    if (!submit_arguments_valid(r, kind)) return AZ_INVALID;
    size_t length;
    if (!utf8(r->comment, &length)) return AZ_INVALID;
    if (r->operation == AZ_CMD_CUE_COLOR && !kind && !submit_color_supported(r->rgb)) return AZ_UNSUPPORTED;
    return AZ_OK;
}
static bool submit_witness_valid(const struct azexit_cue_request *r, const struct azcuepersist_witness *w) {
    return w && w->source_info && w->fingerprint && w->revision &&
           (r->operation == AZ_CMD_HOT_SET || w->revision == r->expected_revision);
}
static bool copy_submit_witness(Slot *slot, const struct azcuepersist_witness *witness) {
    slot->witness = *witness;
    uintptr_t *refs[] = {&slot->witness.source_info, &slot->witness.memory_set, &slot->witness.hot_set, &slot->witness.memory_content, &slot->witness.hot_content};
    uintptr_t input[] = {witness->source_info, witness->memory_set, witness->hot_set, witness->memory_content, witness->hot_content};
    {
        for (unsigned range_index_9 = 0; range_index_9 < sizeof(refs) / sizeof(refs)[0]; ++range_index_9) {
            __auto_type ref = refs[range_index_9];
            *ref = 0;
        }
    }
    for (unsigned i = 0; i < 5; ++i)
        if (input[i])
            ((void (*)(void *, const void *))(state().profile.content_copy))(refs[i], &input[i]);
    bool same = true;
    for (unsigned i = 0; i < 5; ++i) same = same && *refs[i] == input[i];
    return same && witness_current(slot);
}
static int retain_submit_witness(Slot *slot, const struct azcuepersist_witness *witness) {
    const struct azexit_cue_request *request = &slot->request;
    if (!metadata(request->operation)) return AZ_OK;
    if (!submit_witness_valid(request, witness)) {
        atomic_store_explicit(&slot->phase, FREE, memory_order_release);
        return AZ_STALE;
    }
    if (!copy_submit_witness(slot, witness)) {
        atomic_store_explicit(&(slot->diagnostic), 40, memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-witness", AZ_STALE);
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_STALE;
    }
    return AZ_OK;
}
static bool selected_row_matches(uintptr_t row, uintptr_t locator, unsigned kind, uint32_t rate) {
    return *(const unsigned char *) row == kind && *(const unsigned char *) locator == kind &&
           azcue_timestamps_equal(azcue_read_timestamps((const void *) row), azcue_read_timestamps((const void *) locator), rate);
}
static uintptr_t submit_selected_row(Slot *slot, const struct azcuepersist_witness *witness, unsigned kind, uint32_t property_rate) {
    uintptr_t locator = witness ? witness->selected_cue : 0;
    uintptr_t content = kind ? slot->witness.hot_content : slot->witness.memory_content, begin, end;
    uint64_t count;
    uintptr_t selected = 0;
    if (pointer(locator) && vector_bounds(content, &(begin), &(end), &(count))) {
        for (uintptr_t row = begin; row != end; row += 0x80) {
            if (!selected_row_matches(row, locator, kind, property_rate)) continue;
            if (selected) {
                selected = 0;
                break;
            }
            selected = row;
        }
    }
    return selected;
}
static int retain_submit_selection(Slot *slot, const struct azcuepersist_witness *witness, unsigned kind, uint32_t property_rate) {
    unsigned operation = slot->request.operation;
    if (operation != AZ_CMD_SAVED_AUTO && operation != AZ_CMD_CUE_COMMENT && operation != AZ_CMD_CUE_COLOR) return AZ_OK;
    uintptr_t selected = submit_selected_row(slot, witness, kind, property_rate);
    if (!selected) {
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_STALE;
    }
    slot->witness.selected_cue = selected;
    return AZ_OK;
}
static void initialize_submit(Slot *slot, void *manager, void *maintainer, unsigned kind, uint64_t cookie) {
    atomic_store_explicit(&(slot->manager), manager, memory_order_release);
    slot->maintainer = maintainer;
    slot->cookie = cookie;
    slot->kind = kind;
    atomic_store_explicit(&(slot->handler), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->receiver), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->invalid), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->retired), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->mutated), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->native_result), -1, memory_order_relaxed);
    atomic_store_explicit(&(slot->receipt_seen), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->diagnostic), 1, memory_order_relaxed);
    atomic_store_explicit(&(slot->no_effect), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->request_entered), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->submit_time), now(), memory_order_relaxed);
    atomic_store_explicit(&(slot->request_time), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->handler_time), 0, memory_order_relaxed);
    slot->request_source = 0;
    slot->request_rate = 0;
}
static unsigned queue_busy_reason(uintptr_t m, uintptr_t q) {
    unsigned busy = (!pointer(q) ? 1u : 0u) | (word(m + 0x158) ? 2u : 0u) | (word(m + 0x160) ? 4u : 0u) |
                    (pointer(q) && word(q + 0x48) != word(q + 0x68) ? 8u : 0u);
    return busy;
}
static int submit_preflight(Slot *slot, void *maintainer) {
    const struct azexit_cue_request *request = &slot->request;
    (void) request;
    uintptr_t m = ((uintptr_t) (maintainer)), q = word(m + 0x150);
    if (!valid(slot)) {
        atomic_store_explicit(&(slot->diagnostic), 41, memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-invalid", AZ_STALE);
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_STALE;
    }
    if (!pointer(q) || word(m + 0x158) || word(m + 0x160) || word(q + 0x48) != word(q + 0x68)) {
        unsigned busy = queue_busy_reason(m, q);
        atomic_store_explicit(&(slot->diagnostic), 48 + busy, memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-queue", (int) (busy));
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_BUSY;
    }
    return AZ_OK;
}
static int submit_producer(Slot *slot, const struct azexit_cue_request *request, void *maintainer, unsigned kind, const void *property, uint32_t property_rate) {
    uintptr_t prop = (uintptr_t) property;
    memcpy(slot->property_identity, ((const void *) (prop + 0x30)), 12);
    slot->property_in = word(prop + 0x40);
    slot->property_source = word(prop + 0x50);
    slot->property_rate = property_rate;
    atomic_store_explicit(&(slot->phase), ACTIVE, memory_order_release);
    Scope scope __attribute__((cleanup(submitting_scope_drop)));
    submitting_scope_init(&scope, slot);
    struct ProducerCall call = {slot, request, maintainer, kind, property};
    struct pending_call pending __attribute__((cleanup(pending_call_drop))) = {slot, 32, false};
    producer_call(&call);
    pending.returned = true;
    if (atomic_load_explicit(&(slot->no_effect), memory_order_acquire) && !atomic_load_explicit(&(slot->request_entered), memory_order_acquire) &&
        !atomic_load_explicit(&(slot->handler), memory_order_acquire)) {
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_STALE;
    }
    unsigned producer_entered = 20;
    atomic_compare_exchange_strong_explicit(&(slot->diagnostic), &(producer_entered), 21, memory_order_acq_rel, memory_order_acquire);
    /* If a foreign native path bypassed the checked request/handler route, keep
     * this deck fenced; we cannot attest that the producer had no effect. */
    if (!atomic_load_explicit(&(slot->handler), memory_order_acquire)) atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
    return AZ_PENDING;
}
static int prepare_submit(Slot *slot, const struct azexit_cue_request *request, const struct azcuepersist_witness *witness, unsigned kind, uint32_t property_rate) {
    slot->request = *request;
    atomic_store_explicit(&(slot->helper_claim), 0, memory_order_relaxed);
    slot->native_serial = 0;
    atomic_store_explicit(&(slot->reply_captured), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->metadata_applied), 0, memory_order_relaxed);
    slot->witness = (struct azcuepersist_witness) {0};
    int rc = retain_submit_witness(slot, witness);
    if (rc != AZ_OK) return rc;
    return retain_submit_selection(slot, witness, kind, property_rate);
}
static bool submit_input_valid(const struct azexit_cue_request *request, void *manager, void *maintainer, const void *property, uint64_t cookie, unsigned kind) {
    return submit_identity_valid(request) && manager && maintainer && property && cookie && kind <= 8;
}
int azsdk_cue_persist_submit(const struct azexit_cue_request *request, void *manager, void *maintainer,
                             unsigned kind, const void *property, uint64_t cookie, const struct azcuepersist_witness *witness) {
    if (!submit_input_valid(request, manager, maintainer, property, cookie, kind)) return AZ_INVALID;
    uint32_t property_rate = u32((uintptr_t) property + 0x60);
    int rc = submit_operation_valid(request, kind);
    if (rc != AZ_OK) return rc;
    Slot *slot = &state().slots[request->identity.deck];
    unsigned free = FREE;
    if (!atomic_compare_exchange_strong_explicit(&slot->phase, &free, WRITING, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    rc = prepare_submit(slot, request, witness, kind, property_rate);
    if (rc != AZ_OK) return rc;
    initialize_submit(slot, manager, maintainer, kind, cookie);
    rc = submit_preflight(slot, maintainer);
    if (rc != AZ_OK) return rc;
    return submit_producer(slot, request, maintainer, kind, property, property_rate);
}
static bool publication_receipt_ready(Slot *slot) {
    int result = atomic_load_explicit(&slot->native_result, memory_order_acquire);
    return atomic_load_explicit(&slot->phase, memory_order_acquire) == POLLING &&
           atomic_load_explicit(&slot->reply_captured, memory_order_acquire) == 2 &&
           atomic_load_explicit(&slot->metadata_applied, memory_order_acquire) && (result == 0 || result == 5);
}
static bool publication_request_matches(Slot *slot, const struct azexit_cue_request *request) {
    const struct azexit_cue_request *saved = &slot->request;
    return saved->identity.token == request->identity.token && saved->identity.load_generation == request->identity.load_generation &&
           saved->operation == request->operation && saved->expected_revision == request->expected_revision && saved->entry_id == request->entry_id &&
           saved->classification == request->classification &&
           compare_timestamps(saved->position_s, request->position_s, 1, slot->property_rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(saved->end_s, request->end_s, 1, slot->property_rate, 1) == AZ_TIME_EQUAL;
}
static bool publication_source_matches(Slot *slot, const struct azcuepersist_witness *fresh) {
    return fresh->revision && fresh->source_info == slot->witness.source_info && fresh->memory_set == slot->reply_memory_set &&
           fresh->hot_set == slot->reply_hot_set && !memcmp(fresh->source_id, slot->native_track, 12);
}
static bool publication_content_current(const struct azcuepersist_witness *fresh) {
    return fresh->fingerprint && witness_hash(fresh) == fresh->fingerprint &&
           publication_matches(fresh->memory_set, fresh->memory_content) && publication_matches(fresh->hot_set, fresh->hot_content);
}
static bool publication_confirmed(Slot *slot, const struct azexit_cue_request *request, const struct azcuepersist_witness *fresh) {
    if (!publication_receipt_ready(slot) || !publication_request_matches(slot, request) || !valid(slot)) return false;
    if (!publication_source_matches(slot, fresh) || !publication_content_current(fresh) || !valid(slot)) return false;
    return true;
}
int azsdk_cue_persist_confirm_publication(const struct azexit_cue_request *request, const struct azcuepersist_witness *fresh) {
    if (!request || !fresh || request->identity.deck >= 4) return 0;
    Slot *slot = &state().slots[request->identity.deck];
    if (!publication_confirmed(slot, request, fresh)) return 0;
    return fresh->memory_content == slot->reply_memory_content && fresh->hot_content == slot->reply_hot_content ? 1 : 2;
}
struct ConfirmCall {
    Slot *slot;
    void *manager;
    uint64_t *revision;
    bool result;
};
static void confirm_call(void *opaque) {
    struct ConfirmCall *c = opaque;
    Slot *slot = c->slot;
    c->result = state().callbacks.confirm(&slot->request, c->manager, slot->cookie, c->revision, state().callbacks.context) && *c->revision;
}
static bool receipt_eligible(Slot *slot) {
    return !atomic_load_explicit(&state().overflow, memory_order_acquire) && valid(slot);
}
static int finish_receipt(Slot *slot, struct azexit_result *out, bool applied, uint64_t revision) {
    *out = (struct azexit_result) {slot->request.identity.token, slot->request.identity.load_generation, applied ? revision : 0,
                                   applied ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, applied ? AZ_OK : AZ_FAILED};
    release_witness(slot);
    atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
    return AZ_OK;
}
static bool receipt_owned(Slot *slot, const struct azexit_cue_request *request, void *manager) {
    return slot->request.identity.token == request->identity.token && atomic_load_explicit(&slot->manager, memory_order_acquire) == manager;
}
static int observe_receipt(Slot *slot, void *manager, bool *applied, uint64_t *revision) {
    int native_result = atomic_load_explicit(&(slot->native_result), memory_order_acquire);
    bool native_success = native_result == 0 || native_result == 5;
    bool eligible = receipt_eligible(slot) && native_success;
    if (eligible) {
        struct ConfirmCall call = {slot, manager, revision, false};
        confirm_call(&call);
        *applied = call.result;
        /* The repository reply can precede publication of the source-qualified
         * CueSet. Observe on fresh owner continuations until the ORIGINAL
         * deadline; never submit the native save/delete producer again. */
        eligible = receipt_eligible(slot);
        if (!*applied && eligible) {
            return AZ_PENDING;
        }
        *applied = *applied && eligible;
    }
    return AZ_OK;
}
int azsdk_cue_persist_take(const struct azexit_cue_request *request, void *manager, struct azexit_result *out) {
    if (!request || !out || request->identity.deck >= 4 || !manager) return AZ_INVALID;
    Slot *slot = &(state().slots[request->identity.deck]);
    unsigned phase = RECEIPT;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(phase), POLLING, memory_order_acq_rel, memory_order_acquire)) return phase == FREE ? AZ_STALE : AZ_PENDING;
    Slot *polling __attribute__((cleanup(polling_drop))) = slot;
    (void) polling; /* cleanup attribute owns the scope exit */
    if (!receipt_owned(slot, request, manager)) {
        polling = NULL;
        atomic_store_explicit(&(slot->phase), RECEIPT, memory_order_release);
        return AZ_STALE;
    }
    bool applied = false;
    uint64_t revision = 0;
    if (observe_receipt(slot, manager, &applied, &revision) == AZ_PENDING) {
        polling = NULL;
        atomic_store_explicit(&slot->phase, RECEIPT, memory_order_release);
        return AZ_PENDING;
    }
    int rc = finish_receipt(slot, out, applied, revision);
    polling = NULL;
    return rc;
}
int azsdk_cue_persist_poll(const struct azexit_cue_request *request, void *manager, struct azexit_result *out) {
    return azsdk_cue_persist_take(request, manager, out);
}
void azsdk_cue_persist_retire(void *manager) {
    {
        for (unsigned range_index_10 = 0; range_index_10 < sizeof(state().slots) / sizeof(state().slots)[0]; ++range_index_10) {
            __auto_type slot = &(state().slots[range_index_10]);

            unsigned phase = atomic_load_explicit(&(slot->phase), memory_order_acquire);
            if ((phase == ACTIVE || phase == HELPING || phase == RECEIPT || phase == POLLING) && atomic_load_explicit(&(slot->manager), memory_order_acquire) == manager) {
                atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                atomic_store_explicit(&(slot->retired), 1, memory_order_release);
                unsigned receipt_phase = RECEIPT;
                if (atomic_compare_exchange_strong_explicit(&(slot->phase), &(receipt_phase), POLLING, memory_order_acq_rel, memory_order_acquire)) {
                    retire_receipt(slot);
                }
            }
        }
    }
}

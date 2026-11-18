/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuepersist.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <string.h>
#include "../common/aznative_c.h"

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
};
static State persist_state = {.slots = {[0 ... 3] = {.native_result = -1}}, .receipts = {[0 ... 63] = {.result = -1, .deck = 4}}};
#define state() (persist_state)
_Thread_local Slot *submitting = NULL;
typedef struct Scope Scope;
struct Scope {
    Slot *previous;
};
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
    switch (operation) {
    case AZ_CMD_HOT_SET:
    case AZ_CMD_HOT_CLEAR:
    case AZ_CMD_MEMORY_SET:
    case AZ_CMD_MEMORY_DELETE:
    case AZ_CMD_SAVED_SAVE:
    case AZ_CMD_SAVED_DELETE:
    case AZ_CMD_SAVED_AUTO:
    case AZ_CMD_CUE_COMMENT:
    case AZ_CMD_CUE_COLOR:
    case AZ_CMD_CUE_POINT_SET:
    case AZ_CMD_CUE_LOOP_SET: return true;
    default: return false;
    }
}
static bool deleting(unsigned operation) { return operation == AZ_CMD_HOT_CLEAR || operation == AZ_CMD_MEMORY_DELETE || operation == AZ_CMD_SAVED_DELETE; }
static bool utf8(const char *text, size_t *length) {
    (*length) = 0;
    while ((*length) < 128 && text[(*length)]) ++(*length);
    if ((*length) == 128) return false;
    for (size_t i = 0; i < (*length);) {
        unsigned char c = ((unsigned char) (text[i++]));
        if (c < 0x80) continue;
        unsigned count, minimum, value;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            minimum = 0x80;
            value = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            minimum = 0x800;
            value = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            minimum = 0x10000;
            value = c & 7;
        } else return false;
        if (count > (*length) - i) return false;
        while (count--) {
            unsigned char tail = ((unsigned char) (text[i++]));
            if ((tail & 0xc0) != 0x80) return false;
            value = (value << 6) | (tail & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
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
    return az_native_try(validate_call, &call) && call.result;
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
typedef struct {
    void *data;
} NativeString;
struct StringRelease {
    NativeString *text;
    uintptr_t destroy;
};
static void string_release_drop(struct StringRelease *r) { ((void (*)(void *)) r->destroy)(r->text); }
static bool mutate_row(Slot *slot, uintptr_t row) {
    __auto_type profile = &(state().profile);
    if (slot->request.operation == AZ_CMD_CUE_COMMENT || creating(slot->request.operation)) {
        size_t length = 0;
        if (!utf8(slot->request.comment, &(length))) return false;
        NativeString text = {0};
        az_native_result((void *) profile->string_allocate, &text, (const void *) (uintptr_t) (length + 1));
        if (!text.data) return false;
        struct StringRelease release __attribute__((cleanup(string_release_drop))) = {&text, profile->string_destroy};
        memcpy(text.data, slot->request.comment, length + 1);
        ((void (*)(void *, const void *))(profile->string_assign))(((void *) (row + 0x70)), &text);
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
static bool witness_current(const Slot *s) {
    __auto_type w = &(s->witness);
    return w->source_info && (creating(s->request.operation) || (w->memory_set && w->hot_set)) && w->revision && (s->request.operation == AZ_CMD_HOT_SET || w->revision == s->request.expected_revision) && w->fingerprint &&
           (w->memory_set ? word(w->memory_set + 0x28) == w->memory_content : !w->memory_content) &&
           (w->hot_set ? word(w->hot_set + 0x28) == w->hot_content : !w->hot_content) &&
           witness_hash(w) == w->fingerprint;
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
static bool build_metadata(Slot *slot, bool hot, uintptr_t collection, uintptr_t target_begin, uintptr_t target_end, const void *cue) {
    uintptr_t retained = 0, built = 0, vector[3] = {0};
    struct VectorCleanup cleanup __attribute__((cleanup(vector_cleanup))) = {&retained, &built, vector};

    if (!pointer(collection)) return false;
    ((void (*)(void *, const void *))(state().profile.content_copy))(&retained, ((const void *) (collection + 0x28)));
    if (retained != (hot ? slot->witness.hot_content : slot->witness.memory_content) || !witness_current(slot)) return false;
    if (slot->request.operation == AZ_CMD_HOT_SET) {
        uintptr_t original_row = 0;
        for (uintptr_t row = target_begin; row != target_end; row += 0x80)
            if (*((const unsigned char *) (row)) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(((const void *) (row))), slot->native_times, slot->property_rate)) original_row = row;
        if (!original_row) {
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            return false;
        }
            /* registerCue owns this queued/inline copy and constructs the actual
             * write ticket from it only after the helper returns. */
        void *owned_cue = ((void *) (cue));
        ((void (*)(void *))(state().profile.cue_destroy))(owned_cue);
        ((void (*)(void *, const void *))(state().profile.cue_copy))(owned_cue, ((const void *) (original_row)));
        if (!mutate_row(slot, ((uintptr_t) (owned_cue)))) return false;
    }

    uintptr_t begin = word(retained + 0x28), end = word(retained + 0x30);
    unsigned matches = 0;
    for (uintptr_t at = begin; at != end; at += 0x80) {
        bool target = *((const unsigned char *) (at)) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(((const void *) (at))), slot->native_times, slot->property_rate);
        ((void (*)(void *, const void *))(state().profile.vector_append))(vector, ((const void *) (at)));
        if (target) {
            ++matches;
            if (!mutate_row(slot, vector[1] - 0x80)) return false;
        }
    }
    if (matches != 1 || vector[1] - vector[0] != end - begin || !valid(slot) ||
        word(collection + 0x28) != retained || !witness_current(slot)) return false;
    ((void (*)(void *, const void *))((hot ? state().profile.hot_content_from_vector : state().profile.content_from_vector)))(&built, vector);
    if (!built || word(built + 0x30) - word(built + 0x28) != end - begin || !valid(slot) ||
        word(collection + 0x28) != retained || !witness_current(slot)) return false;
    ((void (*)(void *, const void *))(state().profile.content_copy))(&slot->own_content, &built);
    if (slot->own_content != built) return false;
    if (!((bool (*)(void *, const void *, void *))((hot ? state().profile.hot_content_publish : state().profile.content_publish)))(((void *) (collection)), &built, NULL) ||
        word(collection + 0x28) != built) return false;
    atomic_store_explicit(&(slot->metadata_applied), 1, memory_order_release);
    atomic_store_explicit(&(slot->diagnostic), 6, memory_order_release);
    return true; /* official caller persists the selected cue through its native ticket */
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
static int metadata_hook(bool hot, void *cache, const void *request, const void *track, const void *cue, const void *set_ref, unsigned priority) {
    typedef int (*Helper)(void *, const void *, const void *, const void *, const void *, unsigned);
    Slot *slot = NULL;
    if (atomic_load_explicit(&(state().enabled), memory_order_acquire) && request && track && cue && set_ref) {
        {
            for (unsigned range_index_4 = 0; range_index_4 < sizeof(state().slots) / sizeof(state().slots)[0]; ++range_index_4) {
                __auto_type candidate = &(state().slots[range_index_4]);

                unsigned active = ACTIVE;
                if (!atomic_compare_exchange_strong_explicit(&(candidate->phase), &(active), HELPING, memory_order_acq_rel, memory_order_acquire)) continue;
                bool match = atomic_load_explicit(&(candidate->mutated), memory_order_acquire) == 2 && metadata(candidate->request.operation) &&
                             (bool) (candidate->kind) == hot && candidate->native_serial &&
                             candidate->native_serial == u32(((uintptr_t) (request)) + 8) && !memcmp(candidate->native_track, track, 12);
                if (!match) {
                    leave_helper(candidate);
                    continue;
                }
                if (slot) {
                    atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                    atomic_store_explicit(&(candidate->invalid), 1, memory_order_release);
                    leave_helper(candidate);
                    leave_helper(slot);
                    return 1;
                }
                slot = candidate;
            }
        }
    }
    if (!slot) return original(Helper, hot ? &state().hot_original : &state().memory_original)(cache, request, track, cue, set_ref, priority);
    Slot *leave __attribute__((cleanup(leave_helper_drop))) = slot;
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&(slot->helper_claim), &(expected), 1, memory_order_acq_rel, memory_order_acquire) || !valid(slot) || !witness_current(slot) ||
        ((const unsigned char *) (cue))[0] != slot->kind || !azcue_timestamps_equal(azcue_read_timestamps(cue), slot->native_times, slot->property_rate) ||
        memcmp(slot->native_track, slot->witness.source_id, 12) ||
        (!creating(slot->request.operation) && word(((uintptr_t) (set_ref))) != (hot ? slot->witness.hot_set : slot->witness.memory_set))) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return 1;
    }
    if (creating(slot->request.operation)) {
        uintptr_t actual = word(((uintptr_t) (set_ref)));
        uintptr_t *root = hot ? &slot->witness.hot_set : &slot->witness.memory_set;
        uintptr_t *content = hot ? &slot->witness.hot_content : &slot->witness.memory_content;
        if (!(*root)) {
            // The native repository may construct the first publication root
            // after an empty-source admission. Bind only its still-empty set,
            // under this exact correlated source/request helper lease.
            uintptr_t begin = 0, end = 0;
            uint64_t count = 0;
            uintptr_t actual_content = actual ? word(actual + 0x28) : 0;
            if (!actual || !vector_bounds(actual_content, &(begin), &(end), &(count)) || count) {
                atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                return 1;
            }
            __auto_type copy = ((void (*)(void *, const void *))(state().profile.content_copy));
            copy(root, &actual);
            if (actual_content) copy(content, &actual_content);
            if ((*root) != actual || (*content) != actual_content || !witness_current(slot)) {
                atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                return 1;
            }
        } else if (actual != (*root)) {
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            return 1;
        }
    }
    uintptr_t target_content = hot ? slot->witness.hot_content : slot->witness.memory_content;
    uintptr_t target_begin, target_end;
    uint64_t target_count;
    if (!vector_bounds(target_content, &(target_begin), &(target_end), &(target_count))) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return 1;
    }
    if (creating(slot->request.operation)) {
        if (!hot && target_count >= 10) {
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            return 1;
        }
        const uint64_t absent = UINT64_C(0x7fffffffffffffff);
        for (uintptr_t row = target_begin; row != target_end; row += 0x80) {
            const unsigned kind = *((const unsigned char *) (row));
            __auto_type times = azcue_read_timestamps(((const void *) (row)));
            if (kind == 0xff || times.in_us == absent || times.in_sample == absent) continue;
            const int collision = compare_timestamps(times.in_us, slot->native_times.in_us, 1000000, slot->property_rate, 1);
            if ((hot && kind == slot->kind) || (!hot && (collision == AZ_TIME_INVALID || collision == AZ_TIME_EQUAL))) {
                atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                return 1;
            }
        }
        int result = original(Helper, hot ? &state().hot_original : &state().memory_original)(cache, request, track, cue, set_ref, priority);
        if (result == 0) atomic_store_explicit(&(slot->metadata_applied), 1, memory_order_release);
        else atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return result;
    }
    unsigned target_matches = 0;
    for (uintptr_t row = target_begin; row != target_end; row += 0x80)
        if (*((const unsigned char *) (row)) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(((const void *) (row))), slot->native_times, slot->property_rate)) ++target_matches;
    if (target_matches > 1 || (slot->request.operation != AZ_CMD_HOT_SET && target_matches != 1)) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return 1;
    }
    int result;
    struct HelperCall call = {(void *) original_address(hot ? &state().hot_original : &state().memory_original), cache, request, track, cue, set_ref, priority, 1};
    if (!az_native_try(helper_call, &call)) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return 1;
    }
    result = call.result;
    if (slot->request.operation == AZ_CMD_HOT_SET && result == 0) {
        atomic_store_explicit(&(slot->metadata_applied), 1, memory_order_release);
        return result;
    }
    if (result != 1 || !valid(slot) || !witness_current(slot)) {
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
        return result;
    }
    struct BuildCall build = {slot, hot, word(((uintptr_t) (set_ref))), target_begin, target_end, cue, false};
    if (az_native_try(build_call, &build) && build.result) return 0;
    atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
    return result;
}
static int memory_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(false, a, b, c, d, e, f); }
static int hot_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(true, a, b, c, d, e, f); }
static void request_hook(void *maintainer, const void *record) {
    Slot *slot = submitting;
    if (slot && slot->maintainer == maintainer) {
        uintptr_t input = ((uintptr_t) (record));
        atomic_store_explicit(&(slot->request_time), now(), memory_order_release);
        if (pointer(input)) {
            slot->request_source = word(input + 8 + 0x50);
            slot->request_rate = u32(input + 8 + 0x60);
        }
        unsigned reason = !pointer(input) ? 11 : ((const unsigned char *) (record))[0] != (unsigned) (!deleting(slot->request.operation))                                  ? 12
                                             : u32(input + 4) != slot->kind                                                                                                ? 13
                                             : memcmp(((const void *) (input + 8 + 0x30)), slot->property_identity, 12)                                                    ? 14
                                             : compare_timestamps(word(input + 8 + 0x40), slot->property_in, slot->property_rate, slot->property_rate, 1) != AZ_TIME_EQUAL ? 15
                                             : !valid(slot)                                                                                                                ? 16
                                                                                                                                                                           : 0;
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
static bool handler_hook(void *self) {
    if (!atomic_load_explicit(&(state().enabled), memory_order_acquire)) return original(bool (*)(void *), &state().handler_original)(self);
    uintptr_t handler = ((uintptr_t) (self));
    Slot *slot = for_handler(handler);
    if (!slot && submitting) {
        Slot *candidate = submitting;
        uintptr_t maintainer = ((uintptr_t) (candidate->maintainer));
        if (word(handler) == state().profile.handler_table && word(maintainer + 0x160) == handler) {
            uintptr_t receiver = word(handler + 0xc0);
            if (pointer(receiver) && word(receiver) == state().profile.reception_table) {
                atomic_store_explicit(&(candidate->receiver), receiver, memory_order_release);
                atomic_store_explicit(&(candidate->handler), handler, memory_order_release);
                slot = candidate;
            }
        }
    }
    if (slot) {
        atomic_store_explicit(&(slot->handler_time), now(), memory_order_release);
        atomic_store_explicit(&(slot->diagnostic), 3, memory_order_release);
        if (!valid(slot) || atomic_load_explicit(&(state().overflow), memory_order_acquire)) {
            atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
            return false; /* suppress late retries as well */
        }
        unsigned expected = 0;
        if (atomic_compare_exchange_strong_explicit(&(slot->mutated), &(expected), 1, memory_order_acq_rel, memory_order_acquire)) {
            struct MutationCall call = {slot, handler, false};
            if (!az_native_try(mutation_call, &call) || !call.result) {
                atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
                return false;
            }
        } else if (expected != 2) return false;
    }
    return original(bool (*)(void *), &state().handler_original)(self);
}
static bool capture_reply(Slot *owner, const void *memory, const void *hot) {
    __auto_type copy = ((void (*)(void *, const void *))(state().profile.content_copy));
    bool memory_present = memory && word(((uintptr_t) (memory)));
    bool hot_present = hot && word(((uintptr_t) (hot)));
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
static void reply_hook(void *self, const void *request_id, const void *track, const void *memory,
                       const void *hot, const int *result) {
    if (!atomic_load_explicit(&(state().enabled), memory_order_acquire)) {
        original(void (*)(void *, const void *, const void *, const void *, const void *, const int *), &state().reply_original)(self, request_id, track, memory, hot, result);
        return;
    }
    uintptr_t receiver = ((uintptr_t) (self));
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
    if (owner && metadata(owner->request.operation) && result && (*result == 0 || *result == 5)) {
        unsigned absent = 0;
        if (!request_id || !track || !atomic_load_explicit(&(owner->metadata_applied), memory_order_acquire) ||
            u32(((uintptr_t) (request_id)) + 8) != owner->native_serial || memcmp(track, owner->native_track, 12) ||
            !atomic_compare_exchange_strong_explicit(&(owner->reply_captured), &(absent), 1, memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&(owner->invalid), 1, memory_order_release);
        } else {
            struct ReplyCall call = {owner, memory, hot, false};
            if (!az_native_try(reply_call, &call) || !call.result) atomic_store_explicit(&(owner->invalid), 1, memory_order_release);
        }
    }
    if (owner) PERSIST_DIAG(owner->request.identity.deck, owner->request.identity.token, "reply", result ? *result : -1);
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
    if (receipt) {
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
    } else atomic_store_explicit(&(state().overflow), 1, memory_order_release);
    original(void (*)(void *, const void *, const void *, const void *, const void *, const int *), &state().reply_original)(
        self, request_id, track, memory, hot, result);
}
static void registered_hook(void *payload) {
    if (!atomic_load_explicit(&(state().enabled), memory_order_acquire)) {
        original(void (*)(void *), &state().registered_original)(payload);
        return;
    }
    uintptr_t p = ((uintptr_t) (payload));
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
    /* Actual native task disposes its active request/handler before notifying
     * our actor continuation. Merely seeing admission never completes a command. */
#if defined(AZSDK_CUE_DIAGNOSTIC)
    unsigned diagnostic_deck = receipt ? receipt->deck : 4;
    uint64_t diagnostic_token = receipt ? receipt->token : 0;
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-enter", receipt->result);
#endif
    original(void (*)(void *), &state().registered_original)(payload);
#if defined(AZSDK_CUE_DIAGNOSTIC)
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-return", receipt->result);
#endif
    if (!receipt) return;
    unsigned deck = receipt->deck;
    uint64_t token = receipt->token;
    int result = receipt->result;
    atomic_store_explicit(&(receipt->phase), FREE, memory_order_release);
    if (deck >= 4 || !token) return;
    Slot *slot = &(state().slots[deck]);
    unsigned phase = atomic_load_explicit(&(slot->phase), memory_order_acquire);
    if ((phase != ACTIVE && phase != HELPING) || slot->request.identity.token != token) return;
    atomic_store_explicit(&(slot->native_result), result, memory_order_relaxed);
    atomic_store_explicit(&(slot->receipt_seen), 1, memory_order_release);
    complete_receipt(slot);
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
    c->result = true;
}
int azsdk_cue_persist_configure(const struct azcuepersist_profile *profile, azexit_install_fn install,
                                const struct azcuepersist_callbacks *callbacks) {
    if (!profile || !callbacks || !install || !callbacks->validate || !callbacks->confirm || !callbacks->ready ||
        !profile->save || !profile->erase || !profile->request || !profile->handler_request || !profile->reception_reply ||
        !profile->registered_task || !profile->handler_table || !profile->reception_table || !profile->string_allocate ||
        !profile->string_assign || !profile->string_destroy || !profile->color_construct || !profile->memory_helper || !profile->content_copy || !profile->content_release ||
        !profile->vector_append || !profile->vector_destroy || !profile->content_from_vector || !profile->content_publish || !profile->hot_helper || !profile->hot_content_from_vector || !profile->hot_content_publish || !profile->cue_copy || !profile->cue_destroy) return 0;
    unsigned attempted = 0;
    if (!atomic_compare_exchange_strong_explicit(&(state().attempted), &(attempted), 1, memory_order_acq_rel, memory_order_acquire)) return 0;
    state().profile = *profile;
    state().callbacks = *callbacks;
    struct InstallCall call = {profile, install, false};
    if (!az_native_try(install_call, &call) || !call.result) return 0;
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
int azsdk_cue_persist_submit(const struct azexit_cue_request *request, void *manager, void *maintainer,
                             unsigned kind, const void *property, uint64_t cookie, const struct azcuepersist_witness *witness) {
    if (!request || request->size < sizeof *request || request->identity.deck >= 4 || !request->identity.token ||
        !request->identity.load_generation || !request->identity.deadline_ns || request->identity.reserved ||
        !manager || !maintainer || !property || !cookie || kind > 8) return AZ_INVALID;
    uintptr_t prop = ((uintptr_t) (property));
    const uint32_t property_rate = u32(prop + 0x60);
    if (!supported(request->operation) || !(azsdk_cue_persist_capabilities() & (UINT64_C(1) << request->operation))) return AZ_UNSUPPORTED;
    if ((request->operation == AZ_CMD_HOT_SET || request->operation == AZ_CMD_HOT_CLEAR) && (!kind || kind != request->entry_id)) return AZ_INVALID;
    if (!(request->flags & AZ_COMMAND_PERSIST) || request->rgb > 0xffffffu ||
        (request->operation == AZ_CMD_SAVED_AUTO && request->position_s != 0.0 && request->position_s != 1.0)) return AZ_INVALID;
    size_t length;
    if (!utf8(request->comment, &(length))) return AZ_INVALID;
    if (request->operation == AZ_CMD_CUE_COLOR && !kind) {
        bool supported_color = false;
        {
            unsigned range_values_8[] = {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu};
            for (unsigned range_index_8 = 0; range_index_8 < sizeof(range_values_8) / sizeof(range_values_8)[0]; ++range_index_8) {
                unsigned color = range_values_8[range_index_8];
                supported_color = supported_color || request->rgb == color;
            }
        }
        if (!supported_color) return AZ_UNSUPPORTED;
    }
    Slot *slot = &(state().slots[request->identity.deck]);
    unsigned free = FREE;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(free), WRITING, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    slot->request = *request;
    atomic_store_explicit(&(slot->helper_claim), 0, memory_order_relaxed);
    slot->native_serial = 0;
    atomic_store_explicit(&(slot->reply_captured), 0, memory_order_relaxed);
    atomic_store_explicit(&(slot->metadata_applied), 0, memory_order_relaxed);
    slot->witness = (struct azcuepersist_witness) {0};
    if (metadata(request->operation)) {
        if (!witness || !witness->source_info || !witness->fingerprint || !witness->revision || (request->operation != AZ_CMD_HOT_SET && witness->revision != request->expected_revision)) {
            atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
            return AZ_STALE;
        }
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
        if (!same || !witness_current(slot)) {
            atomic_store_explicit(&(slot->diagnostic), 40, memory_order_relaxed);
            PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-witness", AZ_STALE);
            release_witness(slot);
            atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
            return AZ_STALE;
        }
    }
    if (request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) {
        uintptr_t locator = witness ? witness->selected_cue : 0;
        uintptr_t content = kind ? slot->witness.hot_content : slot->witness.memory_content, begin, end;
        uint64_t count;
        uintptr_t selected = 0;
        if (pointer(locator) && vector_bounds(content, &(begin), &(end), &(count))) {
            for (uintptr_t row = begin; row != end; row += 0x80) {
                if (*((const unsigned char *) (row)) != kind || *((const unsigned char *) (locator)) != kind ||
                    !azcue_timestamps_equal(azcue_read_timestamps(((const void *) (row))), azcue_read_timestamps(((const void *) (locator))), property_rate)) continue;
                if (selected) {
                    selected = 0;
                    break;
                }
                selected = row;
            }
        }
        if (!selected) {
            release_witness(slot);
            atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
            return AZ_STALE;
        }
        slot->witness.selected_cue = selected;
    }
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
    uintptr_t m = ((uintptr_t) (maintainer)), q = word(m + 0x150);
    if (!valid(slot)) {
        atomic_store_explicit(&(slot->diagnostic), 41, memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-invalid", AZ_STALE);
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_STALE;
    }
    if (!pointer(q) || word(m + 0x158) || word(m + 0x160) || word(q + 0x48) != word(q + 0x68)) {
        unsigned busy = (!pointer(q) ? 1u : 0u) | (word(m + 0x158) ? 2u : 0u) | (word(m + 0x160) ? 4u : 0u) |
                        (pointer(q) && word(q + 0x48) != word(q + 0x68) ? 8u : 0u);
        atomic_store_explicit(&(slot->diagnostic), 48 + busy, memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-queue", (int) (busy));
        release_witness(slot);
        atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
        return AZ_BUSY;
    }
    memcpy(slot->property_identity, ((const void *) (prop + 0x30)), 12);
    slot->property_in = word(prop + 0x40);
    slot->property_source = word(prop + 0x50);
    slot->property_rate = property_rate;
    atomic_store_explicit(&(slot->phase), ACTIVE, memory_order_release);
    Scope scope __attribute__((cleanup(submitting_scope_drop)));
    submitting_scope_init(&scope, slot);
    struct ProducerCall call = {slot, request, maintainer, kind, property};
    if (!az_native_try(producer_call, &call)) {
        atomic_store_explicit(&(slot->diagnostic), 32, memory_order_release);
        atomic_store_explicit(&(slot->invalid), 1, memory_order_release);
    }
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
int azsdk_cue_persist_confirm_publication(const struct azexit_cue_request *request, const struct azcuepersist_witness *fresh) {
    if (!request || !fresh || request->identity.deck >= 4) return 0;
    Slot *slot = &(state().slots[request->identity.deck]);
    int result = atomic_load_explicit(&(slot->native_result), memory_order_acquire);
    if (atomic_load_explicit(&(slot->phase), memory_order_acquire) != POLLING || atomic_load_explicit(&(slot->reply_captured), memory_order_acquire) != 2 ||
        !atomic_load_explicit(&(slot->metadata_applied), memory_order_acquire) || (result != 0 && result != 5) ||
        slot->request.identity.token != request->identity.token || slot->request.identity.load_generation != request->identity.load_generation ||
        slot->request.operation != request->operation || slot->request.expected_revision != request->expected_revision ||
        slot->request.entry_id != request->entry_id || slot->request.classification != request->classification ||
        compare_timestamps(slot->request.position_s, request->position_s, 1, slot->property_rate, 1) != AZ_TIME_EQUAL ||
        compare_timestamps(slot->request.end_s, request->end_s, 1, slot->property_rate, 1) != AZ_TIME_EQUAL || !valid(slot)) return 0;
    if (!fresh->revision || fresh->source_info != slot->witness.source_info || fresh->memory_set != slot->reply_memory_set ||
        fresh->hot_set != slot->reply_hot_set || memcmp(fresh->source_id, slot->native_track, 12) ||
        !fresh->fingerprint || witness_hash(fresh) != fresh->fingerprint ||
        (fresh->memory_set ? word(fresh->memory_set + 0x28) != fresh->memory_content : (bool) (fresh->memory_content)) ||
        (fresh->hot_set ? word(fresh->hot_set + 0x28) != fresh->hot_content : (bool) (fresh->hot_content)) || !valid(slot)) return 0;
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
int azsdk_cue_persist_take(const struct azexit_cue_request *request, void *manager, struct azexit_result *out) {
    if (!request || !out || request->identity.deck >= 4 || !manager) return AZ_INVALID;
    Slot *slot = &(state().slots[request->identity.deck]);
    unsigned phase = RECEIPT;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(phase), POLLING, memory_order_acq_rel, memory_order_acquire)) return phase == FREE ? AZ_STALE : AZ_PENDING;
    if (slot->request.identity.token != request->identity.token || atomic_load_explicit(&(slot->manager), memory_order_acquire) != manager) {
        atomic_store_explicit(&(slot->phase), RECEIPT, memory_order_release);
        return AZ_STALE;
    }
    int native_result = atomic_load_explicit(&(slot->native_result), memory_order_acquire);
    bool native_success = native_result == 0 || native_result == 5;
    bool eligible = !atomic_load_explicit(&(state().overflow), memory_order_acquire) && valid(slot) && native_success;
    bool applied = false;
    uint64_t revision = 0;
    if (eligible) {
        struct ConfirmCall call = {slot, manager, &revision, false};
        applied = az_native_try(confirm_call, &call) && call.result;
        /* The repository reply can precede publication of the source-qualified
         * CueSet. Observe on fresh owner continuations until the ORIGINAL
         * deadline; never submit the native save/delete producer again. */
        eligible = !atomic_load_explicit(&(state().overflow), memory_order_acquire) && valid(slot);
        if (!applied && eligible) {
            atomic_store_explicit(&(slot->phase), RECEIPT, memory_order_release);
            return AZ_PENDING;
        }
        applied = applied && eligible;
    }
    *out = (struct azexit_result) {slot->request.identity.token, slot->request.identity.load_generation, applied ? revision : 0,
                                   applied ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, applied ? AZ_OK : AZ_FAILED};
    release_witness(slot);
    atomic_store_explicit(&(slot->phase), FREE, memory_order_release);
    return AZ_OK;
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

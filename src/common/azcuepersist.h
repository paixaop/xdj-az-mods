/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEPERSIST_H
#define AZCUEPERSIST_H
#include "azexit.h"
#define AZCUEPERSIST_NOEXCEPT
/* Exact-profile addresses supplied by the verified native profile generator.
 * All pointers are private core values. No SDK caller can supply these. */
struct azcuepersist_profile {
    uintptr_t save, erase, request, handler_request, reception_reply, registered_task;
    uintptr_t handler_table, reception_table;
    uintptr_t string_allocate, string_assign, string_destroy, color_construct;
    uintptr_t memory_helper, content_copy, content_release, vector_append, vector_destroy, content_from_vector, content_publish;
    uintptr_t hot_helper, hot_content_from_vector, hot_content_publish;
    uintptr_t cue_copy, cue_destroy;
    uintptr_t memory_notice;
};
/* Called from the actor's owned CueCallManager thread. validate only examines
 * core-owned identity/lifetime state and must work on native callback threads.
 * confirm runs on the actor owner continuation after the actual native reply task has returned; it refreshes
 * source-qualified state and returns its observed nonzero revision. False
 * confirmation retries observation until the original deadline. ready schedules
 * an actor continuation and must never synchronously reenter poll. */
/* Borrowed from a source-qualified Snapshot on its owner thread. Submit retains
 * all five native refs before returning. Helpers inspect only these held roots
 * and immutable contents; they never dereference a remembered raw Pack. */
struct azcuepersist_witness {
    uintptr_t source_info, memory_set, hot_set, memory_content, hot_content;
    uint64_t fingerprint, revision;
    unsigned char source_id[12];
    uintptr_t selected_cue; /* call-borrowed Cue80 locator; submit resolves it inside pinned content */
};
struct azcuepersist_callbacks {
    int (*validate)(const struct azexit_cue_request *, void *manager, uint64_t source_cookie, void *context);
    int (*confirm)(const struct azexit_cue_request *, void *manager, uint64_t source_cookie,
                   uint64_t *revision, void *context);
    void (*ready)(unsigned deck, uint64_t token, void *context);
    void *context;
};
__attribute__((visibility("hidden"))) int azsdk_cue_persist_configure(
    const struct azcuepersist_profile *, azexit_install_fn, const struct azcuepersist_callbacks *) AZCUEPERSIST_NOEXCEPT;
__attribute__((visibility("hidden"))) uint64_t azsdk_cue_persist_capabilities(void) AZCUEPERSIST_NOEXCEPT;
/* The caller owns manager, maintainer and the source-qualified native property
 * through this call. Native save/delete copy the property before returning.
 * Admission requires the native maintainer queue and active request to be empty;
 * AZ_BUSY guarantees no native producer was called. kind is native0 or hot1..8.
 * source_cookie is a core-owned immutable store/lifetime identity, not a pointer.
 * ready only announces a correlated native reply. Poll on the actor owner
 * continuation to validate, refresh and produce the terminal SDK result.
 * Metadata and HOT_SET require a full collection witness. HOT_SET uses its
 * internally captured revision; selector edits also require expected_revision.
 * A dispatched deadline/stale request cannot be rolled back. */
__attribute__((visibility("hidden"))) int azsdk_cue_persist_submit(
    const struct azexit_cue_request *, void *manager, void *maintainer,
    unsigned kind, const void *owned_property, uint64_t source_cookie,
    const struct azcuepersist_witness *witness) AZCUEPERSIST_NOEXCEPT;
/* During confirm on the owner continuation: 0 rejects; 1 proves exact owned
 * receipt content identity; 2 proves the correlated receipt and owned source/
 * publication roots after invalidation or reload. Both positive cases MUST be
 * paired with a full typed semantic expected-delta check by the caller. */
__attribute__((visibility("hidden"))) int azsdk_cue_persist_confirm_publication(
    const struct azexit_cue_request *, const struct azcuepersist_witness *) AZCUEPERSIST_NOEXCEPT;
__attribute__((visibility("hidden"))) int azsdk_cue_persist_poll(
    const struct azexit_cue_request *, void *live_manager, struct azexit_result *) AZCUEPERSIST_NOEXCEPT;
__attribute__((visibility("hidden"))) int azsdk_cue_persist_take(
    const struct azexit_cue_request *, void *live_manager, struct azexit_result *) AZCUEPERSIST_NOEXCEPT;
/* Retire only after the actor has closed admission. Held native refs are
 * released after the correlated receipt and any in-flight helper have quiesced. */
__attribute__((visibility("hidden"))) void azsdk_cue_persist_retire(void *manager) AZCUEPERSIST_NOEXCEPT;
#include "aztimestamp.h"
#include <string.h>
#include <stdbool.h>
/* Native Cue80 stores microseconds and sample indices at its encoded rate. */
struct azcue_native_timestamps {
    uint32_t sample_rate;
    uint64_t in_us, in_sample, out_us, out_sample;
};
static inline struct azcue_native_timestamps azcue_read_timestamps(const void *cue) {
    struct azcue_native_timestamps result;
    const unsigned char *p = (const unsigned char *) cue;
    memcpy(&result.sample_rate, p + 4, 4);
    memcpy(&result.in_us, p + 8, 8);
    memcpy(&result.in_sample, p + 16, 8);
    memcpy(&result.out_us, p + 24, 8);
    memcpy(&result.out_sample, p + 32, 8);
    return result;
}
static inline bool azcue_microseconds_equal(uint64_t first, uint64_t second, uint32_t source_rate) {
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    /* Absence markers do not represent timestamps. */
    if (first == absent || second == absent) return first == absent && second == absent;
    const int order = compare_timestamps(first, second, 1000000, source_rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static inline bool azcue_samples_equal(uint64_t first, uint64_t second, uint32_t first_rate, uint32_t second_rate, uint32_t source_rate) {
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    if (first == absent || second == absent) return first == absent && second == absent;
    if (!first_rate || !second_rate || first_rate >= 0x7fffffffu || second_rate >= 0x7fffffffu) return false;
    const int order = compare_timestamps((__uint128_t) first * second_rate, (__uint128_t) second * first_rate,
                                         (uint64_t) first_rate * second_rate, source_rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static inline bool azcue_sample_cache_equal(uint64_t first_us, uint64_t second_us, uint64_t first_sample, uint64_t second_sample,
                                            uint32_t first_rate, uint32_t second_rate, uint32_t source_rate) {
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    /* Native points can have unused, non-marker values in the OUT sample cache. */
    if (first_us == absent && second_us == absent) return true;
    return azcue_samples_equal(first_sample, second_sample, first_rate, second_rate, source_rate);
}
static inline bool azcue_timestamps_equal(struct azcue_native_timestamps a, struct azcue_native_timestamps b, uint32_t source_rate) {
    if (!source_rate || source_rate >= 0x7fffffffu) return false;
    return azcue_microseconds_equal(a.in_us, b.in_us, source_rate) && azcue_microseconds_equal(a.out_us, b.out_us, source_rate) &&
           azcue_sample_cache_equal(a.in_us, b.in_us, a.in_sample, b.in_sample, a.sample_rate, b.sample_rate, source_rate) &&
           azcue_sample_cache_equal(a.out_us, b.out_us, a.out_sample, b.out_sample, a.sample_rate, b.sample_rate, source_rate);
}
#undef AZCUEPERSIST_NOEXCEPT
#endif

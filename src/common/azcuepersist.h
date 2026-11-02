/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEPERSIST_H
#define AZCUEPERSIST_H
#include "azexit.h"
#ifdef __cplusplus
extern "C" {
#define AZCUEPERSIST_NOEXCEPT noexcept
#else
#define AZCUEPERSIST_NOEXCEPT
#endif
/* Exact-profile addresses supplied by the verified native profile generator.
 * All pointers are private core values. No SDK caller can supply these. */
struct azcuepersist_profile {
    uintptr_t save, erase, request, handler_request, reception_reply, registered_task;
    uintptr_t handler_table, reception_table;
    uintptr_t string_allocate, string_assign, string_destroy, color_construct;
    uintptr_t memory_helper, content_copy, content_release, vector_append, vector_destroy, content_from_vector, content_publish;
    uintptr_t hot_helper, hot_content_from_vector, hot_content_publish;
    uintptr_t cue_copy, cue_destroy;
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
    const struct azcuepersist_witness *witness
#ifdef __cplusplus
    = nullptr
#endif
    ) AZCUEPERSIST_NOEXCEPT;
/* During confirm on the owner continuation: 0 rejects; 1 proves exact owned
 * receipt content identity; 2 proves the correlated receipt and owned source/
 * publication roots after invalidation or reload. Both positive cases MUST be
 * paired with a full typed semantic expected-delta check by the caller. */
__attribute__((visibility("hidden"))) int azsdk_cue_persist_confirm_publication(
    const struct azexit_cue_request *, const struct azcuepersist_witness *) AZCUEPERSIST_NOEXCEPT;
__attribute__((visibility("hidden"))) int azsdk_cue_persist_poll(
    const struct azexit_cue_request *, void *live_manager, struct azexit_result *) AZCUEPERSIST_NOEXCEPT;
/* Retire only after the actor has closed admission. Held native refs are
 * released after the correlated receipt and any in-flight helper have quiesced. */
__attribute__((visibility("hidden"))) void azsdk_cue_persist_retire(void *manager) AZCUEPERSIST_NOEXCEPT;
#ifdef __cplusplus
}
#include "aztimestamp.h"
#include <cstring>
/* Native Cue80 stores cached microseconds and sample indices at its own
 * encoded rate. The observation window belongs to the actual loaded source. */
struct azcue_native_timestamps {
    uint32_t sample_rate = 0;
    uint64_t in_us = 0, in_sample = 0, out_us = 0, out_sample = 0;
};
static inline azcue_native_timestamps azcue_read_timestamps(const void *cue) noexcept {
    azcue_native_timestamps result;
    const auto *p = static_cast<const unsigned char *>(cue);
    std::memcpy(&result.sample_rate, p + 4, 4);
    std::memcpy(&result.in_us, p + 8, 8);
    std::memcpy(&result.in_sample, p + 16, 8);
    std::memcpy(&result.out_us, p + 24, 8);
    std::memcpy(&result.out_sample, p + 32, 8);
    return result;
}
static inline bool azcue_timestamps_equal(const azcue_native_timestamps &a, const azcue_native_timestamps &b, uint32_t source_rate) noexcept {
    if (!source_rate || source_rate >= 0x7fffffffu) return false;
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    auto microseconds = [&](uint64_t first, uint64_t second) {
        // Absence markers do not represent timestamps.
        if (first == absent || second == absent) return first == absent && second == absent;
        return compare_timestamps(first, second, 1000000, source_rate, 1) == AZ_TIME_EQUAL;
    };
    auto samples = [&](uint64_t first, uint64_t second) {
        if (first == absent || second == absent) return first == absent && second == absent;
        if (!a.sample_rate || !b.sample_rate || a.sample_rate >= 0x7fffffffu || b.sample_rate >= 0x7fffffffu) return false;
        // Exact common ticks explicitly convert the two encoded sample rates.
        return compare_timestamps(__uint128_t(first) * b.sample_rate, __uint128_t(second) * a.sample_rate,
            uint64_t(a.sample_rate) * b.sample_rate, source_rate, 1) == AZ_TIME_EQUAL;
    };
    return microseconds(a.in_us, b.in_us) && microseconds(a.out_us, b.out_us) &&
        samples(a.in_sample, b.in_sample) && samples(a.out_sample, b.out_sample);
}
#endif
#undef AZCUEPERSIST_NOEXCEPT
#endif

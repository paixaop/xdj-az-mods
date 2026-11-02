/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEOBSERVER_H
#define AZCUEOBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define AZSDK_CUE_NOTHROW noexcept
#else
#define AZSDK_CUE_NOTHROW
#endif
struct azsdk_cue_sample {
    uint64_t unit_generation; /* Core registration lifetime, never track/load identity. */
    uint64_t revision;        /* Core GUI-collection counter, never native cue revision. */
    uint64_t observed_ns;     /* CLOCK_MONOTONIC GUI collection time. */
    uint32_t changed_index_mask; /* Coalesced native indices0..8; no cue-kind classification. */
    uint32_t reserved;
};
/* Management context only. Unknown/disabled configuration retires all listeners.
 * The observer has bounded, never-reused process-lifetime generation slots. */
__attribute__((visibility("hidden"))) void azsdk_cue_configure(int enabled, int build_id) AZSDK_CUE_NOTHROW;
/* Call synchronously from the Unit constructor hook, immediately after native
 * construction, while the caller owns its complete unit/publisher/server and
 * has checked its exact-build unit vtable/logical deck association. This is not
 * a late subscription API. The one-time channel tag copy follows native ctor
 * initialization; guards and captured scalars acquire no liveness. Slot8 receives only a
 * core weak listener. Return1 means submitted, not append completion/replay. */
__attribute__((visibility("hidden"))) int azsdk_cue_register(int deck, void *owned_unit) AZSDK_CUE_NOTHROW;
/* Before native unit teardown/replacement. In-flight strong callback leases
 * can touch only their old core slot, never the replacement's pending mask. */
__attribute__((visibility("hidden"))) void azsdk_cue_retire(int deck) AZSDK_CUE_NOTHROW;
/* GUI collection only: one bounded claim/drain. Busy/clock/counter refusal
 * preserves pending changes. No native pointer, owner promotion or IO. */
__attribute__((visibility("hidden"))) int azsdk_cue_take(int deck, struct azsdk_cue_sample *out) AZSDK_CUE_NOTHROW;
/* Last collected receipt, bounded atomic snapshot. It does not drain pending
 * changes. Both read/take leave output unchanged on0. No source/load claim. */
__attribute__((visibility("hidden"))) int azsdk_cue_read(int deck, struct azsdk_cue_sample *out) AZSDK_CUE_NOTHROW;
__attribute__((visibility("hidden"))) unsigned azsdk_cue_capabilities(void) AZSDK_CUE_NOTHROW;
#ifdef __cplusplus
}
#endif
#undef AZSDK_CUE_NOTHROW
#endif

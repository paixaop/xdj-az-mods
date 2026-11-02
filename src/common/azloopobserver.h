/* SPDX-License-Identifier: GPL-3.0-only */
/* Private C boundary for the isolated GNU C++ loop listener. No native pointers escape. */
#ifndef AZLOOPOBSERVER_H
#define AZLOOPOBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define AZSDK_LOOP_NOTHROW noexcept
#else
#define AZSDK_LOOP_NOTHROW
#endif

struct azsdk_loop_sample {
    uint64_t unit_generation; /* Core registration lifetime, NOT a track-load generation. */
    uint64_t revision;        /* Core publication counter, NOT a native state revision. */
    uint64_t observed_ns;     /* CLOCK_MONOTONIC callback receipt, NOT source capture time. */
    int32_t engaged; /* Native current working-loop predicate; independent of playback. */
    int32_t reserved; /* Internal bounds rejection diagnostic; zero when bounds are valid. */
    int32_t bounds_valid; /* Borrowed working-property source coordinates, NOT load identity. */
    int32_t reserved2;
    double in_s;
    double out_s;
    /* Private numeric inputs, captured only after the earlier bounds guards pass.
     * A cache-consistency rejection retains them; inactive/earlier failures clear them. */
    uint64_t check_in_samples;
    uint64_t check_out_samples;
    uint64_t check_in_cached_us;
    uint64_t check_out_cached_us;
    uint32_t check_rate;
    uint32_t check_reserved;
};

/* Configure before any unit construction. Unknown builds and disabled configuration retire
 * all listeners. Registration/retirement/configuration may lock and allocate; never call
 * them from playback/draw callbacks. Build identifiers are EPB_V130=1 and EPB_V200=2. */
__attribute__((visibility("hidden"))) void azsdk_loop_configure(int enabled, int build_id) AZSDK_LOOP_NOTHROW;
/* The caller must own the complete native unit for the entire call, after its constructor
 * returned, and must have checked its existing per-build unit vtable. The chain and slot
 * guards here cannot make a stale native pointer safe. A deck must be retired before reuse.
 * 1 means registration was submitted; native async registration completion is unknown. */
__attribute__((visibility("hidden"))) int azsdk_loop_register(int deck, void *owned_unit) AZSDK_LOOP_NOTHROW;
/* Invalidate before native unit destruction. Already weak-locked callbacks stay alive but
 * cannot publish into a replacement generation. The caller owns deck-to-unit association. */
__attribute__((visibility("hidden"))) void azsdk_loop_retire(int deck) AZSDK_LOOP_NOTHROW;
/* One bounded atomic read, no waiting/IO/native access. On 0, out remains unchanged. */
__attribute__((visibility("hidden"))) int azsdk_loop_read(int deck, struct azsdk_loop_sample *out) AZSDK_LOOP_NOTHROW;
/* Bit per deck whose registration was submitted. This is not runtime certification or a
 * promise of initial replay, completeness, freshness or track/load association. */
__attribute__((visibility("hidden"))) unsigned azsdk_loop_capabilities(void) AZSDK_LOOP_NOTHROW;

#ifdef __cplusplus
}
#endif
#undef AZSDK_LOOP_NOTHROW
#endif

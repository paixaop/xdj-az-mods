/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZLOADOBSERVER_H
#define AZLOADOBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define AZSDK_LOAD_NOTHROW noexcept
#else
#define AZSDK_LOAD_NOTHROW
#endif

/* Expected native original is already stored in *original. Compare the slot to
 * that value, preserve it, then release-store hook. Decline on mismatch. A
 * partial installation must remain forwardable; never clear an original. */
typedef int (*azsdk_load_patch_fn)(uintptr_t slot, void *hook, void **original);
struct azsdk_load_sample {
    uint64_t unit_generation, load_generation, invalidation_epoch, observed_ns;
    uint32_t native_request_id, reserved;
};
/* Configure on management paths before unit construction. Both builds use the
 * same verified interface slots; IDs are EPB_V130=1 and EPB_V200=2. Installed
 * interceptors remain forward-only when disabled. Original storage and this
 * module must outlive all native calls; interception is process-lifetime. */
__attribute__((visibility("hidden"))) void azsdk_load_configure(int on, int build_id, azsdk_load_patch_fn patch) AZSDK_LOAD_NOTHROW;
/* Caller owns the complete constructed unit and its children for this call and
 * has checked its build-specific vtable. Retire before native destruction/reuse.
 * Only opaque identities are kept; no saved pointer is later dereferenced.
 * 1 means entry interceptors installed, not a committed observation/capability. */
__attribute__((visibility("hidden"))) int azsdk_load_register(int deck, void *owned_unit) AZSDK_LOAD_NOTHROW;
__attribute__((visibility("hidden"))) void azsdk_load_retire(int deck) AZSDK_LOAD_NOTHROW;
/* One bounded atomic copy. On0, out is unchanged. A successful native completion
 * matched an observed attempt; this does not qualify library/media/source data,
 * join LTI metadata, certify runtime behavior, or enable native SDK commands. */
__attribute__((visibility("hidden"))) int azsdk_load_read(int deck, struct azsdk_load_sample *out) AZSDK_LOAD_NOTHROW;

#ifdef AZSDK_LOAD_TESTING
void azsdk_load_test_hold(int deck, int held) AZSDK_LOAD_NOTHROW;
void azsdk_load_test_before_publish(void (*hook)(int)) AZSDK_LOAD_NOTHROW;
void azsdk_load_test_before_association(void (*hook)(int)) AZSDK_LOAD_NOTHROW;
void azsdk_load_test_configure_hook(void (*hook)(int, int)) AZSDK_LOAD_NOTHROW;
void azsdk_load_test_profile(int build, void *load, void *unload, void *secondary, void *complete) AZSDK_LOAD_NOTHROW;
void azsdk_load_test_exhaust(int deck, int which) AZSDK_LOAD_NOTHROW;
#endif
#ifdef __cplusplus
}
#endif
#undef AZSDK_LOAD_NOTHROW
#endif

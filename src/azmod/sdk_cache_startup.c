/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after the native installers and source startup configuration.
 * One canonical process-lifetime batch; no reset/uninstall or late retry.
 * Complete installation admits only cache capture. Source/connection/load
 * qualification remains in the separately owned C++ observer scopes.
 */
#include "sdk_cache_hooks.c"
#include <stdatomic.h>

static struct sdk_cache_hook_state g_sdk_cache_hooks;
static const uint32_t sdk_cache_startup_mask = (UINT32_C(1) << AZCE_KIND_COUNT) - 2u;

static int sdk_cache_startup_verify(void *context, const struct sdk_cache_target *target) {
    (void) context;
    const size_t length = sizeof target->expected;
    if (!g_ep_img_lo || g_ep_img_lo >= g_ep_img_hi || target->address < g_ep_img_lo ||
        target->address >= g_ep_img_hi || g_ep_img_hi - target->address < length)
        return 0;
    unsigned char actual[16];
#ifdef AZSDK_CACHE_STARTUP_COPY_BYTES
    /* Host fixture seam replaces only the raw read, never proof comparison. */
    AZSDK_CACHE_STARTUP_COPY_BYTES(actual, target->address, length);
#else
    memcpy(actual, (const void *) target->address, length);
#endif
    return memcmp(actual, target->expected, length) == 0;
}

static int sdk_cache_startup_patch(void *context, const struct sdk_cache_target *target,
                                   void *replacement, void **original) {
    (void) context;
    if (atomic_load_explicit(&g_azm_late, memory_order_acquire)) return 0;
    if (target->island_only)
        return azm_install_island_hook_at(target->address, replacement, original, "sdk-cache");
    return azm_install_hook_at(target->address, replacement, original, "sdk-cache");
}

static void sdk_cache_at_start(void) {
    if (!g_sdk_sources_on || atomic_load_explicit(&g_azm_late, memory_order_acquire)) return;
    if (!sdk_cache_hooks_install(&g_sdk_cache_hooks, g_ep147_build, sdk_cache_startup_mask,
                                 sdk_cache_startup_verify, sdk_cache_startup_patch, NULL)) return;
    if (__atomic_load_n(&g_sdk_cache_hooks.ready, __ATOMIC_ACQUIRE) == 1u &&
        __atomic_load_n(&g_sdk_cache_hooks.installed_mask, __ATOMIC_ACQUIRE) == sdk_cache_startup_mask)
        (void) azsdk_cache_capture_admit(sdk_cache_startup_mask);
}

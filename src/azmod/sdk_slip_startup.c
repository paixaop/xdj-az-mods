/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_slip_targets.h"
static int sdk_slip_patch(uintptr_t entry, void *replacement, void **original, void *context) {
    (void) context;
    if (atomic_load_explicit(&g_azm_late, memory_order_acquire)) return 0;
    return azm_install_island_hook_at(entry, replacement, original, "sdk-slip");
}
static int sdk_slip_image_range(uintptr_t address, size_t bytes) {
    return address >= g_ep_img_lo && address < g_ep_img_hi && g_ep_img_hi - address >= bytes;
}
static int sdk_slip_profile_in_image(const struct azsdk_slip_native_profile *profile) {
    for (unsigned i = 0; i < AZSLIP_TARGET_COUNT; ++i)
        if (!sdk_slip_image_range(profile->targets[i], profile->code_bytes[i])) return 0;
    return sdk_slip_image_range(profile->owner_scope, profile->owner_scope_bytes) &&
           sdk_slip_image_range(profile->registration, profile->registration_bytes);
}
static int sdk_slip_start_enabled(void) {
    return !atomic_load_explicit(&g_azm_late, memory_order_acquire) &&
           access("/home/root/settings/azmod.track-data.off", F_OK) != 0 && access("/home/root/settings/azmod.native-live.off", F_OK) != 0;
}
static void sdk_slip_at_start(void) {
    if (!sdk_slip_start_enabled()) return;
    struct azsdk_slip_native_profile profile;
    if (!sdk_slip_target_profile(g_ep147_build, &profile) || !sdk_slip_profile_in_image(&profile)) return;
    if (azsdk_slip_install(&profile, sdk_slip_patch, NULL) && !access("/home/root/settings/azmod.slip-targets.on", F_OK) &&
        sdk_slip_image_range(profile.producer, profile.producer_bytes))
        (void) azsdk_slip_getters_install(&profile, sdk_slip_patch, NULL);
}

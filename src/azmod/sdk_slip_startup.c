/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_slip_targets.h"
static int sdk_slip_patch(uintptr_t entry,void *replacement,void **original,void *context) {
    (void)context;
    if(atomic_load_explicit(&g_azm_late,memory_order_acquire))return 0;
    return azm_install_island_hook_at(entry,replacement,original,"sdk-slip");
}
static void sdk_slip_at_start(void) {
    if(atomic_load_explicit(&g_azm_late,memory_order_acquire) ||
       access("/home/root/settings/azmod.track-data.on",F_OK) ||
       access("/home/root/settings/azmod.native-live.on",F_OK))return;
    struct azsdk_slip_native_profile profile;
    if(!sdk_slip_target_profile(g_ep147_build,&profile))return;
    for(unsigned i=0;i<AZSLIP_TARGET_COUNT;i++)
        if(profile.targets[i]<g_ep_img_lo || profile.targets[i]>=g_ep_img_hi ||
           g_ep_img_hi-profile.targets[i]<profile.code_bytes[i])return;
    if(profile.owner_scope<g_ep_img_lo || profile.owner_scope>=g_ep_img_hi ||
       g_ep_img_hi-profile.owner_scope<profile.owner_scope_bytes)return;
    if(profile.registration<g_ep_img_lo || profile.registration>=g_ep_img_hi ||
       g_ep_img_hi-profile.registration<profile.registration_bytes)return;
    if(azsdk_slip_install(&profile,sdk_slip_patch,NULL) &&
       !access("/home/root/settings/azmod.slip-targets.on",F_OK) &&
       profile.producer>=g_ep_img_lo && profile.producer<g_ep_img_hi &&
       g_ep_img_hi-profile.producer>=profile.producer_bytes)
        (void)azsdk_slip_getters_install(&profile,sdk_slip_patch,NULL);
}

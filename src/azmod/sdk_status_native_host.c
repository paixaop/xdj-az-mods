/* SPDX-License-Identifier: GPL-3.0-only */
/* Host integration only. The upstream master receipt, weak BpmState registry,
 * image bounds and hook installer retain their existing single owners. */
#include "sdk_status_native_host.h"
_Static_assert((int) DS_BUILD_130 == (int) EPB_V130 && (int) DS_BUILD_200 == (int) EPB_V200, "status profile identifiers");
void az_status_host_master_cut(void (*prepare)(void), void (*consume)(const struct ds_master_state *, void *), void *user) {
    pthread_mutex_lock(&g_master_state_mu);
    if (prepare) prepare();
    struct ds_master_state state = {atomic_load_explicit(&g_master_event, memory_order_acquire),
                                    g_master_state.revision, g_master_state.observed_ns, g_master_state.deck};
    consume(&state, user);
    pthread_mutex_unlock(&g_master_state_mu);
}
int az_status_host_master_supported(void) { return master_state_supported(); }
uintptr_t az_status_host_bpm_object(unsigned deck) { return __atomic_load_n(&g_bpmstate[deck], __ATOMIC_ACQUIRE); }
uint64_t az_status_host_bpm_generation(unsigned deck) { return __atomic_load_n(&g_bpmstate_generation[deck], __ATOMIC_ACQUIRE); }
uintptr_t az_status_host_bpm_control(void) { return BPMCTRL_VT; }
int az_status_host_model_owner(unsigned deck, unsigned offset, uintptr_t object) {
    uintptr_t block = perdeck_block((int) deck);
    return block && *(const uintptr_t *) (block + offset) == object &&
           *(const uintptr_t *) (block + offset + 8) == object - 16;
}
int az_status_host_build(void) { return g_ep147_build; }
int az_status_host_image_contains(uintptr_t address, size_t bytes) {
    return address >= g_ep_img_lo && address + bytes <= g_ep_img_hi;
}
int az_status_host_image_word(uintptr_t address, uintptr_t *value) { return img_q(address, value); }
int az_status_host_install_hook(uintptr_t address, void *hook, void **original) { return azm_install_hook(address, hook, original); }
int az_status_host_install_island(uintptr_t address, void *hook, void **original, const char *name) { return azm_install_island_hook_at(address, hook, original, name); }
int az_status_host_patch_vtable(uintptr_t address, void *hook, void **original) { return azm_patch_vtable(address, hook, original); }

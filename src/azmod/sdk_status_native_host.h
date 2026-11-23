/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_NATIVE_HOST_H
#define SDK_STATUS_NATIVE_HOST_H
#include <stdint.h>
#include <stddef.h>
enum { DS_BUILD_130 = 1, DS_BUILD_200 = 2 };
struct ds_master_state { uint64_t event, revision, observed; int deck; };
/* A master cut runs prepare, copies the state, then calls consume synchronously
 * under the existing writer lock. No callback retains the stack-owned record. */
__attribute__((visibility("hidden"))) void az_status_host_master_cut(void (*prepare)(void), void (*consume)(const struct ds_master_state *, void *), void *);
__attribute__((visibility("hidden"))) int az_status_host_master_supported(void);
__attribute__((visibility("hidden"))) uintptr_t az_status_host_bpm_object(unsigned);
__attribute__((visibility("hidden"))) uint64_t az_status_host_bpm_generation(unsigned);
__attribute__((visibility("hidden"))) uintptr_t az_status_host_bpm_control(void);
__attribute__((visibility("hidden"))) int az_status_host_model_owner(unsigned, unsigned, uintptr_t);
__attribute__((visibility("hidden"))) int az_status_host_build(void);
__attribute__((visibility("hidden"))) int az_status_host_image_contains(uintptr_t, size_t);
__attribute__((visibility("hidden"))) int az_status_host_image_word(uintptr_t, uintptr_t *);
__attribute__((visibility("hidden"))) int az_status_host_install_hook(uintptr_t, void *, void **);
__attribute__((visibility("hidden"))) int az_status_host_install_island(uintptr_t, void *, void **, const char *);
__attribute__((visibility("hidden"))) int az_status_host_patch_vtable(uintptr_t, void *, void **);
#endif

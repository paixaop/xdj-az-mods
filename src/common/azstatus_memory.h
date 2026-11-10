/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_MEMORY_H
#define AZSTATUS_MEMORY_H
#include "azstatus_native.h"
#include "azexit.h"
#include "../azmod/azmod-mod.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {
    AZN_MEMORY = 65,
    AZN_CAP_MEMORY = 1u << 17,
    AZN_CAP_MEMORY_LINK = 1u << 27
};
struct azn_memory_rows {
    uint32_t size, version, availability, count;
    az_cue_entry rows[AZ_NATIVE_CUE_LIMIT];
    /* Optional private tail. Zero means an older reader supplied public rows
     * only; never reconstruct integer coordinates from those seconds. */
    uint32_t clock_version, reserved;
    struct azn_point points[AZ_NATIVE_CUE_LIMIT][2];
    int64_t stored_ms[AZ_NATIVE_CUE_LIMIT][2]; /* clock version 2 */
};
struct azn_memory_api {
    uint32_t size, version;
    /* Each taken record owns one token. Always release it, even when stale. */
    int (*read)(uint64_t token, struct azn_memory_rows *out);
    void (*release)(uint64_t token);
    int (*seed)(unsigned deck, struct azstatus_native_receipt *out);
    void (*connect)(void);
};
#ifdef AZ_STATUS_MEMORY_PRODUCER
__attribute__((visibility("hidden"))) int azn_memory_configure(int build, azexit_install_fn install);
__attribute__((visibility("hidden"))) const struct azn_file_cue_api *azn_file_service(void);
__attribute__((visibility("hidden"))) void azn_file_row_hold(uint64_t token, int acquire);
__attribute__((visibility("hidden"))) void azn_memory_bind(unsigned deck, uint64_t unit, uint64_t epoch, uint64_t load,
                                                           uintptr_t pack, const unsigned char track[12]);
__attribute__((visibility("hidden"))) void azn_memory_retire(unsigned deck, uint64_t fence);
__attribute__((visibility("hidden"))) void azn_memory_maintenance(void);
__attribute__((visibility("hidden"))) struct azn_hot_link azn_memory_copied(unsigned deck, uint64_t fence, uint64_t load,
                                                                            uintptr_t context, uintptr_t destination, uintptr_t source, uint32_t previous_revision, uint64_t *file_row);
__attribute__((visibility("hidden"))) void azn_notify(void);
__attribute__((visibility("hidden"))) const struct azn_memory_api *azn_memory_service(void);
__attribute__((visibility("hidden"))) int azn_ref_pin(uintptr_t owned);
__attribute__((visibility("hidden"))) void azn_ref_release(uintptr_t owned);
#endif
#ifdef __cplusplus
}
#endif
#endif

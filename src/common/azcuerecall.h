/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUERECALL_H
#define AZCUERECALL_H
#include "azexit.h"
#ifdef __cplusplus
extern "C" {
#define AZCUERECALL_NOEXCEPT noexcept
#else
#define AZCUERECALL_NOEXCEPT
#endif
/* Manager-owner preparation copies every owned native member. DSP dispatch is
 * admitted only on the matching unit's native task owner, inside sdk_exit scope. */
int azsdk_cue_recall_configure(int build, uintptr_t bias) AZCUERECALL_NOEXCEPT;
int azsdk_cue_recall_prepare(const struct azexit_cue_request *, void *manager,
                             const void *native_property) AZCUERECALL_NOEXCEPT;
const void *azsdk_cue_recall_property(unsigned deck, uint64_t token) AZCUERECALL_NOEXCEPT;
int azsdk_cue_recall_dispatch(unsigned deck, uint64_t token, void *core,
                              void *control_facade) AZCUERECALL_NOEXCEPT;
void azsdk_cue_recall_release(unsigned deck, uint64_t token) AZCUERECALL_NOEXCEPT;
/* Called under the manager lifecycle gate before native destruction. */
void azsdk_cue_recall_retire(void *manager) AZCUERECALL_NOEXCEPT;
#ifdef AZSDK_CUE_RECALL_TESTING
struct azcue_recall_profile {
    uintptr_t manager_table, loader_table, load_table, property_table;
    uintptr_t load_call, copy_property, destroy_property, affinity;
    uintptr_t locked_property_table, assign_property, assign_locked;
};
void azsdk_cue_recall_test_profile(const struct azcue_recall_profile *) AZCUERECALL_NOEXCEPT;
#endif
#ifdef __cplusplus
}
#endif
#undef AZCUERECALL_NOEXCEPT
#endif

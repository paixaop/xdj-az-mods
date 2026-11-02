/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEPROFILE_H
#define AZCUEPROFILE_H
#include "azcueactor.h"
#include "azcuepersist.h"
struct azcue_entry {
    uintptr_t address;
    unsigned char prologue[16];
};
struct azcue_profile {
    struct azcue_actor_profile actor;
    struct azcue_entry ctor, complete_dtor, deleting_dtor;
    struct azcue_entry store[13];
    struct azcuepersist_profile persist;
    struct azcue_entry persist_hooks[6];
    struct azcue_entry actor_entries[4];
    struct azcue_entry persist_metadata[12];
};
#ifdef __cplusplus
extern "C" {
#define AZCUE_PROFILE_NOEXCEPT noexcept
#else
#define AZCUE_PROFILE_NOEXCEPT
#endif
/* build 1 = stock 1.30, 2 = stock 2.00. Main profile validates mapped bytes. */
int azsdk_cue_profile_get(int build, uintptr_t bias, struct azcue_profile *) AZCUE_PROFILE_NOEXCEPT;
int azsdk_cue_main_profile(int build, struct azcue_profile *) AZCUE_PROFILE_NOEXCEPT;
/* Configure actor callbacks before installing lifecycle interception. */
int azsdk_cue_lifecycle_configure(int build, azexit_install_fn) AZCUE_PROFILE_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef AZCUE_PROFILE_NOEXCEPT
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUECOMMANDS_H
#define AZCUECOMMANDS_H
#include "azexit.h"
#ifdef __cplusplus
extern "C" {
#define AZCUECOMMANDS_NOEXCEPT noexcept
#else
#define AZCUECOMMANDS_NOEXCEPT
#endif
struct az_native_cue_snapshot;
int azsdk_cue_commands_configure(int on, int build, azexit_install_fn) AZCUECOMMANDS_NOEXCEPT;
uint64_t azsdk_cue_commands_commands(void) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_target_cue(unsigned, struct azexit_cue_target *) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_submit(const struct azexit_cue_request *) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_cancel(unsigned, uint64_t) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_poll(unsigned, struct azexit_result *) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_release(unsigned, uint64_t) AZCUECOMMANDS_NOEXCEPT;
void azsdk_cue_commands_tick(void) AZCUECOMMANDS_NOEXCEPT;
void azsdk_cue_commands_drive(void) AZCUECOMMANDS_NOEXCEPT;
void azsdk_cue_commands_notify(unsigned) AZCUECOMMANDS_NOEXCEPT;
int azsdk_cue_commands_native_cues(unsigned, struct az_native_cue_snapshot *) AZCUECOMMANDS_NOEXCEPT;
#ifdef AZSDK_CUE_COMMANDS_TESTING
void azsdk_cue_commands_test_time(uint64_t) AZCUECOMMANDS_NOEXCEPT;
#endif
#ifdef __cplusplus
}
#endif
#undef AZCUECOMMANDS_NOEXCEPT
#endif

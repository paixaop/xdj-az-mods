/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_COMMAND_TARGET_H
#define SDK_COMMAND_TARGET_H
#include "azmod-mod.h"
/* Bounded copies from the existing providers; no new cache or retained state. */
struct az_command_target_source {
    int (*deck_snapshot)(int, az_deck_snapshot *);
    int (*native_cues)(int, az_native_cue_snapshot *);
    int (*capabilities)(az_data_capabilities *);
};
int az_command_target_read(const struct az_command_target_source *, int deck,
                           uint64_t expected_load, uint64_t now_ns,
                           az_command_target_v1 *out);
#endif

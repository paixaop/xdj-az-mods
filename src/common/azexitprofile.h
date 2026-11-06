/* SPDX-License-Identifier: GPL-3.0-only */
/* Private native EXIT verification data shared by production and ABI fixtures. */
#ifndef AZEXITPROFILE_H
#define AZEXITPROFILE_H
#include <stdint.h>

/* Stable order of the explicit nominal types in tools/sdk-exit-profile.py. */
#define AZEXIT_COMMAND_TYPES 17u
#define AZEXIT_QUANTIZE_PENDING 15u

struct azexit_native_color { unsigned char r, g, b; };
struct azexit_native_entry {
    uintptr_t address;
    unsigned char prologue[16];
};
struct azexit_command_profile {
    uintptr_t table, destroy, deleting, execute;
};
struct azexit_profile {
    uintptr_t unit, core, facade, beat, quantize, slip, box;
    uintptr_t process, preprocess, exit, affinity, reloop, control_jump;
    unsigned char prologue[16];
    unsigned char process_prologue[16];
    struct azexit_native_entry manager_submit, quantize_submit, quantize_jump, quantize_cancel;
    uintptr_t chain_table, chain_destroy, chain_deleting;
    uintptr_t jump_table, jump_request;
    struct azexit_command_profile commands[AZEXIT_COMMAND_TYPES];
    uintptr_t set_table, set_point, clear_cue, loop_out, adjust_in, adjust_out, length_beat, length_square, cue_table;
    uintptr_t merger_table, merger_request;
    uintptr_t jump_schedule, interval_table, interval_source_table, jump_cancel;
    uintptr_t control_backcue, cue_base_table;
    struct azexit_native_entry core_jump;
    uintptr_t set_hot;
    uintptr_t play_pause_table;
    struct azexit_native_entry hot_countdown[2];
};
#endif

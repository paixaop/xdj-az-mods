/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_GUI_H
#define SDK_STATUS_GUI_H
#include <stdint.h>
void ds_gui_load(unsigned);
void ds_gui_interest(uint64_t);
void ds_master_capture(uint64_t, uint64_t, int, int);
void ds_tempo_birth(unsigned, uint64_t);
void ds_tempo_install(void);
void ds_pad_install(void);
void ds_pad_fresh(uintptr_t);
void ds_models_install(void);
#endif

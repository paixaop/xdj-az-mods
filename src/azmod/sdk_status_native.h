/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_NATIVE_H
#define SDK_STATUS_NATIVE_H
#include "../common/azstatus_native.h"
#include <stddef.h>
void ds_native_start(void);
void ds_native_interest(unsigned, uint64_t);
size_t ds_native_pending_bytes(void);
int ds_native_load_seed(unsigned, struct azstatus_native_receipt *);
#endif

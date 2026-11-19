/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_HOST_H
#define SDK_STATUS_HOST_H
#include "azmod-mod.h"
/* These narrowly scoped host callbacks carry no mod or native object pointers.
 * Binding and observer delivery happen after the reducer releases its lock. */
void az_status_host_wake(void);
void az_status_host_deadline(uint64_t);
int az_status_host_live(int);
void az_status_host_store_binding(unsigned, uint64_t, const az_track_ref *, uint64_t);
void az_status_host_store_interest(unsigned, uint64_t);
void az_status_host_native_interest(unsigned, uint64_t);
void az_status_host_gui_interest(uint64_t);
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_STORE_H
#define SDK_STATUS_STORE_H
#include "azmod-mod.h"
#include "../common/azstatus_source.h"
/* The stored status projection owns its queues, file witnesses and revisions.
 * Parsing shares the existing track worker; native entry observers retain only
 * scoped copied identities and immutable file receipts. */
void ss_native_load(unsigned, uint64_t, uint64_t);
void sdk_status_store_interest(unsigned, uint64_t);
void sdk_status_store_binding(unsigned, uint64_t, const az_track_ref *, uint64_t);
void sdk_status_store_reseed(unsigned, uint64_t, uint64_t);
int sdk_status_save_install(int, azjs_installer);
#endif

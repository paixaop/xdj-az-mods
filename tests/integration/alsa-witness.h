/* SPDX-License-Identifier: GPL-3.0-only */
/* Test-only process-lifetime counters. Not part of the MOD SDK. */
#ifndef AZSDK_ALSA_WITNESS_H
#define AZSDK_ALSA_WITNESS_H
#include <stdint.h>
#include <stdatomic.h>
enum az_alsa_api { AZ_ALSA_WRITEI, AZ_ALSA_WRITEN, AZ_ALSA_READI, AZ_ALSA_READN,
                   AZ_ALSA_AVAIL, AZ_ALSA_WAIT, AZ_ALSA_RECOVER, AZ_ALSA_APIS };
#define AZ_ALSA_MAGIC UINT64_C(0x415a414c53415731)
#define AZ_ALSA_READY ((UINT64_C(1) << AZ_ALSA_APIS) - 1)
struct az_alsa_count {
    _Atomic uint64_t entered, returned, negative, epipe;
};
struct az_alsa_witness {
    uint64_t magic, version, size, api_count;
    _Atomic uint64_t initialized, ready, unresolved, overflow;
    _Atomic uint64_t next_address[AZ_ALSA_APIS];
    struct az_alsa_count api[AZ_ALSA_APIS];
    _Atomic uint64_t recover_epipe_inputs;
};
_Static_assert(sizeof(uintptr_t) == 8 && ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
               "witness requires lock-free 64-bit scalars");
_Static_assert(sizeof(struct az_alsa_witness) == 352, "read-only witness layout");
#endif

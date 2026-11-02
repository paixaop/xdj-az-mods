/* SPDX-License-Identifier: GPL-3.0-only */
/* Internal, ordered replacement of one already-verified native virtual slot.
 * The caller owns verification/serialization and pre-publishes *orig as the
 * expected native entry. A refusal never authorizes capture in an installed hook. */
#ifndef AZSDK_VTABLE_PATCH_H
#define AZSDK_VTABLE_PATCH_H
#include <stdint.h>
#include <stddef.h>
#ifndef AZSDK_PATCH_TEST
#include "../common/azprot.h"
#endif
static int sdk_patch_slot(uintptr_t address, void *hook, void **orig) {
    if (!address || (address & (sizeof(void *) - 1)) || !hook || !orig) return -1;
    void *expected = __atomic_load_n(orig, __ATOMIC_ACQUIRE);
    if (!expected || expected == hook) return -1;
    void **slot = (void **) address;
    azprot_t protection;
    if (azprot_unlock(address, sizeof *slot, &protection)) return -1;
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != expected) {
        (void) azprot_relock(&protection);
        return -1;
    }
    /* A thread reaching the replacement must already see its original. */
    __atomic_store_n(orig, expected, __ATOMIC_RELAXED);
    __atomic_store_n(slot, hook, __ATOMIC_RELEASE);
    if (!azprot_relock(&protection)) return 1;
    /* A failed protection restoration is a refusal. Best-effort rollback keeps
     * any partially installed hook forward-only, with its original still valid.
     * Never overwrite a different replacement installed after our store. */
    azprot_t rollback;
    if (!azprot_unlock(address, sizeof *slot, &rollback)) {
        void *ours = hook;
        (void) __atomic_compare_exchange_n(slot, &ours, expected, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        rollback.prot = protection.prot;
        (void) azprot_relock(&rollback);
    }
    return -1;
}
#endif

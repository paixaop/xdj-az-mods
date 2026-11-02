/* SPDX-License-Identifier: GPL-3.0-only */
/* Execute the real installer against deterministic protection failures. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    uintptr_t base;
    size_t len;
    int prot;
} azprot_t;
static int locks, unlock_calls, relock_calls, fail_unlock, fail_relock, permissions = 1;
static int native_value, hook_value, foreign_value;
static void *slot, *original;
static int replace_during_failure, fail_rollback;
static int azprot_unlock(uintptr_t address, size_t len, azprot_t *out) {
    assert(address == (uintptr_t) &slot && len == sizeof slot && !locks);
    unlock_calls++;
    if (fail_unlock || (fail_rollback && unlock_calls > 1)) return -1;
    locks = 1;
    *out = (azprot_t) {address, len, permissions};
    permissions = 3;
    return 0;
}
static int azprot_relock(const azprot_t *saved) {
    assert(locks);
    locks = 0;
    relock_calls++;
    if (fail_relock && relock_calls == 1) {
        if (replace_during_failure) __atomic_store_n(&slot, &foreign_value, __ATOMIC_RELEASE);
        return -1;
    }
    permissions = saved->prot;
    return 0;
}
#define AZSDK_PATCH_TEST
#include "sdk_vtable_patch.h"
static void reset(void) {
    slot = original = &native_value;
    locks = unlock_calls = relock_calls = fail_unlock = fail_relock = 0;
    permissions = 1;
    replace_during_failure = fail_rollback = 0;
}
int main(void) {
    reset();
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == 1);
    assert(slot == &hook_value && original == &native_value && permissions == 1 && !locks);
    assert(unlock_calls == 1 && relock_calls == 1);
    reset();
    slot = &foreign_value;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(slot == &foreign_value && original == &native_value && permissions == 1 && !locks);
    reset();
    fail_unlock = 1;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(slot == &native_value && original == &native_value && !relock_calls && !locks);
    reset();
    fail_relock = 1;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(slot == &native_value && original == &native_value && permissions == 1 && !locks);
    assert(unlock_calls == 2 && relock_calls == 2);
    reset();
    fail_relock = replace_during_failure = 1;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(slot == &foreign_value && original == &native_value && permissions == 1 && !locks);
    reset();
    fail_relock = fail_rollback = 1;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(slot == &hook_value && original == &native_value && !locks);
    reset();
    assert(sdk_patch_slot(0, &hook_value, &original) == -1);
    assert(sdk_patch_slot((uintptr_t) &slot + 1, &hook_value, &original) == -1);
    assert(sdk_patch_slot((uintptr_t) &slot, NULL, &original) == -1);
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, NULL) == -1);
    original = NULL;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    original = &hook_value;
    assert(sdk_patch_slot((uintptr_t) &slot, &hook_value, &original) == -1);
    assert(!unlock_calls && slot == &native_value);
    puts("SDK vtable installer: expected-entry guard, permissions, rollback and foreign replacement passed");
    return 0;
}

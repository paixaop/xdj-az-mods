/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azslipobserver.h"
#include "../common/azsha256.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
void *azsdk_slip_entry_originals[AZCE_KIND_COUNT];
void azsdk_slip_invoke_original(struct azce_call *);
#define ENTRY(n) void azsdk_slip_entry_##n(void)
ENTRY(0);
ENTRY(1);
ENTRY(2);
ENTRY(3);
#undef ENTRY
static _Atomic(bool) attempted = false, ready = false;
void azsdk_slip_dispatch_call(struct azce_call *call) {
    if (atomic_load_explicit(&(ready), memory_order_acquire)) az_slip_forward(call, azsdk_slip_invoke_original);
    else if (call) azsdk_slip_invoke_original(call);
}
int azsdk_slip_ready(void) { return atomic_load_explicit(&(ready), memory_order_acquire); }
static int slip_profile_owners_valid(const struct azsdk_slip_native_profile *p) {
    if (!p->construct_return || !p->registration || !p->source_control_vtable || !p->parent_control_vtable) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (!p->parent_returns[i]) return 0;
    return 1;
}
static int slip_code_matches(uintptr_t address, size_t size, const char *expected) {
    if (size < 16 || size > 65536) return 0;
    char digest[65];
    azsha256_hex((const void *) address, size, digest);
    return !strcmp(digest, expected);
}
static int slip_targets_verified(const struct azsdk_slip_native_profile *p) {
    for (unsigned i = 0; i < AZSLIP_TARGET_COUNT; ++i) {
        if (p->targets[i] < 0x10000 || (p->targets[i] & 3) || memcmp((const void *) p->targets[i], p->expected[i], 16)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (p->targets[j] == p->targets[i]) return 0;
        if (!slip_code_matches(p->targets[i], p->code_bytes[i], p->code_sha256[i])) return 0;
    }
    return 1;
}
static int slip_profile_verified(const struct azsdk_slip_native_profile *p) {
    return slip_profile_owners_valid(p) && slip_targets_verified(p) && p->owner_scope &&
           slip_code_matches(p->owner_scope, p->owner_scope_bytes, p->owner_scope_sha256) &&
           slip_code_matches(p->registration, p->registration_bytes, p->registration_sha256);
}
static void (*const slip_entries[AZSLIP_TARGET_COUNT])(void) = {azsdk_slip_entry_0, azsdk_slip_entry_1, azsdk_slip_entry_2, azsdk_slip_entry_3};
/* Retirement must be reachable before construction or notification admission. */
static const unsigned slip_install_order[AZSLIP_TARGET_COUNT] = {2, 3, 0, 1};
static int slip_original_valid(unsigned index, unsigned installed) {
    void *original = azsdk_slip_entry_originals[index];
    uintptr_t address = (uintptr_t) original;
    if (address < 0x10000 || address > UINT64_C(0x0000ffffffffffff) || (address & 3)) return 0;
    for (unsigned j = 0; j < AZSLIP_TARGET_COUNT; ++j)
        if (original == (void *) slip_entries[j]) return 0;
    for (unsigned k = 0; k < installed; ++k)
        if (original == azsdk_slip_entry_originals[slip_install_order[k]]) return 0;
    return 1;
}
static int slip_install_entries(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    for (unsigned n = 0; n < AZSLIP_TARGET_COUNT; ++n) {
        unsigned i = slip_install_order[n];
        if (!install(p->targets[i], (void *) slip_entries[i], &azsdk_slip_entry_originals[i], context) || !azsdk_slip_entry_originals[i]) return 0;
        if (!slip_original_valid(i, n)) return 0;
    }
    return 1;
}
int azsdk_slip_install(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    if (!p || !install || atomic_exchange_explicit(&attempted, true, memory_order_seq_cst)) return 0;
    if (!slip_profile_verified(p) || !slip_install_entries(p, install, context)) return 0;
    const struct az_slip_profile profile = {p->construct_return, {p->parent_returns[0], p->parent_returns[1], p->parent_returns[2], p->parent_returns[3]}, p->source_control_vtable, p->parent_control_vtable, (void (*)(void *, const struct az_native_owner *)) p->registration};
    if (!az_slip_configure(&profile)) return 0;
    atomic_store_explicit(&ready, true, memory_order_release);
    return 1;
}

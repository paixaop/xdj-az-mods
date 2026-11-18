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
int azsdk_slip_install(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    if (!p || !install || atomic_exchange_explicit(&(attempted), true, memory_order_seq_cst)) return 0;
    if (!p->construct_return || !p->registration || !p->source_control_vtable || !p->parent_control_vtable) return 0;
    for (unsigned i = 0; i < 4; i++)
        if (!p->parent_returns[i]) return 0;
    for (unsigned i = 0; i < AZSLIP_TARGET_COUNT; i++) {
        if (p->targets[i] < 0x10000 || (p->targets[i] & 3) ||
            memcmp(((const void *) (p->targets[i])), p->expected[i], 16)) return 0;
        for (unsigned j = 0; j < i; j++)
            if (p->targets[j] == p->targets[i]) return 0;
        if (p->code_bytes[i] < 16 || p->code_bytes[i] > 65536) return 0;
        char digest[65];
        azsha256_hex(((const void *) (p->targets[i])), p->code_bytes[i], digest);
        if (strcmp(digest, p->code_sha256[i])) return 0;
    }
    if (!p->owner_scope || p->owner_scope_bytes < 16 || p->owner_scope_bytes > 65536) return 0;
    char owner_digest[65];
    azsha256_hex(((const void *) (p->owner_scope)), p->owner_scope_bytes, owner_digest);
    if (strcmp(owner_digest, p->owner_scope_sha256)) return 0;
    if (p->registration_bytes < 16 || p->registration_bytes > 65536) return 0;
    char registration_digest[65];
    azsha256_hex(((const void *) (p->registration)), p->registration_bytes, registration_digest);
    if (strcmp(registration_digest, p->registration_sha256)) return 0;
    void (*const entries[AZSLIP_TARGET_COUNT])(void) = {azsdk_slip_entry_0, azsdk_slip_entry_1, azsdk_slip_entry_2, azsdk_slip_entry_3};
    // Retirement is reachable before any construction or notification admission.
    const unsigned order[AZSLIP_TARGET_COUNT] = {2, 3, 0, 1};
    for (unsigned n = 0; n < AZSLIP_TARGET_COUNT; n++) {
        const unsigned i = order[n];
        if (!install(p->targets[i], ((void *) (entries[i])), &azsdk_slip_entry_originals[i], context) ||
            !azsdk_slip_entry_originals[i]) return 0;
        __auto_type original = ((uintptr_t) (azsdk_slip_entry_originals[i]));
        if (original < 0x10000 || original > UINT64_C(0x0000ffffffffffff) || (original & 3)) return 0;
        for (unsigned j = 0; j < AZSLIP_TARGET_COUNT; j++)
            if (azsdk_slip_entry_originals[i] == ((void *) (entries[j]))) return 0;
        for (unsigned k = 0; k < n; k++)
            if (azsdk_slip_entry_originals[i] == azsdk_slip_entry_originals[order[k]]) return 0;
    }
    const struct az_slip_profile profile = {p->construct_return, {p->parent_returns[0], p->parent_returns[1], p->parent_returns[2], p->parent_returns[3]}, p->source_control_vtable, p->parent_control_vtable, ((void (*)(void *, const struct az_native_owner *))(p->registration))};
    if (!az_slip_configure(&profile)) return 0;
    atomic_store_explicit(&(ready), true, memory_order_release);
    return 1;
}

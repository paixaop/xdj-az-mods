/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azslipobserver.h"
#include "../common/azsha256.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
uintptr_t azsdk_slip_getter_returns[3];
uint32_t azsdk_slip_getter_hooks_v1;
void azsdk_slip_getter_entry_0(void), azsdk_slip_getter_entry_1(void), azsdk_slip_getter_entry_2(void);
static int slip_getter_producer_verified(const struct azsdk_slip_native_profile *p) {
    if (!p->producer || p->producer_bytes < 16 || p->producer_bytes > 65536 || p->producer > UINTPTR_MAX - p->producer_bytes) return 0;
    char hash[65];
    azsha256_hex((const void *) p->producer, p->producer_bytes, hash);
    return !strcmp(hash, p->producer_sha256);
}
static int slip_getter_site_valid(const struct azsdk_slip_native_profile *p, unsigned i) {
    static const uint32_t expected[] = {0xf9401021, 0xf9403421, 0xf9403821};
    uintptr_t site = p->getter_sites[i];
    if ((site & 3) || site < p->producer || site > p->producer + p->producer_bytes - 8) return 0;
    uint32_t words[2];
    memcpy(words, (const void *) site, sizeof words);
    return words[0] == expected[i] && words[1] == 0xd63f0020;
}
static int slip_getter_sites_verified(const struct azsdk_slip_native_profile *p) {
    for (unsigned i = 0; i < 3; ++i) {
        if (!slip_getter_site_valid(p, i)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (p->getter_sites[i] == p->getter_sites[j]) return 0;
    }
    return 1;
}
static int slip_getter_patch_entries(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    void (*const entries[])(void) = {azsdk_slip_getter_entry_0, azsdk_slip_getter_entry_1, azsdk_slip_getter_entry_2};
    static void *originals[3];
    for (unsigned i = 0; i < 3; ++i) {
        /* Publish the continuation before exposing the island. The bridge
         * executes the displaced LDR; using a generic trampoline repeats it. */
        __atomic_store_n(&azsdk_slip_getter_returns[i], p->getter_sites[i] + 4, __ATOMIC_RELEASE);
        if (!install(p->getter_sites[i], (void *) entries[i], &originals[i], context) || !originals[i]) return 0;
        __atomic_fetch_or(&azsdk_slip_getter_hooks_v1, 1u << i, __ATOMIC_RELEASE);
    }
    return 1;
}
int azsdk_slip_getters_install(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    static _Atomic(bool) attempted = false;
    if (!p || !install || atomic_exchange_explicit(&attempted, true, memory_order_seq_cst)) return 0;
    if (!slip_getter_producer_verified(p) || !slip_getter_sites_verified(p)) return 0;
    return slip_getter_patch_entries(p, install, context);
}

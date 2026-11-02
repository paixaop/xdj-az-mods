/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azcacheentry.h"
#ifdef __cplusplus
extern "C" {
#endif

void *azsdk_cache_entry_originals[AZCE_KIND_COUNT];
static unsigned cache_capture_ready;
#define ENTRY(n) extern void azsdk_cache_entry_##n(void) __attribute__((visibility("hidden")))
ENTRY(0); ENTRY(1); ENTRY(2); ENTRY(3); ENTRY(4); ENTRY(5);
ENTRY(6); ENTRY(7); ENTRY(8); ENTRY(9); ENTRY(10);
ENTRY(11); ENTRY(12); ENTRY(13); ENTRY(14); ENTRY(15);
ENTRY(16); ENTRY(17); ENTRY(18); ENTRY(19); ENTRY(20);
ENTRY(21); ENTRY(22);
#undef ENTRY

void *azsdk_cache_entry(unsigned kind) {
    static void (*const entries[AZCE_KIND_COUNT])(void) = {
        azsdk_cache_entry_0, azsdk_cache_entry_1, azsdk_cache_entry_2,
        azsdk_cache_entry_3, azsdk_cache_entry_4, azsdk_cache_entry_5,
        azsdk_cache_entry_6, azsdk_cache_entry_7, azsdk_cache_entry_8,
        azsdk_cache_entry_9, azsdk_cache_entry_10, azsdk_cache_entry_11,
        azsdk_cache_entry_12, azsdk_cache_entry_13, azsdk_cache_entry_14,
        azsdk_cache_entry_15, azsdk_cache_entry_16, azsdk_cache_entry_17,
        azsdk_cache_entry_18, azsdk_cache_entry_19, azsdk_cache_entry_20,
        azsdk_cache_entry_21, azsdk_cache_entry_22
    };
    return kind < AZCE_KIND_COUNT ? (void *) entries[kind] : NULL;
}

int azsdk_cache_capture_ready(void) {
    return __atomic_load_n(&cache_capture_ready, __ATOMIC_ACQUIRE) == 1;
}
int azsdk_cache_capture_admit(uint32_t installed_mask) {
    /* Byte/signature verification and patch ordering belong to the canonical
     * startup installer. This final publication cannot repair a partial patch
     * and never resets originals or a previously published live observer. */
    const uint32_t full = (UINT32_C(1) << AZCE_KIND_COUNT) - 2u;
    if (installed_mask != full) return 0;
    if (azsdk_cache_capture_ready()) return 1;
    void *originals[AZCE_KIND_COUNT] = {0};
    void *entries[AZCE_KIND_COUNT] = {0};
    for (unsigned i = 0; i < AZCE_KIND_COUNT; ++i) {
        entries[i] = azsdk_cache_entry(i);
        if (!entries[i]) return 0;
        for (unsigned j = 0; j < i; ++j) if (entries[j] == entries[i]) return 0;
    }
    for (unsigned i = 1; i < AZCE_KIND_COUNT; ++i) {
        originals[i] = __atomic_load_n(&azsdk_cache_entry_originals[i], __ATOMIC_ACQUIRE);
        uintptr_t address = (uintptr_t) originals[i];
        if (address < 0x10000 || address > UINT64_C(0x0000ffffffffffff) || (address & 3u)) return 0;
        for (unsigned j = 0; j < AZCE_KIND_COUNT; ++j) if (originals[i] == entries[j]) return 0;
        for (unsigned j = 1; j < i; ++j) if (originals[j] == originals[i]) return 0;
    }
    for (unsigned i = 1; i < AZCE_KIND_COUNT; ++i)
        if (__atomic_load_n(&azsdk_cache_entry_originals[i], __ATOMIC_ACQUIRE) != originals[i]) return 0;
    __atomic_store_n(&cache_capture_ready, 1u, __ATOMIC_RELEASE);
    return 1;
}

/* A disabled observer still forwards native work. Actual ownership observation
 * is supplied by a strong dispatcher; this default advertises no capability. */
__attribute__((weak)) void azsdk_cache_dispatch_call(struct azce_call *call) {
    azsdk_cache_invoke_original(call);
}
#ifdef __cplusplus
}
#endif

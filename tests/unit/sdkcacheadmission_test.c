/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcacheentry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Transparent missing-backend control also links on Darwin: it can only
 * refuse. The real strong C implementation must replace it to pass. */
__attribute__((weak)) int azsdk_cache_capture_admit(uint32_t unused) {
    (void) unused;
    return 0;
}
__attribute__((weak)) int azsdk_cache_capture_ready() { return 0; }

volatile unsigned trap;
#define ORIGINAL(n) \
    static uint64_t __attribute__((aligned(16))) original_##n(uint64_t x) { return (n + 1) * 1000 + x; }
ORIGINAL(0)
ORIGINAL(1)
ORIGINAL(2)
ORIGINAL(3)
ORIGINAL(4)
ORIGINAL(5)
ORIGINAL(6)
ORIGINAL(7)
ORIGINAL(8)
ORIGINAL(9)
ORIGINAL(10)
ORIGINAL(11)
ORIGINAL(12)
ORIGINAL(13)
ORIGINAL(14)
ORIGINAL(15)
ORIGINAL(16)
ORIGINAL(17)
ORIGINAL(18)
ORIGINAL(19)
ORIGINAL(20)
ORIGINAL(21)
ORIGINAL(22)
#undef ORIGINAL
typedef uint64_t (*Native)(uint64_t);
Native const originals[] = {original_0, original_1, original_2, original_3, original_4, original_5, original_6,
                            original_7, original_8, original_9, original_10, original_11, original_12, original_13, original_14,
                            original_15, original_16, original_17, original_18, original_19, original_20, original_21, original_22};
bool ready() { return azsdk_cache_capture_ready() == 1; }
bool admit(uint32_t mask) { return azsdk_cache_capture_admit(mask) == 1; }

#define ENTRY(n)                                                \
    void __attribute__((aligned(16))) azsdk_cache_entry_##n() { \
        trap = n;                                               \
        abort();                                                \
    }
ENTRY(0)
ENTRY(1)
ENTRY(2)
ENTRY(3)
ENTRY(4)
ENTRY(5)
ENTRY(6)
ENTRY(7)
ENTRY(8)
ENTRY(9)
ENTRY(10)
ENTRY(11)
ENTRY(12)
ENTRY(13)
ENTRY(14)
ENTRY(15)
ENTRY(16)
ENTRY(17)
ENTRY(18)
ENTRY(19)
ENTRY(20)
ENTRY(21)
ENTRY(22)
#undef ENTRY
void azsdk_cache_invoke_original(struct azce_call *call) {
    (void) call;
    abort();
}
int main() {
    const uint32_t mask = (UINT32_C(1) << AZCE_KIND_COUNT) - 2;
    assert(!ready() && !admit(0) && !admit(mask));
    for (unsigned i = 1; i < AZCE_KIND_COUNT; ++i) azsdk_cache_entry_originals[i] = ((void *) (originals[i]));
    for (unsigned i = 1; i < AZCE_KIND_COUNT; ++i) {
        __auto_type native = ((Native) (azsdk_cache_entry_originals[i]));
        assert(native(11) == (i + 1) * 1000 + 11);
        azsdk_cache_entry_originals[i] = NULL;
        assert(!admit(mask) && !ready());
        azsdk_cache_entry_originals[i] = ((void *) (originals[i]));
    }
    assert(!admit(mask | 1) && !admit(mask ^ 2) && !ready());
    for (unsigned slot = 1; slot < AZCE_KIND_COUNT; ++slot) {
        for (unsigned other = 0; other < AZCE_KIND_COUNT; ++other) {
            azsdk_cache_entry_originals[slot] = azsdk_cache_entry(other);
            assert(!admit(mask) && !ready());
        }
        azsdk_cache_entry_originals[slot] = ((void *) (originals[slot]));
    }
    azsdk_cache_entry_originals[18] = azsdk_cache_entry_originals[9];
    assert(!admit(mask) && !ready());
    azsdk_cache_entry_originals[18] = ((void *) (originals[18]));
    azsdk_cache_entry_originals[9] = ((void *) (((uintptr_t) (originals[9])) + 1));
    assert(!admit(mask) && !ready());
    azsdk_cache_entry_originals[9] = ((void *) (originals[9]));
    puts("CACHE_CAPTURE_NATIVE_BASELINE_OK: distinct actual original slots forward before admission");
    fflush(stdout);
    assert(admit(mask) && ready() && "missing complete checked cache entry admission");
    assert(!admit(0) && ready()); // a refused later call cannot reset reachable hooks
    puts("PASS cache admission: full mask, all originals, cross-entry aliases, partials and publication");
}

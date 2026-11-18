/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/aznative_c.h"
#include <assert.h>
#include <stdio.h>
/* Only their addresses are used here. The separate TU owns their native ABI. */
extern void native_word_result(void), native_pair_result(void);
int main(void) {
    const uintptr_t sentinel = UINT64_C(0x8877665544332211);
    const void *argument = (void *) (uintptr_t) 0x12345678;
    uintptr_t words[] = {sentinel, 0, sentinel, sentinel};
    az_native_result_word((void *) native_word_result, words + 1, argument);
    assert(words[0] == sentinel && words[1] == (uintptr_t) argument && words[2] == sentinel && words[3] == sentinel);
    az_native_result_pair((void *) native_pair_result, words + 1, argument);
    assert(words[0] == sentinel && words[1] == (uintptr_t) argument && words[2] == UINT64_C(0x1122334455667788) && words[3] == sentinel);
    puts("PASS C native results: independent 8/16-byte return ABI and destination canaries");
}

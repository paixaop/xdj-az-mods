/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include <stdint.h>
struct fixture_result {
    uintptr_t words[3];
};
extern void test_make_body(void *, const void *);
struct fixture_result test_make(const void *argument) {
    struct fixture_result result = {0};
    test_make_body(&result, argument);
    return result;
}

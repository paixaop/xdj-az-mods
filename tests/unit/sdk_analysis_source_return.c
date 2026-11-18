/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include <stdint.h>
struct fixture_result {
    uintptr_t words[3];
};
extern void fixture_get_state_body(void *);
struct fixture_result fixture_get_state(const void *argument) {
    (void) argument;
    struct fixture_result result = {0};
    fixture_get_state_body(&result);
    return result;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include "../../src/common/aznative_calls.h"
extern void fixture_get_state_body(void *);
struct native_result_storage fixture_get_state(const struct az_native_track *argument) {
    (void) argument;
    struct native_result_storage result = {0};
    fixture_get_state_body(&result);
    return result;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include "../../src/common/aznative_calls.h"
extern void test_make_body(void *, const void *);
struct native_result_storage test_make(const struct az_native_phrase_atom_ref *argument) {
    struct native_result_storage result = {0};
    test_make_body(&result, argument);
    return result;
}

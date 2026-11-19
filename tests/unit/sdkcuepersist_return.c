/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include "../../src/common/aznative_calls.h"
extern void allocate_body(void *, size_t);
struct native_result_storage allocate(size_t argument) {
    struct native_result_storage result = {0};
    allocate_body(&result, argument);
    return result;
}

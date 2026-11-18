/* SPDX-License-Identifier: GPL-3.0-only */
/* C fixture entry for the indirect-result caller. The target ABI regression
 * separately uses GCC's small memory-return convention as an independent oracle. */
#include <stdint.h>
struct fixture_result {
    uintptr_t words[3];
};
extern void capture_body(void *, const void *);
struct fixture_result capture(const void *argument) {
    struct fixture_result result = {0};
    capture_body(&result, argument);
    return result;
}
extern void capture_where_body(void *, const void *);
struct fixture_result capture_where(const void *argument) {
    struct fixture_result result = {0};
    capture_where_body(&result, argument);
    return result;
}
extern void allocate_body(void *, const void *);
struct fixture_result allocate(const void *argument) {
    struct fixture_result result = {0};
    allocate_body(&result, argument);
    return result;
}

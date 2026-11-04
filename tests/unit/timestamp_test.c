/* SPDX-License-Identifier: GPL-3.0-only */
#include "aztimestamp.h"
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    assert(compare_timestamps(-1, -1, 1, 1, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(-1, UINT64_MAX, 1, 1, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(UINT64_MAX, -1, 1, 1, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(-1, -1., 1, 1, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(-2., -1., 1, 1, 1) == AZ_TIME_BEFORE);
    assert(compare_timestamps(0, 0, 1, 1, 1) == AZ_TIME_EQUAL);
    const uint64_t rates[] = {44100, 48000, 96000, 192000};
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        uint64_t rate = rates[i];
        assert(compare_timestamps(UINT64_C(62048), UINT64_C(62157), rate, rate, 256) == AZ_TIME_EQUAL);
        assert(compare_timestamps(UINT64_C(62157), UINT64_C(62048), rate, rate, 256) == AZ_TIME_EQUAL);
        assert(compare_timestamps(UINT64_C(255), UINT64_C(256), rate, rate, 256) == AZ_TIME_EQUAL);
        assert(compare_timestamps(UINT64_C(0), UINT64_C(256), rate, rate, 256) == AZ_TIME_BEFORE);
        assert(compare_timestamps(UINT64_C(256), UINT64_C(0), rate, rate, 256) == AZ_TIME_AFTER);
        assert(compare_timestamps(1., 1. + .5 / rate, 1, rate, 1) == AZ_TIME_EQUAL);
        assert(compare_timestamps(1., 1. + 1.5 / rate, 1, rate, 1) == AZ_TIME_BEFORE);
    }
    assert(compare_timestamps(UINT64_MAX, UINT64_MAX - 1, 1, 1, 2) == AZ_TIME_EQUAL);
    assert(compare_timestamps(UINT64_MAX, UINT64_MAX - 2, 1, 1, 2) == AZ_TIME_AFTER);
    assert(compare_timestamps(UINT64_MAX, UINT64_C(0), UINT64_MAX, UINT64_MAX, UINT64_MAX) == AZ_TIME_AFTER);
    assert(compare_timestamps(UINT64_C(1000000000), UINT64_C(1000020000), 1000000000, 44100, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(UINT64_C(1000000000), UINT64_C(1000030000), 1000000000, 44100, 1) == AZ_TIME_BEFORE);
    assert(compare_timestamps(0., 1. / 128, 1, 128, 1) == AZ_TIME_BEFORE);
    assert(compare_timestamps(0., INFINITY, 1, 44100, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(NAN, 0., 1, 44100, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(UINT64_C(0), UINT64_C(0), 1, 0, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(UINT64_C(0), UINT64_C(0), 0, 1, 1) == AZ_TIME_INVALID);
    assert(compare_timestamps(UINT64_C(0), UINT64_C(0), 1, 1, 0) == AZ_TIME_INVALID);
#ifdef __cplusplus
    __uint128_t wide = __uint128_t(1) << 100;
    assert(compare_timestamps(wide, wide + 1, 1, 1, 2) == AZ_TIME_EQUAL);
    assert(compare_timestamps(wide, wide + 2, 1, 1, 2) == AZ_TIME_BEFORE);
    assert(compare_timestamps(~__uint128_t(0), __uint128_t(0), 1, 2, UINT64_MAX) == AZ_TIME_AFTER);
#endif
    unsigned evaluations = 0;
    assert(compare_timestamps(++evaluations, 1u, 1, 1, 1) == AZ_TIME_EQUAL && evaluations == 1);
    puts("PASS shared timestamp comparison: sample rates, windows, symmetric order, boundaries and wide arithmetic");
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_features.c"
#include <assert.h>
#include "../../src/common/aztimestamp.h"
void test_phrase_edges_features(ta_context *c)
{
    Timing a = reference_timing(c, 120, 0.75, 1, 121);
    assert(a.bpm == 120 && compare_timestamps(a.offset, 250, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 4);
    a = reference_timing(c, 120, 0.75, 2, 121);
    assert(compare_timestamps(a.offset, 250, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 3);
    a = reference_timing(c, 120, 0.5, 0, 121);
    assert(compare_timestamps(a.offset, 500, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 1);
    a = reference_timing(c, 120, 1.0, 5, 121);
    assert(compare_timestamps(a.offset, 1000, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 1);
}

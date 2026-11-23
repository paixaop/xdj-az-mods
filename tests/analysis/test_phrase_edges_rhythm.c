/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_rhythm.c"
#include <assert.h>
void test_phrase_edges_rhythm(ta_context *c)
{
    uint8_t counts[3] = { 0 };
    int positions[] = { 10, 20, 30 };
    B result = { counts, 3 };
    I beats = { positions, 3 };
    count_hihat_beat(result, beats, 3, 9);
    assert(counts[0] == 0 && counts[1] == 0 && counts[2] == 0);
    count_hihat_beat(result, beats, 3, 10);
    count_hihat_beat(result, beats, 3, 19);
    count_hihat_beat(result, beats, 3, 20);
    count_hihat_beat(result, beats, 3, 30);
    count_hihat_beat(result, beats, 3, 300);
    assert(counts[0] == 2 && counts[1] == 1 && counts[2] == 2);
    (void)c;
}

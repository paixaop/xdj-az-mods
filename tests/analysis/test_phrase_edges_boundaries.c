/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_boundaries.c"
#include <assert.h>
void test_phrase_edges_boundaries(ta_context *c)
{
    double low[12] = { 0 };
    low[3] = 3;
    assert(recovered_break_position((D){ low, 12 }, 0) == 0);
    assert(recovered_break_position((D){ low, 12 }, 1) == 0);
    assert(recovered_break_position((D){ low, 12 }, 2) == 2);
    low[3] = 1;
    low[4] = 3;
    assert(recovered_break_position((D){ low, 12 }, 3) == 2);
    low[4] = 1;
    assert(recovered_break_position((D){ low, 12 }, 3) == 0);
    int positions[] = { 9, 25, 29 };
    Anchors a = { .positions = { positions, 3 } };
    Candidates r = initial_build(c, a, 1, 8, 8, 12);
    const int beats[] = { 1, 25, 33, 89, 97, 113 }, roles[] = { 10, 30, 40, 30, 40, 40 };
    assert(r.beats.n == 6 && r.roles.n == 6);
    assert(!memcmp(r.beats.v, beats, sizeof beats) && !memcmp(r.roles.v, roles, sizeof roles));
}

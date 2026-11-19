/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_high_boundaries.c"
#include <assert.h>
void test_phrase_edges_high_boundaries(ta_context *c)
{
    double changes[160] = { 0 };
    uint8_t flagValues[40] = { 0 };
    D ch = { changes, 160 };
    B flags = { flagValues, 40 };
    assert(!irregular_eligible(1, ch, 1, flags));
    changes[5] = 1;
    assert(irregular_eligible(6, ch, 1, flags));
    int selected = 1, second = 0;
    ChangeEvidence h = { .first = 5, .firstAligned = 9, .second = 13, .secondAligned = 17 };
    first_irregular_change(c, h, ch, 1, flags, 1, &selected, &second);
    assert(selected == 5 && second == 0);
    second_irregular_change(c, h, ch, 1, flags, 1, &selected, &second);
    assert(selected == 5 && second == 13);
    selected = 21;
    second = 0;
    first_irregular_change(c, h, ch, 1, flags, 1, &selected, &second);
    assert(selected == 21 && second == 5);
    selected = 1;
    second = 0;
    second_irregular_change(c, h, ch, 1, flags, 1, &selected, &second);
    assert(selected == 13 && second == 0);
    I seeds = irregular_seeds(c, 1, ch, 1, flags, 1, (BreakEvidence){ .start = 5, .end = 9, .deep = true }, h);
    assert(seeds.n == 3 && seeds.v[0] == 1 && seeds.v[1] == 9 && seeds.v[2] == 5);
    for (int i = 0; i < 160; i++) changes[i] = 1;
    assert(offset_irregular_seed(c, 5, ch, 1, flags) == 5);
    seeds = irregular_seeds(c, 1, ch, 1, flags, 5, (BreakEvidence){ 0 }, (ChangeEvidence){ 0 });
    assert(seeds.n == 1 && seeds.v[0] == 1);
    int positions[] = { 1, 9, 9, 17 };
    I list = { positions, 4 };
    remove_rhythm_position(&list, 9);
    assert(list.n == 2 && list.v[0] == 1 && list.v[1] == 17);
    double low[24], middle[24], all[24];
    for (int i = 0; i < 24; i++) low[i] = middle[i] = all[i] = 1;
    low[5] = 3;
    middle[5] = 2;
    all[5] = 2;
    BreakEvidence recovery = { .deep = true };
    assert(break_recovery((D){ low, 24 }, (D){ middle, 24 }, (D){ all, 24 }, 1, 6, 9, &recovery));
    assert(recovery.start == 9 && recovery.end == 13 && !recovery.deep);
    assert(!break_recovery((D){ low, 24 }, (D){ middle, 24 }, (D){ all, 24 }, 21, 6, 9, &recovery));
}

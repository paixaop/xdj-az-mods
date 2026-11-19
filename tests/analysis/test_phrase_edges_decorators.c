/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_decorators.c"
#include <assert.h>
void test_phrase_edges_decorators(ta_context *c)
{
    int roles[] = { 31, 31, 31, 40 }, saved[] = { 30, 50, 30, 40 };
    double metrics[] = { 2, 5, 3, 0 };
    I restored = restore_loops(c, (I){ roles, 4 }, (I){ saved, 4 }, (D){ metrics, 4 });
    const int expected[] = { 30, 31, 30, 40 };
    assert(!memcmp(restored.v, expected, sizeof expected));
    int counts[16];
    for (int i = 0; i < 16; i++) counts[i] = i < 8 ? 1 : i < 12 ? 3
                                                                : 6;
    RhythmGrowth g = rhythm_growth_64((I){ counts, 16 }, 0.9);
    assert(g.staged && !g.simple && g.extent == 48 && g.secondOffset == 32 && g.thirdOffset == 16);
    for (int i = 0; i < 16; i++) counts[i] = 4;
    g = rhythm_growth_64((I){ counts, 16 }, 0.9);
    assert(!g.staged && !g.simple && g.extent == 32);
    for (int i = 0; i < 16; i++) counts[i] = i < 8 ? 4 : i < 12 ? 3
                                                                : 4;
    g = rhythm_growth_64((I){ counts, 16 }, 0.9);
    assert(!g.staged && g.simple);
    double accelerations[10] = { 0 };
    accelerations[1] = 1.5;
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 0) == 21);
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 19) == 19);
    accelerations[8] = 1;
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 0) == 0);
}

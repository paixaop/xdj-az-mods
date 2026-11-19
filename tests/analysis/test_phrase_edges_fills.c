/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_fills.c"
#include <assert.h>
void test_phrase_edges_fills(ta_context *c)
{
    const int current[] = { 40, 41, 440, 441 }, intro[] = { 10, 11, 110, 111 };
    for (int i = 0; i < 4; i++) {
        int roles[] = { 10, current[i], 50 };
        remap_short_hook((I){ roles, 3 }, 1, 3);
        assert(roles[1] == intro[i]);
        int ending[] = { 50, current[i] };
        remap_short_hook((I){ ending, 2 }, 1, 2);
        assert(ending[1] == (i < 2 ? 60 : 61));
    }
    (void)c;
}

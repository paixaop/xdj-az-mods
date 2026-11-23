/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_classifier.c"
#include <assert.h>
void test_phrase_edges_classifier(ta_context *c)
{
    double ratios[15];
    for (int i = 0; i < 15; i++) ratios[i] = 0.5;
    assert(stable_tail_ratios(ratios, 0.4, 0.6));
    for (int i = 12; i < 15; i++) {
        ratios[i] = 0.7;
        assert(!stable_tail_ratios(ratios, 0.4, 0.6));
        ratios[i] = 0.3;
        assert(!stable_tail_ratios(ratios, 0.4, 0.6));
        ratios[i] = 0.5;
    }
    double low[1600], full[1600];
    for (int i = 0; i < 1600; i++) low[i] = full[i] = 1;
    int start = (1600 & ~15) - 528;
    for (int pair = 0; pair < 16; pair++)
        for (int j = 0; j < 32; j++) {
            double level = pair < 12 ? 1 : pow(0.1, pair - 11);
            full[start + pair * 32 + j] = level;
            low[start + pair * 32 + j] = level * 0.5;
        }
    assert(fade_out((D){ low, 1600 }, (D){ full, 1600 }));
    full[start + 15 * 32] = 10;
    assert(!fade_out((D){ low, 1600 }, (D){ full, 1600 }));
    (void)c;
}

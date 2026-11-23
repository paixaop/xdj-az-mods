/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/internal.h"
#include <assert.h>
void test_phrase_edges_boundaries(ta_context *c);
void test_phrase_edges_classifier(ta_context *c);
void test_phrase_edges_fills(ta_context *c);
void test_phrase_edges_rhythm(ta_context *c);
void test_phrase_edges_features(ta_context *c);
void test_phrase_edges_decorators(ta_context *c);
void test_phrase_edges_high_boundaries(ta_context *c);
int main(void)
{
    ta_context *c = calloc(1, sizeof *c);
    assert(c && !setjmp(c->failure));
    test_phrase_edges_boundaries(c);
    ta_cleanup(c);
    test_phrase_edges_classifier(c);
    ta_cleanup(c);
    test_phrase_edges_fills(c);
    ta_cleanup(c);
    test_phrase_edges_rhythm(c);
    ta_cleanup(c);
    test_phrase_edges_features(c);
    ta_cleanup(c);
    test_phrase_edges_decorators(c);
    ta_cleanup(c);
    test_phrase_edges_high_boundaries(c);
    ta_cleanup(c);
    free(c);
    puts("Phrase edge contracts passed: recovery, fade, roles, beat ownership, timing, buildup and seed votes");
}

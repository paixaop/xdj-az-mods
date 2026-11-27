/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_snare.c"
#include "phrase-model-fixture.h"

static void test_snare_pattern(const double *model)
{
    uint8_t marks[64] = { [5] = 1, [11] = 1 };
    I positions = { .n = 16 };
    B rhythm = { marks, sizeof marks };
    assert(snare_rhythm_type(positions, rhythm, model) == 1);
    assert(snare_rhythm_type(positions, (B){ marks, 63 }, model) == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SNARE_PATTERN_WORD_1] = 1;
    assert(snare_rhythm_type(positions, rhythm, changed) == 0);
    marks[11] = 0;
    marks[9] = 1;
    assert(snare_rhythm_type(positions, rhythm, changed) == 1);
}

static void test_snare_support(const double *model)
{
    uint8_t marks[12] = { [2] = 1, [8] = 1 };
    B rhythm = { marks, sizeof marks };
    assert(!snare_votes_against(rhythm, 6, model));
    marks[6] = 1;
    assert(snare_votes_against(rhythm, 6, model));
    assert(!snare_votes_against(rhythm, 5, model));
    assert(!snare_votes_against(rhythm, 7, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SNARE_SUPPORT_CENTER_MARK] = 2;
    assert(!snare_votes_against(rhythm, 6, changed));
    changed[TA_SNARE_SUPPORT_FIRST_AFTER] = 3;
    assert(snare_votes_against(rhythm, 6, changed));
}

static int collect_test_ranks(ta_context *c, SnarePoint points[5], const double *model)
{
    const Ranked input[] = {{1, 1}, {.995, 2}, {.99, 3}, {.25, 4}, {.2, 5}};
    Ranked *ranked = ta_alloc(c, 5, sizeof *ranked);
    memcpy(ranked, input, sizeof input);
    SnareWindow window = {.start = 100, .n = 5, .ranked = ranked};
    return snare_collect_points(c, &window, 1, points, 5, 0, model);
}

static void test_snare_rank_lookahead(ta_context *c, const double *model)
{
    SnarePoint points[5] = {0};
    assert(collect_test_ranks(c, points, model) == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SNARE_RANK_NEXT_NEXT] = 3;
    assert(collect_test_ranks(c, points, changed) == 2);
    assert(points[0].position == 102 && points[1].position == 103);
}

void test_phrase_edges_snare(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_snare_pattern(model);
    test_snare_support(model);
    test_snare_rank_lookahead(c, model);
}

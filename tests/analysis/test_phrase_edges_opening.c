/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_opening.c"
#include "phrase-model-fixture.h"
#include <assert.h>

static void test_score_dispatch(const double *model)
{
    const int first = (int)model[TA_OPENING_SCORE_DISPATCH_FIRST_BAR];
    assert(opening_bar_score(first, model) == model[TA_OPENING_SCORE_BAR_TWO]);
    assert(opening_bar_score(first + 1, model) == model[TA_OPENING_SCORE_BAR_THREE]);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_SCORE_MASK_TWO] = 2;
    changed[TA_OPENING_SCORE_MASK_THREE] = 1;
    assert(opening_bar_score(first, changed) == changed[TA_OPENING_SCORE_BAR_THREE]);
    assert(opening_bar_score(first + 1, changed) == changed[TA_OPENING_SCORE_BAR_TWO]);
}

static void test_majority_window(const double *model)
{
    int values[] = { (int)model[TA_OPENING_VOTE_IGNORED_BAR], 22, 22, 22 };
    I candidates = { values, 4 };
    assert(opening_majority(candidates, model) == 22 && values[0] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_VOTE_MAJORITY_LAST_SLOT] = 0;
    assert(opening_majority(candidates, changed) == 0);
}

static void test_neighbor_sources(ta_context *c, const double *model)
{
    int values[] = {5, 9, 7, 6};
    double weights[4] = {0};
    I candidates = {values, 4};
    D scores = {weights, 4};
    opening_neighbor_scores(c, candidates, scores, model);
    assert(weights[0] == model[TA_OPENING_SCORE_FIRST_NEIGHBOR]);
    assert(weights[2] == model[TA_OPENING_SCORE_NEIGHBOR] + model[TA_OPENING_SCORE_BAND_NEIGHBOR]);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_NEIGHBOR_FIRST_ORDINAL] = 2;
    changed[TA_OPENING_NEIGHBOR_MIDDLE_ORDINAL] = 1;
    memset(weights, 0, sizeof weights);
    opening_neighbor_scores(c, candidates, scores, changed);
    assert(weights[0] == 0);
    assert(weights[2] == model[TA_OPENING_SCORE_NEIGHBOR]);
}

static void test_low_tie_selection(ta_context *c, const double *model)
{
    int values[] = {0, 8, 10, 11};
    double weights[] = {0, 3, 2.75, -1};
    double ratios[12] = {0};
    I hypotheses = {values, 4};
    D scores = {weights, 4}, low_ratios = {ratios, 12};
    assert(opening_select_score(c, hypotheses, hypotheses, scores, low_ratios, model) == 10);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_TIE_UNCHANGED_COUNT] = 2;
    assert(opening_select_score(c, hypotheses, hypotheses, scores, low_ratios, changed) == 8);
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_TIE_EXCLUDED_ORDINAL] = 2;
    assert(opening_select_score(c, hypotheses, hypotheses, scores, low_ratios, changed) == 8);
}

static void test_low_prune_and_bar_conversion(ta_context *c, const double *model)
{
    int raw[] = {5, 5, 5, 0};
    I hypotheses = {raw, 4};
    double quiet[8] = {0};
    D low = {quiet, 8};
    Opening result = reconcile(c, hypotheses, low, low, low, 2, model);
    assert(result.selected == 18 && result.hypotheses.v[2] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_PRUNE_LOW_SLOT] = 1;
    changed[TA_OPENING_RESULT_PHASE_BACK] = 3;
    changed[TA_OPENING_RESULT_BEATS_PER_BAR] = 3;
    result = reconcile(c, hypotheses, low, low, low, 2, changed);
    assert(result.selected == 14 && result.hypotheses.v[1] == 0 && result.hypotheses.v[2] == 14);
}

static void test_envelope_successor(ta_context *c, const double *model)
{
    static const double bars[] = {
        .04,.04,.04,.04,.125,.25,.04,.085,.05,.05,.05,.05,.05,.05,.05,
        .3,.3,.3,.3,.3,.3,.3,.3,.3,.3,.3,1,0
    };
    double samples[sizeof bars / sizeof *bars * 4];
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++)
        samples[i] = bars[i / 4];
    assert(envelope_candidate(c, (D){ samples, sizeof samples / sizeof *samples }, 1, model) == 5);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_ENVELOPE_SUCCESSOR_DISTANCE] = 2;
    int accepted[] = { 7, 5 };
    I candidates = { accepted, sizeof accepted / sizeof *accepted };
    assert(envelope_selected_candidate(c, candidates, model) == 5);
    assert(envelope_selected_candidate(c, candidates, changed) == 7);

    double band[sizeof bars / sizeof *bars], voice[sizeof samples / sizeof *samples] = {0};
    memcpy(band, bars, sizeof band);
    D envelope = {samples, sizeof samples / sizeof *samples}, levels = {band, sizeof band / sizeof *band};
    D vocal = {voice, sizeof voice / sizeof *voice};
    Opening baseline = (opening_judge)(c, envelope, levels, levels, vocal, 1, model);
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_ENVELOPE_HYPOTHESIS_SLOT] = 1;
    changed[TA_OPENING_VOCAL_HYPOTHESIS_SLOT] = 0;
    Opening reordered = (opening_judge)(c, envelope, levels, levels, vocal, 1, changed);
    assert(baseline.raw.v[0] == 5 && baseline.raw.v[1] != baseline.raw.v[0]);
    assert(reordered.raw.v[0] == baseline.raw.v[1] && reordered.raw.v[1] == baseline.raw.v[0]);
}

static void test_envelope_terminal_range(ta_context *c, const double *model)
{
    double samples[112] = { 0 };
    int phase = 2;
    for (int i = 0; i < 26; i++) {
        double level = i == 25 ? 1 : i == 24 ? .42 : .2;
        for (int beat = 0; beat < 4; beat++)
            samples[phase - 1 + i * 4 + beat] = level;
    }
    assert(envelope_candidate(c, (D){ samples, sizeof samples / sizeof *samples }, phase, model) == 0);
}

static void test_envelope_normalization(ta_context *c, const double *model)
{
    double samples[] = {10, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    D input = {samples, sizeof samples / sizeof *samples};
    D baseline = envelope_levels(c, input, 1, model);
    assert(baseline.v[0] == 1 && baseline.v[1] == 0.5);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_ENVELOPE_PHASE_ORIGIN] = 0;
    changed[TA_OPENING_ENVELOPE_NORMALIZE_NUMERATOR] = 2;
    D shifted = envelope_levels(c, input, 1, changed);
    assert(shifted.v[0] == 2 && shifted.v[1] == 2);

    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_ENVELOPE_NORMALIZE_FALLBACK_TEST] = 0.25;
    D fallback = envelope_levels(c, input, 1, changed);
    assert(fallback.v[0] == 4 * model[TA_OPENING_ENVELOPE_NORMALIZE_ZERO]);
}

static void test_vocal_sustained_mean(const double *model)
{
    double levels[112] = {0};
    D input = {levels, sizeof levels / sizeof *levels};
    int start = (int)model[TA_VOICE_SUSTAINED_START];
    for (int i = start; i < start + 8; ++i)
        levels[i] = 0.3;
    assert(vocal_sustained_onset(input, start, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_VOICE_SUSTAINED_MEAN_RECIPROCAL] *= 0.5;
    assert(!vocal_sustained_onset(input, start, changed));
    memcpy(changed, model, sizeof changed);
    changed[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE] = 0.31;
    assert(!vocal_sustained_onset(input, start, changed));
    memcpy(changed, model, sizeof changed);
    changed[TA_VOICE_SUSTAINED_PATTERN_PRODUCT_MIN_EXCLUSIVE] = 1;
    assert(!vocal_sustained_onset(input, start, changed));
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_VOICE_SUSTAINED_QUIET_FROM] = start;
    assert(vocal_first_onset(input, changed) == start + 1);
    changed[TA_VOICE_SUSTAINED_QUIET_SUM] = 0.1;
    assert(vocal_first_onset(input, changed) == 0);
}

static void test_vocal_bar_mean_rounding(const double *model)
{
    double values[112] = {
        [4] = .4, [8] = .375, [9] = .375, [10] = .375,
        [12] = .375, [13] = .375, [14] = .375, [16] = .85, [17] = .85,
    };
    values[18] = nextafter(.85, 1);
    D input = { values, sizeof values / sizeof *values };
    double total = sum(input, 8, 19, model);
    assert(total / 12 > model[TA_OPENING_VOICE_BAR_DENSE_MEAN]);
    assert(total * model[TA_VOICE_BAR_MEAN_RECIPROCAL] == model[TA_OPENING_VOICE_BAR_DENSE_MEAN]);
    assert(!vocal_dense_bars(input, 8, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_VOICE_BAR_MEAN_RECIPROCAL] = nextafter(changed[TA_VOICE_BAR_MEAN_RECIPROCAL], 1);
    assert(vocal_dense_bars(input, 8, changed));
}

static void test_vocal_sparse_mean_rounding(const double *model)
{
    double values[112] = { [7] = .8, [8] = .8, [9] = .8 };
    D input = { values, sizeof values / sizeof *values };
    double total = sum(input, 7, 30, model);
    assert(total * model[TA_VOICE_SPARSE_MEAN_RECIPROCAL] == model[TA_OPENING_VOICE_SPARSE_WINDOW_MEAN]);
    assert(vocal_sparse_onset(input, model) == 8);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_VOICE_SPARSE_WINDOW_MEAN] = total / 24;
    assert(changed[TA_OPENING_VOICE_SPARSE_WINDOW_MEAN] > model[TA_OPENING_VOICE_SPARSE_WINDOW_MEAN]);
    assert(vocal_sparse_onset(input, changed) == 0);
}

static void test_settling_at_last_candidate(ta_context *c, const double *model)
{
    double values[] = { 1, 1, 1, 1.2, 1, 1, 1 };
    double ratios[] = { 1, 1, 1, 1, 1, 1, 1 };
    double contrast[] = { 1, 1, 1, 1, 1, 1, 1 };
    D levels = { values, 7 }, relative = { ratios, 7 }, settled = { contrast, 7 };
    assert(level_candidate_accepted(c, levels, relative, settled, 4, true, model));
    settled.n--;
    assert(!level_candidate_accepted(c, levels, relative, settled, 4, true, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_FALLBACK_LOW_SCALE] *= .5;
    assert(level_candidate_accepted(c, levels, relative, settled, 4, true, changed));
    assert(!level_candidate_accepted(c, levels, relative, settled, 4, false, changed));
}

static void test_quiet_low_scan(const double *model)
{
    double values[18] = { 0 }, contrast[18];
    for (int i = 0; i < 18; i++) contrast[i] = 2;
    D levels = { values, 18 }, ratios = { contrast, 18 };
    opening_quiet_low(levels, ratios, model);
    assert(contrast[0] == 2 && contrast[15] == 1 && contrast[16] == 1 && contrast[17] == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_QUIET_LAST_BAR] = 15;
    changed[TA_OPENING_QUIET_RATIO_REPLACEMENT] = .9;
    opening_quiet_low(levels, ratios, changed);
    assert(contrast[15] == .9 && contrast[16] == 1);
}

static void test_opening_level_neighbors(const double *model)
{
    double contrasts[] = {0, 0, 5, 1, 1, 1, 1, 3, 2, 3};
    D levels = {contrasts, sizeof contrasts / sizeof *contrasts};
    assert(!level_prefer_first(levels, 4, 6, model));
    assert(isnan(level_neighbor_sum(levels, 1, model)));
    assert(isnan(level_neighbor_sum(levels, 9, model)));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_LEVEL_NEIGHBOR_PREVIOUS_BACK] = 0;
    assert(level_prefer_first(levels, 4, 6, changed));

    int ranks[] = {4, 6, 8};
    I candidates = {ranks, 3};
    level_order_candidates(candidates, levels, model);
    assert(ranks[0] == 8 && ranks[1] == 6 && ranks[2] == 4);
    ranks[0] = 4;
    ranks[2] = 8;
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_LEVEL_RANK_FIRST_SLOT] = 2;
    changed[TA_OPENING_LEVEL_RANK_THIRD_SLOT] = 0;
    level_order_candidates(candidates, levels, changed);
    assert(ranks[0] == 4 && ranks[1] == 6 && ranks[2] == 8);
}

static void test_opening_history_window(ta_context *c, const double *model)
{
    double levels[] = {4, 1, 1, 2, 1, 1};
    double ratios[] = {1, 0, 1, 1, 1, 1};
    double contrast[6] = {0};
    D band = {levels, 6}, relative = {ratios, 6}, change = {contrast, 6};
    assert(level_changed(c, band, relative, change, 4, true, model));

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_OPENING_HISTORY_RATIO_INDEX] = 0;
    assert(!level_changed(c, band, relative, change, 4, true, parameters));
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_OPENING_HISTORY_END_BACK] = 2;
    assert(!level_changed(c, band, relative, change, 4, true, parameters));
}

static void test_anchor_phase_fallback(const double *model)
{
    int anchor[] = {6};
    int values[] = {8, 5, 1};
    I anchors = {anchor, 1}, ordered = {values, 3};
    Opening result = {0};
    opening_anchor_phase(&result, anchors, ordered, 1, model);
    assert(result.found && result.selected == 5);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_ANCHOR_PHASE_TAIL_SKIP] = 2;
    result = (Opening){0};
    opening_anchor_phase(&result, anchors, ordered, 1, changed);
    assert(result.found && result.selected == 8);

    anchor[0] = -13;
    values[0] = -1;
    values[1] = -8;
    values[2] = -11;
    result = (Opening){0};
    opening_anchor_phase(&result, anchors, ordered, 4, model);
    assert(result.found && result.selected == -8);
}

void test_phrase_edges_opening(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_score_dispatch(model);
    test_majority_window(model);
    test_neighbor_sources(c, model);
    test_low_tie_selection(c, model);
    test_low_prune_and_bar_conversion(c, model);
    test_envelope_successor(c, model);
    test_envelope_terminal_range(c, model);
    test_envelope_normalization(c, model);
    test_vocal_sustained_mean(model);
    test_vocal_bar_mean_rounding(model);
    test_vocal_sparse_mean_rounding(model);
    test_settling_at_last_candidate(c, model);
    test_quiet_low_scan(model);
    test_opening_level_neighbors(model);
    test_opening_history_window(c, model);
    test_anchor_phase_fallback(model);
}

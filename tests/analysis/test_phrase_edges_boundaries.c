/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_boundaries.c"
#include "phrase-model-fixture.h"
#include <assert.h>

static void test_noise_model_parameters(ta_context *c)
{
    double levels[24];
    for (int i = 0; i < 24; i++)
        levels[i] = i < 4 ? 1 : 2;
    D input = { levels, 24 };

    const double *model = fixture_phrase_parameters();
    assert(noise_changes(c, input, model).v[0] == 2);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_NOISE_CHANGE_RATIO] = 3;
    assert(noise_changes(c, input, changed).v[0] == 0);
    changed[TA_NOISE_CHANGE_RATIO] = model[TA_NOISE_CHANGE_RATIO];
    changed[TA_NOISE_RECOVERY_RATIO] = 0.5;
    assert(noise_changes(c, input, changed).v[0] == 0);
    changed[TA_NOISE_RECOVERY_RATIO] = model[TA_NOISE_RECOVERY_RATIO];
    changed[TA_NOISE_SCAN_MIN_INPUT] = input.n + 1;
    assert(noise_changes(c, input, changed).v[0] == 0);
    changed[TA_NOISE_SCAN_MIN_INPUT] = model[TA_NOISE_SCAN_MIN_INPUT];
    changed[TA_NOISE_POSITION_ADVANCE]++;
    assert(noise_changes(c, input, changed).v[0] == model[TA_NOISE_POSITION_ADVANCE] + 1);
}

static void test_boundary_ratio_extent(ta_context *c)
{
    double levels[] = { 2, 4, 0, 8, 16 };
    D input = { levels, sizeof levels / sizeof *levels };
    const double *model = fixture_phrase_parameters();
    RatioSeries original = ratio_series(c, input, model);
    assert(original.levels.v[3] == .5 && original.levels.v[4] == 16);
    assert(original.levels.v[2] == model[TA_BOUNDARY_ZERO_RATIO_LEVEL]);
    assert(original.direction.v[3] > 0 && original.direction.v[4] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BOUNDARY_NORMALIZATION_TAIL]++;
    changed[TA_BOUNDARY_RATIO_OMITTED]++;
    RatioSeries adjusted = ratio_series(c, input, changed);
    assert(adjusted.levels.v[3] == 8 && adjusted.direction.v[3] == 0);
}

static void test_local_boundary_history_floor(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double levels[12] = { [4] = .02 }, contrast[12] = { [4] = 1.2 };
    RatioSeries series = { .levels = { levels, 12 }, .contrast = { contrast, 12 } };
    assert(local_peak_survives(c, series, 2, 1, 1, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BOUNDARY_HISTORY_LEVEL_ZERO] = levels[4];
    assert(!local_peak_survives(c, series, 2, 1, 1, changed));
}

static void test_local_boundary_zero_trim(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    int count = (int)model[TA_BOUNDARY_LOCAL_ZERO_TRIM_LIMIT];
    I samples = ta_new_ints(c, count + 2);
    samples.v[count] = 7;
    samples.v[count + 1] = 9;
    I result = finish_local_candidates(c, samples, model);
    assert(result.n == 2 && result.v[0] == 7 && result.v[1] == 9);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BOUNDARY_LOCAL_ZERO_TRIM_LIMIT]--;
    result = finish_local_candidates(c, samples, changed);
    assert(result.n == model[TA_BOUNDARY_LOCAL_EMPTY_COUNT] && result.v[0] == 0);
}

static void test_initial_alternative_parameters(ta_context *c)
{
    int positions[] = { 9, 25, 29 };
    Anchors a = { .positions = { positions, 3 } };
    const double *parameters = fixture_phrase_parameters();
    Alternatives alternatives = initial_alternatives(c, a, 1, parameters);
    const int short_beats[] = { 1, 33, 65, 97, 113 };
    assert(alternatives.shortList.beats.n == 5);
    assert(!memcmp(alternatives.shortList.beats.v, short_beats, sizeof short_beats));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, parameters, sizeof changed);
    changed[TA_INITIAL_SHORT_PHASE_SPAN] = 12;
    changed[TA_INITIAL_SHORT_COARSE_MIN] = 8;
    changed[TA_INITIAL_SHORT_GAP_MIN] = 12;
    Candidates r = initial_alternatives(c, a, 1, changed).shortList;
    const int beats[] = { 1, 25, 33, 89, 97, 113 }, roles[] = { 10, 30, 40, 30, 40, 40 };
    assert(r.beats.n == 6 && r.roles.n == 6);
    assert(!memcmp(r.beats.v, beats, sizeof beats) && !memcmp(r.roles.v, roles, sizeof roles));
}

static void test_hook_prune_parameters(void)
{
    int candidates[] = { 1, 8, 10 };
    int original[] = { 1, 8, 10 };
    double scores[] = { 0, 2.1, 1.2 };
    I positions = { candidates, 3 }, before = { original, 3 };
    D strengths = { scores, 3 };
    const double *parameters = fixture_phrase_parameters();
    prune_close_hooks(positions, before, strengths, 6, parameters);
    assert(candidates[2] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, parameters, sizeof changed);
    changed[TA_HOOK_PRUNE_NEAR_MAX] = 1;
    candidates[2] = 10;
    prune_close_hooks(positions, before, strengths, 6, changed);
    assert(candidates[2] == 10);
}

static void test_short_break_scan_extent(const double *parameters)
{
    double levels[24] = { [13] = .6, [14] = .1, [15] = .8, [17] = .6, [18] = .1, [19] = .6 };
    double middle[24] = { [12] = .3, [16] = .3 };
    double ratios[24];
    for (size_t i = 0; i < sizeof ratios / sizeof *ratios; i++)
        ratios[i] = 1;
    ratios[14] = ratios[18] = .3;
    D low = { levels, 24 }, mid = { middle, 24 }, ratio = { ratios, 24 };
    int bars[24];
    I result = { bars, 0 };
    collect_short_breaks(low, mid, ratio, ratio, .4, .4, 22, &result, parameters);
    assert(result.n == 0);
    collect_short_breaks(low, mid, ratio, ratio, .4, .4, 23, &result, parameters);
    assert(result.n == 1 && result.v[0] == 6);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, parameters, sizeof changed);
    changed[TA_SHORT_BREAK_SCAN_FIRST] = 14;
    changed[TA_SHORT_BREAK_PRECEDING_FIRST_BASE] = 13;
    changed[TA_SHORT_BREAK_SCAN_RESULT_BIAS] = 15;
    result.n = 0;
    collect_short_breaks(low, mid, ratio, ratio, .4, .4, 23, &result, changed);
    assert(result.n == 2 && result.v[0] == 5 && result.v[1] == 6);
}

void test_phrase_edges_boundaries(ta_context *c)
{
    test_noise_model_parameters(c);
    test_boundary_ratio_extent(c);
    test_local_boundary_history_floor(c);
    test_local_boundary_zero_trim(c);
    test_initial_alternative_parameters(c);
    test_hook_prune_parameters();
    double low[12] = { 0 };
    const double *parameters = fixture_phrase_parameters();
    test_short_break_scan_extent(parameters);
    low[3] = 3;
    assert(recovered_break_position((D){ low, 12 }, 1, parameters) == 0);
    assert(recovered_break_position((D){ low, 12 }, 2, parameters) == 0);
    assert(recovered_break_position((D){ low, 12 }, 3, parameters) == 2);
    low[3] = 1;
    low[4] = 3;
    assert(recovered_break_position((D){ low, 12 }, 4, parameters) == 2);
    low[4] = 1;
    assert(recovered_break_position((D){ low, 12 }, 4, parameters) == 0);
}

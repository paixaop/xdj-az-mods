/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_fills.c"
#include <assert.h>
#include "phrase-model-fixture.h"

static void test_build_model_parameter(ta_context *c)
{
    int boundaries[] = { 1, 33, 65, 97, 129, 161 };
    int roles[] = { 10, 32, 40, 20, 60 };
    double metric[] = { 0, 0.5, 0, 0, 0 };
    const double *model = fixture_phrase_parameters();
    Fills fills = { .roles = { roles, 5 }, .starts = ta_new_ints(c, 5) };
    (final_adjust)(c, (I){ boundaries, 6 }, true, (D){ metric, 5 }, &fills, model);
    assert(roles[1] == 30);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FINAL_STRONG_BUILD_LEVEL] = 0.75;
    roles[1] = 32;
    (final_adjust)(c, (I){ boundaries, 6 }, true, (D){ metric, 5 }, &fills, changed);
    assert(roles[1] == 32);
}

static void test_intro_fill_role(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    int boundaries[] = { 1, 33, 40 };
    int input_roles[] = { 10, 40 }, output_roles[] = { 10, 40 };
    int starts[2] = { 0 }, routes[2] = { 0 };
    Fills output = { .roles = { output_roles, 2 }, .starts = { starts, 2 }, .routes = { routes, 2 } };
    double levels[40] = { [16] = 1, [17] = 1, [18] = 1, [19] = 1 };
    FillProfile profile = { .low = { levels, 40 } };
    I sections = { boundaries, 3 }, roles = { input_roles, 2 };
    I no_repeated_scores = { 0 };
    attach_intro_fills(c, &output, sections, roles, profile, FILL_INTRO, no_repeated_scores, model);
    assert(output_roles[0] == 110 && starts[0] == 21 && routes[0] == FILL_CUT);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FILL_INTRO_DECORATED_ROLE] = 111;
    changed[TA_FILL_CUT_COARSE_OFFSET_0] = 5;
    attach_intro_fills(c, &output, sections, roles, profile, FILL_INTRO, no_repeated_scores, changed);
    assert(output_roles[0] == 111 && starts[0] == 22 && routes[0] == FILL_CUT);
}

static void test_fill_growth_marker(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double magnitudes[33] = {
        [16] = 1, [17] = 1, [18] = 1, [19] = 1,
        [24] = 1, [25] = 1, [26] = 1, [27] = 1,
    };
    D low = { magnitudes, 33 };
    FillDetection fill = (fill_growth)(c, 1, 33, low, model);
    assert(fill.start == 25 && fill.end == 32 && fill.route == FILL_GROWTH);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FILL_GROWTH_MIDDLE_CANDIDATE_ADVANCE] = 9;
    fill = (fill_growth)(c, 1, 33, low, changed);
    assert(fill.start == 26 && fill.end == 32 && fill.route == FILL_GROWTH);
}

static void test_high_band_tail(ta_context *c)
{
    double samples[64];
    for (int i = 0; i < 64; i++) samples[i] = i + 1;
    D input = { samples, 64 };
    const double *model = fixture_phrase_parameters();
    HighProfile profile = (normalize_high)(c, input, 2, model);
    assert(profile.beats == 7 && profile.values.v[0] == 9.0 / 64);
    assert(profile.values.v[31] == 40.0 / 64 && profile.values.v[32] == 33);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIGH_BAND_NORMALIZE_TAIL]++;
    profile = (normalize_high)(c, input, 2, changed);
    assert(profile.values.v[31] == 32);
}

static void test_snare_stability_window(void)
{
    double levels[] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    double ratios[] = { 1, 1.8, .8, 1, 1, 1, 1, 1 };
    int selected[8] = { 0 };
    I output = { selected, 8 };
    D bars = { levels, 8 }, ratios_by_bar = { ratios, 8 };
    const double *model = fixture_phrase_parameters();
    select_snares(output, bars, ratios_by_bar, 8, 1.2, .7, model);
    assert(selected[0] == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SNARE_SCAN_STABLE_AHEAD] = 1;
    selected[0] = 0;
    select_snares(output, bars, ratios_by_bar, 8, 1.2, .7, changed);
    assert(selected[0] == 0);
}

static void test_fill_cleanup_role(void)
{
    const double *model = fixture_phrase_parameters();
    int roles[] = { 440, 331, 111 }, starts[] = { 8, 8, 8 };
    Fills fills = { .roles = { roles, 3 }, .starts = { starts, 3 } };
    for (int i = 0; i < 3; i++) remove_fill_decoration(&fills, i, model);
    assert(roles[0] == 40 && roles[1] == 31 && roles[2] == 11);
    assert(starts[0] == 0 && starts[1] == 0 && starts[2] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_CLEAN_FILL_HOOK_DECORATION_OFFSET] = 399;
    roles[0] = 440;
    remove_fill_decoration(&fills, 0, changed);
    assert(roles[0] == 41);
}

static void test_final_hook_span(ta_context *c)
{
    int positions[] = { 1, 17, 25, 33 }, roles[] = { 10, 40, 50 };
    double metric[3] = { 0 };
    I boundaries = { positions, 4 };
    D metrics = { metric, 3 };
    Fills fills = { .roles = { roles, 3 } };
    const double *model = fixture_phrase_parameters();
    (final_adjust)(c, boundaries, false, metrics, &fills, model);
    assert(roles[1] == 10);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FINAL_SHORT_HOOK_SPAN_MAX]--;
    roles[1] = 40;
    (final_adjust)(c, boundaries, false, metrics, &fills, changed);
    assert(roles[1] == 40);
}

static void test_final_fill_trim(ta_context *c)
{
    int positions[] = { 1, 17, 25, 33 }, roles[] = { 10, 31, 60 };
    int starts[] = { 5, 0, 0 };
    double metric[3] = { 0 };
    I boundaries = { positions, 4 };
    D metrics = { metric, 3 };
    Fills fills = { .roles = { roles, 3 }, .starts = { starts, 3 } };
    const double *model = fixture_phrase_parameters();
    (final_adjust)(c, boundaries, true, metrics, &fills, model);
    assert(starts[0] == 13);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FINAL_FILL_TRIM_BACK] = 3;
    starts[0] = 5;
    (final_adjust)(c, boundaries, true, metrics, &fills, changed);
    assert(starts[0] == 14);
}

static void test_repeated_peak_offsets(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double samples[64] = { [18] = 1, [20] = 1, [22] = 1 };
    HighProfile high = { { samples, 64 }, 20 };
    I scores = (repeated_scores)(c, high, model);
    assert(scores.n == 5 && scores.v[0] == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_REPEATED_PEAK_WINDOW_OFFSET] = 20;
    scores = (repeated_scores)(c, high, changed);
    assert(scores.v[0] == 0);

    int history[] = { 0, 2, 2, 0, 0 };
    scores = (I){ history, 5 };
    assert((repeated_upper)(13, 1, scores, model).start == 0);
    changed[TA_REPEATED_UPPER_MAX_QUIET_SCORE] = 2;
    assert((repeated_upper)(13, 1, scores, changed).start == 11);
}

void test_phrase_edges_fills(ta_context *c)
{
    test_build_model_parameter(c);
    test_intro_fill_role(c);
    test_fill_growth_marker(c);
    test_high_band_tail(c);
    test_snare_stability_window();
    test_fill_cleanup_role();
    test_final_hook_span(c);
    test_final_fill_trim(c);
    test_repeated_peak_offsets(c);
    const double *model = fixture_phrase_parameters();
    const int current[] = { 40, 41, 440, 441 }, intro[] = { 10, 11, 110, 111 };
    for (int i = 0; i < 4; i++) {
        int roles[] = { 10, current[i], 50 };
        remap_short_hook((I){ roles, 3 }, 1, 3, model);
        assert(roles[1] == intro[i]);
        int ending[] = { 50, current[i] };
        remap_short_hook((I){ ending, 2 }, 1, 2, model);
        assert(ending[1] == (i < 2 ? 60 : 61));
    }
}

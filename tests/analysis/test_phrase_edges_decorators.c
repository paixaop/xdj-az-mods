/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_decorators.c"
#include "phrase-model-fixture.h"
#include <assert.h>

static void test_rhythm_prototype(ta_context *c)
{
    uint8_t activity[16] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    B segment = { activity, sizeof activity };
    const double *model = fixture_phrase_parameters();
    assert(rhythm_codes(c, segment, 1, 1, model).v[0] == model[TA_RHYTHM_RUN_FOUR_VALUE]);

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_RHYTHM_RUN_FOUR_PROTOTYPE_0] = 0;
    assert(rhythm_codes(c, segment, 1, 1, modified).v[0] == 0);
}

static void test_rhythm_dense_history(ta_context *c)
{
    uint8_t activity[64] = {[50] = 1, [51] = 1};
    for (int i = 16; i < 48; i += 2)
        activity[i] = 1;
    B segment = {activity, sizeof activity};
    const double *model = fixture_phrase_parameters();
    I codes = rhythm_codes(c, segment, 1, 4, model);
    assert(codes.v[3] == model[TA_RHYTHM_RUN_DENSE_VALUE]);
    ta_release(c, codes.v);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_RUN_DENSE_SECOND_BACK] = 3;
    changed[TA_RHYTHM_RUN_DENSE_PREVIOUS_BIAS] = 2;
    changed[TA_RHYTHM_RUN_DENSE_MIN_GROUP] = 3;
    codes = rhythm_codes(c, segment, 1, 4, changed);
    assert(codes.v[3] == 0);
    ta_release(c, codes.v);
}

static void test_high_export_threshold(ta_context *c)
{
    int boundaries[] = { 1, 17, 33 };
    int roles[] = { 100, 101 }, zeros[] = { 0, 0 };
    I empty = { zeros, 2 };
    Fills fills = { .roles = { roles, 2 }, .starts = empty, .auxiliary = empty, .routes = empty };
    Decorated sections = { .boundaries = { boundaries, 3 }, .fills = fills, .upStarts = empty,
                           .upFirst = empty, .upSecond = empty, .upThird = empty };
    const double *model = fixture_phrase_parameters();
    PhraseExport export = (phrase_convert)(c, MOOD_HIGH, sections, (Variation){0}, model);
    assert(export.map.entries[0].kind == 1 && !export.map.entries[0].fill);
    assert(export.map.entries[1].kind == 1 && export.map.entries[1].fill);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIGH_EXPORT_INTRO_FILL_MIN_ROLE] = 100;
    export = (phrase_convert)(c, MOOD_HIGH, sections, (Variation){0}, changed);
    assert(export.map.entries[0].fill);
}

static void test_outro_peak_model(void)
{
    double levels[] = { 0.1, 0.4, 0.3, 0.001 };
    double growth[] = { 0, 4, 0.75, 0.003 };
    D samples = { levels, 4 }, ratios = { growth, 4 };
    const double *model = fixture_phrase_parameters();
    assert(outro_final_peak(samples, ratios, 4, 20, 0, model) == 21);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OUTRO_FINAL_PEAK_LEVEL_MIN] = 0.45;
    assert(outro_final_peak(samples, ratios, 4, 20, 0, changed) == 0);

    uint8_t subdivisions[100] = { [51] = 1 };
    B activity = { subdivisions, sizeof subdivisions };
    assert(outro_last_activity(activity, 1, model) == 14);
    changed[TA_OUTRO_ACTIVITY_LOOKBACK_FROM_PREVIOUS_BEAT] = 11;
    assert(outro_last_activity(activity, 1, changed) == 0);
}

static void test_outro_phase_snap(void)
{
    const double *model = fixture_phrase_parameters();
    double values[] = {0, 3, 3, 3, 0};
    D ratios = {values, sizeof values / sizeof *values};
    assert(outro_snap_fall(ratios, 3, 6, 1, model) == 4);
    assert(outro_snap_fall(ratios, 4, 7, 1, model) == 4);
    assert(outro_snap_fall(ratios, 2, 7, 1, model) == 7);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OUTRO_SNAP_FIRST_DISTANCE] = 1;
    assert(outro_snap_fall(ratios, 3, 6, 1, changed) == 5);
    changed[TA_OUTRO_SNAP_SECOND_FAR_BACK] = 4;
    assert(outro_snap_fall(ratios, 4, 7, 1, changed) == 7);
    changed[TA_OUTRO_SNAP_PHASE_PERIOD] = 8;
    assert(outro_snap_fall(ratios, 3, 6, 1, changed) == 6);
}

static void test_outro_terminal_phase(ta_context *c)
{
    double levels[20];
    for (int i = 0; i < 20; i++)
        levels[i] = 1;
    D low = {levels, 20};
    const double *model = fixture_phrase_parameters();
    assert(outro_level_terminal(c, low, 1, 1, 8, model) == 9);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OUTRO_TERMINAL_PHASE_BIAS] = 2;
    assert(outro_level_terminal(c, low, 1, 1, 8, changed) == 8);
    changed[TA_OUTRO_TERMINAL_PHASE_BIAS] = 1;
    changed[TA_OUTRO_TERMINAL_PHASE_PERIOD] = 16;
    assert(outro_level_terminal(c, low, 1, 1, 8, changed) == 8);
}

static void test_beat_four_geometry(void)
{
    const double *model = fixture_phrase_parameters();
    double values[] = {0, 0, 0, 0, 8, 0, 0, 0, 2, 0, 0, 8};
    double amplitudes[3] = {0}, ratios[3] = {0};
    int positions[] = {1, 0, 0};
    D quarters = {values, 12}, peaks = {amplitudes, 3}, similarities = {ratios, 3};
    I slots = {positions, 3};
    quarter_peaks(1, 3, quarters, slots, peaks, similarities, model);
    assert(ratios[0] == 1 && ratios[1] == 0.25);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BEAT_FOUR_EQUAL_PEAK_SCORE] = 0.5;
    changed[TA_BEAT_FOUR_ADJACENCY_MASK] = 3;
    memset(ratios, 0, sizeof ratios);
    quarter_peaks(1, 3, quarters, slots, peaks, similarities, changed);
    assert(ratios[0] == 0.5 && ratios[1] == 0);

    double levels[] = {1, 1, 1, 1, 1, 1, 1};
    D stable = {levels, 7};
    assert(stable_quarter_amplitudes(stable, 7, model));
    changed[TA_BEAT_FOUR_STABILITY_TAIL_COUNT] = 5;
    assert(!stable_quarter_amplitudes(stable, 7, changed));
}

static void test_phrase_variation_reference(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    int markers[] = {1, 3, 5, 7, 9, 11};
    double values[] = {0, 24, 0, 3, 0, 18, 0, 30, 0, 15, 0};
    I boundaries = {markers, 6};
    D envelope = {values, 11};
    Variation result = (phrase_variation)(c, envelope, boundaries, model);
    assert(result.summary == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_VARIATION_OMIT_REFERENCE_END_BACK] = 3;
    result = (phrase_variation)(c, envelope, boundaries, changed);
    assert(fabs(result.summary - 8.0 / 3) < 1e-12);

    memcpy(changed, model, sizeof changed);
    changed[TA_VARIATION_QUIET_MEAN_INDEX] = 0;
    result = (phrase_variation)(c, envelope, boundaries, changed);
    assert(fabs(result.summary - 7.0 / 3) < 1e-12);
}

static void test_outro_activity_at_start(ta_context *c)
{
    int starts[] = { 1 }, roles[] = { 60 };
    double low[12], quarters[48];
    uint8_t activity[48] = { 1 };
    for (size_t i = 0; i < sizeof low / sizeof *low; i++)
        low[i] = 1;
    for (size_t i = 0; i < sizeof quarters / sizeof *quarters; i++)
        quarters[i] = 1;
    const double *model = fixture_phrase_parameters();
    assert(outro_last_activity((B){ activity, sizeof activity }, 1, model) == 2);
    Outro result = phrase_outro(c, (I){ starts, 1 }, (I){ roles, 1 }, (D){ low, 12 },
                                (B){ activity, sizeof activity }, (D){ quarters, 48 }, 1);
    assert(result.terminal == 13);

    double levels[] = { .7, .6, .5, 0, .001 };
    D input = { levels, sizeof levels / sizeof *levels };
    assert(outro_validate_activity(c, input, 2, model) == 0);
    levels[1] = .1;
    assert(outro_validate_activity(c, input, 2, model) == 0);
    assert(outro_validate_activity(c, input, 4, model) == 0);
    assert(levels[3] == model[TA_OUTRO_ACTIVITY_ZERO_LEVEL]);
    assert(outro_validate_activity(c, input, 5, model) == 5);
    assert(outro_validate_activity(c, input, 6, model) == 0);
}

static void test_rhythm_run_tail_geometry(void)
{
    int raw[] = { 8, 8, 8, 0, 16, 0, 0, 0, 0 };
    I codes = { raw, 9 };
    const double *model = fixture_phrase_parameters();
    RhythmRun run = { 0 };
    rhythm_find_run(&run, codes, 6, model);
    assert(run.firstEight == 1 && run.afterEights == 5);
    assert(rhythm_has_tail(run, codes, 6, model));

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_RUN_THIRD_EIGHT_NEXT] = 2;
    rhythm_find_run(&run, codes, 6, changed);
    assert(run.firstEight == 1 && run.afterEights == 3);
    assert(!rhythm_has_tail(run, codes, 6, changed));
}

static void test_rhythm_growth_late_average(void)
{
    int raw[] = { 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 9, 2, 2, 0 };
    I counts = { raw, 16 };
    const double *model = fixture_phrase_parameters();
    RhythmGrowth original = rhythm_growth_64(counts, .9, model);
    assert(original.staged);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_GROWTH_TWO_OFFSET] = -3;
    RhythmGrowth modified = rhythm_growth_64(counts, .9, changed);
    assert(!modified.staged);
}

static void test_simple_loop_roles(ta_context *c)
{
    int boundaries[] = {1, 25, 49, 73};
    int roles[] = {30, 50, 40, 40};
    double first[96] = {0}, second[96] = {0}, levels[4] = {0};
    for (int i = 0; i < 96; ++i)
        second[i] = i % 24 >= 16 ? 120 : 0;
    I beats = {boundaries, 4}, input = {roles, 4};
    D low = {first, 96}, mid = {second, 96}, metrics = {levels, 4};
    const double *model = fixture_phrase_parameters();
    UpAnalysis result = (simple_loops)(c, beats, input, low, mid, metrics, model);
    int expected[] = {31, 31, 40, 40};
    assert(!memcmp(result.roles.v, expected, sizeof expected));

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_SIMPLE_LOOP_FIRST_INPUT_ROLE] = 40;
    parameters[TA_SIMPLE_LOOP_OUTPUT_ROLE] = 32;
    result = (simple_loops)(c, beats, input, low, mid, metrics, parameters);
    int modified[] = {30, 32, 32, 32};
    assert(!memcmp(result.roles.v, modified, sizeof modified));
}

static void test_simple_loop_midpoint_window(ta_context *c)
{
    enum {count = 40};
    double first[count] = {0}, second[count], levels[] = {0};
    for (int i = 0; i < count; ++i)
        second[i] = 1024;
    second[26] = 0;
    int boundary = 1;
    const double *model = fixture_phrase_parameters();
    int role = (int)model[TA_SIMPLE_LOOP_FIRST_INPUT_ROLE];
    I beats = {&boundary, 1}, input = {&role, 1};
    D low = {first, count}, mid = {second, count}, metrics = {levels, 1};
    UpAnalysis result = (simple_loops)(c, beats, input, low, mid, metrics, model);
    assert(result.roles.v[0] == model[TA_SIMPLE_LOOP_OUTPUT_ROLE] && result.starts.v[0] == boundary);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SIMPLE_LOOP_MINIMUM_END_LOOKAHEAD] += 1;
    result = (simple_loops)(c, beats, input, low, mid, metrics, changed);
    assert(result.roles.v[0] == role && result.starts.v[0] == 0);
}

void test_phrase_edges_decorators(ta_context *c)
{
    test_rhythm_prototype(c);
    test_rhythm_dense_history(c);
    test_rhythm_run_tail_geometry();
    test_rhythm_growth_late_average();
    test_high_export_threshold(c);
    test_outro_peak_model();
    test_outro_phase_snap();
    test_outro_terminal_phase(c);
    test_beat_four_geometry();
    test_phrase_variation_reference(c);
    test_outro_activity_at_start(c);
    test_simple_loop_roles(c);
    test_simple_loop_midpoint_window(c);
    int roles[] = { 31, 31, 31, 40 }, saved[] = { 30, 50, 30, 40 };
    double metrics[] = { 2, 5, 3, 0 };
    I restored = restore_loops(c, (I){ roles, 4 }, (I){ saved, 4 }, (D){ metrics, 4 });
    const int expected[] = { 30, 31, 30, 40 };
    assert(!memcmp(restored.v, expected, sizeof expected));
    int counts[16];
    for (int i = 0; i < 16; i++) counts[i] = i < 8 ? 1 : i < 12 ? 3
                                                                : 6;
    RhythmGrowth g = rhythm_growth_64((I){ counts, 16 }, 0.9, fixture_phrase_parameters());
    assert(g.staged && !g.simple && g.extent == 48 && g.secondOffset == 32 && g.thirdOffset == 16);
    for (int i = 0; i < 16; i++) counts[i] = 4;
    g = rhythm_growth_64((I){ counts, 16 }, 0.9, fixture_phrase_parameters());
    assert(!g.staged && !g.simple && g.extent == 32);
    for (int i = 0; i < 16; i++) counts[i] = i < 8 ? 4 : i < 12 ? 3
                                                                : 4;
    g = rhythm_growth_64((I){ counts, 16 }, 0.9, fixture_phrase_parameters());
    assert(!g.staged && g.simple);
    double accelerations[10] = { 0 };
    accelerations[1] = 1.5;
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 0, fixture_phrase_parameters()) == 21);
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 19, fixture_phrase_parameters()) == 19);
    accelerations[8] = 1;
    assert(outro_accelerating((D){ accelerations, 10 }, 10, 20, 0, fixture_phrase_parameters()) == 0);
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_features.c"
#include <assert.h>
#include "../../src/common/aztimestamp.h"
#include "phrase-model-fixture.h"

static void test_phrase_sum_geometry(const double *model)
{
    double values[] = {0x1p60, 0, -0x1p60, 0, 1, 0, 0, 0, 3};
    D input = {values, sizeof values / sizeof *values};
    assert(d_sum(input, model) == 4);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_PHRASE_SUM_PAIR_0] = 2;
    changed[TA_PHRASE_SUM_PAIR_2] = 1;
    assert(d_sum(input, changed) == 3);

    memcpy(changed, model, sizeof changed);
    changed[TA_PHRASE_SUM_GROUP] = 4;
    changed[TA_PHRASE_SUM_PAIR_3] = -1;
    assert(range_sum(input, 0, input.n, changed) == 3);
}

static void test_windows_phrase_sum_geometry(const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double *order = parameters + TA_PHRASE_SUM_PAIR_0;
    fixture_windows_reduction(order);
    double values[] = {10, 0x1p53, 1, 0, 1, 0, 0, 0, 0, 4};
    assert(range_sum((D){values, 10}, 1, 8, model) == 0x1p53 + 2);
    assert(range_sum((D){values, 10}, 1, 8, parameters) == 0x1p53);
    assert(d_sum((D){values + 1, 9}, parameters) == 0x1p53 + 4);
    assert(d_sum((D){values + 1, 4}, parameters) == 0x1p53);

    order[0] = 0x3120;
    order[1] = 0x7564;
    assert(d_sum((D){values + 1, 8}, parameters) == 0x1p53 + 2);
}

static void test_pcm_block_rounding(const double *model)
{
    assert(phrase_pcm_block_frames(44100, model) == 75264);
    assert(phrase_pcm_block_frames(48000, model) == 81920);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_PCM_BLOCK_SECONDS] = 10.75 / 44100;
    assert(phrase_pcm_block_frames(44100, changed) == 11);
    changed[TA_PCM_BLOCK_ROUNDING_BIAS] = 0;
    assert(phrase_pcm_block_frames(44100, changed) == 10);
}

static Samples decimator_impulse(ta_context *c, const double *model)
{
    Samples samples = {0};
    D *bands[] = {&samples.low, &samples.middle, &samples.high};
    int divisors[] = {2, 2, 2};
    double input[] = {1, 0, 0, 0};
    ta_phrase_biquad sections[TA_DECIMATE_BAND_COUNT][TA_DECIMATE_SECTION_CAPACITY];
    initialize_bands(c, 48000, sizeof input / sizeof *input, bands, divisors, model, sections);
    decimator_job job = {
        .sections = sections, .bands = bands, .pending = input, .divisors = divisors,
        .filled = sizeof input / sizeof *input, .section_count = (int)model[TA_DECIMATE_FILTER_SECTIONS]
    };
    for (size_t band = 0; band < sizeof bands / sizeof *bands; ++band)
        decimate_band(c, &job, band, 0);
    return samples;
}

static void test_decimator_section_count(ta_context *c, const double *model)
{
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_DECIMATE_FILTER_SECTIONS] = 1;
    Samples source = decimator_impulse(c, model);
    Samples short_chain = decimator_impulse(c, changed);
    assert(source.low.n == 2 && short_chain.low.n == source.low.n);
    assert(source.low.v[0] != short_chain.low.v[0]);
    assert(source.middle.v[0] != short_chain.middle.v[0]);
    assert(source.high.v[0] != short_chain.high.v[0]);
}

static void test_envelope_section_counts(ta_context *c, const double *model)
{
    double input[] = {1, -0.5, 0.25, 0, 0, 0};
    D band = {input, sizeof input / sizeof *input};
    Samples source = {.rate = 44100, .low = band, .middle = band, .high = band};
    Envelopes baseline = phrase_envelopes(c, source, model);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_ENVELOPE_MIDDLE_HIGHPASS_SECTIONS] = 1;
    changed[TA_ENVELOPE_HIGH_LOWPASS_SECTIONS] = 1;
    changed[TA_ENVELOPE_MIDDLE_LEVEL_SMOOTH_SECTIONS] = 2;
    Envelopes alternate = phrase_envelopes(c, source, changed);
    assert(alternate.middle.v[0] != baseline.middle.v[0]);
    assert(alternate.high.v[0] != baseline.high.v[0]);
    assert(alternate.middleLevel.v[0] != baseline.middleLevel.v[0]);
    assert(!memcmp(alternate.low.v, baseline.low.v, sizeof input));
    assert(!memcmp(alternate.lowLevel.v, baseline.lowLevel.v, sizeof input));
}

static void test_activity_window_geometry(ta_context *c, const double *model)
{
    double values[20];
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        values[i] = i + 1;
    D input = {values, sizeof values / sizeof *values};
    Activity low = (low_activity)(c, input, 1, model);
    Activity middle = (middle_activity)(c, input, 1, model);
    assert(low.values.n == 5 && low.values.v[0] == 10 && low.values.v[2] == 42);
    assert(middle.values.n == 5 && middle.values.v[3] == 58 && middle.changes.v[1]);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_LOW_ACTIVITY_SUM_LAST] = 2;
    changed[TA_LOW_ACTIVITY_SUM_STRIDE] = 3;
    changed[TA_MIDDLE_ACTIVITY_TAIL_BARS] = 2;
    changed[TA_MIDDLE_ACTIVITY_CHANGE_START] = 2;
    low = (low_activity)(c, input, 1, changed);
    middle = (middle_activity)(c, input, 1, changed);
    assert(low.values.v[0] == 6 && low.values.v[2] == 24);
    assert(middle.values.v[3] == 0 && !middle.changes.v[1]);
}

static void test_low_quiet_neighbors(const double *model)
{
    double values[] = {0, 0, 0, 0, 1, 0};
    D input = {values, sizeof values / sizeof *values};
    assert(!low_quiet(0, 0, input, 0, model));
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_LOW_ACTIVITY_QUIET_CENTER] = 3;
    assert(low_quiet(0, 0, input, 0, changed));
    assert(!low_quiet(0, 0, input, 1, changed));
}

static void test_beat_sample_endpoints(const double *model)
{
    double positions[] = {1, 9, 2, 99};
    D values = {positions, sizeof positions / sizeof *positions};
    beat_interval window = interval_geometry(model, TA_BEAT_MIDDLE_PEAK_SAMPLE_ORIGIN,
                                             TA_BEAT_MIDDLE_PEAK_LAST_SAMPLE_BACK);
    assert(interval_max(values, 1, 4, 3, false, window) == 9);
    assert(interval_max(values, 1, 4, 2, false, window) == 0);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BEAT_MIDDLE_PEAK_LAST_SAMPLE_BACK] = 2;
    window = interval_geometry(changed, TA_BEAT_MIDDLE_PEAK_SAMPLE_ORIGIN, TA_BEAT_MIDDLE_PEAK_LAST_SAMPLE_BACK);
    assert(interval_max(values, 1, 4, 2, false, window) == 9);
    changed[TA_BEAT_MIDDLE_PEAK_SAMPLE_ORIGIN] = 2;
    window = interval_geometry(changed, TA_BEAT_MIDDLE_PEAK_SAMPLE_ORIGIN, TA_BEAT_MIDDLE_PEAK_LAST_SAMPLE_BACK);
    assert(interval_max(values, 1, 4, 3, false, window) == 0);
}

static void test_middle_grid_advance(const double *model)
{
    int coordinates[] = {1, 4, 6};
    double data[] = {1, 9, 2, 12, 21, 0};
    D source = {data, sizeof data / sizeof *data};
    Coordinates grid = {.middle = {coordinates, 3}, .n = 1};
    Envelopes envelopes = {.middle = source, .middleLevel = source};
    Samples samples = {.middle = source};
    double middle_level, middle_peak, all_level;
    Beats result = {
        .middleLevels = {&middle_level, 1},
        .middlePeaks = {&middle_peak, 1},
        .allLevels = {&all_level, 1},
    };
    middle_beat(&result, envelopes, grid, samples, 0, source.n, model);
    assert(middle_level == 12 && middle_peak == 9 && all_level == 9);

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_BEAT_MIDDLE_GRID_ADVANCE] = 2;
    middle_beat(&result, envelopes, grid, samples, 0, source.n, parameters);
    assert(middle_level == 45 && middle_peak == 21 && all_level == 21);
}

static void test_low_peak_fall_factor(const double *model)
{
    double previous = fmax(4 * model[TA_LOW_PEAK_QUIET_LEVEL], 3);
    double next = previous * (2.0 / 3.0);
    assert(low_peak_change(previous, next, false, model) == 0);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_LOW_PEAK_FALL_FACTOR] = 1.25;
    assert(low_peak_change(previous, next, false, changed) == model[TA_LOW_PEAK_FALL_LABEL]);
}

static void test_coordinate_profile(ta_context *c, const double *model)
{
    I actual = stage_coordinates(c, 48000, 48000, 120, 0, model);
    assert(actual.n == 2 && actual.v[0] == 1 && actual.v[1] == 24001);
    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_STAGE_COORD_ENDPOINT_BIAS] = 0.5;
    actual = stage_coordinates(c, 48000, 48000, 120, 0, modified);
    assert(actual.n == 2 && actual.v[0] == 0 && actual.v[1] == 24000);
}

static void test_decimated_coordinate_rate(ta_context *c, const double *model)
{
    Samples input = { .rate = 48000, .frames = 480000 };
    Timing timing = { .bpm = 120, .phase = 1 };
    Coordinates baseline = (phrase_coordinates)(c, input, timing, model);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_DECIMATE_MIDDLE_RATE_SCALE] *= 2;
    Coordinates adjusted = (phrase_coordinates)(c, input, timing, changed);
    assert(adjusted.n == baseline.n);
    assert(adjusted.middle.v[1] - baseline.middle.v[1] == 3000);
    assert(adjusted.high.v[1] == baseline.high.v[1] && adjusted.low.v[1] == baseline.low.v[1]);
    assert(baseline.middle.n == baseline.n + 1);
    changed[TA_PHRASE_COORD_GRID_TRAILING_COUNT] = 2;
    Coordinates extended = (phrase_coordinates)(c, input, timing, changed);
    assert(extended.n == baseline.n && extended.middle.n == adjusted.middle.n + 1);
    assert(extended.middle.v[extended.n + 1] - extended.middle.v[extended.n] == 6000);
}

static void test_high_beat_terminal_sample(ta_context *c, const double *model)
{
    D samples = ta_new_doubles(c, 7200);
    samples.v[7199] = 7;
    Envelopes envelope = { .high = samples };
    int start[] = { 1 };
    Coordinates positions = { .high = { start, 1 } };
    Beats result = { .eighths = ta_new_doubles(c, 8) };

    BeatBandLengths bands = beat_band_lengths(44100, 183.75, model);
    assert(bands.high_peak == 7200);
    high_beat(&result, envelope, positions, bands, 0, 1, samples.n, model);
    assert(result.eighths.v[7] == 7);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BEAT_HIGH_PEAK_PERIOD_NUMERATOR] = 59;
    bands = beat_band_lengths(44100, 183.75, changed);
    high_beat(&result, envelope, positions, bands, 0, 1, samples.n, changed);
    assert(result.eighths.v[7] == 0);
    assert(phrase_frame_extent(c, 10, model) == 9);
    changed[TA_PHRASE_SOURCE_FRAME_TRIM] = 2;
    assert(phrase_frame_extent(c, 10, changed) == 8);
}

static void test_first_activity_terminal_sample(const double *model)
{
    double gate = model[TA_FIRST_ACTIVITY_MIN_AMPLITUDE];
    double values[] = { gate, -nextafter(gate, INFINITY), 0 };
    double bpm = model[TA_FIRST_ACTIVITY_BEAT_SECONDS];
    int bias = (int)model[TA_FIRST_ACTIVITY_RESULT_BIAS];
    assert(activity_start((D){ values, 2 }, 1, bpm, 0, model) == bias);
    assert(activity_start((D){ values, 3 }, 1, bpm, 0, model) == bias + 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_FIRST_ACTIVITY_SCAN_TAIL]++;
    assert(activity_start((D){ values, 3 }, 1, bpm, 0, changed) == bias);
}

static void test_activity_bar_extents(ta_context *c, const double *model)
{
    int phase = (int)model[TA_REFERENCE_TIMING_PHASE_1];
    int all_min = (int)(model[TA_ALL_ACTIVITY_MIN_EXTENT] - model[TA_ALL_ACTIVITY_EXTENT_PAD]) + phase;
    int middle_min = (int)(model[TA_MIDDLE_PEAK_MIN_EXTENT] - model[TA_MIDDLE_PEAK_EXTENT_PAD]) + phase;
    D samples = ta_new_doubles(c, ta_max(all_min, middle_min));
    for (int i = 0; i < samples.n; i++)
        samples.v[i] = 2;
    AllActivity all = (all_activity)(c, (D){ samples.v, all_min - 1 }, phase, model);
    assert(all.bars.n && all.bars.v[0] == 0);
    all = (all_activity)(c, (D){ samples.v, all_min }, phase, model);
    assert(all.bars.n == 2 && all.bars.v[0] == 1 && all.bars.v[1] == 0);

    AllActivity middle = middle_peak_activity(c, (D){ samples.v, middle_min - 1 }, middle_min - 1, phase, model);
    assert(middle.bars.n && middle.bars.v[0] == 0);
    middle = middle_peak_activity(c, (D){ samples.v, middle_min }, middle_min, phase, model);
    assert(middle.bars.n == 2 && middle.bars.v[0] == 1 && middle.bars.v[1] == 0);
    assert(middle.normalized.v[middle_min - 1] == 2);
}

static void test_reference_offset_scale(ta_context *c, const double *model)
{
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_NOMINAL_BEAT_OFFSET_SCALE] *= 2;
    double units_per_second = 1 / changed[TA_NOMINAL_BEAT_OFFSET_SCALE];
    Timing timing = reference_timing(c, 120, 0.749, 1, 120, changed);
    assert(compare_timestamps(timing.offset, 375, units_per_second, 44100, 1) == AZ_TIME_EQUAL);
    assert(!valid_reference_timing(12000, 0, NAN, 120));
}

void test_phrase_edges_features(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_phrase_sum_geometry(model);
    test_windows_phrase_sum_geometry(model);
    test_pcm_block_rounding(model);
    test_decimator_section_count(c, model);
    test_envelope_section_counts(c, model);
    test_activity_window_geometry(c, model);
    test_low_quiet_neighbors(model);
    test_beat_sample_endpoints(model);
    test_middle_grid_advance(model);
    test_low_peak_fall_factor(model);
    test_first_activity_terminal_sample(model);
    test_activity_bar_extents(c, model);
    Timing a = reference_timing(c, 120, 0.75, 1, 121, model);
    assert(a.bpm == 120 && compare_timestamps(a.offset, 250, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 4);
    a = reference_timing(c, 120, 0.75, 2, 121, model);
    assert(compare_timestamps(a.offset, 250, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 3);
    a = reference_timing(c, 120, 0.5, 0, 121, model);
    assert(compare_timestamps(a.offset, 500, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 1);
    a = reference_timing(c, 120, 1.0, 5, 121, model);
    assert(compare_timestamps(a.offset, 1000, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 1);

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_REFERENCE_TIMING_PHASE_2] = 3;
    a = reference_timing(c, 120, 0.75, 2, 121, modified);
    assert(compare_timestamps(a.offset, 250, 1000, 44100, 1) == AZ_TIME_EQUAL && a.phase == 2);
    test_coordinate_profile(c, model);
    test_decimated_coordinate_rate(c, model);
    test_high_beat_terminal_sample(c, model);
    test_reference_offset_scale(c, model);
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_classifier.c"
#include "phrase-model-fixture.h"
#include <assert.h>

static void test_fade_reference_window(void)
{
    double ratios[15];
    for (int i = 0; i < 15; i++)
        ratios[i] = 0.5;
    ratios[0] = 0.4;
    ratios[1] = 0.6;
    D values = { ratios, 15 };
    assert(stable_tail_ratios(values, 12));
    for (int i = 12; i < 15; i++) {
        ratios[i] = 0.7;
        assert(!stable_tail_ratios(values, 12));
        ratios[i] = 0.3;
        assert(!stable_tail_ratios(values, 12));
        ratios[i] = 0.5;
    }
    ratios[12] = 0.7;
    assert(stable_tail_ratios(values, 13));
}

static void test_route_profile(ta_context *c)
{
    double levels[160] = { 0 };
    D low = { levels, 160 };
    int type = 31;
    I loops = { &type, 1 }, rhythms = { 0 };
    const double *model = fixture_phrase_parameters();
    Route result = (phrase_route)(c, low, 1, 120, loops, rhythms, MOOD_MID, model);
    assert(result.mood == MOOD_MID);
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_ROUTE_SHORT_LOOP_MIN] = 1;
    result = (phrase_route)(c, low, 1, 120, loops, rhythms, MOOD_MID, parameters);
    assert(result.mood == MOOD_HIGH);
}

static void test_slow_rock_profile(ta_context *c)
{
    uint8_t low_samples[96] = { 0 }, middle_samples[96] = { 0 };
    for (int start = 0; start <= 51; start += 17) {
        low_samples[start] = 1;
        middle_samples[start] = 1;
        middle_samples[start + 7] = 1;
    }
    B low = { low_samples, 96 }, middle = { middle_samples, 96 };
    const double *model = fixture_phrase_parameters();
    assert((slow_rock)(c, low, middle, model));
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SLOW_ROCK_TERMINAL_LAST] = -10;
    assert(!(slow_rock)(c, low, middle, changed));
}

static void test_kick_mask_profile(void)
{
    uint8_t samples[8 * 16] = { 0 };
    for (int i = 0; i < 16; i += 4)
        samples[16 + i] = 1;
    B rhythm = { samples, 8 * 16 };
    const double *model = fixture_phrase_parameters();
    int counts[4] = { 0 };
    count_rhythm_patterns(rhythm, 1, 1, counts, model);
    assert(counts[0] == 1);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_MASK_KICK_FIRST] = 1;
    memset(counts, 0, sizeof counts);
    count_rhythm_patterns(rhythm, 1, 1, counts, changed);
    assert(counts[0] == 0);
}

static void test_rhythm_secondary_votes(void)
{
    int counts[4] = {0, 0, 0, 1};
    const double *model = fixture_phrase_parameters();
    int cutoff = (int)model[TA_RHYTHM_LONG_TRACK_CUTOFF];
    assert(choose_rhythm_flags(counts, cutoff, 120, model).sparseKick == 1);
    assert(choose_rhythm_flags(counts, cutoff + 1, 120, model).sparseKick == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_SECONDARY_LONG_CUTOFF] = 0;
    changed[TA_RHYTHM_ACTIVE_FLAG] = 2;
    assert(choose_rhythm_flags(counts, cutoff + 1, 120, changed).sparseKick == 2);
}

static void test_tempo_onset_shift(ta_context *c)
{
    int coordinates[] = {100, 200, 600};
    SnarePositions onsets = {.values = {coordinates, 3}, .first = 2, .count = 1};
    const double *model = fixture_phrase_parameters();
    double scale = model[TA_TEMPO_ONSET_SECONDS_PER_MILLISECOND];
    D shifted = shifted_onsets(c, onsets, 48000, 5, 3, scale);
    assert(shifted.v[0] == 100 && shifted.v[1] == 296 && shifted.v[2] == 600);
    ta_release(c, shifted.v);

    shifted = shifted_onsets(c, onsets, 48000, 3, 5, scale);
    assert(shifted.v[1] == 104);
    ta_release(c, shifted.v);
}

static void test_mood_flag_model(void)
{
    const double *model = fixture_phrase_parameters();
    MoodInputs inputs = { .slowRock = 1, .lowPowerRatio = 1, .compression = 1, .secondOverride = 1 };
    assert((initial_mood)(inputs, model) == MOOD_LOW);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_MOOD_SLOW_ROCK_FLAG] = 2;
    assert((initial_mood)(inputs, changed) == MOOD_HIGH);
}

static void test_compression_following_phase_limit(void)
{
    double levels[132] = { [127] = 2, [128] = 4, [129] = 32, [130] = 1024 };
    uint8_t marks[128] = { [123] = 1, [124] = 1 };
    D power = { levels, 132 };
    B rhythm = { marks, 128 };
    const double *model = fixture_phrase_parameters();
    compression_hits hits = compression_block(power, power, rhythm, 4, 0, 128, model);
    assert(hits.hits == 2 && hits.low == 6 && hits.full == 6);
    marks[125] = 1;
    hits = compression_block(power, power, rhythm, 4, 0, 128, model);
    assert(hits.hits == 3 && hits.low == 38 && hits.full == 38);
}

static void test_mood_correspondence_grid(ta_context *c)
{
    double levels[13] = {[5] = 2, [10] = 4};
    double peaks[3] = {0}, matches[3] = {0};
    D quarter_power = {levels, 13}, maxima = {peaks, 3}, correspondence = {matches, 3};
    const double *model = fixture_phrase_parameters();
    mood_correspondence(c, quarter_power, 1, 3, maxima, correspondence, model);
    assert(peaks[1] == 2 && peaks[2] == 4);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_REVISE_BEAT_QUARTERS] = 2;
    mood_correspondence(c, quarter_power, 1, 3, maxima, correspondence, changed);
    assert(peaks[1] == 0 && peaks[2] == 2);
}

static void test_seed_geometry(ta_context *c)
{
    int boundaries[] = { 1, 9, 17 };
    double opening[] = { .25, .5, .75 };
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, fixture_phrase_parameters(), sizeof parameters);
    I relative = seed_boundaries(c, (I){ boundaries, 3 }, parameters);
    assert(relative.n == 3 && relative.v[0] == 1 && relative.v[1] == 3 && relative.v[2] == 5);
    D padded = seed_padded_envelope(c, (D){ opening, 3 }, 2, parameters);
    assert(padded.n == 8 && padded.v[2] == .75 && padded.v[7] == 0);
    parameters[TA_SEED_BOUNDARY_DIVISOR] = 8;
    parameters[TA_SEED_BOUNDARY_ORIGIN] = 3;
    parameters[TA_SEED_ENVELOPE_PAD_SCALE] = 3;
    I changed = seed_boundaries(c, (I){ boundaries, 3 }, parameters);
    assert(changed.n == 3 && changed.v[0] == 3 && changed.v[1] == 4 && changed.v[2] == 5);
    D smaller = seed_padded_envelope(c, (D){ opening, 3 }, 2, parameters);
    assert(smaller.n == 6 && smaller.v[2] == .75 && smaller.v[5] == 0);
    ta_release(c, relative.v);
    ta_release(c, padded.v);
    ta_release(c, changed.v);
    ta_release(c, smaller.v);
}

static void test_recurrence_distance_reduction(void)
{
    double a[] = {1, 0, 1, 0, 1, 0, 0x1p27, 0};
    double b[8] = {0};
    D rows[] = {{a, 8}, {b, 8}};
    Spectra spectra = {.rows = rows, .n = 2};
    double zeros[2] = {0};
    D means = {zeros, 2};
    const double *model = fixture_phrase_parameters();
    assert(recurrence_distance(spectra, means, 0, 1, 8, model) == 0x1p54 + 4);

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_RECURRENCE_DISTANCE_PAIR_0] = 3;
    parameters[TA_RECURRENCE_DISTANCE_PAIR_3] = 1;
    assert(recurrence_distance(spectra, means, 0, 1, 8, parameters) == 0x1p54);

    memcpy(parameters, model, sizeof parameters);
    parameters[TA_RECURRENCE_DISTANCE_GROUP] = 4;
    parameters[TA_RECURRENCE_DISTANCE_PAIR_3] = -1;
    assert(recurrence_distance(spectra, means, 0, 1, 8, parameters) == 0x1p54);
}

static void test_windows_recurrence_distance(void)
{
    const double *model = fixture_phrase_parameters();
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    fixture_windows_reduction(parameters + TA_RECURRENCE_DISTANCE_PAIR_0);

    double a[] = {1, 0, 1, 0, 1, 0, 0x1p27, 0, 0};
    double b[9] = {0};
    D rows[] = {{a, 9}, {b, 9}};
    Spectra spectra = {.rows = rows, .n = 2};
    double mean_values[] = {0, 0};
    D means = {mean_values, 2};
    assert(recurrence_distance(spectra, means, 0, 1, 8, parameters) == 0x1p54);

    memset(a, 0, sizeof a);
    a[0] = b[0] = 0x1p53;
    mean_values[0] = 1;
    assert(recurrence_distance(spectra, means, 0, 1, 1, parameters) == 1);
    assert(recurrence_distance(spectra, means, 0, 1, 1, model) == 0);
    assert(recurrence_distance(spectra, means, 0, 1, 8, parameters) == 8);
    assert(recurrence_distance(spectra, means, 0, 1, 8, model) == 7);

    a[0] = a[1] = 0x1p-27;
    a[8] = 0x1.0000002p0;
    b[0] = mean_values[0] = 0;
    assert(recurrence_distance(spectra, means, 0, 1, 9, parameters) == 0x1.0000004p0);
    assert(recurrence_distance(spectra, means, 0, 1, 9, model) == fma(a[8], a[8], 0x1p-53));
}

static void test_recurrence_variance_parameters(void)
{
    const double *model = fixture_phrase_parameters();
    double values[] = {1, 3};
    D positives = {values, 2};
    assert(recurrence_threshold(positives, model) == 2 + sqrt(2));

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_RECURRENCE_VARIANCE_MIN_COUNT] = 3;
    values[1] = 3;
    assert(recurrence_threshold(positives, parameters) == 4);

    parameters[TA_RECURRENCE_VARIANCE_MIN_COUNT] = model[TA_RECURRENCE_VARIANCE_MIN_COUNT];
    parameters[TA_RECURRENCE_VARIANCE_COUNT_BACK] = 0;
    values[1] = 3;
    assert(recurrence_threshold(positives, parameters) == 3);
}

static void test_recurrence_score_parameters(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double a[] = {1}, b[] = {0};
    D rows[] = {{a, 1}, {b, 1}}, scores[2];
    B related[2];
    int boundaries[] = {0, 1, 2};
    double zeros[2] = {0}, positive_value;
    Spectra spectra = {.rows = rows, .n = 2};
    I indices = {boundaries, 3};
    D means = {zeros, 2}, positives = {&positive_value, 0};
    Recurrence result = {.scores = scores, .related = related, .n = 2};
    recurrence_scores(c, spectra, indices, means, result, &positives, model);
    assert(scores[0].v[1] == 1 && positives.n == 1 && positive_value == 1);

    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_RECURRENCE_INVERSE_DISTANCE_NUMERATOR] = 2;
    parameters[TA_RECURRENCE_POSITIVE_SCORE_MIN_EXCLUSIVE] = 2;
    positives.n = 0;
    recurrence_scores(c, spectra, indices, means, result, &positives, parameters);
    assert(scores[0].v[1] == 2 && positives.n == 0);

    parameters[TA_RECURRENCE_MATCH_DISTANCE_MAX] = 1;
    recurrence_scores(c, spectra, indices, means, result, &positives, parameters);
    assert(scores[0].v[1] == model[TA_RECURRENCE_IDENTICAL_FRAME_SCORE] &&
           positives.n == 1 && positive_value == scores[0].v[1]);
}

void test_phrase_edges_classifier(ta_context *c)
{
    test_fade_reference_window();
    test_route_profile(c);
    test_slow_rock_profile(c);
    test_kick_mask_profile();
    test_mood_flag_model();
    test_compression_following_phase_limit();
    test_seed_geometry(c);
    test_mood_correspondence_grid(c);
    test_rhythm_secondary_votes();
    test_tempo_onset_shift(c);
    test_recurrence_distance_reduction();
    test_windows_recurrence_distance();
    test_recurrence_variance_parameters();
    test_recurrence_score_parameters(c);
    double low[1600], full[1600];
    for (int i = 0; i < 1600; i++) low[i] = full[i] = 1;
    int start = (1600 & ~15) - 528;
    for (int pair = 0; pair < 16; pair++)
        for (int j = 0; j < 32; j++) {
            double level = pair < 12 ? 1 : pow(0.1, pair - 11);
            full[start + pair * 32 + j] = level;
            low[start + pair * 32 + j] = level * 0.5;
        }
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, fixture_phrase_parameters(), sizeof parameters);
    assert(fade_out((D){ low, 1600 }, (D){ full, 1600 }, parameters));
    double power_ratio = parameters[TA_FADE_TAIL_POWER_RATIO];
    parameters[TA_FADE_TAIL_POWER_RATIO] = 0;
    assert(!fade_out((D){ low, 1600 }, (D){ full, 1600 }, parameters));
    parameters[TA_FADE_TAIL_POWER_RATIO] = power_ratio;
    full[start + 15 * 32] = 10;
    assert(!fade_out((D){ low, 1600 }, (D){ full, 1600 }, parameters));
    (void)c;
}

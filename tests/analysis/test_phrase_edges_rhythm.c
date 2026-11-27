/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_rhythm.c"
#include "phrase-model-fixture.h"
#include <assert.h>

static void test_attack_mark_threshold(void)
{
    const double *model = fixture_phrase_parameters();
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_ATTACK_MARK_LOW_BAR_RATIO] = 0.8;

    double original_peaks[16] = { 2, 1.5 };
    double adjusted_peaks[16] = { 2, 1.5 };
    uint8_t original[16] = { 0 }, adjusted[16] = { 0 };
    Attacks original_state = { .maximum = 1, .parameters = model };
    Attacks adjusted_state = { .maximum = 1, .parameters = changed };
    mark_attacks(&original_state, original_peaks, (B){ original, 16 }, 0);
    mark_attacks(&adjusted_state, adjusted_peaks, (B){ adjusted, 16 }, 0);
    assert(original[0] == 1 && adjusted[0] == 1);
    assert(original[1] == 1 && adjusted[1] == 0);
}

static void test_empty_attack_model(ta_context *c)
{
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, fixture_phrase_parameters(), sizeof changed);
    changed[TA_RHYTHM_BLOCK_EMPTY_ATTACK_WIDTH] = 2;
    Rhythm rhythm = { .first = rhythm_attack_buffer(c, 0, changed),
                      .second = rhythm_attack_buffer(c, 0, changed) };
    collect_rhythm_attacks(c, &rhythm, ta_new_bytes(c, 1), ta_new_bytes(c, 1), 1, 1, changed);
    assert(rhythm.first.n == 2 && rhythm.second.n == 2);
    assert(rhythm.first.v[1] == 0 && rhythm.second.v[1] == 0);
}

static void test_attack_delay_peak_routes(void)
{
    const double *model = fixture_phrase_parameters();
    double rising[] = { 0, 1, 2, 3, 4, 5, 4, 0 };
    delay_search search = { .envelope = { rising, 8 }, .length = 12, .barLength = 4,
                            .quarter = 4, .parameters = model };
    delay_peak peak = { 0 };
    assert(delay_candidate_valid(&search, 3, 3, 5));
    assert(locate_delay_peak(&search, 3, 3, &peak));
    assert(peak.position == 6 && peak.value == 5);
    assert(!locate_delay_peak(&search, 0, 0, &peak));
    assert(!locate_delay_peak(&search, 7, 7, &peak));

    double falling[] = { 0, 1, 6, 5, 4, 3, 2 };
    search.envelope = (D){ falling, 7 };
    assert(locate_delay_peak(&search, 3, 3, &peak));
    assert(peak.position == 2 && peak.value == 1);
    search.ascending = true;
    assert(!locate_delay_peak(&search, 3, 3, &peak));
}

static void test_attack_valley_recovery_rounding(void)
{
    const double *model = fixture_phrase_parameters();
    delay_search search = { .start = 20, .period = 16, .parameters = model };
    delay_valley valley = { .found = true, .position = 4, .value = 0x1.f19ef0d9bdc0dp-1 };
    double boundary = valley.value * model[TA_ATTACK_DELAY_RECOVERY_RATIO];
    assert(boundary / valley.value > model[TA_ATTACK_DELAY_RECOVERY_RATIO]);

    delay_peak peak = { .value = boundary };
    int result = 0;
    assert(advance_valley(&search, &valley, valley.value, peak, 10, &result) < 0);
    assert(result == 0);

    valley.run = 0;
    peak.value = nextafter(boundary, INFINITY);
    assert(advance_valley(&search, &valley, valley.value, peak, 10, &result) > 0);
    assert(result == 6);
}

static void test_attack_valley_search(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double levels[] = { 1, 1, 1, .1, .1, 1, 1, 1, 1, 1 };
    delay_search search = { .envelope = { levels, 10 }, .start = 1, .period = 16,
                            .barLength = 4, .parameters = model };
    delay_peak peak = { .position = 6, .value = 1 };
    int result = 0;
    assert(locate_delay_valley(c, &search, 7, 5, 1, peak, &result));
    assert(result == search.start);
}

static void test_attack_preserves_derivative_tail(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    D storage = ta_new_doubles(c, 4000);
    storage.v[3999] = 7;
    D envelope = align_attack_storage(c, &storage, 1, 4000, 2, model);
    assert(envelope.n == 4000 && envelope.v[3999] == 7);

    int coordinates[] = { 1, 1001, 2001, 3001 };
    AttackWindow window = { .start = 1, .period = 1000, .phase = 1 };
    double peaks[ATTACK_PEAK_CAPACITY] = { 0 };
    assert(attack_bar_peaks((I){ coordinates, 4 }, envelope, &window, 0, 250, peaks, model));
    assert(peaks[15] == 7);

    storage = ta_new_doubles(c, 4);
    storage.v[0] = 3;
    envelope = align_attack_storage(c, &storage, 1, 8, 5, model);
    assert(envelope.n == 7 && envelope.v[4] == 0 && envelope.v[5] == 3);
}

static void test_no_beat_attack_preserves_filter_state(ta_context *c)
{
    double samples[200] = { [199] = 6 };
    const double *model = fixture_phrase_parameters();
    ta_phrase_biquad identity = { .b0 = 1 };
    Attacks state = { .low = { identity }, .lowCount = 1, .delaySmoother = identity,
                      .parameters = model };
    B output = attacks_timing(c, &state, (D){ samples, 200 }, 2000, 120, 0, 1, 1);
    assert(output.n == 1 && !output.v[0] && state.previous == 6);
}

static void test_attack_rate_bounds(const double *model, int first, int end)
{
    for (int i = first; i < end; i += 2) {
        double low = model[i], high = model[i + 1];
        assert(!attack_rate_in_ranges(low, model, first, end));
        assert(!attack_rate_in_ranges(high, model, first, end));
        assert(attack_rate_in_ranges(nextafter(low, high), model, first, end));
        assert(attack_rate_in_ranges(nextafter(high, low), model, first, end));
    }
}

static void test_attack_rate_profiles(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_attack_rate_bounds(model, TA_ATTACK_FULL_FIRST_RATE_MIN, TA_ATTACK_REDUCED_FIRST_RATE_MIN);
    test_attack_rate_bounds(model, TA_ATTACK_REDUCED_FIRST_RATE_MIN, TA_ATTACK_FULL_FILTER_COUNT);
    double low = model[TA_ATTACK_REDUCED_THIRD_RATE_MIN], high = model[TA_ATTACK_REDUCED_THIRD_RATE_MAX];
    assert(attacks_init(c, nextafter(low, high), false).lowCount == 1);
    assert(attacks_init(c, 5512.5, false).lowCount == model[TA_ATTACK_FULL_FILTER_COUNT]);
}

static void test_rhythm_power_windows(ta_context *c)
{
    double samples[100];
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++)
        samples[i] = 1;
    int points[] = { 1, 22, 43, 64 };
    I coordinates = { points, sizeof points / sizeof *points };
    D source = { samples, sizeof samples / sizeof *samples };
    const double *model = fixture_phrase_parameters();
    Power full = rhythm_power(c, NULL, source, 20, 60, coordinates, coordinates.n, false, model);
    Power low = rhythm_power(c, NULL, source, 20, 60, coordinates, coordinates.n, true, model);
    assert(full.quarters.v[11] == 6 && full.quarters.v[15] == 4);
    assert(low.quarters.v[11] == 16 && low.quarters.v[15] == 16);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_RHYTHM_POWER_SCALE] = 2;
    Power adjusted = rhythm_power(c, NULL, source, 20, 60, coordinates, coordinates.n, true, changed);
    assert(adjusted.quarters.v[11] == 8);
}

static void test_hihat_island_gate(ta_context *c, int length, int remainder, const double *model)
{
    uint8_t outer = (uint8_t)model[TA_HIHAT_LIGHT_LEVEL], inner = (uint8_t)model[TA_HIHAT_MEDIUM_LEVEL];
    B bars = ta_new_bytes(c, length + 2);
    bars.v[0] = bars.v[length + 1] = outer;
    memset(bars.v + 1, inner, (size_t)length);
    smooth_hihat_levels(c, bars, remainder - 1, model);
    assert(bars.v[1] == inner && bars.v[length] == inner);
    smooth_hihat_levels(c, bars, remainder, model);
    assert(bars.v[1] == outer && bars.v[length] == outer);
}

static void test_hihat_changes(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_hihat_island_gate(c, (int)model[TA_HIHAT_ISLAND_FIRST_LENGTH],
                          (int)model[TA_HIHAT_ISLAND_FIRST_MIN_REMAINDER], model);
    test_hihat_island_gate(c, (int)model[TA_HIHAT_ISLAND_SECOND_LENGTH],
                          (int)model[TA_HIHAT_ISLAND_SECOND_MIN_REMAINDER], model);
    test_hihat_island_gate(c, (int)model[TA_HIHAT_ISLAND_THIRD_LENGTH],
                          (int)model[TA_HIHAT_ISLAND_THIRD_MIN_REMAINDER], model);

    int phase = 2, first = phase - (int)model[TA_HIHAT_PHASE_ORIGIN];
    int width = (int)model[TA_HIHAT_BEATS_PER_BAR];
    B counts = ta_new_bytes(c, first + 2 * width + 1);
    counts.v[0] = (uint8_t)model[TA_HIHAT_HEAVY_LEVEL];
    counts.v[first] = counts.v[first + 2 * width] = (uint8_t)(model[TA_HIHAT_SPARSE_MAX_COUNT] + 1);
    counts.v[first + width] = (uint8_t)(model[TA_HIHAT_LIGHT_MAX_COUNT] + 1);
    B changes = hihat_changes(c, (B){ counts.v, counts.n - 1 }, phase, model);
    assert(changes.n == 2 && changes.v[1] == model[TA_HIHAT_CHANGE_MARK]);
    changes = hihat_changes(c, counts, phase, model);
    assert(changes.n == 3 && changes.v[1] == 0 && changes.v[2] == 0);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIHAT_ISLAND_FIRST_MIN_REMAINDER]++;
    changes = hihat_changes(c, counts, phase, changed);
    assert(changes.v[1] == changed[TA_HIHAT_CHANGE_MARK] && changes.v[2] == changed[TA_HIHAT_CHANGE_MARK]);
}

static void test_count_hihat_beat(void)
{
    uint8_t counts[3] = { 0 };
    int positions[] = { 10, 20, 30 };
    B result = { counts, 3 };
    I beats = { positions, 3 };
    const double *model = fixture_phrase_parameters();
    count_hihat_beat(result, beats, 0, 9, model);
    count_hihat_beat(result, beats, 3, 9, model);
    assert(counts[0] == 0 && counts[1] == 0 && counts[2] == 0);
    count_hihat_beat(result, beats, 3, 10, model);
    count_hihat_beat(result, beats, 3, 19, model);
    count_hihat_beat(result, beats, 3, 20, model);
    count_hihat_beat(result, beats, 3, 30, model);
    count_hihat_beat(result, beats, 3, 300, model);
    assert(counts[0] == 2 && counts[1] == 1 && counts[2] == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIHAT_BEAT_HIT_INCREMENT] = 2;
    count_hihat_beat(result, beats, 3, 29, changed);
    count_hihat_beat(result, beats, 3, 30, changed);
    assert(counts[0] == 2 && counts[1] == 3 && counts[2] == 4);
}

void test_phrase_edges_rhythm(ta_context *c)
{
    test_attack_mark_threshold();
    test_empty_attack_model(c);
    test_attack_delay_peak_routes();
    test_attack_valley_recovery_rounding();
    test_attack_valley_search(c);
    test_attack_preserves_derivative_tail(c);
    test_no_beat_attack_preserves_filter_state(c);
    test_attack_rate_profiles(c);
    test_rhythm_power_windows(c);
    test_hihat_changes(c);
    test_count_hihat_beat();
}

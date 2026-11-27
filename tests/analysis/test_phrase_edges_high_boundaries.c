/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_high_boundaries.c"
#include <assert.h>
#include "phrase-model-fixture.h"
#undef high_classify

static void test_anchor_distance(ta_context *c)
{
    int positions[] = { 1, 5, 30, 36, 44 };
    int anchor = 40;
    I candidates = { positions, 5 }, anchors = { &anchor, 1 };
    const double *model = fixture_phrase_parameters();
    I result = finish_candidates(c, candidates, 1, anchors, 1, 96, model);
    const int expected[] = { 1, 5, 30, 40 };
    assert(result.n == 4 && !memcmp(result.v, expected, sizeof expected));

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_ANCHOR_FINAL_EXCLUSION_DISTANCE] = 2;
    result = finish_candidates(c, candidates, 1, anchors, 1, 96, modified);
    const int expanded[] = { 1, 5, 30, 36, 40, 44 };
    assert(result.n == 6 && !memcmp(result.v, expanded, sizeof expanded));
}

static void test_candidate_pruning_profile(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double broad[16], bass[16];
    for (int i = 0; i < 16; ++i) {
        broad[i] = 0.6;
        bass[i] = 300;
    }
    int positions[] = { 1, 13, 49 };
    I candidates = { positions, 3 };
    D all = { broad, 16 }, low = { bass, 16 };
    I result = adjust_high_candidates(c, candidates, all, low, model);
    assert(result.n == 2 && result.v[1] == 13);

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_HIGH_PRUNE_TAIL_BEATS] = 12;
    modified[TA_HIGH_PRUNE_MODERATE_ALL_MAX] = 0.61;
    result = adjust_high_candidates(c, candidates, all, low, modified);
    assert(result.n == 1 && result.v[0] == 1);
    bass[11] = 301;
    result = adjust_high_candidates(c, candidates, all, low, modified);
    assert(result.n == 2 && result.v[1] == 49);
}

static void test_repeat_rhythm_profile(ta_context *c)
{
    uint8_t beats[1024] = { 0 };
    memset(beats + 267, 1, 40);
    int positions[] = { 1, 33, 65, 97 };
    I candidates = { positions, 4 };
    B rhythm = { beats, 1024 };
    const double *model = fixture_phrase_parameters();
    RhythmRejection result = reject_repeated_rhythm(c, candidates, rhythm, model);
    assert(result.scores.n == 3 && result.scores.v[0] == 1 && result.scores.v[1] == 0.5);
    assert(result.candidates.n == 3 && result.candidates.v[1] == 65);

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_REPEAT_RHYTHM_FIRST_END] = 7;
    result = reject_repeated_rhythm(c, candidates, rhythm, modified);
    assert(result.scores.n == 3 && result.scores.v[0] == 41 * 0.0125);
    modified[TA_REPEAT_RHYTHM_SPACING] = 16;
    result = reject_repeated_rhythm(c, candidates, rhythm, modified);
    assert(result.scores.n == 0 && result.candidates.n == 4);
}

static void test_genre_snap_profile(void)
{
    double levels[] = { 1, 1, 1 }, contrast[] = { 2, 1.4, 2 }, direction[] = { 1, 0.5, 1.8 };
    RatioSeries low = { .levels = { levels, 3 }, .contrast = { contrast, 3 },
                        .direction = { direction, 3 } };
    int position = 2;
    bool snapped = false;
    const double *model = fixture_phrase_parameters();
    snap_genre_addition(&position, 3, low, &snapped, model);
    assert(position == 3 && snapped);
    assert(shift_genre_boundary(7, 5, 0.8, 1.2, model));

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_GENRE_SNAP_CONTRAST_MIN] = 1.3;
    modified[TA_GENRE_SNAP_DIRECTION_MAX] = 0.4;
    modified[TA_GENRE_SNAP_PHASE_COMBINED_MIN] = 0.99;
    position = 2;
    snapped = false;
    snap_genre_addition(&position, 3, low, &snapped, modified);
    assert(position == 2 && !snapped);
    assert(!shift_genre_boundary(7, 5, 0.8, 1.2, modified));
}

static void test_verse_score_bar_rounding(ta_context *c)
{
    double beats[12], flat[8], changes[12] = { 0 };
    double low[] = { 1, 1, 4, 4 };
    for (size_t i = 0; i < sizeof beats / sizeof *beats; i++) beats[i] = 1;
    for (size_t i = 0; i < sizeof flat / sizeof *flat; i++) flat[i] = 1;
    VerseProfile profile = { .lowPeaks = { beats, 12 }, .low = { low, 4 }, .middle = { flat, 8 },
                             .opening = { beats, 12 }, .all = { flat, 8 }, .changes = { changes, 12 } };
    const double *model = fixture_phrase_parameters();
    assert(verse_score(c, 10, &profile, 1, model) == 5);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_VERSE_SCORE_BAR_BIAS] = 0;
    assert(verse_score(c, 10, &profile, 1, changed) == 8);
}

static void test_band_change_scan_profile(void)
{
    const double *model = fixture_phrase_parameters();
    double samples[12];
    for (int i = 0; i < 12; i++) samples[i] = 1;
    samples[4] = samples[8] = 100;
    D levels = { samples, 12 };
    int seed = 1, found[2] = { 0 };
    I candidates = { &seed, 1 };
    assert(collect_changes(candidates, levels, levels, levels, levels, levels, 1, 2, seed, 1, found, model) == 2);
    assert(found[0] == 5 && found[1] == 9);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BAND_CHANGE_FIRST_STATE] = 0;
    assert(collect_changes(candidates, levels, levels, levels, levels, levels, 1, 2, seed, 1, found, changed) == 1);
}

static void test_block_mean_scale(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double values[] = { 2, 2, 2, 2, 4, 4, 4, 4 };
    D power = { values, 8 };
    int start = 1;
    I boundaries = { &start, 1 };
    BlockMeans means = block_means(c, boundaries, power, power, power, 1, model);
    assert(means.quarterPower.n == 2 && means.quarterPower.v[0] == 2 && means.quarterPower.v[1] == 4);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_BLOCK_MEAN_SCALE] *= .5;
    means = block_means(c, boundaries, power, power, power, 1, changed);
    assert(means.quarterPower.v[0] == 1 && means.quarterPower.v[1] == 2);
}

static void test_high_classification_role_codes(ta_context *c)
{
    double intro[] = { 1, 4, 1 }, low[] = { 1, 8, 1 };
    int positions[] = { 1, 33, 65 };
    I boundaries = { positions, 3 };
    D intro_means = { intro, 3 }, low_means = { low, 3 };
    const double *model = fixture_phrase_parameters();
    HighRoles result = high_classify(c, boundaries, intro_means, low_means, model);
    assert(result.roles.v[0] == 10 && result.roles.v[1] == 40 && result.roles.v[2] == 60);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIGH_ROLE_HOOK] = 47;
    result = high_classify(c, boundaries, intro_means, low_means, changed);
    assert(result.roles.v[0] == 10 && result.roles.v[1] == 47 && result.roles.v[2] == 60);
}

static void test_high_short_role_dispatch(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    double values[] = {1, 4};
    int positions[] = {1, 33};
    D means = {values, 2};
    I boundaries = {positions, 2};
    HighRoles pair = high_classify(c, boundaries, means, means, model);
    assert(pair.simple && pair.roles.v[0] == model[TA_HIGH_TWO_SECTION_INTRO] &&
           pair.roles.v[1] == model[TA_HIGH_TWO_SECTION_OUTRO]);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_HIGH_SHORT_SIMPLE_FLAG] = 0;
    boundaries.n = means.n = 1;
    HighRoles single = high_classify(c, boundaries, means, means, changed);
    assert(!single.simple && single.roles.v[0] == model[TA_HIGH_SINGLE_SECTION_INTRO]);
}

static void test_low_peak_evidence_profile(void)
{
    double aligned[] = {0, 2, 0, 0};
    uint8_t flags[] = {0};
    D changes = {aligned, 4};
    B bars = {flags, 1};
    const double *model = fixture_phrase_parameters();
    assert(!low_peak_eligible(2, changes, bars, 4, model));

    double modified[TA_PHRASE_PARAMETER_COUNT];
    memcpy(modified, model, sizeof modified);
    modified[TA_LOW_PEAK_CHANGE_MARK] = 2;
    assert(low_peak_eligible(2, changes, bars, 4, modified));
    modified[TA_LOW_PEAK_PRIOR_CHANGE_BACK] = 2;
    assert(!low_peak_eligible(2, changes, bars, 4, modified));
    flags[0] = 1;
    assert(low_peak_eligible(2, changes, bars, 4, modified));
}

void test_phrase_edges_high_boundaries(ta_context *c)
{
    test_low_peak_evidence_profile();
    test_anchor_distance(c);
    test_candidate_pruning_profile(c);
    test_repeat_rhythm_profile(c);
    test_genre_snap_profile();
    test_verse_score_bar_rounding(c);
    test_band_change_scan_profile();
    test_block_mean_scale(c);
    test_high_classification_role_codes(c);
    test_high_short_role_dispatch(c);
    const double *parameters = fixture_phrase_parameters();
    double changes[160] = { 0 };
    uint8_t flagValues[40] = { 0 };
    D ch = { changes, 160 };
    B flags = { flagValues, 40 };
    assert(!irregular_eligible(1, ch, 1, flags, parameters));
    changes[5] = 1;
    assert(irregular_eligible(6, ch, 1, flags, parameters));
    int selected = 1, second = 0;
    ChangeEvidence h = { .first = 5, .firstAligned = 9, .second = 13, .secondAligned = 17 };
    first_irregular_change(c, h, ch, 1, flags, 1, &selected, &second, parameters);
    assert(selected == 5 && second == 0);
    second_irregular_change(c, h, ch, 1, flags, 1, &selected, &second, parameters);
    assert(selected == 5 && second == 13);
    selected = 21;
    second = 0;
    first_irregular_change(c, h, ch, 1, flags, 1, &selected, &second, parameters);
    assert(selected == 21 && second == 5);
    selected = 1;
    second = 0;
    second_irregular_change(c, h, ch, 1, flags, 1, &selected, &second, parameters);
    assert(selected == 13 && second == 0);
    I seeds = irregular_seeds(c, 1, ch, 1, flags, 1, (BreakEvidence){ .start = 5, .end = 9, .deep = true }, h, parameters);
    assert(seeds.n == 3 && seeds.v[0] == 1 && seeds.v[1] == 9 && seeds.v[2] == 5);
    for (int i = 0; i < 160; i++) changes[i] = 1;
    assert(offset_irregular_seed(c, 5, ch, 1, flags, parameters) == 5);
    seeds = irregular_seeds(c, 1, ch, 1, flags, 5, (BreakEvidence){ 0 }, (ChangeEvidence){ 0 }, parameters);
    assert(seeds.n == 1 && seeds.v[0] == 1);
    int positions[] = { 1, 9, 9, 17 };
    I list = { positions, 4 };
    remove_rhythm_position(&list, 9);
    assert(list.n == 2 && list.v[0] == 1 && list.v[1] == 17);
    double low[24], middle[24], all[24];
    for (int i = 0; i < 24; i++) low[i] = middle[i] = all[i] = 1;
    low[5] = 3;
    middle[5] = 2;
    all[5] = 2;
    BreakEvidence recovery = { .deep = true };
    assert(break_recovery((D){ low, 24 }, (D){ middle, 24 }, (D){ all, 24 }, 1, 9, &recovery, parameters));
    assert(recovery.start == 9 && recovery.end == 13 && !recovery.deep);
    assert(!break_recovery((D){ low, 24 }, (D){ middle, 24 }, (D){ all, 24 }, 21, 9, &recovery, parameters));
}

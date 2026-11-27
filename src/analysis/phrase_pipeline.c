/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static int candidate_beat_count(D normalized)
{
    int count = 0;
    for (int i = 0; i < normalized.n; i++)
        count += normalized.v[i] != 0;
    return count;
}

static I genre1(ta_context *c, Beats b, Opening opening, D envelope, B hihat, B rhythm,
                double duration, I anchors, const double *parameters) {
    int phase = b.timing.phase;
    I low =
        low_peak_candidates(c, opening.selected, b.lowPeakChanges, phase, b.lowActivity.changes, parameters);
    int longGap = 0;
    for (int i = 0; i < low.n; i++) {
        int end = i + 1 < low.n ? low.v[i + 1] : b.lowPeakChanges.n;
        if (end - low.v[i] >= parameters[TA_GENRE1_LONG_GAP_MIN]) {
            longGap = low.v[i];
            break;
        }
    }
    BreakEvidence br = break_evidence(c, low, phase, b.lowActivity.values, b.middleActivity.values,
                                      b.allActivity.bars, parameters);
    ChangeEvidence ch =
        change_evidence(c, low, phase, b.lowActivity.values, b.middleActivity.values,
                        b.allActivity.bars, envelope, b.lowPeaks, parameters);
    double opening_only_limit = parameters[TA_GENRE1_OPENING_ONLY_LIMIT];
    I seeds = longGap < opening_only_limit && br.start < opening_only_limit &&
              ch.firstAligned < opening_only_limit
                  ? i_copy(c, (I){&opening.selected, 1})
                  : irregular_seeds(c, opening.selected, b.lowPeakChanges, phase,
                                    b.lowActivity.changes, longGap, br, ch, parameters);
    VerseProfile profile = {b.lowPeaks, b.lowActivity.values, b.middleActivity.values,
                            envelope,   b.allActivity.bars,   b.lowPeakChanges};
    I all = all_band_candidates(c, reconcile_high_seeds(c, seeds, anchors, parameters), phase, b.lowPeakChanges,
                                b.lowActivity.changes, b.middleActivity.changes, hihat, profile, parameters);
    I adjusted = adjust_high_candidates(c, all, b.allActivity.bars, b.lowActivity.values, parameters),
      selected = adjusted.n <= duration * parameters[TA_GENRE1_CANDIDATES_PER_SECOND]
                     ? adjusted
                     : reject_repeated_rhythm(c, adjusted, rhythm, parameters).candidates;
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, b.coordinates.n, phase, parameters);
    CandidateFeatures features = candidate_features(c, b.lowPeaks, b.lowActivity.values,
                                                    middle.normalized, middle.bars, phase, parameters);
    I secondary =
        verse_hook_candidates(c, features, opening.hypotheses, opening.selected, phase,
                              (int)parameters[TA_HIGH_CANDIDATE_ROUTE], parameters);
    int count = candidate_beat_count(middle.normalized);
    int bars = count / (int)parameters[TA_HIGH_CANDIDATE_BAR_DIVISOR];
    HighProfile high = normalize_high(c, b.eighths, phase, parameters);
    I breaks = short_breaks(c, b.lowLevels, middle.normalized, count, bars, parameters),
      snares = snare_candidates(c, high.values, bars, parameters);
    return add_genre1(c, i_join(c, (I){&b.initial, 1}, selected), secondary, features.lowLevel,
                      breaks, snares, phase, parameters);
}
static I genre23(ta_context *c, Beats b, Opening opening, D envelope, D vocal, int route,
                 const double *parameters) {
    int phase = b.timing.phase;
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, b.coordinates.n, phase, parameters);
    CandidateFeatures features = candidate_features(c, b.lowPeaks, b.lowActivity.values,
                                                    middle.normalized, middle.bars, phase, parameters);
    I base = verse_hook_candidates(c, features, opening.hypotheses, opening.selected, phase, route, parameters);
    int count = candidate_beat_count(middle.normalized);
    int bars = count / (int)parameters[TA_OTHER_CANDIDATE_BAR_DIVISOR];
    HighProfile high = normalize_high(c, b.eighths, phase, parameters);
    I snare = snare_candidates(c, high.values, bars, parameters), noise = noise_changes(c, envelope, parameters),
      voice = vocal_entries(c, vocal, parameters),
      breaks = short_breaks(c, b.lowLevels, middle.normalized, count, bars, parameters);
    I extra = i_compact(c, i_join(c, i_join(c, breaks, noise), i_join(c, snare, voice)), true);
    return adjust_candidates(c, base, extra, phase, bars, parameters);
}
I phrase_candidates(ta_context *c, Beats b, Opening opening, D envelope, D vocal, B hihat, B rhythm,
                    double duration, int route, const double *parameters) {
    if (!opening.found)
        return i_copy(c, (I){&b.initial, 1});
    I anchors = secure_points(c, b.lowPeaks, b.timing.phase, parameters).beats;
    I proposals = route == parameters[TA_HIGH_CANDIDATE_ROUTE]
                      ? genre1(c, b, opening, envelope, hihat, rhythm, duration, anchors, parameters)
                      : genre23(c, b, opening, envelope, vocal, route, parameters);
    return finish_candidates(c, proposals, b.initial, anchors, route,
                             ta_min(b.middleLevels.n, envelope.n), parameters);
}
PhraseExport phrase_classify(ta_context *c, Samples samples, Timing timing, const double *phrase_parameters) {
    ta_check(c);
    int rhythmType = 0;
    SnarePositions snares = genre_snares(c, samples, timing, &rhythmType, phrase_parameters);
    ta_profile_mark(c, "timing");
    Beats b = phrase_beats(c, samples, timing, phrase_parameters);
    int n = b.coordinates.n, phase = timing.phase;
    ta_profile_mark(c, "beat_features");
    Rhythm r = phrase_rhythm(c, samples, timing, b.coordinates, phrase_parameters);
    ta_check(c);
    ta_profile_mark(c, "rhythm");
    /* Match the producer's retained middle-band extent, including its last
       complete processing block, before constructing the opening spectrum. */
    double mid_rate = samples.rate * phrase_parameters[TA_DECIMATE_MIDDLE_RATE_SCALE];
    Spectra spectrum =
        opening_spectra(c, d_view(c, samples.middle, 0, r.middleExtent), mid_rate, phrase_parameters);
    ta_profile_mark(c, "opening_cqt");
    int first = opening_first_frame(c, mid_rate, timing, phrase_parameters);
    OpeningFeatures features = opening_features(c, spectrum, first, n, phrase_parameters);
    spectra_release(c, spectrum);
    ta_profile_mark(c, "opening_features");
    Spectra vocalSpectrum = phrase_spectra(c, samples, timing, b.coordinates, false, phrase_parameters);
    ta_profile_mark(c, "vocal_fft");
    D vocal = vocal_activity(c, vocalSpectrum, phrase_parameters);
    spectra_release(c, vocalSpectrum);
    LowPower power = classify_power(c, r.lowPower, r.allPower, n, phrase_parameters);
    Compression compression =
        classify_compression(c, r.lowPower, r.allPower, r.first, phase, phrase_parameters);
    int slow = slow_tempo(c, mid_rate, timing.bpm, power.powers, snares, r.finalMiddle,
                          timing.offset, timing.offset, phrase_parameters);
    RhythmFlags flags = rhythm_flags(c, r.first, timing.bpm, power.powers, phrase_parameters);
    MoodInputs inputs = {0};
    inputs.slowRock = slow_rock(c, r.first, r.second, phrase_parameters);
    inputs.slowTempo = slow;
    inputs.lowPowerRatio = power.ratio;
    inputs.popRock = flags.popRock;
    inputs.alternateRock = flags.alternateRock;
    inputs.sparseKick = flags.sparseKick;
    inputs.fourKick = flags.fourKick;
    inputs.fadeOut = fade_out(r.lowPower, r.allPower, phrase_parameters);
    bpm_flags(timing.bpm, &inputs.narrowBPM, &inputs.broadBPM, phrase_parameters);
    inputs.typicalGenre = timing.bpm >= phrase_parameters[TA_BPM_TYPICAL_MIN] &&
                          timing.bpm <= phrase_parameters[TA_BPM_TYPICAL_MAX] &&
                          rhythmType == phrase_parameters[TA_TYPICAL_RHYTHM_TYPE];
    inputs.compression = compression.second;
    int mood = revise_mood(c, initial_mood(inputs, phrase_parameters), n, b.lowPeaks, b.quarters,
                           phrase_parameters),
        route = (int)phrase_parameters[mood == MOOD_HIGH ? TA_HIGH_CANDIDATE_ROUTE : TA_OTHER_CANDIDATE_ROUTE];
    Anchors anchors = secure_points(c, b.lowPeaks, phase, phrase_parameters);
    double duration = samples.frames / samples.rate;
    if (mood == MOOD_MID) {
        Alternatives alternatives = initial_alternatives(c, anchors, phase, phrase_parameters);
        I loops = {0}, rhythms = {0};
        if (alternatives.shortList.beats.n) {
            Candidates a = alternatives.shortList;
            BlockMeans blocks =
                block_means(c, a.beats, features.envelope, b.lowPeaks, b.quarters, phase, phrase_parameters);
            loops =
                simple_loops(c, a.beats, a.roles, r.dividedLow, r.dividedMiddle, blocks.low,
                             phrase_parameters).roles;
            a = alternatives.longList;
            rhythms = decorate_rhythm(c, a.beats, a.roles, r.second, phrase_parameters).roles;
        }
        Route selected = phrase_route(c, b.lowPeaks, phase, duration, loops, rhythms, mood, phrase_parameters);
        mood = selected.mood;
        route = selected.route;
    }
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, n, phase, phrase_parameters);
    int zero = 0;
    Opening opening = opening_main(c, b.lowPeaks, b.lowActivity.values, middle.bars, vocal, phase,
                                   anchors.opening.n ? anchors.opening : (I){&zero, 1},
                                   phrase_parameters);
    I boundaries = phrase_candidates(c, b, opening, features.envelope, vocal, r.hiHatChanges,
                                     r.first, duration, route, phrase_parameters);
    ta_check(c);
    if (mood == MOOD_HIGH) {
        BlockMeans blocks =
            block_means(c, boundaries, features.envelope, b.lowPeaks, b.quarters, phase, phrase_parameters);
        HighRoles roles = high_classify(c, boundaries, blocks.intro, blocks.low, phrase_parameters);
        FillProfile profile = {b.lowPeaks, b.quarterMean, b.eighths, r.allPower, phase, b.quarters};
        Decorated decorated = decorate_high(c, boundaries, roles.roles, profile, r.dividedLow,
                                            r.dividedMiddle, blocks.low, r.second, phrase_parameters);
        PhraseExport output = phrase_convert(c, mood, decorated, (Variation){!roles.simple, 0}, phrase_parameters);
        ta_profile_mark(c, "classification_export");
        return output;
    }
    ta_profile_mark(c, "classification");
    Spectra recurrent = phrase_spectra(c, samples, timing, b.coordinates, true, phrase_parameters);
    ta_profile_mark(c, "recurrence_fft");
    PhraseExport out =
        export_non_high(c, mood, boundaries, recurrent, phase, n, b.middleActivity.values,
                        features.envelope, vocal, b.lowPeaks, r.second, b.quarters, b.quarters,
                        phrase_parameters);
    spectra_release(c, recurrent);
    ta_profile_mark(c, "classification_export");
    return out;
}

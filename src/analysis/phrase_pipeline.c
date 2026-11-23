/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static I genre1(ta_context *c, Beats b, Opening opening, D envelope, B hihat, B rhythm,
                double duration, I anchors) {
    int phase = b.timing.phase;
    I low =
        low_peak_candidates(c, opening.selected, b.lowPeakChanges, phase, b.lowActivity.changes);
    int longGap = 0;
    for (int i = 0; i < low.n; i++) {
        int end = i + 1 < low.n ? low.v[i + 1] : b.lowPeakChanges.n;
        if (end - low.v[i] >= 113) {
            longGap = low.v[i];
            break;
        }
    }
    BreakEvidence br = break_evidence(c, low, phase, b.lowActivity.values, b.middleActivity.values,
                                      b.allActivity.bars);
    ChangeEvidence ch =
        change_evidence(c, low, phase, b.lowActivity.values, b.middleActivity.values,
                        b.allActivity.bars, envelope, b.lowPeaks);
    I seeds = longGap < 2 && br.start < 2 && ch.firstAligned < 2
                  ? i_copy(c, (I){&opening.selected, 1})
                  : irregular_seeds(c, opening.selected, b.lowPeakChanges, phase,
                                    b.lowActivity.changes, longGap, br, ch);
    VerseProfile profile = {b.lowPeaks, b.lowActivity.values, b.middleActivity.values,
                            envelope,   b.allActivity.bars,   b.lowPeakChanges};
    I all = all_band_candidates(c, reconcile_high_seeds(c, seeds, anchors), phase, b.lowPeakChanges,
                                b.lowActivity.changes, b.middleActivity.changes, hihat, profile);
    I adjusted = adjust_high_candidates(c, all, b.allActivity.bars, b.lowActivity.values),
      selected = adjusted.n <= duration * 0.052000000000000005
                     ? adjusted
                     : reject_repeated_rhythm(c, adjusted, rhythm).candidates;
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, b.coordinates.n, phase);
    CandidateFeatures features = candidate_features(c, b.lowPeaks, b.lowActivity.values,
                                                    middle.normalized, middle.bars, phase);
    I secondary =
        verse_hook_candidates(c, features, opening.hypotheses, opening.selected, phase, 1);
    int count = 0;
    for (int i = 0; i < middle.normalized.n; i++)
        count += middle.normalized.v[i] != 0;
    int bars = count / 4;
    HighProfile high = normalize_high(c, b.eighths, phase);
    I breaks = short_breaks(c, b.lowLevels, middle.normalized, count, bars),
      snares = snare_candidates(c, high.values, bars);
    return add_genre1(c, i_join(c, (I){&b.initial, 1}, selected), secondary, features.lowLevel,
                      breaks, snares, phase);
}
static I genre23(ta_context *c, Beats b, Opening opening, D envelope, D vocal, int route) {
    int phase = b.timing.phase;
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, b.coordinates.n, phase);
    CandidateFeatures features = candidate_features(c, b.lowPeaks, b.lowActivity.values,
                                                    middle.normalized, middle.bars, phase);
    I base = verse_hook_candidates(c, features, opening.hypotheses, opening.selected, phase, route);
    int count = 0;
    for (int i = 0; i < middle.normalized.n; i++)
        count += middle.normalized.v[i] != 0;
    int bars = count / 4;
    HighProfile high = normalize_high(c, b.eighths, phase);
    I snare = snare_candidates(c, high.values, bars), noise = noise_changes(c, envelope),
      voice = vocal_entries(c, vocal),
      breaks = short_breaks(c, b.lowLevels, middle.normalized, count, bars);
    I extra = i_compact(c, i_join(c, i_join(c, breaks, noise), i_join(c, snare, voice)), true);
    return adjust_candidates(c, base, extra, phase, bars);
}
I phrase_candidates(ta_context *c, Beats b, Opening opening, D envelope, D vocal, B hihat, B rhythm,
                    double duration, int route) {
    if (!opening.found)
        return i_copy(c, (I){&b.initial, 1});
    I anchors = secure_points(c, b.lowPeaks, b.timing.phase).beats;
    I proposals = route == 1 ? genre1(c, b, opening, envelope, hihat, rhythm, duration, anchors)
                             : genre23(c, b, opening, envelope, vocal, route);
    return finish_candidates(c, proposals, b.initial, anchors, route,
                             ta_min(b.middleLevels.n, envelope.n));
}
PhraseExport phrase_classify(ta_context *c, Samples samples, Timing timing) {
    ta_check(c);
    int rhythmType = 0;
    SnarePositions snares = genre_snares(c, samples, timing, &rhythmType);
    ta_profile_mark(c, "timing");
    Beats b = phrase_beats(c, samples, timing);
    int n = b.coordinates.n, phase = timing.phase;
    ta_profile_mark(c, "beat_features");
    Rhythm r = phrase_rhythm(c, samples, timing, b.coordinates);
    ta_check(c);
    ta_profile_mark(c, "rhythm");
    /* Match the producer's retained middle-band extent, including its last
       complete processing block, before constructing the opening spectrum. */
    Spectra spectrum =
        opening_spectra(c, d_view(c, samples.middle, 0, r.middleExtent), samples.rate / 8);
    ta_profile_mark(c, "opening_cqt");
    int first = (int)ceil((samples.rate / 8 * 0.00390625) * ((phase - 1) * 60 / timing.bpm));
    OpeningFeatures features = opening_features(c, spectrum, first, n);
    spectra_release(c, spectrum);
    ta_profile_mark(c, "opening_features");
    Spectra vocalSpectrum = phrase_spectra(c, samples, timing, b.coordinates, false);
    ta_profile_mark(c, "vocal_fft");
    D vocal = vocal_activity(c, vocalSpectrum);
    spectra_release(c, vocalSpectrum);
    LowPower power = classify_power(c, r.lowPower, r.allPower, n);
    Compression compression = classify_compression(c, r.lowPower, r.allPower, r.first, phase);
    int slow = slow_tempo(c, samples.rate / 8, timing.bpm, power.powers, snares, r.finalMiddle,
                          timing.offset, timing.offset);
    RhythmFlags flags = rhythm_flags(c, r.first, timing.bpm, power.powers);
    MoodInputs inputs = {0};
    inputs.slowRock = slow_rock(c, r.first, r.second);
    inputs.slowTempo = slow;
    inputs.lowPowerRatio = power.ratio;
    inputs.popRock = flags.popRock;
    inputs.alternateRock = flags.alternateRock;
    inputs.sparseKick = flags.sparseKick;
    inputs.fourKick = flags.fourKick;
    inputs.fadeOut = fade_out(r.lowPower, r.allPower);
    bpm_flags(timing.bpm, &inputs.narrowBPM, &inputs.broadBPM);
    inputs.typicalGenre = timing.bpm >= 96.1 && timing.bpm <= 105.01 && rhythmType == 1;
    inputs.compression = compression.second;
    int mood = revise_mood(c, initial_mood(inputs), n, b.lowPeaks, b.quarters),
        route = mood == MOOD_HIGH ? 1 : 2;
    Anchors anchors = secure_points(c, b.lowPeaks, phase);
    double duration = samples.frames / samples.rate;
    if (mood == MOOD_MID) {
        Alternatives alternatives = initial_alternatives(c, anchors, phase);
        I loops = {0}, rhythms = {0};
        if (alternatives.shortList.beats.n) {
            Candidates a = alternatives.shortList;
            BlockMeans blocks =
                block_means(c, a.beats, features.envelope, b.lowPeaks, b.quarters, phase);
            loops =
                simple_loops(c, a.beats, a.roles, r.dividedLow, r.dividedMiddle, blocks.low).roles;
            a = alternatives.longList;
            rhythms = decorate_rhythm(c, a.beats, a.roles, r.second).roles;
        }
        Route selected = phrase_route(c, b.lowPeaks, phase, duration, loops, rhythms, mood);
        mood = selected.mood;
        route = selected.route;
    }
    AllActivity middle = middle_peak_activity(c, b.middlePeaks, n, phase);
    int zero = 0;
    Opening opening = opening_main(c, b.lowPeaks, b.lowActivity.values, middle.bars, vocal, phase,
                                   anchors.opening.n ? anchors.opening : (I){&zero, 1});
    I boundaries = phrase_candidates(c, b, opening, features.envelope, vocal, r.hiHatChanges,
                                     r.first, duration, route);
    ta_check(c);
    if (mood == MOOD_HIGH) {
        BlockMeans blocks =
            block_means(c, boundaries, features.envelope, b.lowPeaks, b.quarters, phase);
        HighRoles roles = high_classify(c, boundaries, blocks.intro, blocks.low);
        FillProfile profile = {b.lowPeaks, b.quarterMean, b.eighths, r.allPower, phase, b.quarters};
        Decorated decorated = decorate_high(c, boundaries, roles.roles, profile, r.dividedLow,
                                            r.dividedMiddle, blocks.low, r.second);
        PhraseExport output = phrase_convert(c, mood, decorated, (Variation){!roles.simple, 0});
        ta_profile_mark(c, "classification_export");
        return output;
    }
    ta_profile_mark(c, "classification");
    Spectra recurrent = phrase_spectra(c, samples, timing, b.coordinates, true);
    ta_profile_mark(c, "recurrence_fft");
    PhraseExport out =
        export_non_high(c, mood, boundaries, recurrent, phase, n, b.middleActivity.values,
                        features.envelope, vocal, b.lowPeaks, r.second, b.quarters, b.quarters);
    spectra_release(c, recurrent);
    ta_profile_mark(c, "classification_export");
    return out;
}

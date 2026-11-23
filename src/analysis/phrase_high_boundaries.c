/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double nonzero(double x) { return x == 0 ? 0.003 : x; }
static bool contains(I a, int x)
{
    for (int i = 0; i < a.n; i++)
        if (a.v[i] == x)
            return true;
    return false;
}
static D repeat4(ta_context *c, D a, int n)
{
    D r = ta_new_doubles(c, n * 4);
    for (int i = 0; i < n * 4; i++)
        r.v[i] = a.v[i / 4];
    return r;
}
static bool low_peak_eligible(int i, int extent, D aligned, B flags)
{
    return i >= 1 && i < extent &&
           (aligned.v[i] == 1 || aligned.v[i - 1] == 1 || flags.v[i / 4] == 1);
}

static bool peak_input_ready(int count, int phase, D changes)
{
    return count && phase >= 1 && phase <= changes.n;
}

I low_peak_candidates(ta_context *c, int intro, D changes, int phase, B flags)
{
    int extent = flags.n * 4;
    if (!peak_input_ready(extent, phase, changes))
        return (I){ 0 };
    D aligned = ta_new_doubles(c, extent);
    for (int i = 0; i < ta_min(extent, changes.n - phase + 1); i++)
        aligned.v[i] = changes.v[i + phase - 1];
    I result = ta_new_ints(c, extent + 1);
    result.n = 1;
    result.v[0] = intro;
    int last = intro, attempt = 1;
    const int gaps[] = { 32, 64, 16, 96, 48, 80 };
    for (;;) {
        int gap = attempt <= 6 ? gaps[attempt - 1] : attempt * 16, proposed = last + gap;
        if (proposed < extent) {
            int i = proposed - phase;
            if (low_peak_eligible(i, extent, aligned, flags)) {
                result.v[result.n++] = proposed;
                last = proposed;
                attempt = 1;
            } else
                attempt++;
        } else
            attempt++;
        if (proposed >= extent - 4 && attempt >= 7)
            break;
    }
    ta_release(c, aligned.v);
    return i_compact(c, result, false);
}
static bool break_start(D low, D middle, D all, int index, BreakEvidence *r)
{
    low.v[index - 1] = nonzero(low.v[index - 1]);
    middle.v[index - 1] = nonzero(middle.v[index - 1]);
    all.v[index - 1] = nonzero(all.v[index - 1]);
    double mr = middle.v[index] / middle.v[index - 1];
    if (!(low.v[index] / low.v[index - 1] < 0.35 && mr < 0.65 &&
          all.v[index] / all.v[index - 1] < 0.8))
        return false;
    if (mr < 0.3)
        r->deep = true;
    return true;
}

static bool break_recovery(D low, D middle, D all, int index, int n, int position, BreakEvidence *r)
{
    for (int distance = 4; distance <= 12; distance += 4) {
        int next = index + distance;
        if (next >= n * 4)
            continue;
        low.v[next - 1] = nonzero(low.v[next - 1]);
        middle.v[next - 1] = nonzero(middle.v[next - 1]);
        all.v[next - 1] = nonzero(all.v[next - 1]);
        double recovery = middle.v[next] / middle.v[next - 1];
        if (low.v[next] / low.v[next - 1] > 2.7 && recovery > 1.3 &&
            all.v[next] / all.v[next - 1] > 1.2) {
            if (r->deep && recovery < 2.5)
                r->deep = false;
            r->start = position;
            r->end = position + distance;
            return true;
        }
    }
    return false;
}

BreakEvidence break_evidence(ta_context *c, I candidates, int phase, D lowLevels, D middleLevels,
                             D allLevels)
{
    int n = ta_min(lowLevels.n, ta_min(middleLevels.n, allLevels.n));
    D low = repeat4(c, lowLevels, n), middle = repeat4(c, middleLevels, n),
      all = repeat4(c, allLevels, n);
    BreakEvidence r = { 0 };
    for (int i = 1; i < candidates.n - 1; i++) {
        int position = candidates.v[i], index = position - phase;
        if (index < 1 || index >= n * 4)
            continue;
        if (!break_start(low, middle, all, index, &r)) continue;
        if (break_recovery(low, middle, all, index, n, position, &r)) break;
    }
    ta_release(c, low.v);
    ta_release(c, middle.v);
    ta_release(c, all.v);
    return r;
}
typedef struct {
    double low, middle, all, peak, opening;
} band_ratios;
static band_ratios change_ratios(D low, D middle, D all, D peaks, D opening, int index)
{
    low.v[index - 1] = nonzero(low.v[index - 1]);
    middle.v[index - 1] = nonzero(middle.v[index - 1]);
    all.v[index - 1] = nonzero(all.v[index - 1]);
    peaks.v[index - 1] = nonzero(peaks.v[index - 1]);
    if (opening.v[index] == 0)
        opening.v[index] = 0.018;
    if (opening.v[index - 1] == 0)
        opening.v[index - 1] = 0.018;
    double a = low.v[index] / low.v[index - 1], b = middle.v[index] / middle.v[index - 1],
           c = all.v[index] / all.v[index - 1], d = peaks.v[index] / peaks.v[index - 1],
           e = opening.v[index] / opening.v[index - 1];
    return (band_ratios){ a, b, c, d, e };
}
static bool strong_low_entry(band_ratios r) { return r.low > 20 && r.middle > 1.4 && r.all > 1.3; }
static bool broad_entry(band_ratios r) { return r.peak > 1.2 && r.low > 2.5 && r.middle > 2.5 && r.all > 1.7 && r.opening > 3; }
static bool low_peak_break(band_ratios r) { return r.peak > 6.5 && r.low > 8.5 && r.middle < 0.95 && r.opening > 1; }
static bool middle_entry(band_ratios r) { return r.middle > 2.7 && r.all > 2.5 && r.opening > 1.5; }
static bool low_exit(band_ratios r) { return r.peak < 0.1 && r.low < 0.1 && r.middle > 1 && r.all < 0.8 && r.opening < 0.8; }
static bool low_accent(band_ratios r) { return r.peak > 4 && r.low > 2.5 && r.middle > 1.3 && r.all < 1 && r.opening < 1; }
static bool band_transition(band_ratios r)
{
    return strong_low_entry(r) || broad_entry(r) || low_peak_break(r) || middle_entry(r) || low_exit(r) || low_accent(r);
}

static bool suppressed_first_change(int n, I candidates, int delta, int first)
{
    return n == 0 && candidates.n > 1 &&
           (delta % 16 == 0 || contains((I){ candidates.v + 1, candidates.n - 1 }, first + delta));
}

static int collect_changes(I candidates, D low, D middle, D all, D peaks, D opening, int start, int end, int first, int phase, int found[2])
{
    int n = 0;
    for (int bar = 1; bar <= end; bar++) {
        int delta = bar * 4, index = start + delta - 1;
        if (index < 1 || index >= ta_min(low.n, ta_min(opening.n, peaks.n)))
            continue;
        band_ratios ratios = change_ratios(low, middle, all, peaks, opening, index);
        if (!band_transition(ratios)) continue;
        if (suppressed_first_change(n, candidates, delta, first))
            continue;
        found[n++] = index + phase;
        if (n == 2)
            break;
    }
    return n;
}

ChangeEvidence change_evidence(ta_context *ctx, I candidates, int phase, D lowLevels,
                               D middleLevels, D allLevels, D envelope, D lowPeaks)
{
    ChangeEvidence r = { 0 };
    if (!candidates.n || phase < 1 || phase > lowPeaks.n)
        return r;
    D peaks = d_copy(ctx, d_view(ctx, lowPeaks, phase - 1, lowPeaks.n)),
      opening = d_copy(ctx, envelope);
    int count = ta_min(
        ta_min(lowLevels.n, middleLevels.n),
        ta_min(allLevels.n, ta_min((int)(peaks.n * 0.25 + 0.5), (int)(opening.n * 0.25 + 0.5))));
    D low = repeat4(ctx, lowLevels, count), middle = repeat4(ctx, middleLevels, count),
      all = repeat4(ctx, allLevels, count);
    int first = candidates.v[0], start = first - phase + 1,
        end = count - (int)(start * 0.25 + 0.5) - 8, found[2], n = 0;
    n = collect_changes(candidates, low, middle, all, peaks, opening, start, end, first, phase, found);
    if (n) {
        r.first = found[0];
        r.firstAligned = (found[0] - first) / 16 * 16 + first;
    }
    if (n > 1) {
        r.second = found[1];
        r.secondAligned = (found[1] - first) / 16 * 16 + first;
    }
    ta_release(ctx, peaks.v);
    ta_release(ctx, opening.v);
    ta_release(ctx, low.v);
    ta_release(ctx, middle.v);
    ta_release(ctx, all.v);
    return r;
}
static int candidate_count(ta_context *c, int start, D changes, int phase, B flags)
{
    I a = low_peak_candidates(c, start, changes, phase, flags);
    int n = a.n;
    ta_release(c, a.v);
    return n;
}
static bool irregular_eligible(int p, D changes, int phase, B flags)
{
    int i = p - phase;
    return p > 1 && p - 1 < changes.n && i >= 0 && i < flags.n * 4 &&
           (changes.v[p - 1] > 0 || changes.v[p - 2] > 0 || flags.v[i / 4] != 0);
}
static void first_irregular_change(ta_context *c, ChangeEvidence h, D changes, int phase, B flags, int baseline, int *selected, int *second)
{
    if (h.firstAligned && *selected != h.first &&
        candidate_count(c, h.firstAligned, changes, phase, flags) <=
            candidate_count(c, h.first, changes, phase, flags)) {
        if (*selected != baseline)
            *second = h.first;
        else
            *selected = h.first;
    }
}

static void second_irregular_change(ta_context *c, ChangeEvidence h, D changes, int phase, B flags, int baseline, int *selected, int *second)
{
    if (*selected != h.second && h.secondAligned && *second == 0 &&
        candidate_count(c, h.secondAligned, changes, phase, flags) <=
            candidate_count(c, h.second, changes, phase, flags)) {
        if (*selected == baseline)
            *selected = h.second;
        else
            *second = h.second;
    }
}

static int offset_irregular_seed(ta_context *c, int baseline, D changes, int phase, B flags)
{
    int selected = baseline;
    int best = irregular_eligible(baseline, changes, phase, flags)
                   ? candidate_count(c, baseline, changes, phase, flags)
                   : 0;
    for (int delta = 4; delta <= 12; delta += 4)
        if (irregular_eligible(baseline + delta, changes, phase, flags)) {
            int n = candidate_count(c, baseline + delta, changes, phase, flags);
            if (n > best)
                selected = baseline + delta;
            best = ta_max(best, n);
        }
    return selected;
}

I irregular_seeds(ta_context *c, int intro, D changes, int phase, B flags, int baseline,
                  BreakEvidence b, ChangeEvidence h)
{
    int selected = baseline, second = 0;
    if (b.start && candidate_count(c, b.start, changes, phase, flags) <=
                       candidate_count(c, b.end, changes, phase, flags) + (b.deep ? 2 : 0))
        selected = b.end;
    first_irregular_change(c, h, changes, phase, flags, baseline, &selected, &second);
    second_irregular_change(c, h, changes, phase, flags, baseline, &selected, &second);
    if (baseline && selected == baseline) {
        selected = offset_irregular_seed(c, baseline, changes, phase, flags);
    }
    I r = ta_new_ints(c, 3);
    r.n = 1;
    r.v[0] = intro;
    if (selected != baseline)
        r.v[r.n++] = selected;
    if (second)
        r.v[r.n++] = second;
    return r;
}
static double contrast(double v) { return v >= 1 ? v : 1 / v; }
static bool verse_dimensions(int position, int bar, const VerseProfile *p)
{
    return !(position < 5 || position > p->lowPeaks.n || position > p->opening.n ||
             position > p->changes.n || bar < 1 || bar >= p->low.n || bar >= p->middle.n ||
             bar >= p->all.n);
}

static void guard_verse_zeros(VerseProfile *p, int position, int bar)
{
    for (int i = position - 3; i < position; i++)
        p->lowPeaks.v[i] = nonzero(p->lowPeaks.v[i]);
    for (int i = bar - 1; i <= bar; i++) {
        p->low.v[i] = nonzero(p->low.v[i]);
        p->middle.v[i] = nonzero(p->middle.v[i]);
        p->all.v[i] = nonzero(p->all.v[i]);
    }
    for (int i = position - 2; i < position; i++)
        p->opening.v[i] = nonzero(p->opening.v[i]);
}

double verse_score(ta_context *c, int position, VerseProfile *p, double weight)
{
    int bar = (int)(position * 0.25 + 0.5);
    if (!verse_dimensions(position, bar, p))
        ta_fail(c, TA_FAILED, "verse score dimensions");
    guard_verse_zeros(p, position, bar);
    D changes = p->changes;
    int index =
        changes.v[position - 1] == 0 && changes.v[position - 2] != 0 ? position - 2 : position - 1;
    double pr = p->lowPeaks.v[index] / p->lowPeaks.v[index - 1];
    if (changes.v[index] == 0.8 || changes.v[index] == 0.4)
        pr *= 0.7;
    double pc = contrast(pr);
    if ((changes.v[position - 4] + changes.v[position - 5]) +
            (changes.v[position - 3] + changes.v[position - 2]) ==
        3)
        pc *= 1.2;
    double low = contrast(p->low.v[bar] / p->low.v[bar - 1]),
           middle = contrast(p->middle.v[bar] / p->middle.v[bar - 1]),
           opening = contrast(p->opening.v[position - 1] / p->opening.v[position - 2]),
           all = contrast(p->all.v[bar] / p->all.v[bar - 1]);
    return ((low + pc) + (middle + opening) + all) * weight;
}
static bool band_eligible(int p, int count, D changes, B low, B middle, B hi)
{
    return p >= 2 && p <= count &&
           (changes.v[p - 1] == 1 || changes.v[p - 2] == 1 || low.v[(p - 1) / 4] == 1 ||
            middle.v[(p - 1) / 4] == 1 || hi.v[(p - 1) / 4] == 1);
}
static int prefer_earlier_candidate(ta_context *c, int current, int proposed, int attempt, int count, int phase, VerseProfile *local, B low, B middle, B hi)
{
    if (attempt == 1 || attempt == 2) {
        int earlier = current + (attempt == 1 ? 16 : 48);
        if (band_eligible(earlier, count, local->changes, low, middle, hi)) {
            double early = verse_score(c, earlier, local, 1),
                   weight = attempt == 2
                                ? 1.02
                                : (phase - 1 + proposed <= count / 2 ? 1.25 : 1.23),
                   later = verse_score(c, proposed, local, weight);
            if (early > later)
                proposed = earlier;
        }
    }
    return proposed;
}

static void collect_band_segment(ta_context *c, I *r, int current, int end, int *previous, int count, int phase, VerseProfile *local, B low, B middle, B hi)
{
    const int gaps[] = { 32, 64, 16, 96, 48, 80, 128, 112 };
    r->v[r->n++] = current;
    int attempt = 1;
    while (attempt < 12) {
        int gap = attempt <= 8 ? gaps[attempt - 1] : attempt * 16, proposed = current + gap;
        if (proposed >= end || *previous >= end - 4) {
            if (*previous < end - 4)
                *previous = proposed;
            break;
        }
        if (band_eligible(proposed, count, local->changes, low, middle, hi)) {
            proposed = prefer_earlier_candidate(c, current, proposed, attempt, count, phase, local, low, middle, hi);
            current = proposed;
            r->v[r->n++] = proposed;
            attempt = 1;
        } else
            attempt++;
        *previous = proposed;
    }
}

I all_band_candidates(ta_context *c, I seeds, int phase, D changes, B low, B middle, B hi,
                      VerseProfile p)
{
    if (!peak_input_ready(seeds.n, phase, changes))
        return (I){ 0 };
    int bars = ta_min(
            ta_min(low.n, middle.n),
            ta_min(hi.n, ta_min((int)(changes.n * 0.25 + 0.5), (int)(p.opening.n * 0.25 + 0.5)))),
        count = bars * 4;
    if (!count)
        return (I){ 0 };
    VerseProfile local = { d_copy(c, p.lowPeaks), d_copy(c, p.low), d_copy(c, p.middle),
                           d_copy(c, p.opening), d_copy(c, p.all), ta_new_doubles(c, count) };
    for (int i = 0; i < ta_min(count, changes.n - phase + 1); i++)
        local.changes.v[i] = changes.v[i + phase - 1];
    I limits = ta_new_ints(c, seeds.n + 1);
    for (int i = 0; i < seeds.n; i++)
        limits.v[i] = seeds.v[i] + 1 - phase;
    limits.v[seeds.n] = count;
    int previous = 1;
    I r = ta_new_ints(c, count + seeds.n);
    r.n = 0;
    for (int segment = 0; segment < seeds.n; segment++) {
        collect_band_segment(c, &r, limits.v[segment], limits.v[segment + 1], &previous, count, phase, &local, low, middle, hi);
    }
    for (int i = 0; i < r.n; i++)
        r.v[i] += phase - 1;
    D arrays[] = { local.lowPeaks, local.low, local.middle, local.opening, local.all, local.changes };
    for (int i = 0; i < 6; i++)
        ta_release(c, arrays[i].v);
    ta_release(c, limits.v);
    return r;
}
static bool weak_moderate_candidate(int p, D all, D low)
{
    return all.v[p - 1] >= 0.32 && all.v[p - 1] < 0.56 && all.v[p - 2] >= 0.32 &&
           all.v[p - 2] < 0.56 && low.v[p - 1] <= 300 && low.v[p - 2] <= 300;
}

static bool weak_high_candidate(int p, int count, D all, D low)
{
    if (p > count - 16) {
        return true;
    }
    if (p < 2 || p > count)
        return false;
    if (all.v[p - 1] <= 0.32 && all.v[p - 2] <= 0.32)
        return true;
    if (weak_moderate_candidate(p, all, low))
        return true;
    return false;
}

I adjust_high_candidates(ta_context *c, I candidates, D allLevels, D lowLevels)
{
    int count = allLevels.n * 4;
    D all = ta_new_doubles(c, count), low = ta_new_doubles(c, count);
    for (int i = 0; i < (allLevels.n - 1) * 4; i++) {
        all.v[i] = allLevels.v[i / 4];
        low.v[i] = lowLevels.v[i / 4];
    }
    I r = i_copy(c, candidates);
    for (int i = 1; i < candidates.n; i++) {
        if (weak_high_candidate(candidates.v[i], count, all, low)) r.v[i] = 0;
    }
    int out = 0;
    for (int i = 0; i < r.n; i++)
        if (r.v[i] > 0 && r.v[i] <= count)
            r.v[out++] = r.v[i];
    r.n = out;
    if (!out)
        r = ta_new_ints(c, 1);
    ta_release(c, all.v);
    ta_release(c, low.v);
    return r;
}
static int repeated_rhythm_matches(B rhythm, int center)
{
    int matches = 0;
    for (int o = 7; o <= 46; o++)
        matches += rhythm.v[center + o - 128] == rhythm.v[center + o];
    for (int o = 71; o <= 110; o++)
        matches += rhythm.v[center + o - 128] == rhythm.v[center + o];
    return matches;
}

static bool stronger_earlier_rhythm(B rhythm, int center)
{
    int nearby = 0, earlier = 0;
    for (int o = -49; o <= -34; o++)
        nearby += rhythm.v[center + o] == rhythm.v[center + o + 32];
    for (int o = -113; o <= -98; o++)
        earlier += rhythm.v[center + o] == rhythm.v[center + o + 64];
    return nearby < earlier;
}

static void measure_repeated_rhythm(I candidates, B rhythm, I *positions, D *scores, B flags)
{
    for (int i = 1; i < candidates.n; i++) {
        int beat = candidates.v[i];
        if (rhythm.n * 0.25 - beat < 32)
            break;
        if (beat - candidates.v[i - 1] != 32)
            continue;
        int center = beat * 4;
        if (center < 121 || center + 110 >= rhythm.n)
            continue;
        int matches = repeated_rhythm_matches(rhythm, center);
        if (i != 1 && i != candidates.n - 1) {
            flags.v[positions->n] = stronger_earlier_rhythm(rhythm, center);
        }
        positions->v[positions->n++] = beat;
        scores->v[scores->n++] = matches * 0.0125;
    }
}

static bool repeated_rhythm_candidate(I r, int position)
{
    bool found = false, valid = true;
    for (int j = 0; j < r.n; j++)
        if (r.v[j] == position) {
            found = true;
            if (j == 0 || r.v[j] - r.v[j - 1] != 32)
                valid = false;
        }
    return found && valid;
}

static void remove_rhythm_position(I *r, int position)
{
    for (int j = r->n - 1; j >= 0; j--)
        if (r->v[j] == position)
            i_remove(r, j);
}

RhythmRejection reject_repeated_rhythm(ta_context *c, I candidates, B rhythm)
{
    I positions = ta_new_ints(c, candidates.n);
    positions.n = 0;
    D scores = ta_new_doubles(c, candidates.n);
    scores.n = 0;
    B flags = ta_new_bytes(c, candidates.n);
    measure_repeated_rhythm(candidates, rhythm, &positions, &scores, flags);
    double mean = scores.n ? d_sum(scores) / scores.n : 0;
    I r = i_copy(c, candidates);
    for (int i = 0; i < positions.n; i++) {
        if (!repeated_rhythm_candidate(r, positions.v[i])) continue;
        bool reject = i == 0 ? (flags.v[0] == 0 && mean < scores.v[0])
                             : ((flags.v[i] == 0 ? 1.02 : 1.06) * mean < scores.v[i]);
        if (reject) remove_rhythm_position(&r, positions.v[i]);
    }
    ta_release(c, positions.v);
    return (RhythmRejection){ r, scores, flags, mean };
}
static I coarse(ta_context *c, I beats, int phase)
{
    I r = i_sorted(c, i_compact(c, beats, false), false);
    for (int i = 0; i < r.n; i++) {
        double v = (r.v[i] - phase) * 0.25;
        r.v[i] = (int)(v + (v >= 0 ? 0.5 : -0.5)) + 1;
    }
    while (r.n && !r.v[0])
        i_remove(&r, 0);
    return r;
}
static bool genre_snap_range(int p, int existing, RatioSeries low)
{
    return !(abs(p - existing) != 1 || p <= 0 || p > low.levels.n);
}

static void snap_genre_addition(int *addition, int existing, RatioSeries low, bool *snapped)
{
    int p = *addition;
    if (!genre_snap_range(p, existing, low))
        return;
    if (low.contrast.v[p - 1] < 1.5 || low.levels.v[p - 1] < 0.5) {
        *snapped = true;
        *addition = existing;
    } else if (!*snapped && existing - p == 1 && low.direction.v[p - 1] <= 0.5 &&
               low.direction.v[existing - 1] >= 1.8)
        *addition = existing;
}

static bool shift_genre_boundary(int previous, int next, double direction, double following)
{
    return previous >= 4 && previous % 4 == 3 && next >= 4 && next % 4 == 1 && direction <= 0.83 && following * direction >= 0.9;
}
static bool reject_genre_boundary(int previous, int next, double direction)
{
    return previous == 1 || next == 1 || direction > 0.83 || ((previous == 2 || previous % 4 == 2) && next == 2);
}
static void prune_genre_additions(ta_context *c, I *r, I combined, I distinct, RatioSeries low)
{
    int removed = 0;
    for (int original = 1; original < combined.n; original++) {
        int index = original - removed, p = r->v[index], previous = p - r->v[index - 1];
        if (!contains(distinct, combined.v[original]) || previous % 4 == 0 ||
            original == combined.n - 1)
            continue;
        if (p < 1 || p >= low.direction.n)
            ta_fail(c, TA_FAILED, "genre boundary dimensions");
        int next = r->v[index + 1] - p;
        double direction = low.direction.v[p - 1];
        if (shift_genre_boundary(previous, next, direction, low.direction.v[p]))
            r->v[index]++;
        else if (reject_genre_boundary(previous, next, direction)) {
            i_remove(r, index);
            removed++;
        }
    }
}

I add_genre1(ta_context *c, I base, I secondary, RatioSeries low, I breaks, I snares, int phase)
{
    I primary = coarse(c, base, phase), additions = coarse(c, secondary, phase);
    for (int i = 0; i < additions.n; i++) {
        bool snapped = false;
        for (int j = 0; j < primary.n; j++) {
            snap_genre_addition(&additions.v[i], primary.v[j], low, &snapped);
        }
    }
    I distinct = i_compact(c, i_join(c, i_join(c, additions, breaks), snares), true);
    int out = 0;
    for (int i = 0; i < distinct.n; i++)
        if (!contains(primary, distinct.v[i]))
            distinct.v[out++] = distinct.v[i];
    distinct.n = out;
    I combined = i_sorted(c, i_compact(c, i_join(c, primary, distinct), false), false),
      r = i_copy(c, combined);
    prune_genre_additions(c, &r, combined, distinct, low);
    if (r.n && r.v[r.n - 1] > low.levels.n - 4)
        r.n--;
    for (int i = 0; i < r.n; i++)
        r.v[i] = phase - 4 + r.v[i] * 4;
    return r;
}
I reconcile_high_seeds(ta_context *c, I seeds, I anchors)
{
    if (!anchors.n || anchors.v[0] < 1)
        return i_copy(c, seeds);
    I r = i_copy(c, seeds), remaining = i_copy(c, anchors);
    for (int i = r.n - 1; i >= 0; i--)
        for (int j = remaining.n - 1; j >= 0; j--) {
            if (r.v[i] == remaining.v[j])
                i_remove(&remaining, j);
            else if (abs(r.v[i] - remaining.v[j]) == 4)
                r.v[i] = 0;
        }
    return i_sorted(c, i_compact(c, i_join(c, r, remaining), false), false);
}
static void insert_finish_anchors(I *r, I anchors)
{
    for (int i = anchors.n - 1; i >= 0; i--) {
        int anchor = anchors.v[i];
        if (contains(*r, anchor))
            continue;
        int previous = -1;
        for (int j = 0; j < r->n; j++)
            if (r->v[j] < anchor)
                previous = j;
        if (previous >= 0) {
            memmove(r->v + previous + 2, r->v + previous + 1,
                    (size_t)(r->n - previous - 1) * sizeof(int));
            r->v[previous + 1] = anchor;
            r->n++;
        }
    }
}

static void suppress_near_anchors(I *r, I anchors)
{
    for (int i = anchors.n - 1; i >= 0; i--)
        for (int j = r->n - 1; j >= 1; j--)
            if (r->v[j] != anchors.v[i] && abs(r->v[j] - anchors.v[i]) <= 8)
                i_remove(r, j);
}

I finish_candidates(ta_context *c, I candidates, int initial, I anchors, int route, int endBeat)
{
    I r = ta_new_ints(c, candidates.n + anchors.n + 1);
    r.n = candidates.n;
    memcpy(r.v, candidates.v, (size_t)candidates.n * sizeof(int));
    if (route != 1)
        while (r.n && r.v[r.n - 1] > endBeat)
            r.n--;
    while (r.n && r.v[0] <= initial + 3)
        i_remove(&r, 0);
    memmove(r.v + 1, r.v, (size_t)r.n * sizeof(int));
    r.v[0] = initial;
    r.n++;
    if (route == 1) {
        insert_finish_anchors(&r, anchors);
        suppress_near_anchors(&r, anchors);
    }
    if (r.n > 1 && r.v[0] == r.v[1])
        i_remove(&r, 0);
    return r;
}

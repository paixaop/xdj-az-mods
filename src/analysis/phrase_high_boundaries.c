/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static bool contains(I a, int x)
{
    for (int i = 0; i < a.n; i++)
        if (a.v[i] == x)
            return true;
    return false;
}
static D repeat_bars(ta_context *c, D a, int n, int width)
{
    D r = ta_new_doubles(c, n * width);
    for (int i = 0; i < r.n; i++)
        r.v[i] = a.v[i / width];
    return r;
}
static bool low_peak_eligible(int i, D aligned, B flags, int width, const double *parameters)
{
    int back = (int)parameters[TA_LOW_PEAK_PRIOR_CHANGE_BACK];
    double mark = parameters[TA_LOW_PEAK_CHANGE_MARK];
    return i >= back && i < aligned.n &&
           (aligned.v[i] == mark || aligned.v[i - back] == mark ||
            flags.v[i / width] == parameters[TA_LOW_PEAK_FLAG_MARK]);
}

static bool peak_input_ready(int count, int phase, D changes)
{
    return count && phase >= 1 && phase <= changes.n;
}

I low_peak_candidates(ta_context *c, int intro, D changes, int phase, B flags, const double *parameters)
{
    int width = (int)parameters[TA_LOW_PEAK_FLAG_BEATS_PER_BAR];
    int extent = flags.n * width;
    if (!peak_input_ready(extent, phase, changes))
        return (I){ 0 };
    D aligned = ta_new_doubles(c, extent);
    for (int i = 0; i < ta_min(extent, changes.n - phase + 1); i++)
        aligned.v[i] = changes.v[i + phase - 1];
    I result = ta_new_ints(c, extent + 1);
    result.n = 1;
    result.v[0] = intro;
    int first_attempt = (int)parameters[TA_LOW_PEAK_GAP_FIRST_ATTEMPT];
    int last = intro, attempt = first_attempt;
    const double *gaps = parameters + TA_LOW_PEAK_GAP_0;
    const int gap_count = TA_LOW_PEAK_GAP_5 - TA_LOW_PEAK_GAP_0 + 1;
    const int stride = (int)parameters[TA_LOW_PEAK_GAP_STRIDE];
    for (;;) {
        int gap = attempt <= gap_count ? (int)gaps[attempt - 1] : attempt * stride;
        int proposed = last + gap;
        if (proposed < extent) {
            int i = proposed - phase;
            if (low_peak_eligible(i, aligned, flags, width, parameters)) {
                result.v[result.n++] = proposed;
                last = proposed;
                attempt = first_attempt;
            } else
                attempt++;
        } else
            attempt++;
        if (proposed >= extent - parameters[TA_LOW_PEAK_GAP_END_MARGIN] &&
            attempt >= parameters[TA_LOW_PEAK_GAP_STOP_ATTEMPT])
            break;
    }
    ta_release(c, aligned.v);
    return i_compact(c, result, false);
}
static void guard_band_levels(D low, D middle, D all, int index, double zero_level)
{
    if (low.v[index] == 0)
        low.v[index] = zero_level;
    if (middle.v[index] == 0)
        middle.v[index] = zero_level;
    if (all.v[index] == 0)
        all.v[index] = zero_level;
}

static bool break_start(D low, D middle, D all, int index, BreakEvidence *r, const double *parameters)
{
    guard_band_levels(low, middle, all, index - 1, parameters[TA_BREAK_EVIDENCE_ZERO_LEVEL]);
    double mr = middle.v[index] / middle.v[index - 1];
    if (!(low.v[index] / low.v[index - 1] < parameters[TA_BREAK_EVIDENCE_LOW_MAX] && mr < parameters[TA_BREAK_EVIDENCE_MIDDLE_MAX] &&
          all.v[index] / all.v[index - 1] < parameters[TA_BREAK_EVIDENCE_ALL_MAX]))
        return false;
    if (mr < parameters[TA_BREAK_EVIDENCE_DEEP_MAX])
        r->deep = true;
    return true;
}

static bool break_recovery(D low, D middle, D all, int index, int position, BreakEvidence *r, const double *parameters)
{
    for (int slot = TA_BREAK_EVIDENCE_RECOVERY_0; slot <= TA_BREAK_EVIDENCE_RECOVERY_2; ++slot) {
        int distance = (int)parameters[slot];
        int next = index + distance;
        if (next >= low.n)
            continue;
        guard_band_levels(low, middle, all, next - 1, parameters[TA_BREAK_EVIDENCE_ZERO_LEVEL]);
        double recovery = middle.v[next] / middle.v[next - 1];
        if (low.v[next] / low.v[next - 1] > parameters[TA_BREAK_EVIDENCE_RECOVERY_LOW_MIN] && recovery > parameters[TA_BREAK_EVIDENCE_RECOVERY_MIDDLE_MIN] &&
            all.v[next] / all.v[next - 1] > parameters[TA_BREAK_EVIDENCE_RECOVERY_ALL_MIN]) {
            if (r->deep && recovery < parameters[TA_BREAK_EVIDENCE_RECOVERY_DEEP_MIN])
                r->deep = false;
            r->start = position;
            r->end = position + distance;
            return true;
        }
    }
    return false;
}

BreakEvidence break_evidence(ta_context *c, I candidates, int phase, D lowLevels, D middleLevels,
                             D allLevels, const double *parameters)
{
    int n = ta_min(lowLevels.n, ta_min(middleLevels.n, allLevels.n));
    int width = (int)parameters[TA_BREAK_EVIDENCE_BEATS_PER_BAR];
    D low = repeat_bars(c, lowLevels, n, width), middle = repeat_bars(c, middleLevels, n, width),
      all = repeat_bars(c, allLevels, n, width);
    BreakEvidence r = { 0 };
    for (int i = 1; i < candidates.n - 1; i++) {
        int position = candidates.v[i], index = position - phase;
        if (index < 1 || index >= low.n)
            continue;
        if (!break_start(low, middle, all, index, &r, parameters)) continue;
        if (break_recovery(low, middle, all, index, position, &r, parameters)) break;
    }
    ta_release(c, low.v);
    ta_release(c, middle.v);
    ta_release(c, all.v);
    return r;
}
typedef struct {
    double low, middle, all, peak, opening;
} band_ratios;
static band_ratios change_ratios(D low, D middle, D all, D peaks, D opening, int index,
                                 const double *parameters)
{
    int previous = index - (int)parameters[TA_BAND_CHANGE_PRIOR_BACK] + (int)parameters[TA_BAND_CHANGE_CURRENT_BACK];
    guard_band_levels(low, middle, all, previous, parameters[TA_BAND_CHANGE_ZERO_LEVEL]);
    if (peaks.v[previous] == 0)
        peaks.v[previous] = parameters[TA_BAND_CHANGE_ZERO_LEVEL];
    if (opening.v[index] == 0)
        opening.v[index] = parameters[TA_BAND_CHANGE_OPENING_ZERO];
    if (opening.v[previous] == 0)
        opening.v[previous] = parameters[TA_BAND_CHANGE_OPENING_ZERO];
    double a = low.v[index] / low.v[previous], b = middle.v[index] / middle.v[previous],
           c = all.v[index] / all.v[previous], d = peaks.v[index] / peaks.v[previous],
           e = opening.v[index] / opening.v[previous];
    return (band_ratios){ a, b, c, d, e };
}
static bool strong_low_entry(band_ratios r, const double *parameters)
{
    return r.low > parameters[TA_BAND_CHANGE_STRONG_LOW_MIN] &&
           r.middle > parameters[TA_BAND_CHANGE_STRONG_MIDDLE_MIN] &&
           r.all > parameters[TA_BAND_CHANGE_INCREASE_MIDDLE];
}

static bool broad_entry(band_ratios r, const double *parameters)
{
    return r.peak > parameters[TA_BAND_CHANGE_BROAD_PEAK_MIN] &&
           r.low > parameters[TA_BAND_CHANGE_ENTRY_LEVEL] &&
           r.middle > parameters[TA_BAND_CHANGE_ENTRY_LEVEL] &&
           r.all > parameters[TA_BAND_CHANGE_BROAD_ALL_MIN] &&
           r.opening > parameters[TA_BAND_CHANGE_BROAD_OPENING_MIN];
}

static bool low_peak_break(band_ratios r, const double *parameters)
{
    return r.peak > parameters[TA_BAND_CHANGE_BREAK_PEAK_MIN] &&
           r.low > parameters[TA_BAND_CHANGE_BREAK_LOW_MIN] &&
           r.middle < parameters[TA_BAND_CHANGE_BREAK_MIDDLE_MAX] &&
           r.opening > parameters[TA_BAND_CHANGE_FLAT_RATIO];
}

static bool middle_entry(band_ratios r, const double *parameters)
{
    return r.middle > parameters[TA_BAND_CHANGE_MIDDLE_ENTRY_MIN] &&
           r.all > parameters[TA_BAND_CHANGE_ENTRY_LEVEL] &&
           r.opening > parameters[TA_BAND_CHANGE_MIDDLE_OPENING_MIN];
}

static bool low_exit(band_ratios r, const double *parameters)
{
    return r.peak < parameters[TA_BAND_CHANGE_EXIT_PEAK_MAX] &&
           r.low < parameters[TA_BAND_CHANGE_EXIT_LOW_MAX] &&
           r.middle > parameters[TA_BAND_CHANGE_FLAT_RATIO] &&
           r.all < parameters[TA_BAND_CHANGE_EXIT_ALL_MAX] &&
           r.opening < parameters[TA_BAND_CHANGE_EXIT_OPENING_MAX];
}

static bool low_accent(band_ratios r, const double *parameters)
{
    return r.peak > parameters[TA_BAND_CHANGE_ACCENT_PEAK_MIN] &&
           r.low > parameters[TA_BAND_CHANGE_ENTRY_LEVEL] &&
           r.middle > parameters[TA_BAND_CHANGE_INCREASE_MIDDLE] &&
           r.all < parameters[TA_BAND_CHANGE_FLAT_RATIO] &&
           r.opening < parameters[TA_BAND_CHANGE_FLAT_RATIO];
}

static bool band_transition(band_ratios r, const double *parameters)
{
    return strong_low_entry(r, parameters) ||
           broad_entry(r, parameters) ||
           low_peak_break(r, parameters) ||
           middle_entry(r, parameters) ||
           low_exit(r, parameters) ||
           low_accent(r, parameters);
}

static bool suppressed_first_change(int n, I candidates, int delta, int first,
                                    const double *parameters)
{
    int alignment = (int)parameters[TA_BAND_CHANGE_ALIGNMENT_BIAS] + 1;
    return n == 0 && candidates.n >= parameters[TA_BAND_CHANGE_SUPPRESS_MIN_CANDIDATES] &&
           (delta % alignment == 0 || contains((I){ candidates.v + 1, candidates.n - 1 }, first + delta));
}

static int collect_changes(I candidates, D low, D middle, D all, D peaks, D opening, int start, int end, int first, int phase, int found[2], const double *parameters)
{
    int n = 0;
    int width = (int)parameters[TA_BAND_CHANGE_GROUP_BEATS];
    int back = (int)parameters[TA_BAND_CHANGE_CURRENT_BACK];
    int prior = (int)parameters[TA_BAND_CHANGE_PRIOR_BACK] - back;
    int origin = (int)parameters[TA_BAND_CHANGE_PHASE_ORIGIN];
    int limit = (int)parameters[TA_BAND_CHANGE_FIRST_STATE] + 1;
    for (int bar = (int)parameters[TA_BAND_CHANGE_FIRST_BAR]; bar <= end; bar++) {
        int delta = bar * width, index = start + delta - back;
        if (index < prior || index >= ta_min(low.n, ta_min(opening.n, peaks.n)))
            continue;
        band_ratios ratios = change_ratios(low, middle, all, peaks, opening, index, parameters);
        if (!band_transition(ratios, parameters)) continue;
        if (suppressed_first_change(n, candidates, delta, first, parameters))
            continue;
        found[n++] = index + back + phase - origin;
        if (n == limit)
            break;
    }
    return n;
}

ChangeEvidence change_evidence(ta_context *ctx, I candidates, int phase, D lowLevels,
                               D middleLevels, D allLevels, D envelope, D lowPeaks,
                               const double *parameters)
{
    ChangeEvidence r = { 0 };
    if (!candidates.n || phase < 1 || phase > lowPeaks.n)
        return r;
    int origin = (int)parameters[TA_BAND_CHANGE_PHASE_ORIGIN];
    D peaks = d_copy(ctx, d_view(ctx, lowPeaks, phase - origin, lowPeaks.n)),
      opening = d_copy(ctx, envelope);
    double scale = parameters[TA_BAND_CHANGE_EXTENT_SCALE], bias = parameters[TA_BAND_CHANGE_EXTENT_BIAS];
    int count = ta_min(
        ta_min(lowLevels.n, middleLevels.n),
        ta_min(allLevels.n, ta_min((int)fma(peaks.n, scale, bias), (int)fma(opening.n, scale, bias))));
    int width = (int)parameters[TA_BAND_CHANGE_GROUP_BEATS];
    D low = repeat_bars(ctx, lowLevels, count, width), middle = repeat_bars(ctx, middleLevels, count, width),
      all = repeat_bars(ctx, allLevels, count, width);
    int first = candidates.v[0], start = first - phase + origin,
        start_bar = (int)fma(start, parameters[TA_BAND_CHANGE_START_SCALE], parameters[TA_BAND_CHANGE_START_BIAS]),
        end = count - start_bar - (int)parameters[TA_BAND_CHANGE_TAIL_BARS],
        found[2], n = 0, alignment = (int)parameters[TA_BAND_CHANGE_ALIGNMENT_BIAS] + 1;
    n = collect_changes(candidates, low, middle, all, peaks, opening, start, end, first, phase, found, parameters);
    if (n) {
        r.first = found[0];
        r.firstAligned = (found[0] - first) / alignment * alignment + first;
    }
    if (n > 1) {
        r.second = found[1];
        r.secondAligned = (found[1] - first) / alignment * alignment + first;
    }
    ta_release(ctx, peaks.v);
    ta_release(ctx, opening.v);
    ta_release(ctx, low.v);
    ta_release(ctx, middle.v);
    ta_release(ctx, all.v);
    return r;
}
static int candidate_count(ta_context *c, int start, D changes, int phase, B flags, const double *parameters)
{
    I a = low_peak_candidates(c, start, changes, phase, flags, parameters);
    int n = a.n;
    ta_release(c, a.v);
    return n;
}
static bool irregular_eligible(int p, D changes, int phase, B flags, const double *parameters)
{
    int i = p - phase;
    int current = (int)parameters[TA_IRREGULAR_CURRENT_CHANGE_BACK];
    int prior = (int)parameters[TA_IRREGULAR_PRIOR_CHANGE_BACK];
    int width = (int)parameters[TA_IRREGULAR_FLAG_BEATS_PER_BAR];
    return p >= prior && p - current < changes.n && i >= 0 && i / width < flags.n &&
           (changes.v[p - current] > 0 || changes.v[p - prior] > 0 || flags.v[i / width] != 0);
}
static void first_irregular_change(ta_context *c, ChangeEvidence h, D changes, int phase, B flags, int baseline, int *selected, int *second, const double *parameters)
{
    if (h.firstAligned && *selected != h.first &&
        candidate_count(c, h.firstAligned, changes, phase, flags, parameters) <=
            candidate_count(c, h.first, changes, phase, flags, parameters)) {
        if (*selected != baseline)
            *second = h.first;
        else
            *selected = h.first;
    }
}

static void second_irregular_change(ta_context *c, ChangeEvidence h, D changes, int phase, B flags, int baseline, int *selected, int *second, const double *parameters)
{
    if (*selected != h.second && h.secondAligned && *second == 0 &&
        candidate_count(c, h.secondAligned, changes, phase, flags, parameters) <=
            candidate_count(c, h.second, changes, phase, flags, parameters)) {
        if (*selected == baseline)
            *selected = h.second;
        else
            *second = h.second;
    }
}

static int offset_irregular_seed(ta_context *c, int baseline, D changes, int phase, B flags, const double *parameters)
{
    int selected = baseline;
    int best = irregular_eligible(baseline, changes, phase, flags, parameters)
                   ? candidate_count(c, baseline, changes, phase, flags, parameters)
                   : 0;
    for (int i = TA_IRREGULAR_FORWARD_FIRST; i <= TA_IRREGULAR_FORWARD_THIRD; i++) {
        int delta = (int)parameters[i];
        if (baseline > changes.n - delta)
            continue;
        int candidate = baseline + delta;
        if (irregular_eligible(candidate, changes, phase, flags, parameters)) {
            int n = candidate_count(c, candidate, changes, phase, flags, parameters);
            if (n > best)
                selected = candidate;
            best = ta_max(best, n);
        }
    }
    return selected;
}

I irregular_seeds(ta_context *c, int intro, D changes, int phase, B flags, int baseline,
                  BreakEvidence b, ChangeEvidence h, const double *parameters)
{
    int selected = baseline, second = 0;
    if (b.start && candidate_count(c, b.start, changes, phase, flags, parameters) <=
                       candidate_count(c, b.end, changes, phase, flags, parameters) +
                           (b.deep ? parameters[TA_IRREGULAR_BREAK_DEPTH_VOTE] : 0))
        selected = b.end;
    first_irregular_change(c, h, changes, phase, flags, baseline, &selected, &second, parameters);
    second_irregular_change(c, h, changes, phase, flags, baseline, &selected, &second, parameters);
    if (baseline && selected == baseline) {
        selected = offset_irregular_seed(c, baseline, changes, phase, flags, parameters);
    }
    I r = ta_new_ints(c, 1 + (selected != baseline) + (second != 0));
    r.n = 1;
    r.v[0] = intro;
    if (selected != baseline)
        r.v[r.n++] = selected;
    if (second)
        r.v[r.n++] = second;
    return r;
}
static double contrast(double v, double numerator) { return v >= numerator ? v : numerator / v; }
static bool verse_dimensions(int position, int bar, const VerseProfile *p, const double *parameters)
{
    return !(position < -parameters[TA_VERSE_SCORE_MARK_OLDEST_OFFSET] || position > p->lowPeaks.n ||
             position > p->opening.n || position > p->changes.n ||
             bar < parameters[TA_VERSE_SCORE_BAR_LOOKBACK] || bar >= p->low.n || bar >= p->middle.n ||
             bar >= p->all.n);
}

static void guard_verse_zeros(VerseProfile *p, int position, int bar, const double *parameters)
{
    double zero_level = parameters[TA_VERSE_SCORE_ZERO_LEVEL];
    int peak_back = (int)parameters[TA_VERSE_SCORE_PEAK_LOOKBACK];
    int bar_back = (int)parameters[TA_VERSE_SCORE_BAR_LOOKBACK];
    int opening_back = (int)parameters[TA_VERSE_SCORE_OPENING_LOOKBACK];
    for (int i = position - peak_back; i < position; i++)
        if (p->lowPeaks.v[i] == 0) p->lowPeaks.v[i] = zero_level;
    for (int i = bar - bar_back; i <= bar; i++) {
        guard_band_levels(p->low, p->middle, p->all, i, zero_level);
    }
    for (int i = position - opening_back; i < position; i++)
        if (p->opening.v[i] == 0) p->opening.v[i] = zero_level;
}

double verse_score(ta_context *c, int position, VerseProfile *p, double weight, const double *parameters)
{
    int bar = (int)fma(position, parameters[TA_VERSE_SCORE_BAR_SCALE], parameters[TA_VERSE_SCORE_BAR_BIAS]);
    if (!verse_dimensions(position, bar, p, parameters))
        ta_fail(c, TA_FAILED, "verse score dimensions");
    guard_verse_zeros(p, position, bar, parameters);
    D changes = p->changes;
    int current = position - (int)parameters[TA_VERSE_SCORE_CURRENT_BACK];
    int prior = position - (int)parameters[TA_VERSE_SCORE_PRIOR_BACK];
    int oldest = position + (int)parameters[TA_VERSE_SCORE_MARK_OLDEST_OFFSET];
    int peak_before = position - (int)parameters[TA_VERSE_SCORE_PEAK_LOOKBACK];
    int previous_bar = bar - (int)parameters[TA_VERSE_SCORE_BAR_LOOKBACK];
    int index = changes.v[current] == 0 && changes.v[prior] != 0 ? prior : current;
    int previous = index == prior ? peak_before : prior;
    double pr = p->lowPeaks.v[index] / p->lowPeaks.v[previous];
    if (changes.v[index] == parameters[TA_VERSE_SCORE_STRONG_MARK] ||
        changes.v[index] == parameters[TA_VERSE_SCORE_WEAK_MARK])
        pr *= parameters[TA_VERSE_SCORE_MARK_FACTOR];
    double numerator = parameters[TA_VERSE_SCORE_CONTRAST_NUMERATOR];
    double pc = contrast(pr, numerator);
    if ((changes.v[oldest + 1] + changes.v[oldest]) + (changes.v[peak_before] + changes.v[prior]) ==
        parameters[TA_VERSE_SCORE_TOTAL_MARKS])
        pc *= parameters[TA_VERSE_SCORE_TOTAL_FACTOR];
    double low = contrast(p->low.v[bar] / p->low.v[previous_bar], numerator),
           middle = contrast(p->middle.v[bar] / p->middle.v[previous_bar], numerator),
           opening = contrast(p->opening.v[current] / p->opening.v[prior], numerator),
           all = contrast(p->all.v[bar] / p->all.v[previous_bar], numerator);
    return ((low + pc) + (middle + opening) + all) * weight;
}
static bool band_eligible(int p, int count, D changes, B low, B middle, B hi, const double *parameters)
{
    int before = (int)parameters[TA_ALL_BAND_PREVIOUS_BACK];
    int current = p - (int)parameters[TA_ALL_BAND_CURRENT_BACK];
    int bar = current / (int)parameters[TA_ALL_BAND_GROUP_BEATS];
    double change = parameters[TA_ALL_BAND_CHANGE_MARK], flag = parameters[TA_ALL_BAND_FLAG_MARK];
    return p >= before && p <= count &&
           (changes.v[current] == change || changes.v[p - before] == change || low.v[bar] == flag ||
            middle.v[bar] == flag || hi.v[bar] == flag);
}
static int prefer_earlier_candidate(ta_context *c, int current, int proposed, int attempt, int count, int phase, VerseProfile *local, B low, B middle, B hi, const double *parameters)
{
    bool primary = attempt == parameters[TA_ALL_BAND_PRIMARY_ATTEMPT];
    if (primary || attempt == parameters[TA_ALL_BAND_SECONDARY_ATTEMPT]) {
        int offset = (int)parameters[primary ? TA_VERSE_SCORE_PRIMARY_OFFSET_INDEX : TA_VERSE_SCORE_SECONDARY_OFFSET_INDEX];
        int earlier = current + offset + (int)parameters[TA_ALL_BAND_CURRENT_BACK];
        if (band_eligible(earlier, count, local->changes, low, middle, hi, parameters)) {
            double coordinate = phase - parameters[TA_ALL_BAND_PHASE_ORIGIN] + proposed;
            double midpoint = count * parameters[TA_ALL_BAND_MIDPOINT_SCALE];
            double early = verse_score(c, earlier, local, parameters[TA_ALL_BAND_BASELINE_WEIGHT], parameters),
                   weight = primary
                                ? (coordinate <= midpoint ? parameters[TA_VERSE_SCORE_PRIMARY_NEAR_WEIGHT]
                                                          : parameters[TA_VERSE_SCORE_PRIMARY_FAR_WEIGHT])
                                : parameters[TA_VERSE_SCORE_SECONDARY_WEIGHT],
                   later = verse_score(c, proposed, local, weight, parameters);
            if (early > later)
                proposed = earlier;
        }
    }
    return proposed;
}

static void collect_band_segment(ta_context *c, I *r, int current, int end, int *previous, int count, int phase, VerseProfile *local, B low, B middle, B hi, const double *parameters)
{
    const double *gaps = parameters + TA_ALL_BAND_GAP_0;
    const int gap_count = TA_ALL_BAND_GAP_7 - TA_ALL_BAND_GAP_0 + 1;
    const int stride = (int)parameters[TA_ALL_BAND_GAP_STRIDE];
    const double margin = parameters[TA_ALL_BAND_GAP_END_MARGIN];
    r->v[r->n++] = current;
    int first = (int)parameters[TA_ALL_BAND_FIRST_ATTEMPT];
    int attempt = first;
    while (attempt < parameters[TA_ALL_BAND_GAP_STOP_ATTEMPT]) {
        int ordinal = attempt - first;
        int gap = ordinal < gap_count ? (int)gaps[ordinal] : attempt * stride;
        int proposed = current + gap;
        if (proposed >= end || *previous >= end - margin) {
            if (*previous < end - margin)
                *previous = proposed;
            break;
        }
        if (band_eligible(proposed, count, local->changes, low, middle, hi, parameters)) {
            proposed = prefer_earlier_candidate(c, current, proposed, attempt, count, phase, local, low, middle, hi, parameters);
            current = proposed;
            r->v[r->n++] = proposed;
            attempt = first;
        } else
            attempt++;
        *previous = proposed;
    }
}

I all_band_candidates(ta_context *c, I seeds, int phase, D changes, B low, B middle, B hi,
                      VerseProfile p, const double *parameters)
{
    if (!peak_input_ready(seeds.n, phase, changes))
        return (I){ 0 };
    double scale = parameters[TA_ALL_BAND_EXTENT_SCALE], bias = parameters[TA_ALL_BAND_EXTENT_BIAS];
    int width = (int)parameters[TA_ALL_BAND_GROUP_BEATS];
    int origin = (int)parameters[TA_ALL_BAND_PHASE_ORIGIN];
    int bars = ta_min(
            ta_min(low.n, middle.n),
            ta_min(hi.n, ta_min((int)fma(changes.n, scale, bias), (int)fma(p.opening.n, scale, bias)))),
        count = bars * width;
    if (!count)
        return (I){ 0 };
    VerseProfile local = { d_copy(c, p.lowPeaks), d_copy(c, p.low), d_copy(c, p.middle),
                           d_copy(c, p.opening), d_copy(c, p.all), ta_new_doubles(c, count) };
    for (int i = 0; i < ta_min(count, changes.n - phase + origin); i++)
        local.changes.v[i] = changes.v[i + phase - origin];
    I limits = ta_new_ints(c, seeds.n + 1);
    for (int i = 0; i < seeds.n; i++)
        limits.v[i] = seeds.v[i] + origin - phase;
    limits.v[seeds.n] = count;
    int previous = origin;
    I r = ta_new_ints(c, count + seeds.n);
    r.n = 0;
    for (int segment = 0; segment < seeds.n; segment++) {
        collect_band_segment(c, &r, limits.v[segment], limits.v[segment + 1], &previous, count, phase, &local, low, middle, hi, parameters);
    }
    for (int i = 0; i < r.n; i++)
        r.v[i] += phase - origin;
    D arrays[] = { local.lowPeaks, local.low, local.middle, local.opening, local.all, local.changes };
    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++)
        ta_release(c, arrays[i].v);
    ta_release(c, limits.v);
    return r;
}
static bool weak_moderate_candidate(int p, int previous, D all, D low, const double *parameters)
{
    double lo = parameters[TA_HIGH_PRUNE_MODERATE_ALL_MIN];
    double hi = parameters[TA_HIGH_PRUNE_MODERATE_ALL_MAX];
    double low_max = parameters[TA_HIGH_PRUNE_MODERATE_LOW_MAX];
    return all.v[p - 1] >= lo && all.v[p - 1] < hi && all.v[previous] >= lo &&
           all.v[previous] < hi && low.v[p - 1] <= low_max && low.v[previous] <= low_max;
}

static bool weak_high_candidate(int p, int count, D all, D low, const double *parameters)
{
    if (p > count - parameters[TA_HIGH_PRUNE_TAIL_BEATS])
        return true;
    int before = (int)parameters[TA_HIGH_PRUNE_PREVIOUS_BEAT];
    if (p < before || p > count)
        return false;
    double weak_max = parameters[TA_HIGH_PRUNE_WEAK_ALL_MAX];
    return (all.v[p - 1] <= weak_max && all.v[p - before] <= weak_max) ||
           weak_moderate_candidate(p, p - before, all, low, parameters);
}

static D repeat_levels(ta_context *c, D levels, int bars, int beats_per_bar)
{
    if (bars > INT_MAX / beats_per_bar || levels.n < bars - 1)
        ta_fail(c, TA_FAILED, "candidate level dimensions");
    D result = ta_new_doubles(c, bars * beats_per_bar);
    for (int i = 0; i < (bars - 1) * beats_per_bar; i++)
        result.v[i] = levels.v[i / beats_per_bar];
    return result;
}

I adjust_high_candidates(ta_context *c, I candidates, D allLevels, D lowLevels, const double *parameters)
{
    int beats_per_bar = (int)parameters[TA_HIGH_PRUNE_BEATS_PER_BAR];
    D all = repeat_levels(c, allLevels, allLevels.n, beats_per_bar),
      low = repeat_levels(c, lowLevels, allLevels.n, beats_per_bar);
    int count = all.n;
    I r = i_copy(c, candidates);
    for (int i = 1; i < candidates.n; i++) {
        if (weak_high_candidate(candidates.v[i], count, all, low, parameters))
            r.v[i] = 0;
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
typedef struct {
    int first, last, displacement;
} RhythmWindow;

typedef struct {
    RhythmWindow primary[2], preceding[2];
    int samples_per_beat, spacing;
    double beat_scale, tail_beats, score_scale;
} RepeatedRhythmProfile;

static RhythmWindow preceding_rhythm_window(const double *parameters)
{
    int start = (int)parameters[0];
    return (RhythmWindow){ -start, -(int)parameters[1], start - (int)parameters[2] };
}

static RepeatedRhythmProfile repeated_rhythm_profile(const double *parameters)
{
    return (RepeatedRhythmProfile){
        .primary = {
            { (int)parameters[TA_REPEAT_RHYTHM_FIRST_START], (int)parameters[TA_REPEAT_RHYTHM_FIRST_END],
              -(int)parameters[TA_REPEAT_RHYTHM_FIRST_LAG] },
            { (int)parameters[TA_REPEAT_RHYTHM_SECOND_START], (int)parameters[TA_REPEAT_RHYTHM_SECOND_END],
              -(int)parameters[TA_REPEAT_RHYTHM_SECOND_LAG] },
        },
        .preceding = {
            preceding_rhythm_window(parameters + TA_REPEAT_RHYTHM_NEAR_START),
            preceding_rhythm_window(parameters + TA_REPEAT_RHYTHM_EARLIER_START),
        },
        .samples_per_beat = (int)parameters[TA_REPEAT_RHYTHM_SAMPLES_PER_BEAT],
        .spacing = (int)parameters[TA_REPEAT_RHYTHM_SPACING],
        .beat_scale = parameters[TA_REPEAT_RHYTHM_BEAT_SCALE],
        .tail_beats = parameters[TA_REPEAT_RHYTHM_TAIL_BEATS],
        .score_scale = parameters[TA_REPEAT_RHYTHM_SCORE_SCALE],
    };
}

static bool rhythm_window_fits(RhythmWindow window, int64_t center, int count)
{
    return center + window.first + ta_min(0, window.displacement) >= 0 &&
           center + window.last + ta_max(0, window.displacement) < count;
}

static bool rhythm_pair_fits(const RhythmWindow windows[2], int64_t center, int count)
{
    return rhythm_window_fits(windows[0], center, count) && rhythm_window_fits(windows[1], center, count);
}

static int rhythm_window_matches(B rhythm, int64_t center, RhythmWindow window)
{
    int matches = 0;
    for (int offset = window.first; offset <= window.last; offset++)
        matches += rhythm.v[center + offset] == rhythm.v[center + offset + window.displacement];
    return matches;
}

static bool stronger_earlier_rhythm(B rhythm, int64_t center, const RhythmWindow windows[2])
{
    return rhythm_pair_fits(windows, center, rhythm.n) &&
           rhythm_window_matches(rhythm, center, windows[0]) <
               rhythm_window_matches(rhythm, center, windows[1]);
}

static void measure_repeated_rhythm(I candidates, B rhythm, I *positions, D *scores, B flags,
                                   const RepeatedRhythmProfile *profile)
{
    for (int i = 1; i < candidates.n; i++) {
        int beat = candidates.v[i];
        if (rhythm.n * profile->beat_scale - beat < profile->tail_beats)
            break;
        if (beat - candidates.v[i - 1] != profile->spacing)
            continue;
        int64_t center = (int64_t)beat * profile->samples_per_beat;
        if (!rhythm_pair_fits(profile->primary, center, rhythm.n))
            continue;
        int matches = rhythm_window_matches(rhythm, center, profile->primary[0]) +
                      rhythm_window_matches(rhythm, center, profile->primary[1]);
        if (i != 1 && i != candidates.n - 1) {
            flags.v[positions->n] = stronger_earlier_rhythm(rhythm, center, profile->preceding);
        }
        positions->v[positions->n++] = beat;
        scores->v[scores->n++] = matches * profile->score_scale;
    }
}

static bool repeated_rhythm_candidate(I r, int position, double spacing)
{
    bool found = false, valid = true;
    for (int j = 0; j < r.n; j++)
        if (r.v[j] == position) {
            found = true;
            if (j == 0 || r.v[j] - r.v[j - 1] != spacing)
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

RhythmRejection reject_repeated_rhythm(ta_context *c, I candidates, B rhythm, const double *parameters)
{
    I positions = ta_new_ints(c, candidates.n);
    positions.n = 0;
    D scores = ta_new_doubles(c, candidates.n);
    scores.n = 0;
    B flags = ta_new_bytes(c, candidates.n);
    RepeatedRhythmProfile profile = repeated_rhythm_profile(parameters);
    measure_repeated_rhythm(candidates, rhythm, &positions, &scores, flags, &profile);
    double mean = scores.n ? d_sum(scores, parameters) / scores.n : 0;
    I r = i_copy(c, candidates);
    for (int i = 0; i < positions.n; i++) {
        if (!repeated_rhythm_candidate(r, positions.v[i], parameters[TA_REPEAT_RHYTHM_FINAL_SPACING]))
            continue;
        double weight = parameters[flags.v[i] ? TA_REPEAT_RHYTHM_FLAGGED_WEIGHT
                                               : TA_REPEAT_RHYTHM_UNFLAGGED_WEIGHT];
        bool reject = i == 0 ? (flags.v[0] == 0 && mean < scores.v[0])
                             : weight * mean < scores.v[i];
        if (reject) remove_rhythm_position(&r, positions.v[i]);
    }
    ta_release(c, positions.v);
    return (RhythmRejection){ r, scores, flags, mean };
}
static I coarse(ta_context *c, I beats, int phase, const double *parameters)
{
    I r = i_sorted(c, i_compact(c, beats, false), false);
    double scale = parameters[TA_GENRE_SNAP_BEAT_SCALE];
    double bias = parameters[TA_GENRE_SNAP_ROUND_BIAS];
    for (int i = 0; i < r.n; i++) {
        double v = (r.v[i] - phase) * scale;
        r.v[i] = (int)(v + (v >= 0 ? bias : -bias)) + 1;
    }
    while (r.n && !r.v[0])
        i_remove(&r, 0);
    return r;
}
static bool genre_snap_range(int p, int existing, RatioSeries low, double distance)
{
    return p > 0 && p <= low.levels.n && abs(p - existing) == distance;
}

static void snap_genre_addition(int *addition, int existing, RatioSeries low, bool *snapped,
                                const double *parameters)
{
    int p = *addition;
    if (!genre_snap_range(p, existing, low, parameters[TA_GENRE_SNAP_DISTANCE]))
        return;
    if (low.contrast.v[p - 1] < parameters[TA_GENRE_SNAP_CONTRAST_MIN] ||
        low.levels.v[p - 1] < parameters[TA_GENRE_SNAP_LEVEL_MIN]) {
        *snapped = true;
        *addition = existing;
    } else if (!*snapped && existing - p == parameters[TA_GENRE_SNAP_BACKWARD_DISTANCE] &&
               low.direction.v[p - 1] <= parameters[TA_GENRE_SNAP_DIRECTION_MAX] &&
               existing <= low.direction.n &&
               low.direction.v[existing - 1] >= parameters[TA_GENRE_SNAP_ADJACENT_DIRECTION_MIN])
        *addition = existing;
}

static bool shift_genre_boundary(int previous, int next, double direction, double following,
                                 const double *parameters)
{
    int bar = (int)parameters[TA_GENRE_SNAP_BEATS_PER_BAR];
    double span = parameters[TA_GENRE_SNAP_SHIFT_MIN_SPAN];
    return previous >= span && previous % bar == parameters[TA_GENRE_SNAP_PREVIOUS_PHASE] &&
           next >= span && next % bar == parameters[TA_GENRE_SNAP_NEXT_PHASE] &&
           direction <= parameters[TA_GENRE_SNAP_PHASE_DIRECTION_MAX] &&
           following * direction >= parameters[TA_GENRE_SNAP_PHASE_COMBINED_MIN];
}
static bool reject_genre_boundary(int previous, int next, double direction, const double *parameters)
{
    int bar = (int)parameters[TA_GENRE_SNAP_BEATS_PER_BAR];
    double short_gap = parameters[TA_GENRE_SNAP_REJECT_SHORT_GAP];
    double pair_gap = parameters[TA_GENRE_SNAP_REJECT_PAIR_GAP];
    return previous == short_gap || next == short_gap ||
           direction > parameters[TA_GENRE_SNAP_PHASE_DIRECTION_MAX] ||
           ((previous == pair_gap || previous % bar == pair_gap) && next == pair_gap);
}
static void prune_genre_additions(ta_context *c, I *r, I combined, I distinct, RatioSeries low,
                                  const double *parameters)
{
    int removed = 0;
    int bar = (int)parameters[TA_GENRE_SNAP_BEATS_PER_BAR];
    for (int original = 1; original < combined.n; original++) {
        int index = original - removed, p = r->v[index], previous = p - r->v[index - 1];
        if (!contains(distinct, combined.v[original]) || previous % bar == 0 ||
            original == combined.n - 1)
            continue;
        if (p < 1 || p >= low.direction.n)
            ta_fail(c, TA_FAILED, "genre boundary dimensions");
        int next = r->v[index + 1] - p;
        double direction = low.direction.v[p - 1];
        if (shift_genre_boundary(previous, next, direction, low.direction.v[p], parameters))
            r->v[index]++;
        else if (reject_genre_boundary(previous, next, direction, parameters)) {
            i_remove(r, index);
            removed++;
        }
    }
}

I add_genre1(ta_context *c, I base, I secondary, RatioSeries low, I breaks, I snares, int phase,
             const double *parameters)
{
    I primary = coarse(c, base, phase, parameters), additions = coarse(c, secondary, phase, parameters);
    for (int i = 0; i < additions.n; i++) {
        bool snapped = false;
        for (int j = 0; j < primary.n; j++) {
            snap_genre_addition(&additions.v[i], primary.v[j], low, &snapped, parameters);
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
    prune_genre_additions(c, &r, combined, distinct, low, parameters);
    if (r.n && r.v[r.n - 1] > low.levels.n - parameters[TA_GENRE_SNAP_TAIL_BARS])
        r.n--;
    int beats_per_bar = (int)parameters[TA_GENRE_SNAP_BEATS_PER_BAR];
    for (int i = 0; i < r.n; i++)
        r.v[i] = phase - beats_per_bar + r.v[i] * beats_per_bar;
    return r;
}
I reconcile_high_seeds(ta_context *c, I seeds, I anchors, const double *parameters)
{
    if (!anchors.n || anchors.v[0] < 1)
        return i_copy(c, seeds);
    I r = i_copy(c, seeds), remaining = i_copy(c, anchors);
    for (int i = r.n - 1; i >= 0; i--)
        for (int j = remaining.n - 1; j >= 0; j--) {
            if (r.v[i] == remaining.v[j])
                i_remove(&remaining, j);
            else if (abs(r.v[i] - remaining.v[j]) == parameters[TA_ANCHOR_SEED_EXCLUSION_DISTANCE])
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

static void suppress_near_anchors(I *r, I anchors, const double *parameters)
{
    for (int i = anchors.n - 1; i >= 0; i--)
        for (int j = r->n - 1; j >= 1; j--)
            if (r->v[j] != anchors.v[i] && abs(r->v[j] - anchors.v[i]) <= parameters[TA_ANCHOR_FINAL_EXCLUSION_DISTANCE])
                i_remove(r, j);
}

I finish_candidates(ta_context *c, I candidates, int initial, I anchors, int route, int endBeat, const double *parameters)
{
    I r = ta_new_ints(c, candidates.n + anchors.n + 1);
    r.n = candidates.n;
    memcpy(r.v, candidates.v, (size_t)candidates.n * sizeof(int));
    bool high_route = route == parameters[TA_HIGH_CANDIDATE_ROUTE];
    if (!high_route)
        while (r.n && r.v[r.n - 1] > endBeat)
            r.n--;
    while (r.n && r.v[0] - initial <= parameters[TA_ANCHOR_INITIAL_COLLISION_DISTANCE])
        i_remove(&r, 0);
    memmove(r.v + 1, r.v, (size_t)r.n * sizeof(int));
    r.v[0] = initial;
    r.n++;
    if (high_route) {
        insert_finish_anchors(&r, anchors);
        suppress_near_anchors(&r, anchors, parameters);
    }
    if (r.n > 1 && r.v[0] == r.v[1])
        i_remove(&r, 0);
    return r;
}

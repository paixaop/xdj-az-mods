/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double nonzero(double x, double substitute) { return x == 0 ? substitute : x; }
static double boundary_sum(ta_context *c, D a, int first, int last, const double *parameters)
{
    if (first < 0 || last >= a.n || first > last) return 0;
    return d_sum(d_view(c, a, first, last + 1), parameters);
}
RatioSeries ratio_series(ta_context *c, D input, const double *parameters)
{
    int n = input.n;
    RatioSeries r = { d_copy(c, input), ta_new_doubles(c, n), ta_new_doubles(c, n) };
    double maximum = d_max(input);
    double scale = maximum == 0 ? parameters[TA_BOUNDARY_ZERO_MAX_INVERSE] : 1 / maximum;
    double zero_level = parameters[TA_BOUNDARY_ZERO_RATIO_LEVEL];
    int normalize_end = n - (int)parameters[TA_BOUNDARY_NORMALIZATION_TAIL];
    for (int i = 0; i < normalize_end; i++)
        r.levels.v[i] *= scale;
    int first = (int)parameters[TA_BOUNDARY_RATIO_FIRST];
    int end = first + n - (int)parameters[TA_BOUNDARY_RATIO_OMITTED];
    int back = (int)parameters[TA_BOUNDARY_RATIO_PREVIOUS_BACK];
    double numerator = parameters[TA_BOUNDARY_RATIO_INVERSE_NUMERATOR];
    for (int i = first; i < end; i++) {
        if (r.levels.v[i - back] == 0)
            r.levels.v[i - back] = zero_level;
        if (r.levels.v[i] == 0)
            r.levels.v[i] = zero_level;
        double ratio = r.levels.v[i] / r.levels.v[i - back];
        r.direction.v[i] = ratio;
        r.contrast.v[i] = ratio >= numerator ? ratio : numerator / ratio;
    }
    return r;
}
CandidateFeatures candidate_features(ta_context *c, D lowPeaks, D lowLevels, D middlePeaks,
                                     D middleCoarse, int phase, const double *parameters)
{
    int start = phase - (int)parameters[TA_BOUNDARY_SOURCE_PHASE_ORIGIN];
    int available = lowPeaks.n - start;
    int width = (int)parameters[TA_BOUNDARY_SOURCE_GROUP_BEATS];
    int n = ta_max(0, available / width);
    D low = ta_new_doubles(c, n);
    int tail = (int)parameters[TA_BOUNDARY_SOURCE_TAIL_GROUPS];
    double scale = parameters[TA_BOUNDARY_SOURCE_MEAN_SCALE];
    if (available >= parameters[TA_BOUNDARY_SOURCE_MIN_SAMPLES])
        for (int i = 0; i < ta_max(1, n - tail); i++) {
            int first = start + i * width;
            low.v[i] = d_sum(d_view(c, lowPeaks, first, first + width), parameters) * scale;
        }
    return (CandidateFeatures){ ratio_series(c, low, parameters), ratio_series(c, lowLevels, parameters),
                                ratio_series(c, middleCoarse, parameters), ratio_series(c, middlePeaks, parameters) };
}
static RatioSeries ratio_slice(ta_context *c, RatioSeries p, int start, int count)
{
    return (RatioSeries){ d_view(c, p.levels, start, start + count),
                          d_view(c, p.contrast, start, start + count),
                          d_view(c, p.direction, start, start + count) };
}
static int replace_adjacent_peaks(RatioSeries p, I ranked, I strongest, I retained,
                                  const double *parameters)
{
    int replacements = 0;
    int distance = (int)parameters[TA_BOUNDARY_ADJACENT_DISTANCE];
    int min_ranks = (int)parameters[TA_BOUNDARY_ADJACENT_MIN_RANKS];
    int delta = (int)parameters[TA_BOUNDARY_LOCAL_REFERENCE_DELTA];
    for (int i = 0; i + 1 < strongest.n; i++) {
        int position = strongest.v[i], next = strongest.v[i + 1];
        if (position - next == distance && ranked.n >= replacements + min_ranks) {
            double divisor = p.contrast.v[next + delta],
                   ratio = divisor == 0 ? p.contrast.v[position + delta] * parameters[TA_BOUNDARY_ADJACENT_ZERO_INVERSE]
                                        : p.contrast.v[position + delta] / divisor;
            if (ratio < parameters[TA_BOUNDARY_ADJACENT_CONTRAST_MAX] &&
                p.direction.v[position + delta] <= parameters[TA_BOUNDARY_ADJACENT_DIRECTION_LIMIT]) {
                retained.v[i] = 0;
                replacements++;
            }
            if (p.direction.v[position + delta] > parameters[TA_BOUNDARY_ADJACENT_DIRECTION_LIMIT]) {
                retained.v[i + 1] = 0;
                replacements++;
            }
        }
    }
    return replacements;
}

static bool local_peak_shape(ta_context *c, RatioSeries p, int candidate, double avg, bool strongestLevel, double level,
                             const double *parameters)
{
    double ceiling = avg * parameters[TA_BOUNDARY_LOCAL_SHAPE_MAX];
    int last = candidate + (int)parameters[TA_BOUNDARY_SHAPE_LAST];
    int history = candidate - (int)parameters[TA_BOUNDARY_SHAPE_HISTORY_BACK];
    double pair = boundary_sum(c, p.contrast, candidate, last, parameters) * parameters[TA_BOUNDARY_SHAPE_PAIR_SCALE];
    double extended = boundary_sum(c, p.contrast, history, last, parameters) * parameters[TA_BOUNDARY_SHAPE_HISTORY_SCALE];
    if (pair < ceiling && extended < ceiling) return true;
    double neighbor = nonzero(p.levels.v[last], parameters[TA_BOUNDARY_NEIGHBOR_LEVEL_ZERO]);
    return strongestLevel && level / neighbor > parameters[TA_BOUNDARY_NEIGHBOR_LEVEL_MIN];
}

static bool following_peaks_dominate(ta_context *c, RatioSeries p, int candidate, double avg, double strength,
                                    const double *parameters)
{
    int first = candidate + (int)parameters[TA_BOUNDARY_FOLLOWING_FIRST];
    int second = candidate + (int)parameters[TA_BOUNDARY_FOLLOWING_SECOND];
    int last = candidate + (int)parameters[TA_BOUNDARY_FOLLOWING_LAST];
    double scale = parameters[TA_BOUNDARY_SHAPE_PAIR_SCALE];
    double following = nonzero(boundary_sum(c, p.contrast, first, second, parameters) * scale,
                               parameters[TA_BOUNDARY_FIRST_FOLLOWING_ZERO]);
    double next = nonzero(boundary_sum(c, p.contrast, second, last, parameters) * scale,
                          parameters[TA_BOUNDARY_SECOND_FOLLOWING_ZERO]);
    double minimum = parameters[TA_BOUNDARY_FOLLOWING_CONTRAST_MIN];
    if (following >= minimum && strength / following <= avg && next >= minimum &&
        strength / next <= avg &&
        (strength <= parameters[TA_BOUNDARY_FOLLOWING_STRENGTH_MAX] ||
         (p.contrast.v[last] + p.contrast.v[first]) * scale >= minimum))
        return true;
    return false;
}

static bool local_peak_survives(ta_context *c, RatioSeries p, int candidate, int maximum, double avg,
                                const double *parameters)
{
    if (candidate == 0)
        return false;
    int index = candidate + (int)parameters[TA_BOUNDARY_LOCAL_REFERENCE_DELTA];
    double level = p.levels.v[index], strength = p.contrast.v[index];
    bool strongestLevel = maximum == candidate;
    if (!(strength > avg * parameters[TA_BOUNDARY_LOCAL_STRENGTH_MIN] || strongestLevel))
        return false;
    int start = candidate - (int)parameters[TA_BOUNDARY_SHAPE_HISTORY_BACK];
    int last = candidate + (int)parameters[TA_BOUNDARY_SHAPE_LAST];
    double previous = boundary_sum(c, p.levels, start, last, parameters) * parameters[TA_BOUNDARY_SHAPE_HISTORY_SCALE];
    double change = level / nonzero(previous, parameters[TA_BOUNDARY_HISTORY_LEVEL_ZERO]);
    if (!(change > avg * parameters[TA_BOUNDARY_LOCAL_RISE_MIN] ||
          change < parameters[TA_BOUNDARY_LOCAL_FALL_MAX] / avg || strongestLevel))
        return false;
    if (!local_peak_shape(c, p, candidate, avg, strongestLevel, level, parameters)) return false;
    return !following_peaks_dominate(c, p, candidate, avg, strength, parameters);
}

static void trim_leading_zeros(I *result, int limit)
{
    for (int i = 0; i < limit && result->n && result->v[0] == 0; i++)
        i_remove(result, 0);
}

static I empty_local_candidates(I result, const double *parameters)
{
    result.n = (int)parameters[TA_BOUNDARY_LOCAL_EMPTY_COUNT];
    memset(result.v, 0, (size_t)result.n * sizeof *result.v);
    return result;
}

static I finish_local_candidates(ta_context *c, I result, const double *parameters)
{
    while (result.n && result.v[result.n - 1] == 0)
        result.n--;
    if (!result.n) return empty_local_candidates(result, parameters);
    result = i_sorted(c, result, false);
    trim_leading_zeros(&result, (int)parameters[TA_BOUNDARY_LOCAL_ZERO_TRIM_LIMIT]);
    if (result.n && result.v[0] == 0)
        return empty_local_candidates(result, parameters);
    return result;
}

I local_candidates(ta_context *c, RatioSeries p, int firstBar, const double *parameters)
{
    int n = p.levels.n;
    if (n < parameters[TA_BOUNDARY_LOCAL_MIN_BARS] || p.contrast.n != n || p.direction.n != n)
        return (I){ 0 };
    int first = (int)parameters[TA_BOUNDARY_LOCAL_INTERIOR_FIRST];
    int interior = n - (int)parameters[TA_BOUNDARY_LOCAL_EDGE_TRIM];
    int top = (int)parameters[TA_BOUNDARY_LOCAL_STRONGEST_COUNT];
    D level_view = d_view(c, p.levels, first, first + interior);
    D contrasts = d_view(c, p.contrast, first, first + interior);
    double avg = nonzero(d_sum(contrasts, parameters) / interior, parameters[TA_BOUNDARY_LOCAL_ZERO_MEAN]);
    int maximum = 0;
    for (int i = 1; i < interior; i++)
        if (level_view.v[i] > level_view.v[maximum])
            maximum = i;
    I ranked = rank_d(c, contrasts, true);
    for (int i = 0; i < ranked.n; i++)
        ranked.v[i]++;
    I strongest = i_sorted(c, (I){ ranked.v, top }, true), retained = i_copy(c, strongest);
    int replacements = replace_adjacent_peaks(p, ranked, strongest, retained, parameters);
    memcpy(ranked.v, retained.v, (size_t)top * sizeof *ranked.v);
    I result = ta_new_ints(c, top + replacements);
    int origin = firstBar - (int)parameters[TA_BOUNDARY_LOCAL_FIRST_BAR_BACK] +
                 (int)parameters[TA_BOUNDARY_FOLLOWING_FIRST];
    for (int i = 0; i < result.n; i++) {
        int candidate = ranked.v[i];
        if (!local_peak_survives(c, p, candidate, maximum + 1, avg, parameters)) continue;
        result.v[i] = origin + candidate;
    }
    return finish_local_candidates(c, result, parameters);
}
static I coarse_hook_seeds(ta_context *c, I hypotheses, int phase, int n, const double *parameters)
{
    I seeds = ta_new_ints(c, ta_max(1, hypotheses.n));
    seeds.n = hypotheses.n;
    double scale = parameters[TA_HOOK_SEED_BAR_SCALE], bias = parameters[TA_HOOK_SEED_BAR_BIAS];
    for (int i = 0; i < seeds.n; i++) {
        int coarse = (int)((hypotheses.v[i] - phase) * scale + bias);
        seeds.v[i] = coarse <= n ? coarse : 0;
    }
    seeds = i_sorted(c, seeds, false);
    for (int i = 0; i < seeds.n - 1; i++)
        if (seeds.v[i] == seeds.v[i + 1])
            seeds.v[i] = 0;
    seeds = i_sorted(c, seeds, false);
    return seeds;
}

static I trim_hook_seeds(ta_context *c, I seeds, int intro, const double *parameters)
{
    int maximum = (int)parameters[TA_HOOK_SEED_MAX_LEADING_MISSING];
    int removed = 0;
    while (removed < maximum && seeds.n && seeds.v[0] < 1) {
        i_remove(&seeds, 0);
        removed++;
    }
    if (!seeds.n) {
        seeds = ta_new_ints(c, 1);
    }
    if (maximum - removed >= parameters[TA_HOOK_SEED_MIN_REMAINING] &&
        intro - seeds.v[0] == parameters[TA_HOOK_SEED_INTRO_NEAR])
        i_remove(&seeds, 0);
    if (!seeds.n)
        seeds = ta_new_ints(c, 1);
    return seeds;
}

static void add_hook_window(ta_context *c, RatioSeries profiles[3], int start, int end, int n, int route, I *additions,
                            const double *parameters)
{
    int length = end - start + 1;
    if (start >= 1 && length >= 0 && start - 1 + length <= n)
        for (int k = 0; k < 3; k++)
            if (route != parameters[TA_HIGH_CANDIDATE_ROUTE] || k == 1) {
                I values = local_candidates(
                    c, ratio_slice(c, profiles[k], start - 1, length), start, parameters);
                for (int j = 0; j < values.n; j++)
                    additions->v[additions->n++] = values.v[j];
            }
}

static I hook_windows(ta_context *c, RatioSeries profiles[3], I seeds, int n, int route, const double *parameters)
{
    int last = ta_max(seeds.v[seeds.n - 1], (int)parameters[TA_HOOK_WINDOW_MIN_LAST]);
    int end = last + ta_min(n - last, (int)parameters[TA_HOOK_WINDOW_END_FORWARD]);
    int start = ta_min(last - (int)parameters[TA_HOOK_WINDOW_START_BACK], end);
    int step = (int)parameters[TA_HOOK_WINDOW_STEP];
    int windows = ta_max(0, (n - last - 1) / step + 1);
    /* Each window can return four local candidates and up to three replacements per band. */
    const int candidates_per_window = 3 * (4 + 3);
    if (windows > INT_MAX / candidates_per_window)
        ta_fail(c, TA_FAILED, "hook window allocation");
    I additions = ta_new_ints(c, windows * candidates_per_window);
    additions.n = 0;
    for (int iteration = 0; iteration < windows; iteration++) {
        add_hook_window(c, profiles, start, end, n, route, &additions, parameters);
        if (n - end < parameters[TA_HOOK_WINDOW_TAIL_MIN])
            break;
        end += step;
        if (end >= n)
            end = n - 1;
        start += step;
    }
    return additions;
}

static I merge_hook_seeds(ta_context *c, I seeds, I additions, const double *parameters)
{
    I combined = i_join(c, seeds, additions), surviving = i_copy(c, combined);
    double adjacent = parameters[TA_HOOK_MERGE_ADJACENCY];
    for (int i = 0; i < combined.n - 1; i++)
        if (combined.v[i] != 0 && combined.v[i] == combined.v[i + 1] - adjacent)
            surviving.v[i] = 0;
    surviving = i_compact(c, surviving, false);
    surviving = i_sorted(c, surviving, false);
    int first = (int)parameters[TA_HOOK_MERGE_FIRST_BAR];
    if (!surviving.n || surviving.v[0] != first) {
        surviving = i_join(c, (I){ &first, 1 }, surviving);
    }
    return surviving;
}

static void score_hooks(RatioSeries profiles[3], I surviving, D scores, int n, const double *parameters)
{
    for (int i = 0; i < surviving.n; i++) {
        int position = surviving.v[i], index = position - 1;
        if (index < 0 || index >= n || position == parameters[TA_HOOK_SCORE_SKIP_BAR])
            continue;
        double score = fma(profiles[1].contrast.v[index] + profiles[0].contrast.v[index],
                           parameters[TA_HOOK_SCORE_PAIRED_CONTRAST],
                           profiles[2].contrast.v[index] * parameters[TA_HOOK_SCORE_THIRD_CONTRAST]);
        scores.v[i] = position > parameters[TA_HOOK_SCORE_PREVIOUS_MIN_EXCLUSIVE]
                          ? (profiles[1].contrast.v[index - 1] + score) * parameters[TA_HOOK_SCORE_WITH_PREVIOUS_SCALE]
                          : score * parameters[TA_HOOK_SCORE_WITHOUT_PREVIOUS_SCALE];
    }
}

static bool hook_can_be_pruned(int position, double score, int intro, const double *parameters)
{
    return position != parameters[TA_HOOK_PRUNE_PROTECTED_FIRST] &&
           position != parameters[TA_HOOK_PRUNE_PROTECTED_SECOND] && position != intro &&
           score < parameters[TA_HOOK_PRUNE_SCORE_MAX_EXCLUSIVE];
}

static void prune_close_hooks(I surviving, I original, D scores, int intro, const double *parameters)
{
    double zero_score = parameters[TA_HOOK_PRUNE_ZERO_SCORE];
    int first = (int)parameters[TA_HOOK_PRUNE_FIRST_SCORE];
    for (int i = first; i < surviving.n; i++) {
        if (scores.v[i - 1] == 0)
            scores.v[i - 1] = zero_score;
        if (scores.v[i] == 0)
            scores.v[i] = zero_score;
        double previous = scores.v[i - 1], current = scores.v[i];
        if (ta_dmax(current / previous, previous / current) >= parameters[TA_HOOK_PRUNE_RATIO_MIN] &&
            original.v[i] - original.v[i - 1] <= parameters[TA_HOOK_PRUNE_NEAR_MAX]) {
            int remove = current <= previous ? i : i - 1, position = surviving.v[remove];
            if (hook_can_be_pruned(position, scores.v[remove], intro, parameters))
                surviving.v[remove] = 0;
        }
    }
}

I verse_hook_candidates(ta_context *c, CandidateFeatures f, I hypotheses, int introEnd, int phase,
                        int route, const double *parameters)
{
    int n = ta_min(f.lowPeak.levels.n, ta_min(f.lowLevel.levels.n, f.middleCoarse.levels.n));
    if (n <= 0)
        return (I){ 0 };
    RatioSeries profiles[] = { ratio_slice(c, f.lowPeak, 0, n), ratio_slice(c, f.lowLevel, 0, n),
                               ratio_slice(c, f.middleCoarse, 0, n) };
    I seeds = coarse_hook_seeds(c, hypotheses, phase, n, parameters);
    int intro = (int)((introEnd - phase) * parameters[TA_HOOK_INTRO_BAR_SCALE] +
                      parameters[TA_HOOK_INTRO_BAR_BIAS]);
    seeds = trim_hook_seeds(c, seeds, intro, parameters);
    I additions = hook_windows(c, profiles, seeds, n, route, parameters);
    additions = i_compact(c, additions, true);
    I surviving = merge_hook_seeds(c, seeds, additions, parameters);
    I original = i_copy(c, surviving);
    D scores = ta_new_doubles(c, surviving.n);
    score_hooks(profiles, surviving, scores, n, parameters);
    prune_close_hooks(surviving, original, scores, intro, parameters);
    surviving = i_compact(c, surviving, false);
    for (int i = 0; i < surviving.n; i++)
        surviving.v[i] = phase - 4 + surviving.v[i] * 4;
    return surviving;
}
static double break_nonzero(double level, double zero_level)
{
    return level == 0 ? zero_level : level;
}

static void short_break_ratios(D low, D middle, D lr, D mr, D lf, D mf, int count, int *lc,
                               int *mc, const double *parameters)
{
    *lc = *mc = 0;
    double zero_level = parameters[TA_SHORT_BREAK_ZERO_LEVEL];
    double cutoff = parameters[TA_SHORT_BREAK_RATIO_FALL_CUTOFF];
    int first = (int)parameters[TA_SHORT_BREAK_RATIO_FIRST_SLOT];
    int end = count - (int)parameters[TA_SHORT_BREAK_RATIO_END_TRIM];
    for (int i = first; i < end; i++) {
        low.v[i] = break_nonzero(low.v[i], zero_level);
        low.v[i - 1] = break_nonzero(low.v[i - 1], zero_level);
        lr.v[i] = low.v[i] / low.v[i - 1];
        if (lr.v[i] >= cutoff)
            (*lc)++;
        else
            lf.v[i] = lr.v[i];
        middle.v[i] = break_nonzero(middle.v[i], zero_level);
        middle.v[i - 1] = break_nonzero(middle.v[i - 1], zero_level);
        mr.v[i] = middle.v[i] / middle.v[i - 1];
        if (mr.v[i] >= cutoff)
            (*mc)++;
        else
            mf.v[i] = mr.v[i];
    }
}

static double capped_break_threshold(ta_context *c, D lr, int count, double lt,
                                     const double *parameters)
{
    D values = ta_new_doubles(c, count);
    values.n = 0;
    for (int i = 0; i < count; i++)
        if (lr.v[i] != 0)
            values.v[values.n++] = lr.v[i];
    I order = rank_d(c, values, false);
    int small = 0;
    for (int i = 0; i < order.n; i++) {
        double v = values.v[order.v[i]];
        if (v > 0 && v <= lt)
            small++;
        else
            break;
    }
    if (small >= parameters[TA_SHORT_BREAK_RANK_MIN_COUNT])
        lt = values.v[order.v[(int)parameters[TA_SHORT_BREAK_RANK_INDEX]]];
    return lt;
}

static int recovered_break_position(D low, int position, const double *parameters)
{
    int width = (int)parameters[TA_SHORT_BREAK_REMAP_BAR_WIDTH];
    int phase = position % width;
    int excluded = phase - (int)parameters[TA_SHORT_BREAK_EXCLUDED_PHASE_FIRST];
    if (excluded >= 0 && excluded <= parameters[TA_SHORT_BREAK_EXCLUDED_PHASE_DELTA])
        return 0;
    double reference = low.v[position - (int)parameters[TA_SHORT_BREAK_RECOVERY_REFERENCE_BACK]];
    double scale = reference == 0 ? parameters[TA_SHORT_BREAK_RECOVERY_ZERO]
                                  : parameters[TA_SHORT_BREAK_RECOVERY_INVERSE_NUMERATOR] / reference;
    int last = position + (int)parameters[TA_SHORT_BREAK_RECOVERY_LAST];
    for (int j = position; j <= last; j++)
        if (low.v[j] * scale > parameters[TA_SHORT_BREAK_RECOVERY_MIN])
            return position / width + (int)parameters[TA_SHORT_BREAK_REMAP_BAR_BIAS] + (phase != 0);
    return 0;
}

static bool strong_preceding_break(D low, D middle, int i, const double *parameters)
{
    double high = parameters[TA_SHORT_BREAK_PRECEDING_HIGH], weak = parameters[TA_SHORT_BREAK_PRECEDING_LOW];
    double previous = middle.v[i - (int)parameters[TA_SHORT_BREAK_PRECEDING_NEXT_BACK]];
    return (low.v[i] > high && (middle.v[i] > weak || previous > weak)) ||
           (low.v[i] > weak && (middle.v[i] > high || previous > high));
}

static void collect_short_breaks(D low, D middle, D lr, D mr, double lt, double mt, int count,
                                I *result, const double *parameters)
{
    if (count < parameters[TA_SHORT_BREAK_SCAN_MIN_COUNT]) return;
    int first = (int)parameters[TA_SHORT_BREAK_SCAN_FIRST];
    int n = count - (int)parameters[TA_SHORT_BREAK_RATIO_END_TRIM] - first;
    int preceding = (int)parameters[TA_SHORT_BREAK_PRECEDING_FIRST_BASE];
    int position = (int)parameters[TA_SHORT_BREAK_SCAN_RESULT_BIAS];
    for (int i = 0; i < n; i++)
        if (lr.v[first + i] < lt && mr.v[first + i] < mt &&
            strong_preceding_break(low, middle, preceding + i, parameters)) {
            int bar = recovered_break_position(low, position + i, parameters);
            if (bar) result->v[result->n++] = bar;
        }
}

I short_breaks(ta_context *c, D lowLevels, D middlePeaks, int count, int barCount,
               const double *parameters)
{
    if (count <= 0 || barCount <= 0)
        return (I){ 0 };
    double maximum = d_max(lowLevels);
    double scale = maximum == 0 ? parameters[TA_SHORT_BREAK_NORMALIZE_ZERO] : 1 / maximum;
    D low = ta_new_doubles(c, count), middle = ta_new_doubles(c, count),
      lr = ta_new_doubles(c, count), mr = ta_new_doubles(c, count), lf = ta_new_doubles(c, count),
      mf = ta_new_doubles(c, count);
    for (int i = 0; i < ta_min(count, lowLevels.n); i++)
        low.v[i] = lowLevels.v[i] * scale;
    for (int i = 0; i < ta_min(count, middlePeaks.n); i++)
        middle.v[i] = middlePeaks.v[i];
    int lc, mc;
    short_break_ratios(low, middle, lr, mr, lf, mf, count, &lc, &mc, parameters);
    double low_ratio = lc == 0 ? parameters[TA_SHORT_BREAK_LOW_EMPTY_RATIO] : d_sum(lf, parameters) / lc;
    double middle_ratio = mc == 0 ? parameters[TA_SHORT_BREAK_MIDDLE_EMPTY_RATIO] : d_sum(mf, parameters) / mc;
    double lt = capped_break_threshold(c, lr, count, low_ratio * parameters[TA_SHORT_BREAK_LOW_RATIO_SCALE],
                                       parameters);
    double mt = middle_ratio * parameters[TA_SHORT_BREAK_MIDDLE_RATIO_SCALE];
    I result = ta_new_ints(c, ta_max(count, barCount));
    result.n = 0;
    collect_short_breaks(low, middle, lr, mr, lt, mt, count, &result, parameters);
    if (!result.n)
        result.n = barCount;
    return result;
}
static I retain_interior_candidates(I all, int limit)
{
    int out = 0;
    for (int i = 0; i < all.n; i++)
        if (all.v[i] != 0 && all.v[i] <= limit)
            all.v[out++] = all.v[i];
    all.n = out;
    return all;
}

I adjust_candidates(ta_context *c, I base, I additions, int phase, int bars,
                    const double *parameters)
{
    I extra = i_copy(c, additions);
    double adjacency = parameters[TA_BOUNDARY_ADJUST_ADJACENCY];
    for (int i = 0; i < additions.n - 1; i++)
        if (additions.v[i + 1] == additions.v[i] + adjacency)
            extra.v[i] = 0;
    extra = i_sorted(c, i_compact(c, extra, false), false);
    I coarse = i_copy(c, base);
    int bar_width = (int)parameters[TA_BOUNDARY_ADJUST_BAR_ROUND_NEGATIVE] + 1;
    int first_bar = (int)parameters[TA_BOUNDARY_ADJUST_FIRST_BAR];
    for (int i = 0; i < coarse.n; i++)
        coarse.v[i] = (coarse.v[i] - phase) / bar_width + first_bar;
    double before = parameters[TA_BOUNDARY_ADJUST_BEFORE], after = parameters[TA_BOUNDARY_ADJUST_AFTER];
    for (int i = 1; i < coarse.n; i++)
        for (int j = 0; j < extra.n; j++) {
            int delta = extra.v[j] - coarse.v[i];
            if (delta >= -before && delta <= after)
                extra.v[j] = 0;
        }
    I all = i_join(c, coarse, extra);
    all = retain_interior_candidates(all, bars - (int)parameters[TA_BOUNDARY_ADJUST_TAIL]);
    all = i_sorted(c, all, false);
    int origin = phase - (int)parameters[TA_BOUNDARY_ADJUST_OUTPUT_ORIGIN];
    for (int i = 0; i < all.n; i++)
        all.v[i] = origin + all.v[i] * bar_width;
    return all;
}
static void secure_levels(ta_context *c, D low, int phase, D levels, D differences,
                          const double *parameters)
{
    if (low.n < parameters[TA_SECURE_LEVEL_INPUT_MIN])
        return;
    int bars = ta_max(1, levels.n - (int)parameters[TA_SECURE_LEVEL_TAIL_BARS]);
    int beats = (int)parameters[TA_SECURE_LEVEL_BEATS_PER_BAR];
    double scale = parameters[TA_SECURE_LEVEL_AVERAGE_SCALE];
    for (int i = 0; i < bars; i++) {
        int start = phase - 1 + i * beats;
        levels.v[i] = d_sum(d_view(c, low, start, start + beats), parameters) * scale;
    }
    for (int i = 0; i < bars; i++)
        differences.v[i] = levels.v[i + 1] - levels.v[i];
}

static bool sustained_secure_level(D levels, int candidate, double min, double max,
                                   const double *parameters)
{
    int first = candidate + (int)parameters[TA_SECURE_SUSTAIN_FIRST_OFFSET];
    int last = candidate + (int)parameters[TA_SECURE_SUSTAIN_FINAL_OFFSET];
    return last < levels.n && levels.v[first] >= min && levels.v[first] <= max &&
           levels.v[last] >= min && levels.v[last] <= max;
}

static void secure_candidates(D low, int n, D levels, D differences, I *positions,
                              const double *parameters)
{
    double threshold = d_max(differences) * parameters[TA_SECURE_RISE_SCALE];
    if (low.n >= parameters[TA_SECURE_MIN_BEATS])
        for (int i = 0; i < ta_max((int)parameters[TA_SECURE_CANDIDATE_MIN_BARS], n) -
                            (int)parameters[TA_SECURE_CANDIDATE_TAIL_BARS]; i++)
            if (differences.v[i] > threshold && differences.v[i] > parameters[TA_SECURE_MIN_RISE]) {
                int candidate = i + 1;
                double min = levels.v[candidate] * parameters[TA_SECURE_SUSTAIN_MIN];
                double max = levels.v[candidate] * parameters[TA_SECURE_SUSTAIN_MAX];
                if (sustained_secure_level(levels, candidate, min, max, parameters))
                    positions->v[positions->n++] = candidate + 1;
            }
}

Anchors secure_points(ta_context *c, D low, int phase, const double *parameters)
{
    int beats_per_bar = (int)parameters[TA_SECURE_LEVEL_BEATS_PER_BAR];
    int n = low.n / beats_per_bar;
    D levels = ta_new_doubles(c, n), differences = ta_new_doubles(c, n);
    I positions = ta_new_ints(c, ta_max(1, n));
    positions.n = 0;
    secure_levels(c, low, phase, levels, differences, parameters);
    secure_candidates(low, n, levels, differences, &positions, parameters);
    if (!positions.n)
        positions.n = 1;
    I beats = ta_new_ints(c, positions.n), opening = ta_new_ints(c, positions.n);
    opening.n = 0;
    for (int i = 0; i < positions.n; i++) {
        beats.v[i] = positions.v[i] == 0 ? 0 : phase - beats_per_bar + beats_per_bar * positions.v[i];
        if (beats.v[i] > 0 && beats.v[i] < parameters[TA_SECURE_OPENING_BEAT_LIMIT])
            opening.v[opening.n++] = beats.v[i];
    }
    if (positions.n == 1 && positions.v[0] == 0)
        opening.n = 1;
    return (Anchors){ levels, differences, positions, beats, opening };
}
static Candidates initial_build(ta_context *c, Anchors a, int phase, const double *alternative,
                                const double *parameters)
{
    Candidates r = { ta_new_ints(c, a.positions.n * 2 + 1), ta_new_ints(c, a.positions.n * 2 + 1) };
    r.beats.n = r.roles.n = 1;
    r.beats.v[0] = (int)parameters[TA_INITIAL_OPEN_BEAT];
    r.roles.v[0] = (int)parameters[TA_INITIAL_OPEN_ROLE];
    int width = (int)parameters[TA_INITIAL_BEATS_PER_BAR];
    int previous = 0;
    for (int i = 0; i < a.positions.n; i++) {
        int coarse = a.positions.v[i], phase_beat = phase + coarse * width;
        if (i == 0 ? coarse >= alternative[1] : coarse - previous >= alternative[2]) {
            r.beats.v[r.beats.n++] = phase_beat - (int)alternative[0];
            r.roles.v[r.roles.n++] = (int)parameters[TA_INITIAL_PRECEDING_ROLE];
        }
        r.beats.v[r.beats.n++] = phase_beat - width;
        r.roles.v[r.roles.n++] = (int)parameters[TA_INITIAL_HIGH_ROLE];
        previous = coarse;
    }
    return r;
}
Alternatives initial_alternatives(ta_context *c, Anchors a, int phase, const double *parameters)
{
    if (!a.positions.n || !a.positions.v[0])
        return (Alternatives){ 0 };
    return (Alternatives){ initial_build(c, a, phase, parameters + TA_INITIAL_SHORT_PHASE_SPAN, parameters),
                           initial_build(c, a, phase, parameters + TA_INITIAL_LONG_PHASE_SPAN, parameters) };
}
static void noise_means(ta_context *c, D levels, D means, int width, const double *parameters)
{
    if (levels.n < parameters[TA_NOISE_MEAN_MIN_INPUT]) return;
    int end = ta_max(1, means.n - (int)parameters[TA_NOISE_MEAN_TAIL_BARS]);
    double scale = parameters[TA_NOISE_MEAN_SCALE];
    for (int i = 0; i < end; i++)
        means.v[i] = d_sequential(d_view(c, levels, i * width, (i + 1) * width)) * scale;
}

static void noise_ratios(D means, D contrast, int input_samples, const double *parameters)
{
    if (input_samples < parameters[TA_NOISE_RATIO_MIN_INPUT]) return;
    double zero_level = parameters[TA_NOISE_ZERO_LEVEL];
    double numerator = parameters[TA_NOISE_RATIO_INVERSE_NUMERATOR];
    int first = (int)parameters[TA_NOISE_RATIO_FIRST];
    int back = (int)parameters[TA_NOISE_RATIO_PREVIOUS_BACK];
    int end = means.n - (int)parameters[TA_NOISE_MEAN_TAIL_BARS];
    for (int i = first; i < end; i++) {
        if (means.v[i - back] == 0)
            means.v[i - back] = zero_level;
        if (means.v[i] == 0)
            means.v[i] = zero_level;
        double ratio = means.v[i] / means.v[i - back];
        contrast.v[i] = ratio < numerator ? numerator / ratio : ratio;
    }
}

static void noise_detect_changes(ta_context *c, D contrast, I result, const double *parameters)
{
    int offset = (int)parameters[TA_NOISE_SCAN_CONTRAST_OFFSET];
    int first = (int)parameters[TA_NOISE_RECOVERY_FIRST];
    int last = (int)parameters[TA_NOISE_RECOVERY_LAST];
    int advance = (int)parameters[TA_NOISE_POSITION_ADVANCE];
    double scale = parameters[TA_NOISE_RECOVERY_SCALE];
    int written = 0;
    for (int i = 0; i < contrast.n - last; i++) {
        double change = contrast.v[i + offset];
        if (change == 0 || change < parameters[TA_NOISE_CHANGE_RATIO]) continue;
        D recovery = d_view(c, contrast, i + first, i + last + 1);
        if (d_sum(recovery, parameters) * scale <= parameters[TA_NOISE_RECOVERY_RATIO])
            result.v[written++] = i + advance;
    }
}

I noise_changes(ta_context *c, D levels, const double *parameters)
{
    int width = (int)parameters[TA_NOISE_GROUP_BEATS];
    int n = levels.n / width;
    I result = ta_new_ints(c, n);
    if (levels.n < parameters[TA_NOISE_SCAN_MIN_INPUT]) return result;
    D means = ta_new_doubles(c, n), contrast = ta_new_doubles(c, n);
    noise_means(c, levels, means, width, parameters);
    noise_ratios(means, contrast, levels.n, parameters);
    noise_detect_changes(c, contrast, result, parameters);
    ta_release(c, contrast.v);
    ta_release(c, means.v);
    return result;
}

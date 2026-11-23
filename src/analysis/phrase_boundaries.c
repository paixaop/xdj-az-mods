/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double nonzero(double x) { return x == 0 ? 0.003 : x; }
static double mean(ta_context *c, D a, int start, int count)
{
    return d_sum(d_view(c, a, start, start + count)) / count;
}
RatioSeries ratio_series(ta_context *c, D input)
{
    int n = input.n;
    RatioSeries r = { d_copy(c, input), ta_new_doubles(c, n), ta_new_doubles(c, n) };
    double maximum = d_max(input), scale = maximum == 0 ? 333.3333333333333 : 1 / maximum;
    for (int i = 0; i < n - 1; i++)
        r.levels.v[i] *= scale;
    for (int i = 1; i < n - 1; i++) {
        r.levels.v[i - 1] = nonzero(r.levels.v[i - 1]);
        r.levels.v[i] = nonzero(r.levels.v[i]);
        double ratio = r.levels.v[i] / r.levels.v[i - 1];
        r.direction.v[i] = ratio;
        r.contrast.v[i] = ratio >= 1 ? ratio : 1 / ratio;
    }
    return r;
}
CandidateFeatures candidate_features(ta_context *c, D lowPeaks, D lowLevels, D middlePeaks,
                                     D middleCoarse, int phase)
{
    int n = ta_max(0, (lowPeaks.n - phase + 1) / 4);
    D low = ta_new_doubles(c, n);
    if (lowPeaks.n - phase + 1 >= 8)
        for (int i = 0; i < ta_max(1, n - 1); i++)
            low.v[i] = mean(c, lowPeaks, phase - 1 + i * 4, 4);
    return (CandidateFeatures){ ratio_series(c, low), ratio_series(c, lowLevels),
                                ratio_series(c, middleCoarse), ratio_series(c, middlePeaks) };
}
static RatioSeries ratio_slice(ta_context *c, RatioSeries p, int start, int count)
{
    return (RatioSeries){ d_view(c, p.levels, start, start + count),
                          d_view(c, p.contrast, start, start + count),
                          d_view(c, p.direction, start, start + count) };
}
static int replace_adjacent_peaks(RatioSeries p, I ranked, I strongest, I retained)
{
    int replacements = 0;
    for (int i = 0; i < 3; i++) {
        int position = strongest.v[i], next = strongest.v[i + 1];
        if (position - next == 1 && ranked.n > replacements + 4) {
            double divisor = p.contrast.v[next + 2],
                   ratio = divisor == 0 ? p.contrast.v[position + 2] * 333.3333333333333
                                        : p.contrast.v[position + 2] / divisor;
            if (ratio < 1.7 && p.direction.v[position + 2] <= 1.8) {
                retained.v[i] = 0;
                replacements++;
            }
            if (p.direction.v[position + 2] > 1.8) {
                retained.v[i + 1] = 0;
                replacements++;
            }
        }
    }
    return replacements;
}

static bool local_peak_shape(ta_context *c, RatioSeries p, int candidate, double avg, bool strongestLevel, double level)
{
    double ceiling = avg * 1.0573000000000001;
    if (!((mean(c, p.contrast, candidate, 2) < ceiling &&
           mean(c, p.contrast, candidate - 1, 3) < ceiling) ||
          (strongestLevel && level / nonzero(p.levels.v[candidate + 1]) > 1.1)))
        return false;
    return true;
}

static bool following_peaks_dominate(ta_context *c, RatioSeries p, int candidate, double avg, double strength)
{
    double following = nonzero(mean(c, p.contrast, candidate + 3, 2)),
           next = nonzero(mean(c, p.contrast, candidate + 4, 2));
    if (following >= 1.2 && strength / following <= avg && next >= 1.2 &&
        strength / next <= avg &&
        (strength <= 1.4 ||
         (p.contrast.v[candidate + 5] + p.contrast.v[candidate + 3]) * 0.5 >= 1.2))
        return true;
    return false;
}

static bool local_peak_survives(ta_context *c, RatioSeries p, int candidate, int maximum, double avg)
{
    if (candidate == 0)
        return false;
    double level = p.levels.v[candidate + 2], strength = p.contrast.v[candidate + 2];
    bool strongestLevel = maximum == candidate;
    if (!(strength > avg * 1.05 || strongestLevel))
        return false;
    double change = level / nonzero(mean(c, p.levels, candidate - 1, 3));
    if (!(change > avg * 1.09 || change < 0.9174311926605504 / avg || strongestLevel))
        return false;
    if (!local_peak_shape(c, p, candidate, avg, strongestLevel, level)) return false;
    return !following_peaks_dominate(c, p, candidate, avg, strength);
}

static void trim_leading_zeros(I *result)
{
    for (int i = 0; i < 3 && result->n && result->v[0] == 0; i++)
        i_remove(result, 0);
}

static I finish_local_candidates(ta_context *c, I result)
{
    while (result.n && result.v[result.n - 1] == 0)
        result.n--;
    if (!result.n) {
        result.n = 1;
        result.v[0] = 0;
        return result;
    }
    result = i_sorted(c, result, false);
    trim_leading_zeros(&result);
    if (result.n && result.v[0] == 0)
        result.n = 1;
    return result;
}

I local_candidates(ta_context *c, RatioSeries p, int firstBar)
{
    int n = p.levels.n;
    if (n < 10 || p.contrast.n != n || p.direction.n != n)
        return (I){ 0 };
    int interior = n - 6, maximum = 1;
    double avg = nonzero(mean(c, p.contrast, 3, interior));
    for (int i = 1; i < interior; i++)
        if (p.levels.v[i + 3] > p.levels.v[maximum + 2])
            maximum = i + 1;
    I ranked = rank_d(c, d_view(c, p.contrast, 3, 3 + interior), true);
    for (int i = 0; i < ranked.n; i++)
        ranked.v[i]++;
    I strongest = i_sorted(c, (I){ ranked.v, 4 }, true), retained = i_copy(c, strongest);
    int replacements = replace_adjacent_peaks(p, ranked, strongest, retained);
    memcpy(ranked.v, retained.v, 4 * sizeof(int));
    I result = ta_new_ints(c, 4 + replacements);
    for (int i = 0; i < result.n; i++) {
        int candidate = ranked.v[i];
        if (!local_peak_survives(c, p, candidate, maximum, avg)) continue;
        result.v[i] = firstBar - 1 + candidate + 3;
    }
    return finish_local_candidates(c, result);
}
static I coarse_hook_seeds(ta_context *c, I hypotheses, int phase, int n)
{
    I seeds = ta_new_ints(c, ta_max(1, hypotheses.n));
    seeds.n = hypotheses.n;
    for (int i = 0; i < seeds.n; i++) {
        int coarse = (int)((hypotheses.v[i] - phase) * 0.25 + 1.5);
        seeds.v[i] = coarse <= n ? coarse : 0;
    }
    seeds = i_sorted(c, seeds, false);
    for (int i = 0; i < seeds.n - 1; i++)
        if (seeds.v[i] == seeds.v[i + 1])
            seeds.v[i] = 0;
    seeds = i_sorted(c, seeds, false);
    return seeds;
}

static I trim_hook_seeds(ta_context *c, I seeds, int intro)
{
    int removed = 0;
    while (removed < 4 && seeds.n && seeds.v[0] < 1) {
        i_remove(&seeds, 0);
        removed++;
    }
    if (!seeds.n) {
        seeds = ta_new_ints(c, 1);
    }
    if (4 - removed >= 2 && intro - seeds.v[0] == 1)
        i_remove(&seeds, 0);
    if (!seeds.n)
        seeds = ta_new_ints(c, 1);
    return seeds;
}

static void add_hook_window(ta_context *c, RatioSeries profiles[3], int start, int end, int n, int route, I *additions)
{
    int length = end - start + 1;
    if (start >= 1 && length >= 0 && start - 1 + length <= n)
        for (int k = 0; k < 3; k++)
            if (route != 1 || k == 1) {
                I values = local_candidates(
                    c, ratio_slice(c, profiles[k], start - 1, length), start);
                for (int j = 0; j < values.n; j++)
                    additions->v[additions->n++] = values.v[j];
            }
}

static I hook_windows(ta_context *c, RatioSeries profiles[3], I seeds, int n, int route)
{
    int last = ta_max(seeds.v[seeds.n - 1], 3), end = ta_min(last + 19, n),
        start = ta_min(last - 2, end);
    I additions = ta_new_ints(c, n * 3 + 24);
    additions.n = 0;
    if (n - last - 1 > -16)
        for (int iteration = 0; iteration < (n - last - 1) / 16 + 1; iteration++) {
            add_hook_window(c, profiles, start, end, n, route, &additions);
            if (n - end < 10)
                break;
            end += 16;
            if (end >= n)
                end = n - 1;
            start += 16;
        }
    return additions;
}

static I merge_hook_seeds(ta_context *c, I seeds, I additions)
{
    I combined = i_join(c, seeds, additions), surviving = i_copy(c, combined);
    for (int i = 0; i < combined.n - 1; i++)
        if (combined.v[i] != 0 && combined.v[i] == combined.v[i + 1] - 1)
            surviving.v[i] = 0;
    surviving = i_compact(c, surviving, false);
    surviving = i_sorted(c, surviving, false);
    if (!surviving.n || surviving.v[0] != 1) {
        int one = 1;
        surviving = i_join(c, (I){ &one, 1 }, surviving);
    }
    return surviving;
}

static void score_hooks(RatioSeries profiles[3], I surviving, D scores, int n)
{
    for (int i = 0; i < surviving.n; i++) {
        int position = surviving.v[i], index = position - 1;
        if (index < 0 || index >= n || position == 1)
            continue;
        double score = fma(profiles[1].contrast.v[index] + profiles[0].contrast.v[index], 1.05,
                           profiles[2].contrast.v[index] * 0.9);
        scores.v[i] = position > 2 ? (profiles[1].contrast.v[index - 1] + score) * 0.25
                                   : score * 0.3333333333333333;
    }
}

static void prune_close_hooks(I surviving, I original, D scores, int intro)
{
    for (int i = 2; i < surviving.n; i++) {
        scores.v[i - 1] = nonzero(scores.v[i - 1]);
        scores.v[i] = nonzero(scores.v[i]);
        double previous = scores.v[i - 1], current = scores.v[i];
        if (ta_dmax(current / previous, previous / current) >= 1.07 &&
            original.v[i] - original.v[i - 1] < 4) {
            int remove = current <= previous ? i : i - 1, position = surviving.v[remove];
            if (position != 5 && position != 9 && position != intro && scores.v[remove] < 1.6)
                surviving.v[remove] = 0;
        }
    }
}

I verse_hook_candidates(ta_context *c, CandidateFeatures f, I hypotheses, int introEnd, int phase,
                        int route)
{
    int n = ta_min(f.lowPeak.levels.n, ta_min(f.lowLevel.levels.n, f.middleCoarse.levels.n));
    if (n <= 0)
        return (I){ 0 };
    RatioSeries profiles[] = { ratio_slice(c, f.lowPeak, 0, n), ratio_slice(c, f.lowLevel, 0, n),
                               ratio_slice(c, f.middleCoarse, 0, n) };
    I seeds = coarse_hook_seeds(c, hypotheses, phase, n);
    int intro = (int)((introEnd - phase) * 0.25 + 1.5);
    seeds = trim_hook_seeds(c, seeds, intro);
    I additions = hook_windows(c, profiles, seeds, n, route);
    additions = i_compact(c, additions, true);
    I surviving = merge_hook_seeds(c, seeds, additions);
    I original = i_copy(c, surviving);
    D scores = ta_new_doubles(c, surviving.n);
    score_hooks(profiles, surviving, scores, n);
    prune_close_hooks(surviving, original, scores, intro);
    surviving = i_compact(c, surviving, false);
    for (int i = 0; i < surviving.n; i++)
        surviving.v[i] = phase - 4 + surviving.v[i] * 4;
    return surviving;
}
static void short_break_ratios(D low, D middle, D lr, D mr, D lf, D mf, int count, int *lc, int *mc)
{
    *lc = *mc = 0;
    for (int i = 1; i < count - 4; i++) {
        low.v[i] = nonzero(low.v[i]);
        low.v[i - 1] = nonzero(low.v[i - 1]);
        lr.v[i] = low.v[i] / low.v[i - 1];
        if (lr.v[i] >= 1)
            (*lc)++;
        else
            lf.v[i] = lr.v[i];
        middle.v[i] = nonzero(middle.v[i]);
        middle.v[i - 1] = nonzero(middle.v[i - 1]);
        mr.v[i] = middle.v[i] / middle.v[i - 1];
        if (mr.v[i] >= 1)
            (*mc)++;
        else
            mf.v[i] = mr.v[i];
    }
}

static double capped_break_threshold(ta_context *c, D lr, int count, double lt)
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
    if (small > 13)
        lt = values.v[order.v[12]];
    return lt;
}

static int recovered_break_position(D low, int i)
{
    int p = i + 1;
    if (p % 4 == 1 || p % 4 == 2)
        return 0;
    double s = low.v[p - 1] == 0 ? 333.3333333333333 : 1 / low.v[p - 1];
    bool keep = false;
    for (int j = p; j < p + 4; j++)
        if (low.v[j] * s > 2.5)
            keep = true;
    if (keep)
        return p / 4 + (p % 4 == 0 ? 1 : 2);
    return 0;
}

static bool strong_preceding_break(D low, D middle, int i)
{
    return ((low.v[i - 1] > 0.5 && (middle.v[i - 1] > 0.2 || middle.v[i - 2] > 0.2)) ||
            (low.v[i - 1] > 0.2 && (middle.v[i - 1] > 0.5 || middle.v[i - 2] > 0.5)));
}

static void collect_short_breaks(D low, D middle, D lr, D mr, double lt, double mt, int count, I *result)
{
    for (int i = 16; i < count - 4; i++)
        if (lr.v[i] < lt && mr.v[i] < mt &&
            strong_preceding_break(low, middle, i)) {
            int p = recovered_break_position(low, i);
            if (p) result->v[result->n++] = p;
        }
}

I short_breaks(ta_context *c, D lowLevels, D middlePeaks, int count, int barCount)
{
    if (count <= 0 || barCount <= 0)
        return (I){ 0 };
    double maximum = d_max(lowLevels), scale = maximum == 0 ? 333.3333333333333 : 1 / maximum;
    D low = ta_new_doubles(c, count), middle = ta_new_doubles(c, count),
      lr = ta_new_doubles(c, count), mr = ta_new_doubles(c, count), lf = ta_new_doubles(c, count),
      mf = ta_new_doubles(c, count);
    for (int i = 0; i < ta_min(count, lowLevels.n); i++)
        low.v[i] = lowLevels.v[i] * scale;
    for (int i = 0; i < ta_min(count, middlePeaks.n); i++)
        middle.v[i] = middlePeaks.v[i];
    int lc, mc;
    short_break_ratios(low, middle, lr, mr, lf, mf, count, &lc, &mc);
    double lt = (lc == 0 ? 0.003 : d_sum(lf) / lc) * 0.65,
           mt = (mc == 0 ? 0.003 : d_sum(mf) / mc) * 0.95;
    lt = capped_break_threshold(c, lr, count, lt);
    I result = ta_new_ints(c, ta_max(count, barCount));
    result.n = 0;
    collect_short_breaks(low, middle, lr, mr, lt, mt, count, &result);
    if (!result.n)
        result.n = barCount;
    return result;
}
static I retain_interior_candidates(I all, int bars)
{
    int out = 0;
    for (int i = 0; i < all.n; i++)
        if (all.v[i] != 0 && all.v[i] <= bars - 4)
            all.v[out++] = all.v[i];
    all.n = out;
    return all;
}

I adjust_candidates(ta_context *c, I base, I additions, int phase, int bars)
{
    I extra = i_copy(c, additions);
    for (int i = 0; i < additions.n - 1; i++)
        if (additions.v[i + 1] == additions.v[i] + 1)
            extra.v[i] = 0;
    extra = i_sorted(c, i_compact(c, extra, false), false);
    I coarse = i_copy(c, base);
    for (int i = 0; i < coarse.n; i++)
        coarse.v[i] = (coarse.v[i] - phase) / 4 + 1;
    for (int i = 1; i < coarse.n; i++)
        for (int j = 0; j < extra.n; j++)
            if (abs(extra.v[j] - coarse.v[i]) <= 1)
                extra.v[j] = 0;
    I all = i_join(c, coarse, extra);
    all = retain_interior_candidates(all, bars);
    all = i_sorted(c, all, false);
    for (int i = 0; i < all.n; i++)
        all.v[i] = phase - 4 + all.v[i] * 4;
    return all;
}
static void secure_levels(ta_context *c, D low, int phase, int n, D levels, D differences)
{
    if (low.n >= 8) {
        for (int i = 0; i < ta_max(1, n - 1); i++)
            levels.v[i] = mean(c, low, phase - 1 + i * 4, 4);
        for (int i = 0; i < ta_max(1, n - 1); i++)
            differences.v[i] = levels.v[i + 1] - levels.v[i];
    }
}

static bool sustained_secure_level(D levels, int candidate, int n, double min, double max)
{
    return levels.v[candidate + 1] >= min && levels.v[candidate + 1] <= max &&
           candidate + 2 < n && levels.v[candidate + 2] >= min &&
           levels.v[candidate + 2] <= max;
}

static void secure_candidates(D low, int n, D levels, D differences, I *positions)
{
    double threshold = d_max(differences) * 0.6;
    if (low.n >= 12)
        for (int i = 0; i < ta_max(3, n) - 2; i++)
            if (differences.v[i] > threshold && differences.v[i] > 0.15) {
                int candidate = i + 1;
                double min = levels.v[candidate] * 0.7, max = levels.v[candidate] * 1.3;
                if (sustained_secure_level(levels, candidate, n, min, max))
                    positions->v[positions->n++] = candidate + 1;
            }
}

Anchors secure_points(ta_context *c, D low, int phase)
{
    int n = low.n / 4;
    D levels = ta_new_doubles(c, n), differences = ta_new_doubles(c, n);
    I positions = ta_new_ints(c, ta_max(1, n));
    positions.n = 0;
    secure_levels(c, low, phase, n, levels, differences);
    secure_candidates(low, n, levels, differences, &positions);
    if (!positions.n)
        positions.n = 1;
    I beats = ta_new_ints(c, positions.n), opening = ta_new_ints(c, positions.n);
    opening.n = 0;
    for (int i = 0; i < positions.n; i++) {
        beats.v[i] = positions.v[i] == 0 ? 0 : phase - 4 + 4 * positions.v[i];
        if (beats.v[i] > 0 && beats.v[i] < 200)
            opening.v[opening.n++] = beats.v[i];
    }
    if (positions.n == 1 && positions.v[0] == 0)
        opening.n = 1;
    return (Anchors){ levels, differences, positions, beats, opening };
}
static Candidates initial_build(ta_context *c, Anchors a, int phase, int preceding, int threshold,
                                int gap)
{
    Candidates r = { ta_new_ints(c, a.positions.n * 2 + 1), ta_new_ints(c, a.positions.n * 2 + 1) };
    r.beats.n = r.roles.n = 1;
    r.beats.v[0] = 1;
    r.roles.v[0] = 10;
    int previous = 0;
    for (int i = 0; i < a.positions.n; i++) {
        int coarse = a.positions.v[i], beat = phase - 4 + coarse * 4;
        if (i == 0 ? coarse >= threshold : coarse - previous >= gap) {
            r.beats.v[r.beats.n++] = beat - preceding;
            r.roles.v[r.roles.n++] = 30;
        }
        r.beats.v[r.beats.n++] = beat;
        r.roles.v[r.roles.n++] = 40;
        previous = coarse;
    }
    return r;
}
Alternatives initial_alternatives(ta_context *c, Anchors a, int phase)
{
    if (!a.positions.n || !a.positions.v[0])
        return (Alternatives){ 0 };
    return (Alternatives){ initial_build(c, a, phase, 32, 10, 9),
                           initial_build(c, a, phase, 64, 18, 17) };
}
I noise_changes(ta_context *c, D levels)
{
    int n = levels.n / 4;
    D means = ta_new_doubles(c, n), contrast = ta_new_doubles(c, n);
    I result = ta_new_ints(c, n);
    int written = 0;
    for (int i = 0; i < n - 1; i++)
        means.v[i] = d_sequential(d_view(c, levels, i * 4, i * 4 + 4)) * 0.25;
    for (int i = 1; i < n - 1; i++) {
        means.v[i - 1] = nonzero(means.v[i - 1]);
        means.v[i] = nonzero(means.v[i]);
        double ratio = means.v[i] / means.v[i - 1];
        contrast.v[i] = ratio < 1 ? 1 / ratio : ratio;
    }
    for (int i = 0; i < n - 4; i++)
        if (contrast.v[i + 1] >= 1.8 && (contrast.v[i + 3] + contrast.v[i + 4]) * 0.5 <= 1.11)
            result.v[written++] = i + 2;
    return result;
}

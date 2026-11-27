/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double nonzero_level(double value, double zero_level)
{
    return value == 0 ? zero_level : value;
}
static double sum(D a, int low, int high, const double *parameters)
{
    if (low > high || low >= a.n || high < 0)
        return 0;
    int start = ta_max(0, low), end = ta_min(a.n - 1, high);
    return d_sum((D){ a.v + start, end - start + 1 }, parameters);
}
static D normalized(ta_context *c, D input, bool guard, const double *parameters)
{
    double maximum = d_max(input), factor = guard && maximum == 0 ? parameters[TA_OPENING_LEVEL_ZERO_INVERSE] : 1 / maximum;
    D result = d_copy(c, input);
    for (int i = 0; i < input.n - 1; i++)
        result.v[i] *= factor == 0 ? parameters[TA_OPENING_LEVEL_ZERO_RECIPROCAL] : factor;
    return result;
}
static D ratios(ta_context *c, D values, const double *parameters)
{
    D out = ta_new_doubles(c, (int)parameters[TA_OPENING_RATIO_BARS]);
    double zero_level = parameters[TA_OPENING_LEVEL_RATIO_ZERO];
    for (int i = 1; i < out.n; i++)
        out.v[i] = nonzero_level(values.v[i], zero_level) /
                   nonzero_level(values.v[i - 1], zero_level);
    return out;
}
static D symmetric(ta_context *c, D ratios)
{
    D out = d_copy(c, ratios);
    for (int i = 0; i < out.n; i++)
        if (out.v[i] < 1)
            out.v[i] = 1 / out.v[i];
    return out;
}
typedef struct {
    D shortDeviation, longDeviation, priorDeviation, contrast;
} EnvelopeDeviation;
typedef struct {
    I values;
    int capacity;
} EnvelopeSelection;

static double envelope_range_sum(D level, int first, int last, const double *parameters)
{
    if (first < 0 || first > last || last >= level.n)
        return 0;
    return d_sum((D){ level.v + first, last - first + 1 }, parameters);
}

static D envelope_levels(ta_context *c, D envelope, int phase, const double *parameters)
{
    int width = (int)parameters[TA_OPENING_ENVELOPE_GROUP_BEATS];
    int last_delta = (int)parameters[TA_OPENING_ENVELOPE_GROUP_LAST_DELTA];
    double scale = parameters[TA_OPENING_ENVELOPE_GROUP_MEAN_SCALE];
    int first = phase - (int)parameters[TA_OPENING_ENVELOPE_PHASE_ORIGIN];
    int n = (envelope.n - first) / width;
    int available = n - (int)parameters[TA_OPENING_ENVELOPE_GROUP_OMITTED];
    D level = ta_new_doubles(c, n);
    for (int i = 0; i < available; i++) {
        int start = first + i * width;
        level.v[i] = envelope_range_sum(envelope, start, start + last_delta, parameters) * scale;
    }
    double factor = parameters[TA_OPENING_ENVELOPE_NORMALIZE_NUMERATOR] / d_max(level);
    if (factor == parameters[TA_OPENING_ENVELOPE_NORMALIZE_FALLBACK_TEST])
        factor = parameters[TA_OPENING_ENVELOPE_NORMALIZE_ZERO];
    for (int i = 0; i < available; i++)
        level.v[i] *= factor;
    return level;
}
static I envelope_changes(ta_context *c, D level, const double *parameters)
{
    int ratio_slots = (int)parameters[TA_OPENING_ENVELOPE_RATIO_SLOTS];
    int scan_slots = (int)parameters[TA_OPENING_ENVELOPE_SCAN_SLOTS];
    if (ratio_slots > level.n)
        ta_fail(c, TA_FAILED, "short opening envelope");
    D changes = ta_new_doubles(c, ratio_slots);
    I candidates = ta_new_ints(c, scan_slots);
    candidates.n = 0;
    for (int i = 1; i < ratio_slots; i++) {
        if (level.v[i - 1] == 0) level.v[i - 1] = parameters[TA_OPENING_ENVELOPE_ZERO_LEVEL];
        changes.v[i] = level.v[i] / level.v[i - 1];
    }
    for (int i = 0; i < scan_slots; i++)
        if (changes.v[i] >= parameters[TA_OPENING_ENVELOPE_CHANGE_MIN] &&
            level.v[i] > parameters[TA_OPENING_ENVELOPE_LEVEL_MIN])
            candidates.v[candidates.n++] = i + 1;
    ta_release(c, changes.v);
    return candidates;
}
static double envelope_nonzero(double level, const double *parameters)
{
    return level == 0 ? parameters[TA_OPENING_ENVELOPE_ZERO_LEVEL] : level;
}

static void envelope_accept(ta_context *c, EnvelopeSelection *accepted, int position)
{
    int count = accepted->values.n;
    if (count == accepted->capacity) {
        I resized = ta_new_ints(c, count + 1);
        memcpy(resized.v, accepted->values.v, (size_t)count * sizeof(int));
        ta_release(c, accepted->values.v);
        accepted->capacity = resized.n;
        accepted->values.v = resized.v;
    }
    accepted->values.v[accepted->values.n++] = position;
}

static double envelope_stability(D level, int start, int last_delta, double scale, double base,
                                  const double *parameters)
{
    double mean = envelope_nonzero(envelope_range_sum(level, start, start + last_delta, parameters) * scale, parameters);
    double ratio = mean / base;
    return ratio < 1 ? base / mean : ratio;
}

static void envelope_stable_levels(ta_context *c, D level, I candidates, EnvelopeSelection *accepted,
                                   const double *parameters)
{
    int short_delta = (int)parameters[TA_OPENING_ENVELOPE_STABILITY_SHORT_DELTA];
    int long_delta = (int)parameters[TA_OPENING_ENVELOPE_STABILITY_LONG_DELTA];
    double short_scale = parameters[TA_OPENING_ENVELOPE_STABILITY_SHORT_SCALE];
    double long_scale = parameters[TA_OPENING_ENVELOPE_STABILITY_LONG_SCALE];
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        double base = envelope_nonzero(level.v[i - 1], parameters);
        double a = envelope_stability(level, i, short_delta, short_scale, base, parameters);
        double b = envelope_stability(level, i, long_delta, long_scale, base, parameters);
        if (a <= parameters[TA_OPENING_ENVELOPE_SHORT_STABILITY_MAX] &&
            b <= parameters[TA_OPENING_ENVELOPE_LONG_STABILITY_MAX])
            envelope_accept(c, accepted, i);
    }
}
static bool envelope_new_candidate(I accepted, int i)
{
    return !accepted.n || accepted.v[accepted.n - 1] != i;
}
static double envelope_long_mean(D level, int start, int width, double scale, const double *parameters)
{
    int available = level.n - start;
    if (available >= width)
        return envelope_range_sum(level, start, start + width - 1, parameters) * scale;
    return available > 0 ? envelope_range_sum(level, start, level.n - 1, parameters) / available : 0;
}

static double envelope_deviation(D level, int start, D scratch, int end_guard, double mean, const double *parameters)
{
    int available = ta_min(scratch.n, level.n - start - end_guard);
    /* Native reuses the scratch tail when a candidate has fewer available bars. */
    for (int j = 0; j < available; j++)
        scratch.v[j] = fabs(level.v[start + j] - mean);
    return d_sum(scratch, parameters) / scratch.n;
}

static void envelope_stable_deviations(ta_context *c, D level, I candidates, EnvelopeSelection *accepted,
                                       EnvelopeDeviation *d,
                                       const double *parameters)
{
    D short_values = ta_new_doubles(c, (int)parameters[TA_OPENING_ENVELOPE_SHORT_DEVIATION_WIDTH]);
    D long_values = ta_new_doubles(c, (int)parameters[TA_OPENING_ENVELOPE_LONG_DEVIATION_WIDTH]);
    double short_scale = parameters[TA_OPENING_ENVELOPE_SHORT_MEAN_SCALE];
    double long_scale = parameters[TA_OPENING_ENVELOPE_LONG_MEAN_SCALE];
    int guard = (int)parameters[TA_OPENING_ENVELOPE_DEVIATION_END_GUARD];
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        double short_mean = envelope_range_sum(level, i, i + short_values.n - 1, parameters) * short_scale;
        double long_mean = envelope_long_mean(level, i, long_values.n, long_scale, parameters);
        double short_deviation = envelope_deviation(level, i, short_values, guard, short_mean, parameters);
        double long_deviation = envelope_deviation(level, i, long_values, guard, long_mean, parameters);
        d->shortDeviation.v[k] = short_deviation;
        d->longDeviation.v[k] = long_deviation;
        if (short_deviation <= parameters[TA_OPENING_ENVELOPE_SHORT_DEVIATION_MAX] &&
            long_deviation <= parameters[TA_OPENING_ENVELOPE_LONG_DEVIATION_MAX] &&
            envelope_new_candidate(accepted->values, i))
            envelope_accept(c, accepted, i);
    }
    ta_release(c, short_values.v);
    ta_release(c, long_values.v);
}
static bool envelope_prior_stable(const EnvelopeDeviation *d, int i, const double *parameters)
{
    return d->shortDeviation.v[i] <= parameters[TA_OPENING_ENVELOPE_PRIOR_SHORT_MAX] &&
           d->longDeviation.v[i] <= parameters[TA_OPENING_ENVELOPE_PRIOR_LONG_MAX] &&
           d->priorDeviation.v[i] <= parameters[TA_OPENING_ENVELOPE_PRIOR_DEVIATION_MAX] &&
           d->contrast.v[i] >= parameters[TA_OPENING_ENVELOPE_PRIOR_CONTRAST_MIN];
}
static void envelope_prior_deviations(ta_context *c, D level, I candidates, EnvelopeSelection *accepted,
                                      EnvelopeDeviation *d, const double *parameters)
{
    int width = (int)parameters[TA_OPENING_ENVELOPE_PRIOR_WIDTH];
    D deviations = ta_new_doubles(c, width);
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k], length = ta_min(width, i - 1);
        D prior = d_view(c, level, i - 1 - length, i - 1);
        double mean = envelope_nonzero(d_sum(prior, parameters) / (double)prior.n, parameters);
        deviations.n = prior.n;
        for (int j = 0; j < prior.n; j++)
            deviations.v[j] = fabs(prior.v[j] - mean);
        d->priorDeviation.v[k] = d_sum(deviations, parameters) / (double)prior.n;
        d->contrast.v[k] = level.v[i - 1] / mean;
        if (envelope_prior_stable(d, k, parameters) && envelope_new_candidate(accepted->values, i))
            envelope_accept(c, accepted, i);
    }
    ta_release(c, deviations.v);
}
static void envelope_relaxed_deviations(ta_context *c, I candidates, EnvelopeSelection *accepted,
                                        const EnvelopeDeviation *d,
                                        const double *parameters)
{
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        if (d->shortDeviation.v[k] <= parameters[TA_OPENING_ENVELOPE_RELAXED_SHORT_MAX] &&
            d->longDeviation.v[k] <= parameters[TA_OPENING_ENVELOPE_RELAXED_LONG_MAX] &&
            d->priorDeviation.v[k] <= parameters[TA_OPENING_ENVELOPE_RELAXED_PRIOR_MAX] &&
            d->contrast.v[k] >= parameters[TA_OPENING_ENVELOPE_RELAXED_CONTRAST_MIN] &&
            envelope_new_candidate(accepted->values, i))
            envelope_accept(c, accepted, i);
    }
}

static int envelope_selected_candidate(ta_context *c, I accepted, const double *parameters)
{
    if (!accepted.n)
        return 0;
    I sorted = i_sorted(c, accepted, false);
    int minimum = sorted.v[0];
    int result = sorted.n > 1 && sorted.v[1] - minimum == parameters[TA_OPENING_ENVELOPE_SUCCESSOR_DISTANCE]
                     ? sorted.v[1]
                     : minimum;
    ta_release(c, sorted.v);
    return result;
}

static int envelope_candidate(ta_context *c, D envelope, int phase, const double *parameters)
{
    D level = envelope_levels(c, envelope, phase, parameters);
    I candidates = envelope_changes(c, level, parameters);
    int capacity = ta_max(candidates.n, 1);
    EnvelopeSelection accepted = { .values = { ta_alloc(c, capacity, sizeof(int)), 0 }, .capacity = capacity };
    EnvelopeDeviation d = { ta_new_doubles(c, candidates.n), ta_new_doubles(c, candidates.n),
                            ta_new_doubles(c, candidates.n), ta_new_doubles(c, candidates.n) };
    envelope_stable_levels(c, level, candidates, &accepted, parameters);
    if (accepted.values.n < candidates.n)
        envelope_stable_deviations(c, level, candidates, &accepted, &d, parameters);
    if (accepted.values.n < candidates.n)
        envelope_prior_deviations(c, level, candidates, &accepted, &d, parameters);
    if (accepted.values.n < candidates.n)
        envelope_relaxed_deviations(c, candidates, &accepted, &d, parameters);
    int result = envelope_selected_candidate(c, accepted.values, parameters);
    ta_release(c, level.v);
    ta_release(c, candidates.v);
    ta_release(c, accepted.values.v);
    ta_release(c, d.shortDeviation.v);
    ta_release(c, d.longDeviation.v);
    ta_release(c, d.priorDeviation.v);
    ta_release(c, d.contrast.v);
    return result;
}
static int positive(D a, int start, int end, const double *parameters)
{
    int count = 0;
    double floor = parameters[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE];
    for (int i = start; i < end; i++)
        if (a.v[i] > floor)
            count++;
    return count;
}
static int vocal_vote_bar(int beat, const double *parameters)
{
    double base = parameters[TA_VOICE_VOTE_BAR_BASE];
    return (int)round((double)(beat - 1) * parameters[TA_VOICE_VOTE_BAR_SCALE] + base);
}
static bool vocal_dense_start(D v, int i, const double *parameters)
{
    double sample_min = parameters[TA_OPENING_VOICE_DENSE_SAMPLE_MIN];
    int width = (int)parameters[TA_VOICE_OPENING_BAR_WIDTH];
    if (!(v.v[i] >= sample_min && (!(i % width) || sum(v, i + 1, i + width - 1, parameters) != 0)))
        return false;
    int end = i + (int)parameters[TA_VOICE_DENSE_LAST_DELTA];
    int count = positive(v, i, end + 1, parameters);
    double mean = sum(v, i, end, parameters) / (double)count;
    return (count >= parameters[TA_OPENING_VOICE_DENSE_COUNT_MIN] &&
            mean >= parameters[TA_OPENING_VOICE_DENSE_MEAN_MIN]) ||
           (count == parameters[TA_OPENING_VOICE_DENSE_FALLBACK_COUNT] && mean >= sample_min);
}

static bool vocal_sustained_pattern(D v, int i, const double *parameters)
{
    int second = (int)(parameters[TA_VOICE_SUSTAINED_SECOND_PAIR] -
                       parameters[TA_VOICE_SUSTAINED_FIRST_PAIR]);
    double floor = parameters[TA_VOICE_SUSTAINED_PATTERN_PRODUCT_MIN_EXCLUSIVE];
    return v.v[i] * v.v[i + second] * v.v[i + second + 1] > floor ||
           v.v[i] * v.v[i + 1] > floor;
}

static bool vocal_sustained_onset(D v, int i, const double *parameters)
{
    int width = (int)(parameters[TA_VOICE_SUSTAINED_END_COUNTER_DELTA] -
                       parameters[TA_VOICE_SUSTAINED_START]) + 1;
    int count = positive(v, i, i + width, parameters);
    double mean = sum(v, i, i + width - 1, parameters) * parameters[TA_VOICE_SUSTAINED_MEAN_RECIPROCAL];
    int prior = (int)parameters[TA_VOICE_SUSTAINED_PRIOR_FIRST];
    return ((count >= parameters[TA_OPENING_VOICE_SUSTAINED_HIGH_COUNT] &&
             mean > parameters[TA_OPENING_VOICE_SUSTAINED_HIGH_MEAN]) ||
            (count >= parameters[TA_OPENING_VOICE_SUSTAINED_LOW_COUNT] &&
             mean > parameters[TA_OPENING_VOICE_SUSTAINED_LOW_MEAN])) &&
           vocal_sustained_pattern(v, i, parameters) &&
           sum(v, prior, i - 1, parameters) / (double)(i - prior) < parameters[TA_OPENING_VOICE_SUSTAINED_PRIOR_MAX];
}
static int vocal_first_onset(D v, const double *parameters)
{
    int start = (int)parameters[TA_VOICE_SUSTAINED_START];
    int end = start + (int)parameters[TA_VOICE_SUSTAINED_COUNT];
    for (int i = start; i < end; i++) {
        if (i >= parameters[TA_OPENING_VOICE_SUSTAINED_QUIET_FROM] &&
            sum(v, 0, i - 1, parameters) != parameters[TA_VOICE_SUSTAINED_QUIET_SUM])
            break;
        if (!(v.v[i] > parameters[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE])) continue;
        if (vocal_sustained_onset(v, i, parameters)) return i + 1;
    }
    return 0;
}
static bool vocal_adjacent_bars(D v, int i, const double *parameters)
{
    bool adjacent = false;
    int previous = 0;
    int width = (int)parameters[TA_VOICE_OPENING_BAR_WIDTH];
    double minimum = parameters[TA_VOICE_BAR_ADJACENT_MIN];
    for (int j = 0; j < i / width; j++) {
        int start = j * width;
        int count = positive(v, start, start + width, parameters);
        if (j && previous >= minimum && count >= minimum) adjacent = true;
        previous = count;
    }
    return adjacent;
}
static bool vocal_dense_bars(D v, int i, const double *parameters)
{
    bool denseNext = true;
    int width = (int)parameters[TA_VOICE_OPENING_BAR_WIDTH];
    int count = (int)parameters[TA_VOICE_BAR_LAST_DELTA] + 1;
    for (int j = 0; j < count; j += width)
        if (positive(v, i + j, i + j + width, parameters) < parameters[TA_OPENING_VOICE_BAR_DENSE_COUNT])
            denseNext = false;
    double total = sum(v, i, i + count - 1, parameters);
    double mean = total * parameters[TA_VOICE_BAR_MEAN_RECIPROCAL];
    return denseNext && mean > parameters[TA_OPENING_VOICE_BAR_DENSE_MEAN] &&
           sum(v, 0, i - 1, parameters) / (double)i < total * parameters[TA_VOICE_BAR_PRIOR_RECIPROCAL];
}
static int vocal_bar_onset(D v, const double *parameters)
{
    int start = (int)parameters[TA_VOICE_BAR_SCAN_START];
    int end = (int)parameters[TA_VOICE_BAR_SCAN_END];
    for (int i = start; i < end; i++) {
        if (vocal_adjacent_bars(v, i, parameters)) break;
        if (!(v.v[i] > parameters[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE])) continue;
        if (vocal_dense_bars(v, i, parameters)) return i + 1;
    }
    return 0;
}
static bool vocal_high_mean(D v, int i, int count, const double *parameters)
{
    double next_min = parameters[TA_OPENING_VOICE_SPARSE_NEXT_MIN];
    int end = i + (int)parameters[TA_VOICE_SPARSE_ACTIVE_LAST];
    int first_neighbor = (int)parameters[TA_VOICE_SPARSE_FIRST_NEIGHBOR];
    int second_neighbor = (int)parameters[TA_VOICE_SPARSE_SECOND_NEIGHBOR];
    return sum(v, i, end, parameters) / (double)count >= parameters[TA_OPENING_VOICE_SPARSE_ACTIVE_MEAN] ||
           (v.v[i] > parameters[TA_OPENING_VOICE_SPARSE_FIRST_MIN] &&
            v.v[i + first_neighbor] > next_min && v.v[i + second_neighbor] > next_min);
}
static int vocal_sparse_onset(D v, const double *parameters)
{
    int back = (int)parameters[TA_VOICE_SPARSE_COUNTER_INDEX_BACK];
    int start = (int)parameters[TA_VOICE_SPARSE_START_COUNTER] - back;
    int end = (int)parameters[TA_VOICE_SPARSE_END_COUNTER] - back;
    int count_width = (int)parameters[TA_VOICE_SPARSE_ACTIVE_LAST] + 1;
    int mean_width = (int)parameters[TA_VOICE_SPARSE_LONG_END_COUNTER_DELTA] + back + 1;
    for (int i = start; i < end; i++)
        if (v.v[i] > parameters[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE]) {
            int count = positive(v, i, i + count_width, parameters);
            if (count >= parameters[TA_OPENING_VOICE_SPARSE_MIN_COUNT] &&
                sum(v, i, i + mean_width - 1, parameters) * parameters[TA_VOICE_SPARSE_MEAN_RECIPROCAL] >=
                    parameters[TA_OPENING_VOICE_SPARSE_WINDOW_MEAN] &&
                sum(v, 0, i - 1, parameters) / (double)i < parameters[TA_OPENING_VOICE_SPARSE_PRIOR_MAX] &&
                vocal_high_mean(v, i, count, parameters))
                return i + 1;
        }
    return 0;
}
static bool vocal_neighbor_bars(int result, int dense_bar, const double *first_pair,
                                const double *second_pair)
{
    return (result == first_pair[0] && dense_bar == first_pair[1]) ||
           (result == second_pair[0] && dense_bar == second_pair[1]);
}
static bool vocal_rounds_up(int result, I dense, const double *parameters)
{
    return dense.n > 1 &&
           vocal_neighbor_bars(result, vocal_vote_bar(dense.v[1], parameters),
                               parameters + TA_VOICE_VOTE_ADVANCE_FIRST_BAR,
                               parameters + TA_VOICE_VOTE_ADVANCE_SECOND_BAR);
}
static bool vocal_prefer_dense(int result, int dense_bar, const double *parameters)
{
    double fallback = parameters[TA_VOICE_VOTE_FALLBACK_BAR];
    bool preceding = vocal_neighbor_bars(result, dense_bar, parameters + TA_VOICE_VOTE_KEEP_FIRST_ONSET,
                                         parameters + TA_VOICE_VOTE_KEEP_SECOND_ONSET);
    bool next = vocal_neighbor_bars(result, dense_bar, parameters + TA_VOICE_VOTE_REPLACE_FIRST_ONSET,
                                    parameters + TA_VOICE_VOTE_REPLACE_SECOND_ONSET);
    return next || (!(dense_bar == fallback || preceding) &&
                    !(result < dense_bar && result != fallback));
}
static int vocal_snap_onset(int result, int onset, I dense, const double *parameters)
{
    if (result <= parameters[TA_VOICE_VOTE_FALLBACK_BAR]) return result;
    double first = onset - parameters[TA_OPENING_SNAP_FIRST_BEAT];
    double second = onset - parameters[TA_OPENING_SNAP_SECOND_BEAT];
    bool near_first = first >= 0 && first < parameters[TA_OPENING_SNAP_FIRST_WIDTH];
    bool near_second = second >= 0 && second < parameters[TA_OPENING_SNAP_SECOND_WIDTH];
    if (near_second)
        return (int)parameters[TA_OPENING_SNAP_SECOND_BAR];
    if (near_first)
        return (int)parameters[TA_OPENING_SNAP_FIRST_BAR];
    return result + vocal_rounds_up(result, dense, parameters);
}
static int vocal_candidate(ta_context *c, D vocal, const double *parameters)
{
    double maximum = d_max(vocal);
    double scale = maximum == 0 ? parameters[TA_OPENING_VOICE_NORMALIZE_ZERO] : 1 / maximum;
    D v = ta_new_doubles(c, (int)parameters[TA_VOICE_VOTE_BUFFER_BEATS]);
    for (int i = 0; i < v.n; i++)
        v.v[i] = vocal.v[i] * scale;
    const int scan = (int)parameters[TA_VOICE_VOTE_DENSE_SCAN_BEATS];
    I dense = ta_new_ints(c, scan);
    dense.n = 0;
    for (int i = 0; i < scan; i++)
        if (vocal_dense_start(v, i, parameters)) dense.v[dense.n++] = i + 1;
    int onset = vocal_first_onset(v, parameters);
    if (!onset) onset = vocal_bar_onset(v, parameters);
    if (!onset) onset = vocal_sparse_onset(v, parameters);
    int result = vocal_vote_bar(onset, parameters), first = dense.n ? dense.v[0] : 0;
    int dense_bar = vocal_vote_bar(first, parameters);
    if (vocal_prefer_dense(result, dense_bar, parameters)) {
        result = dense_bar;
        onset = first;
    }
    result = vocal_snap_onset(result, onset, dense, parameters);
    ta_release(c, dense.v);
    ta_release(c, v.v);
    return result;
}
static I level_candidates(ta_context *ctx, D sym, const double *parameters)
{
    int rank_count = (int)parameters[TA_OPENING_LEVEL_RANK_COUNT];
    int shortlist = (int)parameters[TA_OPENING_LEVEL_SHORTLIST_COUNT];
    int finalists = (int)parameters[TA_OPENING_LEVEL_FINALIST_COUNT];
    double adjacent = parameters[TA_OPENING_LEVEL_ADJACENT_GAP];
    int first = (int)parameters[TA_OPENING_LEVEL_RANK_FIRST_SLOT];
    int second = (int)parameters[TA_OPENING_LEVEL_RANK_SECOND_SLOT];
    int third = (int)parameters[TA_OPENING_LEVEL_RANK_THIRD_SLOT];
    I ranked = rank_d(ctx, (D){ sym.v, rank_count }, true);
    int n = 0;
    for (int i = 0; i < rank_count; i++) {
        int value = ranked.v[i] + 1;
        if (i == rank_count - 1 || value > parameters[TA_OPENING_LEVEL_EARLY_MAX] ||
            sym.v[value - 1] >= parameters[TA_OPENING_LEVEL_EARLY_CONTRAST])
            ranked.v[n++] = value;
    }
    ranked.n = n;
    I candidates = i_copy(ctx, (I){ ranked.v, ta_min(n, shortlist) });
    if (candidates.v[second] - candidates.v[first] == adjacent)
        i_remove(&candidates, second);
    else if (candidates.v[first] - candidates.v[second] == adjacent)
        i_remove(&candidates, first);
    if (candidates.v[third] - candidates.v[second] == adjacent)
        i_remove(&candidates, third);
    else if (candidates.v[second] - candidates.v[first] == adjacent)
        i_remove(&candidates, second);
    candidates.n = ta_min(candidates.n, finalists);
    I ordered = i_sorted(ctx, candidates, false);
    ta_release(ctx, candidates.v);
    ta_release(ctx, ranked.v);
    return ordered;
}
static double level_neighbor_sum(D sym, int candidate, const double *parameters)
{
    int center = candidate + (int)parameters[TA_OPENING_LEVEL_NEIGHBOR_CENTER];
    int previous = candidate - (int)parameters[TA_OPENING_LEVEL_NEIGHBOR_PREVIOUS_BACK];
    int next = candidate + (int)parameters[TA_OPENING_LEVEL_NEIGHBOR_NEXT_OFFSET];
    if (previous < 0 || ta_max(center, next) >= sym.n)
        return NAN;
    return (sym.v[center] + sym.v[previous]) + sym.v[next];
}

static bool level_prefer_first(D sym, int a, int b, const double *parameters)
{
    double zero_level = parameters[TA_OPENING_LEVEL_RATIO_ZERO];
    int back = (int)parameters[TA_OPENING_LEVEL_ORDER_RATIO_BACK];
    return !(a >= parameters[TA_OPENING_LEVEL_ORDER_FIRST_MIN] &&
             b - a == parameters[TA_OPENING_LEVEL_ORDER_GAP] &&
             (nonzero_level(sym.v[b - back], zero_level) / nonzero_level(sym.v[a - back], zero_level) > parameters[TA_OPENING_LEVEL_ORDER_CONTRAST] ||
              level_neighbor_sum(sym, b, parameters) < level_neighbor_sum(sym, a, parameters)));
}
static bool level_prefer_second(D sym, int b, int c, const double *parameters)
{
    double zero_level = parameters[TA_OPENING_LEVEL_RATIO_ZERO];
    int back = (int)parameters[TA_OPENING_LEVEL_ORDER_RATIO_BACK];
    return !(c - b == parameters[TA_OPENING_LEVEL_ORDER_GAP] &&
             (sym.v[c - back] / nonzero_level(sym.v[b - back], zero_level) > parameters[TA_OPENING_LEVEL_ORDER_CONTRAST] ||
              level_neighbor_sum(sym, b, parameters) > level_neighbor_sum(sym, c, parameters)));
}
static void level_order_candidates(I candidates, D sym, const double *parameters)
{
    int first_slot = (int)parameters[TA_OPENING_LEVEL_RANK_FIRST_SLOT];
    int second_slot = (int)parameters[TA_OPENING_LEVEL_RANK_SECOND_SLOT];
    int third_slot = (int)parameters[TA_OPENING_LEVEL_RANK_THIRD_SLOT];
    int a = candidates.v[first_slot], b = candidates.v[second_slot], c = candidates.v[third_slot];
    bool first = level_prefer_first(sym, a, b, parameters);
    bool second = level_prefer_second(sym, b, c, parameters);
    if (!first && !second) {
        candidates.v[first_slot] = c;
        candidates.v[third_slot] = a;
    } else if (!first) {
        candidates.v[first_slot] = b;
        candidates.v[second_slot] = a;
    } else if (!second) {
        candidates.v[second_slot] = c;
        candidates.v[third_slot] = b;
    }
}
static double level_deviation(D values, double mean, const double *parameters)
{
    double dev[ta_max(1, values.n)];
    for (int i = 0; i < values.n; i++) dev[i] = fabs(mean - values.v[i]);
    return d_sum((D){ dev, values.n }, parameters) / (double)values.n;
}
static bool level_deviation_changed(double current, double mean, double deviation, const double *parameters)
{
    return deviation >= parameters[TA_OPENING_DEVIATION_MIN]
               ? current / (mean + deviation) > parameters[TA_OPENING_DEVIATION_RISE] || current / (mean - deviation) < parameters[TA_OPENING_DEVIATION_FALL]
               : fabs(mean - current) / deviation > parameters[TA_OPENING_DEVIATION_OUTLIER];
}
static bool level_second_accepted(D ratios, D sym, const double *parameters)
{
    int second = (int)parameters[TA_OPENING_SECOND_CANDIDATE];
    int previous = second - (int)parameters[TA_OPENING_SETTLED_SUPPORT_BACK];
    if (ratios.v[previous] <= parameters[TA_OPENING_LEVEL_RATIO_NUMERATOR]) return false;
    int next = second + (int)parameters[TA_OPENING_SETTLED_NEXT_OFFSET];
    double mean = (sym.v[second] + sym.v[next]) * parameters[TA_OPENING_PAIR_MEAN_SCALE];
    return mean < parameters[TA_OPENING_SECOND_NEIGHBOR_MAX] &&
           sym.v[previous] > parameters[TA_OPENING_SECOND_CHANGE_MIN];
}
static bool level_changed(ta_context *ctx, D values, D ratios, D sym, int candidate, bool low, const double *parameters)
{
    int probe = (int)parameters[TA_OPENING_HISTORY_RATIO_INDEX];
    int start = (int)parameters[ratios.v[probe] >= parameters[TA_OPENING_LEVEL_HISTORY_START_RATIO]
                                   ? TA_OPENING_HISTORY_EARLY_ORDINAL : TA_OPENING_HISTORY_LATE_ORDINAL] - 1;
    int end = candidate - (int)parameters[TA_OPENING_HISTORY_END_BACK];
    D preceding = d_view(ctx, values, start, end);
    double mean = d_sum(preceding, parameters) / (double)preceding.n;
    double deviation = level_deviation(preceding, mean, parameters);
    if (low) deviation = nonzero_level(deviation, parameters[TA_OPENING_DEVIATION_ZERO]);
    double current = values.v[end];
    bool changed = level_deviation_changed(current, mean, deviation, parameters);
    if (!changed && candidate >= parameters[TA_OPENING_HISTORY_MIN_CANDIDATE] &&
        sym.v[end] > parameters[TA_OPENING_HISTORY_MIN_CONTRAST]) {
        int count = (int)parameters[candidate <= parameters[TA_OPENING_HISTORY_SHORT_END]
                                        ? TA_OPENING_HISTORY_SHORT_COUNT : TA_OPENING_HISTORY_LONG_COUNT];
        D prior = d_view(ctx, values, end - count, end);
        double m = d_sum(prior, parameters) / (double)count;
        double d = level_deviation(prior, m, parameters);
        changed = current / (m + d) > parameters[TA_OPENING_HISTORY_RISE] ||
                  current / (m - d) < parameters[TA_OPENING_HISTORY_FALL];
    }
    return changed;
}
static bool level_settled(D values, D sym, int candidate, const double *parameters)
{
    int next = candidate + (int)parameters[TA_OPENING_SETTLED_NEXT_OFFSET];
    int last = candidate + (int)parameters[TA_OPENING_SETTLED_LAST_OFFSET];
    int previous = candidate - (int)parameters[TA_OPENING_SETTLED_SUPPORT_BACK];
    if (values.v[last] == 0) values.v[last] = parameters[TA_OPENING_SETTLED_ZERO_LEVEL];
    double scale = parameters[TA_OPENING_PAIR_MEAN_SCALE];
    double a = (sym.v[candidate] + sym.v[next]) * scale;
    double b = (sym.v[next] + sym.v[last]) * scale;
    double ratio = values.v[candidate] / values.v[last];
    double neighbor_max = parameters[TA_OPENING_SETTLED_NEIGHBOR_MAX];
    double support = sym.v[previous] * parameters[TA_OPENING_SETTLED_SUPPORT_MAX];
    return a < neighbor_max || b < neighbor_max || a < support || b < support ||
           ta_dmax(ratio, parameters[TA_OPENING_LEVEL_RATIO_NUMERATOR] / ratio) < parameters[TA_OPENING_SETTLED_CONTRAST_MAX];
}

static double level_fallback_mean(D values, int candidate, bool low, const double *parameters)
{
    int start = (int)parameters[low ? TA_OPENING_FALLBACK_LOW_BACK : TA_OPENING_FALLBACK_MID_BACK];
    int end = (int)parameters[TA_OPENING_FALLBACK_END_BACK];
    double scale = parameters[low ? TA_OPENING_FALLBACK_LOW_SCALE : TA_OPENING_FALLBACK_MID_SCALE];
    return sum(values, candidate - start, candidate - end, parameters) * scale;
}

static bool level_candidate_accepted(ta_context *ctx, D values, D ratios, D sym, int candidate, bool low, const double *parameters)
{
    if (candidate == parameters[TA_OPENING_SECOND_CANDIDATE]) return level_second_accepted(ratios, sym, parameters);
    if (candidate < parameters[TA_OPENING_SETTLED_MIN_CANDIDATE]) return false;
    bool changed = level_changed(ctx, values, ratios, sym, candidate, low, parameters);
    if (changed && sym.n - candidate >= parameters[TA_OPENING_SETTLED_REQUIRED_AHEAD])
        return level_settled(values, sym, candidate, parameters);
    if (candidate > parameters[TA_OPENING_FALLBACK_CANDIDATE_FLOOR]) {
        double mean = level_fallback_mean(values, candidate, low, parameters);
        double ratio = values.v[candidate - 1] /
                       (mean == 0 ? parameters[TA_OPENING_FALLBACK_ZERO_LEVEL] : mean);
        return !(ratio <= parameters[TA_OPENING_FALLBACK_RISE_MAX] &&
                 ratio >= parameters[TA_OPENING_FALLBACK_FALL_MIN]);
    }
    return false;
}
static int level_refine_candidate(D values, D ratios, int candidate, const double *parameters)
{
    int next = candidate + (int)parameters[TA_OPENING_REFINE_NEXT_OFFSET];
    int previous = candidate - (int)parameters[TA_OPENING_REFINE_PREVIOUS_BACK];
    if (values.v[next] == 0) values.v[next] = parameters[TA_OPENING_REFINE_ZERO_LEVEL];
    int result = candidate;
    if (candidate < parameters[TA_OPENING_REFINE_FIRST_LIMIT] && ratios.v[previous] <= parameters[TA_OPENING_REFINE_FIRST_MAX]) return result;
    if (ratios.v[previous] <= parameters[TA_OPENING_LEVEL_RATIO_NUMERATOR])
        result += ratios.v[candidate] * ratios.v[previous] > parameters[TA_OPENING_REFINE_PRODUCT_MIN];
    else if (ratios.v[candidate] > parameters[TA_OPENING_REFINE_RISE] ||
             ratios.v[candidate] < parameters[TA_OPENING_REFINE_FALL]) {
        double ratio = values.v[previous] / values.v[next];
        result += ratio > parameters[TA_OPENING_REFINE_STEP_RISE] ||
                  ratio < parameters[TA_OPENING_REFINE_STEP_FALL];
    }
    return result;
}
static int level_candidate(ta_context *ctx, D values, D ratios, D sym, bool low, const double *parameters)
{
    I candidates = level_candidates(ctx, sym, parameters);
    if (!low) level_order_candidates(candidates, sym, parameters);
    int result = 0;
    for (int k = 0; k < candidates.n; k++) {
        int candidate = candidates.v[k];
        if (level_candidate_accepted(ctx, values, ratios, sym, candidate, low, parameters)) {
            result = level_refine_candidate(values, ratios, candidate, parameters);
            break;
        }
    }
    ta_release(ctx, candidates.v);
    return result;
}
static int opening_vote_count(I h, int value)
{
    int count = 0;
    for (int j = 0; j < h.n; j++)
        if (h.v[j] == value) count++;
    return count;
}
static int opening_majority(I h, const double *parameters)
{
    double ignored = parameters[TA_OPENING_VOTE_IGNORED_BAR];
    double required = parameters[TA_OPENING_VOTE_MAJORITY_MIN];
    for (int i = 0; i < h.n; i++)
        if (h.v[i] == ignored) h.v[i] = 0;
    for (int i = 0; i <= parameters[TA_OPENING_VOTE_MAJORITY_LAST_SLOT]; i++)
        if (h.v[i] && opening_vote_count(h, h.v[i]) >= required) return h.v[i];
    return 0;
}
static void opening_pair_scores(I h, D scores, const double *parameters)
{
    double pair_count = parameters[TA_OPENING_VOTE_PAIR_COUNT];
    for (int i = 0; i < h.n; i++)
        if (h.v[i] && opening_vote_count(h, h.v[i]) == pair_count)
            scores.v[i] += parameters[TA_OPENING_VOTE_PAIR_SCORE];
}
static double opening_earliest_score(I unique, const double *parameters)
{
    return unique.n > 1 && unique.v[1] - unique.v[0] >= parameters[TA_OPENING_SCORE_WIDE_GAP]
               ? parameters[TA_OPENING_SCORE_EARLY_WIDE] : parameters[TA_OPENING_SCORE_EARLY_CLOSE];
}
static void opening_order_scores(ta_context *c, I h, D scores, const double *parameters)
{
    I unique = i_compact(c, h, true);
    for (int i = 0; i < h.n; i++)
        if (h.v[i]) {
            int rank = 0;
            while (rank < unique.n && unique.v[rank] != h.v[i]) rank++;
            if (!rank) scores.v[i] += opening_earliest_score(unique, parameters);
            else if (rank == 1) {
                if (unique.v[1] - unique.v[0] < parameters[TA_OPENING_SCORE_WIDE_GAP])
                    scores.v[i] += parameters[TA_OPENING_SCORE_SECOND_CLOSE];
            } else scores.v[i] += parameters[TA_OPENING_SCORE_LATE];
        }
    ta_release(c, unique.v);
}
static void opening_neighbor_scores(ta_context *c, I h, D scores, const double *parameters)
{
    double values[h.n];
    for (int i = 0; i < h.n; i++) values[i] = h.v[i];
    I order = rank_d(c, (D){ values, h.n }, true);
    int first = (int)parameters[TA_OPENING_NEIGHBOR_FIRST_ORDINAL] - 1;
    int low = (int)parameters[TA_OPENING_NEIGHBOR_LOW_ORDINAL] - 1;
    int middle = (int)parameters[TA_OPENING_NEIGHBOR_MIDDLE_ORDINAL] - 1;
    for (int i = 0; i < h.n - 1; i++) {
        int above = order.v[i], below = order.v[i + 1];
        if (h.v[above] - h.v[below] == parameters[TA_OPENING_SCORE_ADJACENT_GAP]) {
            scores.v[above] += parameters[TA_OPENING_SCORE_NEIGHBOR];
            if (below == first)
                scores.v[first] += parameters[TA_OPENING_SCORE_FIRST_NEIGHBOR];
            else if (above == low && below == middle)
                scores.v[low] += parameters[TA_OPENING_SCORE_BAND_NEIGHBOR];
        }
    }
    ta_release(c, order.v);
}
static double opening_bar_score(int v, const double *parameters)
{
    if (v >= parameters[TA_OPENING_SCORE_LATE_BAR_MIN])
        return parameters[TA_OPENING_SCORE_LATE_BAR];
    int slot = v - (int)parameters[TA_OPENING_SCORE_DISPATCH_FIRST_BAR];
    if (slot >= 0 && slot <= parameters[TA_OPENING_SCORE_DISPATCH_LAST_SLOT]) {
        uint32_t bit = UINT32_C(1) << slot;
        for (int i = 0; i < TA_OPENING_SCORE_MASK_COUNT; ++i)
            if ((uint32_t)parameters[TA_OPENING_SCORE_MASK_TWO + i] & bit)
                return parameters[TA_OPENING_SCORE_BAR_TWO + i];
    }
    return parameters[TA_OPENING_SCORE_DEFAULT];
}
static void opening_scores(ta_context *c, I h, D scores, const double *parameters)
{
    opening_pair_scores(h, scores, parameters);
    opening_order_scores(c, h, scores, parameters);
    opening_neighbor_scores(c, h, scores, parameters);
    for (int i = 0; i < h.n; i++) {
        if (!h.v[i]) scores.v[i] = 0;
        else scores.v[i] += opening_bar_score(h.v[i], parameters);
    }
}
static void opening_prune_low(I h, D scores, D low, D lowSym, const double *parameters)
{
    int slot = (int)parameters[TA_OPENING_PRUNE_LOW_SLOT];
    int bar = h.v[slot] - (int)parameters[TA_OPENING_PRUNE_FIRST_BAR];
    if (bar >= 0 && bar < ta_min(low.n, lowSym.n) &&
        !(lowSym.v[bar] >= parameters[TA_OPENING_LOW_PRUNE_CONTRAST]) &&
        !((bar == 0 ? 0 : fabs(low.v[bar] - low.v[bar - 1])) >= parameters[TA_OPENING_LOW_PRUNE_LEVEL])) {
        h.v[slot] = 0;
        scores.v[slot] = 0;
    }
}
static bool opening_low_tie(I raw, I h, D scores, I order, const double *parameters)
{
    int unchanged = 0;
    for (int i = 0; i < h.n; i++)
        if (scores.v[i] == scores.v[order.v[i]]) unchanged++;
    int low = (int)parameters[TA_OPENING_TIE_LOW_SLOT];
    int selected = order.v[(int)parameters[TA_OPENING_SELECTION_FIRST_RANK]];
    int runner_up = order.v[(int)parameters[TA_OPENING_TIE_SECOND_RANK]];
    return raw.v[low] > 0 && unchanged == parameters[TA_OPENING_TIE_UNCHANGED_COUNT] &&
           scores.v[runner_up] == scores.v[low] &&
           abs(h.v[selected] - h.v[low]) <= parameters[TA_OPENING_LOW_TIE_BAR_GAP_MAX] &&
           scores.v[selected] - scores.v[low] <= parameters[TA_OPENING_LOW_TIE_SCORE_GAP] &&
           selected + 1 != parameters[TA_OPENING_TIE_EXCLUDED_ORDINAL];
}
static int opening_select_score(ta_context *c, I raw, I h, D scores, D lowRatios, const double *parameters)
{
    I order = rank_d(c, scores, true);
    int selected = h.v[order.v[(int)parameters[TA_OPENING_SELECTION_FIRST_RANK]]];
    if (opening_low_tie(raw, h, scores, order, parameters)) {
        int low = (int)parameters[TA_OPENING_TIE_LOW_SLOT];
        int bar = raw.v[low] - 1;
        if (bar < lowRatios.n) {
            double value = lowRatios.v[bar];
            if (!(value >= parameters[TA_OPENING_LOW_TIE_LEVEL_MIN] &&
                  value <= parameters[TA_OPENING_LOW_TIE_LEVEL_MAX]))
                selected = h.v[low];
        }
    }
    ta_release(c, order.v);
    return selected;
}
static Opening reconcile(ta_context *c, I raw, D low, D lowRatios, D lowSym, int phase, const double *parameters)
{
    Opening out = { .hypotheses = i_copy(c, raw), .raw = raw, .scores = ta_new_doubles(c, raw.n), .found = true };
    I h = out.hypotheses;
    D scores = out.scores;
    int selected = opening_majority(h, parameters);
    if (!selected) opening_scores(c, h, scores, parameters);
    opening_prune_low(h, scores, low, lowSym, parameters);
    if (!selected) selected = opening_select_score(c, raw, h, scores, lowRatios, parameters);
    int phase_base = phase - (int)parameters[TA_OPENING_RESULT_PHASE_BACK];
    int beats_per_bar = (int)parameters[TA_OPENING_RESULT_BEATS_PER_BAR];
    for (int i = 0; i < h.n; i++)
        h.v[i] = h.v[i] ? phase_base + h.v[i] * beats_per_bar : 0;
    out.selected = selected ? phase_base + selected * beats_per_bar : phase;
    return out;
}
static void opening_dimensions(ta_context *c, D envelope, D low, D middle, D vocal, int phase,
                               const double *parameters)
{
    int bars = (int)parameters[TA_OPENING_RATIO_BARS];
    int beats_per_bar = (int)parameters[TA_OPENING_PAD_BEATS_PER_BAR];
    if (phase < 1 || phase > beats_per_bar || envelope.n < bars * beats_per_bar || low.n < bars || middle.n < bars ||
        vocal.n < parameters[TA_VOICE_VOTE_BUFFER_BEATS])
        ta_fail(c, TA_FAILED, "opening dimensions");
}
static void opening_quiet_low(D lv, D ls, const double *parameters)
{
    double level_max = parameters[TA_OPENING_QUIET_LEVEL_MAX];
    double ratio_max = parameters[TA_OPENING_QUIET_RATIO_MAX];
    int last = (int)parameters[TA_OPENING_QUIET_LAST_BAR];
    for (int i = 1; i <= last; i++)
        if (lv.v[i - 1] < level_max && lv.v[i] < level_max && ls.v[i] < ratio_max)
            ls.v[i] = parameters[TA_OPENING_QUIET_RATIO_REPLACEMENT];
}
Opening opening_judge(ta_context *c, D envelope, D low, D middle, D vocal, int phase,
                      const double *parameters)
{
    opening_dimensions(c, envelope, low, middle, vocal, phase, parameters);
    I raw = ta_new_ints(c, (int)parameters[TA_OPENING_HYPOTHESIS_COUNT]);
    raw.v[(int)parameters[TA_OPENING_ENVELOPE_HYPOTHESIS_SLOT]] = envelope_candidate(c, envelope, phase, parameters);
    raw.v[(int)parameters[TA_OPENING_VOCAL_HYPOTHESIS_SLOT]] = vocal_candidate(c, vocal, parameters);
    D lv = normalized(c, low, false, parameters), lr = ratios(c, lv, parameters), ls = symmetric(c, lr);
    ls.v[0] = 0;
    opening_quiet_low(lv, ls, parameters);
    raw.v[(int)parameters[TA_OPENING_LOW_HYPOTHESIS_SLOT]] = level_candidate(c, lv, lr, ls, true, parameters);
    D mv = normalized(c, middle, true, parameters), mr = ratios(c, mv, parameters), ms = symmetric(c, mr);
    ms.v[0] = 0;
    raw.v[(int)parameters[TA_OPENING_MIDDLE_HYPOTHESIS_SLOT]] = level_candidate(c, mv, mr, ms, false, parameters);
    D original = normalized(c, low, false, parameters), originalRatios = ratios(c, original, parameters),
      originalSym = symmetric(c, originalRatios);
    Opening out = reconcile(c, raw, original, lr, originalSym, phase, parameters);
    D arrays[] = { lv, lr, ls, mv, mr, ms, original, originalRatios, originalSym };
    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++)
        ta_release(c, arrays[i].v);
    return out;
}
static D repeat(ta_context *c, D values, int count, int times)
{
    D out = ta_new_doubles(c, count * times);
    for (int i = 0; i < times; i++)
        memcpy(out.v + i * count, values.v, count * sizeof(double));
    return out;
}
static Opening opening_padded(ta_context *c, D envelope, D low, D middle, D vocal, int phase,
                              int n, Opening out, const double *parameters)
{
    D e = envelope, l = low, m = middle, v = vocal;
    bool needs_padding = n <= parameters[TA_OPENING_PAD_TRIGGER_LAST_BAR];
    if (needs_padding) {
        int bias = (int)parameters[TA_OPENING_PAD_NUMERATOR_BIAS];
        int times = (n + bias) / n +
                    (int)parameters[TA_OPENING_PAD_INITIAL_COUNTER_DISTANCE] + 1;
        int beats = n * (int)parameters[TA_OPENING_PAD_BEATS_PER_BAR];
        e = repeat(c, envelope, beats, times);
        l = repeat(c, low, n, times);
        m = repeat(c, middle, n, times);
        v = repeat(c, vocal, beats, times);
    }
    ta_release(c, out.hypotheses.v);
    ta_release(c, out.raw.v);
    ta_release(c, out.scores.v);
    out = opening_judge(c, e, l, m, v, phase, parameters);
    if (needs_padding) {
        ta_release(c, e.v);
        ta_release(c, l.v);
        ta_release(c, m.v);
        ta_release(c, v.v);
    }
    return out;
}
static Opening opening_short_result(Opening out, int n, int phase, double pad_bars)
{
    int sum = 0;
    for (int i = 0; i < out.hypotheses.n; i++)
        sum += out.hypotheses.v[i];
    if (!sum) {
        if (n >= pad_bars) {
            out.found = true;
            out.hypotheses.n = 1;
            out.hypotheses.v[0] = phase;
            out.selected = phase;
        } else
            out.found = false;
    } else if (n < pad_bars) {
        out.found = true;
        int retained = 0;
        for (int i = 0; i < out.hypotheses.n; i++)
            if (out.hypotheses.v[i] <= n) {
                retained += out.hypotheses.v[i];
                out.selected = out.hypotheses.v[i];
            }
        if (!retained) {
            out.hypotheses.n = 1;
            out.hypotheses.v[0] = phase;
            out.selected = phase;
        }
    }
    return out;
}
static bool opening_anchor_match(Opening *out, I anchors, I ordered)
{
    bool matched = false;
    for (int i = anchors.n - 1; i >= 0; i--)
        for (int j = 0; j < ordered.n; j++)
            if (anchors.v[i] == ordered.v[j]) {
                out->selected = anchors.v[i];
                out->found = true;
                matched = true;
            }
    return matched;
}
static void opening_anchor_phase(Opening *out, I anchors, I ordered, int phase, const double *parameters)
{
    int width = (int)parameters[TA_OPENING_ANCHOR_BEATS_PER_BAR];
    int period = (int)parameters[TA_OPENING_ANCHOR_PHASE_PERIOD];
    int origin = (int)parameters[TA_OPENING_ANCHOR_ORDINAL_BIAS];
    int remainder = ((anchors.v[0] - phase) / width + origin) % period, same = 0;
    int end = ordered.n - (int)parameters[TA_OPENING_ANCHOR_PHASE_TAIL_SKIP];
    for (int i = 0; i < end; i++) {
        int difference = ordered.v[i] - phase;
        int bar = difference / width;
        if (difference > parameters[TA_OPENING_ANCHOR_CANDIDATE_BIAS_AFTER])
            bar += (int)parameters[TA_OPENING_ANCHOR_CANDIDATE_BIAS];
        if (bar % period == remainder) same = same ? ta_min(same, ordered.v[i]) : ordered.v[i];
    }
    if (same) {
        out->selected = same;
        out->found = true;
    }
}
Opening opening_main(ta_context *c, D envelope, D low, D middle, D vocal, int phase, I anchors,
                     const double *parameters)
{
    if (!anchors.n || phase < 1 || phase > parameters[TA_OPENING_PAD_BEATS_PER_BAR])
        ta_fail(c, TA_FAILED, "opening anchors");
    int envelope_bars = envelope.n / (int)parameters[TA_OPENING_ENVELOPE_BAR_DIVISOR];
    int vocal_bars = vocal.n / (int)parameters[TA_OPENING_VOCAL_BAR_DIVISOR];
    int n = ta_min(ta_min(envelope_bars, low.n), ta_min(middle.n, vocal_bars));
    Opening out = { .hypotheses = ta_new_ints(c, 1), .raw = ta_new_ints(c, 0), .scores = ta_new_doubles(c, 0) };
    if (n > parameters[TA_OPENING_SHORT_MAX_SKIPPED_BARS]) {
        out = opening_padded(c, envelope, low, middle, vocal, phase, n, out, parameters);
        out = opening_short_result(out, n, phase, parameters[TA_OPENING_PAD_BARS]);
    }
    I ordered = i_sorted(c, out.hypotheses, true);
    ta_release(c, out.hypotheses.v);
    out.hypotheses = ordered;
    if (anchors.v[0] && !opening_anchor_match(&out, anchors, ordered))
        opening_anchor_phase(&out, anchors, ordered, phase, parameters);
    return out;
}

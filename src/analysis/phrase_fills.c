/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
HighProfile normalize_high(ta_context *c, D input, int phase, const double *parameters)
{
    int shift = phase * (int)parameters[TA_HIGH_BAND_PHASE_SAMPLES] -
                (int)parameters[TA_HIGH_BAND_PHASE_ORIGIN_SAMPLES];
    if (shift < 0 || shift >= input.n)
        return (HighProfile){ 0 };
    int count = input.n - shift;
    D out = d_copy(c, input);
    double max = d_max(d_view(c, input, shift, input.n));
    double factor = max == 0 ? parameters[TA_HIGH_BAND_ZERO_SCALE]
                             : parameters[TA_HIGH_BAND_INVERSE_NUMERATOR] / max;
    int writes = count - (int)parameters[TA_HIGH_BAND_NORMALIZE_TAIL];
    for (int i = 0; i < writes; i++)
        out.v[i] = input.v[i + shift] * factor;
    return (HighProfile){ out, count / (int)parameters[TA_HIGH_BAND_BEAT_DIVISOR] };
}
static void snare_levels(ta_context *c, D high, int bars, D levels, const double *parameters)
{
    int tail = (int)parameters[TA_SNARE_SPECTRAL_TAIL_BARS];
    int width = (int)parameters[TA_SNARE_SPECTRAL_BAR_SAMPLES];
    for (int bar = 0; bar < bars - tail; bar++) {
        Peaks p = spectral_peaks(c, d_view(c, high, bar * width, (bar + 1) * width), parameters);
        D heights = ta_new_doubles(c, p.n);
        for (int i = 0; i < p.n; i++)
            heights.v[i] = p.v[i].height;
        levels.v[bar] = d_sum(heights, parameters);
        ta_release(c, heights.v);
        ta_release(c, p.v);
    }
    double max = d_max(levels);
    double scale = max == 0 ? parameters[TA_SNARE_LEVEL_ZERO_SCALE]
                            : parameters[TA_SNARE_LEVEL_INVERSE_NUMERATOR] / max;
    for (int i = 0; i < bars; i++) {
        double v = levels.v[i] * scale;
        levels.v[i] = v == 0 ? parameters[TA_SNARE_LEVEL_ZERO_VALUE] : v;
    }
}

typedef struct {
    double upper, lower;
} snare_thresholds;
static snare_thresholds snare_ratios(D levels, D ratios, D rising, D falling, int bars,
                                     const double *parameters)
{
    int rc = 0, fc = 0;
    for (int i = 1; i < bars; i++) {
        ratios.v[i] = levels.v[i] / levels.v[i - 1];
        if (ratios.v[i] >= parameters[TA_SNARE_RATIO_NO_CHANGE]) {
            rising.v[i] = ratios.v[i];
            rc++;
        } else if (ratios.v[i] != 0) {
            falling.v[i] = ratios.v[i];
            fc++;
        }
    }
    double upper = rc == 0 ? parameters[TA_SNARE_RISE_EMPTY_LIMIT]
                           : d_sum(rising, parameters) * parameters[TA_SNARE_RISE_MEAN_SCALE] / rc;
    double lower = fc == 0 ? parameters[TA_SNARE_FALL_EMPTY_LIMIT]
                           : d_sum(falling, parameters) * parameters[TA_SNARE_FALL_MEAN_SCALE] / fc;
    return (snare_thresholds){ upper, lower };
}

static void snare_percentiles(ta_context *c, D rising, int bars, double *upper, double *lower,
                              const double *parameters)
{
    if (bars >= parameters[TA_SNARE_PERCENTILE_MIN_BARS]) {
        int percentile = bars / (int)parameters[TA_SNARE_PERCENTILE_BAR_DIVISOR] -
                         (int)parameters[TA_SNARE_PERCENTILE_RANK_ORIGIN];
        I order = rank_d(c, rising, true);
        *upper = ta_dmax(*upper, rising.v[order.v[percentile]]);
        D nonzero = ta_new_doubles(c, bars);
        nonzero.n = 0;
        for (int i = 0; i < bars; i++)
            if (rising.v[i] != 0)
                nonzero.v[nonzero.n++] = rising.v[i];
        order = rank_d(c, nonzero, false);
        if (percentile < order.n)
            *lower = ta_dmin(*lower, nonzero.v[order.v[percentile]]);
    }
}

static int falling_snare(double current, double next, double lower, double level, int i,
                         const double *parameters)
{
    double minimum = parameters[current <= parameters[TA_SNARE_LARGE_FALL_RATIO]
                                   ? TA_SNARE_LARGE_CHANGE_LEVEL : TA_SNARE_SMALL_FALL_LEVEL];
    if (current < parameters[TA_SNARE_SELECT_NO_CHANGE] && next > lower && level > minimum) {
        int advance = (int)parameters[next * current < parameters[TA_SNARE_SELECT_PRODUCT]
                                          ? TA_SNARE_FIRST_POSITION_ADVANCE : TA_SNARE_SCAN_STABLE_AHEAD];
        return i + advance;
    }
    return 0;
}

static int rising_snare(double current, double next, double upper, double level, int i,
                        const double *parameters)
{
    double minimum = parameters[current >= parameters[TA_SNARE_LARGE_RISE_RATIO]
                                   ? TA_SNARE_LARGE_CHANGE_LEVEL : TA_SNARE_SMALL_RISE_LEVEL];
    if (current > parameters[TA_SNARE_SELECT_NO_CHANGE] && next < upper && level > minimum) {
        int advance = (int)parameters[next * current > parameters[TA_SNARE_SELECT_PRODUCT]
                                          ? TA_SNARE_FIRST_POSITION_ADVANCE : TA_SNARE_SCAN_STABLE_AHEAD];
        return i + advance;
    }
    return 0;
}

static void select_snares(I output, D levels, D ratios, int bars, double upper, double lower,
                          const double *parameters)
{
    bool skip = false;
    int count = 0;
    int last = bars - (int)parameters[TA_SNARE_SPECTRAL_TAIL_BARS];
    int lookahead = (int)parameters[TA_SNARE_SCAN_STABLE_AHEAD];
    for (int i = (int)parameters[TA_SNARE_SCAN_START_BAR]; i < last; i++) {
        if (skip) {
            skip = false;
            continue;
        }
        double current = ratios.v[i], next = ratios.v[i + 1];
        double stable = ratios.v[i + lookahead];
        if (!((current > upper || current < lower) && stable > parameters[TA_SNARE_NEIGHBOR_RATIO_MIN] &&
              stable < parameters[TA_SNARE_NEIGHBOR_RATIO_MAX]))
            continue;
        int falling = falling_snare(current, next, lower, levels.v[i - 1], i, parameters);
        if (falling) {
            output.v[count++] = falling;
            skip = true;
        }
        int rising = rising_snare(current, next, upper, levels.v[i], i, parameters);
        if (rising) {
            output.v[count++] = rising;
            skip = true;
        }
    }
}

I snare_candidates(ta_context *c, D high, int bars, const double *parameters)
{
    int tail = (int)parameters[TA_SNARE_SPECTRAL_TAIL_BARS];
    int width = (int)parameters[TA_SNARE_SPECTRAL_BAR_SAMPLES];
    if (bars < 0 || high.n < (int64_t)ta_max(0, bars - tail) * width)
        ta_fail(c, TA_FAILED, "snare dimensions");
    I output = ta_new_ints(c, bars);
    if (bars < parameters[TA_SNARE_MIN_BARS])
        return output;
    D levels = ta_new_doubles(c, bars), ratios = ta_new_doubles(c, bars),
      rising = ta_new_doubles(c, bars), falling = ta_new_doubles(c, bars);
    snare_levels(c, high, bars, levels, parameters);
    snare_thresholds limits = snare_ratios(levels, ratios, rising, falling, bars, parameters);
    double upper = limits.upper, lower = limits.lower;
    snare_percentiles(c, rising, bars, &upper, &lower, parameters);
    select_snares(output, levels, ratios, bars, upper, lower, parameters);
    return output;
}
enum GrowthWindow { GROWTH_FIRST, GROWTH_SECOND, GROWTH_THIRD, GROWTH_FOURTH, GROWTH_WINDOW_SLOTS };

static FillDetection growth_candidate(const double m[GROWTH_WINDOW_SLOTS], int end, const double *parameters)
{
    int advance;
    double drop = parameters[TA_FILL_GROWTH_DROP_SCALE], rise = parameters[TA_FILL_GROWTH_RISE_SCALE];
    if (m[GROWTH_FIRST] * drop <= m[GROWTH_SECOND]) {
        if (!(m[GROWTH_THIRD] < m[GROWTH_SECOND] * drop && m[GROWTH_FOURTH] > m[GROWTH_THIRD] * rise))
            return (FillDetection){ 0 };
        advance = (int)parameters[TA_FILL_GROWTH_LAST_CANDIDATE_ADVANCE];
    } else if (m[GROWTH_THIRD] > m[GROWTH_SECOND] * rise)
        advance = (int)parameters[TA_FILL_GROWTH_MIDDLE_CANDIDATE_ADVANCE];
    else {
        if (!(m[GROWTH_FOURTH] > m[GROWTH_THIRD] * rise))
            return (FillDetection){ 0 };
        advance = (int)parameters[TA_FILL_GROWTH_LAST_CANDIDATE_ADVANCE];
    }
    int candidate = end - (int)parameters[TA_FILL_GROWTH_WINDOW_BASE_BACK] + advance;
    return (FillDetection){ candidate, end - (int)parameters[TA_FILL_GROWTH_SECTION_END_BACK], FILL_GROWTH };
}

FillDetection fill_growth(ta_context *c, int start, int end, D low, const double *parameters)
{
    const int sample_bytes = sizeof(double);
    const int offsets[GROWTH_WINDOW_SLOTS] = {
        [GROWTH_FIRST] = -(int)parameters[TA_FILL_GROWTH_WINDOW_FIRST_BACK_BYTES] / sample_bytes,
        [GROWTH_SECOND] = (int)parameters[TA_FILL_GROWTH_WINDOW_SECOND_BYTES] / sample_bytes,
        [GROWTH_THIRD] = (int)parameters[TA_FILL_GROWTH_WINDOW_THIRD_BYTES] / sample_bytes,
        [GROWTH_FOURTH] = (int)parameters[TA_FILL_GROWTH_WINDOW_FOURTH_BYTES] / sample_bytes,
    };
    int count = (int)parameters[TA_FILL_GROWTH_WINDOW_COUNT];
    int width = (int)parameters[TA_FILL_GROWTH_WINDOW_BEATS];
    int base = end - (int)parameters[TA_FILL_GROWTH_WINDOW_BASE_BACK];
    if (end - start < parameters[TA_FILL_GROWTH_MIN_LENGTH] ||
        base + offsets[GROWTH_FIRST] < 0 || (int64_t)base + offsets[GROWTH_FOURTH] + width > low.n)
        return (FillDetection){ 0 };
    double m[GROWTH_WINDOW_SLOTS];
    for (int i = 0; i < count; i++) {
        int first = base + offsets[i];
        m[i] = d_sequential(d_view(c, low, first, first + width)) / width;
    }
    return growth_candidate(m, end, parameters);
}
static bool stable_cut_lead(D fine, int anchor, const double *parameters)
{
    int first = anchor - (int)parameters[TA_FILL_CUT_STEADY_FIRST_BEFORE_ANCHOR];
    int stop = anchor - (int)parameters[TA_FILL_CUT_STEADY_STOP_BEFORE_ANCHOR];
    for (int i = first; i < stop; i++) {
        if (fine.v[i] == 0)
            fine.v[i] = parameters[TA_FILL_CUT_ZERO_LEVEL];
        double ratio = fine.v[i + 1] / fine.v[i];
        if (!(ratio >= parameters[TA_FILL_CUT_STEADY_MIN] &&
              ratio <= parameters[TA_FILL_CUT_STEADY_MAX]))
            return false;
    }
    return true;
}

static bool cut_level_change(ta_context *c, D fine, int i, int history, const double *parameters)
{
    double previous = fine.v[i - 1], current = fine.v[i], next = fine.v[i + 1],
           sum = d_sequential(d_view(c, fine, i - history, i));
    bool decrease = current < previous * parameters[TA_FILL_CUT_DROP_PREVIOUS] &&
                    current < sum * parameters[TA_FILL_CUT_DROP_TOTAL] &&
                    next * current < current * parameters[TA_FILL_CUT_DROP_SUSTAIN] * previous;
    bool increase = current > previous + previous && current > sum * parameters[TA_FILL_CUT_RISE_TOTAL] &&
                    next * current > current * (previous + previous);
    return decrease || increase;
}

typedef struct {
    int first, stride, width, count;
} CutWindows;

static double cut_window_level(ta_context *c, D low, int start, int width, double scale)
{
    return d_sequential(d_view(c, low, start, start + width)) * scale;
}

static int coarse_cut_start(ta_context *c, D low, int end, CutWindows windows, const double *parameters)
{
    double scale = parameters[TA_FILL_CUT_WINDOW_SCALE];
    double drop = parameters[TA_FILL_CUT_COARSE_DROP];
    double previous = cut_window_level(c, low, windows.first, windows.width, scale);
    for (int i = 1; i < windows.count; i++) {
        double next = cut_window_level(c, low, windows.first + i * windows.stride, windows.width, scale);
        if (next < previous * drop) {
            int base = end - (int)parameters[TA_FILL_CUT_COARSE_BASE_BACK];
            return base + (int)parameters[TA_FILL_CUT_COARSE_OFFSET_0 + i - 1];
        }
        previous = next;
    }
    return 0;
}

static int fine_cut_start(ta_context *c, D fine, int end, const double *parameters)
{
    int anchor = end - (int)parameters[TA_FILL_CUT_FINE_ANCHOR_BACK];
    int first = anchor - (int)parameters[TA_FILL_CUT_FINE_FIRST_BEFORE_ANCHOR];
    int last = anchor + (int)parameters[TA_FILL_CUT_FINE_LAST_AFTER_ANCHOR];
    if ((int64_t)last + 1 >= fine.n)
        return 0;
    int history = first - (end - (int)parameters[TA_FILL_CUT_FINE_HISTORY_BACK]);
    fine = d_copy(c, fine);
    int start = 0;
    if (stable_cut_lead(fine, anchor, parameters)) {
        for (int i = first; i <= last; i++) {
            if (cut_level_change(c, fine, i, history, parameters)) {
                start = i + (int)parameters[TA_FILL_CUT_FINE_MARKER_ADVANCE];
                break;
            }
        }
    }
    ta_release(c, fine.v);
    return start;
}

FillDetection fill_cut(ta_context *c, int start, int end, D low, D fine, const double *parameters)
{
    int first_back = (int)parameters[TA_FILL_CUT_WINDOW_FIRST_BACK];
    CutWindows windows = {
        .first = end - first_back,
        .width = first_back - (int)parameters[TA_FILL_CUT_WINDOW_FIRST_STOP_BACK] + 1,
        .stride = first_back - (int)parameters[TA_FILL_CUT_WINDOW_SECOND_BACK],
        .count = (int)parameters[TA_FILL_CUT_WINDOW_COUNT],
    };
    int64_t stop = (int64_t)windows.first + (windows.count - 1) * windows.stride + windows.width;
    if (end - start < parameters[TA_FILL_CUT_MIN_LENGTH] || windows.first < 0 || stop > low.n)
        return (FillDetection){ 0 };
    int candidate = coarse_cut_start(c, low, end, windows, parameters);
    if (!candidate)
        candidate = fine_cut_start(c, fine, end, parameters);
    if (!candidate)
        return (FillDetection){ 0 };
    return (FillDetection){ candidate, end - (int)parameters[TA_FILL_CUT_SECTION_END_BACK], FILL_CUT };
}
static int collect_local_peaks(D v, int *peaks)
{
    int n = 0;
    for (int i = 1; i < v.n - 1; i++)
        if (v.v[i] > v.v[i - 1] && v.v[i] >= v.v[i + 1])
            peaks[n++] = i;
    return n;
}

static int repeated_peak_run(D v, const int *peaks, int n, double *amplitudes, int *spacings,
                             const double *parameters)
{
    int run = 0;
    double min_spacing = parameters[TA_REPEATED_PEAK_SPACING_MIN];
    for (int i = 0; i < n - 1; i++) {
        int a = peaks[i], b = peaks[i + 1], spacing = b - a;
        double ratio = v.v[b] / v.v[a];
        if (spacing >= min_spacing && spacing - min_spacing <= parameters[TA_REPEATED_PEAK_SPACING_RANGE] &&
            ratio > parameters[TA_REPEATED_PEAK_RATIO_MIN] && ratio < parameters[TA_REPEATED_PEAK_RATIO_MAX]) {
            amplitudes[run] = v.v[a];
            spacings[run++] = spacing;
        } else
            run = 0;
    }
    return run;
}

int repeated_peak_score(D v, const double *parameters)
{
    int window = (int)parameters[TA_REPEATED_PEAK_WINDOW_SAMPLES];
    if (v.n > window)
        return 0;
    int peaks[window], n = collect_local_peaks(v, peaks);
    if (n < parameters[TA_REPEATED_PEAK_MIN_COUNT])
        return 0;
    int capacity = (int)parameters[TA_REPEATED_PEAK_AMPLITUDE_SLOTS];
    double amplitudes[capacity];
    int spacings[(int)parameters[TA_REPEATED_PEAK_SPACING_SLOTS]];
    memset(amplitudes, 0, sizeof amplitudes);
    int run = repeated_peak_run(v, peaks, n, amplitudes, spacings, parameters);
    if (run < parameters[TA_REPEATED_PEAK_MIN_RUN])
        return 0;
    double max = ta_dmax(d_max((D){ amplitudes, capacity }), v.v[peaks[n - 1]]);
    if (max < parameters[TA_REPEATED_PEAK_MIN_STRENGTH])
        return 0;
    if (max < parameters[TA_REPEATED_PEAK_MIN_UNEVEN])
        for (int i = 1; i < run; i++)
            if (spacings[i] != spacings[i - 1])
                return 0;
    return run;
}
I repeated_scores(ta_context *c, HighProfile high, const double *parameters)
{
    int count = high.beats / (int)parameters[TA_REPEATED_PEAK_GROUP_BEATS];
    I scores = ta_new_ints(c, count);
    if (high.beats < parameters[TA_REPEATED_PEAK_TRACK_MIN])
        return scores;
    int window = (int)parameters[TA_REPEATED_PEAK_WINDOW_SAMPLES];
    int last = count - (int)parameters[TA_REPEATED_PEAK_TAIL_GROUPS];
    int stride = (int)parameters[TA_REPEATED_PEAK_GROUP_STRIDE];
    int offset = (int)parameters[TA_REPEATED_PEAK_WINDOW_OFFSET];
    for (int g = 0; g < last; g++) {
        int start = g * stride + offset;
        if (start + window > high.values.n)
            break;
        scores.v[g] = repeated_peak_score(d_view(c, high.values, start, start + window), parameters);
    }
    return scores;
}

static bool repeated_upper_evidence(I scores, int group, const double *parameters)
{
    int active = group - (int)parameters[TA_REPEATED_UPPER_ACTIVE_LOOKBACK];
    int earlier = group - (int)parameters[TA_REPEATED_UPPER_QUIET_LOOKBACK];
    if (earlier < 0 || active >= scores.n)
        return false;
    return scores.v[active] >= parameters[TA_REPEATED_UPPER_MIN_ACTIVE] &&
           scores.v[earlier] <= parameters[TA_REPEATED_UPPER_MAX_QUIET_SCORE];
}

FillDetection repeated_upper(int end, int origin, I scores, const double *parameters)
{
    int delta = end - origin, group = delta / (int)parameters[TA_REPEATED_PEAK_GROUP_BEATS];
    if (scores.n < parameters[TA_REPEATED_UPPER_MIN_SCORES] ||
        delta < parameters[TA_REPEATED_UPPER_MIN_SPAN] || !repeated_upper_evidence(scores, group, parameters))
        return (FillDetection){ 0 };
    return (FillDetection){ end - (int)parameters[TA_REPEATED_UPPER_START_BACK], 0, FILL_UPPER };
}
static void save(Fills *r, FillDetection d, int i, int role)
{
    if (d.start) {
        r->roles.v[i] = role;
        r->starts.v[i] = d.start;
        r->routes.v[i] = d.route;
    }
}
static void attach_intro_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p,
                               int stage, I scores, const double *parameters)
{
    int n = roles.n;
    int cut_end = (int)parameters[TA_FILL_INTRO_CUT_END_INDEX];
    for (int i = 0; i < n - 1; i++)
        if (stage != FILL_UP_HOOK && roles.v[i] == parameters[TA_FILL_INTRO_BASE_ROLE]) {
            FillDetection d = fill_cut(c, boundaries.v[0], boundaries.v[cut_end], p.low, p.quarterMean,
                                       parameters);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores, parameters);
            save(r, d, 0, (int)parameters[TA_FILL_INTRO_DECORATED_ROLE]);
        }
}

static void attach_build_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p,
                               I scores, const double *parameters)
{
    int n = roles.n;
    int first_section = (int)parameters[TA_FILL_BUILD_FIRST_ROLE_BYTE_OFFSET] / (int)sizeof(int32_t);
    int end_role = (int)parameters[TA_FILL_BUILD_ROLE_EXCLUSIVE];
    int first_role = end_role - (int)parameters[TA_FILL_BUILD_ROLE_COUNT];
    for (int i = first_section; i < n - 1; i++)
        if (roles.v[i] >= first_role && roles.v[i] < end_role) {
            FillDetection d = fill_growth(c, boundaries.v[i], boundaries.v[i + 1], p.low,
                                          parameters);
            if (!d.start)
                d = fill_cut(c, boundaries.v[i], boundaries.v[i + 1], p.low, p.quarterMean,
                             parameters);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores, parameters);
            save(r, d, i, roles.v[i] + (int)parameters[TA_FILL_BUILD_ROLE_ADVANCE]);
        }
}

static void attach_hook_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p,
                              I scores, const double *parameters)
{
    int n = roles.n;
    for (int i = (int)parameters[TA_FILL_HOOK_FIRST_SECTION]; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_FILL_HOOK_BASE_ROLE]) {
            FillDetection d = fill_cut(c, boundaries.v[i], boundaries.v[i + 1], p.low,
                                       p.quarterMean, parameters);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores, parameters);
            save(r, d, i, (int)parameters[TA_FILL_HOOK_DECORATED_ROLE]);
        }
}

Fills attach_fills(ta_context *c, I boundaries, I roles, FillProfile p, int stage,
                   const double *parameters)
{
    int n = roles.n;
    if (boundaries.n != n + 1)
        ta_fail(c, TA_FAILED, "fill dimensions");
    Fills r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n) };
    if (n <= parameters[TA_FILL_INTRO_MIN_EXCLUSIVE])
        return r;
    HighProfile high = normalize_high(c, p.high, p.phase, parameters);
    I scores = repeated_scores(c, high, parameters);
    attach_intro_fills(c, &r, boundaries, roles, p, stage, scores, parameters);
    if (stage != FILL_INTRO) {
        if (n >= parameters[TA_FILL_BUILD_MIN_SECTIONS])
            attach_build_fills(c, &r, boundaries, roles, p, scores, parameters);
        if (n >= parameters[TA_FILL_HOOK_MIN_SECTIONS])
            attach_hook_fills(c, &r, boundaries, roles, p, scores, parameters);
    }
    ta_release(c, high.values.v);
    ta_release(c, scores.v);
    return r;
}
static bool quiet_fill_end(D low, D power, int end, const double *parameters)
{
    int first = end - (int)parameters[TA_CLEAN_FILL_QUIET_FIRST_BACK];
    int last = end - (int)parameters[TA_CLEAN_FILL_QUIET_LAST_BACK];
    int quiet = 0;
    for (int j = first; j <= last; j++)
        quiet += low.v[j] < parameters[TA_CLEAN_FILL_QUIET_LEVEL];
    int first_power = end - (int)parameters[TA_CLEAN_FILL_QUIET_POWER_FIRST_BACK];
    int second_power = end - (int)parameters[TA_CLEAN_FILL_QUIET_POWER_SECOND_BACK];
    double limit = parameters[TA_CLEAN_FILL_QUIET_POWER];
    return quiet >= parameters[TA_CLEAN_FILL_QUIET_MIN] &&
           power.v[first_power] < limit && power.v[second_power] < limit &&
           (power.v[last] < limit || power.v[first] < limit);
}

static int preceding_fill_activity(D low, int start, const double *parameters)
{
    int first = start - (int)parameters[TA_CLEAN_FILL_PREVIOUS_FIRST_BACK];
    int last = start - (int)parameters[TA_CLEAN_FILL_PREVIOUS_LAST_BACK];
    double active_level = parameters[TA_CLEAN_FILL_PREVIOUS_ACTIVE_LEVEL];
    int active = 0;
    if (first >= 0 && last < low.n)
        for (int j = first; j <= last; j++)
            active += low.v[j] > active_level;
    return active;
}

static void remove_fill_decoration(Fills *r, int i, const double *parameters)
{
    r->starts.v[i] = 0;
    int role = r->roles.v[i];
    if (role > parameters[TA_CLEAN_FILL_HOOK_DECORATION_MIN_EXCLUSIVE])
        r->roles.v[i] -= (int)parameters[TA_CLEAN_FILL_HOOK_DECORATION_OFFSET];
    else if (role > parameters[TA_CLEAN_FILL_BUILD_DECORATION_MIN_EXCLUSIVE])
        r->roles.v[i] -= (int)parameters[TA_CLEAN_FILL_BUILD_DECORATION_OFFSET];
    else if (role >= parameters[TA_CLEAN_FILL_INTRO_DECORATION_MIN])
        r->roles.v[i] -= (int)parameters[TA_CLEAN_FILL_INTRO_DECORATION_OFFSET];
}

static void clean_fill_activity(I boundaries, Fills *r, D low, D power, int i,
                                const double *parameters)
{
    int end = boundaries.v[i + 1], start = r->starts.v[i];
    if (!start || end - start < parameters[TA_CLEAN_FILL_MIN_LENGTH] ||
        end < parameters[TA_CLEAN_FILL_QUIET_FIRST_BACK] ||
        end - parameters[TA_CLEAN_FILL_QUIET_LAST_BACK] >= low.n)
        return;
    if (!quiet_fill_end(low, power, end, parameters)) return;
    int active = preceding_fill_activity(low, start, parameters);
    if (active >= parameters[TA_CLEAN_FILL_PREVIOUS_ACTIVE_MIN])
        r->auxiliary.v[i] = (int)parameters[TA_CLEAN_FILL_AUXILIARY_MARK];
    else {
        remove_fill_decoration(r, i, parameters);
    }
}

void clean_activity(ta_context *c, I boundaries, FillProfile p, Fills *r, const double *parameters)
{
    int width = (int)parameters[TA_CLEAN_FILL_POWER_STRIDE_BYTES] / (int)sizeof(double);
    if (r->roles.n <= 1 || p.power.n < (int64_t)p.low.n * width)
        return;
    D power = ta_new_doubles(c, p.low.n);
    for (int i = 0; i < p.low.n; i++)
        power.v[i] = d_sequential(d_view(c, p.power, i * width, (i + 1) * width));
    double lm = d_max(p.low), pm = d_max(power);
    double zero_inverse = parameters[TA_CLEAN_FILL_ZERO_MAX_INVERSE];
    double numerator = parameters[TA_CLEAN_FILL_INVERSE_NUMERATOR];
    double ls = lm == 0 ? zero_inverse : numerator / lm, ps = pm == 0 ? zero_inverse : numerator / pm;
    D low = d_copy(c, p.low);
    for (int i = 0; i < low.n; i++) {
        low.v[i] *= ls;
        power.v[i] *= ps;
    }
    for (int i = 0; i < r->roles.n - 1; i++) {
        clean_fill_activity(boundaries, r, low, power, i, parameters);
    }
    ta_release(c, power.v);
    ta_release(c, low.v);
}
static bool is_hook(int role, const double *parameters)
{
    return role == parameters[TA_FINAL_ROLE_HOOK] || role == parameters[TA_FINAL_ROLE_HOOK_VARIANT] ||
           role == parameters[TA_FINAL_ROLE_HOOK_DECORATED] ||
           role == parameters[TA_FINAL_ROLE_HOOK_DECORATED_VARIANT];
}

static bool is_up(int role, const double *parameters)
{
    return (role >= parameters[TA_FINAL_ROLE_BUILD] && role <= parameters[TA_FINAL_ROLE_BUILD_SIMPLE]) ||
           (role >= parameters[TA_FINAL_ROLE_BUILD_DECORATED] &&
            role <= parameters[TA_FINAL_ROLE_BUILD_DECORATED_SIMPLE]);
}

static int undecorated(int role, const double *parameters)
{
    static const struct { int decorated, plain; } roles[] = {
        { TA_FINAL_ROLE_INTRO_DECORATED, TA_FINAL_ROLE_INTRO },
        { TA_FINAL_ROLE_INTRO_DECORATED_VARIANT, TA_FINAL_ROLE_INTRO_VARIANT },
        { TA_FINAL_ROLE_BUILD_DECORATED, TA_FINAL_ROLE_BUILD },
        { TA_FINAL_ROLE_BUILD_DECORATED_VARIANT, TA_FINAL_ROLE_BUILD_VARIANT },
        { TA_FINAL_ROLE_BUILD_DECORATED_STAGED, TA_FINAL_ROLE_BUILD_STAGED },
        { TA_FINAL_ROLE_BUILD_DECORATED_SIMPLE, TA_FINAL_ROLE_BUILD_SIMPLE },
        { TA_FINAL_ROLE_HOOK_DECORATED, TA_FINAL_ROLE_HOOK },
        { TA_FINAL_ROLE_HOOK_DECORATED_VARIANT, TA_FINAL_ROLE_HOOK_VARIANT },
    };
    for (size_t i = 0; i < sizeof roles / sizeof *roles; i++)
        if (role == parameters[roles[i].decorated])
            return (int)parameters[roles[i].plain];
    return role;
}

static int ending_hook_role(int current, const double *parameters)
{
    return (int)parameters[current == parameters[TA_FINAL_ROLE_HOOK] || current == parameters[TA_FINAL_ROLE_HOOK_VARIANT]
                               ? TA_FINAL_ROLE_OUTRO : TA_FINAL_ROLE_OUTRO_VARIANT];
}

static int intro_hook_role(int current, const double *parameters)
{
    if (current == parameters[TA_FINAL_ROLE_HOOK])
        return (int)parameters[TA_FINAL_ROLE_INTRO];
    if (current == parameters[TA_FINAL_ROLE_HOOK_VARIANT])
        return (int)parameters[TA_FINAL_ROLE_INTRO_VARIANT];
    return (int)parameters[current == parameters[TA_FINAL_ROLE_HOOK_DECORATED]
                               ? TA_FINAL_ROLE_INTRO_DECORATED : TA_FINAL_ROLE_INTRO_DECORATED_VARIANT];
}

static void remap_short_hook(I roles, int i, int n, const double *parameters)
{
    int current = roles.v[i], previous = roles.v[i - 1];
    if (is_hook(previous, parameters))
        return;
    if (i == n - 1) {
        roles.v[i] = ending_hook_role(current, parameters);
    } else if (!is_hook(roles.v[i + 1], parameters)) {
        if (i == 1)
            roles.v[i] = intro_hook_role(current, parameters);
        if (previous == parameters[TA_FINAL_ROLE_BREAK])
            roles.v[i] = previous;
        if (is_up(previous, parameters))
            roles.v[i] = (int)parameters[current == parameters[TA_FINAL_ROLE_HOOK]
                                             ? TA_FINAL_ROLE_BUILD : TA_FINAL_ROLE_BUILD_DECORATED];
    }
}

static void adjust_short_hooks(I b, I roles, int n, const double *parameters)
{
    double max_span = parameters[TA_FINAL_SHORT_HOOK_SPAN_MAX];
    for (int i = 1; i < n; i++)
        if (b.v[i + 1] - b.v[i] <= max_span && is_hook(roles.v[i], parameters))
            remap_short_hook(roles, i, n, parameters);
}

static bool neighboring_hook_variant(int preceding, int following, const double *parameters)
{
    int plain = (int)parameters[TA_FINAL_ROLE_HOOK_VARIANT];
    int decorated = (int)parameters[TA_FINAL_ROLE_HOOK_DECORATED_VARIANT];
    return preceding == plain || preceding == decorated || following == plain || following == decorated;
}

static void extend_hook_variants(I b, I roles, int n, const double *parameters)
{
    int plain = (int)parameters[TA_FINAL_ROLE_HOOK];
    int decorated = (int)parameters[TA_FINAL_ROLE_HOOK_DECORATED];
    for (int i = 1; i < n; i++) {
        int role = roles.v[i];
        if (b.v[i + 1] - b.v[i] <= parameters[TA_FINAL_HOOK_VARIANT_SPAN_MAX] &&
            (role == plain || role == decorated)) {
            int preceding = roles.v[i - 1], following = i + 1 < n ? roles.v[i + 1] : 0;
            if (neighboring_hook_variant(preceding, following, parameters))
                roles.v[i] = (int)parameters[role == plain ? TA_FINAL_ROLE_HOOK_VARIANT
                                                           : TA_FINAL_ROLE_HOOK_DECORATED_VARIANT];
        }
    }
}

static bool clear_fill_at_boundary(I b, I roles, int i, int n, const double *parameters)
{
    int current_length = b.v[i + 1] - b.v[i], following_length = b.v[i + 2] - b.v[i + 1];
    int following = roles.v[i + 1];
    int ahead = i + (int)parameters[TA_FINAL_FOLLOWING_HOOK_AHEAD];
    double short_length = parameters[TA_FINAL_CLEAR_FILL_SHORT_LENGTH];
    bool clear = current_length < short_length;
    if (following_length < short_length) {
        clear = true;
        if (is_hook(following, parameters) && current_length >= short_length && ahead < n)
            clear = !is_hook(roles.v[ahead], parameters);
    }
    return clear || following == parameters[TA_FINAL_ROLE_BREAK] || following == parameters[TA_FINAL_ROLE_OUTRO];
}

static void clear_short_fills(I b, Fills *r, int n, const double *parameters)
{
    I roles = r->roles;
    for (int i = 0; i < n - 1; i++) {
        if (clear_fill_at_boundary(b, roles, i, n, parameters)) {
            r->starts.v[i] = 0;
            roles.v[i] = undecorated(roles.v[i], parameters);
        }
    }
}

static void extend_build_variants(I b, I roles, int n, const double *parameters)
{
    int variant = (int)parameters[TA_FINAL_ROLE_BUILD_VARIANT];
    int decorated_variant = (int)parameters[TA_FINAL_ROLE_BUILD_DECORATED_VARIANT];
    int first = (int)parameters[TA_FINAL_BUILD_EXTENSION_FIRST];
    for (int i = first; i < n - 1; i++)
        if (b.v[i + 1] - b.v[i] <= parameters[TA_FINAL_SHORT_BUILD_MAX] &&
            (roles.v[i - 1] == decorated_variant || roles.v[i - 1] == variant)) {
            if (roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_DECORATED])
                roles.v[i] = decorated_variant;
            else if (roles.v[i] == parameters[TA_FINAL_ROLE_BUILD])
                roles.v[i] = variant;
        }
}

static void shorten_long_builds(I b, I roles, int n, const double *parameters)
{
    int build = (int)parameters[TA_FINAL_ROLE_BUILD];
    int variant = (int)parameters[TA_FINAL_ROLE_BUILD_VARIANT];
    if (n >= parameters[TA_FINAL_LONG_BUILD_MIN_SECTIONS]) {
        for (int i = 1; i < n - 1; i++)
            if (b.v[i + 1] - b.v[i] >= parameters[TA_FINAL_LONG_BUILD_MIN]) {
                if (roles.v[i] == variant)
                    roles.v[i] = build;
                else if (roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_DECORATED_VARIANT])
                    roles.v[i] = (int)parameters[TA_FINAL_ROLE_BUILD_DECORATED];
            }
        int penultimate = n - (int)parameters[TA_FINAL_PENULTIMATE_BACK];
        if (roles.v[penultimate] >= variant && roles.v[penultimate] <= parameters[TA_FINAL_ROLE_BUILD_SIMPLE])
            roles.v[penultimate] = build;
    }
}

static bool has_interior_hook(I roles, int n, const double *parameters)
{
    bool hook = false;
    for (int i = 1; i < n - 1; i++)
        hook |= is_hook(roles.v[i], parameters);
    return hook;
}

static void restore_missing_hook(I roles, int n, const double *parameters)
{
    if (has_interior_hook(roles, n, parameters))
        return;
    int first = (int)parameters[TA_FINAL_RESTORE_FIRST_SECTION];
    int limit = n - (int)parameters[TA_FINAL_PENULTIMATE_BACK];
    for (int i = first; i < limit; i++) {
        int previous = roles.v[i - 1];
        bool decorated = previous >= parameters[TA_FINAL_ROLE_BUILD_DECORATED_VARIANT] &&
                         previous <= parameters[TA_FINAL_ROLE_BUILD_DECORATED_SIMPLE];
        bool plain = previous == parameters[TA_FINAL_ROLE_BUILD_VARIANT] ||
                     previous == parameters[TA_FINAL_ROLE_BUILD_STAGED];
        if ((plain || decorated) && roles.v[i + 1] == parameters[TA_FINAL_ROLE_BREAK])
            roles.v[i] = (int)parameters[roles.v[n - 1] == parameters[TA_FINAL_ROLE_OUTRO_VARIANT]
                                             ? TA_FINAL_ROLE_HOOK_VARIANT : TA_FINAL_ROLE_HOOK];
    }
}

static void simplify_strong_builds(I roles, D metric, int n, const double *parameters)
{
    for (int i = 1; i < n - 1; i++)
        if (metric.v[i] >= parameters[TA_FINAL_STRONG_BUILD_LEVEL]) {
            if (roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_DECORATED_STAGED] ||
                roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_DECORATED_SIMPLE])
                roles.v[i] = (int)parameters[TA_FINAL_ROLE_BUILD_DECORATED];
            else if (roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_STAGED] ||
                     roles.v[i] == parameters[TA_FINAL_ROLE_BUILD_SIMPLE])
                roles.v[i] = (int)parameters[TA_FINAL_ROLE_BUILD];
        }
}

void final_adjust(ta_context *c, I b, bool high, D metric, Fills *r, const double *parameters)
{
    int n = r->roles.n;
    if (b.n != n + 1 || metric.n != n)
        ta_fail(c, TA_FAILED, "final fill dimensions");
    if (n < parameters[TA_FINAL_MIN_SECTIONS])
        return;
    I roles = r->roles;
    adjust_short_hooks(b, roles, n, parameters);
    if (!high)
        return;
    extend_hook_variants(b, roles, n, parameters);
    clear_short_fills(b, r, n, parameters);
    extend_build_variants(b, roles, n, parameters);
    shorten_long_builds(b, roles, n, parameters);
    if (n >= parameters[TA_FINAL_HOOK_AND_BUILD_MIN_SECTIONS]) {
        restore_missing_hook(roles, n, parameters);
        simplify_strong_builds(roles, metric, n, parameters);
    }
    int trim_min = (int)parameters[TA_FINAL_FILL_TRIM_MIN];
    int trim_back = (int)parameters[TA_FINAL_FILL_TRIM_BACK];
    for (int i = 0; i < n - 1; i++)
        if (r->starts.v[i] != 0 && b.v[i + 1] - r->starts.v[i] >= trim_min)
            r->starts.v[i] = b.v[i + 1] - trim_back;
}

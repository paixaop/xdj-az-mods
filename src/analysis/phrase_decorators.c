/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static int quarter_peak(D quarters, int base, int slots)
{
    int peak = 0;
    for (int j = 1; j < slots; j++)
        if (quarters.v[base + j] > quarters.v[base + peak])
            peak = j;
    return peak;
}
static void quarter_peaks(int start, int n, D quarters, I positions, D amplitudes, D ratios,
                          const double *parameters)
{
    int slots = (int)parameters[TA_BEAT_FOUR_QUARTERS_PER_BEAT];
    uint32_t mask = (uint32_t)parameters[TA_BEAT_FOUR_ADJACENCY_MASK];
    uint32_t adjacent = (uint32_t)parameters[TA_BEAT_FOUR_ADJACENCY_VALUE];
    double zero_level = parameters[TA_BEAT_FOUR_CORRESPONDENCE_ZERO];
    for (int i = 1; i < n; i++) {
        int base = (start - 1 + i) * slots, peak = quarter_peak(quarters, base, slots);
        positions.v[i] = peak + 1;
        amplitudes.v[i] = quarters.v[base + peak];
        uint32_t distance = (uint32_t)abs(positions.v[i] - positions.v[i - 1]);
        if (distance == 0)
            ratios.v[i - 1] = parameters[TA_BEAT_FOUR_EQUAL_PEAK_SCORE];
        else if ((distance & mask) == adjacent)
            ratios.v[i - 1] = quarters.v[base - 1 + positions.v[i - 1]] /
                              (amplitudes.v[i] == 0 ? zero_level : amplitudes.v[i]);
    }
}
static bool stable_quarter_amplitudes(D amplitudes, int n, const double *parameters)
{
    double zero_level = parameters[TA_BEAT_FOUR_STABILITY_ZERO];
    int stable = ta_max(0, n - (int)parameters[TA_BEAT_FOUR_STABILITY_TAIL_COUNT]);
    int first = (int)parameters[TA_BEAT_FOUR_STABILITY_START];
    if (stable >= parameters[TA_BEAT_FOUR_STABILITY_MIN_COUNT]) {
        double total = 0;
        for (int i = first; i < stable; i++) {
            if (amplitudes.v[i] == 0)
                amplitudes.v[i] = zero_level;
            if (amplitudes.v[i - 1] == 0)
                amplitudes.v[i - 1] = zero_level;
            total += ta_dmax(amplitudes.v[i] / amplitudes.v[i - 1],
                             amplitudes.v[i - 1] / amplitudes.v[i]);
        }
        int divisor = stable - (int)parameters[TA_BEAT_FOUR_STABILITY_DIVISOR_BACK];
        return total / divisor < parameters[TA_BEAT_FOUR_STABILITY_MAX];
    }
    return false;
}
static D beat_four_mean_window(D low, int start, int end, const double *parameters)
{
    if (start < 1 || end <= start)
        return (D){0};
    int first = start - (int)parameters[TA_BEAT_FOUR_MEAN_FIRST_BACK];
    int past_end = end - (int)parameters[TA_BEAT_FOUR_MEAN_LAST_BACK] + 1;
    if (first < 0 || past_end <= first || past_end > low.n)
        return (D){0};
    return (D){low.v + first, past_end - first};
}
bool beat_four(ta_context *c, int start, int end, bool last, D low, D quarters,
               const double *parameters)
{
    D window = beat_four_mean_window(low, start, end, parameters);
    if (!window.n)
        return false;
    double mean = d_sum(window, parameters) / window.n;
    int slots = (int)parameters[TA_BEAT_FOUR_QUARTERS_PER_BEAT];
    int quarters_in_section = (end - start) * slots;
    int qc = quarters_in_section - (int)parameters[TA_BEAT_FOUR_ORDINARY_END_BACK];
    if (qc >= parameters[TA_BEAT_FOUR_TAIL_TRIM_MIN] && !last)
        qc = quarters_in_section - (int)parameters[TA_BEAT_FOUR_TAIL_TRIM];
    int n = qc / slots;
    if (n <= 0 || (start - 1 + n) * slots > quarters.n)
        return false;
    I positions = ta_new_ints(c, n);
    D amplitudes = ta_new_doubles(c, n), ratios = ta_new_doubles(c, n);
    quarter_peaks(start, n, quarters, positions, amplitudes, ratios, parameters);
    bool accepted = d_sum(ratios, parameters) / n > parameters[TA_BEAT_FOUR_CORRESPONDENCE_MIN];
    if (!accepted) accepted = stable_quarter_amplitudes(amplitudes, n, parameters);
    ta_release(c, positions.v);
    ta_release(c, amplitudes.v);
    ta_release(c, ratios.v);
    return accepted && mean >= parameters[TA_BEAT_FOUR_LEVEL_MIN];
}
typedef struct {
    double maximum, minimum;
    int maxAt, minAt;
} LoopExtrema;
static LoopExtrema loop_extrema(ta_context *c, D second, int start, int end, int length,
                                const double *parameters)
{
    int window = (int)parameters[TA_SIMPLE_LOOP_WINDOW_BEATS];
    double scale = parameters[TA_SIMPLE_LOOP_MEAN_SCALE];
    bool long_section = length > parameters[TA_SIMPLE_LOOP_LONG_SPLIT_MAX];
    int middle = start + length / (int)parameters[TA_SIMPLE_LOOP_MIDPOINT_DIVISOR];
    LoopExtrema m = { .maxAt = end - window, .minAt = start };
    int begin = long_section ? middle : start;
    for (int p = begin; p <= end - window; p++) {
        double v = d_sum(d_view(c, second, p - 1, p - 1 + window), parameters) * scale;
        if (v > m.maximum) {
            m.maximum = v;
            m.maxAt = p;
        }
    }
    m.minimum = m.maximum;
    int lookahead = (int)parameters[TA_SIMPLE_LOOP_MINIMUM_END_LOOKAHEAD];
    int past_last = long_section ? middle - lookahead + 1 : end - window;
    for (int i = start - 1; i < past_last; i++) {
        double v = d_sum(d_view(c, second, i, i + window), parameters) * scale;
        if (v < m.minimum) {
            m.minimum = v;
            m.minAt = i + 1;
        }
    }
    return m;
}
static bool loop_growth(double fd, double sd, double slope, const double *parameters)
{
    if (!(sd > parameters[TA_SIMPLE_LOOP_RISE_MIN] &&
          slope < parameters[TA_SIMPLE_LOOP_SLOPE_MAX]))
        return false;
    return (fd < parameters[TA_SIMPLE_LOOP_LOW_RISE_MAX] &&
            slope > parameters[TA_SIMPLE_LOOP_SLOPE_SLOW_MIN]) ||
           sd - fd > parameters[TA_SIMPLE_LOOP_RISE_GAP_MIN] ||
           slope > parameters[TA_SIMPLE_LOOP_SLOPE_FAST_MIN];
}
static void simple_loop_segment(ta_context *c, UpAnalysis *r, int i, I b, D first, D second, D metric,
                                const double *parameters)
{
    int start = b.v[i], end = i + 1 < b.n ? b.v[i + 1] : first.n + 1, length = end - start;
    if (start <= 0 || end - 2 >= ta_min(first.n, second.n)) return;
    double fd = 0, sd = 0, slope = 0;
    if (length > parameters[TA_SIMPLE_LOOP_SHORT_MAX]) {
        int window = (int)parameters[TA_SIMPLE_LOOP_WINDOW_BEATS];
        LoopExtrema m = loop_extrema(c, second, start, end, length, parameters);
        r->metrics.v[i] = m.maximum;
        fd = (d_sum(d_view(c, first, m.maxAt - 1, m.maxAt - 1 + window), parameters) -
              d_sum(d_view(c, first, m.minAt - 1, m.minAt - 1 + window), parameters)) *
             parameters[TA_SIMPLE_LOOP_MEAN_SCALE];
        sd = m.maximum - m.minimum;
        if (m.maxAt != m.minAt) slope = sd / (m.maxAt - m.minAt);
    }
    if (metric.v[i] >= parameters[TA_SIMPLE_LOOP_LEVEL_MAX]) return;
    if (loop_growth(fd, sd, slope, parameters)) {
        r->roles.v[i] = (int)parameters[TA_SIMPLE_LOOP_OUTPUT_ROLE];
        r->starts.v[i] = start;
    }
}
UpAnalysis simple_loops(ta_context *c, I b, I roles, D first, D second, D metric,
                        const double *parameters)
{
    int n = roles.n;
    if (b.n != n || metric.n != n) ta_fail(c, TA_FAILED, "loop dimensions");
    UpAnalysis r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_doubles(c, n) };
    for (int i = 0; i < n; i++)
        if (roles.v[i] == parameters[TA_SIMPLE_LOOP_FIRST_INPUT_ROLE] ||
            roles.v[i] == parameters[TA_SIMPLE_LOOP_SECOND_INPUT_ROLE])
            simple_loop_segment(c, &r, i, b, first, second, metric, parameters);
    return r;
}
static void loop_spans(I roles, I spans, int loop_role)
{
    int n = roles.n;
    int remaining = 0, runEnd = n;
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == loop_role) {
            for (int next = i + 1; next < n; next++)
                if (roles.v[next] != loop_role) {
                    runEnd = next + 1;
                    break;
                }
            if (remaining == 0) {
                remaining = runEnd - (i + 1);
                spans.v[i] = remaining;
            }
            remaining--;
        }
}
I restore_loops(ta_context *c, I roles, I saved, D metric, const double *parameters)
{
    int n = roles.n;
    if (saved.n != n || metric.n != n)
        ta_fail(c, TA_FAILED, "restore loop dimensions");
    I r = i_copy(c, roles), spans = ta_new_ints(c, n);
    loop_spans(roles, spans, (int)parameters[TA_LOOP_RESTORE_ROLE]);
    for (int i = 0; i < n - 1; i++)
        if (spans.v[i] >= parameters[TA_LOOP_RESTORE_MIN_SPAN]) {
            int end = i + spans.v[i], selected = i;
            for (int j = i + 1; j < end; j++)
                if (metric.v[j] > metric.v[selected])
                    selected = j;
            for (int j = i; j < end; j++)
                if (j != selected)
                    r.v[j] = saved.v[j];
        }
    ta_release(c, spans.v);
    return r;
}
static double average(I counts, int from, int to, double scale)
{
    double total = 0;
    for (int i = counts.n + from; i <= counts.n + to; i++)
        total += counts.v[i];
    return total * scale;
}
typedef struct {
    int firstEight, afterEights, precedingFour;
    bool tail, preceding;
} RhythmRun;
typedef struct {
    bool staged, simple;
    int extent, secondOffset, thirdOffset;
} RhythmGrowth;
static bool role_in_rhythm_mask(int role, uint64_t mask)
{
    return role >= 0 && role < 64 && (mask & (UINT64_C(1) << role));
}

static bool rhythm_intro_role(int role, const double *parameters)
{
    return (role & ~1) == parameters[TA_RHYTHM_GROWTH_INTRO_ROLE];
}

static bool rhythm_build_role(int role, const double *parameters)
{
    return role_in_rhythm_mask(role, (uint64_t)parameters[TA_RHYTHM_RUN_ALLOWED_ROLES]);
}
static bool rhythm_group_range(int start, int groups, B activity, int beat_slots, int origin, int group_slots)
{
    int64_t end = (int64_t)start * beat_slots - origin + (int64_t)groups * group_slots;
    return groups > 0 && start > 0 && end <= activity.n;
}
static bool rhythm_dense_tail(B activity, int base, I codes, int g, const double *parameters)
{
    bool all = true;
    int second = g - (int)parameters[TA_RHYTHM_RUN_DENSE_SECOND_BACK];
    int offset = codes.v[second] == parameters[TA_RHYTHM_RUN_EIGHT_VALUE]
                     ? (int)parameters[TA_RHYTHM_RUN_DENSE_SHORT_START] : 0;
    int end = (int)parameters[TA_RHYTHM_RUN_DENSE_LONG_LENGTH];
    uint8_t mark = (uint8_t)(unsigned)parameters[TA_RHYTHM_RUN_DENSE_PACKED_PAIR];
    for (int j = offset; j < end; j++)
        all &= activity.v[base + j] == mark;
    return all;
}
static bool rhythm_prototype_matches(const uint8_t *segment, const double *prototype)
{
    const int words = TA_RHYTHM_RUN_FOUR_PROTOTYPE_3 - TA_RHYTHM_RUN_FOUR_PROTOTYPE_0 + 1;
    for (int i = 0; i < words; ++i)
        if (ta_le32(segment + i * sizeof(uint32_t)) != (uint32_t)prototype[i])
            return false;
    return true;
}

static I rhythm_codes(ta_context *c, B activity, int start, int groups, const double *parameters)
{
    I codes = ta_new_ints(c, groups + (int)parameters[TA_RHYTHM_RUN_CODE_PADDING]);
    int begin = start * (int)parameters[TA_RHYTHM_RUN_BEAT_ACTIVITY_SLOTS] -
                (int)parameters[TA_RHYTHM_RUN_ACTIVITY_ORIGIN_SLOTS];
    int width = (int)parameters[TA_RHYTHM_RUN_GROUP_ACTIVITY_SLOTS];
    int previous = (int)parameters[TA_RHYTHM_RUN_DENSE_SECOND_BACK] -
                   (int)parameters[TA_RHYTHM_RUN_DENSE_PREVIOUS_BIAS];
    for (int g = 0; g < groups; g++) {
        int base = begin + g * width;
        const uint8_t *segment = activity.v + base;
        if (rhythm_prototype_matches(segment, parameters + TA_RHYTHM_RUN_FOUR_PROTOTYPE_0))
            codes.v[g] = (int)parameters[TA_RHYTHM_RUN_FOUR_VALUE];
        if (rhythm_prototype_matches(segment, parameters + TA_RHYTHM_RUN_EIGHT_PROTOTYPE_0))
            codes.v[g] = (int)parameters[TA_RHYTHM_RUN_EIGHT_VALUE];
        else if (g >= parameters[TA_RHYTHM_RUN_DENSE_MIN_GROUP] &&
                 codes.v[g - previous] == parameters[TA_RHYTHM_RUN_EIGHT_VALUE] &&
                 rhythm_dense_tail(activity, base, codes, g, parameters))
            codes.v[g] = (int)parameters[TA_RHYTHM_RUN_DENSE_VALUE];
    }
    return codes;
}
static void rhythm_extend_eights(RhythmRun *run, I codes, int g, int groups, const double *parameters)
{
    int counter = g + (int)parameters[TA_RHYTHM_RUN_COUNTER_BIAS];
    int third = g + 1 + (int)parameters[TA_RHYTHM_RUN_THIRD_EIGHT_NEXT];
    if (codes.v[third] != parameters[TA_RHYTHM_RUN_EIGHT_VALUE])
        counter -= (int)parameters[TA_RHYTHM_RUN_TWO_EIGHTS_BACK];
    if (counter < groups) {
        run->afterEights = counter;
        run->tail = true;
    }
}
static void rhythm_find_run(RhythmRun *run, I codes, int groups, const double *parameters)
{
    run->tail = run->preceding = false;
    for (int g = 0; g < groups; g++)
        if (codes.v[g] == parameters[TA_RHYTHM_RUN_EIGHT_VALUE]) {
            int counter = g + (int)parameters[TA_RHYTHM_RUN_COUNTER_BIAS];
            run->firstEight = counter - (int)parameters[TA_RHYTHM_RUN_FIRST_EIGHT_BACK];
            if (run->firstEight < groups) {
                if (codes.v[g + 1] == parameters[TA_RHYTHM_RUN_EIGHT_VALUE]) rhythm_extend_eights(run, codes, g, groups, parameters);
                else {
                    run->preceding = run->firstEight > parameters[TA_RHYTHM_RUN_PRECEDING_EIGHT_MIN_EXCLUSIVE];
                    if (run->preceding)
                        run->precedingFour = counter - (int)parameters[TA_RHYTHM_RUN_PRECEDING_FOUR_BACK];
                    run->tail = true;
                    run->afterEights = counter - (int)parameters[TA_RHYTHM_RUN_ONE_EIGHT_BACK];
                }
            }
            break;
        }
}
static bool rhythm_has_tail(RhythmRun run, I codes, int groups, const double *parameters)
{
    return run.tail && run.afterEights <= groups && run.afterEights > 0 && codes.v[run.afterEights - 1] == parameters[TA_RHYTHM_RUN_DENSE_VALUE];
}
static void rhythm_save_run(RhythmAnalysis *r, int i, int start, RhythmRun run, const double *parameters)
{
    int count = run.afterEights - run.firstEight;
    int offset = (int)parameters[count == parameters[TA_RHYTHM_RUN_SINGLE_TAIL_COUNT] ? TA_RHYTHM_RUN_OFFSET_SHORT
                                 : count <= parameters[TA_RHYTHM_RUN_OFFSET_MIDDLE_MAX]
                                       ? TA_RHYTHM_RUN_OFFSET_MIDDLE : TA_RHYTHM_RUN_OFFSET_LONG];
    int group_beats = (int)parameters[TA_RHYTHM_RUN_GROUP_BEATS];
    int second = start + run.firstEight * group_beats - (int)parameters[TA_RHYTHM_RUN_SECOND_BEAT_ORIGIN];
    int proposed = second - offset;
    r->roles.v[i] = (int)parameters[TA_RHYTHM_RUN_OUTPUT_ROLE];
    r->starts.v[i] = proposed >= start ? proposed : second - (int)parameters[TA_RHYTHM_RUN_FALLBACK_SECOND];
    r->first.v[i] = r->starts.v[i];
    r->second.v[i] = second;
    r->third.v[i] = start + (run.afterEights - 1) * group_beats;
}
static bool rhythm_preceding_four(RhythmAnalysis r, int i, RhythmRun run, int length, I codes, int groups, const double *parameters)
{
    return r.roles.v[i] != parameters[TA_RHYTHM_RUN_OUTPUT_ROLE] && run.preceding && run.precedingFour > 0 && length > parameters[TA_RHYTHM_RUN_PRECEDING_MIN_LENGTH] &&
           codes.v[run.precedingFour - 1] == parameters[TA_RHYTHM_RUN_FOUR_VALUE] && run.precedingFour == groups - parameters[TA_RHYTHM_RUN_PRECEDING_LAST_GROUP];
}
static void rhythm_detect_runs(ta_context *c, RhythmAnalysis *r, I b, I roles, B activity, const double *parameters)
{
    RhythmRun run = { 0 };
    int group_beats = (int)parameters[TA_RHYTHM_RUN_GROUP_BEATS];
    int divisor = (int)parameters[TA_RHYTHM_RUN_ROLE_DIVISOR];
    int beat_slots = (int)parameters[TA_RHYTHM_RUN_BEAT_ACTIVITY_SLOTS];
    int origin = (int)parameters[TA_RHYTHM_RUN_ACTIVITY_ORIGIN_SLOTS];
    int group_slots = (int)parameters[TA_RHYTHM_RUN_GROUP_ACTIVITY_SLOTS];
    for (int i = 0; i < roles.n - 1; i++) {
        if (!rhythm_build_role(roles.v[i] % divisor, parameters)) continue;
        int start = b.v[i], length = b.v[i + 1] - start, groups = length / group_beats;
        if (!rhythm_group_range(start, groups, activity, beat_slots, origin, group_slots)) continue;
        I codes = rhythm_codes(c, activity, start, groups, parameters);
        rhythm_find_run(&run, codes, groups, parameters);
        if (rhythm_has_tail(run, codes, groups, parameters)) rhythm_save_run(r, i, start, run, parameters);
        if (rhythm_preceding_four(*r, i, run, length, codes, groups, parameters)) {
            r->roles.v[i] = (int)parameters[TA_RHYTHM_RUN_OUTPUT_ROLE];
            r->starts.v[i] = start + (run.precedingFour - 1) * group_beats;
            r->first.v[i] = r->starts.v[i];
            r->second.v[i] = r->starts.v[i] + (int)parameters[TA_RHYTHM_RUN_FALLBACK_SECOND];
            r->third.v[i] = r->starts.v[i] + (int)parameters[TA_RHYTHM_RUN_FALLBACK_THIRD];
        }
        ta_release(c, codes.v);
    }
}
static bool rhythm_before_hook(int base, int length, const double *parameters)
{
    return role_in_rhythm_mask(base, (uint64_t)parameters[TA_RHYTHM_GROWTH_ALLOWED_ROLES]) ||
           (length >= parameters[TA_RHYTHM_GROWTH_INTRO_MIN_LENGTH] && rhythm_intro_role(base, parameters));
}
static I rhythm_counts(ta_context *c, B activity, int start, int groups, const double *parameters)
{
    I counts = ta_new_ints(c, groups);
    int begin = start * (int)parameters[TA_RHYTHM_GROWTH_BEAT_ACTIVITY_SLOTS] -
                (int)parameters[TA_RHYTHM_GROWTH_ACTIVITY_ORIGIN_SLOTS];
    int width = (int)parameters[TA_RHYTHM_GROWTH_GROUP_ACTIVITY_SLOTS];
    for (int g = 0; g < groups; g++) {
        int p = begin + g * width;
        for (int j = 0; j < width; j++) counts.v[g] += activity.v[p + j];
    }
    return counts;
}
static RhythmGrowth rhythm_growth_32(I counts, double factor, const double *parameters)
{
    int first = (int)parameters[TA_RHYTHM_GROWTH_FOUR_OFFSET];
    int second = (int)parameters[TA_RHYTHM_GROWTH_TWO_OFFSET];
    int last = -(int)parameters[TA_RHYTHM_GROWTH_LAST_BACK];
    double a = average(counts, first, second - 1, parameters[TA_RHYTHM_GROWTH_MEAN_FOUR]);
    double b = average(counts, second, last - 1, parameters[TA_RHYTHM_GROWTH_MEAN_TWO]);
    double d = counts.v[counts.n + last];
    bool staged = a <= b * factor && b <= d * factor;
    return (RhythmGrowth){ staged, !staged && a < (b + d) * parameters[TA_RHYTHM_GROWTH_MEAN_TWO], (int)parameters[TA_RHYTHM_GROWTH_START_MEDIUM],
                           (int)parameters[TA_RHYTHM_GROWTH_START_SHORT],
                           (int)parameters[TA_RHYTHM_RUN_OFFSET_SHORT] };
}
static RhythmGrowth rhythm_growth_64(I counts, double factor, const double *parameters)
{
    int first = (int)parameters[TA_RHYTHM_GROWTH_EIGHT_OFFSET];
    int second = (int)parameters[TA_RHYTHM_GROWTH_FOUR_OFFSET];
    int third = (int)parameters[TA_RHYTHM_GROWTH_TWO_OFFSET];
    int last = -(int)parameters[TA_RHYTHM_GROWTH_LAST_BACK];
    double a = average(counts, first, second - 1, parameters[TA_RHYTHM_GROWTH_MEAN_EIGHT]);
    double b = average(counts, second, third - 1, parameters[TA_RHYTHM_GROWTH_MEAN_FOUR]);
    double d = average(counts, third, last, parameters[TA_RHYTHM_GROWTH_MEAN_THREE]);
    bool staged = a <= b * factor && b <= d * factor;
    int min = counts.v[0], max = min;
    int end = counts.n - (int)parameters[TA_RHYTHM_GROWTH_SPREAD_TAIL];
    for (int j = 1; j < end; j++) {
        min = ta_min(min, counts.v[j]);
        max = ta_max(max, counts.v[j]);
    }
    bool simple = !staged && (max - min <= parameters[TA_RHYTHM_GROWTH_SPREAD_MAX] ? b <= d * factor : a < (d + b) * parameters[TA_RHYTHM_GROWTH_MEAN_TWO]);
    return (RhythmGrowth){ staged, simple, (int)parameters[staged ? TA_RHYTHM_GROWTH_START_LONG
                                                   : TA_RHYTHM_GROWTH_START_MEDIUM],
                           (int)parameters[TA_RHYTHM_GROWTH_START_MEDIUM],
                           (int)parameters[TA_RHYTHM_GROWTH_START_SHORT] };
}
static RhythmGrowth rhythm_growth(I counts, int length, double factor, const double *parameters)
{
    if (length >= parameters[TA_RHYTHM_GROWTH_MEDIUM_LENGTH] && length < parameters[TA_RHYTHM_GROWTH_LONG_LENGTH]) return rhythm_growth_32(counts, factor, parameters);
    if (length >= parameters[TA_RHYTHM_GROWTH_LONG_LENGTH]) return rhythm_growth_64(counts, factor, parameters);
    if (length >= parameters[TA_RHYTHM_GROWTH_SHORT_LENGTH]) {
        int first = (int)parameters[TA_RHYTHM_GROWTH_TWO_OFFSET];
        int last = -(int)parameters[TA_RHYTHM_GROWTH_LAST_BACK];
        double a = average(counts, first, last - 1, parameters[TA_RHYTHM_GROWTH_MEAN_TWO]);
        double b = counts.v[counts.n + last];
        bool staged = a <= b * factor;
        return (RhythmGrowth){ staged, !staged && a < b, (int)parameters[TA_RHYTHM_GROWTH_START_SHORT],
                               (int)parameters[TA_RHYTHM_GROWTH_START_SHORT], (int)parameters[TA_RHYTHM_RUN_OFFSET_SHORT] };
    }
    return (RhythmGrowth){ 0 };
}
static void rhythm_save_growth(RhythmAnalysis *r, int i, int end, RhythmGrowth g, const double *parameters)
{
    r->roles.v[i] = (int)parameters[g.staged ? TA_RHYTHM_GROWTH_STAGED_ROLE : TA_RHYTHM_GROWTH_SIMPLE_ROLE];
    r->starts.v[i] = end - g.extent;
    r->first.v[i] = g.staged ? r->starts.v[i] : 0;
    r->second.v[i] = g.staged ? end - g.secondOffset : 0;
    r->third.v[i] = g.staged ? end - g.thirdOffset : 0;
}
static void rhythm_hook_growth(ta_context *c, RhythmAnalysis *r, I b, I roles, B activity, int hook, const double *parameters)
{
    int i = hook - 1, base = roles.v[i] % (int)parameters[TA_RHYTHM_GROWTH_ROLE_DIVISOR];
    int length = b.v[hook] - b.v[i], groups = length / (int)parameters[TA_RHYTHM_GROWTH_GROUP_BEATS];
    int beat_slots = (int)parameters[TA_RHYTHM_GROWTH_BEAT_ACTIVITY_SLOTS];
    int origin = (int)parameters[TA_RHYTHM_GROWTH_ACTIVITY_ORIGIN_SLOTS];
    int group_slots = (int)parameters[TA_RHYTHM_GROWTH_GROUP_ACTIVITY_SLOTS];
    if (r->roles.v[i] == parameters[TA_RHYTHM_RUN_OUTPUT_ROLE] || !rhythm_before_hook(base, length, parameters) ||
        !rhythm_group_range(b.v[i], groups, activity, beat_slots, origin, group_slots)) return;
    I counts = rhythm_counts(c, activity, b.v[i], groups, parameters);
    double factor = parameters[rhythm_intro_role(base, parameters) ? TA_RHYTHM_GROWTH_INTRO_FACTOR
                                                                    : TA_RHYTHM_GROWTH_NORMAL_FACTOR];
    RhythmGrowth growth = rhythm_growth(counts, length, factor, parameters);
    if (growth.staged || growth.simple) rhythm_save_growth(r, i, b.v[hook], growth, parameters);
    ta_release(c, counts.v);
}
static void rhythm_restore_intros(RhythmAnalysis *r, I roles, const double *parameters)
{
    int divisor = (int)parameters[TA_RHYTHM_GROWTH_RESTORE_ROLE_DIVISOR];
    for (int i = 0; i < roles.n - 1; i++)
        if (rhythm_intro_role(roles.v[i] % divisor, parameters) &&
            (r->roles.v[i] == parameters[TA_RHYTHM_GROWTH_STAGED_ROLE] ||
             r->roles.v[i] == parameters[TA_RHYTHM_GROWTH_SIMPLE_ROLE]))
            r->roles.v[i] = roles.v[i];
}
RhythmAnalysis decorate_rhythm(ta_context *c, I b, I roles, B activity, const double *parameters)
{
    int n = roles.n;
    if (b.n != n) ta_fail(c, TA_FAILED, "rhythm decorator dimensions");
    RhythmAnalysis r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n) };
    if (n <= 1) return r;
    rhythm_detect_runs(c, &r, b, roles, activity, parameters);
    for (int hook = 1; hook < n; hook++)
        if (roles.v[hook] == parameters[TA_RHYTHM_GROWTH_HOOK_ROLE])
            rhythm_hook_growth(c, &r, b, roles, activity, hook, parameters);
    rhythm_restore_intros(&r, roles, parameters);
    return r;
}
static bool outro_active_beat(B activity, int base, int subdivisions)
{
    bool active = false;
    for (int j = 0; j < subdivisions; j++)
        active |= activity.v[base + j] != 0;
    return active;
}
static int outro_last_activity(B activity, int lastStart, const double *parameters)
{
    int subdivisions = (int)parameters[TA_OUTRO_ACTIVE_SUBDIVISIONS];
    int count = activity.n / subdivisions - lastStart + 1;
    if (count <= 0) return 0;
    int lastActive = count;
    for (int i = count - 1; i >= 0; i--)
        if (outro_active_beat(activity, (lastStart - 1 + i) * subdivisions, subdivisions)) {
            lastActive = i + 1;
            break;
        }
    int lookback = (int)parameters[TA_OUTRO_ACTIVITY_LOOKBACK_FROM_PREVIOUS_BEAT];
    return lastActive >= count - lookback && lastActive != count ? lastActive + lastStart : 0;
}
static int outro_validate_activity(ta_context *c, D low, int selected, const double *parameters)
{
    int prior = selected - (int)parameters[TA_OUTRO_ACTIVITY_PRIOR_LEVEL_BACK];
    int latest = selected - (int)parameters[TA_OUTRO_ACTIVITY_LATEST_LEVEL_BACK];
    if (prior < 0 || latest >= low.n) return 0;
    if (low.v[latest] == 0) low.v[latest] = parameters[TA_OUTRO_ACTIVITY_ZERO_LEVEL];
    if (low.v[prior] == 0) low.v[prior] = parameters[TA_OUTRO_ACTIVITY_ZERO_LEVEL];
    if (low.v[latest] / low.v[prior] > parameters[TA_OUTRO_ACTIVITY_MAX_RATIO]) return 0;

    int start = selected - (int)parameters[TA_OUTRO_ACTIVITY_MEAN_START_BACK];
    int stop = selected - (int)parameters[TA_OUTRO_ACTIVITY_MEAN_END_BACK] + 1;
    double total = start < 0 ? 0 : d_sum(d_view(c, low, start, stop), parameters);
    return total * parameters[TA_OUTRO_ACTIVITY_MEAN_SCALE] < parameters[TA_OUTRO_ACTIVITY_MIN_MEAN] ? 0 : selected;
}
static int outro_last_audible(D low, int start, const double *parameters)
{
    for (int i = low.n - 1; i >= start - 1; i--)
        if (low.v[i] > parameters[TA_OUTRO_AUDIBLE_LEVEL])
            return i + (int)parameters[TA_OUTRO_LAST_AUDIBLE_ADVANCE];
    return low.n;
}
static void outro_rates(D samples, D ratios, D accelerations, int count, const double *parameters)
{
    for (int i = 1; i < count; i++) {
        if (samples.v[i] == 0) samples.v[i] = parameters[TA_OUTRO_RATIO_ZERO_LEVEL];
        if (samples.v[i - 1] == 0) samples.v[i - 1] = parameters[TA_OUTRO_RATIO_ZERO_LEVEL];
        ratios.v[i] = samples.v[i] / samples.v[i - 1];
        if (i > 1) accelerations.v[i] = ratios.v[i] / ratios.v[i - 1];
    }
}
static bool outro_rebounds(D samples, D ratios, D next, int i, int count, const double *parameters)
{
    next.n = 0;
    bool rising = false;
    for (int j = i + 1; j < count; j++)
        if (samples.v[j] > parameters[TA_OUTRO_REBOUND_LEVEL_MIN]) {
            next.v[next.n++] = ratios.v[j];
            rising |= ratios.v[j] > parameters[TA_OUTRO_REBOUND_RATIO_MIN];
        }
    return (next.n && d_sum(next, parameters) / next.n > parameters[TA_OUTRO_REBOUND_AVERAGE_MIN]) || rising;
}
static int outro_snap_fall(D ratios, int i, int candidate, int phase, const double *parameters)
{
    double snap_ratio = parameters[TA_OUTRO_PHASE_SNAP_RATIO_MIN];
    int origin = (int)parameters[TA_OUTRO_SNAP_PHASE_ORIGIN];
    int period = (int)parameters[TA_OUTRO_SNAP_PHASE_PERIOD];
    int remainder = (candidate + origin - phase) % period;
    int first_near = (int)parameters[TA_OUTRO_SNAP_FIRST_NEAR_BACK];
    int first_far = (int)parameters[TA_OUTRO_SNAP_FIRST_FAR_BACK];
    int first_min = ta_max((int)parameters[TA_OUTRO_SNAP_FIRST_MIN_INDEX], first_far);
    if (remainder == parameters[TA_OUTRO_SNAP_FIRST_REMAINDER] && i >= first_min &&
        (ratios.v[i - first_near] > snap_ratio || ratios.v[i - first_far] > snap_ratio))
        return candidate - (int)parameters[TA_OUTRO_SNAP_FIRST_DISTANCE];
    int second_near = (int)parameters[TA_OUTRO_SNAP_SECOND_NEAR_BACK];
    int second_far = (int)parameters[TA_OUTRO_SNAP_SECOND_FAR_BACK];
    int second_min = ta_max((int)parameters[TA_OUTRO_SNAP_SECOND_MIN_INDEX], second_far);
    if (remainder == parameters[TA_OUTRO_SNAP_SECOND_REMAINDER] && i >= second_min &&
        ratios.v[i - second_near] > snap_ratio && ratios.v[i - second_far] > snap_ratio)
        return candidate - (int)parameters[TA_OUTRO_SNAP_SECOND_DISTANCE];
    return candidate;
}
static int outro_falling(ta_context *c, D samples, D ratios, int count, int start, int lastAudible, int phase, int selected, const double *parameters)
{
    D next = ta_new_doubles(c, count);
    for (int i = 1; i < count; i++)
        if (ratios.v[i] < parameters[TA_OUTRO_FALL_RATIO_MAX]) {
            if (outro_rebounds(samples, ratios, next, i, count, parameters)) continue;
            int candidate = start + i;
            if (!selected || candidate < selected) {
                selected = candidate;
                if (candidate < lastAudible - (int)parameters[TA_OUTRO_FALL_SNAP_CLEARANCE])
                    selected = outro_snap_fall(ratios, i, candidate, phase, parameters);
            }
        }
    ta_release(c, next.v);
    return selected;
}
static bool outro_decays_to_silence(D samples, D ratios, int i, int count, const double *parameters)
{
    int j = i + 1;
    double floor = parameters[TA_OUTRO_FINAL_DECAY_LEVEL_MIN];
    while (j < count && samples.v[j] >= floor && ratios.v[j] <= parameters[TA_OUTRO_FINAL_DECAY_RATIO_MAX])
        j++;
    return j == count || samples.v[j] < floor;
}
static int outro_final_peak(D samples, D ratios, int count, int start, int selected, const double *parameters)
{
    for (int i = 1; i < count; i++)
        if (samples.v[i] > parameters[TA_OUTRO_FINAL_PEAK_LEVEL_MIN] &&
            ratios.v[i] > parameters[TA_OUTRO_FINAL_PEAK_RATIO_MIN] &&
            outro_decays_to_silence(samples, ratios, i, count, parameters)) {
            int candidate = start + i;
            if (!selected || candidate < selected) selected = candidate;
            break;
        }
    return selected;
}
static bool outro_slowing(D accelerations, int i, int count, const double *parameters)
{
    bool all = true;
    for (int j = i + (int)parameters[TA_OUTRO_ACCEL_TAIL_FIRST_DELTA]; j < count; j++)
        all &= accelerations.v[j] <= parameters[TA_OUTRO_TAIL_ACCELERATION_MAX];
    return all;
}
static int outro_accelerating(D accelerations, int count, int start, int selected, const double *parameters)
{
    double next_max = parameters[TA_OUTRO_NEXT_ACCELERATION_MAX];
    int second = (int)parameters[TA_OUTRO_ACCEL_NEXT_SECOND_DELTA];
    for (int i = 1; i < count - second; i++)
        if (accelerations.v[i] > parameters[TA_OUTRO_ACCELERATION_MIN] &&
            accelerations.v[i + 1] < next_max && accelerations.v[i + second] < next_max) {
            if (count - i <= parameters[TA_OUTRO_ACCELERATION_REQUIRED_TAIL]) break;
            if (outro_slowing(accelerations, i, count, parameters)) {
                int candidate = start + i;
                if (!selected || candidate < selected) selected = candidate;
                break;
            }
        }
    return selected;
}
static int outro_level_terminal(ta_context *c, D low, int lastStart, int phase, int selected, const double *parameters)
{
    int start = lastStart + (int)parameters[TA_OUTRO_LEVEL_START_OFFSET], lastAudible = outro_last_audible(low, start, parameters);
    int count = ta_max(0, lastAudible - start);
    D samples = start > 0 && start - 1 < low.n ? d_copy(c, d_view(c, low, start - 1, low.n)) : (D){ 0 };
    D ratios = ta_new_doubles(c, count), accelerations = ta_new_doubles(c, count);
    if (count > 1 && samples.n >= count) {
        outro_rates(samples, ratios, accelerations, count, parameters);
        selected = outro_falling(c, samples, ratios, count, start, lastAudible, phase, selected, parameters);
        selected = outro_final_peak(samples, ratios, count, start, selected, parameters);
    }
    selected = outro_accelerating(accelerations, count, start, selected, parameters);
    int terminal = !selected ? lastAudible : selected;
    int bias = (int)parameters[TA_OUTRO_TERMINAL_PHASE_BIAS];
    unsigned phase_mask = (unsigned)parameters[TA_OUTRO_TERMINAL_PHASE_PERIOD] - 1;
    if (((unsigned)(terminal - phase + bias) & phase_mask) == 0 && terminal < lastAudible)
        terminal += (int)parameters[TA_OUTRO_TERMINAL_ADVANCE];
    ta_release(c, samples.v);
    ta_release(c, ratios.v);
    ta_release(c, accelerations.v);
    return terminal;
}
Outro phrase_outro(ta_context *c, I b, I roles, D low, B activity, D quarters, int phase,
                   const double *parameters)
{
    if (b.n != roles.n) ta_fail(c, TA_FAILED, "outro dimensions");
    Outro out = { i_copy(c, roles), low.n };
    if (!b.n) return out;
    int lastStart = b.v[b.n - 1], selected = 0;
    low = d_copy(c, low);
    if (roles.v[roles.n - 1] == parameters[TA_OUTRO_ACTIVITY_ROLE])
        selected = outro_last_activity(activity, lastStart, parameters);
    selected = outro_validate_activity(c, low, selected, parameters);
    int terminal = outro_level_terminal(c, low, lastStart, phase, selected, parameters);
    if (beat_four(c, lastStart, terminal, true, low, quarters, parameters))
        out.roles.v[out.roles.n - 1] += (int)parameters[TA_OUTRO_BEAT_FOUR_ROLE_ADVANCE];
    out.terminal = terminal;
    ta_release(c, low.v);
    return out;
}
typedef struct {
    int first_section, base_role, filled_role, role_advance;
} BeatFourDecoration;

static void decorate_four_beats(ta_context *c, I b, I roles, FillProfile p, BeatFourDecoration stage,
                                const double *parameters)
{
    int terminal = b.n - (int)parameters[TA_BEAT_FOUR_TERMINAL_FROM_END];
    for (int i = stage.first_section; i < roles.n - 1; i++)
        if ((roles.v[i] == stage.base_role || roles.v[i] == stage.filled_role) &&
            beat_four(c, b.v[i], b.v[i + 1], i + 1 == terminal, p.low, p.quarters, parameters))
            roles.v[i] += stage.role_advance;
}
Decorated decorate_high(ta_context *c, I b, I roles, FillProfile p, D first, D second, D metric,
                        B activity, const double *parameters)
{
    if (b.n != roles.n || metric.n != roles.n)
        ta_fail(c, TA_FAILED, "high decorator dimensions");
    int terminal = p.low.n + 1;
    I ends = i_join(c, b, (I){ &terminal, 1 });
    Fills r = attach_fills(c, ends, roles, p, FILL_INTRO, parameters);
    BeatFourDecoration intro = {
        .first_section = (int)parameters[TA_BEAT_FOUR_INTRO_FIRST_SECTION],
        .base_role = (int)parameters[TA_BEAT_FOUR_INTRO_BASE_ROLE],
        .filled_role = (int)parameters[TA_BEAT_FOUR_INTRO_FILLED_ROLE],
        .role_advance = (int)parameters[TA_BEAT_FOUR_INTRO_ROLE_ADVANCE],
    };
    decorate_four_beats(c, b, r.roles, p, intro, parameters);
    I saved = i_copy(c, r.roles);
    UpAnalysis loop = simple_loops(c, b, r.roles, first, second, metric, parameters);
    RhythmAnalysis pattern = decorate_rhythm(c, b, loop.roles, activity, parameters);
    r.roles = restore_loops(c, pattern.roles, saved, loop.metrics, parameters);
    Fills candidates = attach_fills(c, ends, r.roles, p, FILL_UP_HOOK, parameters);
    r.roles = candidates.roles;
    for (int i = 0; i < r.roles.n; i++)
        if (candidates.starts.v[i]) {
            r.starts.v[i] = candidates.starts.v[i];
            r.routes.v[i] = candidates.routes.v[i];
        }
    BeatFourDecoration hook = {
        .first_section = (int)parameters[TA_BEAT_FOUR_HOOK_FIRST_SECTION],
        .base_role = (int)parameters[TA_BEAT_FOUR_HOOK_BASE_ROLE],
        .filled_role = (int)parameters[TA_BEAT_FOUR_HOOK_FILLED_ROLE],
        .role_advance = (int)parameters[TA_BEAT_FOUR_HOOK_ROLE_ADVANCE],
    };
    decorate_four_beats(c, b, r.roles, p, hook, parameters);
    Outro ending = phrase_outro(c, b, r.roles, p.low, activity, p.quarters, p.phase, parameters);
    r.roles = ending.roles;
    ends.v[ends.n - 1] = ending.terminal;
    clean_activity(c, ends, p, &r, parameters);
    final_adjust(c, ends, true, metric, &r, parameters);
    return (Decorated){ ends, r, pattern.starts, pattern.first, pattern.second, pattern.third };
}

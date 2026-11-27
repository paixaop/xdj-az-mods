/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
typedef struct {
    double value;
    int index;
} Ranked;
typedef struct {
    int start, n;
    Ranked *ranked;
} SnareWindow;
typedef struct {
    int position, ordinal;
    double strength;
} SnarePoint;
static int rank_order(const void *aa, const void *bb)
{
    const Ranked *a = aa, *b = bb;
    if (a->value > b->value)
        return -1;
    if (a->value < b->value)
        return 1;
    return (a->index > b->index) - (a->index < b->index);
}
static int point_order(const void *aa, const void *bb)
{
    const SnarePoint *a = aa, *b = bb;
    if (a->position < b->position)
        return -1;
    if (a->position > b->position)
        return 1;
    return (a->ordinal > b->ordinal) - (a->ordinal < b->ordinal);
}
static bool neighbor(int position, SnarePoint *points, int n, int distance, int before, int after)
{
    for (int i = 0; i < n; i++) {
        int value = points[i].position;
        if (value &&
            ((value - before <= position - distance && position - distance <= value + after) ||
             (value - before <= position + distance && position + distance <= value + after)))
            return true;
    }
    return false;
}
static bool snare_support_pair(B rhythm, int i, int before, int after, int mark)
{
    return rhythm.v[i - before] == mark && rhythm.v[i + after] == mark;
}
static bool supports_snare(B rhythm, int i, const double *parameters)
{
    int first_back = (int)parameters[TA_SNARE_SUPPORT_FIRST_BACK];
    int second_back = (int)parameters[TA_SNARE_SUPPORT_SECOND_BACK];
    int second_after = (int)parameters[TA_SNARE_SUPPORT_SECOND_AFTER];
    int mark = (int)parameters[TA_SNARE_SUPPORT_MARK];
    return (snare_support_pair(rhythm, i, first_back, (int)parameters[TA_SNARE_SUPPORT_FIRST_AFTER], mark) ||
            snare_support_pair(rhythm, i, second_back, second_after, mark) ||
            snare_support_pair(rhythm, i, first_back, (int)parameters[TA_SNARE_SUPPORT_THIRD_AFTER], mark)) &&
           rhythm.v[i] != parameters[TA_SNARE_SUPPORT_CENTER_MARK];
}
static uint8_t snare_pattern_byte(const double *pattern, int i)
{
    const int word_bytes = sizeof(uint32_t);
    uint32_t word = (uint32_t)pattern[i / word_bytes];
    return (uint8_t)(word >> (CHAR_BIT * (i % word_bytes)));
}
static bool snare_pattern_matches(B rhythm, int64_t i, int length, const double *pattern)
{
    for (int j = 0; j < length; j++) {
        uint8_t mark = i + j < rhythm.n ? rhythm.v[i + j] : 0;
        if (mark != snare_pattern_byte(pattern, j))
            return false;
    }
    return true;
}
static int snare_rhythm_type(I positions, B rhythm, const double *parameters)
{
    int groups = (int)(rhythm.n * parameters[TA_SNARE_PATTERN_GROUP_SCALE]);
    if (positions.n < parameters[TA_SNARE_PATTERN_MIN_POSITIONS] ||
        groups <= parameters[TA_SNARE_PATTERN_GROUP_MIN_EXCLUSIVE])
        return 0;
    int64_t limit = (int64_t)(positions.n / (int)parameters[TA_SNARE_PATTERN_BEAT_DIVISOR]) *
                    (int)parameters[TA_SNARE_PATTERN_WINDOW_STRIDE];
    int length = (int)(parameters[TA_SNARE_PATTERN_FIRST_LENGTH] + parameters[TA_SNARE_PATTERN_SECOND_LENGTH]);
    for (int64_t i = 0; i < limit; i++)
        if (snare_pattern_matches(rhythm, i, length, parameters + TA_SNARE_PATTERN_WORD_0))
            return (int)parameters[TA_SNARE_PATTERN_MATCH_TYPE];
    return 0;
}
static D snare_filtered(ta_context *c, D input, double rate, const double *parameters)
{
    const double *damping = parameters + TA_ENVELOPE_DAMPING_FOURTH_ORDER_FIRST;
    ta_phrase_biquad sections[4];
    for (int i = 0; i < 4; i++)
        sections[i] = second_order(rate, parameters[TA_SNARE_FILTER_CUTOFF], damping[i % 2], true);
    D tmp = filter(c, sections, 4, input, false);
    ta_phrase_biquad smooth = smoother(rate, parameters[TA_SNARE_FILTER_SMOOTH]);
    D filtered = filter(c, &smooth, 1, tmp, true);
    ta_release(c, tmp.v);
    for (int i = filtered.n - 1; i > 0; i--)
        filtered.v[i] -= filtered.v[i - 1];
    if (filtered.n)
        filtered.v[0] = 0;
    return filtered;
}
static void snare_windows(ta_context *c, SnareWindow *windows, int windowCount, I positions, D filtered,
                          int position_stride)
{
    for (int block = 0; block < windowCount; block++) {
        ta_check(c);
        int start = positions.v[block * position_stride], end = positions.v[(block + 1) * position_stride];
        if (start < 1 || end <= start || end - 1 > filtered.n)
            ta_fail(c, TA_FAILED, "snare window range");
        SnareWindow *w = &windows[block];
        w->start = start;
        w->n = end - start;
        w->ranked = ta_alloc(c, w->n, sizeof(Ranked));
        for (int i = 0; i < w->n; i++)
            w->ranked[i] = (Ranked){ filtered.v[start - 1 + i], i + 1 };
        qsort(w->ranked, w->n, sizeof(Ranked), rank_order);
    }
}
static bool snare_duplicate(SnarePoint *points, int filled, int position, int separation)
{
    for (int j = 0; j < filled; j++)
        if (abs(points[j].position - position) <= separation) return true;
    return false;
}
static int snare_collect_points(ta_context *c, SnareWindow *windows, int windowCount,
                                SnarePoint *points, int capacity, int separation,
                                const double *parameters)
{
    double peak_ratio = parameters[TA_SNARE_PEAK_SEPARATION_RATIO];
    int trailing = (int)parameters[TA_SNARE_RANK_LOOP_TRAILING];
    int next_rank = (int)parameters[TA_SNARE_RANK_NEXT];
    int next_next_rank = (int)parameters[TA_SNARE_RANK_NEXT_NEXT];
    int filled = 0;
    for (int block = 0; block < windowCount; block++) {
        SnareWindow w = windows[block];
        for (int i = 0; i < w.n - trailing; i++) {
            int position = w.start + w.ranked[i].index;
            bool duplicate = snare_duplicate(points, filled, position, separation);
            if (duplicate || filled >= capacity)
                continue;
            double current = w.ranked[i].value, next = w.ranked[i + next_rank].value;
            if (next < current * peak_ratio) {
                points[filled] = (SnarePoint){ position - 1, filled, current };
                filled++;
            } else if (i + next_next_rank >= w.n || next * peak_ratio <= w.ranked[i + next_next_rank].value)
                break;
        }
        ta_release(c, w.ranked);
    }
    return filled;
}
static bool snare_votes_against(B rhythm, int i, const double *parameters)
{
    int before = (int)parameters[TA_SNARE_SUPPORT_SECOND_BACK];
    int after = (int)parameters[TA_SNARE_SUPPORT_SECOND_AFTER];
    return i >= before && i < rhythm.n - after && !supports_snare(rhythm, i, parameters);
}
static void snare_qualify_point(SnarePoint *points, SnarePoint *output, int capacity, int i, double period,
                                int quarter, int eighth, int offset, B rhythm, const double *parameters)
{
    int position = points[i].position;
    double bias = parameters[TA_SNARE_QUALIFY_ROUND_BIAS];
    bool two =
        neighbor(position, points, capacity, (int)(period + period + bias), eighth, eighth);
    bool four =
        neighbor(position, points, capacity,
                 (int)(period * parameters[TA_SNARE_QUALIFY_LONG_PERIOD_MULTIPLIER] + bias), quarter, eighth);
    if (two) {
        int at = quarter ? (position - offset) / quarter : 0;
        if (snare_votes_against(rhythm, at, parameters)) {
            points[i].position = 0;
            points[i].strength = 0;
        }
    } else {
        points[i].position = 0;
        points[i].strength = 0;
        if (!four) {
            output[i].position = 0;
            output[i].strength = 0;
        }
    }
}
static void snare_remove_kicks(SnarePoint *output, int capacity, int quarter, int offset, B rhythm,
                               const double *parameters)
{
    int limit = rhythm.n - (int)parameters[TA_SNARE_KICK_REJECT_TAIL];
    for (int i = 0; i < capacity; i++)
        if (output[i].position) {
            int at = quarter ? (output[i].position - 1 - offset) / quarter : 0;
            if (at >= 0 && at < limit && rhythm.v[at] == parameters[TA_SNARE_KICK_REJECT_MARK])
                output[i].position = 0;
        }
}
SnarePositions genre_snares(ta_context *c, Samples samples, Timing t, int *rhythmType,
                           const double *parameters)
{
    double rate = samples.rate * parameters[TA_DECIMATE_MIDDLE_RATE_SCALE];
    D input = d_view(c, samples.middle, 0, ta_min(samples.middle.n, (int)(rate * parameters[TA_GENRE_SNARE_SCAN_SECONDS])));
    I positions = stage_coordinates(c, input.n, rate, t.bpm, t.offset, parameters);
    Attacks attack = attacks_init(c, rate, false, parameters);
    B rhythm = attacks_timing(c, &attack, input, rate, t.bpm, t.offset, 1, 1);
    ta_phrase_biquad low = second_order(rate, parameters[TA_RHYTHM_POWER_FILTER_CUTOFF],
                                       parameters[TA_RHYTHM_POWER_FILTER_DAMPING], false);
    Power powers = rhythm_power(c, &low, samples.middle, rate, t.bpm, positions, positions.n, true, parameters);
    RhythmFlags flags = rhythm_flags(c, rhythm, t.bpm, powers.bars, parameters);
    ta_release(c, powers.quarters.v);
    ta_release(c, powers.peaks.v);
    ta_release(c, powers.bars.v);
    *rhythmType = snare_rhythm_type(positions, rhythm, parameters);
    D filtered = snare_filtered(c, input, rate, parameters);
    int group_beats = (int)parameters[TA_SNARE_RANK_GROUP_BEATS];
    int windowCount = positions.n / group_beats;
    SnareWindow *windows = ta_alloc(c, windowCount, sizeof *windows);
    int position_stride = (int)parameters[TA_SNARE_RANK_WINDOW_POSITION_BYTES] / (int)sizeof(int32_t);
    snare_windows(c, windows, windowCount, positions, filtered, position_stride);
    ta_release(c, filtered.v);
    double period = rate * parameters[TA_SNARE_QUALIFY_PERIOD_SECONDS] / t.bpm;
    double bias = parameters[TA_SNARE_QUALIFY_ROUND_BIAS];
    int quarter = (int)(period * parameters[TA_SNARE_QUALIFY_QUARTER_FRACTION] + bias);
    int eighth = (int)(period * parameters[TA_SNARE_QUALIFY_EIGHTH_FRACTION] + bias);
    int offset = (int)((double)t.offset * rate * parameters[TA_SNARE_QUALIFY_OFFSET_TO_SECONDS] + bias);
    int capacity = windowCount * group_beats;
    int separation = quarter - (int)parameters[TA_GENRE_SNARE_SEPARATION_ADJUSTMENT];
    SnarePoint *points = ta_alloc(c, capacity, sizeof *points);
    int filled = snare_collect_points(c, windows, windowCount, points, capacity, separation, parameters);
    ta_release(c, windows);
    for (int i = filled; i < capacity; i++)
        points[i].ordinal = i;
    qsort(points, capacity, sizeof *points, point_order);
    SnarePoint *output = ta_alloc(c, capacity, sizeof *output);
    memcpy(output, points, capacity * sizeof *output);
    for (int i = 0; i < capacity; i++)
        if (points[i].position)
            snare_qualify_point(points, output, capacity, i, period, quarter, eighth, offset, rhythm, parameters);
    if (flags.fourKick != parameters[TA_SNARE_KICK_REJECT_FOUR_FLAG])
        snare_remove_kicks(output, capacity, quarter, offset, rhythm, parameters);
    qsort(output, capacity, sizeof *output, point_order);
    int first = 0;
    while (first < capacity && !output[first].position)
        first++;
    SnarePositions out = { ta_new_ints(c, capacity), ta_new_doubles(c, capacity), capacity - first,
                           first + 1 };
    for (int i = 0; i < capacity; i++) {
        out.values.v[i] = output[i].position - (i >= first ? offset : 0);
        out.strengths.v[i] = output[i].strength;
    }
    ta_release(c, points);
    ta_release(c, output);
    ta_release(c, positions.v);
    ta_release(c, rhythm.v);
    return out;
}

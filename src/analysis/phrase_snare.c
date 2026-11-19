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
static bool supports_snare(B rhythm, int i)
{
    return ((rhythm.v[i - 4] == 1 && rhythm.v[i + 2] == 1) ||
            (rhythm.v[i - 6] == 1 && rhythm.v[i + 5] == 1) ||
            (rhythm.v[i - 4] == 1 && rhythm.v[i + 4] == 1)) &&
           rhythm.v[i] != 1;
}
static bool snare_pattern_matches(B rhythm, int i, const uint8_t pattern[16])
{
    bool match = true;
    for (int j = 0; j < 16; j++)
        if ((i + j < rhythm.n ? rhythm.v[i + j] : 0) != pattern[j]) match = false;
    return match;
}
static int snare_rhythm_type(I positions, B rhythm)
{
    if (positions.n >= 16 && rhythm.n / 32 >= 2) {
        const uint8_t pattern[] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (int i = 0; i < (positions.n / 8) * 16; i++) {
            bool match = snare_pattern_matches(rhythm, i, pattern);
            if (match) {
                return 1;
            }
        }
    }
    return 0;
}
static D snare_filtered(ta_context *c, D input, double rate)
{
    const double damping[] = { 1.8477590650225735, 0.7653668647301797, 1.8477590650225735,
                               0.7653668647301797 };
    ta_phrase_biquad sections[4];
    for (int i = 0; i < 4; i++)
        sections[i] = second_order(rate, 1884.9555921538758, damping[i], true);
    D tmp = filter(c, sections, 4, input, false);
    ta_phrase_biquad smooth = smoother(rate, 31.41592653589793);
    D filtered = filter(c, &smooth, 1, tmp, true);
    ta_release(c, tmp.v);
    for (int i = filtered.n - 1; i > 0; i--)
        filtered.v[i] -= filtered.v[i - 1];
    if (filtered.n)
        filtered.v[0] = 0;
    return filtered;
}
static void snare_windows(ta_context *c, SnareWindow *windows, int windowCount, I positions, D filtered)
{
    for (int block = 0; block < windowCount; block++) {
        ta_check(c);
        int start = positions.v[block * 4], end = positions.v[(block + 1) * 4];
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
static int snare_collect_points(ta_context *c, SnareWindow *windows, int windowCount, SnarePoint *points, int capacity, int separation)
{
    int filled = 0;
    for (int block = 0; block < windowCount; block++) {
        SnareWindow w = windows[block];
        for (int i = 0; i < w.n - 1; i++) {
            int position = w.start + w.ranked[i].index;
            bool duplicate = snare_duplicate(points, filled, position, separation);
            if (duplicate || filled >= capacity)
                continue;
            double current = w.ranked[i].value, next = w.ranked[i + 1].value;
            if (next < current * 0.9886) {
                points[filled] = (SnarePoint){ position - 1, filled, current };
                filled++;
            } else if (i + 2 >= w.n || next * 0.9886 <= w.ranked[i + 2].value)
                break;
        }
        ta_release(c, w.ranked);
    }
    return filled;
}
static void snare_qualify_point(SnarePoint *points, SnarePoint *output, int capacity, int i, double period, int quarter, int eighth, int offset, B rhythm)
{
    int position = points[i].position;
    bool two =
        neighbor(position, points, capacity, (int)(period + period + 0.5), eighth, eighth);
    bool four =
        neighbor(position, points, capacity, (int)(period * 4 + 0.5), quarter, eighth);
    if (two) {
        int at = quarter ? (position - offset) / quarter : 0;
        if (at > 5 && at + 5 < rhythm.n && !supports_snare(rhythm, at)) {
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
static void snare_remove_kicks(SnarePoint *output, int capacity, int quarter, int offset, B rhythm)
{
    for (int i = 0; i < capacity; i++)
        if (output[i].position) {
            int at = quarter ? (output[i].position - 1 - offset) / quarter : 0;
            if (at >= 0 && at + 1 < rhythm.n && rhythm.v[at] == 1)
                output[i].position = 0;
        }
}
SnarePositions genre_snares(ta_context *c, Samples samples, Timing t, int *rhythmType)
{
    double rate = samples.rate / 8;
    D input = d_view(c, samples.middle, 0, ta_min(samples.middle.n, (int)(rate * 210)));
    I positions = stage_coordinates(c, input.n, rate, t.bpm, t.offset);
    Attacks attack = attacks_init(c, rate, false);
    B rhythm = attacks_timing(c, &attack, input, rate, t.bpm, t.offset, 1, 1);
    ta_phrase_biquad low = second_order(rate, 628.3185307179587, 1.4142135623730951, false);
    Power powers = rhythm_power(c, &low, samples.middle, rate, t.bpm, positions, positions.n, true);
    RhythmFlags flags = rhythm_flags(c, rhythm, t.bpm, powers.bars);
    ta_release(c, powers.quarters.v);
    ta_release(c, powers.peaks.v);
    ta_release(c, powers.bars.v);
    *rhythmType = snare_rhythm_type(positions, rhythm);
    D filtered = snare_filtered(c, input, rate);
    int windowCount = positions.n / 8;
    SnareWindow *windows = ta_alloc(c, windowCount, sizeof *windows);
    snare_windows(c, windows, windowCount, positions, filtered);
    ta_release(c, filtered.v);
    double period = rate * 60 / t.bpm;
    int quarter = (int)(period * 0.25 + 0.5), eighth = (int)(period * 0.125 + 0.5),
        offset = (int)((double)t.offset * rate * 0.001 + 0.5);
    int capacity = positions.n / 8 * 8, filled = 0, separation = quarter - 20;
    SnarePoint *points = ta_alloc(c, capacity, sizeof *points);
    filled = snare_collect_points(c, windows, windowCount, points, capacity, separation);
    ta_release(c, windows);
    for (int i = filled; i < capacity; i++)
        points[i].ordinal = i;
    qsort(points, capacity, sizeof *points, point_order);
    SnarePoint *output = ta_alloc(c, capacity, sizeof *output);
    memcpy(output, points, capacity * sizeof *output);
    for (int i = 0; i < capacity; i++)
        if (points[i].position) snare_qualify_point(points, output, capacity, i, period, quarter, eighth, offset, rhythm);
    if (flags.fourKick != 1) snare_remove_kicks(output, capacity, quarter, offset, rhythm);
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

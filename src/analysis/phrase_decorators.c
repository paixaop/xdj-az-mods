/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static int quarter_peak(D quarters, int base)
{
    int peak = 0;
    for (int j = 1; j < 4; j++)
        if (quarters.v[base + j] > quarters.v[base + peak])
            peak = j;
    return peak;
}
static void quarter_peaks(int start, int n, D quarters, I positions, D amplitudes, D ratios)
{
    for (int i = 1; i < n; i++) {
        int base = (start - 1 + i) * 4, peak = quarter_peak(quarters, base);
        positions.v[i] = peak + 1;
        amplitudes.v[i] = quarters.v[base + peak];
        int distance = abs(positions.v[i] - positions.v[i - 1]);
        if (distance == 0)
            ratios.v[i - 1] = 1;
        else if (distance == 1 || distance == 3)
            ratios.v[i - 1] = quarters.v[base - 1 + positions.v[i - 1]] /
                              (amplitudes.v[i] == 0 ? 0.003 : amplitudes.v[i]);
    }
}
static bool stable_quarter_amplitudes(D amplitudes, int n)
{
    int stable = ta_max(0, n - 4);
    if (stable > 2) {
        double total = 0;
        for (int i = 2; i < stable; i++) {
            if (amplitudes.v[i] == 0)
                amplitudes.v[i] = 0.003;
            if (amplitudes.v[i - 1] == 0)
                amplitudes.v[i - 1] = 0.003;
            total += ta_dmax(amplitudes.v[i] / amplitudes.v[i - 1],
                             amplitudes.v[i - 1] / amplitudes.v[i]);
        }
        return total / (stable - 2) < 1.12;
    }
    return false;
}
static bool beat_four_range(int start, int end, D low)
{
    return start >= 1 && end > start && end - 2 < low.n;
}
bool beat_four(ta_context *c, int start, int end, bool last, D low, D quarters)
{
    if (!beat_four_range(start, end, low))
        return false;
    double mean = d_sum(d_view(c, low, start - 1, end - 1)) / (end - start);
    int qc = (end - start) * 4 - 1;
    if (qc > 48 && !last)
        qc -= 32;
    int n = qc / 4;
    if (n <= 0 || (start - 1 + n) * 4 > quarters.n)
        return false;
    I positions = ta_new_ints(c, n);
    D amplitudes = ta_new_doubles(c, n), ratios = ta_new_doubles(c, n);
    quarter_peaks(start, n, quarters, positions, amplitudes, ratios);
    bool accepted = d_sum(ratios) / n > 0.8;
    if (!accepted) accepted = stable_quarter_amplitudes(amplitudes, n);
    ta_release(c, positions.v);
    ta_release(c, amplitudes.v);
    ta_release(c, ratios.v);
    return accepted && mean >= 0.22;
}
typedef struct {
    double maximum, minimum;
    int maxAt, minAt;
} LoopExtrema;
static LoopExtrema loop_extrema(ta_context *c, D second, int start, int end, int length)
{
    LoopExtrema m = { .maxAt = end - 8, .minAt = start };
    int begin = length >= 32 ? start + length / 2 : start;
    for (int p = begin; p <= end - 8; p++) {
        double v = d_sum(d_view(c, second, p - 1, p + 7)) * 0.125;
        if (v > m.maximum) {
            m.maximum = v;
            m.maxAt = p;
        }
    }
    m.minimum = m.maximum;
    int last = length >= 32 ? start + length / 2 - 2 : end - 8;
    for (int p = start; p <= last; p++) {
        double v = d_sum(d_view(c, second, p - 1, p + 7)) * 0.125;
        if (v < m.minimum) {
            m.minimum = v;
            m.minAt = p;
        }
    }
    return m;
}
static bool loop_growth(double fd, double sd, double slope)
{
    if (!(sd > 85 && slope < 22)) return false;
    return (fd < 10 && slope > 3.5) || sd - fd > 150 || slope > 6;
}
static void simple_loop_segment(ta_context *c, UpAnalysis *r, int i, I b, D first, D second, D metric)
{
    int start = b.v[i], end = i + 1 < b.n ? b.v[i + 1] : first.n + 1, length = end - start;
    if (start <= 0 || end - 2 >= ta_min(first.n, second.n)) return;
    double fd = 0, sd = 0, slope = 0;
    if (length >= 16) {
        LoopExtrema m = loop_extrema(c, second, start, end, length);
        r->metrics.v[i] = m.maximum;
        fd = (d_sum(d_view(c, first, m.maxAt - 1, m.maxAt + 7)) -
              d_sum(d_view(c, first, m.minAt - 1, m.minAt + 7))) *
             0.125;
        sd = m.maximum - m.minimum;
        if (m.maxAt != m.minAt) slope = sd / (m.maxAt - m.minAt);
    }
    if (metric.v[i] >= 0.16) return;
    if (loop_growth(fd, sd, slope)) {
        r->roles.v[i] = 31;
        r->starts.v[i] = start;
    }
}
UpAnalysis simple_loops(ta_context *c, I b, I roles, D first, D second, D metric)
{
    int n = roles.n;
    if (b.n != n || metric.n != n) ta_fail(c, TA_FAILED, "loop dimensions");
    UpAnalysis r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_doubles(c, n) };
    for (int i = 0; i < n; i++)
        if (roles.v[i] == 30 || roles.v[i] == 50) simple_loop_segment(c, &r, i, b, first, second, metric);
    return r;
}
static void loop_spans(I roles, I spans)
{
    int n = roles.n;
    int remaining = 0, runEnd = n;
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == 31) {
            for (int next = i + 1; next < n; next++)
                if (roles.v[next] != 31) {
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
I restore_loops(ta_context *c, I roles, I saved, D metric)
{
    int n = roles.n;
    if (saved.n != n || metric.n != n)
        ta_fail(c, TA_FAILED, "restore loop dimensions");
    I r = i_copy(c, roles), spans = ta_new_ints(c, n);
    loop_spans(roles, spans);
    for (int i = 0; i < n - 1; i++)
        if (spans.v[i] > 1) {
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
static bool rhythm_build_role(int role)
{
    return role == 10 || role == 11 || role == 30 || role == 31 || role == 50 || role == 51;
}
static bool rhythm_group_range(I b, int i, int groups, B activity)
{
    return groups > 0 && b.v[i] > 0 && (b.v[i] - 1) * 4 + groups * 16 <= activity.n;
}
static bool rhythm_dense_tail(B activity, int base, I codes, int g)
{
    bool all = true;
    for (int j = codes.v[g - 2] == 8 ? 2 : 0; j < 4; j++) all &= activity.v[base + j] == 1;
    return all;
}
static I rhythm_codes(ta_context *c, B activity, int start, int groups)
{
    I codes = ta_new_ints(c, groups + 8);
    for (int g = 0; g < groups; g++) {
        int base = (start - 1) * 4 + g * 16;
        bool four = true, eight = true;
        for (int j = 0; j < 16; j++) {
            four &= activity.v[base + j] == (j % 4 == 0);
            eight &= activity.v[base + j] == (j % 2 == 0);
        }
        if (four) codes.v[g] = 4;
        if (eight) codes.v[g] = 8;
        else if (g > 1 && codes.v[g - 1] == 8 && rhythm_dense_tail(activity, base, codes, g)) codes.v[g] = 16;
    }
    return codes;
}
static void rhythm_extend_eights(RhythmRun *run, I codes, int g, int groups)
{
    if (codes.v[g + 2] == 8) {
        if (g + 5 < groups) {
            run->afterEights = g + 5;
            run->tail = true;
        }
    } else if (g + 3 < groups) {
        run->afterEights = g + 3;
        run->tail = true;
    }
}
static void rhythm_find_run(RhythmRun *run, I codes, int groups)
{
    run->tail = run->preceding = false;
    for (int g = 0; g < groups; g++)
        if (codes.v[g] == 8) {
            run->firstEight = g + 1;
            if (run->firstEight < groups) {
                if (codes.v[g + 1] == 8) rhythm_extend_eights(run, codes, g, groups);
                else {
                    run->preceding = run->firstEight > 1;
                    if (run->firstEight >= 2) run->precedingFour = g;
                    run->tail = true;
                    run->afterEights = g + 2;
                }
            }
            break;
        }
}
static bool rhythm_has_tail(RhythmRun run, I codes, int groups)
{
    return run.tail && run.afterEights <= groups && run.afterEights > 0 && codes.v[run.afterEights - 1] == 16;
}
static void rhythm_save_run(RhythmAnalysis *r, int i, int start, RhythmRun run)
{
    int count = run.afterEights - run.firstEight, offset = count == 1 ? 8 : count < 3 ? 16
                                                                                      : 32,
        second = start + run.firstEight * 4 - 4, proposed = second - offset;
    r->roles.v[i] = 32;
    r->starts.v[i] = proposed >= start ? proposed : second - 4;
    r->first.v[i] = r->starts.v[i];
    r->second.v[i] = second;
    r->third.v[i] = start + (run.afterEights - 1) * 4;
}
static bool rhythm_preceding_four(RhythmAnalysis r, int i, RhythmRun run, int length, I codes, int groups)
{
    return r.roles.v[i] != 32 && run.preceding && run.precedingFour > 0 && length - 1 > 34 &&
           codes.v[run.precedingFour - 1] == 4 && run.precedingFour == groups - 2;
}
static void rhythm_detect_runs(ta_context *c, RhythmAnalysis *r, I b, I roles, B activity)
{
    RhythmRun run = { 0 };
    for (int i = 0; i < roles.n - 1; i++) {
        if (!rhythm_build_role(roles.v[i] % 100)) continue;
        int start = b.v[i], length = b.v[i + 1] - start, groups = length / 4;
        if (!rhythm_group_range(b, i, groups, activity)) continue;
        I codes = rhythm_codes(c, activity, start, groups);
        rhythm_find_run(&run, codes, groups);
        if (rhythm_has_tail(run, codes, groups)) rhythm_save_run(r, i, start, run);
        if (rhythm_preceding_four(*r, i, run, length, codes, groups)) {
            r->roles.v[i] = 32;
            r->starts.v[i] = start + (run.precedingFour - 1) * 4;
            r->first.v[i] = r->starts.v[i];
            r->second.v[i] = r->starts.v[i] + 4;
            r->third.v[i] = r->starts.v[i] + 8;
        }
        ta_release(c, codes.v);
    }
}
static bool rhythm_before_hook(int base, int length)
{
    return base == 30 || base == 31 || base == 50 || base == 51 || (length > 35 && (base == 10 || base == 11));
}
static I rhythm_counts(ta_context *c, B activity, int start, int groups)
{
    I counts = ta_new_ints(c, groups);
    for (int g = 0; g < groups; g++) {
        int p = (start - 1) * 4 + g * 16;
        for (int j = 0; j < 16; j++) counts.v[g] += activity.v[p + j];
    }
    return counts;
}
static RhythmGrowth rhythm_growth_32(I counts, double factor)
{
    double a = average(counts, -8, -5, 0.25), b = average(counts, -4, -3, 0.5), d = counts.v[counts.n - 2];
    bool staged = a <= b * factor && b <= d * factor;
    return (RhythmGrowth){ staged, !staged && a < (b + d) * 0.5, 32, 16, 8 };
}
static RhythmGrowth rhythm_growth_64(I counts, double factor)
{
    double a = average(counts, -16, -9, 0.125), b = average(counts, -8, -5, 0.25),
           d = average(counts, -4, -2, 0.33333);
    bool staged = a <= b * factor && b <= d * factor;
    int min = counts.v[0], max = min;
    for (int j = 1; j < counts.n - 1; j++) {
        min = ta_min(min, counts.v[j]);
        max = ta_max(max, counts.v[j]);
    }
    bool simple = !staged && (max - min < 5 ? b <= d * factor : a < (d + b) * 0.5);
    return (RhythmGrowth){ staged, simple, staged ? 48 : 32, 32, 16 };
}
static RhythmGrowth rhythm_growth(I counts, int length, double factor)
{
    if (length >= 32 && length < 64) return rhythm_growth_32(counts, factor);
    if (length >= 64) return rhythm_growth_64(counts, factor);
    if (length >= 16) {
        double a = average(counts, -4, -3, 0.5), b = counts.v[counts.n - 2];
        bool staged = a <= b * factor;
        return (RhythmGrowth){ staged, !staged && a < b, 16, 16, 8 };
    }
    return (RhythmGrowth){ 0 };
}
static void rhythm_save_growth(RhythmAnalysis *r, int i, int end, RhythmGrowth g)
{
    r->roles.v[i] = g.staged ? 32 : 33;
    r->starts.v[i] = end - g.extent;
    r->first.v[i] = g.staged ? r->starts.v[i] : 0;
    r->second.v[i] = g.staged ? end - g.secondOffset : 0;
    r->third.v[i] = g.staged ? end - g.thirdOffset : 0;
}
static void rhythm_hook_growth(ta_context *c, RhythmAnalysis *r, I b, I roles, B activity, int hook)
{
    int i = hook - 1, base = roles.v[i] % 100, length = b.v[hook] - b.v[i], groups = length / 4;
    if (r->roles.v[i] == 32 || !rhythm_before_hook(base, length) || !rhythm_group_range(b, i, groups, activity)) return;
    I counts = rhythm_counts(c, activity, b.v[i], groups);
    double factor = base == 10 || base == 11 ? 0.85 : 0.9;
    RhythmGrowth growth = rhythm_growth(counts, length, factor);
    if (growth.staged || growth.simple) rhythm_save_growth(r, i, b.v[hook], growth);
    ta_release(c, counts.v);
}
static void rhythm_restore_intros(RhythmAnalysis *r, I roles)
{
    for (int i = 0; i < roles.n - 1; i++)
        if ((roles.v[i] % 100 == 10 || roles.v[i] % 100 == 11) && (r->roles.v[i] == 32 || r->roles.v[i] == 33))
            r->roles.v[i] = roles.v[i];
}
RhythmAnalysis decorate_rhythm(ta_context *c, I b, I roles, B activity)
{
    int n = roles.n;
    if (b.n != n) ta_fail(c, TA_FAILED, "rhythm decorator dimensions");
    RhythmAnalysis r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n) };
    if (n <= 1) return r;
    rhythm_detect_runs(c, &r, b, roles, activity);
    for (int hook = 1; hook < n; hook++)
        if (roles.v[hook] == 40) rhythm_hook_growth(c, &r, b, roles, activity, hook);
    rhythm_restore_intros(&r, roles);
    return r;
}
static bool outro_active_beat(B activity, int base)
{
    bool active = false;
    for (int j = 0; j < 4; j++) active |= activity.v[base + j] != 0;
    return active;
}
static int outro_last_activity(B activity, int lastStart)
{
    int count = activity.n / 4 - lastStart + 1;
    if (count <= 0) return 0;
    int lastActive = count;
    for (int i = count - 1; i >= 0; i--)
        if (outro_active_beat(activity, (lastStart - 1 + i) * 4)) {
            lastActive = i + 1;
            break;
        }
    return lastActive > count - 13 && lastActive != count ? lastActive + lastStart : 0;
}
static int outro_validate_activity(ta_context *c, D low, int selected)
{
    if (selected && selected >= 5 && selected - 1 < low.n) {
        if (low.v[selected - 1] == 0) low.v[selected - 1] = 0.003;
        if (low.v[selected - 2] == 0) low.v[selected - 2] = 0.003;
        if (low.v[selected - 1] / low.v[selected - 2] > 0.8 ||
            d_sum(d_view(c, low, selected - 5, selected - 1)) * 0.25 < 0.05) selected = 0;
    }
    return selected;
}
static int outro_last_audible(D low, int start)
{
    for (int i = low.n - 1; i >= start - 1; i--)
        if (low.v[i] > 0.0007) return i + 2;
    return low.n;
}
static void outro_rates(D samples, D ratios, D accelerations, int count)
{
    for (int i = 1; i < count; i++) {
        if (samples.v[i] == 0) samples.v[i] = 0.003;
        if (samples.v[i - 1] == 0) samples.v[i - 1] = 0.003;
        ratios.v[i] = samples.v[i] / samples.v[i - 1];
        if (i > 1) accelerations.v[i] = ratios.v[i] / ratios.v[i - 1];
    }
}
static bool outro_rebounds(D samples, D ratios, D next, int i, int count)
{
    next.n = 0;
    bool rising = false;
    for (int j = i + 1; j < count; j++)
        if (samples.v[j] > 0.001) {
            next.v[next.n++] = ratios.v[j];
            rising |= ratios.v[j] > 1.1;
        }
    return (next.n && d_sum(next) / next.n > 0.86) || rising;
}
static int outro_snap_fall(D ratios, int i, int candidate, int phase)
{
    int remainder = (candidate + 1 - phase) % 4;
    if (remainder == 2 && i > 1 && (ratios.v[i - 1] > 1.3 || ratios.v[i - 2] > 1.3)) return candidate - 2;
    if (remainder == 3 && i > 2 && ratios.v[i - 2] > 1.3 && ratios.v[i - 3] > 1.3) return candidate - 3;
    return candidate;
}
static int outro_falling(ta_context *c, D samples, D ratios, int count, int start, int lastAudible, int phase, int selected)
{
    D next = ta_new_doubles(c, count);
    for (int i = 1; i < count; i++)
        if (ratios.v[i] < 0.5) {
            if (outro_rebounds(samples, ratios, next, i, count)) continue;
            int candidate = start + i;
            if (!selected || candidate < selected) {
                selected = candidate;
                if (candidate < lastAudible - 3) selected = outro_snap_fall(ratios, i, candidate, phase);
            }
        }
    ta_release(c, next.v);
    return selected;
}
static bool outro_decays_to_silence(D samples, D ratios, int i, int count)
{
    int j = i + 1;
    while (j < count && samples.v[j] >= 0.0007 && ratios.v[j] <= 1) j++;
    return j == count || samples.v[j] < 0.0007;
}
static int outro_final_peak(D samples, D ratios, int count, int start, int selected)
{
    for (int i = 1; i < count; i++)
        if (samples.v[i] > 0.2 && ratios.v[i] > 2 && outro_decays_to_silence(samples, ratios, i, count)) {
            int candidate = start + i;
            if (!selected || candidate < selected) selected = candidate;
            break;
        }
    return selected;
}
static bool outro_slowing(D accelerations, int i, int count)
{
    bool all = true;
    for (int j = i + 3; j < count; j++) all &= accelerations.v[j] <= 0.91;
    return all;
}
static int outro_accelerating(D accelerations, int count, int start, int selected)
{
    for (int i = 1; i < count - 2; i++)
        if (accelerations.v[i] > 1.3 && accelerations.v[i + 1] < 1.03 && accelerations.v[i + 2] < 1.03) {
            if (count <= i + 4) break;
            if (outro_slowing(accelerations, i, count)) {
                int candidate = start + i;
                if (!selected || candidate < selected) selected = candidate;
                break;
            }
        }
    return selected;
}
static int outro_level_terminal(ta_context *c, D low, int lastStart, int phase, int selected)
{
    int start = lastStart + 9, lastAudible = outro_last_audible(low, start);
    int count = ta_max(0, lastAudible - start);
    D samples = start > 0 && start - 1 < low.n ? d_copy(c, d_view(c, low, start - 1, low.n)) : (D){ 0 };
    D ratios = ta_new_doubles(c, count), accelerations = ta_new_doubles(c, count);
    if (count > 1 && samples.n >= count) {
        outro_rates(samples, ratios, accelerations, count);
        selected = outro_falling(c, samples, ratios, count, start, lastAudible, phase, selected);
        selected = outro_final_peak(samples, ratios, count, start, selected);
    }
    selected = outro_accelerating(accelerations, count, start, selected);
    int terminal = !selected ? lastAudible : selected;
    if (((terminal - phase + 1) & 3) == 0 && terminal < lastAudible) terminal++;
    ta_release(c, samples.v);
    ta_release(c, ratios.v);
    ta_release(c, accelerations.v);
    return terminal;
}
Outro phrase_outro(ta_context *c, I b, I roles, D low, B activity, D quarters, int phase)
{
    if (b.n != roles.n) ta_fail(c, TA_FAILED, "outro dimensions");
    Outro out = { i_copy(c, roles), low.n };
    if (!b.n) return out;
    int lastStart = b.v[b.n - 1], selected = 0;
    low = d_copy(c, low);
    if (roles.v[roles.n - 1] == 60) selected = outro_last_activity(activity, lastStart);
    selected = outro_validate_activity(c, low, selected);
    int terminal = outro_level_terminal(c, low, lastStart, phase, selected);
    if (beat_four(c, lastStart, terminal, true, low, quarters)) out.roles.v[out.roles.n - 1]++;
    out.terminal = terminal;
    ta_release(c, low.v);
    return out;
}
static void decorate_four_beats(ta_context *c, I b, I roles, FillProfile p, int first, int baseRole)
{
    for (int i = first; i < roles.n - 1; i++)
        if ((roles.v[i] == baseRole || roles.v[i] == baseRole * 11) &&
            beat_four(c, b.v[i], b.v[i + 1], i == roles.n - 2, p.low, p.quarters)) roles.v[i]++;
}
Decorated decorate_high(ta_context *c, I b, I roles, FillProfile p, D first, D second, D metric,
                        B activity)
{
    if (b.n != roles.n || metric.n != roles.n)
        ta_fail(c, TA_FAILED, "high decorator dimensions");
    int terminal = p.low.n + 1;
    I ends = i_join(c, b, (I){ &terminal, 1 });
    Fills r = attach_fills(c, ends, roles, p, FILL_INTRO);
    decorate_four_beats(c, b, r.roles, p, 0, 10);
    I saved = i_copy(c, r.roles);
    UpAnalysis loop = simple_loops(c, b, r.roles, first, second, metric);
    RhythmAnalysis pattern = decorate_rhythm(c, b, loop.roles, activity);
    r.roles = restore_loops(c, pattern.roles, saved, loop.metrics);
    Fills candidates = attach_fills(c, ends, r.roles, p, FILL_UP_HOOK);
    r.roles = candidates.roles;
    for (int i = 0; i < r.roles.n; i++)
        if (candidates.starts.v[i]) {
            r.starts.v[i] = candidates.starts.v[i];
            r.routes.v[i] = candidates.routes.v[i];
        }
    decorate_four_beats(c, b, r.roles, p, 1, 40);
    Outro ending = phrase_outro(c, b, r.roles, p.low, activity, p.quarters, p.phase);
    r.roles = ending.roles;
    ends.v[ends.n - 1] = ending.terminal;
    clean_activity(c, ends, p, &r);
    final_adjust(c, ends, true, metric, &r);
    return (Decorated){ ends, r, pattern.starts, pattern.first, pattern.second, pattern.third };
}

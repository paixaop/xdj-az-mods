/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
HighProfile normalize_high(ta_context *c, D input, int phase)
{
    int shift = (phase - 1) * 8;
    if (shift < 0 || shift >= input.n)
        return (HighProfile){ 0 };
    int count = input.n - shift;
    D out = d_copy(c, input);
    double max = d_max(d_view(c, input, shift, input.n)),
           factor = max == 0 ? 333.3333333333333 : 1 / max;
    for (int i = 0; i < count - 24; i++)
        out.v[i] = input.v[i + shift] * factor;
    return (HighProfile){ out, count / 8 };
}
static void snare_levels(ta_context *c, D high, int bars, D levels)
{
    for (int bar = 0; bar < bars - 4; bar++) {
        Peaks p = spectral_peaks(c, d_view(c, high, bar * 32, bar * 32 + 32));
        D heights = ta_new_doubles(c, p.n);
        for (int i = 0; i < p.n; i++)
            heights.v[i] = p.v[i].height;
        levels.v[bar] = d_sum(heights);
        ta_release(c, heights.v);
        ta_release(c, p.v);
    }
    double max = d_max(levels), scale = max == 0 ? 333.3333333333333 : 1 / max;
    for (int i = 0; i < bars; i++) {
        double v = levels.v[i] * scale;
        levels.v[i] = v == 0 ? 0.003 : v;
    }
}

typedef struct {
    double upper, lower;
} snare_thresholds;
static snare_thresholds snare_ratios(D levels, D ratios, D rising, D falling, int bars)
{
    int rc = 0, fc = 0;
    for (int i = 1; i < bars; i++) {
        ratios.v[i] = levels.v[i] / levels.v[i - 1];
        if (ratios.v[i] >= 1) {
            rising.v[i] = ratios.v[i];
            rc++;
        } else if (ratios.v[i] != 0) {
            falling.v[i] = ratios.v[i];
            fc++;
        }
    }
    double upper = rc == 0 ? 0.00345 : d_sum(rising) * 1.15 / rc,
           lower = fc == 0 ? 0.00255 : d_sum(falling) * 0.85 / fc;
    return (snare_thresholds){ upper, lower };
}

static void snare_percentiles(ta_context *c, D rising, int bars, double *upper, double *lower)
{
    if (bars >= 16) {
        int percentile = bars / 16 - 1;
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

static int falling_snare(double current, double next, double lower, double level, int i)
{
    if (current < 1 && next > lower && level > (current <= 0.5 ? 0.5 : 0.3)) {
        return next * current < 0.8 ? i + 1 : i + 2;
    }
    return 0;
}

static int rising_snare(double current, double next, double upper, double level, int i)
{
    if (current > 1 && next < upper && level > (current >= 2 ? 0.5 : 0.4)) {
        return next * current > 0.8 ? i + 1 : i + 2;
    }
    return 0;
}

static void select_snares(I output, D levels, D ratios, int bars, double upper, double lower)
{
    bool skip = false;
    int count = 0;
    for (int i = 1; i < bars - 4; i++) {
        if (skip) {
            skip = false;
            continue;
        }
        double current = ratios.v[i], next = ratios.v[i + 1];
        if (!((current > upper || current < lower) && ratios.v[i + 2] > 0.85 &&
              ratios.v[i + 2] < 1.15))
            continue;
        int falling = falling_snare(current, next, lower, levels.v[i - 1], i);
        if (falling) {
            output.v[count++] = falling;
            skip = true;
        }
        int rising = rising_snare(current, next, upper, levels.v[i], i);
        if (rising) {
            output.v[count++] = rising;
            skip = true;
        }
    }
}

I snare_candidates(ta_context *c, D high, int bars)
{
    if (bars < 0 || high.n < ta_max(0, bars - 4) * 32)
        ta_fail(c, TA_FAILED, "snare dimensions");
    I output = ta_new_ints(c, bars);
    if (bars < 6)
        return output;
    D levels = ta_new_doubles(c, bars), ratios = ta_new_doubles(c, bars),
      rising = ta_new_doubles(c, bars), falling = ta_new_doubles(c, bars);
    snare_levels(c, high, bars, levels);
    snare_thresholds limits = snare_ratios(levels, ratios, rising, falling, bars);
    double upper = limits.upper, lower = limits.lower;
    snare_percentiles(c, rising, bars, &upper, &lower);
    select_snares(output, levels, ratios, bars, upper, lower);
    return output;
}
static FillDetection growth_candidate(const double m[4], int end)
{
    int candidate;
    if (m[0] * 0.5 <= m[1]) {
        if (!(m[2] < m[1] * 0.5 && m[3] > m[2] * 1.25))
            return (FillDetection){ 0 };
        candidate = end - 4;
    } else if (m[2] > m[1] * 1.25)
        candidate = end - 8;
    else {
        if (!(m[3] > m[2] * 1.25))
            return (FillDetection){ 0 };
        candidate = end - 4;
    }
    return (FillDetection){ candidate, end - 1, FILL_GROWTH };
}

FillDetection fill_growth(ta_context *c, int start, int end, D low)
{
    if (end - start < 16 || end < 17 || end - 2 >= low.n)
        return (FillDetection){ 0 };
    double m[4];
    for (int g = 0; g < 4; g++)
        m[g] = d_sequential(d_view(c, low, end - 17 + g * 4, end - 13 + g * 4)) / 4;
    return growth_candidate(m, end);
}
static bool stable_cut_lead(D fine, int end)
{
    for (int i = end - 13; i < end - 6; i++) {
        if (fine.v[i] == 0)
            fine.v[i] = 0.003;
        double ratio = fine.v[i + 1] / fine.v[i];
        if (!(ratio >= 0.7 && ratio <= 1.5))
            return false;
    }
    return true;
}

static bool cut_level_change(ta_context *c, D fine, int i)
{
    double previous = fine.v[i - 1], current = fine.v[i], next = fine.v[i + 1],
           sum = d_sequential(d_view(c, fine, i - 4, i));
    bool decrease = current < previous * 0.66 && current < sum * 0.165 &&
                    next * current < current * 0.8 * previous;
    bool increase = current > previous + previous && current > sum * 0.5 &&
                    next * current > current * (previous + previous);
    return decrease || increase;
}

static int coarse_cut_start(ta_context *c, D low, int end)
{
    double m[4];
    for (int g = 0; g < 4; g++)
        m[g] = d_sequential(d_view(c, low, end - 17 + g * 4, end - 13 + g * 4)) * 0.25;
    for (int g = 1; g < 4; g++)
        if (m[g] < m[g - 1] * 0.5)
            return end - 16 + g * 4;
    return 0;
}
FillDetection fill_cut(ta_context *c, int start, int end, D low, D fine)
{
    if (end - start < 16 || end < 17 || end - 2 >= low.n)
        return (FillDetection){ 0 };
    int coarse = coarse_cut_start(c, low, end);
    if (coarse) return (FillDetection){ coarse, end - 1, FILL_CUT };
    if (end - 1 >= fine.n)
        return (FillDetection){ 0 };
    fine = d_copy(c, fine);
    FillDetection result = { 0 };
    if (!stable_cut_lead(fine, end)) goto done;
    for (int i = end - 5; i <= end - 2; i++) {
        bool changed = cut_level_change(c, fine, i);
        if (changed) {
            result = (FillDetection){ i + 1, end - 1, FILL_CUT };
            break;
        }
    }
done:
    ta_release(c, fine.v);
    return result;
}
static int collect_local_peaks(D v, int peaks[16])
{
    int n = 0;
    for (int i = 1; i < v.n - 1; i++)
        if (v.v[i] > v.v[i - 1] && v.v[i] >= v.v[i + 1])
            peaks[n++] = i;
    return n;
}

static int repeated_peak_run(D v, const int peaks[16], int n, double amplitudes[8], int spacings[8])
{
    int run = 0;
    for (int i = 0; i < n - 1; i++) {
        int a = peaks[i], b = peaks[i + 1], spacing = b - a;
        double ratio = v.v[b] / v.v[a];
        if (spacing >= 2 && spacing <= 4 && ratio > 0.7 && ratio < 1.43) {
            amplitudes[run] = v.v[a];
            spacings[run++] = spacing;
        } else
            run = 0;
    }
    return run;
}

int repeated_peak_score(D v)
{
    if (v.n < 3)
        return 0;
    /* Production windows contain sixteen samples; the same bound keeps the
       fixed scratch arrays valid for every local maximum in that window. */
    if (v.n > 16)
        return 0;
    int peaks[16], n = collect_local_peaks(v, peaks);
    if (n < 3)
        return 0;
    double amplitudes[8] = { 0 };
    int spacings[8] = { 0 }, run = 0;
    run = repeated_peak_run(v, peaks, n, amplitudes, spacings);
    if (run < 2)
        return 0;
    double max = ta_dmax(d_max((D){ amplitudes, 8 }), v.v[peaks[n - 1]]);
    if (max < 0.1)
        return 0;
    if (max < 0.35)
        for (int i = 1; i < run; i++)
            if (spacings[i] != spacings[i - 1])
                return 0;
    return run;
}
I repeated_scores(ta_context *c, HighProfile high)
{
    int count = high.beats / 4;
    I scores = ta_new_ints(c, count);
    if (high.beats < 20)
        return scores;
    for (int g = 0; g < count - 4; g++) {
        int start = g * 32 + 16;
        if (start + 16 > high.values.n)
            break;
        scores.v[g] = repeated_peak_score(d_view(c, high.values, start, start + 16));
    }
    return scores;
}
FillDetection repeated_upper(int end, int origin, I scores)
{
    int delta = end - origin, group = delta / 4;
    if (scores.n <= 3 || delta <= 7 || group < 2 || group - 1 >= scores.n ||
        scores.v[group - 1] <= 1 || scores.v[group - 2] >= 2)
        return (FillDetection){ 0 };
    return (FillDetection){ end - 2, 0, FILL_UPPER };
}
static void save(Fills *r, FillDetection d, int i, int role)
{
    if (d.start) {
        r->roles.v[i] = role;
        r->starts.v[i] = d.start;
        r->routes.v[i] = d.route;
    }
}
static void attach_intro_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p, int stage, I scores)
{
    int n = roles.n;
    for (int i = 0; i < n - 1; i++)
        if (stage != FILL_UP_HOOK && roles.v[i] == 10) {
            FillDetection d = fill_cut(c, boundaries.v[0], boundaries.v[1], p.low, p.quarterMean);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores);
            save(r, d, 0, 110);
        }
}

static void attach_build_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p, I scores)
{
    int n = roles.n;
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] >= 30 && roles.v[i] <= 33) {
            FillDetection d = fill_growth(c, boundaries.v[i], boundaries.v[i + 1], p.low);
            if (!d.start)
                d = fill_cut(c, boundaries.v[i], boundaries.v[i + 1], p.low, p.quarterMean);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores);
            save(r, d, i, roles.v[i] + 300);
        }
}

static void attach_hook_fills(ta_context *c, Fills *r, I boundaries, I roles, FillProfile p, I scores)
{
    int n = roles.n;
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 40) {
            FillDetection d =
                fill_cut(c, boundaries.v[i], boundaries.v[i + 1], p.low, p.quarterMean);
            if (!d.start)
                d = repeated_upper(boundaries.v[i + 1], boundaries.v[0], scores);
            save(r, d, i, 440);
        }
}

Fills attach_fills(ta_context *c, I boundaries, I roles, FillProfile p, int stage)
{
    int n = roles.n;
    if (boundaries.n != n + 1)
        ta_fail(c, TA_FAILED, "fill dimensions");
    Fills r = { i_copy(c, roles), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n) };
    if (n < 2)
        return r;
    HighProfile high = normalize_high(c, p.high, p.phase);
    I scores = repeated_scores(c, high);
    attach_intro_fills(c, &r, boundaries, roles, p, stage, scores);
    if (n > 2 && stage != FILL_INTRO) {
        attach_build_fills(c, &r, boundaries, roles, p, scores);
        attach_hook_fills(c, &r, boundaries, roles, p, scores);
    }
    ta_release(c, high.values.v);
    ta_release(c, scores.v);
    return r;
}
static bool quiet_fill_end(D low, D power, int end)
{
    int quiet = 0;
    for (int j = end - 5; j <= end - 2; j++)
        quiet += low.v[j] < 0.15;
    if (!(quiet > 2 && power.v[end - 3] < 0.04 && power.v[end - 4] < 0.04 &&
          (power.v[end - 2] < 0.04 || power.v[end - 5] < 0.04)))
        return false;
    return true;
}

static int preceding_fill_activity(D low, int start)
{
    int active = 0;
    if (start > 8 && start - 2 < low.n)
        for (int j = start - 9; j <= start - 2; j++)
            active += low.v[j] > 0.4;
    return active;
}

static void remove_fill_decoration(Fills *r, int i)
{
    r->starts.v[i] = 0;
    int role = r->roles.v[i];
    if (role >= 401)
        r->roles.v[i] -= 400;
    else if (role >= 301)
        r->roles.v[i] -= 300;
    else if (role >= 101)
        r->roles.v[i] -= 100;
}

static void clean_fill_activity(I boundaries, Fills *r, D low, D power, int i)
{
    int end = boundaries.v[i + 1], start = r->starts.v[i];
    if (!start || end - start <= 2 || end < 5 || end - 2 >= low.n)
        return;
    if (!quiet_fill_end(low, power, end)) return;
    int active = preceding_fill_activity(low, start);
    if (active > 5)
        r->auxiliary.v[i] = 1;
    else {
        remove_fill_decoration(r, i);
    }
}

void clean_activity(ta_context *c, I boundaries, FillProfile p, Fills *r)
{
    if (r->roles.n <= 1 || p.power.n < p.low.n * 4)
        return;
    D power = ta_new_doubles(c, p.low.n);
    for (int i = 0; i < p.low.n; i++)
        power.v[i] = d_sequential(d_view(c, p.power, i * 4, i * 4 + 4));
    double lm = d_max(p.low), pm = d_max(power), ls = lm == 0 ? 333.3333333333333 : 1 / lm,
           ps = pm == 0 ? 333.3333333333333 : 1 / pm;
    D low = d_copy(c, p.low);
    for (int i = 0; i < low.n; i++) {
        low.v[i] *= ls;
        power.v[i] *= ps;
    }
    for (int i = 0; i < r->roles.n - 1; i++) {
        clean_fill_activity(boundaries, r, low, power, i);
    }
    ta_release(c, power.v);
    ta_release(c, low.v);
}
bool is_hook(int r) { return r == 40 || r == 41 || r == 440 || r == 441; }
bool is_up(int r) { return (r >= 30 && r <= 33) || (r >= 330 && r <= 333); }
int undecorated(int r)
{
    if (r == 110 || r == 111)
        return r - 100;
    if (r >= 330 && r <= 333)
        return r - 300;
    if (r == 440 || r == 441)
        return r - 400;
    return r;
}
static int ending_hook_role(int current) { return current == 40 || current == 41 ? 60 : 61; }
static int intro_hook_role(int current) { return current == 40 ? 10 : current == 41 ? 11
                                                                  : current == 440  ? 110
                                                                                    : 111; }
static void remap_short_hook(I roles, int i, int n)
{
    int current = roles.v[i], previous = roles.v[i - 1];
    if (is_hook(previous))
        return;
    if (i == n - 1)
        roles.v[i] = ending_hook_role(current);
    else if (!is_hook(roles.v[i + 1])) {
        if (i == 1)
            roles.v[i] = intro_hook_role(current);
        if (previous == 50)
            roles.v[i] = previous;
        if (is_up(previous))
            roles.v[i] = current == 40 ? 30 : 330;
    }
}
static void adjust_short_hooks(I b, I roles, int n)
{
    for (int i = 1; i < n; i++)
        if (b.v[i + 1] - b.v[i] < 9 && is_hook(roles.v[i])) {
            remap_short_hook(roles, i, n);
        }
}

static bool neighboring_hook_variant(int preceding, int following) { return preceding == 41 || preceding == 441 || following == 41 || following == 441; }
static void extend_hook_variants(I b, I roles, int n)
{
    for (int i = 1; i < n; i++)
        if (b.v[i + 1] - b.v[i] < 32 && (roles.v[i] == 40 || roles.v[i] == 440)) {
            int preceding = roles.v[i - 1], following = i + 1 < n ? roles.v[i + 1] : 0;
            if (neighboring_hook_variant(preceding, following))
                roles.v[i] = roles.v[i] == 40 ? 41 : 441;
        }
}

static bool clear_fill_at_boundary(I b, I roles, int i, int n)
{
    int currentLength = b.v[i + 1] - b.v[i], followingLength = b.v[i + 2] - b.v[i + 1],
        following = roles.v[i + 1];
    bool clear = currentLength < 5;
    if (followingLength < 5) {
        clear = true;
        if (is_hook(following) && currentLength > 4 && i + 2 < n)
            clear = !is_hook(roles.v[i + 2]);
    }
    if (following == 50 || following == 60)
        clear = true;
    return clear;
}

static void clear_short_fills(I b, Fills *r, int n)
{
    I roles = r->roles;
    for (int i = 0; i < n - 1; i++) {
        bool clear = clear_fill_at_boundary(b, roles, i, n);
        if (clear) {
            r->starts.v[i] = 0;
            roles.v[i] = undecorated(roles.v[i]);
        }
    }
}

static void extend_build_variants(I b, I roles, int n)
{
    for (int i = 2; i < n - 1; i++)
        if (b.v[i + 1] - b.v[i] < 13 && (roles.v[i - 1] == 331 || roles.v[i - 1] == 31)) {
            if (roles.v[i] == 330)
                roles.v[i] = 331;
            else if (roles.v[i] == 30)
                roles.v[i] = 31;
        }
}

static void shorten_long_builds(I b, I roles, int n)
{
    if (n > 2) {
        for (int i = 1; i < n - 1; i++)
            if (b.v[i + 1] - b.v[i] > 68) {
                if (roles.v[i] == 31)
                    roles.v[i] = 30;
                else if (roles.v[i] == 331)
                    roles.v[i] = 330;
            }
        if (roles.v[n - 2] >= 31 && roles.v[n - 2] <= 33)
            roles.v[n - 2] = 30;
    }
}

static bool has_interior_hook(I roles, int n)
{
    bool hook = false;
    for (int i = 1; i < n - 1; i++)
        hook |= is_hook(roles.v[i]);
    return hook;
}

static void restore_missing_hook(I roles, int n)
{
    bool hook = has_interior_hook(roles, n);
    if (!hook)
        for (int i = 2; i < n - 2; i++) {
            int previous = roles.v[i - 1];
            if (((previous >= 331 && previous <= 333) || previous == 31 || previous == 32) &&
                roles.v[i + 1] == 50)
                roles.v[i] = roles.v[n - 1] == 61 ? 41 : 40;
        }
}

static void simplify_strong_builds(I roles, D metric, int n)
{
    for (int i = 1; i < n - 1; i++)
        if (metric.v[i] >= 0.28) {
            if (roles.v[i] == 332 || roles.v[i] == 333)
                roles.v[i] = 330;
            else if (roles.v[i] == 32 || roles.v[i] == 33)
                roles.v[i] = 30;
        }
}

void final_adjust(ta_context *c, I b, bool high, D metric, Fills *r)
{
    int n = r->roles.n;
    if (b.n != n + 1 || metric.n != n)
        ta_fail(c, TA_FAILED, "final fill dimensions");
    if (n < 2)
        return;
    I roles = r->roles;
    adjust_short_hooks(b, roles, n);
    if (!high)
        return;
    extend_hook_variants(b, roles, n);
    clear_short_fills(b, r, n);
    extend_build_variants(b, roles, n);
    shorten_long_builds(b, roles, n);
    if (n > 4) {
        restore_missing_hook(roles, n);
        simplify_strong_builds(roles, metric, n);
    }
    for (int i = 0; i < n - 1; i++)
        if (r->starts.v[i] != 0 && b.v[i + 1] - r->starts.v[i] > 4)
            r->starts.v[i] = b.v[i + 1] - 4;
}

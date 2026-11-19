/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double reciprocal(double v) { return v == 0 ? 1 / 0.003 : 1 / v; }
static D normalized(ta_context *c, D a)
{
    D r = ta_new_doubles(c, a.n);
    double scale = reciprocal(d_max(a));
    for (int i = 0; i < a.n; i++) {
        double v = a.v[i] * scale;
        r.v[i] = v == 0 ? 0.003 : v;
    }
    return r;
}
static void high_intro_ratios(int n, D intro, D ir)
{
    for (int i = 1; i < n; i++) {
        if (intro.v[i - 1] == 0)
            intro.v[i - 1] = 0.003;
        if (intro.v[i] == 0)
            intro.v[i] = 0.003;
        ir.v[i] = intro.v[i] / intro.v[i - 1];
    }
}
static D high_turns(ta_context *c, int n, I boundaries, D ni)
{
    D turns = ta_new_doubles(c, n);
    turns.n = 1;
    turns.v[0] = ni.v[1];
    if (n >= 4) {
        bool rising = ni.v[1] < ni.v[2];
        double mean = d_sequential(ni) / n;
        for (int i = 2; i < n - 1; i++) {
            if (boundaries.v[i + 1] - boundaries.v[i] < 16 && ni.v[i] <= mean * 0.65)
                continue;
            bool next = rising ? ni.v[i] <= ni.v[i + 1] : ni.v[i] < ni.v[i + 1];
            if (rising != next)
                turns.v[turns.n++] = ni.v[i];
            rising = next;
        }
    }
    return turns;
}
static bool high_simple_turns(D turns)
{
    int large = 0;
    for (int i = 1; i < turns.n; i++)
        if (fabs(turns.v[i - 1] - turns.v[i]) >= 0.3)
            large++;
    return turns.n <= 3 || large <= 2;
}
static void high_low_ratios(int n, D low, D lr)
{
    for (int i = 1; i < n; i++) {
        if (low.v[i - 1] == 0)
            low.v[i - 1] = 0.003;
        lr.v[i] = low.v[i] / low.v[i - 1];
    }
}
static double high_support(ta_context *c, int n, D intro, D nl, D support)
{
    double scale = reciprocal(d_sequential(intro) / n);
    for (int i = 1; i < n - 1; i++) {
        support.v[i] = 1;
        if (intro.v[i] != 0 && nl.v[i] >= 0.84) {
            if (intro.v[i - 1] == 0)
                intro.v[i - 1] = 0.003;
            if (intro.v[i + 1] == 0)
                intro.v[i + 1] = 0.003;
            support.v[i] =
                (intro.v[i] / intro.v[i + 1] + intro.v[i] / intro.v[i - 1]) * intro.v[i] * scale;
        }
    }
    return reciprocal(d_sequential(d_view(c, support, 1, n - 1)) / (n - 2));
}
static bool high_hook_rise(double ratio, double low, double intro)
{
    static const double limits[][3] = { { 4, 0.8, 0.3 }, { 2, 0.5, 0.75 }, { 1.2, 0.65, 0.45 } };
    for (unsigned i = 0; i < sizeof limits / sizeof *limits; i++)
        if (ratio > limits[i][0] && low > limits[i][1] && intro > limits[i][2]) return true;
    return false;
}
static bool high_supported_hook(double ratio, double low, double support)
{
    return low > 0.8 && ((ratio <= 1.5 && support > 1.25) || (ratio > 1.5 && support > 1));
}
static void high_seed_hooks(int n, I roles, D nl, D ir, D lr, D intro, D support, double scale)
{
    roles.v[n - 1] = 60;
    for (int i = 1; i < n - 1; i++) {
        if (roles.v[i - 1] == 40) {
            if (nl.v[i] > 0.7 && ir.v[i] > 0.7 && ir.v[i] < 1.9)
                roles.v[i] = 40;
        } else {
            double ratio = lr.v[i];
            if (high_hook_rise(ratio, nl.v[i], intro.v[i]))
                roles.v[i] = 40;
            if (roles.v[i] == 0 && high_supported_hook(ratio, nl.v[i], support.v[i] * scale))
                roles.v[i] = 40;
        }
    }
}
static double high_hook_peak(I roles, D levels)
{
    double peak = 0;
    for (int i = 0; i < roles.n - 1; i++)
        if (roles.v[i] == 40) peak = ta_dmax(peak, levels.v[i]);
    return reciprocal(peak);
}
static void high_remove_weak_hooks(int n, I roles, D low, D intro, D support)
{
    double hookLow = high_hook_peak(roles, low), hookIntro = high_hook_peak(roles, intro);
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == 40 && (low.v[i] * hookLow < 0.48 || intro.v[i] * hookIntro < 0.48) &&
            support.v[i] < 1.1)
            roles.v[i] = 0;
}
static void high_remove_repeated_hooks(int n, I roles, D ir)
{
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == 40 && roles.v[i + 1] == 40 && ir.v[i + 1] > 1.4)
            roles.v[i] = 0;
}
static int high_hook_count(I roles)
{
    int hooks = 0;
    for (int i = 1; i < roles.n - 1; i++) hooks += roles.v[i] == 40;
    return hooks;
}
static void high_fallback_hooks(ta_context *c, int n, I roles, D lr, D nl)
{
    D values = ta_new_doubles(c, n);
    values.n = 0;
    for (int i = 1; i < n - 1; i++)
        if (lr.v[i] != 0)
            values.v[values.n++] = lr.v[i];
    I order = rank_d(c, values, true);
    int rank = 0, selected = 0;
    for (int i = 1; i < n - 1; i++)
        if (rank < order.n && lr.v[i] == values.v[order.v[rank]]) {
            if (nl.v[i] > 0.84) {
                roles.v[i] = 40;
                if (selected > 2)
                    break;
                selected++;
            }
            rank++;
        }
}
static void high_before_hooks(int n, I roles)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 0 && roles.v[i + 1] == 40)
            roles.v[i] = 30;
}
static void high_after_hooks(int n, I roles)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 0 && roles.v[i - 1] == 40)
            roles.v[i] = 50;
}
static void high_extend_intro(int n, I roles, D ni, D ir)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 0 && roles.v[i - 1] == 10 && ni.v[i] < 0.5 && ir.v[i] < 1.1)
            roles.v[i] = 10;
}
static void high_rising_bridge(int n, I roles, I boundaries, D ir)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 0 && ((ir.v[i] > 1 && roles.v[i + 1] != 60) ||
                                (boundaries.v[i + 1] - boundaries.v[i] < 32 &&
                                 roles.v[i - 1] == 30 && roles.v[i + 1] == 30)))
            roles.v[i] = 30;
}
static void high_falling_bridge(int n, I roles, D ir)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == 0 && (roles.v[i + 1] == 60 || ir.v[i] < 1))
            roles.v[i] = 50;
    if (roles.v[n - 2] == 0)
        roles.v[n - 2] = 50;
}
static int high_after_peak_role(I roles, int i)
{
    return (roles.v[i + 1] == 60 || roles.v[i + 1] == 50) ? 50 : 30;
}
static void high_promote_bridge(I roles, D nl, D ni, int i, int hooks)
{
    if (!(roles.v[i - 1] == 30 && nl.v[i] > 0.75)) return;
    if (ni.v[i] > 0.7 && hooks < 4) roles.v[i] = 40;
    else if (ni.v[i] > 0.7 && hooks >= 4 && high_after_peak_role(roles, i) == 30) roles.v[i] = 30;
}
static void high_fill_unknown(I roles, D nl, D ni, int i, int hooks)
{
    if (nl.v[i] > 0.75 && ni.v[i] > 0.7)
        roles.v[i] = hooks < 4 ? 40 : high_after_peak_role(roles, i);
    else if (roles.v[i - 1] == 10) roles.v[i] = 30;
    else roles.v[i] = high_after_peak_role(roles, i) == 50 ? 50 : roles.v[i - 1];
}
static void high_complete_roles(int n, I roles, D nl, D ni)
{
    int hooks = high_hook_count(roles);
    for (int i = 1; i < n - 1; i++) {
        if (roles.v[i] == 50) high_promote_bridge(roles, nl, ni, i, hooks);
        else if (roles.v[i] == 0) high_fill_unknown(roles, nl, ni, i, hooks);
    }
}
static void high_restore_energy(int n, I roles, D nl, D lr)
{
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == 50 && roles.v[i + 1] != 60 && nl.v[i] > 0.9 && lr.v[i] > 0.9)
            roles.v[i] = 30;
}
HighRoles high_classify(ta_context *c, I boundaries, D introMeans, D lowMeans)
{
    int n = boundaries.n;
    if (n < 1 || introMeans.n != n || lowMeans.n != n)
        ta_fail(c, TA_FAILED, "invalid high-role dimensions");
    I roles = ta_new_ints(c, n);
    roles.v[0] = 10;
    if (n <= 2) {
        if (n == 2)
            roles.v[1] = 60;
        return (HighRoles){ roles, true };
    }
    D ni = normalized(c, introMeans), nl = normalized(c, lowMeans), intro = d_copy(c, introMeans),
      low = d_copy(c, lowMeans);
    D ir = ta_new_doubles(c, n), lr = ta_new_doubles(c, n), support = ta_new_doubles(c, n);
    high_intro_ratios(n, intro, ir);
    bool simple = high_simple_turns(high_turns(c, n, boundaries, ni));
    high_low_ratios(n, low, lr);
    double scale = high_support(c, n, intro, nl, support);
    high_seed_hooks(n, roles, nl, ir, lr, intro, support, scale);
    high_remove_weak_hooks(n, roles, low, intro, support);
    high_remove_repeated_hooks(n, roles, ir);
    if (!high_hook_count(roles)) high_fallback_hooks(c, n, roles, lr, nl);
    high_before_hooks(n, roles);
    high_after_hooks(n, roles);
    high_extend_intro(n, roles, ni, ir);
    high_rising_bridge(n, roles, boundaries, ir);
    high_falling_bridge(n, roles, ir);
    high_complete_roles(n, roles, nl, ni);
    high_restore_energy(n, roles, nl, lr);
    return (HighRoles){ roles, simple };
}
BlockMeans block_means(ta_context *c, I boundaries, D intro, D low, D quarterPower, int phase)
{
    int count = ta_min(intro.n, low.n);
    I limits = ta_new_ints(c, boundaries.n + 1);
    limits.n = 0;
    for (int i = 0; i < boundaries.n; i++)
        if (boundaries.v[i] <= count)
            limits.v[limits.n++] = boundaries.v[i];
    limits.v[limits.n++] = count + 1;
    BlockMeans r = { ta_new_doubles(c, boundaries.n), ta_new_doubles(c, boundaries.n),
                     ta_new_doubles(c, quarterPower.n / 4) };
    for (int i = 0; i < limits.n - 1; i++) {
        int start = i == 0 ? ta_max(1, limits.v[i] - phase + 1) : limits.v[i] - phase + 1,
            end = limits.v[i + 1] - phase + 1;
        int trim = ta_min(4, (int)((end - start) * 0.045 + 0.5)) * 4;
        D a = d_view(c, intro, start - 1, end - 1 - trim),
          b = d_view(c, low, limits.v[i] - 1, limits.v[i + 1] - 1 - trim);
        r.intro.v[i] = a.n ? d_sequential(a) / a.n : 0;
        r.low.v[i] = b.n ? d_sequential(b) / b.n : 0;
    }
    for (int i = 0; i < r.quarterPower.n; i++)
        r.quarterPower.v[i] = d_sequential(d_view(c, quarterPower, i * 4, i * 4 + 4)) * 0.25;
    return r;
}
static void flatten(D v)
{
    for (int i = 1; i < v.n; i++) {
        double d = fabs(v.v[i - 1] - v.v[i]);
        if (d < v.v[i - 1] * 0.1 || d < 0.05)
            v.v[i] = v.v[i - 1];
    }
}
static int left_valley(D v, int p, int fallback)
{
    while (p > 0) {
        if (!(v.v[p - 1] < v.v[p]))
            return p;
        p--;
    }
    return fallback;
}
static int right_valley(D v, int p, int fallback, bool restart)
{
    int i = p;
    while (i < v.n - 1 && v.v[i] <= v.v[i + 1])
        i++;
    if (i == v.n - 1)
        i = p + (restart ? 0 : 1);
    while (i < v.n - 1) {
        if (!(v.v[i + 1] < v.v[i]))
            return i;
        i++;
    }
    return fallback;
}
static void fill_edge(D values, D original, int left, int right)
{
    if (original.v[left] <= original.v[right]) {
        for (int i = left + 1; i < right; i++)
            values.v[i] = values.v[right];
    } else
        for (int i = left; i < right; i++)
            values.v[i] = values.v[left];
}
static D route_inverted(ta_context *c, D low, int phase)
{
    int n = low.n / 4;
    D bars = ta_new_doubles(c, n);
    for (int i = 0; i < n - 1; i++)
        bars.v[i] = d_sequential(d_view(c, low, phase - 1 + i * 4, phase + 3 + i * 4)) * 0.25;
    flatten(bars);
    D inverted = ta_new_doubles(c, n);
    for (int i = 0; i < n; i++)
        inverted.v[i] = 1 - bars.v[i];
    return inverted;
}
static D route_filtered(ta_context *c, D inverted)
{
    int n = inverted.n;
    D filtered = d_copy(c, inverted);
    Peaks peaks = spectral_peaks(c, inverted);
    int left = 0, right = 0;
    for (int i = peaks.n - 2; i >= 1; i--) {
        if (peaks.v[i].width >= 4) {
            memmove(peaks.v + i, peaks.v + i + 1, (size_t)(peaks.n - i - 1) * sizeof *peaks.v);
            peaks.n--;
            continue;
        }
        left = left_valley(inverted, peaks.v[i].position - 1, left);
        right = right_valley(inverted, peaks.v[i].position - 1, right, false);
        for (int j = left + 1; j < right; j++)
            filtered.v[j] = filtered.v[left];
    }
    if (peaks.n && peaks.v[0].position > 0) {
        left = left_valley(inverted, peaks.v[0].position - 1, 0);
        right = right_valley(inverted, peaks.v[0].position - 1, right, true);
        fill_edge(filtered, inverted, left, right);
        if (peaks.n > 1) {
            left = left_valley(inverted, peaks.v[peaks.n - 1].position - 1, left);
            right = right_valley(inverted, peaks.v[peaks.n - 1].position - 1, n - 1, true);
            fill_edge(filtered, inverted, left, right);
        }
    }
    for (int i = 0; i < n; i++)
        filtered.v[i] = 1 - filtered.v[i];
    flatten(filtered);
    return filtered;
}
static D route_changes(ta_context *c, D filtered)
{
    int n = filtered.n;
    D changes = ta_new_doubles(c, n);
    for (int i = 0; i < n - 1; i++)
        changes.v[i] = filtered.v[i + 1] - filtered.v[i];
    if (n > 34)
        changes = d_view(c, changes, 16, n - 17);
    I order = rank_d(c, changes, true);
    D sorted = ta_new_doubles(c, changes.n);
    for (int i = 0; i < changes.n; i++)
        sorted.v[i] = changes.v[order.v[i]];
    return sorted;
}
static void route_histograms(D sorted, D positive, D negative)
{
    int n = sorted.n;
    int half = n / 2;
    for (int i = 0; i < half; i++) {
        positive.v[half - 1 - i] = sorted.v[i];
        negative.v[half - 1 - i] = -sorted.v[n - 1 - i];
    }
    for (int i = 0; i < half - 1; i++) {
        positive.v[half + i] = sorted.v[i];
        negative.v[half + i] = -sorted.v[n - 1 - i];
    }
}
static int route_loop_count(I types)
{
    int count = 0;
    for (int i = 0; i < types.n; i++) count += types.v[i] == 31;
    return count;
}
static int route_rhythm_count(I types)
{
    int count = 0;
    for (int i = 1; i < types.n; i++)
        count += types.v[i] == 32 || (types.v[i] == 33 && types.v[i - 1] != 10);
    return count;
}
static bool route_high(double score, double duration, I loopTypes, I rhythmTypes)
{
    bool longTrack = duration > 600;
    double strong = longTrack ? duration * 0.00833 : 3;
    int loopThreshold = longTrack ? (int)(duration * 0.00667) : 2;
    int rhythmThreshold = longTrack ? (int)(duration * 0.00333) : 1;
    return score >= strong || route_loop_count(loopTypes) >= loopThreshold || route_rhythm_count(rhythmTypes) >= rhythmThreshold;
}
Route phrase_route(ta_context *c, D low, int phase, double duration, I loopTypes, I rhythmTypes,
                   int mood)
{
    if (phase < 1 || phase > 4 || low.n < 8 || duration < 0)
        ta_fail(c, TA_FAILED, "invalid route dimensions");
    D inverted = route_inverted(c, low, phase);
    D filtered = route_filtered(c, inverted);
    D sorted = route_changes(c, filtered);
    D positive = ta_new_doubles(c, sorted.n), negative = ta_new_doubles(c, sorted.n);
    route_histograms(sorted, positive, negative);
    Peaks pp = spectral_peaks(c, positive), qq = spectral_peaks(c, negative);
    Peak p = pp.v[0], q = qq.v[0];
    double score = fma(q.height, q.width, p.width * p.height);
    double weak = duration > 600 ? duration * 0.00417 : 1.5;
    bool high = route_high(score, duration, loopTypes, rhythmTypes);
    return (Route){ high ? MOOD_HIGH : mood, high || score >= weak ? 1 : 2, score, positive, negative };
}

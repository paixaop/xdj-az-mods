/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double reciprocal(double v, const double *parameters)
{
    return v == 0 ? parameters[TA_HIGH_ROLE_ZERO_MAX_INVERSE] : 1 / v;
}
static D normalized(ta_context *c, D a, const double *parameters)
{
    D r = ta_new_doubles(c, a.n);
    double scale = reciprocal(d_max(a), parameters);
    for (int i = 0; i < a.n; i++) {
        double v = a.v[i] * scale;
        r.v[i] = v == 0 ? parameters[TA_HIGH_ROLE_NORMALIZED_ZERO_LEVEL] : v;
    }
    return r;
}
static void high_intro_ratios(int n, D intro, D ir, const double *parameters)
{
    for (int i = 1; i < n; i++) {
        if (intro.v[i - 1] == 0)
            intro.v[i - 1] = parameters[TA_HIGH_ROLE_INTRO_RATIO_ZERO_LEVEL];
        if (intro.v[i] == 0)
            intro.v[i] = parameters[TA_HIGH_ROLE_INTRO_RATIO_ZERO_LEVEL];
        ir.v[i] = intro.v[i] / intro.v[i - 1];
    }
}
static D high_turns(ta_context *c, int n, I boundaries, D ni, const double *parameters)
{
    int seed = (int)parameters[TA_HIGH_TURN_SEED_INDEX];
    int first = (int)parameters[TA_HIGH_TURN_FIRST_INTERIOR_INDEX];
    D turns = ta_new_doubles(c, n);
    turns.n = 1;
    turns.v[0] = ni.v[seed];
    if (n >= parameters[TA_HIGH_TURN_SECTION_MIN]) {
        bool rising = ni.v[seed] < ni.v[first];
        double mean = d_sequential(ni) / n;
        for (int i = first; i < n - 1; i++) {
            if (boundaries.v[i + 1] - boundaries.v[i] <= parameters[TA_HIGH_ROLE_TURN_SHORT_MAX] && ni.v[i] <= mean * parameters[TA_HIGH_ROLE_TURN_SHORT_SCALE])
                continue;
            bool next = rising ? ni.v[i] <= ni.v[i + 1] : ni.v[i] < ni.v[i + 1];
            if (rising != next)
                turns.v[turns.n++] = ni.v[i];
            rising = next;
        }
    }
    return turns;
}
static bool high_simple_turns(D turns, const double *parameters)
{
    int large = 0;
    for (int i = 1; i < turns.n; i++)
        if (fabs(turns.v[i - 1] - turns.v[i]) >= parameters[TA_HIGH_ROLE_TURN_DELTA_MIN])
            large++;
    return turns.n < parameters[TA_HIGH_SIMPLE_TURN_COUNT_EXCLUSIVE] ||
           large <= parameters[TA_HIGH_ROLE_SIMPLE_MAX_LARGE_TURNS];
}
static void high_low_ratios(int n, D low, D lr, const double *parameters)
{
    for (int i = 1; i < n; i++) {
        if (low.v[i - 1] == 0)
            low.v[i - 1] = parameters[TA_HIGH_ROLE_LOW_RATIO_ZERO_LEVEL];
        lr.v[i] = low.v[i] / low.v[i - 1];
    }
}
static double high_support(ta_context *c, int n, D intro, D nl, D support, const double *parameters)
{
    double scale = reciprocal(d_sequential(intro) / n, parameters);
    int skip = (int)parameters[TA_HIGH_ROLE_SUPPORT_SKIP];
    for (int i = skip; i < n - 1; i++) {
        support.v[i] = parameters[TA_HIGH_ROLE_SUPPORT_DEFAULT];
        if (intro.v[i] != 0 && nl.v[i] >= parameters[TA_HIGH_ROLE_SUPPORT_LEVEL_MIN]) {
            if (intro.v[i - 1] == 0)
                intro.v[i - 1] = parameters[TA_HIGH_ROLE_PREVIOUS_INTRO_SUPPORT_ZERO];
            if (intro.v[i + 1] == 0)
                intro.v[i + 1] = parameters[TA_HIGH_ROLE_NEXT_INTRO_SUPPORT_ZERO];
            support.v[i] =
                (intro.v[i] / intro.v[i + 1] + intro.v[i] / intro.v[i - 1]) * intro.v[i] * scale;
        }
    }
    D inner = d_view(c, support, skip, n - 1);
    return reciprocal(d_sequential(inner) / (n - parameters[TA_HIGH_ROLE_SUPPORT_MEAN_SHRINK]), parameters);
}
static bool high_hook_rise(double ratio, double low, double intro, const double *parameters)
{
    const double limits[][3] = {
        { parameters[TA_HIGH_HOOK_SHARP_RATIO], parameters[TA_HIGH_HOOK_SHARP_LOW],
          parameters[TA_HIGH_HOOK_SHARP_INTRO] },
        { parameters[TA_HIGH_HOOK_MEDIUM_RATIO], parameters[TA_HIGH_HOOK_MEDIUM_LOW],
          parameters[TA_HIGH_HOOK_MEDIUM_INTRO] },
        { parameters[TA_HIGH_HOOK_GENTLE_RATIO], parameters[TA_HIGH_HOOK_GENTLE_LOW],
          parameters[TA_HIGH_HOOK_GENTLE_INTRO] },
    };
    for (unsigned i = 0; i < sizeof limits / sizeof *limits; i++)
        if (ratio > limits[i][0] && low > limits[i][1] && intro > limits[i][2]) return true;
    return false;
}
static bool high_supported_hook(double ratio, double low, double support, const double *parameters)
{
    double split = parameters[TA_HIGH_HOOK_SUPPORTED_RATIO_SPLIT];
    return low > parameters[TA_HIGH_HOOK_SUPPORTED_LEVEL_MIN] &&
           ((ratio <= split && support > parameters[TA_HIGH_HOOK_SUPPORTED_NEAR_MIN]) ||
            (ratio > split && support > parameters[TA_HIGH_HOOK_SUPPORTED_FAR_MIN]));
}
static void high_seed_hooks(int n, I roles, D nl, D ir, D lr, D intro, D support, double scale, const double *parameters)
{
    int hook = (int)parameters[TA_HIGH_ROLE_HOOK];
    int unset = (int)parameters[TA_HIGH_ROLE_UNSET];
    roles.v[n - 1] = (int)parameters[TA_HIGH_ROLE_OUTRO];
    for (int i = 1; i < n - 1; i++) {
        if (roles.v[i - 1] == hook) {
            if (nl.v[i] > parameters[TA_HIGH_HOOK_CONTINUATION_MIN] && ir.v[i] > parameters[TA_HIGH_HOOK_CONTINUATION_MIN] && ir.v[i] < parameters[TA_HIGH_HOOK_CONTINUATION_RATIO_MAX])
                roles.v[i] = hook;
        } else {
            double ratio = lr.v[i];
            if (high_hook_rise(ratio, nl.v[i], intro.v[i], parameters))
                roles.v[i] = hook;
            if (roles.v[i] == unset && high_supported_hook(ratio, nl.v[i], support.v[i] * scale, parameters))
                roles.v[i] = hook;
        }
    }
}
static double high_hook_peak(I roles, D levels, const double *parameters)
{
    double peak = 0;
    for (int i = 0; i < roles.n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_HOOK]) peak = ta_dmax(peak, levels.v[i]);
    return reciprocal(peak, parameters);
}
static void high_remove_weak_hooks(int n, I roles, D low, D intro, D support, const double *parameters)
{
    double hookLow = high_hook_peak(roles, low, parameters), hookIntro = high_hook_peak(roles, intro, parameters);
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_HOOK] &&
            (low.v[i] * hookLow < parameters[TA_HIGH_HOOK_WEAK_LEVEL] || intro.v[i] * hookIntro < parameters[TA_HIGH_HOOK_WEAK_LEVEL]) &&
            support.v[i] < parameters[TA_HIGH_HOOK_WEAK_SUPPORT])
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_UNSET];
}
static void high_remove_repeated_hooks(int n, I roles, D ir, const double *parameters)
{
    int hook = (int)parameters[TA_HIGH_ROLE_HOOK];
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == hook && roles.v[i + 1] == hook && ir.v[i + 1] > parameters[TA_HIGH_HOOK_REPEAT_RATIO])
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_UNSET];
}
static int high_hook_count(I roles, const double *parameters)
{
    int hooks = 0;
    int hook = (int)parameters[TA_HIGH_ROLE_HOOK];
    for (int i = 1; i < roles.n - 1; i++) hooks += roles.v[i] == hook;
    return hooks;
}
static void high_fallback_hooks(ta_context *c, int n, I roles, D lr, D nl, const double *parameters)
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
            if (nl.v[i] > parameters[TA_HIGH_HOOK_FALLBACK_LOW_MIN]) {
                roles.v[i] = (int)parameters[TA_HIGH_ROLE_HOOK];
                if (selected > parameters[TA_HIGH_HOOK_FALLBACK_LAST_COUNTER])
                    break;
                selected++;
            }
            rank++;
        }
}
static void high_before_hooks(int n, I roles, const double *parameters)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_UNSET] && roles.v[i + 1] == parameters[TA_HIGH_ROLE_HOOK])
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_BUILD];
}
static void high_after_hooks(int n, I roles, const double *parameters)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_UNSET] && roles.v[i - 1] == parameters[TA_HIGH_ROLE_HOOK])
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_BREAK];
}
static void high_extend_intro(int n, I roles, D ni, D ir, const double *parameters)
{
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_UNSET] && roles.v[i - 1] == parameters[TA_HIGH_ROLE_INTRO] &&
            ni.v[i] < parameters[TA_HIGH_INTRO_EXTEND_LEVEL_MAX] &&
            ir.v[i] < parameters[TA_HIGH_INTRO_EXTEND_RATIO_MAX])
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_INTRO];
}
static void high_rising_bridge(int n, I roles, I boundaries, D ir, const double *parameters)
{
    int build = (int)parameters[TA_HIGH_ROLE_BUILD];
    int outro = (int)parameters[TA_HIGH_ROLE_OUTRO];
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_UNSET] &&
            ((ir.v[i] > parameters[TA_HIGH_RISING_BRIDGE_RATIO] && roles.v[i + 1] != outro) ||
             (boundaries.v[i + 1] - boundaries.v[i] <= parameters[TA_HIGH_RISING_BRIDGE_SPAN_MAX] &&
              roles.v[i - 1] == build && roles.v[i + 1] == build)))
            roles.v[i] = build;
}
static void high_falling_bridge(int n, I roles, D ir, const double *parameters)
{
    int unset = (int)parameters[TA_HIGH_ROLE_UNSET];
    int break_role = (int)parameters[TA_HIGH_ROLE_BREAK];
    for (int i = 1; i < n - 1; i++)
        if (roles.v[i] == unset && (roles.v[i + 1] == parameters[TA_HIGH_ROLE_OUTRO] || ir.v[i] < parameters[TA_HIGH_FALLING_BRIDGE_RATIO]))
            roles.v[i] = break_role;
    int final = n - (int)parameters[TA_HIGH_FALLING_BRIDGE_FINAL_BACK];
    if (roles.v[final] == unset)
        roles.v[final] = break_role;
}
static int high_after_peak_role(I roles, int i, const double *parameters)
{
    int break_role = (int)parameters[TA_HIGH_ROLE_BREAK];
    return (roles.v[i + 1] == parameters[TA_HIGH_ROLE_OUTRO] || roles.v[i + 1] == break_role)
               ? break_role : (int)parameters[TA_HIGH_ROLE_BUILD];
}
static void high_promote_bridge(I roles, D nl, D ni, int i, int hooks, const double *parameters)
{
    int build = (int)parameters[TA_HIGH_ROLE_BUILD];
    if (roles.v[i - 1] != build || !(nl.v[i] > parameters[TA_HIGH_PROMOTE_BRIDGE_LOW_MIN]) ||
        !(ni.v[i] > parameters[TA_HIGH_PROMOTE_BRIDGE_INTRO_MIN]))
        return;
    if (hooks < parameters[TA_HIGH_PROMOTE_BRIDGE_HOOK_SPLIT])
        roles.v[i] = (int)parameters[TA_HIGH_ROLE_HOOK];
    else if (high_after_peak_role(roles, i, parameters) == build)
        roles.v[i] = build;
}
static void high_fill_unknown(I roles, D nl, D ni, int i, int hooks, const double *parameters)
{
    int break_role = (int)parameters[TA_HIGH_ROLE_BREAK];
    if (nl.v[i] > parameters[TA_HIGH_PROMOTE_BRIDGE_LOW_MIN] &&
        ni.v[i] > parameters[TA_HIGH_PROMOTE_BRIDGE_INTRO_MIN])
        roles.v[i] = hooks < parameters[TA_HIGH_PROMOTE_BRIDGE_HOOK_SPLIT]
                         ? (int)parameters[TA_HIGH_ROLE_HOOK] : high_after_peak_role(roles, i, parameters);
    else if (roles.v[i - 1] == parameters[TA_HIGH_ROLE_INTRO])
        roles.v[i] = (int)parameters[TA_HIGH_ROLE_BUILD];
    else
        roles.v[i] = high_after_peak_role(roles, i, parameters) == break_role ? break_role : roles.v[i - 1];
}
static void high_complete_roles(int n, I roles, D nl, D ni, const double *parameters)
{
    int hooks = high_hook_count(roles, parameters);
    for (int i = 1; i < n - 1; i++) {
        if (roles.v[i] == parameters[TA_HIGH_ROLE_BREAK]) high_promote_bridge(roles, nl, ni, i, hooks, parameters);
        else if (roles.v[i] == parameters[TA_HIGH_ROLE_UNSET]) high_fill_unknown(roles, nl, ni, i, hooks, parameters);
    }
}
static void high_restore_energy(int n, I roles, D nl, D lr, const double *parameters)
{
    double limit = parameters[TA_HIGH_RESTORE_ENERGY_MIN];
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == parameters[TA_HIGH_ROLE_BREAK] && roles.v[i + 1] != parameters[TA_HIGH_ROLE_OUTRO] &&
            nl.v[i] > limit && lr.v[i] > limit)
            roles.v[i] = (int)parameters[TA_HIGH_ROLE_BUILD];
}
HighRoles high_classify(ta_context *c, I boundaries, D introMeans, D lowMeans, const double *parameters)
{
    int n = boundaries.n;
    if (n < 1 || introMeans.n != n || lowMeans.n != n)
        ta_fail(c, TA_FAILED, "invalid high-role dimensions");
    I roles = ta_new_ints(c, n);
    bool short_simple = parameters[TA_HIGH_SHORT_SIMPLE_FLAG] != 0;
    if (n == parameters[TA_HIGH_SHORT_SINGLE_COUNT]) {
        roles.v[0] = (int)parameters[TA_HIGH_SINGLE_SECTION_INTRO];
        return (HighRoles){ roles, short_simple };
    }
    if (n == parameters[TA_HIGH_SHORT_PAIR_COUNT]) {
        roles.v[0] = (int)parameters[TA_HIGH_TWO_SECTION_INTRO];
        roles.v[1] = (int)parameters[TA_HIGH_TWO_SECTION_OUTRO];
        return (HighRoles){ roles, short_simple };
    }
    roles.v[0] = (int)parameters[TA_HIGH_ROLE_INTRO];
    D ni = normalized(c, introMeans, parameters), nl = normalized(c, lowMeans, parameters), intro = d_copy(c, introMeans),
      low = d_copy(c, lowMeans);
    D ir = ta_new_doubles(c, n), lr = ta_new_doubles(c, n), support = ta_new_doubles(c, n);
    high_intro_ratios(n, intro, ir, parameters);
    bool simple = high_simple_turns(high_turns(c, n, boundaries, ni, parameters), parameters);
    high_low_ratios(n, low, lr, parameters);
    double scale = high_support(c, n, intro, nl, support, parameters);
    high_seed_hooks(n, roles, nl, ir, lr, intro, support, scale, parameters);
    high_remove_weak_hooks(n, roles, low, intro, support, parameters);
    high_remove_repeated_hooks(n, roles, ir, parameters);
    if (!high_hook_count(roles, parameters)) high_fallback_hooks(c, n, roles, lr, nl, parameters);
    high_before_hooks(n, roles, parameters);
    high_after_hooks(n, roles, parameters);
    high_extend_intro(n, roles, ni, ir, parameters);
    high_rising_bridge(n, roles, boundaries, ir, parameters);
    high_falling_bridge(n, roles, ir, parameters);
    high_complete_roles(n, roles, nl, ni, parameters);
    high_restore_energy(n, roles, nl, lr, parameters);
    return (HighRoles){ roles, simple };
}
static int block_tail_trim(int span, const double *parameters)
{
    double scale = parameters[TA_BLOCK_TAIL_TRIM_SCALE];
    double bias = parameters[TA_BLOCK_TAIL_TRIM_ROUND_BIAS];
    int cap = (int)parameters[TA_BLOCK_TAIL_TRIM_CAP];
    int groups = ta_min(cap, (int)fma(span, scale, bias));
    return groups * (int)parameters[TA_BLOCK_TAIL_TRIM_GROUP_BEATS];
}

BlockMeans block_means(ta_context *c, I boundaries, D intro, D low, D quarterPower, int phase,
                       const double *parameters)
{
    int count = ta_min(intro.n, low.n);
    int origin = (int)parameters[TA_BLOCK_MEAN_PHASE_ORIGIN];
    int width = (int)parameters[TA_BLOCK_MEAN_GROUP_BEATS];
    double scale = parameters[TA_BLOCK_MEAN_SCALE];
    I limits = ta_new_ints(c, boundaries.n + 1);
    limits.n = 0;
    for (int i = 0; i < boundaries.n; i++)
        if (boundaries.v[i] <= count)
            limits.v[limits.n++] = boundaries.v[i];
    limits.v[limits.n++] = count + (int)parameters[TA_BLOCK_MEAN_TERMINAL_ADVANCE];
    BlockMeans r = { ta_new_doubles(c, boundaries.n), ta_new_doubles(c, boundaries.n),
                     ta_new_doubles(c, quarterPower.n / width) };
    for (int i = 0; i < limits.n - 1; i++) {
        int start = limits.v[i] - phase + origin;
        if (i == 0) start = ta_max(origin, start);
        int end = limits.v[i + 1] - phase + origin;
        int trim = block_tail_trim(end - start, parameters);
        D a = d_view(c, intro, start - origin, end - origin - trim),
          b = d_view(c, low, limits.v[i] - origin, limits.v[i + 1] - origin - trim);
        r.intro.v[i] = a.n ? d_sequential(a) / a.n : 0;
        r.low.v[i] = b.n ? d_sequential(b) / b.n : 0;
    }
    for (int i = 0; i < r.quarterPower.n; i++)
        r.quarterPower.v[i] = d_sequential(d_view(c, quarterPower, i * width, (i + 1) * width)) * scale;
    return r;
}
static void flatten(D v, double relative, double absolute)
{
    for (int i = 1; i < v.n; i++) {
        double d = fabs(v.v[i - 1] - v.v[i]);
        if (d < v.v[i - 1] * relative || d < absolute)
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
static D route_inverted(ta_context *c, D low, int phase, const double *parameters)
{
    int width = (int)parameters[TA_ROUTE_INVERT_GROUP_BEATS];
    int n = low.n / width;
    int start = phase - (int)parameters[TA_ROUTE_INVERT_PHASE_ORIGIN];
    int tail = (int)parameters[TA_ROUTE_INVERT_TAIL_GROUPS];
    double scale = parameters[TA_ROUTE_INVERT_MEAN_SCALE];
    D bars = ta_new_doubles(c, n);
    if (low.n >= parameters[TA_ROUTE_INVERT_MIN_MEAN_SAMPLES]) {
        for (int i = 0; i < n - tail; i++) {
            int from = start + i * width;
            bars.v[i] = d_sequential(d_view(c, low, from, from + width)) * scale;
        }
    }
    flatten(bars, parameters[TA_ROUTE_PRE_FILTER_FLATTEN_RELATIVE],
             parameters[TA_ROUTE_PRE_FILTER_FLATTEN_ABSOLUTE]);
    D inverted = ta_new_doubles(c, n);
    double base = parameters[TA_ROUTE_INVERT_BASE];
    for (int i = 0; i < n; i++)
        inverted.v[i] = base - bars.v[i];
    return inverted;
}
static D route_filtered(ta_context *c, D inverted, const double *parameters)
{
    int n = inverted.n;
    D filtered = d_copy(c, inverted);
    Peaks peaks = spectral_peaks(c, inverted, parameters);
    int left = 0, right = 0;
    int last_back = (int)parameters[TA_ROUTE_INTERIOR_PEAK_LAST_BACK];
    for (int i = peaks.n - last_back; i >= 1; i--) {
        if (peaks.v[i].width >= parameters[TA_ROUTE_INTERIOR_PEAK_WIDTH_MIN]) {
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
    double base = parameters[TA_ROUTE_RESTORE_BASE];
    for (int i = 0; i < n; i++)
        filtered.v[i] = base - filtered.v[i];
    flatten(filtered, parameters[TA_ROUTE_POST_FILTER_FLATTEN_RELATIVE],
             parameters[TA_ROUTE_POST_FILTER_FLATTEN_ABSOLUTE]);
    return filtered;
}
static D route_changes(ta_context *c, D filtered, const double *parameters)
{
    int n = filtered.n;
    D changes = ta_new_doubles(c, n);
    for (int i = 0; i < n - 1; i++)
        changes.v[i] = filtered.v[i + 1] - filtered.v[i];
    if (n >= parameters[TA_ROUTE_HISTOGRAM_TRIM_MIN])
        changes = d_view(c, changes, (int)parameters[TA_ROUTE_HISTOGRAM_HEAD_LAST] + 1,
                         n - (int)parameters[TA_ROUTE_HISTOGRAM_TAIL]);
    I order = rank_d(c, changes, true);
    D sorted = ta_new_doubles(c, changes.n);
    for (int i = 0; i < changes.n; i++)
        sorted.v[i] = changes.v[order.v[i]];
    return sorted;
}
static void route_histograms(D sorted, D positive, D negative, const double *parameters)
{
    int n = sorted.n;
    int half = n / (int)parameters[TA_ROUTE_HISTOGRAM_DIVISOR];
    for (int i = 0; i < half; i++) {
        positive.v[half - 1 - i] = sorted.v[i];
        negative.v[half - 1 - i] = -sorted.v[n - 1 - i];
    }
    for (int i = 0; i < half - 1; i++) {
        positive.v[half + i] = sorted.v[i];
        negative.v[half + i] = -sorted.v[n - 1 - i];
    }
}
static int route_loop_count(I types, double kind)
{
    int count = 0;
    for (int i = 0; i < types.n; i++) count += types.v[i] == kind;
    return count;
}
static int route_rhythm_count(I types, const double *parameters)
{
    int count = 0;
    for (int i = 1; i < types.n; i++)
        count += types.v[i] == parameters[TA_ROUTE_RHYTHM_PRIMARY_KIND] ||
                 (types.v[i] == parameters[TA_ROUTE_RHYTHM_SECONDARY_KIND] &&
                  types.v[i - 1] != parameters[TA_ROUTE_RHYTHM_PRIOR_EXCLUSION_KIND]);
    return count;
}
static bool route_high(double score, double duration, bool long_track, I loop_types, I rhythm_types,
                       const double *parameters)
{
    double strong = long_track ? duration * parameters[TA_ROUTE_LONG_STRONG_SCALE]
                               : parameters[TA_ROUTE_SHORT_STRONG_SCORE];
    double loop_limit = long_track ? trunc(duration * parameters[TA_ROUTE_LONG_LOOP_SCALE])
                                   : parameters[TA_ROUTE_SHORT_LOOP_MIN];
    double rhythm_limit = long_track ? trunc(duration * parameters[TA_ROUTE_LONG_RHYTHM_SCALE])
                                     : parameters[TA_ROUTE_SHORT_RHYTHM_MIN];
    return score >= strong ||
           route_loop_count(loop_types, parameters[TA_ROUTE_LOOP_EVIDENCE_KIND]) >= loop_limit ||
           route_rhythm_count(rhythm_types, parameters) >= rhythm_limit;
}
Route phrase_route(ta_context *c, D low, int phase, double duration, I loopTypes, I rhythmTypes,
                   int mood, const double *parameters)
{
    if (phase < parameters[TA_ROUTE_INVERT_PHASE_ORIGIN] || phase > parameters[TA_ROUTE_INVERT_GROUP_BEATS] ||
        low.n < parameters[TA_ROUTE_INVERT_MIN_MEAN_SAMPLES] || duration < 0)
        ta_fail(c, TA_FAILED, "invalid route dimensions");
    D inverted = route_inverted(c, low, phase, parameters);
    D filtered = route_filtered(c, inverted, parameters);
    D sorted = route_changes(c, filtered, parameters);
    D positive = ta_new_doubles(c, sorted.n), negative = ta_new_doubles(c, sorted.n);
    route_histograms(sorted, positive, negative, parameters);
    Peaks pp = spectral_peaks(c, positive, parameters), qq = spectral_peaks(c, negative, parameters);
    Peak p = pp.v[0], q = qq.v[0];
    double score = fma(q.height, q.width, p.width * p.height);
    bool long_track = duration > parameters[TA_ROUTE_LONG_DURATION];
    double weak = long_track ? duration * parameters[TA_ROUTE_LONG_WEAK_SCALE]
                             : parameters[TA_ROUTE_SHORT_WEAK_SCORE];
    bool high = route_high(score, duration, long_track, loopTypes, rhythmTypes, parameters);
    int route = high || score >= weak ? (int)parameters[TA_HIGH_CANDIDATE_ROUTE]
                                     : (int)parameters[TA_OTHER_CANDIDATE_ROUTE];
    return (Route){ high ? MOOD_HIGH : mood, route, score, positive, negative };
}

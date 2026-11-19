/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double safe(double x) { return x == 0 ? 0.003 : x; }
static double sum(D a, int low, int high)
{
    if (low > high || low >= a.n || high < 0)
        return 0;
    int start = ta_max(0, low), end = ta_min(a.n - 1, high);
    return d_sum((D){ a.v + start, end - start + 1 });
}
static D normalized(ta_context *c, D input, bool guard)
{
    double maximum = d_max(input), factor = guard && maximum == 0 ? 333.3333333333333 : 1 / maximum;
    D result = d_copy(c, input);
    for (int i = 0; i < input.n - 1; i++)
        result.v[i] *= factor == 0 ? 0.003 : factor;
    return result;
}
static D ratios(ta_context *c, D values)
{
    D out = ta_new_doubles(c, 28);
    for (int i = 1; i < 28; i++)
        out.v[i] = safe(values.v[i]) / safe(values.v[i - 1]);
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
    double shortDeviation[26], longDeviation[26], priorDeviation[26], contrast[26];
} EnvelopeDeviation;
static D envelope_levels(ta_context *c, D envelope, int phase)
{
    int n = (envelope.n - phase + 1) / 4;
    D level = ta_new_doubles(c, n);
    for (int i = 0; i < n - 1; i++)
        level.v[i] = sum(envelope, phase - 1 + i * 4, phase + 2 + i * 4) * 0.25;
    double factor = 1 / d_max(level);
    for (int i = 0; i < n - 1; i++)
        level.v[i] *= factor == 0 ? 0.003 : factor;
    return level;
}
static I envelope_changes(ta_context *c, D level)
{
    double changes[25] = { 0 };
    I candidates = ta_new_ints(c, 25);
    candidates.n = 0;
    for (int i = 1; i < 25; i++) {
        if (level.v[i - 1] == 0) level.v[i - 1] = 0.003;
        changes[i] = level.v[i] / level.v[i - 1];
    }
    for (int i = 0; i < 25; i++)
        if (changes[i] >= 2 && level.v[i] > 0.095) candidates.v[candidates.n++] = i + 1;
    return candidates;
}
static void envelope_stable_levels(D level, I candidates, I *accepted)
{
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        double base = safe(level.v[i - 1]), a = safe(sum(level, i, i + 1) * 0.5) / base,
               b = safe(sum(level, i, i + 2) / 3) / base;
        if (ta_dmax(a, 1 / a) <= 1.2 && ta_dmax(b, 1 / b) <= 1.3)
            accepted->v[accepted->n++] = i;
    }
}
static bool envelope_new_candidate(I accepted, int i)
{
    return !accepted.n || accepted.v[accepted.n - 1] != i;
}
static void envelope_deviations(D level, int i, double shortValues[3], double longValues[7], EnvelopeDeviation *d)
{
    int n = level.n;
    double sm = sum(level, i, i + 2) / 3,
           lm = sum(level, i, ta_min(i + 6, n - 1)) / (double)ta_min(7, ta_max(1, n - i));
    for (int j = 0; j < 7; j++)
        if (i + j + 1 < n) {
            longValues[j] = fabs(level.v[i + j] - lm);
            if (j < 3) shortValues[j] = fabs(level.v[i + j] - sm);
        }
    d->shortDeviation[i] = d_sum((D){ shortValues, 3 }) / 3;
    d->longDeviation[i] = d_sum((D){ longValues, 7 }) / 7;
}
static void envelope_stable_deviations(D level, I candidates, I *accepted, EnvelopeDeviation *d)
{
    double shortValues[3] = { 0 }, longValues[7] = { 0 };
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        envelope_deviations(level, i, shortValues, longValues, d);
        if (d->shortDeviation[i] <= 0.1 && d->longDeviation[i] <= 0.13 && envelope_new_candidate(*accepted, i))
            accepted->v[accepted->n++] = i;
    }
}
static bool envelope_prior_stable(const EnvelopeDeviation *d, int i)
{
    return d->shortDeviation[i] <= 0.14 && d->longDeviation[i] <= 0.16 &&
           d->priorDeviation[i] <= 0.04 && d->contrast[i] >= 3;
}
static void envelope_prior_deviations(ta_context *c, D level, I candidates, I *accepted, EnvelopeDeviation *d)
{
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k], length = ta_min(4, i - 1);
        D prior = d_view(c, level, i - 1 - length, i - 1);
        double mean = safe(d_sum(prior) / (double)prior.n), deviations[4];
        for (int j = 0; j < prior.n; j++) deviations[j] = fabs(prior.v[j] - mean);
        d->priorDeviation[i] = d_sum((D){ deviations, prior.n }) / (double)prior.n;
        d->contrast[i] = level.v[i - 1] / mean;
        if (envelope_prior_stable(d, i) && envelope_new_candidate(*accepted, i)) accepted->v[accepted->n++] = i;
    }
}
static void envelope_relaxed_deviations(I candidates, I *accepted, const EnvelopeDeviation *d)
{
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        if (d->shortDeviation[i] <= 0.18 && d->longDeviation[i] <= 0.3 && d->priorDeviation[i] <= 0.01 &&
            d->contrast[i] >= 3 && envelope_new_candidate(*accepted, i)) accepted->v[accepted->n++] = i;
    }
}
static int envelope_candidate(ta_context *c, D envelope, int phase)
{
    D level = envelope_levels(c, envelope, phase);
    I candidates = envelope_changes(c, level), accepted = ta_new_ints(c, 100);
    accepted.n = 0;
    EnvelopeDeviation d = { 0 };
    envelope_stable_levels(level, candidates, &accepted);
    if (accepted.n < candidates.n) envelope_stable_deviations(level, candidates, &accepted, &d);
    if (accepted.n < candidates.n) envelope_prior_deviations(c, level, candidates, &accepted, &d);
    if (accepted.n < candidates.n) envelope_relaxed_deviations(candidates, &accepted, &d);
    I unique = i_compact(c, accepted, true);
    int result = !unique.n ? 0 : unique.n > 1 && unique.v[1] == unique.v[0] + 1 ? unique.v[1]
                                                                                : unique.v[0];
    ta_release(c, level.v);
    ta_release(c, candidates.v);
    ta_release(c, accepted.v);
    ta_release(c, unique.v);
    return result;
}
static int positive(D a, int start, int end)
{
    int count = 0;
    for (int i = start; i < end; i++)
        if (a.v[i] > 0)
            count++;
    return count;
}
static int bar(int beat) { return (int)round((double)(beat - 1) * 0.25 + 1); }
static bool vocal_dense_start(D v, int i)
{
    if (!(v.v[i] >= 0.5 && (!(i % 4) || v.v[i + 1] + v.v[i + 2] + v.v[i + 3] != 0))) return false;
    int count = positive(v, i, i + 8);
    double mean = sum(v, i, i + 7) / (double)count;
    return (count >= 5 && mean >= 0.4) || (count == 4 && mean >= 0.5);
}
static bool vocal_sustained_onset(D v, int i)
{
    int count = positive(v, i, i + 16);
    double mean = sum(v, i, i + 15) / 16;
    return ((count > 7 && mean > 0.1) || (count > 5 && mean > 0.2)) &&
           (v.v[i] * v.v[i + 2] * v.v[i + 3] > 0 || v.v[i] * v.v[i + 1] > 0) &&
           sum(v, 4, i - 1) / (double)(i - 4) < 0.05;
}
static int vocal_first_onset(D v)
{
    for (int i = 5; i < 97; i++) {
        if (i > 34 && sum(v, 0, i - 1) != 0) break;
        if (!(v.v[i] > 0)) continue;
        if (vocal_sustained_onset(v, i)) return i + 1;
    }
    return 0;
}
static bool vocal_adjacent_bars(D v, int i)
{
    bool adjacent = false;
    int previous = 0;
    for (int j = 0; j < i / 4; j++) {
        int count = positive(v, j * 4, j * 4 + 4);
        if (j && previous >= 3 && count >= 3) adjacent = true;
        previous = count;
    }
    return adjacent;
}
static bool vocal_dense_bars(D v, int i)
{
    bool denseNext = true;
    for (int j = 0; j < 3; j++)
        if (positive(v, i + j * 4, i + j * 4 + 4) <= 2) denseNext = false;
    double mean = sum(v, i, i + 11) / 12;
    return denseNext && mean > 0.4 && sum(v, 0, i - 1) / (double)i < mean * 0.5;
}
static int vocal_bar_onset(D v)
{
    for (int i = 8; i < 34; i++) {
        if (vocal_adjacent_bars(v, i)) break;
        if (!(v.v[i] > 0)) continue;
        if (vocal_dense_bars(v, i)) return i + 1;
    }
    return 0;
}
static bool vocal_high_mean(D v, int i, int count)
{
    return sum(v, i, i + 7) / (double)count >= 0.65 ||
           (v.v[i] > 0.7 && v.v[i + 1] > 0.3 && v.v[i + 2] > 0.3);
}
static int vocal_sparse_onset(D v)
{
    for (int i = 7; i < 66; i++)
        if (v.v[i] > 0) {
            int count = positive(v, i, i + 8);
            if (count > 2 && sum(v, i, i + 23) / 24 >= 0.1 &&
                sum(v, 0, i - 1) / (double)i < 0.04 && vocal_high_mean(v, i, count)) return i + 1;
        }
    return 0;
}
static bool vocal_neighbor_bars(int result, int denseBar)
{
    return (result == 5 && denseBar == 4) ||
           (result == 9 && denseBar == 8);
}
static bool vocal_rounds_up(int result, I dense)
{
    return (result == 4 || result == 8) && dense.n > 1 && bar(dense.v[1]) == result + 1;
}
static bool vocal_prefer_dense(int result, int denseBar)
{
    bool preceding = vocal_neighbor_bars(result, denseBar);
    bool next = vocal_neighbor_bars(denseBar, result);
    return next || (!(denseBar == 1 || preceding) && !(result < denseBar && result != 1));
}
static int vocal_snap_onset(int result, int onset, I dense)
{
    if (result <= 1) return result;
    bool nearFive = onset >= 14 && onset <= 19, nearNine = onset >= 30 && onset <= 35;
    if (nearFive) result = 5;
    if (nearNine) result = 9;
    if (!nearFive && !nearNine && vocal_rounds_up(result, dense)) result++;
    return result;
}
static int vocal_candidate(ta_context *c, D vocal)
{
    double maximum = d_max(vocal), scale = maximum == 0 ? 333.3333333333333 : 1 / maximum;
    double values[112];
    D v = { values, 112 };
    for (int i = 0; i < 112; i++) values[i] = vocal.v[i] * scale;
    int denseValues[96];
    I dense = { denseValues, 0 };
    for (int i = 0; i < 96; i++)
        if (vocal_dense_start(v, i)) dense.v[dense.n++] = i + 1;
    int onset = vocal_first_onset(v);
    if (!onset) onset = vocal_bar_onset(v);
    if (!onset) onset = vocal_sparse_onset(v);
    int result = bar(onset), first = dense.n ? dense.v[0] : 0, denseBar = bar(first);
    if (vocal_prefer_dense(result, denseBar)) {
        result = denseBar;
        onset = first;
    }
    (void)c;
    return vocal_snap_onset(result, onset, dense);
}
static I level_candidates(ta_context *ctx, D sym)
{
    I ranked = rank_d(ctx, (D){ sym.v, 17 }, true);
    int n = 0;
    for (int i = 0; i < 17; i++) {
        int value = ranked.v[i] + 1;
        if (i == 16 || value > 3 || sym.v[value - 1] >= 1.35)
            ranked.v[n++] = value;
    }
    ranked.n = n;
    I candidates = i_copy(ctx, (I){ ranked.v, ta_min(n, 5) });
    if (candidates.v[1] - candidates.v[0] == 1)
        i_remove(&candidates, 1);
    else if (candidates.v[0] - candidates.v[1] == 1)
        i_remove(&candidates, 0);
    if (candidates.v[2] - candidates.v[1] == 1)
        i_remove(&candidates, 2);
    else if (candidates.v[1] - candidates.v[0] == 1)
        i_remove(&candidates, 1);
    candidates.n = ta_min(candidates.n, 3);
    I ordered = i_sorted(ctx, candidates, false);
    ta_release(ctx, candidates.v);
    ta_release(ctx, ranked.v);
    return ordered;
}
static bool level_prefer_first(D sym, int a, int b)
{
    return !(a > 2 && b - a == 2 && (safe(sym.v[b - 1]) / safe(sym.v[a - 1]) > 2 || sym.v[b] + sym.v[b - 2] + sym.v[b + 1] < sym.v[a] + sym.v[a - 2] + sym.v[a + 1]));
}
static bool level_prefer_second(D sym, int b, int c)
{
    return !(c - b == 2 && (sym.v[c - 1] / safe(sym.v[b - 1]) > 2 ||
                            sym.v[b] + sym.v[b - 2] + sym.v[b + 1] > sym.v[c] + sym.v[c - 2] + sym.v[c + 1]));
}
static void level_order_candidates(I candidates, D sym)
{
    int a = candidates.v[0], b = candidates.v[1], c = candidates.v[2];
    bool first = level_prefer_first(sym, a, b), second = level_prefer_second(sym, b, c);
    if (!first && !second) {
        candidates.v[0] = c;
        candidates.v[2] = a;
    } else if (!first) {
        candidates.v[0] = b;
        candidates.v[1] = a;
    } else if (!second) {
        candidates.v[1] = c;
        candidates.v[2] = b;
    }
}
static double level_deviation(D values, double mean)
{
    double dev[28];
    for (int i = 0; i < values.n; i++) dev[i] = fabs(mean - values.v[i]);
    return d_sum((D){ dev, values.n }) / (double)values.n;
}
static bool level_deviation_changed(double current, double mean, double deviation)
{
    return deviation >= 0.005
               ? current / (mean + deviation) > 1.15 || current / (mean - deviation) < 0.8695652173913044
               : fabs(mean - current) / deviation > 15;
}
static bool level_second_accepted(D ratios, D sym)
{
    return ratios.v[1] > 1 && (sym.v[2] + sym.v[3]) * 0.5 < 1.4 && sym.v[1] > 1.5;
}
static bool level_changed(ta_context *ctx, D values, D ratios, D sym, int candidate, bool low)
{
    int start = ratios.v[1] >= 0.85 ? 0 : 1;
    D preceding = d_view(ctx, values, start, candidate - 1);
    double mean = d_sum(preceding) / (double)preceding.n;
    double deviation = level_deviation(preceding, mean);
    if (low) deviation = safe(deviation);
    double current = values.v[candidate - 1];
    bool changed = level_deviation_changed(current, mean, deviation);
    if (!changed && candidate > 3 && sym.v[candidate - 1] > 1.3) {
        int count = candidate < 10 ? 2 : 4;
        D prior = d_view(ctx, values, candidate - 1 - count, candidate - 1);
        double m = d_sum(prior) / (double)count;
        double d = level_deviation(prior, m);
        changed = current / (m + d) > 1.2 || current / (m - d) < 0.8333333333333334;
    }
    return changed;
}
static bool level_settled(D values, D sym, int candidate)
{
    if (values.v[candidate + 2] == 0) values.v[candidate + 2] = 0.003;
    double a = (sym.v[candidate] + sym.v[candidate + 1]) * 0.5,
           b = (sym.v[candidate + 1] + sym.v[candidate + 2]) * 0.5,
           ratio = values.v[candidate] / values.v[candidate + 2];
    return a < 1.4 || b < 1.4 || a < sym.v[candidate - 1] * 0.6 ||
           b < sym.v[candidate - 1] * 0.6 || ta_dmax(ratio, 1 / ratio) < 1.1;
}
static bool level_candidate_accepted(ta_context *ctx, D values, D ratios, D sym, int candidate, bool low)
{
    if (candidate == 2) return level_second_accepted(ratios, sym);
    if (candidate <= 2) return false;
    bool changed = level_changed(ctx, values, ratios, sym, candidate, low);
    if (changed && candidate + 3 <= 27) return level_settled(values, sym, candidate);
    if (candidate > 3) {
        int count = low ? 3 : 2;
        double m = safe(sum(values, candidate - 1 - count, candidate - 2) / (double)count),
               ratio = values.v[candidate - 1] / m;
        return !(ratio <= 1.4 && ratio >= 0.7);
    }
    return false;
}
static int level_refine_candidate(D values, D ratios, int candidate)
{
    if (values.v[candidate + 1] == 0) values.v[candidate + 1] = 0.003;
    int result = candidate;
    if (candidate < 3 && ratios.v[candidate - 1] <= 1.6) return result;
    if (ratios.v[candidate - 1] <= 1) result += ratios.v[candidate] * ratios.v[candidate - 1] > 0.83;
    else if (ratios.v[candidate] > 1.15 || ratios.v[candidate] < 0.6) {
        double ratio = values.v[candidate - 1] / values.v[candidate + 1];
        result += ratio > 1.05 || ratio < 0.95;
    }
    return result;
}
static int level_candidate(ta_context *ctx, D values, D ratios, D sym, bool low)
{
    I candidates = level_candidates(ctx, sym);
    if (!low) level_order_candidates(candidates, sym);
    int result = 0;
    for (int k = 0; k < candidates.n; k++) {
        int candidate = candidates.v[k];
        if (level_candidate_accepted(ctx, values, ratios, sym, candidate, low)) {
            result = level_refine_candidate(values, ratios, candidate);
            break;
        }
    }
    ta_release(ctx, candidates.v);
    return result;
}
static int opening_vote_count(I h, int value)
{
    int count = 0;
    for (int j = 0; j < 4; j++)
        if (h.v[j] == value) count++;
    return count;
}
static int opening_majority(I h)
{
    for (int i = 0; i < 4; i++)
        if (h.v[i] == 1) h.v[i] = 0;
    for (int i = 0; i <= 1; i++)
        if (h.v[i] && opening_vote_count(h, h.v[i]) >= 3) return h.v[i];
    return 0;
}
static void opening_pair_scores(I h, D scores)
{
    for (int i = 0; i < 4; i++)
        if (h.v[i] && opening_vote_count(h, h.v[i]) == 2) scores.v[i] += 1;
}
static double opening_earliest_score(I unique)
{
    return unique.n > 1 && unique.v[1] - unique.v[0] > 3 ? 0.7 : 0.4;
}
static void opening_order_scores(ta_context *c, I h, D scores)
{
    I unique = i_compact(c, h, true);
    for (int i = 0; i < 4; i++)
        if (h.v[i]) {
            int rank = 0;
            while (rank < unique.n && unique.v[rank] != h.v[i]) rank++;
            if (!rank) scores.v[i] += opening_earliest_score(unique);
            else if (rank == 1) {
                if (unique.v[1] - unique.v[0] < 4) scores.v[i] += 0.2;
            } else scores.v[i] -= 0.35;
        }
    ta_release(c, unique.v);
}
static void opening_neighbor_scores(ta_context *c, I h, D scores)
{
    double values[4];
    for (int i = 0; i < 4; i++) values[i] = h.v[i];
    I order = rank_d(c, (D){ values, 4 }, true);
    for (int i = 0; i < 3; i++)
        if (h.v[order.v[i]] - h.v[order.v[i + 1]] == 1) {
            scores.v[order.v[i]] += 0.75;
            if (order.v[i + 1] == 0) scores.v[0] += 0.86;
            else if (order.v[i] == 2 && order.v[i + 1] == 3) scores.v[2] += 0.3;
        }
    ta_release(c, order.v);
}
static double opening_bar_score(int v)
{
    return v == 2 ? 1.2 : v == 3          ? 1.3
                      : v == 5 || v == 17 ? 1.8
                      : v == 9            ? 1.85
                      : v >= 18           ? 0.44999999999999996
                                          : 1;
}
static void opening_scores(ta_context *c, I h, D scores)
{
    opening_pair_scores(h, scores);
    opening_order_scores(c, h, scores);
    opening_neighbor_scores(c, h, scores);
    for (int i = 0; i < 4; i++) {
        if (!h.v[i]) scores.v[i] = 0;
        else scores.v[i] += opening_bar_score(h.v[i]);
    }
}
static void opening_prune_low(I h, D scores, D low, D lowSym)
{
    int third = h.v[2];
    if (third > 0 && !(lowSym.v[third - 1] >= 1.3) &&
        !((third == 1 ? 0 : fabs(low.v[third - 1] - low.v[third - 2])) >= 0.09)) {
        h.v[2] = 0;
        scores.v[2] = 0;
    }
}
static bool opening_low_tie(I raw, I h, D scores, I order)
{
    int unchanged = 0;
    for (int i = 0; i < 4; i++)
        if (scores.v[i] == scores.v[order.v[i]]) unchanged++;
    return raw.v[2] > 0 && unchanged == 1 && scores.v[order.v[1]] == scores.v[2] &&
           abs(h.v[order.v[0]] - h.v[2]) < 5 && scores.v[order.v[0]] - scores.v[2] <= 1.1 && order.v[0] != 0;
}
static int opening_select_score(ta_context *c, I raw, I h, D scores, D lowRatios)
{
    I order = rank_d(c, scores, true);
    int selected = h.v[order.v[0]];
    if (opening_low_tie(raw, h, scores, order)) {
        double value = lowRatios.v[raw.v[2] - 1];
        if (!(value >= 0.32 && value <= 1.5)) selected = h.v[2];
    }
    ta_release(c, order.v);
    return selected;
}
static Opening reconcile(ta_context *c, I raw, D low, D lowRatios, D lowSym, int phase)
{
    Opening out = { .hypotheses = i_copy(c, raw), .raw = raw, .scores = ta_new_doubles(c, 4), .found = true };
    I h = out.hypotheses;
    D scores = out.scores;
    int selected = opening_majority(h);
    if (!selected) opening_scores(c, h, scores);
    opening_prune_low(h, scores, low, lowSym);
    if (!selected) selected = opening_select_score(c, raw, h, scores, lowRatios);
    for (int i = 0; i < 4; i++) h.v[i] = h.v[i] ? phase - 4 + h.v[i] * 4 : 0;
    out.selected = selected ? phase - 4 + selected * 4 : phase;
    return out;
}
static void opening_dimensions(ta_context *c, D envelope, D low, D middle, D vocal, int phase)
{
    if (phase < 1 || phase > 4 || envelope.n < 112 || low.n < 28 || middle.n < 28 || vocal.n < 112)
        ta_fail(c, TA_FAILED, "opening dimensions");
}
static void opening_quiet_low(D lv, D ls)
{
    for (int i = 1; i <= 16; i++)
        if (lv.v[i - 1] < 0.02 && lv.v[i] < 0.02 && ls.v[i] < 3) ls.v[i] = 1;
}
Opening opening_judge(ta_context *c, D envelope, D low, D middle, D vocal, int phase)
{
    opening_dimensions(c, envelope, low, middle, vocal, phase);
    I raw = ta_new_ints(c, 4);
    raw.v[0] = envelope_candidate(c, envelope, phase);
    raw.v[1] = vocal_candidate(c, vocal);
    D lv = normalized(c, low, false), lr = ratios(c, lv), ls = symmetric(c, lr);
    ls.v[0] = 0;
    opening_quiet_low(lv, ls);
    raw.v[2] = level_candidate(c, lv, lr, ls, true);
    D mv = normalized(c, middle, true), mr = ratios(c, mv), ms = symmetric(c, mr);
    ms.v[0] = 0;
    raw.v[3] = level_candidate(c, mv, mr, ms, false);
    D original = normalized(c, low, false), originalRatios = ratios(c, original),
      originalSym = symmetric(c, originalRatios);
    Opening out = reconcile(c, raw, original, lr, originalSym, phase);
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
static Opening opening_padded(ta_context *c, D envelope, D low, D middle, D vocal, int phase, int n, Opening out)
{
    D e = envelope, l = low, m = middle, v = vocal;
    if (n < 28) {
        int times = (n + 27) / n + 2;
        e = repeat(c, envelope, n * 4, times);
        l = repeat(c, low, n, times);
        m = repeat(c, middle, n, times);
        v = repeat(c, vocal, n * 4, times);
    }
    ta_release(c, out.hypotheses.v);
    ta_release(c, out.raw.v);
    ta_release(c, out.scores.v);
    out = opening_judge(c, e, l, m, v, phase);
    if (n < 28) {
        ta_release(c, e.v);
        ta_release(c, l.v);
        ta_release(c, m.v);
        ta_release(c, v.v);
    }
    return out;
}
static Opening opening_short_result(Opening out, int n, int phase)
{
    int sum = 0;
    for (int i = 0; i < out.hypotheses.n; i++)
        sum += out.hypotheses.v[i];
    if (!sum) {
        if (n >= 28) {
            out.found = true;
            out.hypotheses.n = 1;
            out.hypotheses.v[0] = phase;
            out.selected = phase;
        } else
            out.found = false;
    } else if (n < 28) {
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
static void opening_anchor_phase(Opening *out, I anchors, I ordered, int phase)
{
    int remainder = ((anchors.v[0] - phase) / 4 + 1) % 4, same = 0;
    for (int i = 0; i < ordered.n - 1; i++) {
        int difference = ordered.v[i] - phase, bar = difference / 4 + (difference > 3 ? 1 : 0);
        if (bar % 4 == remainder) same = same ? ta_min(same, ordered.v[i]) : ordered.v[i];
    }
    if (same) {
        out->selected = same;
        out->found = true;
    }
}
Opening opening_main(ta_context *c, D envelope, D low, D middle, D vocal, int phase, I anchors)
{
    if (!anchors.n || phase < 1 || phase > 4) ta_fail(c, TA_FAILED, "opening anchors");
    int n = ta_min(ta_min(envelope.n / 4, low.n), ta_min(middle.n, vocal.n / 4));
    Opening out = { .hypotheses = ta_new_ints(c, 1), .raw = ta_new_ints(c, 0), .scores = ta_new_doubles(c, 0) };
    if (n >= 8) {
        out = opening_padded(c, envelope, low, middle, vocal, phase, n, out);
        out = opening_short_result(out, n, phase);
    }
    I ordered = i_sorted(c, out.hypotheses, true);
    ta_release(c, out.hypotheses.v);
    out.hypotheses = ordered;
    if (anchors.v[0] && !opening_anchor_match(&out, anchors, ordered)) opening_anchor_phase(&out, anchors, ordered, phase);
    return out;
}

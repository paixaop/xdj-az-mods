/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
double d_sum(D a)
{
    double lanes[8] = { 0 };
    int end = a.n & ~7;
    for (int i = 0; i < end; i++)
        lanes[i & 7] += a.v[i];
    double total = (lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))) +
                   (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])));
    for (int i = end; i < a.n; i++)
        total += a.v[i];
    return total;
}
double d_sequential(D a)
{
    double x = 0;
    for (int i = 0; i < a.n; i++)
        x += a.v[i];
    return x;
}
double d_square(D a)
{
    double x = 0;
    for (int i = 0; i < a.n; i++)
        x += a.v[i] * a.v[i];
    return x;
}
double d_max(D a)
{
    double x = a.n ? a.v[0] : 0;
    for (int i = 1; i < a.n; i++)
        if (x < a.v[i])
            x = a.v[i];
    return x;
}
double d_min(D a)
{
    double x = a.n ? a.v[0] : 0;
    for (int i = 1; i < a.n; i++)
        if (a.v[i] < x)
            x = a.v[i];
    return x;
}
D d_view(ta_context *c, D a, int start, int end)
{
    if (start < 0 || end < start || end > a.n)
        ta_fail(c, TA_FAILED, "phrase array range");
    return (D){ a.v + start, end - start };
}
D d_copy(ta_context *c, D a)
{
    D b = ta_new_doubles(c, a.n);
    if (a.n)
        memcpy(b.v, a.v, (size_t)a.n * sizeof(double));
    return b;
}
I i_copy(ta_context *c, I a)
{
    I b = ta_new_ints(c, a.n);
    if (a.n)
        memcpy(b.v, a.v, (size_t)a.n * sizeof(int));
    return b;
}
D d_normalized(ta_context *c, D a)
{
    D b = d_copy(c, a);
    double m = d_max(a), scale = m == 0 ? 333.3333333333333 : 1 / m;
    for (int i = 0; i < b.n; i++)
        b.v[i] *= scale;
    return b;
}
ta_phrase_biquad second_order(double rate, double cutoff, double damping, bool high)
{
    double warped = tan(cutoff / (rate + rate)) * (rate + rate), reciprocal = 1 / rate;
    double den2 = reciprocal * reciprocal * (warped * warped),
           den1 = (reciprocal + reciprocal) * (warped * damping);
    double scale = 1 / (den1 + 4 + den2), num0 = high ? 4 : 0,
           num2 = high ? 0 : reciprocal * reciprocal * (warped * warped);
    return (ta_phrase_biquad){ .b0 = scale * (num0 + num2),
                               .b1 = (scale + scale) * (num2 - num0),
                               .b2 = scale * (num0 + num2),
                               .a1 = (scale + scale) * (den2 - 4),
                               .a2 = scale * ((4 - den1) + den2) };
}
ta_phrase_biquad smoother(double rate, double cutoff)
{
    double warped = tan(cutoff / (rate + rate)) * (rate + rate), numerator = (1 / rate) * warped,
           scale = 1 / (numerator + 2);
    return (ta_phrase_biquad){ .b0 = scale * numerator,
                               .b1 = scale * numerator,
                               .a1 = scale * (numerator - 2),
                               .first_order = 1 };
}
D filter(ta_context *c, ta_phrase_biquad *sections, int count, D input, bool rectify)
{
    D output = ta_new_doubles(c, input.n);
    for (int start = 0; start < input.n; start += 16384) {
        ta_check(c);
        int n = ta_min(16384, input.n - start);
        const double *source = input.v + start;
        for (int i = 0; i < count; i++) {
            ta_phrase_biquad_process(sections + i, source, output.v + start, n, rectify && i == 0);
            source = output.v + start;
        }
        if (!count)
            memcpy(output.v + start, source, n * sizeof(double));
    }
    return output;
}
typedef struct {
    ta_phrase_biquad (*sections)[4];
    D **bands;
    const double *pending;
    const int *divisors;
    int filled;
} decimator_job;
static void decimate_band(ta_context *c, void *data, size_t band, unsigned worker)
{
    (void)worker;
    decimator_job *job = data;
    D *output = job->bands[band];
    ta_check(c);
    ta_phrase_decimate(job->sections[band], 4, job->pending, output->v + output->n,
                       (size_t)job->filled, (size_t)job->divisors[band]);
    output->n += job->filled / job->divisors[band];
}
static void decode_bands(ta_context *c, uint64_t frames, int block, int16_t *pcm, double *pending, ta_phrase_biquad sections[3][4], D **bands, const int *divisors)
{
    int received = 0, filled = 0;
    while (received < (int)frames) {
        ta_check(c);
        int need = ta_min(16384, ta_min(block - filled, (int)frames - received));
        int64_t n = ta_decode_read(c->decoder, pcm, need);
        if (n <= 0 || n > need)
            ta_fail(c, TA_FAILED, "truncated phrase decode");
        for (int i = 0; i < n; i++)
            pending[filled + i] = fma((double)pcm[i * 2 + 1], 3.0517578125e-07,
                                      (double)pcm[i * 2] * 3.021240234375e-05);
        filled += (int)n;
        received += (int)n;
        if (filled == block || received == (int)frames) {
            decimator_job job = { .sections = sections, .bands = bands, .pending = pending, .divisors = divisors, .filled = filled };
            ta_parallel(c, 3, ta_phrase_threads(c), decimate_band, &job);
            filled = 0;
        }
    }
}

static void initialize_bands(ta_context *c, unsigned rate, uint64_t frames, D **bands, const int *divisors, const double *damping, ta_phrase_biquad sections[3][4])
{
    for (int b = 0; b < 3; b++) {
        *bands[b] = ta_new_doubles(c, (int)(frames / divisors[b]));
        bands[b]->n = 0;
        double cutoff = ((double)rate / divisors[b]) * (b == 2 ? 0.55 : 0.4) * 6.283185307179586;
        for (int j = 0; j < 4; j++)
            sections[b][j] = second_order(rate, cutoff, damping[j], b == 2);
    }
}
Samples phrase_decode(ta_context *c)
{
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(c->request->audio_path, &rate, &frames);
    if (!c->decoder || frames < 2 || frames > (uint64_t)rate * 1200 ||
        (rate != 44100 && rate != 48000))
        ta_fail(c, TA_FAILED, "unsupported phrase audio");
    Samples s = { .rate = rate, .frames = (int)frames - 1 };
    D *bands[] = { &s.low, &s.middle, &s.high };
    const int divisors[] = { 32, 8, 2 };
    const double damping[] = { 1.9615705608064609, 1.6629392246050905, 1.1111404660392046,
                               0.39018064403225666 };
    ta_phrase_biquad sections[3][4];
    int block = (int)((double)rate * 1.7066666666666668 + 0.5);
    initialize_bands(c, rate, frames, bands, divisors, damping, sections);
    int16_t *pcm = ta_alloc(c, 16384 * 2, sizeof(int16_t));
    double *pending = ta_alloc(c, block, sizeof(double));
    decode_bands(c, frames, block, pcm, pending, sections, bands, divisors);
    if (ta_decode_read(c->decoder, pcm, 1) != 0)
        ta_fail(c, TA_FAILED, "phrase frame count changed");
    ta_decode_close(c->decoder);
    c->decoder = NULL;
    ta_release(c, pcm);
    ta_release(c, pending);
    return s;
}
static D envelope(ta_context *c, D input, double rate, double lower, double upper, double smooth,
                  const double *damping, int n)
{
    ta_phrase_biquad sections[9];
    int count = 0;
    if (lower > 0)
        for (int i = 0; i < n; i++)
            sections[count++] = second_order(rate, lower, damping[i], true);
    for (int i = 0; i < n; i++)
        sections[count++] = second_order(rate, upper, damping[i], false);
    ta_phrase_biquad last = smoother(rate, smooth);
    D out = ta_new_doubles(c, input.n);
    for (int start = 0; start < input.n; start += 16384) {
        ta_check(c);
        int size = ta_min(16384, input.n - start);
        const double *source = input.v + start;
        for (int i = 0; i < count; i++) {
            ta_phrase_biquad_process(sections + i, source, out.v + start, size, 0);
            source = out.v + start;
        }
        ta_phrase_biquad_process(&last, source, out.v + start, size, 1);
    }
    return out;
}
Envelopes phrase_envelopes(ta_context *c, Samples s)
{
    const double second[] = { 1.4142135623730951 },
                 fourth[] = { 1.8477590650225735, 0.7653668647301797 };
    Envelopes e;
    e.low = envelope(c, s.low, s.rate / 32, 0, 628.3185307179587, 31.41592653589793, second, 1);
    e.middle = envelope(c, s.middle, s.rate / 8, 2513.2741228718346, 6283.185307179586,
                        62.83185307179586, fourth, 2);
    e.high = envelope(c, s.high, s.rate / 2, 31415.926535897932, 43982.2971502571,
                      62.83185307179586, fourth, 2);
    e.lowLevel =
        envelope(c, s.low, s.rate / 32, 0, 942.4777960769379, 31.41592653589793, second, 1);
    ta_phrase_biquad section = second_order(s.rate / 8, 3141.592653589793, second[0], true);
    D tmp = filter(c, &section, 1, s.middle, false);
    section = smoother(s.rate / 8, 31.41592653589793);
    e.middleLevel = filter(c, &section, 1, tmp, true);
    ta_release(c, tmp.v);
    return e;
}
static bool valid_reference_timing(double hundred, double first, double firstBpm)
{
    return !(!isfinite(hundred) || hundred < 1 || hundred > INT32_MAX || !isfinite(first) || first < 0 ||
             first * 1000 + 0.5 > INT_MAX || !isfinite(firstBpm) || firstBpm <= 0);
}

Timing reference_timing(ta_context *c, double bpm, double first, int position, double firstBpm)
{
    double hundred = round(bpm * 100);
    if (!valid_reference_timing(hundred, first, firstBpm))
        ta_fail(c, TA_FAILED, "invalid phrase timing");
    int offset = (int)(first * 1000 + 0.5), positions[] = { 1, 4, 3, 2 },
        phase = position >= 1 && position <= 4 ? positions[position - 1] : 1;
    if (round(firstBpm * 100) != hundred) {
        double period = 6000000 / hundred;
        if (period < offset && offset < period + period) {
            offset -= (int)period;
            phase = phase == 1 ? 4 : phase - 1;
        }
    }
    return (Timing){ hundred * 0.01, offset, phase };
}
int nominal_beats(double period, int phase, double seconds)
{
    return phase - 1 +
           4 * (int)((35.11 - fma(period, (double)(phase - 1), seconds)) / (period * 4));
}
I stage_coordinates(ta_context *c, int count, double rate, double bpm, int offset)
{
    double period = (rate * 60) / bpm;
    int first = (int)fma(rate * 0.001, (double)offset, 0.5);
    I a = ta_new_ints(c, ta_max(0, (int)((double)(count - first) / period)));
    for (int i = 0; i < a.n; i++)
        a.v[i] = (int)fma(period, (double)i, (double)first + 1.5);
    if (a.n)
        a.v[a.n - 1] = ta_min(a.v[a.n - 1], count);
    return a;
}
static bool valid_coordinates(Samples s, Timing t)
{
    return !(!isfinite(s.rate) || s.rate <= 0 || !isfinite(t.bpm) || t.bpm <= 0 || t.offset < 0 ||
             t.phase < 1 || t.phase > 4);
}

Coordinates phrase_coordinates(ta_context *c, Samples s, Timing t)
{
    if (!valid_coordinates(s, t))
        ta_fail(c, TA_FAILED, "invalid phrase coordinates");
    double rates[] = { s.rate / 2, s.rate / 8, s.rate / 32 },
           counts[] = { (double)s.frames / 2, (double)s.frames / 8, (double)s.frames / 32 };
    double periods[3];
    int offsets[3], n = INT_MAX;
    for (int b = 0; b < 3; b++) {
        offsets[b] = (int)fma((double)t.offset * 0.001, rates[b], 0.5);
        periods[b] = (60 / t.bpm) * rates[b];
        double count = (counts[b] - (double)offsets[b]) / periods[b];
        if (!isfinite(count) || count > 32768 || count < 1)
            ta_fail(c, TA_FAILED, "phrase beat count out of range");
        n = ta_min(n, (int)count);
    }
    Coordinates out = { .n = n };
    I *indices[] = { &out.high, &out.middle, &out.low };
    for (int b = 0; b < 3; b++) {
        *indices[b] = ta_new_ints(c, n + 1);
        for (int i = 0; i <= n; i++)
            indices[b]->v[i] = (int)fma(periods[b], (double)i, (double)offsets[b] + 1.5);
        indices[b]->v[n - 1] = ta_min(indices[b]->v[n - 1], (int)counts[b]);
    }
    return out;
}
int activity_start(D samples, double rate, double bpm, int offset)
{
    double period = bpm == 0 ? 0.5 : 60 / bpm;
    int per = (int)fma(period, rate, 0.5), first = (int)fma((double)offset * 0.001, rate, 0.5);
    int length = ta_min(samples.n, (int)(rate * 30)), displacement = 0;
    for (int i = 0; i < length - 1; i++)
        if (first + i >= 0 && first + i < length && fabs(samples.v[first + i]) > 0.0005) {
            displacement = i + 1;
            break;
        }
    return (int)((per == 0 ? (double)displacement * 0.00036281179138321996
                           : (double)displacement / per) +
                 0.5) +
           1;
}
static double range_sum(D s, int start, int n)
{
    if (start < 0 || n <= 0 || start + n > s.n)
        return 0;
    return d_sum((D){ s.v + start, n });
}
static bool low_change(double prev, double next)
{
    return (next > prev && next > prev * 1.2) || (next < prev && prev > next * 1.2);
}
static bool low_quiet(double prev, double next, D s, int i)
{
    return next <= 750 && prev <= 750 && s.v[i * 4 + 4] <= 0.09 && s.v[i * 4 + 3] <= 0.09;
}

Activity low_activity(ta_context *c, D s, int phase)
{
    int extent = s.n - phase + 4, n = ta_max(0, extent / 4);
    Activity a = { ta_new_doubles(c, n), ta_new_bytes(c, n) };
    if (extent >= 12) {
        int written = ta_max(3, n) - 2;
        for (int i = 0; i < written; i++)
            a.values.v[i] = range_sum(s, phase - 1 + i * 4, 4);
        for (int i = 0; i < written; i++) {
            double prev = a.values.v[i], next = a.values.v[i + 1];
            if (low_change(prev, next))
                a.changes.v[i + 1] = 1;
            if (low_quiet(prev, next, s, i))
                a.changes.v[i + 1] = 0;
        }
    }
    return a;
}
static bool middle_change(double previous, double current)
{
    double next = current == 0 ? 0.003 : current,
           prev = previous == 0 ? 0.003 : previous;
    if ((next > prev && next / prev > 1.2) || (prev > next && prev / next > 1.21))
        return 1;
    return 0;
}

Activity middle_activity(ta_context *c, D s, int phase)
{
    int extent = s.n - phase + 4, n = ta_max(0, extent / 4);
    Activity a = { ta_new_doubles(c, n), ta_new_bytes(c, n) };
    if (extent >= 8)
        for (int i = 0; i < ta_max(1, n - 1); i++)
            a.values.v[i] = range_sum(s, phase - 1 + i * 4, 4);
    if (extent >= 12)
        for (int i = 1; i < ta_max(2, n - 1); i++) {
            a.changes.v[i] = middle_change(a.values.v[i - 1], a.values.v[i]);
        }
    return a;
}
AllActivity all_activity(ta_context *c, D s, int phase)
{
    double reciprocal = 1 / d_max(s), scale = reciprocal == 0 ? 0.003 : reciprocal;
    int n = ta_max(0, (s.n - phase + 4) / 4);
    AllActivity a = { d_copy(c, s), ta_new_doubles(c, n) };
    for (int i = 0; i < s.n; i++)
        a.normalized.v[i] *= scale;
    if (s.n - phase + 4 >= 8)
        for (int i = 0; i < ta_max(1, n - 1); i++)
            a.bars.v[i] = range_sum(a.normalized, phase - 1 + i * 4, 4) * 0.25;
    return a;
}
AllActivity middle_peak_activity(ta_context *c, D s, int count, int phase)
{
    double max = d_max(s), scale = max == 0 ? 333.3333333333333 : 1 / max;
    int extent = s.n - phase + 1, n = ta_max(0, extent / 4);
    AllActivity a = { d_copy(c, s), ta_new_doubles(c, n) };
    if (count > 1)
        for (int i = 0; i < ta_min(s.n, count - 1); i++)
            a.normalized.v[i] *= scale;
    if (extent >= 8)
        for (int i = 0; i < ta_max(1, n - 1); i++)
            a.bars.v[i] = range_sum(a.normalized, phase - 1 + i * 4, 4) * 0.25;
    return a;
}
static double interval_sum(D source, int start, int end, int limit)
{
    if (start <= 0 || start > limit || end - 1 > limit)
        return 0;
    return range_sum(source, start - 1, end - start);
}
static double interval_max(D source, int start, int end, int limit, bool absolute)
{
    if (start <= 0 || start > limit || end - 1 > limit || end <= start || end - 1 > source.n)
        return 0;
    double value = absolute ? fabs(source.v[start - 1]) : source.v[start - 1];
    for (int i = start; i < end - 1; i++)
        value = ta_dmax(value, absolute ? fabs(source.v[i]) : source.v[i]);
    return value;
}
static void low_beat(Beats *b, Envelopes e, Coordinates pos, Samples s, double period, int beat, int last, int lowLimit)
{
    int start = pos.low.v[beat],
        end = beat == last - 1 ? start + (int)(period * s.rate / 32) : pos.low.v[beat + 1];
    int levelEnd = beat == last - 1 && end - 1 > lowLimit ? lowLimit : end;
    b->lowLevels.v[beat] = interval_sum(e.lowLevel, start, levelEnd, lowLimit) * 4;
    b->lowPeaks.v[beat] = interval_max(e.low, start, end, lowLimit, false);
    int quarter = (int)((double)(end - start) * 0.25 + 0.5);
    for (int part = 0; part < 4; part++)
        b->quarters.v[beat * 4 + part] =
            interval_max(e.low, start + part * quarter,
                         part == 3 ? end : start + (part + 1) * quarter, lowLimit, false);
    b->quarterMean.v[beat] = range_sum(b->quarters, beat * 4, 4) * 0.25;
}
static void middle_beat(Beats *b, Envelopes e, Coordinates pos, Samples s, int beat, int midLimit)
{
    int start = pos.middle.v[beat];
    int end = pos.middle.v[beat + 1];
    b->middleLevels.v[beat] = interval_sum(e.middleLevel, start, end, midLimit);
    b->middlePeaks.v[beat] = interval_max(e.middle, start, end, midLimit, false);
    b->allLevels.v[beat] = interval_max(s.middle, start, end, midLimit, true);
}
static void high_beat(Beats *b, Envelopes e, Coordinates pos, Samples s, double period, int beat, int last, int highLimit)
{
    int start = pos.high.v[beat];
    double highPeriod = period * s.rate / 2;
    int proposed = start + (int)highPeriod;
    int end = beat == last - 1 ? (proposed - 1 <= highLimit ? proposed : highLimit)
                               : pos.high.v[beat + 1];
    int eighth = (int)((double)(int)(highPeriod + 0.5) * 0.125 + 0.5);
    for (int part = 0; part < 8; part++)
        b->eighths.v[beat * 8 + part] = interval_max(
            e.high, part ? start + part * eighth + 1 : start,
            part == 7 ? end : start + (part + 1) * eighth + 1, highLimit, false);
}

static void collect_beat_bands(ta_context *c, Beats *b, Envelopes e, Samples s, Timing t, B starts)
{
    Coordinates pos = b->coordinates;
    int n = pos.n;
    double period = 60 / t.bpm;
    for (int first = 0; first < n;) {
        ta_check(c);
        starts.v[first] = 1;
        int nominal =
            nominal_beats(period, first ? 1 : t.phase, first ? 0 : (double)t.offset * 0.001);
        if (nominal <= 0)
            ta_fail(c, TA_FAILED, "invalid phrase block timing");
        int last = ta_min(n, first + nominal);
        bool final = first + nominal > n;
        int lowLimit = final ? s.low.n : pos.low.v[last] - 1,
            midLimit = final ? s.middle.n : pos.middle.v[last] - 1,
            highLimit = final ? s.high.n : pos.high.v[last] - 1;
        for (int beat = first; beat < last; beat++) {
            low_beat(b, e, pos, s, period, beat, last, lowLimit);
            middle_beat(b, e, pos, s, beat, midLimit);
            high_beat(b, e, pos, s, period, beat, last, highLimit);
        }
        first = last;
    }
}

static double low_peak_change(double prev, double next, bool block_start)
{
    double value = 0;
    if (block_start && prev == 0)
        return 0;
    if (next > prev && next > prev * 1.77)
        value = 1;
    if (next < prev && prev > next + next)
        value = 0.5;
    if (next <= 0.09 && prev <= 0.09)
        value *= 0.8;
    return value;
}

Beats phrase_beats(ta_context *c, Samples s, Timing t)
{
    Coordinates pos = phrase_coordinates(c, s, t);
    Envelopes e = phrase_envelopes(c, s);
    int n = pos.n;
    Beats b = { .timing = t,
                .coordinates = pos,
                .initial = activity_start(s.middle, s.rate / 8, t.bpm, t.offset) };
    D *arrays[] = { &b.lowLevels, &b.middleLevels, &b.allLevels, &b.lowPeaks,
                    &b.lowPeakChanges, &b.quarterMean, &b.middlePeaks };
    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++)
        *arrays[i] = ta_new_doubles(c, n);
    b.quarters = ta_new_doubles(c, n * 4);
    b.eighths = ta_new_doubles(c, n * 8);
    B starts = ta_new_bytes(c, n);
    collect_beat_bands(c, &b, e, s, t, starts);
    for (int beat = 1; beat < n; beat++) {
        b.lowPeakChanges.v[beat] = low_peak_change(b.lowPeaks.v[beat - 1], b.lowPeaks.v[beat], starts.v[beat]);
    }
    ta_release(c, starts.v);
    ta_release(c, e.low.v);
    ta_release(c, e.middle.v);
    ta_release(c, e.high.v);
    ta_release(c, e.lowLevel.v);
    ta_release(c, e.middleLevel.v);
    b.lowActivity = low_activity(c, b.lowLevels, t.phase);
    b.middleActivity = middle_activity(c, b.middleLevels, t.phase);
    b.allActivity = all_activity(c, b.allLevels, t.phase);
    return b;
}
static int integer_ascending(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int integer_descending(const void *a, const void *b) { return -integer_ascending(a, b); }
I i_sorted(ta_context *c, I a, bool descending)
{
    I out = i_copy(c, a);
    qsort(out.v, out.n, sizeof(int), descending ? integer_descending : integer_ascending);
    return out;
}
I i_compact(ta_context *c, I a, bool unique)
{
    I out = i_sorted(c, a, false);
    int n = 0;
    for (int i = 0; i < out.n; i++)
        if (out.v[i] && (!unique || !n || out.v[i] != out.v[n - 1]))
            out.v[n++] = out.v[i];
    out.n = n;
    return out;
}
I i_join(ta_context *c, I a, I b)
{
    I out = ta_new_ints(c, a.n + b.n);
    if (a.n)
        memcpy(out.v, a.v, a.n * sizeof(int));
    if (b.n)
        memcpy(out.v + a.n, b.v, b.n * sizeof(int));
    return out;
}
I rank_d(ta_context *c, D a, bool descending)
{
    I order = ta_new_ints(c, a.n);
    for (int i = 0; i < a.n; i++) {
        int j = i;
        while (j > 0 &&
               (descending ? a.v[i] > a.v[order.v[j - 1]] : a.v[i] < a.v[order.v[j - 1]])) {
            order.v[j] = order.v[j - 1];
            j--;
        }
        order.v[j] = i;
    }
    return order;
}
void i_remove(I *a, int index)
{
    if (index < 0 || index >= a->n)
        return;
    memmove(a->v + index, a->v + index + 1, (a->n - index - 1) * sizeof(int));
    a->n--;
}

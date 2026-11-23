/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
static double peak_prominence(ta_context *c, D s, int i)
{
    int left = i, right = i;
    double height = s.v[i];
    while (left > 0 && s.v[left] <= height)
        left--;
    while (right < s.n - 1 && s.v[right] <= height)
        right++;
    double prominence = height - ta_dmax(d_min(d_view(c, s, left, i + 1)),
                                         d_min(d_view(c, s, i, right + 1)));
    return prominence;
}

static Peak measure_peak(ta_context *c, D s, int i)
{
    double height = s.v[i], prominence = peak_prominence(c, s, i);
    double half = fma(prominence, -0.5, height);
    int left = i;
    int right = i;
    while (left > 0 && s.v[left] >= half)
        left--;
    while (right < s.n - 1 && s.v[right] >= half)
        right++;
    double lc = (double)(left + 1) + (half - s.v[left]) / (s.v[left + 1] - s.v[left]);
    double rc = (double)right + (half - s.v[right - 1]) / (s.v[right] - s.v[right - 1]);
    return (Peak){ i + 1, height, rc - lc, prominence };
}

Peaks spectral_peaks(ta_context *c, D s)
{
    Peaks out = { ta_alloc(c, ta_max(1, s.n), sizeof(Peak)), 0 };
    int previous = 0;
    for (int i = 0; i < s.n - 1; i++) {
        double difference = s.v[i] - s.v[i + 1];
        int direction = difference < 0 ? 1 : difference == 0 ? 0
                                                             : 2;
        if (difference >= 0 && previous == 1) {
            out.v[out.n++] = measure_peak(c, s, i);
        }
        previous = direction;
    }
    if (!out.n)
        out.n = 1;
    return out;
}
void spectra_release(ta_context *c, Spectra s)
{
    for (int i = 0; i < s.n; i++)
        ta_release(c, s.rows[i].v);
    ta_release(c, s.rows);
}
typedef struct {
    D samples, window, real[3], imag[3];
    Coordinates coordinates;
    D *rows;
    int begin, phase, base, bars, buffer_count, size, rows_per_bar;
} fft_job;
static D fft_row(ta_context *c, fft_job *job, D samples, D real, D imag, int position, int available)
{
    int size = job->size;
    memset(real.v, 0, (size_t)size * sizeof(double));
    memset(imag.v, 0, (size_t)size * sizeof(double));
    for (int i = position; i < available; i++)
        real.v[i - position] = samples.v[i] * job->window.v[i - position];
    ta_fft_d(real.v, imag.v, (size_t)size);
    D row = ta_new_doubles(c, size / 2);
    for (int i = 0; i < size / 2; i++) {
        double magnitude = sqrt(fma(imag.v[i], imag.v[i], real.v[i] * real.v[i]));
        row.v[i] = magnitude < 1e-10 ? -200 : log10(magnitude) * 20;
    }
    return row;
}

static void fft_bar(ta_context *c, void *data, size_t index, unsigned worker)
{
    fft_job *job = data;
    int bar = (int)index, size = job->size;
    D real = job->real[worker], imag = job->imag[worker], samples = job->samples;
    int beat = job->begin + job->phase - 1 + bar * 4;
    int start = job->coordinates.middle.v[beat] - 1 - job->base;
    int end = bar + 1 < job->bars
                  ? ta_min(samples.n, job->coordinates.middle.v[beat + 4] - 1 - job->base)
                  : ta_min(samples.n, start + job->buffer_count);
    if (start < 0 || start >= samples.n || end < start)
        ta_fail(c, TA_FAILED, "spectral input range");
    for (int offset = 0; offset < job->rows_per_bar; offset++) {
        int position = job->coordinates.middle.v[beat + offset * (4 / job->rows_per_bar)] - 1 - job->base;
        int available = ta_min(end, position + size);
        if (position < 0) ta_fail(c, TA_FAILED, "spectral beat range");
        D row = fft_row(c, job, samples, real, imag, position, available);
        job->rows[bar * job->rows_per_bar + offset] = row;
    }
}
static int prepare_fft_block(ta_context *c, fft_job *job, Samples s, Timing t, Coordinates coordinates, int begin, const Spectra *out)
{
    int phase = begin ? 1 : t.phase, offset = begin ? 0 : t.offset;
    int nominal = nominal_beats(60 / t.bpm, phase, (double)offset * 0.001);
    if (nominal <= 0) ta_fail(c, TA_FAILED, "spectral block timing");
    int endBeat = ta_min(coordinates.n, begin + nominal), n = endBeat - begin;
    int base = begin ? coordinates.middle.v[begin] - 1 : 0;
    int limit = nominal > coordinates.n - begin ? s.middle.n : coordinates.middle.v[endBeat] - 1;
    job->samples = d_view(c, s.middle, base, limit);
    job->begin = begin;
    job->phase = phase;
    job->base = base;
    job->bars = (n - phase + 1) / 4;
    job->buffer_count = (int)ceil((s.rate / 8) * 60 / t.bpm) * 4 + job->size + 1;
    job->rows = out->rows + out->n;
    return endBeat;
}

Spectra phrase_spectra(ta_context *c, Samples s, Timing t, Coordinates coordinates,
                       bool recurrence)
{
    int size = recurrence ? 4096 : 2048;
    unsigned threads = ta_phrase_threads(c);
    Spectra out = { ta_alloc(c, coordinates.n, sizeof(D)), 0 };
    fft_job job = { .coordinates = coordinates, .window = ta_new_doubles(c, size), .size = size, .rows_per_bar = recurrence ? 2 : 4 };
    for (unsigned i = 0; i < threads; i++) {
        job.real[i] = ta_new_doubles(c, size);
        job.imag[i] = ta_new_doubles(c, size);
    }
    double center = (double)(size - 1) * 0.5, step = M_PI / (center + 1);
    for (int i = 0; i < size; i++)
        job.window.v[i] = fma(cos(((double)i - center) * step), 0.5, 0.5);
    for (int begin = 0; begin < coordinates.n;) {
        int endBeat = prepare_fft_block(c, &job, s, t, coordinates, begin, &out);
        ta_parallel(c, (size_t)job.bars, threads, fft_bar, &job);
        out.n += job.bars * job.rows_per_bar;
        begin = endBeat;
    }
    ta_release(c, job.window.v);
    for (unsigned i = 0; i < threads; i++) {
        ta_release(c, job.real[i].v);
        ta_release(c, job.imag[i].v);
    }
    return out;
}
typedef struct {
    double frequencies[73];
    int lengths[72], frames;
    D samples;
    double rate;
    Spectra out;
} cqt_job;
static void cqt_basis(const cqt_job *job, int bin, D real, D imag)
{
    int length = real.n;
    double rate = job->rate;
    double angular = job->frequencies[bin % 12] * 6.283185307179586, reciprocal = 1 / rate;
    for (int i = 0; i < length; i++) {
        double angle = ((double)(i + 1) * reciprocal) * angular, r = cos(angle),
               im = sin(angle);
        for (int step = 0; step < bin / 12; step++) {
            double next = fma(-im, im, r * r);
            im = r * (im + im);
            r = next;
        }
        real.v[i] = r;
        imag.v[i] = im;
    }
}

static void cqt_window(D real, D imag)
{
    int length = real.n;
    double reciprocalLength = 1 / (double)(length + 1);
    for (int i = 0; i < length / 2; i++) {
        double window = 0.5 - cos((double)(i + 1) * 6.283185307179586 * reciprocalLength) * 0.5;
        real.v[i] *= window;
        imag.v[i] *= window;
        real.v[length - i - 1] *= window;
        imag.v[length - i - 1] *= window;
    }
    if (length % 2) {
        int middle = length / 2;
        double window =
            0.5 - cos((double)(middle + 1) * 6.283185307179586 / (double)(length + 1)) * 0.5;
        real.v[middle] *= window;
        imag.v[middle] *= window;
    }
}

static bool cqt_range_valid(D samples, int start, int length)
{
    return start >= 0 && start + length <= samples.n;
}
static double cqt_frame(ta_context *c, D samples, D real, D imag, int longest, int frame)
{
    int length = real.n;
    int start = frame * 256 + longest / 2 - length / 2, full = length / 8 * 8;
    double r[8] = { 0 }, im[8] = { 0 };
    if (!cqt_range_valid(samples, start, length))
        ta_fail(c, TA_FAILED, "CQT sample range");
#if defined(__aarch64__)
        /* Preserve Swift SIMD8<Double>'s eight independent FMA chains. */
    float64x2_t vr[4], vi[4];
    for (int lane = 0; lane < 4; lane++) vr[lane] = vi[lane] = vdupq_n_f64(0);
    for (int i = 0; i < full; i += 8)
        for (int lane = 0; lane < 4; lane++) {
            float64x2_t input = vld1q_f64(samples.v + start + i + lane * 2);
            vr[lane] = vfmaq_f64(vr[lane], vld1q_f64(real.v + i + lane * 2), input);
            vi[lane] = vfmaq_f64(vi[lane], vld1q_f64(imag.v + i + lane * 2), input);
        }
    for (int lane = 0; lane < 4; lane++) {
        vst1q_f64(r + lane * 2, vr[lane]);
        vst1q_f64(im + lane * 2, vi[lane]);
    }
#else
    for (int i = 0; i < full; i++) {
        r[i & 7] = fma(real.v[i], samples.v[start + i], r[i & 7]);
        im[i & 7] = fma(imag.v[i], samples.v[start + i], im[i & 7]);
    }
#endif
    double rr = (r[6] + (r[4] + (r[2] + r[0]))) + (r[7] + (r[5] + (r[3] + r[1])));
    double ii = (im[6] + (im[4] + (im[2] + im[0]))) + (im[7] + (im[5] + (im[3] + im[1])));
    for (int i = full; i < length; i++) {
        rr = fma(real.v[i], samples.v[start + i], rr);
        ii = fma(imag.v[i], samples.v[start + i], ii);
    }
    return ((double)longest / (double)length) * sqrt(fma(ii, ii, rr * rr));
}

static void cqt_bin(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)worker;
    cqt_job *job = data;
    int bin = (int)index + 23, frames = job->frames, longest = job->lengths[0];
    D samples = job->samples;
    job->out.rows[bin] = ta_new_doubles(c, frames);
    int length = job->lengths[bin];
    D real = ta_new_doubles(c, length), imag = ta_new_doubles(c, length);
    cqt_basis(job, bin, real, imag);
    cqt_window(real, imag);
    for (int frame = 0; frame < frames; frame++) {
        if (!(frame % 64))
            ta_check(c);
        job->out.rows[bin].v[frame] = cqt_frame(c, samples, real, imag, longest, frame);
    }
    ta_release(c, real.v);
    ta_release(c, imag.v);
}
Spectra opening_spectra(ta_context *c, D samples, double rate)
{
    cqt_job job = { .samples = samples, .rate = rate, .out = { ta_alloc(c, 72, sizeof(D)), 72 } };
    for (int i = 0; i <= 72; i++)
        job.frequencies[i] = exp2((double)i * 0.08333333333333333) * 27.5;
    for (int i = 0; i < 72; i++)
        job.lengths[i] = (int)((rate + rate) / (job.frequencies[i + 1] - job.frequencies[i]));
    if (job.lengths[71] <= 0) ta_fail(c, TA_FAILED, "CQT input rate");
    job.frames = ta_max(0, (samples.n - job.lengths[0]) / 256);
    for (int bin = 0; bin < (job.frames ? 23 : 72); bin++)
        job.out.rows[bin] = ta_new_doubles(c, job.frames);
    if (job.frames) ta_parallel(c, 49, ta_phrase_threads(c), cqt_bin, &job);
    return job.out;
}
static double opening_maximum(ta_context *c, Spectra s, int frames)
{
    double maximum = 0;
    for (int i = 0; i < 72; i++) {
        if (s.rows[i].n != frames)
            ta_fail(c, TA_FAILED, "ragged opening spectra");
        maximum = ta_dmax(maximum, d_max(s.rows[i]));
    }
    return maximum;
}

static double opening_peak_count(Spectra s, int frame, double scale)
{
    double count = 0;
    double peak = s.rows[0].v[frame] * scale;
    bool rising = false, armed = false;
    for (int bin = 1; bin < 72; bin++) {
        double current = s.rows[bin].v[frame] * scale,
               previous = s.rows[bin - 1].v[frame] * scale;
        if (current <= previous) {
            if (rising) {
                if (peak < previous) {
                    peak = previous;
                    armed = true;
                }
            } else if (armed) {
                if (current < peak * 0.9) {
                    if (current > 0.001 && peak > 0.1)
                        count++;
                    armed = false;
                    peak = 0;
                }
            } else
                armed = false;
        }
        rising = previous < current;
    }
    return count;
}

static D opening_levels(ta_context *c, D counts, int first, int beats, int frames)
{
    int bars = beats / 4, position = first, previousEnd = 0;
    D levels = ta_new_doubles(c, bars);
    for (int bar = 0; bar < bars; bar++) {
        int end = (int)((1 / (double)beats) * (double)frames * 4 * (double)(bar + 1) + 0.5);
        if (previousEnd < end) {
            int n = end - previousEnd;
            levels.v[bar] = d_sum(d_view(c, counts, position, position + n));
            position += n;
        }
        previousEnd = position;
    }
    int divisor = ta_max(1, (int)d_max(levels));
    for (int i = 0; i < bars; i++)
        levels.v[i] *= 1 / (double)divisor;
    return levels;
}

static OpeningFeatures opening_presence(ta_context *c, D levels, int bars)
{
    OpeningFeatures f = { ta_new_bytes(c, bars * 4), ta_new_bytes(c, bars * 4),
                          ta_new_doubles(c, bars * 4) };
    for (int i = 0; i < bars; i++) {
        if (levels.v[i] > 0.36)
            f.presence.v[i] = 1;
        if (i > 0) {
            if (f.presence.v[i - 1] < f.presence.v[i]) {
                if (levels.v[i - 1] * 1.2 < levels.v[i])
                    f.rises.v[i] = 1;
            } else if (levels.v[i] > 0.2 && levels.v[i] / levels.v[i - 1] > 1.8)
                f.rises.v[i] = 1;
        }
        for (int j = 0; j < 4; j++)
            f.envelope.v[i * 4 + j] = levels.v[i];
    }
    return f;
}

OpeningFeatures opening_features(ta_context *c, Spectra s, int first, int beats)
{
    if (s.n != 72 || first < 0 || beats < 0)
        ta_fail(c, TA_FAILED, "opening spectrum dimensions");
    int frames = s.rows[0].n;
    double maximum = opening_maximum(c, s, frames);
    if (!(maximum > 0))
        return (OpeningFeatures){ ta_new_bytes(c, 1), ta_new_bytes(c, 1), ta_new_doubles(c, 1) };
    double scale = 1 / maximum;
    D counts = ta_new_doubles(c, frames);
    for (int frame = 0; frame < frames; frame++) {
        counts.v[frame] = opening_peak_count(s, frame, scale);
    }
    int bars = beats / 4;
    D levels = opening_levels(c, counts, first, beats, frames);
    OpeningFeatures f = opening_presence(c, levels, bars);
    ta_release(c, counts.v);
    ta_release(c, levels.v);
    return f;
}
static double vocal_peak(Peaks p)
{
    double peak = 0;
    for (int i = 0; i < p.n; i++)
        if (p.v[i].position >= 112 && p.v[i].width / (double)p.v[i].position > 0.11 &&
            p.v[i].prominence > 10)
            peak = ta_dmax(peak, p.v[i].height);
    return peak;
}

D vocal_activity(ta_context *c, Spectra s)
{
    if (!s.n)
        ta_fail(c, TA_FAILED, "empty vocal spectrum");
    D peaks = ta_new_doubles(c, s.n);
    double total = 0;
    for (int row = 0; row < s.n; row++) {
        if (s.rows[row].n < 743)
            ta_fail(c, TA_FAILED, "short vocal spectrum");
        ta_phrase_biquad section =
            second_order(5512.5, 471.23889803846896, 1.4142135623730951, false);
        D values = filter(c, &section, 1, s.rows[row], false);
        Peaks p = spectral_peaks(c, values);
        peaks.v[row] = vocal_peak(p);
        double sum = 0;
        for (int i = 111; i < 743; i++)
            sum += pow(10, values.v[i] * 0.05);
        double level = log10(sum * 0.0015822784810126582) * 20;
        total += pow(10, level * 0.05);
        ta_release(c, p.v);
        ta_release(c, values.v);
    }
    double reference = log10(total / (double)s.n);
    for (int i = 0; i < peaks.n; i++)
        peaks.v[i] = peaks.v[i] > reference * 38.8 ? ta_dmax(0, peaks.v[i]) : 0;
    return peaks;
}
static void long_silence_entries(B mask, B selected, int limit)
{
    int silence = 0;
    bool armed = false;
    for (int i = 0; i < limit; i++) {
        if (!mask.v[i]) {
            if (silence > 6)
                armed = true;
            silence++;
        } else {
            int n = 0;
            for (int j = i; j <= ta_min(i + 2, mask.n - 1); j++)
                n += mask.v[j];
            if (armed && n >= 2)
                selected.v[i + 1] = 1;
            armed = false;
            silence = 0;
        }
    }
}

static bool short_entry_ready(B mask, D normalized, int i, bool longSilence)
{
    return !longSilence && mask.v[i] + mask.v[i + 1] + mask.v[i + 2] > 2 && normalized.v[i] >= 0.7;
}
static void short_silence_entries(B mask, B selected, D normalized, D ratios, int count)
{
    int silence = 0;
    bool armed = false;
    bool longSilence = false;
    for (int i = 0; i < count - 8; i++) {
        if (!mask.v[i]) {
            if (silence > 6) {
                armed = false;
                longSilence = true;
            } else if (silence > 1)
                armed = true;
            silence++;
        } else {
            if (armed && short_entry_ready(mask, normalized, i, longSilence)) {
                int bar = i / 4;
                if (ratios.v[bar] >= 1.8 || ratios.v[bar + 1] >= 3)
                    selected.v[i + 1] = 1;
            }
            silence = 0;
            armed = false;
            longSilence = false;
        }
    }
}

static int prepare_vocal_entries(ta_context *c, D normalized, D sums, D ratios, B mask, int limit)
{
    int active = 0;
    for (int i = 0; i < limit; i++) {
        mask.v[i] = normalized.v[i] > 0;
        active += mask.v[i];
    }
    for (int i = 0; i < sums.n - 1; i++)
        sums.v[i] = d_sequential(d_view(c, normalized, i * 4, i * 4 + 4));
    for (int i = 1; i < sums.n - 1; i++)
        ratios.v[i] = sums.v[i - 1] == 0 ? 0 : sums.v[i] / sums.v[i - 1];
    return active;
}
I vocal_entries(ta_context *c, D activity)
{
    I entries = ta_new_ints(c, activity.n);
    entries.n = 0;
    if (!activity.n)
        return entries;
    D normalized = d_normalized(c, activity), sums = ta_new_doubles(c, activity.n / 4),
      ratios = ta_new_doubles(c, sums.n);
    B mask = ta_new_bytes(c, activity.n), selected = ta_new_bytes(c, activity.n + 1);
    int limit = ta_max(0, activity.n - 4);
    int active = prepare_vocal_entries(c, normalized, sums, ratios, mask, limit);
    long_silence_entries(mask, selected, limit);
    double density = limit ? (double)active / limit : 0;
    if (activity.n > 8 && density < 0.6) {
        short_silence_entries(mask, selected, normalized, ratios, activity.n);
    }
    for (int i = 1; i < selected.n; i++)
        if (selected.v[i])
            entries.v[entries.n++] = i / 4 + 1;
    ta_release(c, normalized.v);
    ta_release(c, sums.v);
    ta_release(c, ratios.v);
    ta_release(c, mask.v);
    ta_release(c, selected.v);
    return entries;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
static double peak_prominence(ta_context *c, D s, int i, const double *parameters)
{
    int left = i, right = i;
    double height = s.v[i];
    int last = s.n - (int)parameters[TA_SPECTRAL_PEAK_PROMINENCE_ORIGIN];
    while (left > 0 && s.v[left] <= height)
        left--;
    while (right < last && s.v[right] <= height)
        right++;
    int left_end = ta_min(s.n, i + (int)parameters[TA_SPECTRAL_PEAK_PROMINENCE_LEFT_END]);
    int right_end = ta_min(s.n, right + (int)parameters[TA_SPECTRAL_PEAK_PROMINENCE_RIGHT_END]);
    return height - ta_dmax(d_min(d_view(c, s, left, left_end)), d_min(d_view(c, s, i, right_end)));
}

static double interpolated_peak_width(ta_context *c, D s, int left, int right, double half,
                                      const double *parameters)
{
    int left_position = left + (int)parameters[TA_SPECTRAL_PEAK_WIDTH_LEFT_BIAS];
    int left_sample = left_position + (int)parameters[TA_SPECTRAL_PEAK_WIDTH_LEFT_PAIR_BACK];
    int right_position = right + (int)parameters[TA_SPECTRAL_PEAK_WIDTH_SCAN_ORIGIN];
    int right_sample = right_position - (int)parameters[TA_SPECTRAL_PEAK_WIDTH_RIGHT_INDEX_BACK];
    int previous = right_position - (int)parameters[TA_SPECTRAL_PEAK_WIDTH_RIGHT_SAMPLE_BACK];
    if (left_sample < 0 || left_sample >= s.n - 1 || previous < 0 || right_sample >= s.n)
        ta_fail(c, TA_FAILED, "spectral peak interpolation range");
    double lc = (double)left_position + (half - s.v[left_sample]) / (s.v[left_sample + 1] - s.v[left_sample]);
    double rc = (double)right_sample + (half - s.v[previous]) / (s.v[right_sample] - s.v[previous]);
    return rc - lc;
}

static Peak measure_peak(ta_context *c, D s, int i, const double *parameters)
{
    double height = s.v[i], prominence = peak_prominence(c, s, i, parameters);
    double half = fma(prominence, parameters[TA_SPECTRAL_PEAK_WIDTH_PROMINENCE_SCALE], height);
    int left = i;
    int right = i;
    int last = s.n - (int)parameters[TA_SPECTRAL_PEAK_WIDTH_SCAN_ORIGIN];
    while (left > 0 && s.v[left] >= half)
        left--;
    while (right < last && s.v[right] >= half)
        right++;
    int position = i + (int)parameters[TA_SPECTRAL_PEAK_POSITION_ORIGIN];
    return (Peak){ position, height, interpolated_peak_width(c, s, left, right, half, parameters), prominence };
}

Peaks spectral_peaks(ta_context *c, D s, const double *parameters)
{
    int fallback = (int)parameters[TA_SPECTRAL_PEAK_FALLBACK_COUNT];
    Peaks out = { ta_alloc(c, ta_max(fallback, s.n), sizeof(Peak)), 0 };
    int end = s.n < parameters[TA_SPECTRAL_PEAK_SCAN_MIN] ? 0 : s.n - (int)parameters[TA_SPECTRAL_PEAK_SCAN_TAIL];
    int step = (int)parameters[TA_SPECTRAL_PEAK_SCAN_STEP];
    double slope = parameters[TA_SPECTRAL_PEAK_SCAN_SLOPE_THRESHOLD];
    bool rising = false;
    for (int i = 0; i < end; i += step) {
        double difference = s.v[i] - s.v[i + 1];
        if (difference >= slope && rising)
            out.v[out.n++] = measure_peak(c, s, i, parameters);
        rising = difference < slope;
    }
    if (!out.n)
        out.n = fallback;
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
    const double *parameters;
    double middle_rate, magnitude_floor, db_floor, db_scale;
    int begin, phase, base, bars, buffer_count, size, rows_per_bar, output_bins;
    int beats_per_bar, beat_origin, sample_origin, buffer_fixed_samples;
} fft_job;
static D fft_row(ta_context *c, fft_job *job, D samples, D real, D imag, int position, int available)
{
    int size = job->size;
    memset(real.v, 0, (size_t)size * sizeof(double));
    memset(imag.v, 0, (size_t)size * sizeof(double));
    for (int i = position; i < available; i++)
        real.v[i - position] = samples.v[i] * job->window.v[i - position];
    ta_fft_d(real.v, imag.v, (size_t)size);
    D row = ta_new_doubles(c, job->output_bins);
    for (int i = 0; i < row.n; i++) {
        double magnitude = sqrt(fma(imag.v[i], imag.v[i], real.v[i] * real.v[i]));
        row.v[i] = magnitude < job->magnitude_floor ? job->db_floor : log10(magnitude) * job->db_scale;
    }
    return row;
}

static int fft_sample(const fft_job *job, int beat)
{
    return job->coordinates.middle.v[beat] - job->sample_origin - job->base;
}

static void fft_bar(ta_context *c, void *data, size_t index, unsigned worker)
{
    fft_job *job = data;
    int bar = (int)index, size = job->size;
    D real = job->real[worker], imag = job->imag[worker], samples = job->samples;
    int beat = job->begin + job->phase - job->beat_origin + bar * job->beats_per_bar;
    int start = fft_sample(job, beat);
    int end = bar + 1 < job->bars
                  ? ta_min(samples.n, fft_sample(job, beat + job->beats_per_bar))
                  : ta_min(samples.n, start + job->buffer_count);
    if (start < 0 || start >= samples.n || end < start)
        ta_fail(c, TA_FAILED, "spectral input range");
    int beat_stride = job->beats_per_bar / job->rows_per_bar;
    for (int offset = 0; offset < job->rows_per_bar; offset++) {
        int position = fft_sample(job, beat + offset * beat_stride);
        int available = ta_min(end, position + size);
        if (position < 0) ta_fail(c, TA_FAILED, "spectral beat range");
        D row = fft_row(c, job, samples, real, imag, position, available);
        job->rows[bar * job->rows_per_bar + offset] = row;
    }
}
static int prepare_fft_block(ta_context *c, fft_job *job, Samples s, Timing t, Coordinates coordinates, int begin, const Spectra *out)
{
    const double *parameters = job->parameters;
    int phase = begin ? (int)parameters[TA_RHYTHM_BLOCK_RESET_PHASE] : t.phase;
    int offset = begin ? (int)parameters[TA_BEAT_BLOCK_RESET_OFFSET_MS] : t.offset;
    int nominal = nominal_beats(t.bpm, phase, offset, job->parameters);
    if (nominal <= 0) ta_fail(c, TA_FAILED, "spectral block timing");
    int endBeat = ta_min(coordinates.n, begin + nominal), n = endBeat - begin;
    int back = (int)parameters[TA_BEAT_BLOCK_MIDDLE_SAMPLE_BACK];
    int base = begin ? coordinates.middle.v[begin] - back : 0;
    int limit = nominal > coordinates.n - begin
                ? s.middle.n : coordinates.middle.v[endBeat] - back;
    job->samples = d_view(c, s.middle, base, limit);
    job->begin = begin;
    job->phase = phase;
    job->base = base;
    job->bars = ta_max(0, (n - phase + job->beat_origin) / job->beats_per_bar);
    double seconds_per_minute = job->parameters[TA_NOMINAL_BEAT_PERIOD_NUMERATOR];
    job->buffer_count = (int)ceil(job->middle_rate * seconds_per_minute / t.bpm) * job->beats_per_bar +
                        job->buffer_fixed_samples;
    job->rows = out->rows + out->n;
    return endBeat;
}

static void phrase_fft_window(D window, const double *parameters)
{
    double center = (window.n - parameters[TA_PHRASE_FFT_WINDOW_TERMINAL_BACK]) *
                    parameters[TA_PHRASE_FFT_WINDOW_CENTER_SCALE];
    double step = parameters[TA_PHRASE_FFT_WINDOW_PHASE] /
                  (center + parameters[TA_PHRASE_FFT_WINDOW_DENOMINATOR_BIAS]);
    double gain = parameters[TA_PHRASE_FFT_WINDOW_GAIN];
    for (int i = 0; i < window.n; i++)
        window.v[i] = fma(cos(((double)i - center) * step), gain, gain);
}

Spectra phrase_spectra(ta_context *c, Samples s, Timing t, Coordinates coordinates,
                       bool recurrence, const double *parameters)
{
    static const struct {
        int size, rows, magnitude_floor, db_floor, db_scale;
        int beats, beat_origin, sample_origin, buffer_fixed_samples, output_bins;
    } fields[] = {
        [false] = { TA_VOCAL_FFT_SIZE, TA_VOCAL_FFT_ROWS_PER_BAR, TA_VOCAL_FFT_MAGNITUDE_FLOOR,
                    TA_VOCAL_FFT_DB_FLOOR, TA_VOCAL_FFT_DB_SCALE, TA_VOCAL_FFT_BEATS_PER_BAR,
                    TA_VOCAL_FFT_BEAT_ORIGIN, TA_VOCAL_FFT_SAMPLE_ORIGIN,
                    TA_VOCAL_FFT_BUFFER_FIXED_SAMPLES, TA_VOCAL_FFT_OUTPUT_BINS },
        [true] = { TA_RECURRENCE_FFT_SIZE, TA_RECURRENCE_FFT_ROWS_PER_BAR, TA_RECURRENCE_FFT_MAGNITUDE_FLOOR,
                   TA_RECURRENCE_FFT_DB_FLOOR, TA_RECURRENCE_FFT_DB_SCALE, TA_RECURRENCE_FFT_BEATS_PER_BAR,
                   TA_RECURRENCE_FFT_BEAT_ORIGIN, TA_RECURRENCE_FFT_SAMPLE_ORIGIN,
                   TA_RECURRENCE_FFT_BUFFER_FIXED_SAMPLES, TA_RECURRENCE_FFT_OUTPUT_BINS }
    };
    int size = (int)parameters[fields[recurrence].size];
    unsigned threads = ta_phrase_threads(c);
    Spectra out = { ta_alloc(c, coordinates.n, sizeof(D)), 0 };
    fft_job job = {
        .parameters = parameters,
        .coordinates = coordinates,
        .window = ta_new_doubles(c, size),
        .size = size,
        .rows_per_bar = (int)parameters[fields[recurrence].rows],
        .middle_rate = s.rate * parameters[TA_DECIMATE_MIDDLE_RATE_SCALE],
        .magnitude_floor = parameters[fields[recurrence].magnitude_floor],
        .db_floor = parameters[fields[recurrence].db_floor],
        .db_scale = parameters[fields[recurrence].db_scale],
        .beats_per_bar = (int)parameters[fields[recurrence].beats],
        .beat_origin = (int)parameters[fields[recurrence].beat_origin],
        .sample_origin = (int)parameters[fields[recurrence].sample_origin],
        .buffer_fixed_samples = (int)parameters[fields[recurrence].buffer_fixed_samples],
        .output_bins = (int)parameters[fields[recurrence].output_bins],
    };
    for (unsigned i = 0; i < threads; i++) {
        job.real[i] = ta_new_doubles(c, size);
        job.imag[i] = ta_new_doubles(c, size);
    }
    phrase_fft_window(job.window, parameters);
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
    double center_fraction;
    int group, pairs;
    bool windows_model;
    /* ARM64 reduction order or the Windows fused flag for each sample. */
    uint8_t real_steps[TA_OPENING_CQT_PAIR_CAPACITY], imag_steps[TA_OPENING_CQT_PAIR_CAPACITY];
} cqt_geometry;

typedef struct {
    double *frequencies;
    int *lengths;
    int frames, first_computed_bin, bins_per_octave, hop;
    D samples;
    const double *parameters;
    double rate, angular_scale;
    cqt_geometry geometry;
    Spectra out;
} cqt_job;
static void cqt_basis(const cqt_job *job, int bin, D real, D imag)
{
    int length = real.n;
    double rate = job->rate;
    double angular = job->frequencies[bin % job->bins_per_octave] * job->angular_scale;
    double reciprocal = 1 / rate;
    double first = job->parameters[TA_OPENING_CQT_BASIS_FIRST_ORDINAL];
    for (int i = 0; i < length; i++) {
        double angle = ((i + first) * reciprocal) * angular, r = cos(angle),
               im = sin(angle);
        for (int step = 0; step < bin / job->bins_per_octave; step++) {
            double next = fma(-im, im, r * r);
            im = r * (im + im);
            r = next;
        }
        real.v[i] = r;
        imag.v[i] = im;
    }
}

static void cqt_window(D real, D imag, const double *parameters)
{
    int length = real.n;
    double denominator = length + parameters[TA_OPENING_CQT_HANN_LENGTH_BIAS];
    double reciprocalLength = 1 / denominator;
    double first = parameters[TA_OPENING_CQT_HANN_FIRST_ORDINAL];
    double angle = parameters[TA_OPENING_CQT_HANN_ANGLE];
    double gain = parameters[TA_OPENING_CQT_HANN_GAIN];
    int mirror_back = (int)parameters[TA_OPENING_CQT_HANN_MIRROR_BACK];
    bool odd = length % 2;
    int split = (int)parameters[odd ? TA_OPENING_CQT_HANN_ODD_SPLIT : TA_OPENING_CQT_HANN_EVEN_SPLIT];
    int half = length / split;
    for (int i = 0; i < half; i++) {
        double window = gain - cos((i + first) * angle * reciprocalLength) * gain;
        real.v[i] *= window;
        imag.v[i] *= window;
        real.v[length - i - mirror_back] *= window;
        imag.v[length - i - mirror_back] *= window;
    }
    if (odd) {
        double center_gain = parameters[TA_OPENING_CQT_HANN_ODD_CENTER_GAIN];
        double window = center_gain - cos((half + first) * angle / denominator) * center_gain;
        real.v[half] *= window;
        imag.v[half] *= window;
    }
}

static bool cqt_range_valid(D samples, int start, int length)
{
    return start >= 0 && start + length <= samples.n;
}
/* Each pair keeps two independent FMA chains until the profiled reduction. */
#if defined(__aarch64__)
typedef float64x2_t cqt_pair;

static cqt_pair cqt_pair_fma(cqt_pair sum, const double *kernel, const double *samples)
{
    return vfmaq_f64(sum, vld1q_f64(kernel), vld1q_f64(samples));
}

static cqt_pair cqt_pair_add(cqt_pair a, cqt_pair b)
{
    return vaddq_f64(a, b);
}

static double cqt_pair_sum(cqt_pair pair)
{
    return vaddvq_f64(pair);
}
#else
typedef struct { double even, odd; } cqt_pair;

static cqt_pair cqt_pair_fma(cqt_pair sum, const double *kernel, const double *samples)
{
    return (cqt_pair){ fma(kernel[0], samples[0], sum.even),
                       fma(kernel[1], samples[1], sum.odd) };
}

static cqt_pair cqt_pair_add(cqt_pair a, cqt_pair b)
{
    return (cqt_pair){ a.even + b.even, a.odd + b.odd };
}

static double cqt_pair_sum(cqt_pair pair)
{
    return pair.even + pair.odd;
}
#endif

static double cqt_reduce_pairs(const cqt_pair *sums, const uint8_t *order, int pairs)
{
    cqt_pair sum = cqt_pair_add(sums[order[0]], sums[order[1]]);
    for (int pair = 2; pair < pairs; pair++)
        sum = cqt_pair_add(sums[order[pair]], sum);
    return cqt_pair_sum(sum);
}

static double cqt_add_product(double sum, double sample, double kernel, bool fused)
{
    if (fused)
        return fma(sample, kernel, sum);
    /* The analysis target compiles with FP contraction disabled. */
    return sum + sample * kernel;
}

static double cqt_scalar_dot(const double *samples, D kernel, int group, const uint8_t *steps)
{
    double sum = 0;
    int full = kernel.n / group * group;
    for (int i = 0; i < full; i += group)
        for (int lane = 0; lane < group; ++lane)
            sum = cqt_add_product(sum, samples[i + lane], kernel.v[i + lane], steps[lane]);
    for (int i = full; i < kernel.n; ++i)
        sum = cqt_add_product(sum, samples[i], kernel.v[i], false);
    return sum;
}

static double cqt_magnitude(double real, double imag, double scale)
{
    return sqrt(fma(imag, imag, real * real)) * scale;
}

static double cqt_frame(ta_context *c, D samples, D real, D imag, int longest, int offset,
                        const cqt_geometry *geometry)
{
    int length = real.n;
    double center = geometry->center_fraction;
    int start = offset + (int)(longest * center) - (int)(length * center);
    if (!cqt_range_valid(samples, start, length))
        ta_fail(c, TA_FAILED, "CQT sample range");
    double scale = (double)longest / (double)length;
    if (geometry->windows_model) {
        double rr = cqt_scalar_dot(samples.v + start, real, geometry->group, geometry->real_steps);
        double ii = cqt_scalar_dot(samples.v + start, imag, geometry->group, geometry->imag_steps);
        return cqt_magnitude(rr, ii, scale);
    }
    int full = length / geometry->group * geometry->group;
    cqt_pair r[TA_OPENING_CQT_PAIR_CAPACITY] = { 0 }, im[TA_OPENING_CQT_PAIR_CAPACITY] = { 0 };
    for (int i = 0; i < full; i += geometry->group)
        for (int pair = 0; pair < geometry->pairs; pair++) {
            int lane = i + pair * 2;
            const double *input = samples.v + start + lane;
            r[pair] = cqt_pair_fma(r[pair], real.v + lane, input);
            im[pair] = cqt_pair_fma(im[pair], imag.v + lane, input);
        }
    double rr = cqt_reduce_pairs(r, geometry->real_steps, geometry->pairs);
    double ii = cqt_reduce_pairs(im, geometry->imag_steps, geometry->pairs);
    for (int i = full; i < length; i++) {
        rr = fma(real.v[i], samples.v[start + i], rr);
        ii = fma(imag.v[i], samples.v[start + i], ii);
    }
    return cqt_magnitude(rr, ii, scale);
}

static void cqt_bin(ta_context *c, void *data, size_t index, unsigned worker)
{
    (void)worker;
    cqt_job *job = data;
    int bin = (int)index + job->first_computed_bin, frames = job->frames, longest = job->lengths[0];
    D samples = job->samples;
    job->out.rows[bin] = ta_new_doubles(c, frames);
    int length = job->lengths[bin];
    D real = ta_new_doubles(c, length), imag = ta_new_doubles(c, length);
    cqt_basis(job, bin, real, imag);
    cqt_window(real, imag, job->parameters);
    for (int frame = 0; frame < frames; frame++) {
        if (!(frame % 64))
            ta_check(c);
        job->out.rows[bin].v[frame] = cqt_frame(c, samples, real, imag, longest, frame * job->hop,
                                                &job->geometry);
    }
    ta_release(c, real.v);
    ta_release(c, imag.v);
}
static void cqt_lengths(ta_context *c, cqt_job *job, const double *parameters)
{
    double base = parameters[TA_OPENING_CQT_BASE_FREQUENCY];
    double step = parameters[TA_OPENING_CQT_FREQUENCY_STEP];
    double rate = job->rate * parameters[TA_OPENING_CQT_LENGTH_RATE_SCALE];
    for (int i = 0; i <= job->out.n; i++)
        job->frequencies[i] = exp2((double)i * step) * base;
    for (int i = 0; i < job->out.n; i++) {
        double width = job->frequencies[i + 1] - job->frequencies[i];
        double length = rate / width;
        if (!isfinite(length) || length < 1 || length >= INT_MAX)
            ta_fail(c, TA_FAILED, "CQT input rate");
        job->lengths[i] = (int)length;
    }
}

static cqt_geometry opening_cqt_geometry(const ta_context *c, const double *parameters)
{
    int group = (int)parameters[TA_OPENING_CQT_FMA_GROUP];
    cqt_geometry geometry = {
        .center_fraction = parameters[TA_OPENING_CQT_CENTER_FRACTION],
        .group = group, .pairs = group / 2, .windows_model = c->windows_model
    };
    int steps = geometry.windows_model ? group : geometry.pairs;
    for (int i = 0; i < steps; i++) {
        geometry.real_steps[i] = (uint8_t)parameters[TA_OPENING_CQT_REAL_PAIR_0 + i];
        geometry.imag_steps[i] = (uint8_t)parameters[TA_OPENING_CQT_IMAG_PAIR_0 + i];
    }
    return geometry;
}

Spectra opening_spectra(ta_context *c, D samples, double rate, const double *parameters)
{
    int bins = (int)parameters[TA_OPENING_CQT_BIN_COUNT];
    cqt_job job = {
        .parameters = parameters,
        .samples = samples, .rate = rate, .out = { ta_alloc(c, bins, sizeof(D)), bins },
        .frequencies = ta_alloc(c, bins + 1, sizeof(double)),
        .lengths = ta_alloc(c, bins, sizeof(int)),
        .first_computed_bin = (int)parameters[TA_OPENING_CQT_FIRST_COMPUTED_BIN],
        .bins_per_octave = (int)parameters[TA_OPENING_CQT_BINS_PER_OCTAVE],
        .hop = (int)parameters[TA_OPENING_CQT_HOP_SAMPLES],
        .angular_scale = parameters[TA_OPENING_CQT_ANGULAR_SCALE],
        .geometry = opening_cqt_geometry(c, parameters),
    };
    cqt_lengths(c, &job, parameters);
    job.frames = ta_max(0, (samples.n - job.lengths[0]) / job.hop);
    for (int bin = 0; bin < (job.frames ? job.first_computed_bin : bins); bin++)
        job.out.rows[bin] = ta_new_doubles(c, job.frames);
    if (job.frames)
        ta_parallel(c, (size_t)(bins - job.first_computed_bin), ta_phrase_threads(c), cqt_bin, &job);
    ta_release(c, job.lengths);
    ta_release(c, job.frequencies);
    return job.out;
}
static double opening_maximum(ta_context *c, Spectra s, int frames)
{
    double maximum = 0;
    for (int i = 0; i < s.n; i++) {
        if (s.rows[i].n != frames)
            ta_fail(c, TA_FAILED, "ragged opening spectra");
        maximum = ta_dmax(maximum, d_max(s.rows[i]));
    }
    return maximum;
}

static double opening_peak_count(Spectra s, int frame, double scale, const double *parameters)
{
    double count = 0;
    double peak = s.rows[0].v[frame] * scale;
    bool rising = false, armed = false;
    for (int bin = 1; bin < s.n; bin++) {
        double current = s.rows[bin].v[frame] * scale,
               previous = s.rows[bin - 1].v[frame] * scale;
        if (current <= previous) {
            if (rising) {
                if (peak < previous) {
                    peak = previous;
                    armed = true;
                }
            } else if (armed) {
                if (current < peak * parameters[TA_OPENING_PEAK_FALL_SCALE]) {
                    if (current > parameters[TA_OPENING_PEAK_FALL_MIN] && peak > parameters[TA_OPENING_PEAK_MIN])
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

static int opening_frame_end(bool windows_model, double frame_bar, double inverse, double bias, int ordinal)
{
    if (windows_model)
        return (int)fma(ordinal, frame_bar, bias);
    return (int)fma(frame_bar * ordinal, inverse, bias);
}

static D opening_levels(ta_context *c, D counts, int first, int beats, int frames, const double *parameters)
{
    int bars = beats / (int)parameters[TA_OPENING_LEVEL_BAR_DIVISOR];
    D levels = ta_new_doubles(c, bars);
    if (beats < parameters[TA_OPENING_LEVEL_MIN_BEATS]) return levels;
    double frame_bar = frames * parameters[TA_OPENING_LEVEL_FRAME_BAR_SCALE];
    double inverse = 0;
    if (c->windows_model)
        frame_bar /= beats;
    else
        inverse = parameters[TA_OPENING_LEVEL_FRAME_INVERSE_NUMERATOR] / beats;
    double bias = parameters[TA_OPENING_LEVEL_FRAME_ROUND_BIAS];
    int ordinal = (int)parameters[TA_OPENING_LEVEL_BAR_ORDINAL_BIAS];
    int position = first, previousEnd = 0;
    for (int bar = 0; bar < bars; bar++) {
        int end = opening_frame_end(c->windows_model, frame_bar, inverse, bias, bar + ordinal);
        if (previousEnd < end) {
            int n = end - previousEnd;
            levels.v[bar] = d_sum(d_view(c, counts, position, position + n), parameters);
            position += n;
        }
        previousEnd = position;
    }
    int divisor = (int)d_max(levels);
    if (!divisor) divisor = (int)parameters[TA_OPENING_LEVEL_ZERO_DIVISOR];
    double scale = parameters[TA_OPENING_LEVEL_NORMALIZE_NUMERATOR] / divisor;
    for (int i = 0; i < bars; i++)
        levels.v[i] *= scale;
    return levels;
}

static OpeningFeatures opening_presence(ta_context *c, D levels, int bars, const double *parameters)
{
    int width = (int)parameters[TA_OPENING_PRESENCE_GROUP_BEATS];
    int back = (int)parameters[TA_OPENING_PRESENCE_PREVIOUS_BACK];
    uint8_t mark = (uint8_t)parameters[TA_OPENING_PRESENCE_MARK];
    OpeningFeatures f = { ta_new_bytes(c, bars * width), ta_new_bytes(c, bars * width),
                          ta_new_doubles(c, bars * width) };
    for (int i = 0; i < bars; i++) {
        if (levels.v[i] > parameters[TA_OPENING_PRESENCE_LEVEL])
            f.presence.v[i] = mark;
        if (i >= back) {
            if (f.presence.v[i - back] < f.presence.v[i]) {
                if (levels.v[i - back] * parameters[TA_OPENING_PRESENCE_RISE_SCALE] < levels.v[i])
                    f.rises.v[i] = mark;
            } else if (levels.v[i] > parameters[TA_OPENING_RISE_LEVEL] &&
                       levels.v[i] / levels.v[i - back] > parameters[TA_OPENING_RISE_RATIO])
                f.rises.v[i] = mark;
        }
        for (int j = 0; j < width; j++)
            f.envelope.v[i * width + j] = levels.v[i];
    }
    return f;
}

int opening_first_frame(ta_context *c, double middle_rate, Timing timing, const double *parameters)
{
    double phase = timing.phase - parameters[TA_OPENING_START_PHASE_ORIGIN];
    double phase_seconds = phase * parameters[TA_OPENING_START_PERIOD_NUMERATOR] / timing.bpm;
    double seconds = fma(parameters[TA_OPENING_START_OFFSET_MS],
                         parameters[TA_OPENING_START_OFFSET_SCALE], phase_seconds);
    double frame = ceil((middle_rate * parameters[TA_OPENING_HOP_RATE]) * seconds);
    if (!(frame >= 0 && frame <= INT_MAX))
        ta_fail(c, TA_FAILED, "invalid opening first frame");
    return (int)frame;
}

OpeningFeatures opening_features(ta_context *c, Spectra s, int first, int beats, const double *parameters)
{
    if (s.n != parameters[TA_OPENING_CQT_BIN_COUNT] || first < 0 || beats < 0)
        ta_fail(c, TA_FAILED, "opening spectrum dimensions");
    int frames = s.rows[0].n;
    double maximum = opening_maximum(c, s, frames);
    if (!(maximum > 0))
        return (OpeningFeatures){ ta_new_bytes(c, 1), ta_new_bytes(c, 1), ta_new_doubles(c, 1) };
    double scale = 1 / maximum;
    D counts = ta_new_doubles(c, frames);
    for (int frame = 0; frame < frames; frame++) {
        counts.v[frame] = opening_peak_count(s, frame, scale, parameters);
    }
    D levels = opening_levels(c, counts, first, beats, frames, parameters);
    OpeningFeatures f = opening_presence(c, levels, levels.n, parameters);
    ta_release(c, counts.v);
    ta_release(c, levels.v);
    return f;
}
static double vocal_peak(Peaks p, const double *parameters)
{
    double peak = 0;
    for (int i = 0; i < p.n; i++)
        if (p.v[i].position >= parameters[TA_PHRASE_VOCAL_MIN_PEAK_BIN] &&
            p.v[i].width / (double)p.v[i].position > parameters[TA_PHRASE_VOCAL_MIN_PEAK_WIDTH] &&
            p.v[i].prominence > parameters[TA_PHRASE_VOCAL_MIN_PEAK_PROMINENCE])
            peak = ta_dmax(peak, p.v[i].height);
    return peak;
}

static double spectral_vocal_row_power(D values, int first, int end, const double *parameters)
{
    double bin_scale = parameters[TA_PHRASE_VOCAL_BIN_EXPONENT_SCALE];
    double sum = 0;
    for (int i = first; i < end; i++)
        sum += pow(10, values.v[i] * bin_scale);
    double level = log10(sum * parameters[TA_PHRASE_VOCAL_BIN_MEAN_SCALE]) *
                   parameters[TA_PHRASE_VOCAL_ROW_DECIBEL_SCALE];
    return pow(10, level * parameters[TA_PHRASE_VOCAL_ROW_EXPONENT_SCALE]);
}

static D spectral_vocal_filter(ta_context *c, D row, const double *parameters)
{
    int count = (int)parameters[TA_PHRASE_VOCAL_FILTER_SECTIONS];
    ta_phrase_biquad sections[count];
    for (int i = 0; i < count; i++)
        sections[i] = second_order(parameters[TA_PHRASE_VOCAL_FILTER_RATE], parameters[TA_PHRASE_VOCAL_FILTER_CUTOFF],
                                   parameters[TA_PHRASE_VOCAL_FILTER_DAMPING], false);
    return filter(c, sections, count, row, false);
}

D vocal_activity(ta_context *c, Spectra s, const double *parameters)
{
    if (!s.n)
        ta_fail(c, TA_FAILED, "empty vocal spectrum");
    D peaks = ta_new_doubles(c, s.n);
    double total = 0;
    int first_bin = (int)parameters[TA_PHRASE_VOCAL_FIRST_BIN];
    int end_bin = (int)parameters[TA_PHRASE_VOCAL_END_BIN];
    for (int row = 0; row < s.n; row++) {
        if (s.rows[row].n < end_bin)
            ta_fail(c, TA_FAILED, "short vocal spectrum");
        D values = spectral_vocal_filter(c, s.rows[row], parameters);
        Peaks p = spectral_peaks(c, values, parameters);
        peaks.v[row] = vocal_peak(p, parameters);
        total += spectral_vocal_row_power(values, first_bin, end_bin, parameters);
        ta_release(c, p.v);
        ta_release(c, values.v);
    }
    double reference = log10(total / (double)s.n);
    for (int i = 0; i < peaks.n; i++)
        peaks.v[i] = peaks.v[i] > reference * parameters[TA_PHRASE_VOCAL_REFERENCE_SCALE] ? ta_dmax(0, peaks.v[i]) : 0;
    return peaks;
}
static int vocal_resume_count(B mask, int start, int last_delta)
{
    int end = ta_min(last_delta, mask.n - 1 - start);
    int count = 0;
    for (int i = 0; i <= end; i++)
        count += mask.v[start + i];
    return count;
}
static void long_silence_entries(B mask, B selected, int limit, const double *parameters)
{
    int silence = 0;
    bool armed = false;
    int last_delta = (int)parameters[TA_VOCAL_ENTRY_LONG_RESUME_LAST_DELTA];
    int marker = (int)parameters[TA_VOCAL_ENTRY_LONG_MARKER_OFFSET];
    for (int i = 0; i < limit; i++) {
        if (!mask.v[i]) {
            if (silence > parameters[TA_VOCAL_ENTRY_LONG_SILENCE_COUNT])
                armed = true;
            silence++;
        } else {
            if (armed && vocal_resume_count(mask, i, last_delta) >= parameters[TA_VOCAL_ENTRY_LONG_RESUME_COUNT])
                selected.v[i + marker] = 1;
            armed = false;
            silence = 0;
        }
    }
}

static bool short_entry_ready(B mask, D normalized, int i, bool longSilence,
                              const double *parameters)
{
    int last_delta = (int)parameters[TA_VOCAL_ENTRY_SHORT_RESUME_LAST_DELTA];
    return !longSilence && vocal_resume_count(mask, i, last_delta) >= parameters[TA_VOCAL_ENTRY_SHORT_RESUME_COUNT]
           && normalized.v[i] >= parameters[TA_VOCAL_ENTRY_SHORT_RESUME_LEVEL];
}
static bool vocal_entry_bar_rise(D ratios, int i, const double *parameters)
{
    int bar = i / (int)parameters[TA_VOCAL_ENTRY_BEATS_PER_BAR];
    int next = bar + (int)parameters[TA_VOCAL_ENTRY_SHORT_NEXT_BAR];
    return ratios.v[bar] >= parameters[TA_VOCAL_ENTRY_SAME_BAR_RISE_MIN] ||
           ratios.v[next] >= parameters[TA_VOCAL_ENTRY_NEXT_BAR_RISE_MIN];
}
static void short_silence_entries(B mask, B selected, D normalized, D ratios, int count,
                                  const double *parameters)
{
    int silence = 0;
    bool armed = false;
    bool longSilence = false;
    int limit = count - (int)parameters[TA_VOCAL_ENTRY_SHORT_SCAN_TAIL];
    int marker = (int)parameters[TA_VOCAL_ENTRY_SHORT_MARKER_OFFSET];
    for (int i = 0; i < limit; i++) {
        if (!mask.v[i]) {
            if (silence > parameters[TA_VOCAL_ENTRY_LONG_SILENCE_COUNT]) {
                armed = false;
                longSilence = true;
            } else if (silence > parameters[TA_VOCAL_ENTRY_SHORT_SILENCE_COUNT])
                armed = true;
            silence++;
        } else {
            if (armed && short_entry_ready(mask, normalized, i, longSilence, parameters) &&
                vocal_entry_bar_rise(ratios, i, parameters))
                selected.v[i + marker] = 1;
            silence = 0;
            armed = false;
            longSilence = false;
        }
    }
}

static int prepare_vocal_entries(ta_context *c, D normalized, D sums, D ratios, B mask, int limit,
                                 const double *parameters)
{
    int active = 0;
    for (int i = 0; i < limit; i++) {
        mask.v[i] = normalized.v[i] > 0;
        active += mask.v[i];
    }
    int width = (int)parameters[TA_VOCAL_ENTRY_BEATS_PER_BAR];
    int aggregate_end = sums.n - (int)parameters[TA_VOCAL_ENTRY_SUM_TAIL_BARS];
    for (int i = 0; i < aggregate_end; i++)
        sums.v[i] = d_sequential(d_view(c, normalized, i * width, (i + 1) * width));
    for (int i = (int)parameters[TA_VOCAL_ENTRY_FIRST_RATIO_BAR]; i < aggregate_end; i++)
        ratios.v[i] = sums.v[i - 1] == 0 ? 0 : sums.v[i] / sums.v[i - 1];
    return active;
}
I vocal_entries(ta_context *c, D activity, const double *parameters)
{
    I entries = ta_new_ints(c, activity.n);
    entries.n = 0;
    if (!activity.n)
        return entries;
    D normalized = d_normalized(c, activity, parameters[TA_VOCAL_ENTRY_NORMALIZE_ZERO]);
    int width = (int)parameters[TA_VOCAL_ENTRY_BEATS_PER_BAR];
    D sums = ta_new_doubles(c, activity.n / width), ratios = ta_new_doubles(c, sums.n);
    B mask = ta_new_bytes(c, activity.n), selected = ta_new_bytes(c, activity.n + 1);
    int limit = ta_max(0, activity.n - (int)parameters[TA_VOCAL_ENTRY_LONG_SCAN_TAIL]);
    int active = prepare_vocal_entries(c, normalized, sums, ratios, mask, limit, parameters);
    long_silence_entries(mask, selected, limit, parameters);
    double density = limit ? (double)active / limit : 0;
    if (activity.n > parameters[TA_VOCAL_ENTRY_SHORT_TRACK_CUTOFF] &&
        density < parameters[TA_VOCAL_ENTRY_SHORT_DENSITY_MAX]) {
        short_silence_entries(mask, selected, normalized, ratios, activity.n, parameters);
    }
    int first_marker = ta_min((int)parameters[TA_VOCAL_ENTRY_LONG_MARKER_OFFSET],
                              (int)parameters[TA_VOCAL_ENTRY_SHORT_MARKER_OFFSET]);
    int first_bar = (int)parameters[TA_VOCAL_ENTRY_OUTPUT_FIRST_BAR];
    for (int i = first_marker; i < selected.n; i++)
        if (selected.v[i])
            entries.v[entries.n++] = i / width + first_bar;
    ta_release(c, normalized.v);
    ta_release(c, sums.v);
    ta_release(c, ratios.v);
    ta_release(c, mask.v);
    ta_release(c, selected.v);
    return entries;
}

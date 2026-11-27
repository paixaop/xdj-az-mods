/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azanalysis_policy.h"
#include "phrase.h"

bool phrase_reduction_uses_windows(const double *order, int width)
{
    return width == TA_WINDOWS_REDUCTION_LANES && order[TA_WINDOWS_REDUCTION_PAIRS / 2] < 0.0;
}

static unsigned windows_pair(const double *order, int index)
{
    /* The final packed pair also marks the Windows representation in memory. */
    if (index == TA_WINDOWS_REDUCTION_PAIRS - 1)
        return (unsigned)(-1.0 - order[TA_WINDOWS_REDUCTION_PAIRS / 2]);
    unsigned word = (unsigned)order[index / 2];
    return (word >> ((index % 2) * 8)) & UINT8_MAX;
}

static double reduce_windows_pairs(const double *lanes, const double *order)
{
    double nodes[TA_WINDOWS_REDUCTION_LANES + TA_WINDOWS_REDUCTION_PAIRS];
    memcpy(nodes, lanes, TA_WINDOWS_REDUCTION_LANES * sizeof *nodes);
    for (int index = 0; index < TA_WINDOWS_REDUCTION_PAIRS; ++index) {
        unsigned pair = windows_pair(order, index);
        nodes[TA_WINDOWS_REDUCTION_LANES + index] = nodes[pair & 0x0fu] + nodes[pair >> 4];
    }
    return nodes[TA_WINDOWS_REDUCTION_LANES + TA_WINDOWS_REDUCTION_PAIRS - 1];
}

double phrase_reduce_pairs(const double *lanes, const double *order, int width)
{
    if (phrase_reduction_uses_windows(order, width))
        return reduce_windows_pairs(lanes, order);
    int first = (int)order[0] * 2, second = (int)order[1] * 2;
    double even = lanes[first] + lanes[second];
    double odd = lanes[first + 1] + lanes[second + 1];
    for (int pair = 2; pair < width / 2; pair++) {
        int lane = (int)order[pair] * 2;
        even = lanes[lane] + even;
        odd = lanes[lane + 1] + odd;
    }
    return even + odd;
}

double d_sum(D a, const double *parameters)
{
    int width = (int)parameters[TA_PHRASE_SUM_GROUP], mask = width - 1;
    int end = a.n & ~mask;
    if (!end)
        return d_sequential(a);
    double lanes[TA_PHRASE_SUM_PAIR_CAPACITY * 2] = {0};
    for (int i = 0; i < end; i++)
        lanes[i & mask] += a.v[i];
    const double *order = parameters + TA_PHRASE_SUM_PAIR_0;
    double total = phrase_reduce_pairs(lanes, order, width);
    if (phrase_reduction_uses_windows(order, width))
        total = 0.0 + total;
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
D d_normalized(ta_context *c, D a, double zero_scale)
{
    D b = d_copy(c, a);
    double m = d_max(a), scale = m == 0 ? zero_scale : 1 / m;
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
    ta_phrase_biquad (*sections)[TA_DECIMATE_SECTION_CAPACITY];
    D **bands;
    const double *pending;
    const int *divisors;
    int filled, section_count;
} decimator_job;
static void decimate_band(ta_context *c, void *data, size_t band, unsigned worker)
{
    (void)worker;
    decimator_job *job = data;
    D *output = job->bands[band];
    ta_check(c);
    ta_phrase_decimate(job->sections[band], (size_t)job->section_count, job->pending, output->v + output->n,
                       (size_t)job->filled, (size_t)job->divisors[band]);
    output->n += job->filled / job->divisors[band];
}
static void decode_bands(ta_context *c, uint64_t frames, int block, int16_t *pcm, double *pending,
                         ta_phrase_biquad sections[][TA_DECIMATE_SECTION_CAPACITY], D **bands, const int *divisors,
                         const double *parameters)
{
    int received = 0, filled = 0;
    int section_count = (int)parameters[TA_DECIMATE_FILTER_SECTIONS];
    double left = parameters[TA_PCM_LEFT_SCALE], right = parameters[TA_PCM_RIGHT_SCALE];
    while (received < (int)frames) {
        ta_check(c);
        int need = ta_min(16384, ta_min(block - filled, (int)frames - received));
        int64_t n = ta_decode_read(c->decoder, pcm, need);
        if (n <= 0 || n > need)
            ta_fail(c, TA_FAILED, "truncated phrase decode");
        for (int i = 0; i < n; i++)
            pending[filled + i] = fma((double)pcm[i * 2 + 1], right, (double)pcm[i * 2] * left);
        filled += (int)n;
        received += (int)n;
        if (filled == block || received == (int)frames) {
            decimator_job job = { .sections = sections, .bands = bands, .pending = pending,
                                  .divisors = divisors, .filled = filled, .section_count = section_count };
            ta_parallel(c, TA_DECIMATE_BAND_COUNT, ta_phrase_threads(c), decimate_band, &job);
            filled = 0;
        }
    }
}

static void initialize_bands(ta_context *c, unsigned rate, uint64_t frames, D **bands,
                             const int *divisors, const double *parameters,
                             ta_phrase_biquad sections[][TA_DECIMATE_SECTION_CAPACITY])
{
    const double *damping = parameters + TA_DECIMATE_DAMPING_FIRST;
    int section_count = (int)parameters[TA_DECIMATE_FILTER_SECTIONS];
    for (int b = 0; b < TA_DECIMATE_BAND_COUNT; b++) {
        *bands[b] = ta_new_doubles(c, (int)(frames / divisors[b]));
        bands[b]->n = 0;
        double scale = parameters[b == 2 ? TA_DECIMATE_HIGHPASS_SCALE : TA_DECIMATE_LOWPASS_SCALE];
        double cutoff = ((double)rate / divisors[b]) * scale * (2 * M_PI);
        for (int j = 0; j < section_count; j++)
            sections[b][j] = second_order(rate, cutoff, damping[j], b == 2);
    }
}
static int phrase_frame_extent(ta_context *c, uint64_t frames, const double *parameters)
{
    unsigned trim = (unsigned)parameters[TA_PHRASE_SOURCE_FRAME_TRIM];
    unsigned minimum = (unsigned)parameters[TA_PHRASE_MIN_ADJUSTED_FRAMES];
    if (frames < trim + minimum)
        ta_fail(c, TA_FAILED, "unsupported phrase audio");
    return (int)(frames - trim);
}

static int phrase_pcm_block_frames(unsigned rate, const double *parameters)
{
    return (int)fma(rate, parameters[TA_PCM_BLOCK_SECONDS], parameters[TA_PCM_BLOCK_ROUNDING_BIAS]);
}

Samples phrase_decode(ta_context *c, const double *parameters)
{
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(c->request->audio_path, &rate, &frames);
    if (!c->decoder || frames > (uint64_t)rate * AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS ||
        !az_analysis_source_rate_supported(rate))
        ta_fail(c, TA_FAILED, "unsupported phrase audio");
    Samples s = { .rate = rate, .frames = phrase_frame_extent(c, frames, parameters) };
    D *bands[] = { &s.low, &s.middle, &s.high };
    const int divisors[] = { (int)parameters[TA_DECIMATE_LOW_DIVISOR],
                             (int)parameters[TA_DECIMATE_MIDDLE_DIVISOR],
                             (int)parameters[TA_DECIMATE_HIGH_DIVISOR] };
    ta_phrase_biquad sections[TA_DECIMATE_BAND_COUNT][TA_DECIMATE_SECTION_CAPACITY];
    int block = phrase_pcm_block_frames(rate, parameters);
    initialize_bands(c, rate, frames, bands, divisors, parameters, sections);
    int16_t *pcm = ta_alloc(c, 16384 * 2, sizeof(int16_t));
    double *pending = ta_alloc(c, block, sizeof(double));
    decode_bands(c, frames, block, pcm, pending, sections, bands, divisors, parameters);
    if (ta_decode_read(c->decoder, pcm, 1) != 0)
        ta_fail(c, TA_FAILED, "phrase frame count changed");
    ta_decode_close(c->decoder);
    c->decoder = NULL;
    ta_release(c, pcm);
    ta_release(c, pending);
    return s;
}
typedef struct {
    const double *damping;
    double lower, upper, smoothing;
    int highpass_count, lowpass_count, smoother_count;
} envelope_profile;

static void envelope_biquads(ta_phrase_biquad *sections, double rate, const envelope_profile *profile)
{
    for (int i = 0; i < profile->highpass_count; i++)
        sections[i] = second_order(rate, profile->lower, profile->damping[i], true);
    for (int i = 0; i < profile->lowpass_count; i++)
        sections[profile->highpass_count + i] = second_order(rate, profile->upper, profile->damping[i], false);
}

static void envelope_smooth_tile(ta_phrase_biquad *sections, int count,
                                 const double *source, double *output, int size)
{
    for (int i = 0; i < count; i++) {
        ta_phrase_biquad_process(sections + i, source, output, size, i == 0);
        source = output;
    }
}

static D envelope(ta_context *c, D input, double rate, envelope_profile profile)
{
    ta_phrase_biquad sections[2 * TA_ENVELOPE_DAMPING_PAIRS], smoothing[profile.smoother_count];
    int count = profile.highpass_count + profile.lowpass_count;
    envelope_biquads(sections, rate, &profile);
    for (int i = 0; i < profile.smoother_count; i++)
        smoothing[i] = smoother(rate, profile.smoothing);
    D out = ta_new_doubles(c, input.n);
    for (int start = 0; start < input.n; start += 16384) {
        ta_check(c);
        int size = ta_min(16384, input.n - start);
        const double *source = input.v + start;
        for (int i = 0; i < count; i++) {
            ta_phrase_biquad_process(sections + i, source, out.v + start, size, 0);
            source = out.v + start;
        }
        envelope_smooth_tile(smoothing, profile.smoother_count, source, out.v + start, size);
    }
    return out;
}
Envelopes phrase_envelopes(ta_context *c, Samples s, const double *parameters)
{
    const double *second = parameters + TA_ENVELOPE_DAMPING_SECOND_ORDER;
    const double *fourth = parameters + TA_ENVELOPE_DAMPING_FOURTH_ORDER_FIRST;
    double low_rate = s.rate * parameters[TA_DECIMATE_LOW_RATE_SCALE];
    double mid_rate = s.rate * parameters[TA_DECIMATE_MIDDLE_RATE_SCALE];
    double high_rate = s.rate * parameters[TA_DECIMATE_HIGH_RATE_SCALE];
    Envelopes e;
    e.low = envelope(c, s.low, low_rate, (envelope_profile){
        .damping = second, .upper = parameters[TA_ENVELOPE_LOW_CUTOFF],
        .smoothing = parameters[TA_ENVELOPE_LOW_SMOOTH],
        .lowpass_count = (int)parameters[TA_ENVELOPE_LOW_LOWPASS_SECTIONS],
        .smoother_count = (int)parameters[TA_ENVELOPE_LOW_SMOOTH_SECTIONS],
    });
    e.middle = envelope(c, s.middle, mid_rate, (envelope_profile){
        .damping = fourth, .lower = parameters[TA_ENVELOPE_MIDDLE_LOWER],
        .upper = parameters[TA_ENVELOPE_MIDDLE_UPPER], .smoothing = parameters[TA_ENVELOPE_MIDDLE_SMOOTH],
        .highpass_count = (int)parameters[TA_ENVELOPE_MIDDLE_HIGHPASS_SECTIONS],
        .lowpass_count = (int)parameters[TA_ENVELOPE_MIDDLE_LOWPASS_SECTIONS],
        .smoother_count = (int)parameters[TA_ENVELOPE_MIDDLE_SMOOTH_SECTIONS],
    });
    e.high = envelope(c, s.high, high_rate, (envelope_profile){
        .damping = fourth, .lower = parameters[TA_ENVELOPE_HIGH_LOWER],
        .upper = parameters[TA_ENVELOPE_HIGH_UPPER], .smoothing = parameters[TA_ENVELOPE_HIGH_SMOOTH],
        .highpass_count = (int)parameters[TA_ENVELOPE_HIGH_HIGHPASS_SECTIONS],
        .lowpass_count = (int)parameters[TA_ENVELOPE_HIGH_LOWPASS_SECTIONS],
        .smoother_count = (int)parameters[TA_ENVELOPE_HIGH_SMOOTH_SECTIONS],
    });
    e.lowLevel = envelope(c, s.low, low_rate, (envelope_profile){
        .damping = second, .upper = parameters[TA_ENVELOPE_LOW_LEVEL_CUTOFF],
        .smoothing = parameters[TA_ENVELOPE_LOW_LEVEL_SMOOTH],
        .lowpass_count = (int)parameters[TA_ENVELOPE_LOW_LEVEL_LOWPASS_SECTIONS],
        .smoother_count = (int)parameters[TA_ENVELOPE_LOW_LEVEL_SMOOTH_SECTIONS],
    });
    e.middleLevel = envelope(c, s.middle, mid_rate, (envelope_profile){
        .damping = second, .lower = parameters[TA_ENVELOPE_MIDDLE_LEVEL_CUTOFF],
        .smoothing = parameters[TA_ENVELOPE_MIDDLE_LEVEL_SMOOTH],
        .highpass_count = (int)parameters[TA_ENVELOPE_MIDDLE_LEVEL_HIGHPASS_SECTIONS],
        .smoother_count = (int)parameters[TA_ENVELOPE_MIDDLE_LEVEL_SMOOTH_SECTIONS],
    });
    return e;
}
static bool valid_reference_timing(double hundred, double first, double rounded_offset, double firstBpm)
{
    return !(!isfinite(hundred) || hundred < 1 || hundred > INT32_MAX || !isfinite(first) || first < 0 ||
             !(rounded_offset <= INT_MAX) || !isfinite(firstBpm) || firstBpm <= 0);
}

static int reference_phase(int position, const double *parameters)
{
    const int count = TA_REFERENCE_TIMING_PHASE_4 - TA_REFERENCE_TIMING_PHASE_1 + 1;
    if (position < 1 || position > count)
        return (int)parameters[TA_REFERENCE_TIMING_DEFAULT_PHASE];
    return (int)parameters[TA_REFERENCE_TIMING_PHASE_1 + position - 1];
}

Timing reference_timing(ta_context *c, double bpm, double first, int position, double firstBpm,
                        const double *parameters)
{
    double unit = parameters[TA_REFERENCE_TIMING_BPM_UNIT];
    double units_per_bpm = 1 / unit;
    double hundred = round(bpm * units_per_bpm);
    double milliseconds_per_second = 1 / parameters[TA_NOMINAL_BEAT_OFFSET_SCALE];
    /* The SDK rounds its floating-point input before constructing the native offset. */
    double rounded_offset = first * milliseconds_per_second + az_analysis_offset_round_bias();
    if (!valid_reference_timing(hundred, first, rounded_offset, firstBpm))
        ta_fail(c, TA_FAILED, "invalid phrase timing");
    int offset = (int)rounded_offset, phase = reference_phase(position, parameters);
    if (round(firstBpm * units_per_bpm) != hundred) {
        double period = parameters[TA_REFERENCE_TIMING_PERIOD_NUMERATOR] / hundred;
        if (period < offset && offset < period + period) {
            offset -= (int)period;
            phase = phase == 1 ? (int)parameters[TA_REFERENCE_TIMING_ROLLBACK_PHASE] : phase - 1;
        }
    }
    return (Timing){ hundred * unit, offset, phase };
}
int nominal_beats(double bpm, int phase, int offset_ms, const double *parameters)
{
    double period = parameters[TA_NOMINAL_BEAT_PERIOD_NUMERATOR] / bpm;
    double seconds = offset_ms * parameters[TA_NOMINAL_BEAT_OFFSET_SCALE];
    double block = parameters[TA_PHRASE_BLOCK_SECONDS];
    int phase_delta = phase - (int)parameters[TA_NOMINAL_BEAT_PHASE_ORIGIN];
    int beats_per_bar = (int)parameters[TA_NOMINAL_BEATS_PER_BAR];
    return phase_delta + beats_per_bar *
           (int)((block - fma(period, (double)phase_delta, seconds)) / (period * beats_per_bar));
}
I stage_coordinates(ta_context *c, int count, double rate, double bpm, int offset,
                    const double *parameters)
{
    double period = (rate * parameters[TA_STAGE_COORD_SECONDS_PER_MINUTE]) / bpm;
    double rate_per_ms = rate * parameters[TA_STAGE_COORD_SECONDS_PER_MILLISECOND];
    int first = (int)fma(rate_per_ms, (double)offset, parameters[TA_STAGE_COORD_OFFSET_BIAS]);
    I a = ta_new_ints(c, ta_max(0, (int)((double)(count - first) / period)));
    double endpoint = (double)first + parameters[TA_STAGE_COORD_ENDPOINT_BIAS];
    for (int i = 0; i < a.n; i++)
        a.v[i] = (int)fma(period, (double)i, endpoint);
    int last = a.n - (int)parameters[TA_STAGE_COORD_CLAMP_DISTANCE];
    if (last >= 0)
        a.v[last] = ta_min(a.v[last], count);
    return a;
}
static bool valid_coordinates(Samples s, Timing t)
{
    return !(!isfinite(s.rate) || s.rate <= 0 || !isfinite(t.bpm) || t.bpm <= 0 || t.offset < 0 ||
             !az_analysis_phase_supported((unsigned)t.phase));
}

Coordinates phrase_coordinates(ta_context *c, Samples s, Timing t, const double *parameters)
{
    if (!valid_coordinates(s, t))
        ta_fail(c, TA_FAILED, "invalid phrase coordinates");
    double rates[3], counts[3];
    double periods[3];
    int offsets[3], n = INT_MAX;
    double offset_s = (double)t.offset * parameters[TA_PHRASE_COORD_SECONDS_PER_MILLISECOND];
    double beat_seconds = parameters[TA_PHRASE_COORD_SECONDS_PER_MINUTE] / t.bpm;
    for (int b = 0; b < 3; b++) {
        double scale = parameters[TA_DECIMATE_HIGH_RATE_SCALE + b];
        rates[b] = s.rate * scale;
        counts[b] = s.frames * scale;
        offsets[b] = (int)fma(offset_s, rates[b], parameters[TA_PHRASE_COORD_OFFSET_BIAS]);
        periods[b] = beat_seconds * rates[b];
        double count = (counts[b] - (double)offsets[b]) / periods[b];
        if (!isfinite(count) || count > AZ_ANALYSIS_POLICY_MAXIMUM_GRID_BEATS || count < 1)
            ta_fail(c, TA_FAILED, "phrase beat count out of range");
        n = ta_min(n, (int)count);
    }
    Coordinates out = { .n = n };
    I *indices[] = { &out.high, &out.middle, &out.low };
    int last = n - (int)parameters[TA_PHRASE_COORD_CLAMP_DISTANCE];
    int grid_count = n + (int)parameters[TA_PHRASE_COORD_GRID_TRAILING_COUNT];
    for (int b = 0; b < 3; b++) {
        *indices[b] = ta_new_ints(c, grid_count);
        double endpoint = (double)offsets[b] + parameters[TA_PHRASE_COORD_ENDPOINT_BIAS];
        for (int i = 0; i < grid_count; i++)
            indices[b]->v[i] = (int)fma(periods[b], (double)i, endpoint);
        if (last >= 0)
            indices[b]->v[last] = ta_min(indices[b]->v[last], (int)counts[b]);
    }
    return out;
}
static int first_activity_displacement(D samples, int first, int length, const double *parameters)
{
    if (length < parameters[TA_FIRST_ACTIVITY_MIN_SCAN_SAMPLES]) return 0;
    int end = length - (int)parameters[TA_FIRST_ACTIVITY_SCAN_TAIL];
    double minimum = parameters[TA_FIRST_ACTIVITY_MIN_AMPLITUDE];
    for (int i = 0; i < end; i++) {
        int index = first + i;
        if (index >= 0 && index < length && fabs(samples.v[index]) > minimum)
            return i + (int)parameters[TA_FIRST_ACTIVITY_DISPLACEMENT_BIAS];
    }
    return 0;
}

int activity_start(D samples, double rate, double bpm, int offset, const double *parameters)
{
    double period = bpm == 0 ? parameters[TA_FIRST_ACTIVITY_ZERO_BPM_PERIOD]
                             : parameters[TA_FIRST_ACTIVITY_BEAT_SECONDS] / bpm;
    double bias = parameters[TA_FIRST_ACTIVITY_COORDINATE_BIAS];
    int per = (int)fma(period, rate, bias);
    int first = (int)fma((double)offset * parameters[TA_FIRST_ACTIVITY_OFFSET_SCALE], rate, bias);
    int length = ta_min(samples.n, (int)(rate * parameters[TA_FIRST_ACTIVITY_SCAN_SECONDS]));
    double displacement = first_activity_displacement(samples, first, length, parameters);
    double position = per == 0 ? displacement * parameters[TA_FIRST_ACTIVITY_ZERO_PERIOD_SCALE]
                               : displacement / per;
    return (int)(position + bias) + (int)parameters[TA_FIRST_ACTIVITY_RESULT_BIAS];
}
static double range_sum(D s, int start, int n, const double *parameters)
{
    if (start < 0 || n <= 0 || start + n > s.n)
        return 0;
    return d_sum((D){ s.v + start, n }, parameters);
}
static bool low_change(double prev, double next, double ratio)
{
    return (next > prev && next > prev * ratio) || (next < prev && prev > next * ratio);
}
static bool low_quiet(double prev, double next, D s, int i, const double *parameters)
{
    double level = parameters[TA_LOW_ACTIVITY_QUIET_LEVEL];
    double peak = parameters[TA_LOW_ACTIVITY_QUIET_PEAK];
    int center = (int)parameters[TA_LOW_ACTIVITY_QUIET_CENTER];
    int back = (int)parameters[TA_LOW_ACTIVITY_QUIET_BACK];
    int stride = (int)parameters[TA_LOW_ACTIVITY_QUIET_STRIDE];
    int64_t sample = (int64_t)i * stride + center;
    return next <= level && prev <= level && sample < s.n && sample + back >= 0 &&
           s.v[sample] <= peak && s.v[sample + back] <= peak;
}

static void activity_bar_sums(D output, D source, int start, int stride, int width, int count,
                              const double *parameters)
{
    for (int i = 0; i < count; i++) {
        int64_t first = start + (int64_t)i * stride;
        if (first >= 0 && first <= source.n - width)
            output.v[i] = range_sum(source, (int)first, width, parameters);
    }
}

static void low_activity_changes(Activity a, D s, const double *parameters)
{
    int minimum = (int)parameters[TA_LOW_ACTIVITY_CHANGE_MIN_BARS];
    int tail = (int)parameters[TA_LOW_ACTIVITY_CHANGE_TAIL_BARS];
    int end_back = (int)parameters[TA_LOW_ACTIVITY_CHANGE_END_BACK];
    int write = (int)parameters[TA_LOW_ACTIVITY_CHANGE_WRITE_START];
    int next_level = (int)parameters[TA_LOW_ACTIVITY_CHANGE_NEXT_LEVEL];
    int count = ta_min(ta_max(minimum, a.values.n) - tail - end_back,
                       a.values.n - ta_max(write, next_level));
    for (int i = 0; i < count; i++) {
        double prev = a.values.v[i], next = a.values.v[i + next_level];
        if (low_change(prev, next, parameters[TA_LOW_ACTIVITY_CHANGE_RATIO]))
            a.changes.v[i + write] = 1;
        if (low_quiet(prev, next, s, i, parameters))
            a.changes.v[i + write] = 0;
    }
}

Activity low_activity(ta_context *c, D s, int phase, const double *parameters)
{
    int extent = s.n - phase + (int)parameters[TA_LOW_ACTIVITY_EXTENT_PAD];
    int n = ta_max(0, extent / (int)parameters[TA_LOW_ACTIVITY_BAR_DIVISOR]);
    Activity a = { ta_new_doubles(c, n), ta_new_bytes(c, n) };
    if (extent >= parameters[TA_LOW_ACTIVITY_MIN_EXTENT]) {
        int minimum = (int)parameters[TA_LOW_ACTIVITY_FILL_MIN_BARS];
        int tail = (int)parameters[TA_LOW_ACTIVITY_FILL_TAIL_BARS];
        int written = ta_min(n, ta_max(minimum, n) - tail);
        activity_bar_sums(a.values, s, phase - (int)parameters[TA_LOW_ACTIVITY_PHASE_BACK],
                          (int)parameters[TA_LOW_ACTIVITY_SUM_STRIDE],
                          (int)parameters[TA_LOW_ACTIVITY_SUM_LAST] + 1, written, parameters);
        low_activity_changes(a, s, parameters);
    }
    return a;
}
static bool middle_change(double previous, double current, const double *parameters)
{
    double zero_level = parameters[TA_MIDDLE_ACTIVITY_ZERO_LEVEL];
    double next = current == 0 ? zero_level : current;
    double prev = previous == 0 ? zero_level : previous;
    if ((next > prev && next / prev > parameters[TA_MIDDLE_ACTIVITY_RISE_RATIO]) ||
        (prev > next && prev / next > parameters[TA_MIDDLE_ACTIVITY_FALL_RATIO]))
        return 1;
    return 0;
}

Activity middle_activity(ta_context *c, D s, int phase, const double *parameters)
{
    int extent = s.n - phase + (int)parameters[TA_MIDDLE_ACTIVITY_EXTENT_PAD];
    int n = ta_max(0, extent / (int)parameters[TA_MIDDLE_ACTIVITY_BAR_DIVISOR]);
    Activity a = { ta_new_doubles(c, n), ta_new_bytes(c, n) };
    int tail = n - (int)parameters[TA_MIDDLE_ACTIVITY_TAIL_BARS];
    if (extent >= parameters[TA_MIDDLE_ACTIVITY_MIN_BAR_EXTENT]) {
        int written = ta_min(n, ta_max((int)parameters[TA_MIDDLE_ACTIVITY_FILL_MIN_BARS], tail));
        activity_bar_sums(a.values, s, phase - (int)parameters[TA_MIDDLE_ACTIVITY_PHASE_BACK],
                          (int)parameters[TA_MIDDLE_ACTIVITY_SUM_STRIDE],
                          (int)parameters[TA_MIDDLE_ACTIVITY_SUM_LAST] + 1, written, parameters);
    }
    if (extent >= parameters[TA_MIDDLE_ACTIVITY_MIN_CHANGE_EXTENT]) {
        int end = ta_min(n, ta_max((int)parameters[TA_MIDDLE_ACTIVITY_CHANGE_MIN_BARS], tail));
        for (int i = (int)parameters[TA_MIDDLE_ACTIVITY_CHANGE_START]; i < end; i++)
            a.changes.v[i] = middle_change(a.values.v[i - 1], a.values.v[i], parameters);
    }
    return a;
}
static void average_activity_bars(D bars, D levels, int start, int width, int tail, double scale, const double *parameters)
{
    int count = ta_max(1, bars.n - tail);
    for (int i = 0; i < count; i++)
        bars.v[i] = range_sum(levels, start + i * width, width, parameters) * scale;
}

AllActivity all_activity(ta_context *c, D s, int phase, const double *parameters)
{
    double reciprocal = parameters[TA_ALL_ACTIVITY_INVERSE_NUMERATOR] / d_max(s);
    double scale = reciprocal == 0 ? parameters[TA_ALL_ACTIVITY_ZERO_SCALE] : reciprocal;
    int extent = s.n - phase + (int)parameters[TA_ALL_ACTIVITY_EXTENT_PAD];
    int width = (int)parameters[TA_ALL_ACTIVITY_BEATS_PER_BAR];
    int n = ta_max(0, extent / width);
    AllActivity a = { d_copy(c, s), ta_new_doubles(c, n) };
    for (int i = 0; i < s.n; i++)
        a.normalized.v[i] *= scale;
    if (extent >= parameters[TA_ALL_ACTIVITY_MIN_EXTENT])
        average_activity_bars(a.bars, a.normalized, phase - (int)parameters[TA_ALL_ACTIVITY_PHASE_ORIGIN],
                              width, (int)parameters[TA_ALL_ACTIVITY_TAIL_BARS], parameters[TA_ALL_ACTIVITY_BAR_MEAN], parameters);
    return a;
}
AllActivity middle_peak_activity(ta_context *c, D s, int count, int phase, const double *parameters)
{
    double max = d_max(s);
    double scale = max == 0 ? parameters[TA_MIDDLE_PEAK_ZERO_SCALE]
                            : parameters[TA_MIDDLE_PEAK_INVERSE_NUMERATOR] / max;
    int extent = s.n - phase + (int)parameters[TA_MIDDLE_PEAK_EXTENT_PAD];
    int width = (int)parameters[TA_MIDDLE_PEAK_BEATS_PER_BAR];
    int n = ta_max(0, extent / width);
    AllActivity a = { d_copy(c, s), ta_new_doubles(c, n) };
    if (count >= parameters[TA_MIDDLE_PEAK_MIN_NORM_COUNT]) {
        int norm_end = ta_min(s.n, count - (int)parameters[TA_MIDDLE_PEAK_NORM_TAIL]);
        for (int i = 0; i < norm_end; i++)
            a.normalized.v[i] *= scale;
    }
    if (extent >= parameters[TA_MIDDLE_PEAK_MIN_EXTENT])
        average_activity_bars(a.bars, a.normalized, phase - (int)parameters[TA_MIDDLE_PEAK_PHASE_ORIGIN],
                              width, (int)parameters[TA_MIDDLE_PEAK_TAIL_BARS], parameters[TA_MIDDLE_PEAK_BAR_MEAN], parameters);
    return a;
}
typedef struct {
    int origin, last_back;
} beat_interval;

static beat_interval interval_geometry(const double *parameters, int origin, int last_back)
{
    return (beat_interval){(int)parameters[origin], (int)parameters[last_back]};
}

static double interval_sum(D source, int start, int end, int limit, beat_interval interval,
                           const double *parameters)
{
    if (start < interval.origin || start > limit || end - interval.last_back > limit)
        return 0;
    return range_sum(source, start - interval.origin, end - start, parameters);
}
static double interval_max(D source, int start, int end, int limit, bool absolute, beat_interval interval)
{
    int first = start - interval.origin, after_last = end - interval.origin;
    if (first < 0 || start > limit || end - interval.last_back > limit || end <= start || after_last > source.n)
        return 0;
    double value = absolute ? fabs(source.v[first]) : source.v[first];
    for (int i = first + 1; i < after_last; i++)
        value = ta_dmax(value, absolute ? fabs(source.v[i]) : source.v[i]);
    return value;
}
typedef struct {
    int low_level, low_peak, high_peak, eighth;
} BeatBandLengths;

static BeatBandLengths beat_band_lengths(double rate, double bpm, const double *parameters)
{
    double low_rate = rate * parameters[TA_DECIMATE_LOW_RATE_SCALE];
    double high_rate = rate * parameters[TA_DECIMATE_HIGH_RATE_SCALE];
    double high_period = high_rate * parameters[TA_BEAT_HIGH_PEAK_PERIOD_NUMERATOR] / bpm;
    double round = parameters[TA_BEAT_HIGH_EIGHTH_ROUND_BIAS];
    double rounded = trunc(high_period + round);
    return (BeatBandLengths){
        .low_level = (int)(low_rate * parameters[TA_BEAT_LOW_LEVEL_PERIOD_NUMERATOR] / bpm),
        .low_peak = (int)(low_rate * parameters[TA_BEAT_LOW_PEAK_PERIOD_NUMERATOR] / bpm),
        .high_peak = (int)high_period,
        .eighth = (int)fma(rounded, parameters[TA_BEAT_HIGH_EIGHTH_FRACTION], round),
    };
}

static void low_beat(Beats *b, Envelopes e, Coordinates pos, BeatBandLengths bands,
                     int beat, int last, int lowLimit, const double *parameters)
{
    int start = pos.low.v[beat];
    int end = pos.low.v[beat + (int)parameters[TA_BEAT_LOW_GRID_ADVANCE]], levelEnd = end;
    if (beat == last - (int)parameters[TA_BEAT_LOW_PEAK_LAST_BEAT_BACK])
        end = start + bands.low_peak;
    if (beat == last - (int)parameters[TA_BEAT_LOW_LEVEL_LAST_BEAT_BACK]) {
        levelEnd = start + bands.low_level;
        if (levelEnd - (int)parameters[TA_BEAT_LOW_LEVEL_CLAMP_BACK] > lowLimit)
            levelEnd = lowLimit;
    }
    beat_interval level = interval_geometry(parameters, TA_BEAT_LOW_LEVEL_SAMPLE_ORIGIN, TA_BEAT_LOW_LEVEL_LAST_SAMPLE_BACK);
    beat_interval peak = interval_geometry(parameters, TA_BEAT_LOW_PEAK_SAMPLE_ORIGIN, TA_BEAT_LOW_PEAK_LAST_SAMPLE_BACK);
    b->lowLevels.v[beat] = interval_sum(e.lowLevel, start, levelEnd, lowLimit, level, parameters) *
                          parameters[TA_BEAT_LOW_LEVEL_GAIN];
    b->lowPeaks.v[beat] = interval_max(e.low, start, end, lowLimit, false, peak);
    int slots = (int)parameters[TA_BEAT_LOW_QUARTER_COUNT];
    int quarter = (int)fma((double)(end - start), parameters[TA_BEAT_LOW_QUARTER_FRACTION],
                            parameters[TA_BEAT_LOW_QUARTER_ROUND_BIAS]);
    for (int part = 0; part < slots; part++) {
        beat_interval window = interval_geometry(parameters, TA_BEAT_QUARTER_SAMPLE_ORIGIN,
                                                 TA_BEAT_QUARTER_LAST_SAMPLE_BACK_0 + part);
        b->quarters.v[beat * slots + part] =
            interval_max(e.low, start + part * quarter,
                         part + 1 == slots ? end : start + (part + 1) * quarter, lowLimit, false, window);
    }
    b->quarterMean.v[beat] = range_sum(b->quarters, beat * slots, slots, parameters) / slots;
}
static void middle_beat(Beats *b, Envelopes e, Coordinates pos, Samples s, int beat, int midLimit, const double *parameters)
{
    int start = pos.middle.v[beat];
    int end = pos.middle.v[beat + (int)parameters[TA_BEAT_MIDDLE_GRID_ADVANCE]];
    beat_interval level = interval_geometry(parameters, TA_BEAT_MIDDLE_LEVEL_SAMPLE_ORIGIN,
                                            TA_BEAT_MIDDLE_LEVEL_LAST_SAMPLE_BACK);
    beat_interval peak = interval_geometry(parameters, TA_BEAT_MIDDLE_PEAK_SAMPLE_ORIGIN,
                                           TA_BEAT_MIDDLE_PEAK_LAST_SAMPLE_BACK);
    beat_interval all = interval_geometry(parameters, TA_BEAT_ALL_LEVEL_SAMPLE_ORIGIN,
                                          TA_BEAT_ALL_LEVEL_LAST_SAMPLE_BACK);
    b->middleLevels.v[beat] = interval_sum(e.middleLevel, start, end, midLimit, level, parameters);
    b->middlePeaks.v[beat] = interval_max(e.middle, start, end, midLimit, false, peak);
    b->allLevels.v[beat] = interval_max(s.middle, start, end, midLimit, true, all);
}
static void high_beat(Beats *b, Envelopes e, Coordinates pos, BeatBandLengths bands,
                      int beat, int last, int highLimit, const double *parameters)
{
    int start = pos.high.v[beat];
    int proposed = start + bands.high_peak;
    beat_interval peak = interval_geometry(parameters, TA_BEAT_HIGH_PEAK_SAMPLE_ORIGIN,
                                           TA_BEAT_HIGH_PEAK_LAST_SAMPLE_BACK);
    int end = beat == last - (int)parameters[TA_BEAT_HIGH_PEAK_LAST_BEAT_BACK]
                  ? (proposed - peak.last_back <= highLimit ? proposed : highLimit)
                  : pos.high.v[beat + (int)parameters[TA_BEAT_HIGH_GRID_ADVANCE]];
    int slots = (int)parameters[TA_BEAT_HIGH_EIGHTH_COUNT];
    int advance = (int)parameters[TA_BEAT_HIGH_INTERIOR_ADVANCE];
    for (int part = 0; part < slots; part++)
        b->eighths.v[beat * slots + part] = interval_max(
            e.high, part ? start + part * bands.eighth + advance : start,
            part + 1 == slots ? end : start + (part + 1) * bands.eighth + advance, highLimit, false, peak);
}

static void collect_beat_bands(ta_context *c, Beats *b, Envelopes e, Samples s, Timing t, B starts,
                               const double *parameters)
{
    Coordinates pos = b->coordinates;
    int n = pos.n;
    BeatBandLengths bands = beat_band_lengths(s.rate, t.bpm, parameters);
    for (int first = 0; first < n;) {
        ta_check(c);
        starts.v[first] = 1;
        int phase = first ? (int)parameters[TA_RHYTHM_BLOCK_RESET_PHASE] : t.phase;
        int offset = first ? (int)parameters[TA_BEAT_BLOCK_RESET_OFFSET_MS] : t.offset;
        int nominal = nominal_beats(t.bpm, phase, offset, parameters);
        if (nominal <= 0)
            ta_fail(c, TA_FAILED, "invalid phrase block timing");
        int last = ta_min(n, first + nominal);
        bool final = first + nominal > n;
        int lowLimit = final ? s.low.n : pos.low.v[last] - (int)parameters[TA_BEAT_BLOCK_LOW_SAMPLE_BACK],
            midLimit = final ? s.middle.n : pos.middle.v[last] - (int)parameters[TA_BEAT_BLOCK_MIDDLE_SAMPLE_BACK],
            highLimit = final ? s.high.n : pos.high.v[last] - (int)parameters[TA_BEAT_BLOCK_HIGH_SAMPLE_BACK];
        for (int beat = first; beat < last; beat++) {
            low_beat(b, e, pos, bands, beat, last, lowLimit, parameters);
            middle_beat(b, e, pos, s, beat, midLimit, parameters);
            high_beat(b, e, pos, bands, beat, last, highLimit, parameters);
        }
        first = last;
    }
}

static double low_peak_change(double prev, double next, bool block_start, const double *parameters)
{
    double value = 0;
    if (block_start && prev == 0)
        return 0;
    if (next > prev && next > prev * parameters[TA_LOW_PEAK_RISE_RATIO])
        value = parameters[TA_LOW_PEAK_RISE_LABEL];
    if (next < prev && prev > next * parameters[TA_LOW_PEAK_FALL_FACTOR])
        value = parameters[TA_LOW_PEAK_FALL_LABEL];
    double quiet = parameters[TA_LOW_PEAK_QUIET_LEVEL];
    if (next <= quiet && prev <= quiet)
        value *= parameters[TA_LOW_PEAK_QUIET_SCALE];
    return value;
}

Beats phrase_beats(ta_context *c, Samples s, Timing t, const double *parameters)
{
    Coordinates pos = phrase_coordinates(c, s, t, parameters);
    Envelopes e = phrase_envelopes(c, s, parameters);
    int n = pos.n;
    Beats b = { .timing = t,
                .coordinates = pos,
                .initial = activity_start(s.middle, s.rate * parameters[TA_DECIMATE_MIDDLE_RATE_SCALE],
                                          t.bpm, t.offset, parameters) };
    b.lowLevels = ta_new_doubles(c, n);
    b.middleLevels = ta_new_doubles(c, n);
    b.allLevels = ta_new_doubles(c, n);
    b.lowPeaks = ta_new_doubles(c, n);
    b.lowPeakChanges = ta_new_doubles(c, n);
    b.quarterMean = ta_new_doubles(c, n);
    b.middlePeaks = ta_new_doubles(c, n);
    b.quarters = ta_new_doubles(c, n * (int)parameters[TA_BEAT_LOW_QUARTER_COUNT]);
    b.eighths = ta_new_doubles(c, n * (int)parameters[TA_BEAT_HIGH_EIGHTH_COUNT]);
    B starts = ta_new_bytes(c, n);
    collect_beat_bands(c, &b, e, s, t, starts, parameters);
    for (int beat = 1; beat < n; beat++) {
        b.lowPeakChanges.v[beat] = low_peak_change(b.lowPeaks.v[beat - 1], b.lowPeaks.v[beat], starts.v[beat], parameters);
    }
    ta_release(c, starts.v);
    ta_release(c, e.low.v);
    ta_release(c, e.middle.v);
    ta_release(c, e.high.v);
    ta_release(c, e.lowLevel.v);
    ta_release(c, e.middleLevel.v);
    b.lowActivity = low_activity(c, b.lowLevels, t.phase, parameters);
    b.middleActivity = middle_activity(c, b.middleLevels, t.phase, parameters);
    b.allActivity = all_activity(c, b.allLevels, t.phase, parameters);
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

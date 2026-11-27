/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static void power_quarters(ta_context *c, D source, Power p, int beat, int start, int end, int quarter,
                           int slots)
{
    int base = beat * slots;
    for (int part = 0; part < slots; part++) {
        int first = start + quarter * part, last = part + 1 == slots ? end : first + quarter;
        if (first < 0 || last < first || last > source.n)
            continue;
        p.quarters.v[base + part] = d_square(d_view(c, source, first, last));
        double peak = 0;
        for (int i = first; i < last; i++)
            peak = ta_dmax(peak, fabs(source.v[i]));
        p.peaks.v[base + part] = peak;
    }
}

static void scale_low_power(Power p, double rate, const double *parameters)
{
    if (!(rate > parameters[TA_RHYTHM_POWER_UNSCALED_RATE_MIN] &&
          rate < parameters[TA_RHYTHM_POWER_UNSCALED_RATE_MAX]))
        for (int i = 0; i < p.quarters.n; i++)
            p.quarters.v[i] *= parameters[TA_RHYTHM_POWER_SCALE];
}

Power rhythm_power(ta_context *c, ta_phrase_biquad *section, D input, double rate, double bpm,
                   I coordinates, int n, bool low, const double *parameters)
{
    D source = section ? filter(c, section, 1, input, false) : input;
    double period = rate * parameters[TA_RHYTHM_POWER_PERIOD_NUMERATOR] / bpm;
    double bias = parameters[TA_RHYTHM_POWER_ROUND_BIAS];
    int quarter = (int)fma(period, parameters[TA_RHYTHM_POWER_QUARTER_FRACTION], bias);
    int next_margin = (int)parameters[low ? TA_RHYTHM_POWER_LOW_NEXT_MARGIN : TA_RHYTHM_POWER_FULL_NEXT_MARGIN];
    int width = (int)(period + bias) - (int)parameters[TA_RHYTHM_POWER_TERMINAL_BACK];
    int origin = (int)parameters[TA_RHYTHM_POWER_SAMPLE_BACK_BYTES] / (int)sizeof(double);
    int slots = (int)parameters[TA_RHYTHM_BLOCK_POWER_SLOTS];
    int bar_slots = (int)parameters[TA_RHYTHM_BLOCK_ATTACK_SLOTS];
    int bars = (int)(n * parameters[TA_RHYTHM_BLOCK_BEAT_TO_BAR_SCALE]);
    Power p = { ta_new_doubles(c, n * slots), ta_new_doubles(c, n * slots), ta_new_doubles(c, bars) };
    for (int beat = 0; beat < n; beat++) {
        int start = coordinates.v[beat];
        int end = beat + next_margin < n ? coordinates.v[beat + 1] : start + width;
        power_quarters(c, source, p, beat, start - origin, end - origin, quarter, slots);
    }
    if (low) scale_low_power(p, rate, parameters);
    for (int bar = 0; bar < p.bars.n; bar++)
        p.bars.v[bar] = d_sequential(d_view(c, p.quarters, bar * bar_slots, (bar + 1) * bar_slots));
    if (section)
        ta_release(c, source.v);
    return p;
}
static bool attack_rate_in_ranges(double rate, const double *parameters, int first, int end)
{
    for (int i = first; i < end; i += 2)
        if (rate > parameters[i] && rate < parameters[i + 1])
            return true;
    return false;
}

Attacks attacks_init(ta_context *c, double rate, bool second, const double *parameters)
{
    const double damping[] = { parameters[TA_ATTACK_BUTTERWORTH_FIRST],
                               parameters[TA_ATTACK_BUTTERWORTH_SECOND] };
    double smooth_damping = parameters[TA_ATTACK_SMOOTH_DAMPING];
    const size_t damping_count = sizeof damping / sizeof *damping;
    bool full = attack_rate_in_ranges(rate, parameters, TA_ATTACK_FULL_FIRST_RATE_MIN,
                                      TA_ATTACK_REDUCED_FIRST_RATE_MIN);
    if (!full && !attack_rate_in_ranges(rate, parameters, TA_ATTACK_REDUCED_FIRST_RATE_MIN,
                                       TA_ATTACK_FULL_FILTER_COUNT))
        ta_fail(c, TA_FAILED, "unsupported rhythm rate");
    Attacks a = { .lowCount = full ? (int)parameters[TA_ATTACK_FULL_FILTER_COUNT] : 1, .second = second,
                  .maximum = parameters[TA_ATTACK_INITIAL_MAXIMUM], .parameters = parameters };
    for (int i = 0; i < a.lowCount; i++)
        a.low[i] =
            second_order(rate, parameters[TA_ATTACK_LOW_PASS_CUTOFF],
                         full ? damping[i % damping_count] : smooth_damping, false);
    for (size_t i = 0; i < sizeof a.high / sizeof *a.high; i++)
        a.high[i] = second_order(rate, parameters[TA_ATTACK_HIGH_PASS_CUTOFF], damping[i % damping_count], true);
    a.delaySmoother = second_order(rate, parameters[TA_ATTACK_SMOOTH_CUTOFF], smooth_damping, false);
    a.bandSmoother = a.delaySmoother;
    return a;
}
typedef struct {
    D envelope;
    int start, base, length, barLength, quarter;
    double period;
    bool ascending;
    const double *parameters;
} delay_search;

typedef struct {
    int position;
    double value;
} delay_peak;

typedef struct {
    bool found;
    int run, position;
    double value;
} delay_valley;

static int largest_index(D values)
{
    int index = 0;
    for (int i = 0; i < values.n; i++)
        if (values.v[i] > values.v[index]) index = i;
    return index;
}

static int delay_region_first(const delay_search *search, int position)
{
    return position - (int)search->parameters[TA_ATTACK_DELAY_CANDIDATE_BACK];
}

static int delay_region_width(const delay_search *search)
{
    return search->barLength + (int)search->parameters[TA_ATTACK_DELAY_REGION_EXTRA];
}

static bool delay_candidate_valid(const delay_search *search, int boundary, int position, double maximum)
{
    int first = delay_region_first(search, position);
    return first >= 0 && delay_region_width(search) <= search->envelope.n - first &&
           boundary < search->length - search->barLength &&
           search->envelope.v[first] > maximum * search->parameters[TA_ATTACK_DELAY_CANDIDATE_RATIO];
}

static bool delay_trend(D envelope, int first, int last, bool rising)
{
    for (int i = first; i < last; i++) {
        double left = envelope.v[i], right = envelope.v[i + 1];
        if (!(rising ? left <= right : left > right))
            return false;
    }
    return true;
}

static void set_delay_peak(const delay_search *search, int position, delay_peak *peak)
{
    int back = (int)search->parameters[TA_ATTACK_DELAY_PEAK_REFERENCE_BACK];
    int index = search->base - back + position;
    if (index >= 0 && index < search->envelope.n)
        *peak = (delay_peak){ position, search->envelope.v[index] };
}

static void rising_peak(const delay_search *search, int boundary, int position, delay_peak *peak)
{
    D envelope = search->envelope;
    int delta = (int)search->parameters[TA_ATTACK_DELAY_FORWARD_PROBE_DELTA];
    int stride = (int)search->parameters[TA_ATTACK_DELAY_FORWARD_STEP];
    for (int step = 0; step < search->quarter; step += stride) {
        int first = position + step, next = first + delta;
        if (next < envelope.n && envelope.v[next] - envelope.v[first] < 0) {
            set_delay_peak(search, boundary + step + stride, peak);
            break;
        }
    }
}

static void falling_peak(const delay_search *search, int boundary, int position, delay_peak *peak)
{
    D envelope = search->envelope;
    int delta = (int)search->parameters[TA_ATTACK_DELAY_BACKWARD_PAIR_FIRST];
    int stride = (int)search->parameters[TA_ATTACK_DELAY_BACKWARD_STEP];
    int bias = (int)search->parameters[TA_ATTACK_DELAY_BACKWARD_RESULT_BIAS];
    for (int step = 0; step < search->quarter; step += stride) {
        int first = position + delta - step;
        if (first >= 0 && envelope.v[first] - envelope.v[first + 1] < 0) {
            set_delay_peak(search, boundary - step - bias, peak);
            break;
        }
    }
}

static bool locate_delay_peak(const delay_search *search, int boundary, int position, delay_peak *peak)
{
    int first = delay_region_first(search, position);
    int last = position + (int)search->parameters[TA_ATTACK_DELAY_TREND_LAST];
    if (first < 0 || last >= search->envelope.n) return false;
    if (delay_trend(search->envelope, first, last, true)) rising_peak(search, boundary, position, peak);
    else if (!search->ascending && delay_trend(search->envelope, first, last, false))
        falling_peak(search, boundary, position, peak);
    else return false;
    return true;
}

/* 1 accepts the search (including a silent valley); -1 rejects this candidate. */
static int advance_valley(const delay_search *search, delay_valley *valley, double value,
                          delay_peak peak, int boundary, int *result)
{
    if (value <= valley->value * search->parameters[TA_ATTACK_DELAY_VALLEY_RATIO]) {
        if (++valley->run >= (int)(search->period * search->parameters[TA_ATTACK_DELAY_VALLEY_BEATS])) {
            if (valley->value == 0) return 1;
            if (peak.value > valley->value * search->parameters[TA_ATTACK_DELAY_RECOVERY_RATIO]) {
                double bias = search->parameters[TA_ATTACK_DELAY_RESULT_ROUND_BIAS];
                *result = ta_min(search->start, (int)((double)boundary + bias - (double)valley->position));
                return 1;
            }
            return -1;
        }
    } else {
        valley->found = false;
        valley->run = 0;
    }
    return 0;
}

static void observe_valley(double level, double previous, int relative, double threshold, delay_valley *valley)
{
    if (level <= previous && level < threshold) {
        valley->found = true;
        valley->value = level;
        valley->position = relative;
    }
}

static bool locate_delay_valley(ta_context *c, const delay_search *search, int boundary, int position,
                                double maximum, delay_peak peak, int *result)
{
    D envelope = search->envelope;
    const double *parameters = search->parameters;
    int first = delay_region_first(search, position);
    double minimum = d_min(d_view(c, envelope, first, first + delay_region_width(search)));
    double threshold = (minimum + maximum) * parameters[TA_ATTACK_DELAY_VALLEY_MIDPOINT_SCALE];
    int initial_step = (int)parameters[TA_ATTACK_DELAY_FIRST_VALLEY_STEP];
    int final_step = (int)(search->period * parameters[TA_ATTACK_DELAY_MAX_BEATS]);
    int origin = search->base + peak.position;
    int index = origin - (int)parameters[TA_ATTACK_DELAY_VALLEY_FIRST_BACK];
    int previous = origin - (int)parameters[TA_ATTACK_DELAY_VALLEY_PREVIOUS_BACK];
    delay_valley valley = { .value = maximum };
    for (int step = initial_step; step <= final_step; step++, index--, previous--) {
        if (previous < 0 || index >= envelope.n) break;
        if (valley.found) {
            int status = advance_valley(search, &valley, envelope.v[index], peak, boundary, result);
            if (status > 0) return true;
            if (status < 0) break;
        } else observe_valley(envelope.v[index], envelope.v[previous], peak.position - step, threshold, &valley);
    }
    return false;
}

static int search_delay(ta_context *c, const delay_search *window, D search)
{
    delay_peak peak = { 0 };
    int result = 0;
    int trials = (int)window->parameters[TA_ATTACK_DELAY_TRIAL_LIMIT];
    double bar_scale = window->parameters[TA_ATTACK_DELAY_BAR_POSITION_SCALE];
    double inverse = window->parameters[TA_ATTACK_DELAY_PERIOD_INVERSE_NUMERATOR] / window->period;
    for (int pass = 0; pass < trials; pass++) {
        ta_check(c);
        int maxIndex = largest_index(search);
        double maximum = search.v[maxIndex];
        search.v[maxIndex] = 0;
        int bar = (int)((double)(maxIndex + 1) * bar_scale * inverse),
            boundary = window->barLength + bar * window->barLength, position = window->base + boundary;
        if (!delay_candidate_valid(window, boundary, position, maximum)) continue;
        if (!locate_delay_peak(window, boundary, position, &peak)) continue;
        if (peak.value <= maximum * window->parameters[TA_ATTACK_DELAY_PEAK_RATIO]) continue;
        if (locate_delay_valley(c, window, boundary, position, maximum, peak, &result)) break;
    }
    return result;
}

static int attack_bar_length(double period, bool rounded, const double *parameters)
{
    int index = rounded ? TA_ATTACK_ORDINARY_BAR_PERIOD_COUNT : TA_ATTACK_GRID_BAR_PERIOD_COUNT;
    return (int)(period * parameters[index]);
}

static double attack_quarter_fraction(bool rounded, const double *parameters)
{
    return parameters[rounded ? TA_ATTACK_ORDINARY_QUARTER_FRACTION : TA_ATTACK_GRID_QUARTER_FRACTION];
}

static int attack_quarter_length(double period, bool rounded, const double *parameters)
{
    double bias = rounded ? parameters[TA_ATTACK_ORDINARY_ROUND_BIAS] : 0;
    return (int)(period * attack_quarter_fraction(rounded, parameters) + bias);
}

static int find_delay(ta_context *c, D envelope, int start, double period, int bars, bool ascending,
                      bool rounded, const double *parameters)
{
    if (bars < parameters[TA_ATTACK_DELAY_MIN_BARS]) return 0;
    int barLength = attack_bar_length(period, rounded, parameters);
    int quarter = attack_quarter_length(period, rounded, parameters);
    int base = start - (int)parameters[TA_ATTACK_COORDINATE_ORIGIN];
    int discarded = (int)parameters[TA_ATTACK_DELAY_SEARCH_DISCARD_BARS];
    int length = (bars - discarded) * barLength, first = base + discarded * barLength;
    if (first < 0 || length <= 0 || length > envelope.n - first) return 0;
    D search = d_copy(c, d_view(c, envelope, first, first + length));
    delay_search window = { envelope, start, base, length, barLength, quarter, period, ascending, parameters };
    int result = search_delay(c, &window, search);
    ta_release(c, search.v);
    return result;
}
static D attack_storage(ta_context *c, Attacks *a, D lowEnvelope, D filtered, int start, double period, int bars, int block, bool integerPeriod, int *activeDelay)
{
    int oldDelay = a->delay;
    a->delay = find_delay(c, lowEnvelope, start, period, bars, a->second, integerPeriod, a->parameters);
    *activeDelay = block <= a->parameters[TA_ATTACK_DELAY_LAST_FRESH_BLOCK] ? a->delay : oldDelay;
    if (integerPeriod && a->second)
        a->delay = oldDelay;
    D storage = lowEnvelope;
    if (a->second) {
        D band = filter(c, a->high, sizeof a->high / sizeof *a->high, filtered, false);
        storage = filter(c, &a->bandSmoother, 1, band, true);
        ta_release(c, band.v);
        ta_release(c, lowEnvelope.v);
    }
    ta_release(c, filtered.v);
    double previous = a->previous;
    if (!integerPeriod || a->second)
        a->previous = storage.v[storage.n - 1];
    for (int i = storage.n - 1; i >= 1; i--)
        storage.v[i] -= storage.v[i - 1];
    storage.v[0] -= previous;
    return storage;
}

static D attack_shifted_storage(ta_context *c, D storage, int zeros, int copy_count, int required)
{
    size_t bytes = (size_t)copy_count * sizeof(double);
    if (required <= storage.n) {
        memmove(storage.v + zeros, storage.v, bytes);
        memset(storage.v, 0, (size_t)zeros * sizeof(double));
        return storage;
    }
    D expanded = ta_new_doubles(c, required);
    memcpy(expanded.v + zeros, storage.v, bytes);
    ta_release(c, storage.v);
    return expanded;
}

static D align_attack_storage(ta_context *c, D *storage, int start, int end, int activeDelay,
                              const double *parameters)
{
    int origin = (int)parameters[TA_ATTACK_COORDINATE_ORIGIN];
    if (activeDelay < start && end - activeDelay <= storage->n)
        return d_view(c, *storage, start - origin - activeDelay, end - activeDelay);
    if (start < activeDelay && end - activeDelay - origin <= storage->n) {
        int copy_count = ta_max(0, end - activeDelay - origin);
        int zeros = activeDelay - start + origin;
        int length = ta_max(zeros + copy_count, end - start);
        *storage = attack_shifted_storage(c, *storage, zeros, copy_count, length);
    }
    return *storage;
}

typedef struct {
    double period;
    int start, bars, phase;
    bool integer_period;
} AttackWindow;

enum { ATTACK_PHASE_COUNT = TA_REFERENCE_TIMING_PHASE_4 - TA_REFERENCE_TIMING_PHASE_1 + 1,
       ATTACK_PEAK_CAPACITY = ATTACK_PHASE_COUNT * ATTACK_PHASE_COUNT };

static bool attack_bar_peaks(I coordinates, D envelope, const AttackWindow *window, int bar,
                             int quarter, double peaks[ATTACK_PEAK_CAPACITY], const double *parameters)
{
    int beats = (int)parameters[TA_ATTACK_BAR_BEAT_DIVISOR];
    int per_beat = (int)parameters[TA_ATTACK_BAR_PEAK_COUNT] / beats;
    int origin = (int)parameters[TA_ATTACK_COORDINATE_ORIGIN];
    double fraction = attack_quarter_fraction(window->integer_period, parameters);
    double bias = parameters[TA_ATTACK_GRID_PEAK_ROUND_BIAS];
    for (int beat = 0; beat < beats; beat++) {
        int local = bar * beats + beat;
        int ordinal = window->phase - origin + local;
        if (ordinal < 0 || ordinal >= coordinates.n)
            return false;
        double base = window->integer_period ? (double)local * window->period
                                             : (double)(coordinates.v[ordinal] - window->start) + bias;
        for (int part = 0; part < per_beat; part++) {
            int first = (int)(base + window->period * ((double)part * fraction));
            if (first < 0 || first + quarter > envelope.n)
                return false;
            peaks[beat * per_beat + part] = d_max((D){ envelope.v + first, quarter });
        }
    }
    return true;
}

static int attack_peak_index(const double *peaks, int count)
{
    int index = 0;
    for (int i = 1; i < count; i++)
        if (peaks[i] > peaks[index])
            index = i;
    return index;
}

static void mark_attacks(Attacks *a, double *peaks, B output, int bar)
{
    const double *parameters = a->parameters;
    int last_pass = (int)parameters[a->second ? TA_ATTACK_MARK_HIGH_LAST_PASS
                                             : TA_ATTACK_MARK_LOW_LAST_PASS];
    double bar_ratio = parameters[a->second ? TA_ATTACK_MARK_HIGH_BAR_RATIO
                                            : TA_ATTACK_MARK_LOW_BAR_RATIO];
    int slots = (int)parameters[TA_ATTACK_BAR_PEAK_COUNT];
    double maximum = d_max((D){ peaks, slots });
    for (int pass = 0; pass <= last_pass; pass++) {
        double threshold = d_max((D){ peaks, slots });
        if (threshold > parameters[TA_ATTACK_MARK_ABSOLUTE_FLOOR] &&
            threshold > maximum * bar_ratio &&
            threshold > a->maximum * parameters[TA_ATTACK_MARK_HISTORY_RATIO]) {
            int index = attack_peak_index(peaks, slots);
            output.v[bar * slots + index] = 1;
            peaks[index] = 0;
        }
    }
    a->maximum = ta_dmax(a->maximum, maximum);
}

static B empty_attack_output(ta_context *c, const double *parameters)
{
    return ta_new_bytes(c, (int)parameters[TA_ATTACK_EMPTY_WIDTH]);
}

static B empty_attacks(ta_context *c, Attacks *a, D filtered, D lowEnvelope)
{
    a->previous = lowEnvelope.n ? lowEnvelope.v[lowEnvelope.n - 1] : 0;
    ta_release(c, filtered.v);
    ta_release(c, lowEnvelope.v);
    return empty_attack_output(c, a->parameters);
}

static void reset_attack_block(Attacks *a, int block, bool integer_period)
{
    const double *parameters = a->parameters;
    bool reset = integer_period ? block < parameters[TA_ATTACK_ORDINARY_FIRST_CARRY_BLOCK]
                                : block <= parameters[TA_ATTACK_GRID_LAST_RESET_BLOCK];
    if (reset)
        a->maximum = parameters[TA_ATTACK_INITIAL_MAXIMUM];
}

static double attack_period(double rate, double bpm, bool integer_period, const double *parameters)
{
    int index = integer_period ? TA_ATTACK_ORDINARY_PERIOD_NUMERATOR : TA_ATTACK_GRID_PERIOD_NUMERATOR;
    double period = rate * parameters[index] / bpm;
    return integer_period ? trunc(period + parameters[TA_ATTACK_ORDINARY_ROUND_BIAS]) : period;
}

static int attack_offset_samples(double rate, int offset, bool integer_period, const double *parameters)
{
    int seconds = integer_period ? TA_ATTACK_ORDINARY_OFFSET_SCALE : TA_ATTACK_GRID_OFFSET_SCALE;
    int round = integer_period ? TA_ATTACK_ORDINARY_ROUND_BIAS : TA_ATTACK_GRID_OFFSET_ROUND_BIAS;
    return (int)fma(offset * parameters[seconds], rate, parameters[round]);
}

static AttackWindow attack_window(double rate, Timing timing, int frames, int n, bool integer_period,
                                   const double *parameters)
{
    double period = attack_period(rate, timing.bpm, integer_period, parameters);
    int offset = attack_offset_samples(rate, timing.offset, integer_period, parameters);
    int origin = (int)parameters[TA_ATTACK_COORDINATE_ORIGIN];
    int bias = (int)parameters[TA_ATTACK_COMPLETED_BEAT_BIAS];
    int beat_divisor = (int)parameters[TA_ATTACK_BAR_BEAT_DIVISOR];
    int start = (int)fma(timing.phase - origin, period, offset + origin);
    int bars = (n - timing.phase + bias) / beat_divisor;
    int bar_length = attack_bar_length(period, integer_period, parameters);
    if (frames < (int64_t)start - origin + (int64_t)bars * bar_length)
        bars--;
    return (AttackWindow){ .period = period, .start = start, .bars = bars, .phase = timing.phase,
                           .integer_period = integer_period };
}

static B attack_window_output(ta_context *c, Attacks *a, int frames, D filtered, D lowEnvelope,
                               AttackWindow window, int block, I coordinates, int n)
{
    const double *parameters = a->parameters;
    int activeDelay;
    D storage = attack_storage(c, a, lowEnvelope, filtered, window.start, window.period, window.bars,
                               block, window.integer_period, &activeDelay);
    int end = coordinates.v[n - 1];
    int proposed = end + (int)window.period - (int)parameters[TA_ATTACK_TERMINAL_INDEX_BACK];
    if (proposed <= frames)
        end = proposed;
    D envelope = align_attack_storage(c, &storage, window.start, end, activeDelay, parameters);
    B output = ta_new_bytes(c, window.bars * (int)parameters[TA_ATTACK_BAR_PEAK_COUNT]);
    int quarter = attack_quarter_length(window.period, window.integer_period, parameters);
    if (window.integer_period)
        a->maximum = parameters[TA_ATTACK_INITIAL_MAXIMUM];
    for (int bar = 0; bar < window.bars; bar++) {
        ta_check(c);
        double peaks[ATTACK_PEAK_CAPACITY] = { 0 };
        if (!attack_bar_peaks(coordinates, envelope, &window, bar, quarter, peaks, parameters))
            break;
        mark_attacks(a, peaks, output, bar);
    }
    ta_release(c, storage.v);
    return output;
}

B attacks_process(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset,
                  int phase, int block, I coordinates, int n, bool integerPeriod)
{
    const double *parameters = a->parameters;
    if (!input.n)
        return empty_attack_output(c, parameters);
    Timing timing = { .bpm = bpm, .offset = offset, .phase = phase };
    AttackWindow window = attack_window(rate, timing, input.n, n, integerPeriod, parameters);
    D filtered = filter(c, a->low, a->lowCount, input, false);
    D lowEnvelope = filter(c, &a->delaySmoother, 1, filtered, true);
    reset_attack_block(a, block, integerPeriod);
    if (n <= 0 || window.bars <= 0)
        return empty_attacks(c, a, filtered, lowEnvelope);
    return attack_window_output(c, a, input.n, filtered, lowEnvelope, window, block, coordinates, n);
}

B attacks_timing(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset, int phase,
                 int block)
{
    const double *parameters = a->parameters;
    int period = (int)attack_period(rate, bpm, true, parameters);
    if (period <= 0)
        return empty_attack_output(c, parameters);
    int first = attack_offset_samples(rate, offset, true, parameters);
    int origin = (int)parameters[TA_ATTACK_COORDINATE_ORIGIN];
    int phase_start = phase - origin;
    int n = ta_max(0, (input.n - first - phase_start * period) / period + phase_start);
    I positions = ta_new_ints(c, n);
    for (int i = 0; i < n; i++)
        positions.v[i] = first + origin + i * period;
    B result = attacks_process(c, a, input, rate, bpm, offset, phase, block, positions, n, true);
    ta_release(c, positions.v);
    return result;
}
static void count_hihat_beat(B result, I positions, int n, int position, const double *parameters)
{
    int last = n - (int)parameters[TA_HIHAT_LAST_BEAT_BACK];
    int increment = (int)parameters[TA_HIHAT_BEAT_HIT_INCREMENT];
    for (int beat = 0; beat < last; beat++)
        if (positions.v[beat] <= position && position < positions.v[beat + 1])
            result.v[beat] += increment;
    if (last >= 0 && positions.v[last] <= position)
        result.v[last] += increment;
}

static B hihat_counts(ta_context *c, D s, double rate, double bpm, I positions, int n,
                      const double *parameters)
{
    B result = ta_new_bytes(c, n);
    double fast = parameters[TA_HIHAT_ENVELOPE_INITIAL], slow = fast;
    double reciprocal = parameters[TA_HIHAT_INVERSE_RATE_NUMERATOR] / rate;
    double gap = rate * parameters[TA_HIHAT_GAP_SCALE] / bpm;
    int previous = (int)parameters[TA_HIHAT_FIRST_GAP_ORIGIN];
    int bias = (int)parameters[TA_HIHAT_HIT_POSITION_BIAS];
    for (int i = 0; i < s.n; i++) {
        if (!(i & 16383))
            ta_check(c);
        double value = fabs(s.v[i]);
        fast = fma(parameters[value < fast ? TA_HIHAT_FAST_FALL : TA_HIHAT_FAST_RISE] * (value - fast), reciprocal, fast);
        slow = fma(parameters[value < slow ? TA_HIHAT_SLOW_FALL : TA_HIHAT_SLOW_RISE] * (value - slow), reciprocal, slow);
        int position = i + bias;
        if (slow < fast && fast > parameters[TA_HIHAT_MIN_LEVEL] && gap < (double)(position - previous)) {
            previous = position;
            count_hihat_beat(result, positions, n, position, parameters);
        }
    }
    return result;
}
static bool replace_hihat_island(B levels, int i, int length, int previous)
{
    int level = levels.v[i + previous];
    bool different = true;
    for (int j = i; j < i + length; j++)
        if (levels.v[j] == level)
            different = false;
    if (level == levels.v[i + length] && different) {
        for (int j = i; j < i + length; j++)
            levels.v[j] = level;
        return true;
    }
    return false;
}

static void smooth_hihat_islands(ta_context *c, B levels, int first, int length, int previous)
{
    bool changed;
    do {
        ta_check(c);
        changed = false;
        for (int i = first; i < levels.n - length; i++)
            changed |= replace_hihat_island(levels, i, length, previous);
    } while (changed);
}

static void smooth_hihat_levels(ta_context *c, B levels, int remainder, const double *parameters)
{
    int first = (int)parameters[TA_HIHAT_ISLAND_SCAN_FIRST];
    int previous = (int)parameters[TA_HIHAT_ISLAND_PREVIOUS_OFFSET];
    for (int pass = TA_HIHAT_ISLAND_FIRST_LENGTH; pass <= TA_HIHAT_ISLAND_THIRD_LENGTH; pass++) {
        int threshold = TA_HIHAT_ISLAND_FIRST_MIN_REMAINDER + pass - TA_HIHAT_ISLAND_FIRST_LENGTH;
        if (remainder < parameters[threshold]) break;
        smooth_hihat_islands(c, levels, first, (int)parameters[pass], previous);
    }
}

static uint8_t hihat_level(uint8_t count, const double *parameters)
{
    if (count <= parameters[TA_HIHAT_SPARSE_MAX_COUNT])
        return (uint8_t)parameters[TA_HIHAT_SPARSE_LEVEL];
    if (count <= parameters[TA_HIHAT_LIGHT_MAX_COUNT])
        return (uint8_t)parameters[TA_HIHAT_LIGHT_LEVEL];
    if (count <= parameters[TA_HIHAT_MEDIUM_MAX_COUNT])
        return (uint8_t)parameters[TA_HIHAT_MEDIUM_LEVEL];
    return (uint8_t)parameters[TA_HIHAT_HEAVY_LEVEL];
}

static void mark_hihat_changes(B levels, B changes, int remainder, const double *parameters)
{
    if (remainder < parameters[TA_HIHAT_CHANGE_MIN_REMAINDER]) return;
    int first = (int)parameters[TA_HIHAT_CHANGE_FIRST_BAR];
    int previous = (int)parameters[TA_HIHAT_CHANGE_PREVIOUS_OFFSET];
    uint8_t mark = (uint8_t)parameters[TA_HIHAT_CHANGE_MARK];
    for (int i = first; i < levels.n; i++)
        if (levels.v[i] != levels.v[i + previous])
            changes.v[i] = mark;
}

static B hihat_changes(ta_context *c, B counts, int phase, const double *parameters)
{
    int remainder = counts.n - phase;
    if (remainder < 0)
        return ta_new_bytes(c, 0);
    int width = (int)parameters[TA_HIHAT_BEATS_PER_BAR];
    int phase_start = phase - (int)parameters[TA_HIHAT_PHASE_ORIGIN];
    int bars = (remainder + width) / width;
    B levels = ta_new_bytes(c, bars), changes = ta_new_bytes(c, bars);
    for (int i = 0; i <= remainder; i++)
        levels.v[i / width] += counts.v[phase_start + i];
    for (int i = 0; i < bars; i++)
        levels.v[i] = hihat_level(levels.v[i], parameters);
    smooth_hihat_levels(c, levels, remainder, parameters);
    mark_hihat_changes(levels, changes, remainder, parameters);
    ta_release(c, levels.v);
    return changes;
}
static D divided_band(ta_context *c, D samples, double rate, bool high, I positions, int n,
                      const double *parameters)
{
    ta_phrase_biquad band = second_order(rate, parameters[TA_RHYTHM_BAND_CUTOFF], parameters[TA_RHYTHM_BAND_DAMPING], high),
                     smooth = smoother(rate, parameters[TA_RHYTHM_BAND_SMOOTH_CUTOFF]);
    D tmp = filter(c, &band, 1, samples, false), envelope = filter(c, &smooth, 1, tmp, true),
      result = ta_new_doubles(c, n);
    ta_release(c, tmp.v);
    for (int i = 0; i < n; i++) {
        int first = positions.v[i] - 1, end = i + 1 < n ? positions.v[i + 1] - 1 : envelope.n;
        if (first >= 0 && end > first && end <= envelope.n)
            result.v[i] = d_sum(d_view(c, envelope, first, end), parameters);
    }
    ta_release(c, envelope.v);
    return result;
}
static void release_power(ta_context *c, Power p)
{
    ta_release(c, p.quarters.v);
    ta_release(c, p.peaks.v);
    ta_release(c, p.bars.v);
}
static void rhythm_regions(ta_context *c, Samples s, Coordinates coordinates, int begin, int end,
                           bool partial, D regions[3], I pos[3], int *lowEnd, int *midEnd,
                           const double *parameters)
{
    int n = end - begin;
    D sources[] = { s.low, s.middle, s.high };
    I allpos[] = { coordinates.low, coordinates.middle, coordinates.high };
    for (int band = 0; band < 3; band++) {
        int back = (int)parameters[TA_BEAT_BLOCK_LOW_SAMPLE_BACK + band];
        int start = begin ? allpos[band].v[begin] - back : 0,
            stop = partial ? sources[band].n : allpos[band].v[end] - back;
        regions[band] = d_view(c, sources[band], start, stop);
        pos[band] = ta_new_ints(c, n + 1);
        for (int i = 0; i <= n; i++)
            pos[band].v[i] = allpos[band].v[begin + i] - start;
        if (!band)
            *lowEnd = stop;
        if (band == 1)
            *midEnd = stop;
    }
}

static bool nonzero_band(D region)
{
    for (int i = 0; i < region.n; i++)
        if (region.v[i] != 0) return true;
    return false;
}

static void collect_rhythm_power(ta_context *c, Rhythm *r, ta_phrase_biquad *low, ta_phrase_biquad *sub, D regions[3], I pos[3], double lowRate, double midRate, double bpm, int begin, int n,
                                 const double *parameters)
{
    Power lp = rhythm_power(c, low, regions[0], lowRate, bpm, pos[0], n, true, parameters);
    D subFiltered = filter(c, sub, (int)parameters[TA_RHYTHM_SUB_FILTER_SECTIONS], regions[0], false);
    Power sp = rhythm_power(c, NULL, subFiltered, lowRate, bpm, pos[0], n, false, parameters);
    ta_release(c, subFiltered.v);
    scale_low_power(sp, lowRate, parameters);
    Power ap = rhythm_power(c, NULL, regions[1], midRate, bpm, pos[1], n, false, parameters);
    int slots = (int)parameters[TA_RHYTHM_BLOCK_POWER_SLOTS];
    int base = begin * slots;
    size_t bytes = (size_t)n * slots * sizeof(double);
    memcpy(r->lowPower.v + base, lp.quarters.v, bytes);
    memcpy(r->subPower.v + base, sp.quarters.v, bytes);
    memcpy(r->allPower.v + base, ap.quarters.v, bytes);
    memcpy(r->allPeaks.v + base, ap.peaks.v, bytes);
    release_power(c, lp);
    release_power(c, sp);
    release_power(c, ap);
}

static int rhythm_completed_bars(int end, int phase, const double *parameters)
{
    int bias = (int)parameters[TA_RHYTHM_BLOCK_COMPLETED_BEAT_BIAS];
    return (int)((end - phase + bias) * parameters[TA_RHYTHM_BLOCK_BEAT_TO_BAR_SCALE]);
}

static B rhythm_attack_buffer(ta_context *c, int bars, const double *parameters)
{
    int slots = bars * (int)parameters[TA_RHYTHM_BLOCK_ATTACK_SLOTS];
    int empty_width = (int)parameters[TA_RHYTHM_BLOCK_EMPTY_ATTACK_WIDTH];
    B buffer = ta_new_bytes(c, ta_max(slots, empty_width));
    buffer.n = slots;
    return buffer;
}

static void collect_rhythm_attacks(ta_context *c, Rhythm *r, B a, B b, int end, int phase,
                                   const double *parameters)
{
    int completed = rhythm_completed_bars(end, phase, parameters);
    int slots = (int)parameters[TA_RHYTHM_BLOCK_ATTACK_SLOTS];
    int empty_width = (int)parameters[TA_RHYTHM_BLOCK_EMPTY_ATTACK_WIDTH];
    B values[] = { a, b };
    B *dest[] = { &r->first, &r->second };
    for (size_t k = 0; k < sizeof values / sizeof *values; k++) {
        int start = completed * slots - values[k].n;
        if (start < 0 || completed < parameters[TA_RHYTHM_BLOCK_ATTACK_MIN_BARS]) {
            dest[k]->n = empty_width;
            memset(dest[k]->v, 0, (size_t)empty_width);
        } else {
            if (start + values[k].n > dest[k]->n)
                ta_fail(c, TA_FAILED, "rhythm attack range");
            memcpy(dest[k]->v + start, values[k].v, values[k].n);
        }
        ta_release(c, values[k].v);
    }
}

static void advance_middle_coordinates(I positions, int n, const double *parameters)
{
    int origin = positions.v[n];
    for (int i = 0; i < positions.n; i++)
        positions.v[i] -= origin;
    int advance = (int)parameters[TA_RHYTHM_BLOCK_CARRIED_COORDINATE_ADVANCE];
    for (int i = 0; i < positions.n - n; i++)
        positions.v[i] = positions.v[i + n] + advance;
}

Rhythm phrase_rhythm(ta_context *c, Samples s, Timing t, Coordinates coordinates,
                     const double *parameters)
{
    int count = coordinates.n, bars = ta_max(0, rhythm_completed_bars(count, t.phase, parameters));
    int block = 0, lowEnd = 0, midEnd = 0;
    double lowRate = s.rate * parameters[TA_DECIMATE_LOW_RATE_SCALE],
           midRate = s.rate * parameters[TA_DECIMATE_MIDDLE_RATE_SCALE],
           highRate = s.rate * parameters[TA_DECIMATE_HIGH_RATE_SCALE];
    int sub_count = (int)parameters[TA_RHYTHM_SUB_FILTER_SECTIONS];
    ta_phrase_biquad low = second_order(lowRate, parameters[TA_RHYTHM_POWER_FILTER_CUTOFF],
                                       parameters[TA_RHYTHM_POWER_FILTER_DAMPING], false),
                     sub[sub_count];
    for (int i = 0; i < sub_count; i++)
        sub[i] = second_order(lowRate, parameters[TA_RHYTHM_SUB_FILTER_CUTOFF],
                               parameters[TA_RHYTHM_SUB_FILTER_DAMPING], false);
    Attacks first = attacks_init(c, midRate, false, parameters),
            second = attacks_init(c, midRate, true, parameters);
    int power_slots = count * (int)parameters[TA_RHYTHM_BLOCK_POWER_SLOTS];
    Rhythm r = { .lowPower = ta_new_doubles(c, power_slots),
                 .subPower = ta_new_doubles(c, power_slots),
                 .allPower = ta_new_doubles(c, power_slots),
                 .allPeaks = ta_new_doubles(c, power_slots),
                 .first = rhythm_attack_buffer(c, bars, parameters),
                 .second = rhythm_attack_buffer(c, bars, parameters),
                 .hiHat = ta_new_bytes(c, count),
                 .finalMiddle = i_copy(c, coordinates.middle) };
    for (int begin = 0; begin < count;) {
        ta_check(c);
        int phase = begin ? (int)parameters[TA_RHYTHM_BLOCK_RESET_PHASE] : t.phase,
            offset = begin ? (int)parameters[TA_BEAT_BLOCK_RESET_OFFSET_MS] : t.offset,
            nominal = nominal_beats(t.bpm, phase, offset, parameters);
        if (nominal <= 0)
            ta_fail(c, TA_FAILED, "invalid rhythm block timing");
        int end = ta_min(count, begin + nominal), n = end - begin;
        bool partial = nominal > count - begin;
        block++;
        D regions[3];
        I pos[3];
        rhythm_regions(c, s, coordinates, begin, end, partial, regions, pos, &lowEnd, &midEnd, parameters);
        if (!r.zeroData) r.zeroData = nonzero_band(regions[0]);
        collect_rhythm_power(c, &r, &low, sub, regions, pos, lowRate, midRate, t.bpm, begin, n, parameters);
        B a = attacks_process(c, &first, regions[1], midRate, t.bpm, offset, phase, block, pos[1],
                              n, false),
          b = attacks_process(c, &second, regions[1], midRate, t.bpm, offset, phase, block, pos[1],
                              n, false);
        collect_rhythm_attacks(c, &r, a, b, end, t.phase, parameters);
        B h = hihat_counts(c, regions[2], highRate, t.bpm, pos[2], n, parameters);
        memcpy(r.hiHat.v + begin, h.v, n);
        ta_release(c, h.v);
        for (int band = 0; band < 3; band++)
            ta_release(c, pos[band].v);
        if (end < count) {
            advance_middle_coordinates(r.finalMiddle, n, parameters);
        }
        begin = end;
    }
    r.dividedLow =
        divided_band(c, d_view(c, s.low, 0, lowEnd), lowRate, false, coordinates.low, count, parameters);
    r.dividedMiddle =
        divided_band(c, d_view(c, s.middle, 0, midEnd), midRate, true, coordinates.middle, count, parameters);
    r.middleExtent = midEnd;
    r.hiHatChanges = hihat_changes(c, r.hiHat, t.phase, parameters);
    return r;
}

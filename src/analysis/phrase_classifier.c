/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
int initial_mood(MoodInputs x, const double parameters[TA_PHRASE_PARAMETER_COUNT])
{
    if (!(x.lowPowerRatio >= parameters[TA_MOOD_MIN_LOW_POWER]) ||
        x.slowRock == parameters[TA_MOOD_SLOW_ROCK_FLAG] ||
        !(x.compression >= parameters[TA_MOOD_MIN_COMPRESSION] || x.slowTempo != parameters[TA_MOOD_SLOW_TEMPO_FLAG]))
        return MOOD_LOW;
    if (x.fadeOut == parameters[TA_MOOD_FADE_FLAG] ||
        x.firstOverride == parameters[TA_MOOD_FIRST_OVERRIDE_FLAG])
        return MOOD_MID;
    if (x.secondOverride == parameters[TA_MOOD_SECOND_OVERRIDE_FLAG])
        return MOOD_HIGH;
    double score = (double)x.alternateRock * parameters[TA_MOOD_ALT_ROCK_WEIGHT];
    score = fma((double)x.narrowBPM - (double)x.sparseKick, parameters[TA_MOOD_TEMPO_WEIGHT], score);
    score = fma((double)x.typicalGenre + (double)x.broadBPM, parameters[TA_MOOD_GENRE_WEIGHT], score);
    score = fma((double)x.fourKick - (double)x.popRock + x.compression,
                parameters[TA_MOOD_RHYTHM_WEIGHT], score);
    return score > parameters[TA_MOOD_HIGH_CUTOFF] ? MOOD_HIGH : MOOD_MID;
}
void bpm_flags(double bpm, int *narrow, int *broad, const double *parameters)
{
    *narrow = (bpm >= parameters[TA_BPM_NARROW_FIRST_MIN] && bpm <= parameters[TA_BPM_NARROW_FIRST_MAX]) ||
              (bpm >= parameters[TA_BPM_NARROW_SECOND_MIN] && bpm <= parameters[TA_BPM_NARROW_SECOND_MAX]);
    *broad = !*narrow &&
             ((bpm >= parameters[TA_BPM_BROAD_FIRST_MIN] && bpm <= parameters[TA_BPM_BROAD_FIRST_MAX]) ||
              (bpm >= parameters[TA_BPM_BROAD_SECOND_MIN] && bpm <= parameters[TA_BPM_BROAD_SECOND_MAX]));
}
static void summarize_power(ta_context *c, LowPower *p, int n, int beats, const double *parameters)
{
    double short_max = parameters[TA_POWER_SHORT_MAX_BEATS];
    if (n > 0 && beats <= short_max) {
        p->ratioMean = d_sum(p->ratios, parameters) / (double)n;
        p->powerMean = d_sum(p->powers, parameters) / (double)n;
        return;
    }
    if (beats <= short_max)
        return;
    int windows = beats / (int)parameters[TA_POWER_WINDOW_BEATS] -
                  (int)parameters[TA_POWER_FINAL_GROUPS_OMITTED];
    int bars = (int)parameters[TA_POWER_WINDOW_BARS];
    double scale = parameters[TA_POWER_WINDOW_AVERAGE_SCALE];
    for (int w = 0; w < windows; w++) {
        int start = w * bars, end = start + bars;
        p->ratioMean = ta_dmax(p->ratioMean, d_sequential(d_view(c, p->ratios, start, end)) * scale);
        p->powerMean = ta_dmax(p->powerMean, d_sequential(d_view(c, p->powers, start, end)) * scale);
    }
}

LowPower classify_power(ta_context *c, D low, D full, int beats, const double *parameters)
{
    int n = beats / (int)parameters[TA_POWER_BEATS_PER_BAR];
    int slots = (int)parameters[TA_POWER_BAR_SLOTS];
    if (low.n != full.n || beats < 0 || n > low.n / slots)
        ta_fail(c, TA_FAILED, "power dimensions");
    double den = d_sum(full, parameters);
    LowPower p = { .ratio = den > 0 ? sqrt(d_sum(low, parameters) / den) : parameters[TA_POWER_FULL_QUIET_RATIO],
                   .ratios = ta_new_doubles(c, ta_max(1, n)),
                   .powers = ta_new_doubles(c, ta_max(1, n)) };
    for (int b = 0; b < n; b++) {
        int start = b * slots, end = start + slots;
        double a = d_sequential(d_view(c, low, start, end)),
               d = d_sequential(d_view(c, full, start, end));
        p.powers.v[b] = a;
        p.ratios.v[b] = d > 0 ? a / d : parameters[TA_POWER_BAR_QUIET_RATIO];
    }
    summarize_power(c, &p, n, beats, parameters);
    return p;
}
typedef struct {
    double low, full;
    int hits;
} compression_hits;

static compression_hits compression_block(D low, D full, B rhythm, int offset, int base,
                                          int slots, const double *parameters)
{
    double selectedLow = 0, selectedFull = 0;
    int hits = 0, previous = -1;
    int following = (int)parameters[TA_COMPRESSION_FOLLOWING_SLOT];
    int phase_limit = full.n - offset;
    double hit_mark = parameters[TA_COMPRESSION_HIT_MARK];
    for (int slot = 0; slot < slots; slot++)
        if (rhythm.v[base + slot] == hit_mark) {
            hits++;
            int index = offset + base + slot;
            if (index != previous && index < full.n) {
                selectedLow += low.v[index];
                selectedFull += full.v[index];
            }
            if (following <= phase_limit - index && following < full.n - index) {
                selectedLow += low.v[index + following];
                selectedFull += full.v[index + following];
                previous = index + following;
            } else
                previous = -1;
        }
    return (compression_hits){ selectedLow, selectedFull, hits };
}

Compression classify_compression(ta_context *c, D low, D full, B rhythm, int phase,
                                 const double *parameters)
{
    if (low.n != full.n || phase < 1)
        ta_fail(c, TA_FAILED, "compression dimensions");
    int slots = (int)parameters[TA_COMPRESSION_WINDOW_LAST_SLOT] + 1;
    int64_t offset_slots = (int64_t)phase * (int)parameters[TA_COMPRESSION_PHASE_SLOT_SCALE] -
                           (int)parameters[TA_COMPRESSION_PHASE_BASE_SLOTS];
    if (rhythm.n - offset_slots < slots)
        return (Compression){ parameters[TA_COMPRESSION_SHORT_FIRST],
                              parameters[TA_COMPRESSION_SHORT_SECOND] };
    int offset = (int)offset_slots, blocks = (rhythm.n - offset) / slots;
    if (offset + blocks * slots > full.n)
        ta_fail(c, TA_FAILED, "compression range");
    double maximum = 0, selectedMax = 0;
    for (int b = 0; b < blocks; b++) {
        int base = b * slots;
        compression_hits selected = compression_block(low, full, rhythm, offset, base, slots, parameters);
        double selectedLow = selected.low, selectedFull = selected.full;
        int hits = selected.hits;
        if (hits < parameters[TA_COMPRESSION_MIN_HITS])
            continue;
        double total = d_sum(d_view(c, full, offset + base, offset + base + slots), parameters);
        double off = hits < slots ? (total - selectedFull) / (double)(slots - hits)
                                  : parameters[TA_COMPRESSION_OFF_FALLBACK],
               on = selectedFull / (double)hits;
        maximum = ta_dmax(maximum, off + on > 0 ? off / (off + on) : 0);
        selectedMax = ta_dmax(selectedMax, selectedLow / (double)hits);
    }
    return (Compression){ maximum, selectedMax * parameters[TA_COMPRESSION_PEAK_SCALE] };
}
static bool silent_rhythm_window(B low, B middle, int start, int first, int last)
{
    for (int i = first; i <= last; i++)
        if (low.v[start + i] || middle.v[start + i]) return false;
    return true;
}
static bool middle_only_hit(B low, B middle, int start, int first, int last, double hit_mark)
{
    for (int i = first; i <= last; i++)
        if (!low.v[start + i] && middle.v[start + i] == hit_mark) return true;
    return false;
}
static bool joint_hit(B low, B middle, int start, int first, int last, double hit_mark)
{
    for (int i = first; i <= last; i++)
        if (low.v[start + i] == hit_mark && middle.v[start + i] == hit_mark) return true;
    return false;
}
typedef struct {
    int first, last;
} RockWindow;

static RockWindow rock_window(const double *window, int origin)
{
    return (RockWindow){ (int)window[0] - origin, (int)window[1] - origin };
}

static bool slow_rock_pattern(B low, B middle, int start, const double *parameters)
{
    double mark = parameters[TA_SLOW_ROCK_HIT_MARK];
    if (low.v[start] != mark || middle.v[start] != mark)
        return false;
    int origin = (int)parameters[TA_SLOW_ROCK_ORIGIN];
    RockWindow first = rock_window(parameters + TA_SLOW_ROCK_QUIET_FIRST, origin),
               middle_only = rock_window(parameters + TA_SLOW_ROCK_MIDDLE_FIRST, origin),
               second = rock_window(parameters + TA_SLOW_ROCK_SECOND_QUIET_FIRST, origin),
               terminal = rock_window(parameters + TA_SLOW_ROCK_TERMINAL_FIRST, origin);
    return silent_rhythm_window(low, middle, start, first.first, first.last) &&
           middle_only_hit(low, middle, start, middle_only.first, middle_only.last, mark) &&
           silent_rhythm_window(low, middle, start, second.first, second.last) &&
           joint_hit(low, middle, start, terminal.first, terminal.last, mark);
}

static bool slow_rock_near_previous(int counter, int previous, const double *parameters)
{
    return counter - parameters[TA_SLOW_ROCK_MATCH_ORIGIN] == previous ||
           counter - parameters[TA_SLOW_ROCK_MATCH_NEAR] == previous ||
           counter - parameters[TA_SLOW_ROCK_MATCH_FAR] == previous;
}

int slow_rock(ta_context *c, B low, B middle, const double *parameters)
{
    if (middle.n < low.n)
        ta_fail(c, TA_FAILED, "slow rock dimensions");
    int reset = (int)parameters[TA_SLOW_ROCK_RESET_POSITION];
    int previous = reset, matches = 0;
    int limit = low.n - (int)parameters[TA_SLOW_ROCK_TAIL_SLOTS];
    for (int start = 0; start < limit; start++) {
        if (!slow_rock_pattern(low, middle, start, parameters))
            continue;
        int counter = start + (int)parameters[TA_SLOW_ROCK_COUNTER_START];
        if (slow_rock_near_previous(counter, previous, parameters)) {
            matches++;
            previous = reset;
        } else
            previous = counter - (int)parameters[TA_SLOW_ROCK_NEXT_ORIGIN];
    }
    return matches > parameters[TA_SLOW_ROCK_MATCH_MIN_EXCLUSIVE];
}
static void rhythm_patterns(B rhythm, int bar, int slots, const uint32_t masks[TA_RHYTHM_MASK_COUNT],
                            bool matches[TA_RHYTHM_MASK_COUNT])
{
    for (int p = 0; p < TA_RHYTHM_MASK_COUNT; p++) matches[p] = true;
    for (int p = 0; p < TA_RHYTHM_MASK_COUNT; p++)
        for (int k = 0; k < slots; k++)
            if (rhythm.v[bar * slots + k] != ((masks[p] >> k) & 1u))
                matches[p] = false;
}

static bool rhythm_pattern_bar(int bar, int offset, const double *parameters)
{
    unsigned position = (unsigned)offset + (unsigned)parameters[TA_RHYTHM_PATTERN_OFFSET_BIAS];
    unsigned mask = (unsigned)parameters[TA_RHYTHM_PATTERN_OFFSET_MASK];
    return bar >= parameters[TA_RHYTHM_PATTERN_FIRST_VALID_BAR] &&
           (position & mask) == (unsigned)parameters[TA_RHYTHM_PATTERN_OFFSET_MATCH];
}

static void count_rhythm_patterns(B rhythm, int start, int groups, int counts[4],
                                  const double *parameters)
{
    uint32_t masks[TA_RHYTHM_MASK_COUNT];
    for (int i = 0; i < TA_RHYTHM_MASK_COUNT; ++i)
        masks[i] = (uint32_t)parameters[TA_PHRASE_OPERAND_COUNT + i];
    int bars = (int)parameters[TA_RHYTHM_PATTERN_GROUP_BARS];
    int slots = (int)parameters[TA_RHYTHM_PATTERN_BAR_SLOTS];
    for (int group = 0; group < groups; group++) {
        bool found[4] = { 0 };
        for (int offset = 0; offset < bars; offset++) {
            int bar = start - 1 + group * bars + offset;
            if (!rhythm_pattern_bar(bar, offset, parameters))
                continue;
            bool matches[TA_RHYTHM_MASK_COUNT];
            rhythm_patterns(rhythm, bar, slots, masks, matches);
            found[0] |= matches[0] || matches[1];
            found[1] |= matches[2];
            found[2] |= matches[3];
            found[3] |= matches[4];
        }
        for (int i = 0; i < 4; i++)
            if (found[i])
                counts[i]++;
    }
}

static RhythmFlags choose_rhythm_flags(const int counts[4], int remaining, double bpm,
                                       const double *parameters)
{
    RhythmFlags f = { 0 };
    double required = remaining > parameters[TA_RHYTHM_LONG_TRACK_CUTOFF]
                          ? parameters[TA_RHYTHM_SECONDARY_LONG_CUTOFF]
                          : parameters[TA_RHYTHM_SECONDARY_SHORT_CUTOFF];
    int active = (int)parameters[TA_RHYTHM_ACTIVE_FLAG];
    if (counts[0] > parameters[TA_RHYTHM_FOUR_KICK_VOTE_CUTOFF])
        f.fourKick = active;
    else if (counts[3] > required)
        f.sparseKick = active;
    else if (counts[1] > required && bpm < parameters[TA_RHYTHM_POP_ROCK_BPM_LIMIT])
        f.popRock = active;
    else if (counts[2] > required)
        f.alternateRock = active;
    return f;
}

RhythmFlags rhythm_flags(ta_context *c, B rhythm, double bpm, D powers, const double *parameters)
{
    if (!powers.n)
        ta_fail(c, TA_FAILED, "rhythm flags dimensions");
    double mean = d_sum(powers, parameters) / (double)powers.n;
    int first_bar = (int)parameters[TA_RHYTHM_SCAN_START_BAR];
    int fallback_bar = (int)parameters[TA_RHYTHM_MEAN_FALLBACK_BAR];
    int start = powers.n < first_bar ? fallback_bar : first_bar;
    int tail = (int)parameters[TA_RHYTHM_MEAN_TAIL_BARS];
    for (int i = start; i <= powers.n - tail; i++)
        if (powers.v[i - 1] > mean) {
            start = i;
            break;
        }
    int remaining = (int)((double)rhythm.n * parameters[TA_RHYTHM_LENGTH_SCALE]) - start;
    int bars = (int)parameters[TA_RHYTHM_PATTERN_GROUP_BARS];
    int slots = (int)parameters[TA_RHYTHM_PATTERN_BAR_SLOTS];
    int groups = ta_min((int)parameters[TA_RHYTHM_GROUPS_CAP], remaining / bars);
    RhythmFlags f = { 0 };
    if (groups <= 0)
        return f;
    if ((int64_t)start - 1 + (int64_t)groups * bars > rhythm.n / slots)
        ta_fail(c, TA_FAILED, "rhythm flags range");
    int counts[4] = { 0 };
    count_rhythm_patterns(rhythm, start, groups, counts, parameters);
    return choose_rhythm_flags(counts, remaining, bpm, parameters);
}
static bool slow_tempo_dimensions(SnarePositions onsets, D powers, I beats, int beats_per_bar)
{
    return !(onsets.first < 1 || onsets.count < 0 || onsets.first - 1 + onsets.count > onsets.values.n ||
             powers.n > beats.n / beats_per_bar);
}

static D shifted_onsets(ta_context *c, SnarePositions onsets, double rate, int sourceOffset,
                        int analysisOffset, double seconds_per_millisecond)
{
    D onset = ta_new_doubles(c, onsets.values.n);
    for (int i = 0; i < onset.n; i++)
        onset.v[i] = (double)onsets.values.v[i];
    int last = onsets.first + onsets.count;
    double shift = rate * seconds_per_millisecond * (double)(sourceOffset - analysisOffset);
    for (int i = onsets.first - 1; i < last - 1; i++)
        onset.v[i] += shift;
    return onset;
}

typedef struct {
    int votes, yes;
} tempo_votes;
static bool tempo_gap(double gap, double period, double low, double high)
{
    return gap > period * low && gap < period * high;
}

static void observe_tempo_vote(tempo_votes *tally, D onset, int i, double start, int rounded,
                               double quarter, double period, const double *parameters)
{
    double t = onset.v[i];
    double divided = (t - start) * (1 / (double)rounded),
           position = divided - (double)((int)divided * rounded);
    bool inside = position >= quarter && position <= quarter * parameters[TA_TEMPO_INSIDE_QUARTER_LIMIT];
    double gap = onset.v[i + 1] - t;
    double vote_limit = parameters[TA_SLOW_TEMPO_VOTE_LIMIT];
    if (tempo_gap(gap, period, parameters[TA_SLOW_TEMPO_DOUBLE_GAP_MIN],
                  parameters[TA_SLOW_TEMPO_DOUBLE_GAP_MAX]) && !inside && tally->votes <= vote_limit)
        tally->votes++;
    if (tempo_gap(gap, period, parameters[TA_SLOW_TEMPO_SINGLE_GAP_MIN],
                  parameters[TA_SLOW_TEMPO_SINGLE_GAP_MAX]) && inside && tally->votes <= vote_limit) {
        tally->votes++;
        tally->yes++;
    }
}
static void scan_tempo_bar(tempo_votes *tally, D onset, int first, int last, I beats, int bar,
                           double quarter, int rounded, double period, const double *parameters)
{
    int last_delta = (int)parameters[TA_TEMPO_SCAN_LAST_BEAT_DELTA];
    int trailing = (int)parameters[TA_TEMPO_TRAILING_ONSETS];
    int start_beat = bar * (int)parameters[TA_TEMPO_SCAN_BAR_BEATS];
    int end_beat = start_beat + ta_min(last_delta, beats.n - 1 - start_beat);
    double start = beats.v[start_beat], end = beats.v[end_beat];
    int i = first - 1;
    do {
        double t = onset.v[i];
        if (t >= start && t <= end && i < last - trailing) {
            observe_tempo_vote(tally, onset, i, start, rounded, quarter, period, parameters);
        }
        i++;
        if (t > end)
            break;
    } while (i < last - trailing);
}
static tempo_votes count_tempo_votes(D onset, int first, int last, I beats, D powers, double mean,
                                     double quarter, int rounded, double period,
                                     const double *parameters)
{
    tempo_votes tally = { 0 };
    if (first <= last - parameters[TA_TEMPO_TRAILING_ONSETS])
        for (int bar = 0; bar < powers.n - parameters[TA_TEMPO_POWER_TAIL_BARS]; bar++)
            if (powers.v[bar] > mean) {
                scan_tempo_bar(&tally, onset, first, last, beats, bar, quarter, rounded, period,
                               parameters);
                if (tally.votes > parameters[TA_SLOW_TEMPO_VOTE_LIMIT])
                    break;
            }
    return tally;
}

int slow_tempo(ta_context *c, double rate, double bpm, D powers, SnarePositions onsets, I beats,
               int sourceOffset, int analysisOffset, const double *parameters)
{
    if (!slow_tempo_dimensions(onsets, powers, beats, (int)parameters[TA_TEMPO_SCAN_BAR_BEATS]))
        ta_fail(c, TA_FAILED, "slow tempo dimensions");
    int last = onsets.first + onsets.count;
    D onset = shifted_onsets(c, onsets, rate, sourceOffset, analysisOffset,
                            parameters[TA_TEMPO_ONSET_SECONDS_PER_MILLISECOND]);
    double period = bpm == 0 ? rate * parameters[TA_TEMPO_ZERO_BPM_PERIOD]
                             : rate * parameters[TA_TEMPO_SECONDS_PER_MINUTE] / bpm;
    double bias = parameters[TA_TEMPO_ROUND_BIAS];
    int rounded = (int)(period + bias);
    if (!rounded)
        ta_fail(c, TA_FAILED, "slow tempo period");
    double quarter = (double)(int)(period * parameters[TA_TEMPO_QUARTER_FRACTION] + bias),
           mean = powers.n ? d_sum(powers, parameters) / (double)powers.n : 0;
    tempo_votes tally = count_tempo_votes(onset, onsets.first, last, beats, powers, mean, quarter,
                                         rounded, period, parameters);
    int votes = tally.votes, yes = tally.yes;
    bool sync = votes && (double)yes / (double)votes > parameters[TA_SLOW_TEMPO_SYNC_CUTOFF];
    ta_release(c, onset.v);
    return !sync && bpm < parameters[TA_SLOW_TEMPO_BPM_LIMIT];
}
static bool fading_tail(D pairs, int first_descending, double power_ratio)
{
    int last = pairs.n - 1;
    if (!(pairs.v[last] < d_max(pairs) * power_ratio))
        return false;
    for (int i = last; i > first_descending; --i)
        if (!(pairs.v[i] < pairs.v[i - 1]))
            return false;
    return true;
}
static bool stable_tail_ratios(D ratios, int reference_count)
{
    D reference = { ratios.v, reference_count };
    double min = d_min(reference), max = d_max(reference);
    for (int i = reference_count; i < ratios.n; ++i)
        if (!(ratios.v[i] >= min && ratios.v[i] <= max))
            return false;
    return true;
}

static double fade_pair(double *values, int group_size)
{
    return d_sequential((D){ values, group_size }) +
           d_sequential((D){ values + group_size, group_size });
}

static bool collect_fade_pairs(D pairs, D full, int start, int group_size, double floor)
{
    bool silent = false;
    for (int i = 0; i < pairs.n; ++i) {
        pairs.v[i] = fade_pair(full.v + start + i * 2 * group_size, group_size);
        if (pairs.v[i] < floor)
            silent = true;
    }
    return silent;
}

static void calculate_fade_ratios(D ratios, D pairs, D low, int start, int group_size, bool silent)
{
    if (silent) {
        memset(ratios.v, 0, (size_t)ratios.n * sizeof(double));
        return;
    }
    for (int i = 0; i < ratios.n; ++i)
        ratios.v[i] = fade_pair(low.v + start + i * 2 * group_size, group_size) / pairs.v[i];
}

int fade_out(D low, D full, const double *parameters)
{
    if (low.n <= parameters[TA_FADE_SHORT_INPUT_MAX] || full.n < low.n)
        return 0;
    int align = (int)parameters[TA_FADE_END_ALIGNMENT];
    int start = low.n - low.n % align - (int)parameters[TA_FADE_TAIL_LOOKBACK];
    int group_size = (int)parameters[TA_FADE_GROUP_SAMPLES];
    int final_index = (int)parameters[TA_FADE_FINAL_PAIR_INDEX];
    double pair_values[final_index + 1], ratio_values[final_index];
    D pairs = { pair_values, final_index + 1 }, ratios = { ratio_values, final_index };
    bool silent = collect_fade_pairs(pairs, full, start, group_size, parameters[TA_FADE_SILENCE_FLOOR]);
    calculate_fade_ratios(ratios, pairs, low, start, group_size, silent);
    return fading_tail(pairs, (int)parameters[TA_FADE_FIRST_DESCENDING_PAIR],
                       parameters[TA_FADE_TAIL_POWER_RATIO]) &&
           stable_tail_ratios(ratios, (int)parameters[TA_FADE_REFERENCE_RATIO_COUNT]);
}
static void mood_correspondence(ta_context *c, D quarters, int from, int count, D maxima,
                                D correspondence, const double *parameters)
{
    int previous = 0;
    int width = (int)parameters[TA_REVISE_BEAT_QUARTERS];
    unsigned delta_mask = (unsigned)parameters[TA_REVISE_DELTA_MASK];
    int delta_match = (int)parameters[TA_REVISE_DELTA_MATCH];
    double zero_level = parameters[TA_REVISE_ZERO_LEVEL];
    for (int bar = 1; bar < count; bar++) {
        int first = (from + bar - 1) * width;
        D values = d_view(c, quarters, first, first + width);
        int index = 0;
        for (int i = 1; i < width; i++)
            if (values.v[i] > values.v[index])
                index = i;
        int position = index + 1, delta = abs(position - previous);
        double maximum = values.v[index];
        maxima.v[bar] = maximum;
        if (!delta)
            correspondence.v[bar - 1] = parameters[TA_REVISE_EQUAL_CORRESPONDENCE];
        else if (((unsigned)delta & delta_mask) == (unsigned)delta_match)
            correspondence.v[bar - 1] =
                quarters.v[first - 1 + previous] / (maximum == 0 ? zero_level : maximum);
        previous = position;
    }
}

static double mood_changes(D maxima, int first, int end, double zero_level)
{
    double changes = 0;
    for (int bar = first; bar < end; bar++) {
        if (maxima.v[bar] == 0)
            maxima.v[bar] = zero_level;
        if (maxima.v[bar - 1] == 0)
            maxima.v[bar - 1] = zero_level;
        changes += ta_dmax(maxima.v[bar], maxima.v[bar - 1]) /
                   ta_dmin(maxima.v[bar], maxima.v[bar - 1]);
    }
    return changes;
}

int revise_mood(ta_context *c, int mood, int beats, D low, D quarters, const double *parameters)
{
    if (mood != MOOD_MID || beats <= parameters[TA_REVISE_SHORT_TRACK_MAX])
        return mood;
    int from = (int)parameters[TA_REVISE_FIRST_BEAT];
    int to = beats - (int)parameters[TA_REVISE_END_BEATS_SKIPPED];
    int width = (int)parameters[TA_REVISE_BEAT_QUARTERS];
    int count = ((to - from) * width - 1 - (int)parameters[TA_REVISE_QUARTER_TRIM]) / width;
    double mean = d_sequential(d_view(c, low, from - 1, to - 1)) / (double)(to - from);
    int first = (int)parameters[TA_REVISE_CHANGE_HEAD_SKIP];
    int end = count - (int)parameters[TA_REVISE_CHANGE_TAIL_SKIP];
    if (end <= first)
        return mood;
    D maxima = ta_new_doubles(c, count), correspondence = ta_new_doubles(c, count);
    double zero_level = parameters[TA_REVISE_ZERO_LEVEL];
    mood_correspondence(c, quarters, from, count, maxima, correspondence, parameters);
    if (d_sequential(correspondence) / (double)count > parameters[TA_REVISE_MIN_CORRESPONDENCE]) {
        double changes = mood_changes(maxima, first, end, zero_level);
        if (changes / (double)(end - first) >= parameters[TA_REVISE_MIN_CHANGE] &&
            mean >= parameters[TA_REVISE_MIN_POWER])
            mood = MOOD_HIGH;
    }
    ta_release(c, maxima.v);
    ta_release(c, correspondence.v);
    return mood;
}
static void recurrence_coordinates(ta_context *c, Spectra spectra, I boundaries, int phase,
                                   int end_beat, I indices, D means, const double *parameters)
{
    int n = boundaries.n, width = spectra.rows[0].n;
    int minimum = (int)parameters[TA_RECURRENCE_FRAME_MIN];
    double scale = parameters[TA_RECURRENCE_FRAME_SCALE], bias = parameters[TA_RECURRENCE_FRAME_BIAS];
    for (int i = 0; i <= n; i++) {
        int beat = i < n ? boundaries.v[i] : end_beat;
        indices.v[i] = ta_max(minimum, (int)fma((double)beat - phase, scale, bias));
        if (i && indices.v[i] < indices.v[i - 1])
            ta_fail(c, TA_FAILED, "recurrence boundaries");
    }
    for (int i = 0; i < spectra.n; i++) {
        if (spectra.rows[i].n != width)
            ta_fail(c, TA_FAILED, "ragged recurrence spectra");
        means.v[i] = d_sum(spectra.rows[i], parameters) / (double)width;
    }
}

static double recurrence_delta(double a, double b, double mean_a, double mean_b, bool windows)
{
    if (windows)
        return ((a - b) - mean_a) + mean_b;
    return (mean_b + a) - (mean_a + b);
}

static double recurrence_distance(Spectra spectra, D means, int a, int b, int width,
                                  const double *parameters)
{
    int group = (int)parameters[TA_RECURRENCE_DISTANCE_GROUP], mask = group - 1;
    int full = width & ~mask;
    const double *order = parameters + TA_RECURRENCE_DISTANCE_PAIR_0;
    bool windows = phrase_reduction_uses_windows(order, group);
    double lanes[TA_RECURRENCE_DISTANCE_PAIR_CAPACITY * 2] = { 0 };
    for (int col = 0; col < full; col++) {
        double delta = recurrence_delta(spectra.rows[a].v[col], spectra.rows[b].v[col],
                                        means.v[a], means.v[b], windows);
        lanes[col & mask] = fma(delta, delta, lanes[col & mask]);
    }
    double distance = full ? phrase_reduce_pairs(lanes, order, group) : 0.0;
    for (int col = full; col < width; col++) {
        double delta = recurrence_delta(spectra.rows[a].v[col], spectra.rows[b].v[col],
                                        means.v[a], means.v[b], windows);
        distance = windows ? distance + delta * delta : fma(delta, delta, distance);
    }
    return distance;
}

static void recurrence_scores(ta_context *c, Spectra spectra, I indices, D means, Recurrence r,
                              D *positives, const double *parameters)
{
    int n = r.n, width = spectra.rows[0].n;
    double identical_frame_score = parameters[TA_RECURRENCE_IDENTICAL_FRAME_SCORE];
    double max_match_distance = parameters[TA_RECURRENCE_MATCH_DISTANCE_MAX];
    double inverse_distance_numerator = parameters[TA_RECURRENCE_INVERSE_DISTANCE_NUMERATOR];
    double score_floor = parameters[TA_RECURRENCE_POSITIVE_SCORE_MIN_EXCLUSIVE];
    for (int i = 0; i < n; i++) {
        r.scores[i] = ta_new_doubles(c, n);
        r.related[i] = ta_new_bytes(c, n);
        for (int j = i + 1; j < n; j++) {
            ta_check(c);
            int count =
                ta_min(ta_min(indices.v[i + 1] - indices.v[i], indices.v[j + 1] - indices.v[j]),
                       spectra.n - indices.v[j]);
            if (count <= 0)
                continue;
            double total = 0;
            for (int k = 0; k < count; k++) {
                double distance = recurrence_distance(spectra, means, indices.v[i] + k, indices.v[j] + k,
                                                      width, parameters);
                total += distance <= max_match_distance ? identical_frame_score : inverse_distance_numerator / distance;
            }
            r.scores[i].v[j] = total / (double)count;
            if (r.scores[i].v[j] > score_floor)
                positives->v[positives->n++] = r.scores[i].v[j];
        }
    }
}

static double recurrence_threshold(D positives, const double *parameters)
{
    double mean = positives.n ? d_sum(positives, parameters) / (double)positives.n : 0;
    for (int i = 0; i < positives.n; i++)
        positives.v[i] = (positives.v[i] - mean) * (positives.v[i] - mean);
    double squares = d_sum(positives, parameters);
    if (positives.n < parameters[TA_RECURRENCE_VARIANCE_MIN_COUNT])
        return mean + squares;
    int divisor = positives.n - (int)parameters[TA_RECURRENCE_VARIANCE_COUNT_BACK];
    return mean + sqrt(squares / (double)divisor);
}

Recurrence recurrence(ta_context *c, Spectra spectra, I boundaries, int phase, int endBeat,
                      const double *parameters)
{
    if (!spectra.n || spectra.rows[0].n <= 0 || !boundaries.n)
        ta_fail(c, TA_FAILED, "recurrence dimensions");
    int n = boundaries.n;
    I indices = ta_new_ints(c, n + 1);
    D means = ta_new_doubles(c, spectra.n);
    recurrence_coordinates(c, spectra, boundaries, phase, endBeat, indices, means, parameters);
    Recurrence r = {
        .scores = ta_alloc(c, n, sizeof(D)), .related = ta_alloc(c, n, sizeof(B)), .n = n
    };
    D positives = ta_new_doubles(c, n * n);
    positives.n = 0;
    recurrence_scores(c, spectra, indices, means, r, &positives, parameters);
    r.threshold = recurrence_threshold(positives, parameters);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (r.scores[i].v[j] >= r.threshold)
                r.related[i].v[j] = r.related[j].v[i] = 1;
    ta_release(c, indices.v);
    ta_release(c, means.v);
    ta_release(c, positives.v);
    return r;
}
static void remove_bridge_matches(I roles, B removed, Recurrence r, int n,
                                  const double *parameters)
{
    double bridge = parameters[TA_RECURRENCE_BRIDGE_ROLE], hook = parameters[TA_RECURRENCE_HOOK_ROLE];
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (r.related[i].v[j]) {
                if (roles.v[i] == bridge && roles.v[j] == hook) {
                    roles.v[i] = 0;
                    removed.v[i] = 1;
                } else if (roles.v[i] == hook && roles.v[j] == bridge) {
                    roles.v[j] = 0;
                    removed.v[j] = 1;
                }
            }
}

static bool assign_empty_role(I roles, B removed, int i, int j, int next,
                              const double *parameters)
{
    if (!roles.v[j]) {
        roles.v[i] = next;
        roles.v[j] = next;
        return true;
    } else if (roles.v[j] == parameters[TA_RECURRENCE_HOOK_ROLE]) {
        if (!removed.v[i]) roles.v[i] = roles.v[j];
    } else if (roles.v[j] != parameters[TA_RECURRENCE_INTRO_ROLE] &&
               roles.v[j] != parameters[TA_RECURRENCE_BRIDGE_ROLE])
        roles.v[i] = roles.v[j];
    return false;
}
static bool propagate_role(I roles, B removed, int i, int j, int next, const double *parameters)
{
    if (!roles.v[i])
        return assign_empty_role(roles, removed, i, j, next, parameters);
    if (!roles.v[j] && roles.v[i] != parameters[TA_RECURRENCE_INTRO_ROLE] &&
        roles.v[i] != parameters[TA_RECURRENCE_BRIDGE_ROLE]) {
        if (roles.v[i] != parameters[TA_RECURRENCE_HOOK_ROLE] || !removed.v[j])
            roles.v[j] = roles.v[i];
    }
    return false;
}
static void finish_material_roles(I roles, int n, int next, const double *parameters)
{
    if (n && roles.v[n - 1] != parameters[TA_RECURRENCE_HOOK_ROLE])
        roles.v[n - 1] = (int)parameters[TA_RECURRENCE_OUTRO_ROLE];
    for (int i = 1; i < n - 1; i++)
        if (!roles.v[i])
            roles.v[i] = next++;
}

I reconcile_roles(ta_context *c, I seeds, Recurrence r, const double *parameters)
{
    if (seeds.n != r.n)
        ta_fail(c, TA_FAILED, "recurrence role dimensions");
    I roles = i_copy(c, seeds);
    B removed = ta_new_bytes(c, seeds.n);
    int n = roles.n, next = (int)parameters[TA_RECURRENCE_FIRST_MATERIAL_ROLE];
    remove_bridge_matches(roles, removed, r, n, parameters);
    for (int i = 0; i < n; i++) {
        bool created = false;
        for (int j = i + 1; j < n; j++)
            if (r.related[i].v[j]) {
                if (propagate_role(roles, removed, i, j, next, parameters)) created = true;
            }
        if (created)
            next++;
    }
    finish_material_roles(roles, n, next, parameters);
    ta_release(c, removed.v);
    return roles;
}
static int int_order(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static double segment(ta_context *c, D a, int start, int end, double minimum, const double *parameters)
{
    if (end - start < minimum) {
        if (end < 1 || end > a.n)
            ta_fail(c, TA_FAILED, "seed segment range");
        return a.v[end - 1];
    }
    return d_sum(d_view(c, a, start, end), parameters) / (double)(end - start);
}
static I seed_boundaries(ta_context *c, I boundaries, const double *parameters)
{
    I relative = ta_new_ints(c, boundaries.n + 1);
    relative.n = 0;
    int divisor = (int)parameters[TA_SEED_BOUNDARY_DIVISOR];
    int origin = (int)parameters[TA_SEED_BOUNDARY_ORIGIN];
    for (int i = 0; i < boundaries.n; i++) {
        int v = (boundaries.v[i] - boundaries.v[0]) / divisor + origin;
        if (v)
            relative.v[relative.n++] = v;
    }
    qsort(relative.v, relative.n, sizeof(int), int_order);
    return relative;
}

static D seed_padded_envelope(ta_context *c, D opening, int middle_count, const double *parameters)
{
    int scale = (int)parameters[TA_SEED_ENVELOPE_PAD_SCALE];
    if (middle_count > INT_MAX / scale)
        ta_fail(c, TA_LIMIT, "seed envelope extent");
    D envelope = ta_new_doubles(c, ta_max(opening.n, middle_count * scale));
    memcpy(envelope.v, opening.v, opening.n * sizeof(double));
    return envelope;
}

static void seed_terminal(ta_context *c, D middle, I relative, int n, double trim)
{
    int nonzero = 0;
    for (int i = 0; i < middle.n; i++)
        if (middle.v[i] != 0)
            nonzero++;
    relative.v[n] = nonzero - (int)trim;
    if (relative.v[n] < relative.v[n - 1])
        ta_fail(c, TA_FAILED, "seed terminal boundary");
}

static void measure_seed_segments(ta_context *c, SeedEvidence e, I relative, D middle, D envelope,
                                  D vocal, int n, const double *parameters)
{
    double minimum = parameters[TA_SEED_MEAN_RANGE_MIN];
    int beat_width = (int)parameters[TA_SEED_ENVELOPE_BEATS_PER_BAR];
    int envelope_start_back = (int)parameters[TA_SEED_ENVELOPE_START_BACK];
    int envelope_end_back = (int)parameters[TA_SEED_ENVELOPE_TERMINAL_SAMPLE_BACK];
    int middle_start_back = (int)parameters[TA_SEED_MIDDLE_START_BACK];
    int middle_end_back = (int)parameters[TA_SEED_MIDDLE_TERMINAL_SAMPLE_BACK];
    for (int i = 0; i < n; i++) {
        int start = relative.v[i], end = relative.v[i + 1];
        int envelope_last = end * beat_width - envelope_end_back, middle_last = end - middle_end_back;
        if (start < 1 || middle_last < 0 || middle_last >= middle.n ||
            envelope_last < 0 || envelope_last >= envelope.n || envelope_last >= vocal.n)
            ta_fail(c, TA_FAILED, "seed range");
        int envelope_start = start * beat_width - envelope_start_back;
        e.level.v[i] = segment(c, envelope, envelope_start, envelope_last + 1, minimum, parameters);
        e.vocal.v[i] = segment(c, vocal, envelope_start, envelope_last + 1, minimum, parameters);
        e.middle.v[i] = segment(c, middle, start - middle_start_back, middle_last + 1, minimum, parameters);
    }
}

static void normalize_seed_evidence(SeedEvidence e, int n, const double *parameters)
{
    D *arrays[] = { &e.level, &e.vocal, &e.middle };
    const int zero_scales[] = { TA_SEED_LEVEL_ZERO_SCALE, TA_SEED_VOCAL_ZERO_SCALE,
                                TA_SEED_MIDDLE_ZERO_SCALE };
    for (int k = 0; k < 3; k++) {
        double m = d_max(*arrays[k]), scale = m == 0 ? parameters[zero_scales[k]] : 1 / m;
        for (int i = 0; i < n; i++)
            arrays[k]->v[i] *= scale;
    }
}

static double active_vocal_mean(ta_context *c, SeedEvidence e, int n, const double *parameters)
{
    D active = ta_new_doubles(c, n);
    active.n = 0;
    for (int i = 0; i < n; i++)
        if (e.vocal.v[i] != 0)
            active.v[active.n++] = e.vocal.v[i];
    double vocalMean = active.n ? d_sum(active, parameters) / (double)active.n : 0;
    ta_release(c, active.v);
    return vocalMean;
}

static bool strong_seed(SeedEvidence e, int i, double levelMean, const double *parameters)
{
    double split = parameters[TA_SEED_MEAN_SPLIT];
    return (levelMean > split && e.level.v[i] > parameters[TA_SEED_LEVEL_HIGH_MEAN_MIN]) ||
           (levelMean <= split && e.level.v[i] > parameters[TA_SEED_LEVEL_LOW_MEAN_MIN]) ||
           (e.level.v[i] > split && e.vocal.v[i] > parameters[TA_SEED_VOCAL_STRONG_MIN]);
}

static bool vocal_seed_growth(SeedEvidence e, int i, const double *parameters)
{
    if (e.vocal.v[i] == 0)
        e.vocal.v[i] = parameters[TA_SEED_ZERO_LEVEL];
    if (e.vocal.v[i - 1] == 0)
        e.vocal.v[i - 1] = parameters[TA_SEED_ZERO_LEVEL];
    if (e.level.v[i] > parameters[TA_SEED_GROWTH_LEVEL_MIN] &&
        e.vocal.v[i] > parameters[TA_SEED_GROWTH_VOCAL_MIN] &&
        e.vocal.v[i] / e.vocal.v[i - 1] > parameters[TA_SEED_GROWTH_VOCAL_RATIO]) {
        return true;
    }
    return false;
}

static bool level_seed_growth(SeedEvidence e, int i, const double *parameters)
{
    if (e.level.v[i] == 0)
        e.level.v[i] = parameters[TA_SEED_ZERO_LEVEL];
    if (e.level.v[i - 1] == 0)
        e.level.v[i - 1] = parameters[TA_SEED_ZERO_LEVEL];
    double ratio = e.level.v[i] / e.level.v[i - 1];
    if ((e.level.v[i] > parameters[TA_SEED_GROWTH_LEVEL_MIN] &&
         ratio > parameters[TA_SEED_GROWTH_LEVEL_RATIO]) ||
        (e.roles.v[i - 1] == parameters[TA_SEED_HOOK_ROLE] && ratio > parameters[TA_SEED_VOCAL_STRONG_MIN]))
        return true;
    return false;
}
static void classify_seed(SeedEvidence e, int i, double vocalMean, double levelMean,
                          const double *parameters)
{
    double minimum = parameters[TA_SEED_ELIGIBILITY_MIN];
    bool eligible = e.middle.v[i] >= minimum && (vocalMean < minimum || e.vocal.v[i] >= minimum);
    bool strong = strong_seed(e, i, levelMean, parameters);
    int hook = (int)parameters[TA_SEED_HOOK_ROLE];
    if (!eligible)
        return;
    if (strong) {
        e.roles.v[i] = hook;
        return;
    }
    if (!i)
        return;
    if (vocal_seed_growth(e, i, parameters)) {
        e.roles.v[i] = hook;
        return;
    }
    if (level_seed_growth(e, i, parameters)) e.roles.v[i] = hook;
}

static double seed_decline(int mood, int previous_role, const double *parameters)
{
    bool hook = previous_role == parameters[TA_SEED_HOOK_ROLE];
    if (mood == MOOD_MID)
        return parameters[hook ? TA_SEED_DECLINE_MID_HOOK : TA_SEED_DECLINE_MID_OTHER];
    return parameters[hook ? TA_SEED_DECLINE_LOW_HOOK : TA_SEED_DECLINE_LOW_OTHER];
}
static void classify_seed_declines(SeedEvidence e, int n, int mood, const double *parameters)
{
    int intro = (int)parameters[TA_SEED_INTRO_ROLE];
    double hook = parameters[TA_SEED_HOOK_ROLE];
    int decline_role = (int)parameters[TA_SEED_DECLINE_ROLE];
    for (int i = 1; i < n; i++) {
        double decline = seed_decline(mood, e.roles.v[i - 1], parameters);
        if (e.roles.v[i - 1] == intro && e.vocal.v[i] == 0) {
            if (e.roles.v[i] != hook)
                e.roles.v[i] = intro;
        } else if (e.roles.v[i] != hook && e.vocal.v[i] < e.vocal.v[i - 1] * decline)
            e.roles.v[i] = decline_role;
    }
}

SeedEvidence non_high_seeds(ta_context *c, D middleLevels, D opening, I boundaries, D vocalActivity,
                            int mood, const double *parameters)
{
    if (!middleLevels.n || !boundaries.n || !vocalActivity.n)
        ta_fail(c, TA_FAILED, "seed dimensions");
    D envelope = seed_padded_envelope(c, opening, middleLevels.n, parameters);
    D middle = d_normalized(c, middleLevels, parameters[TA_SEED_MIDDLE_INPUT_ZERO]);
    D vocal = d_normalized(c, vocalActivity, parameters[TA_SEED_VOCAL_INPUT_ZERO]);
    I relative = seed_boundaries(c, boundaries, parameters);
    int n = relative.n;
    if (!n)
        ta_fail(c, TA_FAILED, "seed relative boundaries");
    SeedEvidence e = { ta_new_ints(c, n), ta_new_doubles(c, n), ta_new_doubles(c, n),
                       ta_new_doubles(c, n) };
    if (d_sum(middle, parameters) == 0)
        goto done;
    seed_terminal(c, middle, relative, n, parameters[TA_SEED_TERMINAL_TRIM]);
    measure_seed_segments(c, e, relative, middle, envelope, vocal, n, parameters);
    normalize_seed_evidence(e, n, parameters);
    double vocalMean = active_vocal_mean(c, e, n, parameters), levelMean = d_sum(e.level, parameters) / (double)n;
    e.roles.v[0] = (int)parameters[TA_SEED_INTRO_ROLE];
    for (int i = 0; i < n; i++) {
        classify_seed(e, i, vocalMean, levelMean, parameters);
    }
    if (n + 1 >= parameters[TA_SEED_DECLINE_MIN_BOUNDARIES])
        classify_seed_declines(e, n, mood, parameters);
done:
    ta_release(c, envelope.v);
    ta_release(c, middle.v);
    ta_release(c, vocal.v);
    ta_release(c, relative.v);
    return e;
}

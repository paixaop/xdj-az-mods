/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azanalysis_policy.h"
#include "internal.h"
#include <math.h>

static size_t read_model_file(FILE *file, uint8_t *bytes)
{
    if (fread(bytes, 1, TA_MODEL_HEADER_BYTES, file) != TA_MODEL_HEADER_BYTES)
        return 0;
    size_t length = memcmp(bytes, TA_MODEL_WINDOWS_MAGIC, 8) == 0 ?
                    TA_MODEL_WINDOWS_BYTES : TA_MODEL_BYTES;
    size_t remaining = length - TA_MODEL_HEADER_BYTES;
    if (fread(bytes + TA_MODEL_HEADER_BYTES, 1, remaining, file) != remaining ||
        fgetc(file) != EOF || ferror(file))
        return 0;
    return length;
}

static size_t model_read(ta_context *c, const char *path, uint8_t *bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        ta_fail(c, TA_FAILED, "analysis model open failed");
    size_t length = read_model_file(file, bytes);
    if (fclose(file) || !length)
        ta_fail(c, TA_FAILED, "invalid model size; regenerate pvdi-model.bin");
    return length;
}

static void model_validate(ta_context *c, const uint8_t *bytes, size_t length)
{
    bool windows = length == TA_MODEL_WINDOWS_BYTES;
    const char *magic = windows ? TA_MODEL_WINDOWS_MAGIC : TA_MODEL_MAGIC;
    if (memcmp(bytes, magic, 8) || ta_le32(bytes + 8) != length - TA_MODEL_HEADER_BYTES)
        ta_fail(c, TA_FAILED, "invalid model version; regenerate pvdi-model.bin");
    uint32_t hash = 2166136261u;
    for (size_t i = TA_MODEL_HEADER_BYTES; i < length; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    if (ta_le32(bytes + 12) != hash)
        ta_fail(c, TA_FAILED, "analysis model checksum");
    for (int i = 16; i < TA_MODEL_HEADER_BYTES; ++i)
        if (bytes[i])
            ta_fail(c, TA_FAILED, "analysis model reserved fields");
    c->windows_model = windows;
}

uint8_t *ta_model_open(ta_context *c)
{
    char path[1200];
    const char *assets = c->request->assets_path;
    if (!assets || snprintf(path, sizeof path, "%s/pvdi-model.bin", assets) >= (int)sizeof path)
        ta_fail(c, TA_FAILED, "missing analysis model path");
    uint8_t *bytes = ta_alloc(c, TA_MODEL_WINDOWS_BYTES, 1);
    size_t length = model_read(c, path, bytes);
    model_validate(c, bytes, length);
    return bytes;
}

static bool valid_phrase_index(double value, int min, int max)
{
    return value >= min && value <= max && trunc(value) == value;
}

static void validate_phrase_scan(ta_context *c, const double *parameters)
{
    /* These parameters control array indexing or a floating-point to int conversion. */
    if (!valid_phrase_index(parameters[TA_RHYTHM_SCAN_START_BAR], 1, INT_MAX) ||
        parameters[TA_RHYTHM_LENGTH_SCALE] <= 0 || parameters[TA_RHYTHM_LENGTH_SCALE] > 1 ||
        !valid_phrase_index(parameters[TA_FADE_SHORT_INPUT_MAX], 527, INT_MAX) ||
        !valid_phrase_index(parameters[TA_SECURE_MIN_BEATS], 12, INT_MAX))
        ta_fail(c, TA_FAILED, "invalid phrase scan parameters");
}

static void validate_phrase_block(ta_context *c, const double *parameters)
{
    double seconds = parameters[TA_PCM_BLOCK_SECONDS], bias = parameters[TA_PCM_BLOCK_ROUNDING_BIAS];
    /* The decoder processes 44.1 and 48 kHz input. Every block must have at least one frame. */
    if (bias < 0 || bias >= 1 || fma(44100, seconds, bias) < 1 || fma(48000, seconds, bias) > INT_MAX)
        ta_fail(c, TA_FAILED, "invalid phrase PCM block size");
}

static void validate_short_break_rank(ta_context *c, const double *parameters)
{
    double count = parameters[TA_SHORT_BREAK_RANK_MIN_COUNT];
    double rank = parameters[TA_SHORT_BREAK_RANK_INDEX];
    if (!valid_phrase_index(count, 1, INT_MAX) || !valid_phrase_index(rank, 0, (int)count - 1))
        ta_fail(c, TA_FAILED, "invalid short break rank");
}

static void validate_phrase_activity_extents(ta_context *c, const double *parameters)
{
    /* Minimum extents protect the bar and change arrays on short inputs. */
    double low_min = fmax(parameters[TA_LOW_ACTIVITY_FILL_MIN_BARS], parameters[TA_LOW_ACTIVITY_CHANGE_MIN_BARS]);
    double middle_tail = parameters[TA_MIDDLE_ACTIVITY_TAIL_BARS];
    double middle_width = parameters[TA_MIDDLE_ACTIVITY_BAR_DIVISOR];
    if (parameters[TA_LOW_ACTIVITY_MIN_EXTENT] < low_min * parameters[TA_LOW_ACTIVITY_BAR_DIVISOR] ||
        parameters[TA_MIDDLE_ACTIVITY_MIN_BAR_EXTENT] <
            (parameters[TA_MIDDLE_ACTIVITY_FILL_MIN_BARS] + middle_tail) * middle_width ||
        parameters[TA_MIDDLE_ACTIVITY_MIN_CHANGE_EXTENT] <
            (parameters[TA_MIDDLE_ACTIVITY_CHANGE_MIN_BARS] + middle_tail) * middle_width ||
        parameters[TA_ALL_ACTIVITY_MIN_EXTENT] < 2 * parameters[TA_ALL_ACTIVITY_BEATS_PER_BAR] ||
        parameters[TA_MIDDLE_PEAK_MIN_EXTENT] < 2 * parameters[TA_MIDDLE_PEAK_BEATS_PER_BAR])
        ta_fail(c, TA_FAILED, "invalid phrase activity extents");
}

static void validate_boundary_window(ta_context *c, const double *parameters)
{
    double width = parameters[TA_BOUNDARY_LOCAL_STRONGEST_COUNT];
    double trim = parameters[TA_BOUNDARY_LOCAL_EDGE_TRIM];
    if (parameters[TA_BOUNDARY_LOCAL_MIN_BARS] < width + trim ||
        parameters[TA_BOUNDARY_LOCAL_INTERIOR_FIRST] > trim ||
        parameters[TA_BOUNDARY_ADJACENT_MIN_RANKS] <= width ||
        parameters[TA_BOUNDARY_LOCAL_EMPTY_COUNT] != 1)
        ta_fail(c, TA_FAILED, "invalid phrase boundary window");
}

static void validate_phrase_positive_indices(ta_context *c, const double *parameters, int first,
                                             int last, int maximum)
{
    for (int i = first; i <= last; ++i)
        if (!valid_phrase_index(parameters[i], 1, maximum))
            ta_fail(c, TA_FAILED, "invalid phrase index table");
}

static void validate_phrase_activity_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_LOW_ACTIVITY_EXTENT_PAD,
                                     TA_LOW_ACTIVITY_QUIET_CENTER, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_MIDDLE_ACTIVITY_EXTENT_PAD,
                                     TA_MIDDLE_ACTIVITY_CHANGE_START, UINT8_MAX);
    int center = (int)parameters[TA_LOW_ACTIVITY_QUIET_CENTER];
    if (!valid_phrase_index(parameters[TA_LOW_ACTIVITY_QUIET_BACK], -center, 0) ||
        !valid_phrase_index(parameters[TA_LOW_ACTIVITY_QUIET_STRIDE], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid low activity sample geometry");
}

static void validate_high_candidate_search(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_LOW_PEAK_GAP_0, TA_LOW_PEAK_GAP_5, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ALL_BAND_GAP_0, TA_ALL_BAND_GAP_7, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_BREAK_EVIDENCE_RECOVERY_0,
                                     TA_BREAK_EVIDENCE_RECOVERY_2, UINT16_MAX);
    if (!valid_phrase_index(parameters[TA_LOW_PEAK_GAP_STRIDE], 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_ALL_BAND_GAP_STRIDE], 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_LOW_PEAK_GAP_STOP_ATTEMPT], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_ALL_BAND_GAP_STOP_ATTEMPT], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid high phrase search bounds");
}

static void validate_verse_alternatives(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_VERSE_SCORE_PRIMARY_OFFSET_INDEX], 0, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_VERSE_SCORE_SECONDARY_OFFSET_INDEX], 0, UINT16_MAX) ||
        parameters[TA_VERSE_SCORE_PRIMARY_OFFSET_INDEX] + 1 >= parameters[TA_ALL_BAND_GAP_0] ||
        parameters[TA_VERSE_SCORE_SECONDARY_OFFSET_INDEX] + 1 >= parameters[TA_ALL_BAND_GAP_1])
        ta_fail(c, TA_FAILED, "invalid verse alternative offsets");
}

static void validate_band_change(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_BAND_CHANGE_ALIGNMENT_BIAS], 0, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_BAND_CHANGE_TAIL_BARS], 0, UINT16_MAX) ||
        parameters[TA_BAND_CHANGE_SUPPRESS_MIN_CANDIDATES] < 2)
        ta_fail(c, TA_FAILED, "invalid band change windows");
}

static void validate_phrase_timing_numbers(ta_context *c, const double *parameters)
{
    /* Bound conversions to int for both supported input rates and all supported BPMs. */
    if (parameters[TA_PHRASE_BLOCK_SECONDS] <= 0 ||
        parameters[TA_PHRASE_BLOCK_SECONDS] > AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS ||
        parameters[TA_FIRST_ACTIVITY_SCAN_SECONDS] <= 0 ||
        parameters[TA_FIRST_ACTIVITY_SCAN_SECONDS] > AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS ||
        parameters[TA_OPENING_HOP_RATE] <= 0 || parameters[TA_OPENING_HOP_RATE] > 1)
        ta_fail(c, TA_FAILED, "invalid phrase timing parameters");
}

static void validate_phrase_vocal_bins(ta_context *c, const double *parameters)
{
    double end = parameters[TA_PHRASE_VOCAL_END_BIN];
    if (!valid_phrase_index(end, 1, INT_MAX) ||
        !valid_phrase_index(parameters[TA_PHRASE_VOCAL_FIRST_BIN], 0, (int)end - 1))
        ta_fail(c, TA_FAILED, "invalid phrase vocal range");
}

static void validate_phrase_fill_ranges(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_FINAL_FILL_TRIM_MIN], 1, INT_MAX) ||
        !valid_phrase_index(parameters[TA_SNARE_PERCENTILE_MIN_BARS], 1, INT_MAX))
        ta_fail(c, TA_FAILED, "invalid phrase fill range");
}

static void validate_opening_snap_bars(ta_context *c, const double *parameters)
{
    /* Opening decisions index the 28-bar level profiles. */
    if (!valid_phrase_index(parameters[TA_OPENING_SNAP_FIRST_BAR], 1, 28) ||
        !valid_phrase_index(parameters[TA_OPENING_SNAP_SECOND_BAR], 1, 28))
        ta_fail(c, TA_FAILED, "invalid phrase opening snap");
}

static void validate_opening_rank(ta_context *c, const double *parameters)
{
    /* The shortlist can lose two neighbors before the three-way ordering. */
    double ranked = parameters[TA_OPENING_LEVEL_RANK_COUNT];
    if (!valid_phrase_index(ranked, 8, 25) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_SHORTLIST_COUNT], 5, (int)ranked) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_FINALIST_COUNT], 3, (int)ranked) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_EARLY_MAX], 1, 3))
        ta_fail(c, TA_FAILED, "invalid opening candidate ranks");
}

static void validate_opening_history(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_OPENING_HISTORY_SHORT_COUNT], 1, 28) ||
        !valid_phrase_index(parameters[TA_OPENING_HISTORY_LONG_COUNT], 1, 28) ||
        parameters[TA_OPENING_LEVEL_ORDER_FIRST_MIN] < 3 ||
        parameters[TA_OPENING_LEVEL_ORDER_GAP] < 1)
        ta_fail(c, TA_FAILED, "invalid opening candidate history");
}

static void validate_snare_geometry(ta_context *c, const double *parameters)
{
    if (parameters[TA_GENRE_SNARE_SCAN_SECONDS] <= 0 ||
        parameters[TA_GENRE_SNARE_SCAN_SECONDS] > AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS ||
        !valid_phrase_index(parameters[TA_GENRE_SNARE_SEPARATION_ADJUSTMENT], 0, INT_MAX))
        ta_fail(c, TA_FAILED, "invalid genre snare parameters");
}

static void validate_attack_delay(ta_context *c, const double *parameters)
{
    double search = parameters[TA_ATTACK_DELAY_MAX_BEATS];
    double valley = parameters[TA_ATTACK_DELAY_VALLEY_BEATS];
    if (search <= 0 || search > 4 || valley <= 0 || valley > search)
        ta_fail(c, TA_FAILED, "invalid rhythm delay windows");
}

static void validate_attack_marking(ta_context *c, const double *parameters)
{
    /* Each pass can consume one of the sixteen available peaks. */
    if (!valid_phrase_index(parameters[TA_ATTACK_MARK_LOW_LAST_PASS], 0, 15) ||
        !valid_phrase_index(parameters[TA_ATTACK_MARK_HIGH_LAST_PASS], 0, 15))
        ta_fail(c, TA_FAILED, "invalid attack marking passes");
}

static void validate_decoration_windows(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_BEAT_FOUR_TAIL_TRIM], 1, INT_MAX) ||
        !valid_phrase_index(parameters[TA_SIMPLE_LOOP_WINDOW_BEATS], 1, 32) ||
        !valid_phrase_index(parameters[TA_SIMPLE_LOOP_MIDPOINT_DIVISOR], 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_SIMPLE_LOOP_MINIMUM_END_LOOKAHEAD], 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_REPEATED_PEAK_WINDOW_SAMPLES], 3, UINT8_MAX) ||
        parameters[TA_REPEATED_PEAK_MIN_COUNT] < 3)
        ta_fail(c, TA_FAILED, "invalid phrase decoration windows");
}

static void validate_hihat_levels(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_HIHAT_LIGHT_LEVEL], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_HIHAT_MEDIUM_LEVEL], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_HIHAT_HEAVY_LEVEL], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid hi-hat levels");
}

static void validate_phrase_outro(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_OUTRO_LEVEL_START_OFFSET], 0, UINT16_MAX) ||
        parameters[TA_VARIATION_MIN_BOUNDARIES_EXCLUSIVE] < 2 ||
        parameters[TA_VARIATION_QUIET_SECOND_MIN_BOUNDARIES] < 4)
        ta_fail(c, TA_FAILED, "invalid outro and variation ranges");
    double phase_period = parameters[TA_OUTRO_TERMINAL_PHASE_PERIOD];
    if (phase_period != parameters[TA_NOMINAL_BEATS_PER_BAR] ||
        !valid_phrase_index(phase_period, 2, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid outro terminal phase period");
    unsigned period = (unsigned)phase_period;
    if ((period & (period - 1)) ||
        !valid_phrase_index(parameters[TA_OUTRO_TERMINAL_PHASE_BIAS], 0, (int)period - 1) ||
        parameters[TA_OUTRO_TERMINAL_ADVANCE] != 1)
        ta_fail(c, TA_FAILED, "invalid outro terminal phase");
}

static void validate_variation_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_VARIATION_MEAN_ENDPOINT_BACK,
                                     TA_VARIATION_REFERENCE_COUNT_BACK, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_VARIATION_OMIT_REFERENCE_START,
                                     TA_VARIATION_DEVIATION_OMIT_ADVANCE, UINT8_MAX);
    double index = parameters[TA_VARIATION_QUIET_MEAN_INDEX];
    if (!valid_phrase_index(index, 0, UINT8_MAX) ||
        index >= parameters[TA_VARIATION_MIN_BOUNDARIES_EXCLUSIVE] ||
        parameters[TA_VARIATION_OMIT_REFERENCE_END_BACK] < 2 ||
        parameters[TA_VARIATION_OMIT_REFERENCE_DIVISOR_BACK] >= parameters[TA_VARIATION_QUIET_SECOND_MIN_BOUNDARIES])
        ta_fail(c, TA_FAILED, "invalid phrase variation geometry");
}

static void validate_rhythm_prototypes(ta_context *c, const double *parameters)
{
    for (int i = TA_RHYTHM_RUN_FOUR_PROTOTYPE_0; i <= TA_RHYTHM_RUN_EIGHT_PROTOTYPE_3; ++i) {
        double word = parameters[i];
        if (word < 0 || word > UINT32_MAX || trunc(word) != word)
            ta_fail(c, TA_FAILED, "invalid rhythm prototype");
    }
}

static bool valid_rhythm_role_mask(double mask)
{
    return mask >= 0 && mask < (double)(UINT64_C(1) << 53) && trunc(mask) == mask;
}

static void validate_rhythm_masks(ta_context *c, const double *parameters)
{
    if (!valid_rhythm_role_mask(parameters[TA_RHYTHM_RUN_ALLOWED_ROLES]) ||
        !valid_rhythm_role_mask(parameters[TA_RHYTHM_GROWTH_ALLOWED_ROLES]))
        ta_fail(c, TA_FAILED, "invalid rhythm role mask");
}

static void validate_rhythm_growth(ta_context *c, const double *parameters)
{
    double group = parameters[TA_RHYTHM_GROWTH_GROUP_BEATS];
    if (parameters[TA_RHYTHM_GROWTH_SHORT_LENGTH] < -parameters[TA_RHYTHM_GROWTH_TWO_OFFSET] * group ||
        parameters[TA_RHYTHM_GROWTH_MEDIUM_LENGTH] < -parameters[TA_RHYTHM_GROWTH_FOUR_OFFSET] * group ||
        parameters[TA_RHYTHM_GROWTH_LONG_LENGTH] < -parameters[TA_RHYTHM_GROWTH_EIGHT_OFFSET] * group)
        ta_fail(c, TA_FAILED, "invalid rhythm growth lengths");
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_GROWTH_START_SHORT,
                                     TA_RHYTHM_GROWTH_START_LONG, UINT16_MAX);
}

static void validate_rhythm_tail(ta_context *c, const double *parameters)
{
    double length = parameters[TA_RHYTHM_RUN_DENSE_LONG_LENGTH];
    const int slots = (int)parameters[TA_RHYTHM_RUN_GROUP_ACTIVITY_SLOTS];
    if (!valid_phrase_index(parameters[TA_RHYTHM_RUN_DENSE_SHORT_START], 0, slots - 1) ||
        !valid_phrase_index(length, 1, slots) ||
        parameters[TA_RHYTHM_RUN_DENSE_SHORT_START] >= length ||
        parameters[TA_RHYTHM_RUN_DENSE_MIN_GROUP] < parameters[TA_RHYTHM_RUN_DENSE_SECOND_BACK])
        ta_fail(c, TA_FAILED, "invalid rhythm dense tail");
    double second_back = parameters[TA_RHYTHM_RUN_DENSE_SECOND_BACK];
    if (!valid_phrase_index(second_back, 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_RHYTHM_RUN_DENSE_PREVIOUS_BIAS], 1, (int)second_back - 1))
        ta_fail(c, TA_FAILED, "invalid rhythm dense history");
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_RUN_DENSE_VALUE,
                                     TA_RHYTHM_RUN_OUTPUT_ROLE, INT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_RUN_OFFSET_SHORT,
                                     TA_RHYTHM_RUN_FALLBACK_THIRD, UINT16_MAX);
}

static void validate_non_high_kinds(ta_context *c, const double *parameters)
{
    const int first = TA_NON_HIGH_MATERIAL_KIND_0;
    const int last = TA_NON_HIGH_MATERIAL_KIND_5;
    for (int i = TA_NON_HIGH_KIND_INTRO; i <= TA_NON_HIGH_KIND_BRIDGE; ++i)
        if (!valid_phrase_index(parameters[i], 1, UINT8_MAX))
            ta_fail(c, TA_FAILED, "invalid non-high phrase kind");
    for (int i = first; i <= last; ++i)
        if (!valid_phrase_index(parameters[i], 1, UINT8_MAX))
            ta_fail(c, TA_FAILED, "invalid non-high material kind");
    if (!valid_phrase_index(parameters[TA_NON_HIGH_MATERIAL_FIRST_INDEX], 0, last - first))
        ta_fail(c, TA_FAILED, "invalid initial material index");
}

static void validate_candidate_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_IRREGULAR_FORWARD_FIRST,
                                     TA_IRREGULAR_FORWARD_THIRD, UINT16_MAX);
    if (!valid_phrase_index(parameters[TA_HIGH_PRUNE_BEATS_PER_BAR], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_HIGH_PRUNE_PREVIOUS_BEAT], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid candidate level geometry");
}

static void validate_repeat_rhythm_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_REPEAT_RHYTHM_SAMPLES_PER_BEAT,
                                     TA_REPEAT_RHYTHM_EARLIER_PEER_START, UINT16_MAX);
    if (parameters[TA_REPEAT_RHYTHM_FIRST_START] > parameters[TA_REPEAT_RHYTHM_FIRST_END] ||
        parameters[TA_REPEAT_RHYTHM_SECOND_START] > parameters[TA_REPEAT_RHYTHM_SECOND_END] ||
        parameters[TA_REPEAT_RHYTHM_NEAR_START] < parameters[TA_REPEAT_RHYTHM_NEAR_END] ||
        parameters[TA_REPEAT_RHYTHM_EARLIER_START] < parameters[TA_REPEAT_RHYTHM_EARLIER_END])
        ta_fail(c, TA_FAILED, "invalid repeated rhythm windows");
}

static void validate_genre_snap_geometry(ta_context *c, const double *parameters)
{
    double bar = parameters[TA_GENRE_SNAP_BEATS_PER_BAR];
    if (!valid_phrase_index(bar, 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_GENRE_SNAP_PREVIOUS_PHASE], 0, (int)bar - 1) ||
        !valid_phrase_index(parameters[TA_GENRE_SNAP_NEXT_PHASE], 0, (int)bar - 1))
        ta_fail(c, TA_FAILED, "invalid genre snap beat geometry");
    double scale = parameters[TA_GENRE_SNAP_BEAT_SCALE];
    double bias = parameters[TA_GENRE_SNAP_ROUND_BIAS];
    if (scale <= 0 || scale > 1 || bias < 0 || bias >= 1)
        ta_fail(c, TA_FAILED, "invalid genre snap rounding");
}

static void validate_reference_timing(ta_context *c, const double *parameters)
{
    const int phase_count = TA_REFERENCE_TIMING_PHASE_4 - TA_REFERENCE_TIMING_PHASE_1 + 1;
    validate_phrase_positive_indices(c, parameters, TA_REFERENCE_TIMING_PHASE_1,
                                     TA_REFERENCE_TIMING_ROLLBACK_PHASE, phase_count);
    double unit = parameters[TA_REFERENCE_TIMING_BPM_UNIT];
    double period = parameters[TA_REFERENCE_TIMING_PERIOD_NUMERATOR];
    if (unit < 0x1p-16 || unit > 1 || period <= 0 || period > INT_MAX)
        ta_fail(c, TA_FAILED, "invalid reference beat timing");
    if (parameters[TA_NOMINAL_BEAT_OFFSET_SCALE] != parameters[TA_STAGE_COORD_SECONDS_PER_MILLISECOND])
        ta_fail(c, TA_FAILED, "inconsistent reference offset units");
}

static bool valid_coordinate_parameters(const double *parameters)
{
    return parameters[0] > 0 && parameters[0] <= 60 &&
           parameters[1] > 0 && parameters[1] <= 0.001 &&
           parameters[2] >= 0 && parameters[2] <= 1 &&
           parameters[3] >= 0 && parameters[3] <= 2 &&
           valid_phrase_index(parameters[4], 1, UINT8_MAX);
}

static void validate_coordinate_parameters(ta_context *c, const double *parameters)
{
    if (!valid_coordinate_parameters(parameters + TA_STAGE_COORD_SECONDS_PER_MINUTE) ||
        !valid_coordinate_parameters(parameters + TA_PHRASE_COORD_SECONDS_PER_MINUTE))
        ta_fail(c, TA_FAILED, "invalid beat coordinate parameters");
}

static bool valid_fft_size(double value)
{
    if (!valid_phrase_index(value, 2, UINT16_MAX))
        return false;
    unsigned size = (unsigned)value;
    return (size & (size - 1)) == 0;
}

static bool valid_fft_lattice(double beats, double rows, double beat_origin, double sample_origin)
{
    return valid_phrase_index(beats, 1, UINT8_MAX) && valid_phrase_index(rows, 1, (int)beats) &&
           (int)beats % (int)rows == 0 && valid_phrase_index(beat_origin, 0, 1) &&
           valid_phrase_index(sample_origin, 0, 1);
}

static bool valid_fft_buffer(double size, double bins, double fixed_samples)
{
    return valid_fft_size(size) && valid_phrase_index(bins, 1, (int)size) &&
           valid_phrase_index(fixed_samples, (int)size, UINT16_MAX);
}

static void validate_phrase_spectra(ta_context *c, const double *parameters)
{
    double vocal_bins = parameters[TA_VOCAL_FFT_OUTPUT_BINS];
    if (!valid_fft_buffer(parameters[TA_VOCAL_FFT_SIZE], vocal_bins,
                          parameters[TA_VOCAL_FFT_BUFFER_FIXED_SAMPLES]) ||
        !valid_fft_buffer(parameters[TA_RECURRENCE_FFT_SIZE], parameters[TA_RECURRENCE_FFT_OUTPUT_BINS],
                          parameters[TA_RECURRENCE_FFT_BUFFER_FIXED_SAMPLES]) ||
        !valid_fft_lattice(parameters[TA_VOCAL_FFT_BEATS_PER_BAR], parameters[TA_VOCAL_FFT_ROWS_PER_BAR],
                           parameters[TA_VOCAL_FFT_BEAT_ORIGIN], parameters[TA_VOCAL_FFT_SAMPLE_ORIGIN]) ||
        !valid_fft_lattice(parameters[TA_RECURRENCE_FFT_BEATS_PER_BAR], parameters[TA_RECURRENCE_FFT_ROWS_PER_BAR],
                           parameters[TA_RECURRENCE_FFT_BEAT_ORIGIN], parameters[TA_RECURRENCE_FFT_SAMPLE_ORIGIN]) ||
        parameters[TA_PHRASE_VOCAL_END_BIN] > vocal_bins ||
        parameters[TA_PHRASE_FFT_WINDOW_CENTER_SCALE] < 0)
        ta_fail(c, TA_FAILED, "invalid phrase FFT geometry");
}

static bool valid_index_order(const double *order, unsigned count)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!valid_phrase_index(order[i], 0, (int)count - 1))
            return false;
        seen |= 1u << (unsigned)order[i];
    }
    return seen == (1u << count) - 1;
}

static void validate_opening_level_rank_slots(ta_context *c, const double *parameters)
{
    if (!valid_index_order(parameters + TA_OPENING_LEVEL_RANK_FIRST_SLOT, TA_OPENING_LEVEL_ORDER_SLOTS))
        ta_fail(c, TA_FAILED, "invalid opening level rank slots");
}

static bool valid_opening_lane_modes(const double *real, const double *imaginary, unsigned lanes)
{
    for (unsigned i = 0; i < lanes; ++i)
        if (!valid_phrase_index(real[i], 0, 1) || !valid_phrase_index(imaginary[i], 0, 1))
            return false;
    return true;
}

static void validate_opening_cqt_order(ta_context *c, const double *parameters, unsigned group)
{
    const double *real = parameters + TA_OPENING_CQT_REAL_PAIR_0;
    const double *imaginary = parameters + TA_OPENING_CQT_IMAG_PAIR_0;
    if (c->windows_model) {
        if (group != TA_OPENING_CQT_PAIR_CAPACITY || !valid_opening_lane_modes(real, imaginary, group))
            ta_fail(c, TA_FAILED, "invalid Windows opening CQT lane modes");
        return;
    }
    unsigned pairs = group / 2;
    if (!valid_index_order(real, pairs) || !valid_index_order(imaginary, pairs))
        ta_fail(c, TA_FAILED, "invalid opening CQT reduction order");
}

static void validate_opening_cqt_accumulation(ta_context *c, const double *parameters)
{
    double group = parameters[TA_OPENING_CQT_FMA_GROUP];
    double center = parameters[TA_OPENING_CQT_CENTER_FRACTION];
    if (!valid_phrase_index(group, 4, TA_OPENING_CQT_PAIR_CAPACITY * 2) || (int)group % 2 ||
        center < 0 || center > 1 || !valid_phrase_index(parameters[TA_OPENING_CQT_HANN_MIRROR_BACK], 1, 2))
        ta_fail(c, TA_FAILED, "invalid opening CQT accumulation dimensions");
    validate_opening_cqt_order(c, parameters, (unsigned)group);
}

static bool valid_grouped_reduction(double group, const double *order, int capacity)
{
    if (!valid_phrase_index(group, 4, capacity * 2))
        return false;
    unsigned width = (unsigned)group;
    return (width & (width - 1)) == 0 && valid_index_order(order, width / 2);
}

static void validate_phrase_reductions(ta_context *c, const double *parameters)
{
    if (c->windows_model)
        return;
    if (!valid_grouped_reduction(parameters[TA_PHRASE_SUM_GROUP], parameters + TA_PHRASE_SUM_PAIR_0,
                                 TA_PHRASE_SUM_PAIR_CAPACITY))
        ta_fail(c, TA_FAILED, "invalid phrase sum reduction order");
    if (!valid_grouped_reduction(parameters[TA_RECURRENCE_DISTANCE_GROUP],
                                 parameters + TA_RECURRENCE_DISTANCE_PAIR_0,
                                 TA_RECURRENCE_DISTANCE_PAIR_CAPACITY))
        ta_fail(c, TA_FAILED, "invalid phrase recurrence reduction order");
}

static bool pack_reduction_pair(unsigned left, unsigned right, unsigned available,
                                unsigned *used, uint8_t *packed)
{
    if (left >= available || right >= available || left == right)
        return false;
    unsigned mask = (1u << left) | (1u << right);
    if (*used & mask)
        return false;
    *used |= mask;
    *packed = (uint8_t)(left | (right << 4));
    return true;
}

static bool decode_reduction_pairs(const uint8_t *pairs, double *order)
{
    uint8_t packed[TA_WINDOWS_REDUCTION_PAIRS];
    unsigned used = 0;
    for (unsigned i = 0; i < TA_WINDOWS_REDUCTION_PAIRS; ++i) {
        if (!pack_reduction_pair(pairs[2 * i], pairs[2 * i + 1], TA_WINDOWS_REDUCTION_LANES + i,
                                 &used, &packed[i]))
            return false;
    }
    for (unsigned i = 0; i < TA_WINDOWS_REDUCTION_PAIRS / 2; ++i)
        order[i] = packed[2 * i] | (unsigned)packed[2 * i + 1] << 8;
    order[TA_WINDOWS_REDUCTION_PAIRS / 2] = -1.0 - packed[TA_WINDOWS_REDUCTION_PAIRS - 1];
    return true;
}

static bool reduction_record_matches(const uint8_t *record, const uint8_t *modes, double width)
{
    return width == record[1] && memcmp(record, modes, TA_WINDOWS_REDUCTION_MODES) == 0;
}

static void read_windows_reduction(ta_context *c, const uint8_t *recipe, double *parameters)
{
    static const uint8_t sum[TA_WINDOWS_REDUCTION_MODES] = {1, TA_WINDOWS_REDUCTION_LANES, 7, 0, 0, 0, 1};
    static const uint8_t recurrence[TA_WINDOWS_REDUCTION_MODES] = {2, TA_WINDOWS_REDUCTION_LANES, 7, 1, 1, 2, 0};
    const uint8_t *first = recipe + TA_WINDOWS_REDUCTION_HEADER_BYTES;
    const uint8_t *second = first + TA_WINDOWS_REDUCTION_RECORD_BYTES;
    if (memcmp(recipe, "PVDIRDX1", 8) || recipe[8] != TA_WINDOWS_REDUCTION_RECORD_BYTES || recipe[9] != 2 ||
        !reduction_record_matches(first, sum, parameters[TA_PHRASE_SUM_GROUP]) ||
        !reduction_record_matches(second, recurrence, parameters[TA_RECURRENCE_DISTANCE_GROUP]) ||
        !decode_reduction_pairs(first + sizeof sum, parameters + TA_PHRASE_SUM_PAIR_0) ||
        !decode_reduction_pairs(second + sizeof recurrence, parameters + TA_RECURRENCE_DISTANCE_PAIR_0))
        ta_fail(c, TA_FAILED, "invalid Windows phrase reduction recipe");
}

static void validate_decimator_sections(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_DECIMATE_FILTER_SECTIONS], 1, TA_DECIMATE_SECTION_CAPACITY))
        ta_fail(c, TA_FAILED, "invalid phrase decimator section count");
}

static void validate_envelope_sections(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ENVELOPE_LOW_LOWPASS_SECTIONS,
                                     TA_ENVELOPE_LOW_LOWPASS_SECTIONS, 1);
    validate_phrase_positive_indices(c, parameters, TA_ENVELOPE_MIDDLE_HIGHPASS_SECTIONS,
                                     TA_ENVELOPE_HIGH_LOWPASS_SECTIONS, TA_ENVELOPE_DAMPING_PAIRS);
    validate_phrase_positive_indices(c, parameters, TA_ENVELOPE_LOW_LEVEL_LOWPASS_SECTIONS,
                                     TA_ENVELOPE_MIDDLE_LEVEL_HIGHPASS_SECTIONS, 1);
    validate_phrase_positive_indices(c, parameters, TA_ENVELOPE_LOW_SMOOTH_SECTIONS,
                                     TA_ENVELOPE_MIDDLE_LEVEL_SMOOTH_SECTIONS, UINT8_MAX);
}

static void validate_opening_cqt(ta_context *c, const double *parameters)
{
    double bins = parameters[TA_OPENING_CQT_BIN_COUNT];
    if (!valid_phrase_index(bins, 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_CQT_FIRST_COMPUTED_BIN], 0, (int)bins - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_CQT_BINS_PER_OCTAVE], 1, (int)bins) ||
        !valid_phrase_index(parameters[TA_OPENING_CQT_HOP_SAMPLES], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid opening CQT geometry");
}

static void validate_spectral_scales(ta_context *c, const double *parameters)
{
    if (parameters[TA_RECURRENCE_FFT_DB_SCALE] <= 0 || parameters[TA_VOCAL_FFT_DB_SCALE] <= 0 ||
        parameters[TA_OPENING_CQT_HANN_GAIN] <= 0 || parameters[TA_OPENING_CQT_HANN_ANGLE] <= 0 ||
        parameters[TA_OPENING_CQT_LENGTH_RATE_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid spectral scales");
    validate_phrase_positive_indices(c, parameters, TA_OPENING_CQT_HANN_LENGTH_BIAS,
                                     TA_OPENING_CQT_BASIS_FIRST_ORDINAL, UINT16_MAX);
}

static void validate_spectral_windows(ta_context *c, const double *parameters)
{
    double smaller = fmin(parameters[TA_RECURRENCE_FFT_SIZE], parameters[TA_VOCAL_FFT_SIZE]);
    if (!valid_phrase_index(parameters[TA_PHRASE_FFT_WINDOW_TERMINAL_BACK], 0, (int)smaller - 1) ||
        parameters[TA_PHRASE_FFT_WINDOW_DENOMINATOR_BIAS] <= 0 ||
        !valid_phrase_index(parameters[TA_OPENING_CQT_HANN_EVEN_SPLIT], 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_CQT_HANN_ODD_SPLIT], 2, UINT8_MAX) ||
        parameters[TA_OPENING_CQT_HANN_ODD_CENTER_GAIN] <= 0)
        ta_fail(c, TA_FAILED, "invalid spectral window parameters");
}

static void validate_nominal_beat_geometry(ta_context *c, const double *parameters)
{
    if (parameters[TA_NOMINAL_BEAT_PERIOD_NUMERATOR] <= 0 || parameters[TA_NOMINAL_BEAT_OFFSET_SCALE] <= 0 ||
        parameters[TA_NOMINAL_BEATS_PER_BAR] * parameters[TA_RHYTHM_BLOCK_BEAT_TO_BAR_SCALE] != 1)
        ta_fail(c, TA_FAILED, "invalid nominal beat geometry");
    validate_phrase_positive_indices(c, parameters, TA_NOMINAL_BEAT_PHASE_ORIGIN,
                                     TA_NOMINAL_BEATS_PER_BAR, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_SUB_FILTER_SECTIONS,
                                     TA_RHYTHM_SUB_FILTER_SECTIONS, UINT8_MAX);
}

static void validate_vocal_vote(ta_context *c, const double *parameters)
{
    double samples = parameters[TA_VOICE_VOTE_BUFFER_BEATS];
    if (!valid_phrase_index(samples, 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_VOICE_VOTE_DENSE_SCAN_BEATS], 1, (int)samples) ||
        !valid_phrase_index(parameters[TA_VOICE_VOTE_FALLBACK_BAR], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid vocal vote range");
    if (parameters[TA_VOICE_VOTE_BAR_SCALE] <= 0 || parameters[TA_VOICE_VOTE_BAR_SCALE] > 1 ||
        parameters[TA_VOICE_VOTE_BAR_BASE] < 1 || parameters[TA_VOICE_VOTE_BAR_BASE] > UINT8_MAX)
        ta_fail(c, TA_FAILED, "invalid vocal vote bar scale");
}

static void validate_voice_dense_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_VOICE_DENSE_LAST_DELTA,
                                     TA_VOICE_SUSTAINED_PRIOR_FIRST, UINT16_MAX);
    int bar_width = (int)parameters[TA_VOICE_OPENING_BAR_WIDTH];
    int delta = ta_max((int)parameters[TA_VOICE_DENSE_LAST_DELTA], bar_width - 1);
    if (parameters[TA_VOICE_VOTE_DENSE_SCAN_BEATS] + delta > parameters[TA_VOICE_VOTE_BUFFER_BEATS])
        ta_fail(c, TA_FAILED, "invalid dense vocal window");
}

static void validate_voice_sustained_windows(ta_context *c, const double *parameters)
{
    double first = parameters[TA_VOICE_SUSTAINED_FIRST_PAIR], second = parameters[TA_VOICE_SUSTAINED_SECOND_PAIR];
    if (!valid_phrase_index(first, -64, 63) || !valid_phrase_index(second, (int)first, 63))
        ta_fail(c, TA_FAILED, "invalid sustained vocal onset sample pairs");
    double start = parameters[TA_VOICE_SUSTAINED_START];
    double end_delta = parameters[TA_VOICE_SUSTAINED_END_COUNTER_DELTA];
    if (parameters[TA_VOICE_SUSTAINED_PRIOR_FIRST] >= start ||
        end_delta - start < second - first + 1 ||
        parameters[TA_VOICE_SUSTAINED_COUNT] + end_delta > parameters[TA_VOICE_VOTE_BUFFER_BEATS])
        ta_fail(c, TA_FAILED, "invalid sustained vocal window");
}

static void validate_opening_voice_scores(ta_context *c, const double *parameters)
{
    double reciprocal = parameters[TA_VOICE_SUSTAINED_MEAN_RECIPROCAL];
    if (parameters[TA_VOICE_OPENING_POSITIVE_MIN_EXCLUSIVE] < 0 ||
        parameters[TA_VOICE_SUSTAINED_PATTERN_PRODUCT_MIN_EXCLUSIVE] < 0 ||
        parameters[TA_VOICE_SUSTAINED_QUIET_SUM] < 0 || reciprocal <= 0 || reciprocal > 1)
        ta_fail(c, TA_FAILED, "invalid opening vocal score parameters");
}

static void validate_voice_bar_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_VOICE_BAR_SCAN_START,
                                     TA_VOICE_BAR_LAST_DELTA, UINT16_MAX);
    int bars = (int)parameters[TA_VOICE_BAR_LAST_DELTA] + 1;
    int width = (int)parameters[TA_VOICE_OPENING_BAR_WIDTH];
    if (bars % width || parameters[TA_VOICE_BAR_SCAN_END] < parameters[TA_VOICE_BAR_SCAN_START] ||
        parameters[TA_VOICE_BAR_SCAN_END] + bars - 1 > parameters[TA_VOICE_VOTE_BUFFER_BEATS] ||
        parameters[TA_VOICE_BAR_MEAN_RECIPROCAL] <= 0 || parameters[TA_VOICE_BAR_PRIOR_RECIPROCAL] <= 0)
        ta_fail(c, TA_FAILED, "invalid vocal bar window");
}

static void validate_voice_sparse_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_VOICE_SPARSE_START_COUNTER,
                                     TA_VOICE_SPARSE_SECOND_NEIGHBOR, UINT16_MAX);
    double back = parameters[TA_VOICE_SPARSE_COUNTER_INDEX_BACK];
    double end = parameters[TA_VOICE_SPARSE_END_COUNTER];
    double farthest = ta_dmax(parameters[TA_VOICE_SPARSE_ACTIVE_LAST],
                             ta_dmax(parameters[TA_VOICE_SPARSE_FIRST_NEIGHBOR],
                                     parameters[TA_VOICE_SPARSE_SECOND_NEIGHBOR]));
    if (back >= parameters[TA_VOICE_SPARSE_START_COUNTER] ||
        parameters[TA_VOICE_SPARSE_START_COUNTER] > end ||
        end + parameters[TA_VOICE_SPARSE_LONG_END_COUNTER_DELTA] > parameters[TA_VOICE_VOTE_BUFFER_BEATS] ||
        end - back + farthest > parameters[TA_VOICE_VOTE_BUFFER_BEATS] ||
        parameters[TA_VOICE_SPARSE_MEAN_RECIPROCAL] <= 0)
        ta_fail(c, TA_FAILED, "invalid sparse vocal window");
}

static void validate_opening_vote_counts(ta_context *c, const double *parameters)
{
    int count = (int)parameters[TA_OPENING_HYPOTHESIS_COUNT];
    if (!valid_phrase_index(parameters[TA_OPENING_VOTE_MAJORITY_LAST_SLOT], 0, count - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_VOTE_MAJORITY_MIN], 1, count) ||
        !valid_phrase_index(parameters[TA_OPENING_VOTE_PAIR_COUNT], 1, count))
        ta_fail(c, TA_FAILED, "invalid opening vote counts");
}

static void validate_opening_selection(ta_context *c, const double *parameters)
{
    int count = (int)parameters[TA_OPENING_HYPOTHESIS_COUNT];
    validate_phrase_positive_indices(c, parameters, TA_OPENING_NEIGHBOR_FIRST_ORDINAL,
                                     TA_OPENING_NEIGHBOR_MIDDLE_ORDINAL, count);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_PRUNE_FIRST_BAR,
                                     TA_OPENING_PRUNE_FIRST_BAR, (int)parameters[TA_OPENING_RATIO_BARS]);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_TIE_EXCLUDED_ORDINAL,
                                     TA_OPENING_TIE_EXCLUDED_ORDINAL, count);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_RESULT_PHASE_BACK,
                                     TA_OPENING_RESULT_BEATS_PER_BAR, UINT8_MAX);
    if (!valid_phrase_index(parameters[TA_OPENING_PRUNE_LOW_SLOT], 0, count - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_TIE_LOW_SLOT], 0, count - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_SELECTION_FIRST_RANK], 0, count - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_TIE_SECOND_RANK], 0, count - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_TIE_UNCHANGED_COUNT], 0, count) ||
        !valid_index_order(parameters + TA_OPENING_ENVELOPE_HYPOTHESIS_SLOT, (unsigned)count))
        ta_fail(c, TA_FAILED, "invalid opening selection layout");
}

static void validate_opening_score_dispatch(ta_context *c, const double *parameters)
{
    double last = parameters[TA_OPENING_SCORE_DISPATCH_LAST_SLOT];
    if (!valid_phrase_index(last, 0, 31) ||
        !valid_phrase_index(parameters[TA_OPENING_SCORE_DISPATCH_FIRST_BAR], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid opening score dispatch range");
    double limit = (double)(UINT64_C(1) << ((unsigned)last + 1));
    for (int i = TA_OPENING_SCORE_MASK_TWO; i <= TA_OPENING_SCORE_MASK_NINE; ++i) {
        double mask = parameters[i];
        if (mask < 0 || mask >= limit || trunc(mask) != mask)
            ta_fail(c, TA_FAILED, "invalid opening score mask");
    }
}

static void validate_opening_anchor_phase(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_ANCHOR_BEATS_PER_BAR,
                                     TA_OPENING_ANCHOR_ORDINAL_BIAS, UINT8_MAX);
    if (!valid_phrase_index(parameters[TA_OPENING_ANCHOR_CANDIDATE_BIAS_AFTER], 0, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_ANCHOR_CANDIDATE_BIAS], 0, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_ANCHOR_PHASE_TAIL_SKIP], 0, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid opening anchor phase");
}

static void validate_opening_padding(ta_context *c, const double *parameters)
{
    double bars = parameters[TA_OPENING_RATIO_BARS];
    double padding = parameters[TA_OPENING_PAD_BARS];
    if (!valid_phrase_index(bars, 28, UINT8_MAX) ||
        !valid_phrase_index(padding, (int)bars, UINT8_MAX) ||
        parameters[TA_VOICE_VOTE_BUFFER_BEATS] > padding * parameters[TA_OPENING_PAD_BEATS_PER_BAR] ||
        parameters[TA_OPENING_HYPOTHESIS_COUNT] != 4)
        ta_fail(c, TA_FAILED, "invalid opening hypothesis dimensions");
    if (!valid_phrase_index(parameters[TA_OPENING_PAD_INITIAL_COUNTER_DISTANCE], 0, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_SHORT_MAX_SKIPPED_BARS], 0, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid opening padding counts");
}

static void validate_opening_repeat(ta_context *c, const double *parameters)
{
    int bars = (int)parameters[TA_OPENING_RATIO_BARS];
    int beats = (int)parameters[TA_VOICE_VOTE_BUFFER_BEATS];
    double nominal = parameters[TA_NOMINAL_BEATS_PER_BAR];
    if (parameters[TA_OPENING_ENVELOPE_BAR_DIVISOR] != nominal ||
        parameters[TA_OPENING_VOCAL_BAR_DIVISOR] != nominal ||
        parameters[TA_OPENING_PAD_BEATS_PER_BAR] != nominal)
        ta_fail(c, TA_FAILED, "invalid opening grid agreement");
    int width = (int)nominal;
    int needed_bars = ta_max(bars, (beats + width - 1) / width);
    if (!valid_phrase_index(parameters[TA_OPENING_PAD_TRIGGER_LAST_BAR], needed_bars - 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_PAD_NUMERATOR_BIAS], needed_bars - 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid opening repeat geometry");
}

static void validate_power_and_tempo(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_POWER_SHORT_MAX_BEATS,
                                     TA_POWER_WINDOW_BARS, UINT16_MAX);
    double width = parameters[TA_POWER_BEATS_PER_BAR];
    if (!valid_phrase_index(parameters[TA_POWER_BAR_SLOTS], 1, UINT16_MAX) ||
        parameters[TA_POWER_WINDOW_BEATS] < parameters[TA_POWER_WINDOW_BARS] * width)
        ta_fail(c, TA_FAILED, "invalid power summary window");
    validate_phrase_positive_indices(c, parameters, TA_TEMPO_POWER_TAIL_BARS,
                                     TA_TEMPO_TRAILING_ONSETS, UINT16_MAX);
    if (width != parameters[TA_NOMINAL_BEATS_PER_BAR] ||
        parameters[TA_TEMPO_SCAN_BAR_BEATS] != width)
        ta_fail(c, TA_FAILED, "invalid tempo scan beat grid");
    if (parameters[TA_TEMPO_ONSET_SECONDS_PER_MILLISECOND] != parameters[TA_STAGE_COORD_SECONDS_PER_MILLISECOND])
        ta_fail(c, TA_FAILED, "invalid tempo scan time unit");
}

static void validate_rhythm_secondary_votes(ta_context *c, const double *parameters)
{
    double short_cutoff = parameters[TA_RHYTHM_SECONDARY_SHORT_CUTOFF];
    double long_cutoff = parameters[TA_RHYTHM_SECONDARY_LONG_CUTOFF];
    if (!valid_phrase_index(short_cutoff, 0, UINT8_MAX - 1) ||
        !valid_phrase_index(long_cutoff, (int)short_cutoff + 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_RHYTHM_ACTIVE_FLAG], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid secondary rhythm votes");
}

static void validate_high_boundary_flag_widths(ta_context *c, const double *parameters)
{
    double width = parameters[TA_NOMINAL_BEATS_PER_BAR];
    if (parameters[TA_LOW_PEAK_FLAG_BEATS_PER_BAR] != width ||
        parameters[TA_BREAK_EVIDENCE_BEATS_PER_BAR] != width ||
        parameters[TA_IRREGULAR_FLAG_BEATS_PER_BAR] != width)
        ta_fail(c, TA_FAILED, "invalid high boundary beat grid");
}

static void validate_high_boundary_indices(ta_context *c, const double *parameters)
{
    int gap_count = TA_LOW_PEAK_GAP_5 - TA_LOW_PEAK_GAP_0 + 1;
    if (!valid_phrase_index(parameters[TA_LOW_PEAK_PRIOR_CHANGE_BACK], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_LOW_PEAK_FLAG_MARK], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_LOW_PEAK_GAP_FIRST_ATTEMPT], 1, gap_count))
        ta_fail(c, TA_FAILED, "invalid low peak evidence geometry");
    double current = parameters[TA_IRREGULAR_CURRENT_CHANGE_BACK];
    if (!valid_phrase_index(current, 1, UINT8_MAX - 1) ||
        !valid_phrase_index(parameters[TA_IRREGULAR_PRIOR_CHANGE_BACK], (int)current + 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid irregular evidence geometry");
}

static void validate_phrase_scan_starts(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_HOOK_PRUNE_FIRST_SCORE], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_ROUTE_INTERIOR_PEAK_LAST_BACK], 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_FINAL_BUILD_EXTENSION_FIRST], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid phrase scan starts");
}

static void validate_high_turn_indices(ta_context *c, const double *parameters)
{
    double seed = parameters[TA_HIGH_TURN_SEED_INDEX];
    double first = parameters[TA_HIGH_TURN_FIRST_INTERIOR_INDEX];
    if (!valid_phrase_index(seed, 0, (int)parameters[TA_HIGH_SHORT_PAIR_COUNT]) ||
        !valid_phrase_index(first, (int)seed + 1, UINT8_MAX) ||
        first >= parameters[TA_HIGH_TURN_SECTION_MIN])
        ta_fail(c, TA_FAILED, "invalid high turn indices");
}

static void validate_fade_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FADE_END_ALIGNMENT,
                                     TA_FADE_REFERENCE_RATIO_COUNT, UINT16_MAX);
    double last = parameters[TA_FADE_FINAL_PAIR_INDEX];
    double lookback = parameters[TA_FADE_TAIL_LOOKBACK];
    int alignment = (int)parameters[TA_FADE_END_ALIGNMENT];
    int64_t first = (int64_t)parameters[TA_FADE_SHORT_INPUT_MAX] + 1;
    int64_t aligned_first = first - first % alignment;
    if (last > UINT8_MAX || lookback < (last + 1) * 2 * parameters[TA_FADE_GROUP_SAMPLES] ||
        lookback > (double)aligned_first ||
        parameters[TA_FADE_REFERENCE_RATIO_COUNT] >= last ||
        parameters[TA_FADE_FIRST_DESCENDING_PAIR] >= last)
        ta_fail(c, TA_FAILED, "invalid fade geometry");
}

static void validate_mood_revision(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_REVISE_FIRST_BEAT,
                                     TA_REVISE_QUARTER_TRIM, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_REVISE_CHANGE_HEAD_SKIP,
                                     TA_REVISE_CHANGE_TAIL_SKIP, UINT16_MAX);
    double mask = parameters[TA_REVISE_DELTA_MASK];
    if (mask < 0 || mask > UINT32_MAX || trunc(mask) != mask ||
        !valid_phrase_index(parameters[TA_REVISE_DELTA_MATCH], 0, INT_MAX))
        ta_fail(c, TA_FAILED, "invalid mood correspondence mask");
    if (parameters[TA_REVISE_FIRST_BEAT] + parameters[TA_REVISE_END_BEATS_SKIPPED] >
        parameters[TA_REVISE_SHORT_TRACK_MAX])
        ta_fail(c, TA_FAILED, "invalid mood revision extent");
    if (parameters[TA_REVISE_BEAT_QUARTERS] != parameters[TA_RHYTHM_BLOCK_POWER_SLOTS])
        ta_fail(c, TA_FAILED, "invalid mood revision quarter grid");
}

static void validate_route_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BLOCK_TAIL_TRIM_CAP,
                                     TA_BLOCK_TAIL_TRIM_GROUP_BEATS, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ROUTE_HISTOGRAM_TRIM_MIN,
                                     TA_ROUTE_HISTOGRAM_TAIL, UINT16_MAX);
    if (parameters[TA_BLOCK_TAIL_TRIM_SCALE] < 0 || parameters[TA_BLOCK_TAIL_TRIM_SCALE] > 1 ||
        parameters[TA_BLOCK_TAIL_TRIM_ROUND_BIAS] < 0 || parameters[TA_BLOCK_TAIL_TRIM_ROUND_BIAS] >= 1)
        ta_fail(c, TA_FAILED, "invalid block tail trim");
    if (parameters[TA_ROUTE_HISTOGRAM_HEAD_LAST] + 1 + parameters[TA_ROUTE_HISTOGRAM_TAIL] >=
        parameters[TA_ROUTE_HISTOGRAM_TRIM_MIN])
        ta_fail(c, TA_FAILED, "invalid route histogram extent");
}

static void validate_compression_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_COMPRESSION_WINDOW_LAST_SLOT,
                                     TA_COMPRESSION_FOLLOWING_SLOT, UINT16_MAX);
    if (parameters[TA_COMPRESSION_MIN_HITS] < 1)
        ta_fail(c, TA_FAILED, "invalid compression hit minimum");
    double scale = parameters[TA_COMPRESSION_PHASE_SLOT_SCALE];
    if (!valid_phrase_index(scale, 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_COMPRESSION_PHASE_BASE_SLOTS], 0, (int)scale))
        ta_fail(c, TA_FAILED, "invalid compression phase geometry");
}

static void validate_slow_rock_windows(ta_context *c, const double *parameters)
{
    double base = parameters[TA_SLOW_ROCK_ORIGIN];
    double tail = parameters[TA_SLOW_ROCK_TAIL_SLOTS];
    if (!valid_phrase_index(base, -256, 255))
        ta_fail(c, TA_FAILED, "invalid slow rock origin");
    for (int i = TA_SLOW_ROCK_QUIET_FIRST; i <= TA_SLOW_ROCK_TERMINAL_LAST; i += 2) {
        if (!valid_phrase_index(parameters[i], (int)base, 255) ||
            !valid_phrase_index(parameters[i + 1], (int)parameters[i], 255) ||
            parameters[i + 1] - base > tail)
            ta_fail(c, TA_FAILED, "invalid slow rock windows");
    }
}

static void validate_slow_rock(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SLOW_ROCK_TAIL_SLOTS,
                                     TA_SLOW_ROCK_NEXT_ORIGIN, UINT16_MAX);
    double counter = parameters[TA_SLOW_ROCK_COUNTER_START];
    if (counter > parameters[TA_SLOW_ROCK_TAIL_SLOTS] ||
        !valid_phrase_index(parameters[TA_SLOW_ROCK_RESET_POSITION], 0, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid slow rock counter");
    validate_slow_rock_windows(c, parameters);
}

static void validate_rhythm_pattern_scan(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_PATTERN_GROUP_BARS,
                                     TA_RHYTHM_PATTERN_OFFSET_BIAS, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_PATTERN_OFFSET_MATCH,
                                     TA_RHYTHM_GROUPS_CAP, UINT16_MAX);
    double mask = parameters[TA_RHYTHM_PATTERN_OFFSET_MASK];
    if (mask < 0 || mask > UINT32_MAX || trunc(mask) != mask ||
        parameters[TA_RHYTHM_PATTERN_BAR_SLOTS] > 32)
        ta_fail(c, TA_FAILED, "invalid rhythm pattern scan");
}

static void validate_rhythm_pattern_masks(ta_context *c, const double *parameters)
{
    unsigned slots = (unsigned)parameters[TA_RHYTHM_PATTERN_BAR_SLOTS];
    double limit = (double)(UINT64_C(1) << slots);
    for (int i = TA_RHYTHM_MASK_KICK_FIRST; i <= TA_RHYTHM_MASK_SPARSE_KICK; ++i) {
        double mask = parameters[i];
        if (mask < 1 || mask >= limit || trunc(mask) != mask)
            ta_fail(c, TA_FAILED, "invalid rhythm pattern mask");
    }
}

static void validate_hook_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HOOK_SEED_MAX_LEADING_MISSING,
                                     TA_HOOK_MERGE_FIRST_BAR, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_HOOK_SCORE_PREVIOUS_MIN_EXCLUSIVE,
                                     TA_HOOK_SCORE_SKIP_BAR, UINT16_MAX);
    if (parameters[TA_HOOK_SEED_BAR_SCALE] <= 0 || parameters[TA_HOOK_SEED_BAR_SCALE] > 1 ||
        parameters[TA_HOOK_INTRO_BAR_SCALE] <= 0 || parameters[TA_HOOK_INTRO_BAR_SCALE] > 1)
        ta_fail(c, TA_FAILED, "invalid hook bar scales");
    if (parameters[TA_HOOK_PRUNE_ZERO_SCORE] <= 0)
        ta_fail(c, TA_FAILED, "invalid hook score floor");
}

static void validate_boundary_adjustment(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SHORT_BREAK_RATIO_END_TRIM,
                                     TA_SHORT_BREAK_RATIO_FIRST_SLOT, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_BOUNDARY_ADJUST_ADJACENCY,
                                     TA_BOUNDARY_ADJUST_OUTPUT_ORIGIN, UINT16_MAX);
}

static void validate_secure_levels(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SECURE_LEVEL_INPUT_MIN,
                                     TA_SECURE_LEVEL_TAIL_BARS, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SECURE_LEVEL_BEATS_PER_BAR,
                                     TA_SECURE_OPENING_BEAT_LIMIT, UINT16_MAX);
    double width = parameters[TA_SECURE_LEVEL_BEATS_PER_BAR];
    if (parameters[TA_SECURE_LEVEL_INPUT_MIN] < width * 2 ||
        parameters[TA_SECURE_MIN_BEATS] < width * parameters[TA_SECURE_CANDIDATE_MIN_BARS] ||
        parameters[TA_SECURE_SUSTAIN_FIRST_OFFSET] > parameters[TA_SECURE_SUSTAIN_FINAL_OFFSET])
        ta_fail(c, TA_FAILED, "invalid secure anchor geometry");
}

static void validate_initial_alternatives(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_INITIAL_SHORT_PHASE_SPAN,
                                     TA_INITIAL_LONG_GAP_MIN, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_INITIAL_BEATS_PER_BAR,
                                     TA_INITIAL_HIGH_ROLE, UINT8_MAX);
}

static void validate_recurrence_frames(ta_context *c, const double *parameters)
{
    double scale = parameters[TA_RECURRENCE_FRAME_SCALE], bias = parameters[TA_RECURRENCE_FRAME_BIAS];
    if (scale <= 0 || scale > 1 || bias < 0 || bias > UINT16_MAX ||
        !valid_phrase_index(parameters[TA_RECURRENCE_FRAME_MIN], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid recurrence frame geometry");
    validate_phrase_positive_indices(c, parameters, TA_RECURRENCE_BRIDGE_ROLE,
                                     TA_RECURRENCE_FIRST_MATERIAL_ROLE, UINT8_MAX);
}

static void validate_recurrence_score_model(ta_context *c, const double *parameters)
{
    double minimum = parameters[TA_RECURRENCE_VARIANCE_MIN_COUNT];
    if (!valid_phrase_index(minimum, 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_RECURRENCE_VARIANCE_COUNT_BACK], 0, (int)minimum - 1) ||
        parameters[TA_RECURRENCE_MATCH_DISTANCE_MAX] < 0 ||
        parameters[TA_RECURRENCE_POSITIVE_SCORE_MIN_EXCLUSIVE] < 0 ||
        parameters[TA_RECURRENCE_INVERSE_DISTANCE_NUMERATOR] <= 0 ||
        parameters[TA_RECURRENCE_IDENTICAL_FRAME_SCORE] <= 0)
        ta_fail(c, TA_FAILED, "invalid recurrence score parameters");
}

static void validate_seed_segments(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SEED_TERMINAL_TRIM,
                                     TA_SEED_ENVELOPE_TERMINAL_SAMPLE_BACK, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SEED_INTRO_ROLE,
                                     TA_SEED_DECLINE_MIN_BOUNDARIES, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SEED_ENVELOPE_START_BACK,
                                     TA_SEED_MIDDLE_START_BACK, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SEED_ENVELOPE_PAD_SCALE,
                                     TA_SEED_BOUNDARY_ORIGIN, UINT8_MAX);
}

static void validate_snare_support(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SNARE_SUPPORT_FIRST_BACK,
                                     TA_SNARE_SUPPORT_CENTER_MARK, UINT8_MAX);
    double after = parameters[TA_SNARE_SUPPORT_SECOND_AFTER];
    if (parameters[TA_SNARE_SUPPORT_FIRST_BACK] > parameters[TA_SNARE_SUPPORT_SECOND_BACK] ||
        parameters[TA_SNARE_SUPPORT_FIRST_AFTER] > after || parameters[TA_SNARE_SUPPORT_THIRD_AFTER] > after)
        ta_fail(c, TA_FAILED, "invalid snare support offsets");
}

static void validate_snare_pattern(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SNARE_PATTERN_GROUP_MIN_EXCLUSIVE,
                                     TA_SNARE_PATTERN_MATCH_TYPE, UINT8_MAX);
    double scale = parameters[TA_SNARE_PATTERN_GROUP_SCALE];
    double length = parameters[TA_SNARE_PATTERN_FIRST_LENGTH] + parameters[TA_SNARE_PATTERN_SECOND_LENGTH];
    size_t capacity = (TA_SNARE_PATTERN_WORD_3 - TA_SNARE_PATTERN_WORD_0 + 1) * sizeof(uint32_t);
    if (!valid_phrase_index(parameters[TA_SNARE_PATTERN_MIN_POSITIONS], 1, UINT16_MAX) ||
        scale <= 0 || scale > 1 || length > capacity)
        ta_fail(c, TA_FAILED, "invalid snare pattern geometry");
    for (int i = TA_SNARE_PATTERN_WORD_0; i <= TA_SNARE_PATTERN_WORD_3; i++)
        if (parameters[i] < 0 || parameters[i] > UINT32_MAX || trunc(parameters[i]) != parameters[i])
            ta_fail(c, TA_FAILED, "invalid snare pattern word");
}

static void validate_snare_rank(ta_context *c, const double *parameters)
{
    double group = parameters[TA_SNARE_RANK_GROUP_BEATS];
    double stride = parameters[TA_SNARE_RANK_WINDOW_POSITION_BYTES];
    if (!valid_phrase_index(group, 2, UINT8_MAX) ||
        !valid_phrase_index(stride, sizeof(int32_t), UINT16_MAX) ||
        (int)stride % sizeof(int32_t) || stride / sizeof(int32_t) >= group)
        ta_fail(c, TA_FAILED, "invalid snare rank geometry");
    double next = parameters[TA_SNARE_RANK_NEXT];
    if (!valid_phrase_index(next, 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_SNARE_RANK_LOOP_TRAILING], (int)next, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_SNARE_RANK_NEXT_NEXT], (int)next + 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid snare rank lookahead");
}

static void validate_vocal_entry_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_VOCAL_ENTRY_BEATS_PER_BAR,
                                     TA_VOCAL_ENTRY_OUTPUT_FIRST_BAR, UINT16_MAX);
    double long_tail = parameters[TA_VOCAL_ENTRY_LONG_SCAN_TAIL];
    double short_tail = parameters[TA_VOCAL_ENTRY_SHORT_SCAN_TAIL];
    double width = parameters[TA_VOCAL_ENTRY_BEATS_PER_BAR];
    double next = parameters[TA_VOCAL_ENTRY_SHORT_NEXT_BAR];
    if (parameters[TA_VOCAL_ENTRY_LONG_MARKER_OFFSET] > long_tail + 1 ||
        parameters[TA_VOCAL_ENTRY_SHORT_MARKER_OFFSET] > short_tail + 1 ||
        short_tail < width * (next + 1))
        ta_fail(c, TA_FAILED, "invalid vocal entry window geometry");
}

static void validate_fill_attachment(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FILL_INTRO_MIN_EXCLUSIVE,
                                     TA_FILL_HOOK_DECORATED_ROLE, UINT16_MAX);
    int build_offset = (int)parameters[TA_FILL_BUILD_FIRST_ROLE_BYTE_OFFSET];
    if (parameters[TA_FILL_INTRO_CUT_END_INDEX] > parameters[TA_FILL_INTRO_MIN_EXCLUSIVE] ||
        build_offset % sizeof(int32_t) ||
        parameters[TA_FILL_BUILD_ROLE_COUNT] > parameters[TA_FILL_BUILD_ROLE_EXCLUSIVE])
        ta_fail(c, TA_FAILED, "invalid fill attachment indices");
}

static void validate_beat_four_decoration(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_BEAT_FOUR_INTRO_FIRST_SECTION], 0, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid beat-four intro scope");
    validate_phrase_positive_indices(c, parameters, TA_BEAT_FOUR_INTRO_BASE_ROLE,
                                     TA_OUTRO_BEAT_FOUR_ROLE_ADVANCE, UINT16_MAX);
}

static void validate_fill_growth_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FILL_GROWTH_WINDOW_COUNT,
                                     TA_FILL_GROWTH_SECTION_END_BACK, UINT16_MAX);
    int offsets = TA_FILL_GROWTH_WINDOW_FOURTH_BYTES - TA_FILL_GROWTH_WINDOW_FIRST_BACK_BYTES + 1;
    if (parameters[TA_FILL_GROWTH_WINDOW_COUNT] != offsets ||
        parameters[TA_FILL_GROWTH_WINDOW_SECOND_BYTES] > parameters[TA_FILL_GROWTH_WINDOW_THIRD_BYTES] ||
        parameters[TA_FILL_GROWTH_WINDOW_THIRD_BYTES] > parameters[TA_FILL_GROWTH_WINDOW_FOURTH_BYTES])
        ta_fail(c, TA_FAILED, "invalid fill growth windows");
    for (int i = TA_FILL_GROWTH_WINDOW_FIRST_BACK_BYTES; i <= TA_FILL_GROWTH_WINDOW_FOURTH_BYTES; i++)
        if ((int)parameters[i] % sizeof(double))
            ta_fail(c, TA_FAILED, "unaligned fill growth window");
}

static void validate_fill_cut_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FILL_CUT_WINDOW_COUNT,
                                     TA_FILL_CUT_WINDOW_SECOND_BACK, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_FILL_CUT_COARSE_BASE_BACK,
                                     TA_FILL_CUT_FINE_MARKER_ADVANCE, UINT16_MAX);
    const int transitions = TA_FILL_CUT_COARSE_OFFSET_2 - TA_FILL_CUT_COARSE_OFFSET_0 + 1;
    double first = parameters[TA_FILL_CUT_WINDOW_FIRST_BACK];
    if (!valid_phrase_index(parameters[TA_FILL_CUT_WINDOW_COUNT], 2, transitions + 1) ||
        parameters[TA_FILL_CUT_WINDOW_FIRST_STOP_BACK] > first ||
        parameters[TA_FILL_CUT_WINDOW_SECOND_BACK] >= first ||
        parameters[TA_FILL_CUT_WINDOW_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid coarse fill cut geometry");
}

static void validate_fine_fill_cut(ta_context *c, const double *parameters)
{
    double anchor = parameters[TA_FILL_CUT_FINE_ANCHOR_BACK];
    double lead = parameters[TA_FILL_CUT_STEADY_FIRST_BEFORE_ANCHOR];
    double first = parameters[TA_FILL_CUT_FINE_FIRST_BEFORE_ANCHOR];
    double history = parameters[TA_FILL_CUT_FINE_HISTORY_BACK];
    double earliest = parameters[TA_FILL_CUT_WINDOW_FIRST_BACK];
    if (lead <= parameters[TA_FILL_CUT_STEADY_STOP_BEFORE_ANCHOR] ||
        anchor + lead > earliest || history > earliest || history <= anchor + first)
        ta_fail(c, TA_FAILED, "invalid fine fill cut geometry");
}

static void validate_high_role_scales(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HIGH_EXPORT_DECORATED_REMAINDER_DIVISOR,
                                     TA_HIGH_EXPORT_FIRST_FAMILY, UINT8_MAX);
    double plain_scale = parameters[TA_HIGH_EXPORT_PLAIN_FAMILY_SCALE];
    double decorated_scale = parameters[TA_HIGH_EXPORT_DECORATED_FAMILY_SCALE];
    if (plain_scale * parameters[TA_HIGH_EXPORT_PLAIN_REMAINDER_DIVISOR] != 1 ||
        decorated_scale * parameters[TA_HIGH_EXPORT_DECORATED_REMAINDER_DIVISOR] != 1)
        ta_fail(c, TA_FAILED, "invalid high export decimal scales");
}

static void validate_high_role_dispatch(ta_context *c, const double *parameters)
{
    if (parameters[TA_HIGH_EXPORT_LAST_FAMILY_SLOT] != TA_HIGH_EXPORT_HANDLER_COUNT - 1)
        ta_fail(c, TA_FAILED, "invalid high export family range");
    unsigned seen = 0;
    for (int i = 0; i < TA_HIGH_EXPORT_HANDLER_COUNT; i++) {
        double handler = parameters[TA_HIGH_EXPORT_DISPATCH_FIRST + i];
        if (!valid_phrase_index(handler, 0, TA_HIGH_EXPORT_HANDLER_COUNT - 1))
            ta_fail(c, TA_FAILED, "invalid high export family handler");
        seen |= 1u << (int)handler;
    }
    if (seen != (1u << TA_HIGH_EXPORT_HANDLER_COUNT) - 1)
        ta_fail(c, TA_FAILED, "missing high export family handler");
}

static void validate_spectral_peak_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SPECTRAL_PEAK_SCAN_MIN,
                                     TA_SPECTRAL_PEAK_SCAN_STEP, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SPECTRAL_PEAK_POSITION_ORIGIN,
                                     TA_SPECTRAL_PEAK_WIDTH_LEFT_BIAS, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SPECTRAL_PEAK_WIDTH_RIGHT_INDEX_BACK,
                                     TA_SPECTRAL_PEAK_WIDTH_RIGHT_SAMPLE_BACK, UINT8_MAX);
    if (parameters[TA_SPECTRAL_PEAK_SCAN_MIN] <= parameters[TA_SPECTRAL_PEAK_SCAN_TAIL] ||
        !valid_phrase_index(parameters[TA_SPECTRAL_PEAK_WIDTH_LEFT_PAIR_BACK], -UINT8_MAX, 0) ||
        parameters[TA_SPECTRAL_PEAK_WIDTH_RIGHT_SAMPLE_BACK] <= parameters[TA_SPECTRAL_PEAK_WIDTH_RIGHT_INDEX_BACK])
        ta_fail(c, TA_FAILED, "invalid spectral peak geometry");
}

static void validate_spectral_peak_model(ta_context *c, const double *parameters)
{
    double scale = parameters[TA_SPECTRAL_PEAK_WIDTH_PROMINENCE_SCALE];
    if (!valid_phrase_index(parameters[TA_SPECTRAL_PEAK_FALLBACK_COUNT], 1, UINT8_MAX) ||
        scale < -1 || scale >= 0)
        ta_fail(c, TA_FAILED, "invalid spectral peak parameters");
}

static void validate_outro_activity_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OUTRO_ACTIVE_SUBDIVISIONS,
                                     TA_OUTRO_ACTIVITY_MEAN_END_BACK, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_OUTRO_ACTIVITY_LATEST_LEVEL_BACK,
                                     TA_OUTRO_LAST_AUDIBLE_ADVANCE, UINT16_MAX);
    double first = parameters[TA_OUTRO_ACTIVITY_MEAN_START_BACK];
    double last = parameters[TA_OUTRO_ACTIVITY_MEAN_END_BACK];
    double latest = parameters[TA_OUTRO_ACTIVITY_LATEST_LEVEL_BACK];
    double prior = parameters[TA_OUTRO_ACTIVITY_PRIOR_LEVEL_BACK];
    if (first < last || last < latest || prior < latest || prior > first)
        ta_fail(c, TA_FAILED, "invalid outro activity window");
}

static void validate_outro_acceleration_window(ta_context *c, const double *parameters)
{
    double second = parameters[TA_OUTRO_ACCEL_NEXT_SECOND_DELTA];
    if (!valid_phrase_index(second, 2, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_OUTRO_ACCEL_TAIL_FIRST_DELTA], (int)second + 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_OUTRO_FALL_SNAP_CLEARANCE], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid outro acceleration window");
}

static void validate_outro_snap_geometry(ta_context *c, const double *parameters)
{
    double period = parameters[TA_OUTRO_SNAP_PHASE_PERIOD];
    double first = parameters[TA_OUTRO_SNAP_FIRST_REMAINDER], second = parameters[TA_OUTRO_SNAP_SECOND_REMAINDER];
    if (!valid_phrase_index(period, 2, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OUTRO_SNAP_PHASE_ORIGIN], 1, (int)period) ||
        !valid_phrase_index(first, 0, (int)period - 1) || !valid_phrase_index(second, 0, (int)period - 1) ||
        first == second || parameters[TA_OUTRO_SNAP_FIRST_NEAR_BACK] > parameters[TA_OUTRO_SNAP_FIRST_FAR_BACK] ||
        parameters[TA_OUTRO_SNAP_SECOND_NEAR_BACK] > parameters[TA_OUTRO_SNAP_SECOND_FAR_BACK])
        ta_fail(c, TA_FAILED, "invalid outro phase snap geometry");
    validate_phrase_positive_indices(c, parameters, TA_OUTRO_SNAP_FIRST_MIN_INDEX,
                                     TA_OUTRO_SNAP_SECOND_DISTANCE, UINT8_MAX);
}

static void validate_rhythm_block_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_BLOCK_ATTACK_SLOTS,
                                     TA_RHYTHM_BLOCK_CARRIED_COORDINATE_ADVANCE, UINT8_MAX);
    enum { phases = TA_REFERENCE_TIMING_PHASE_4 - TA_REFERENCE_TIMING_PHASE_1 + 1 };
    double quarters = parameters[TA_RHYTHM_BLOCK_POWER_SLOTS];
    double attack_slots = parameters[TA_RHYTHM_BLOCK_ATTACK_SLOTS];
    if (quarters != phases || attack_slots != phases * phases ||
        parameters[TA_RHYTHM_BLOCK_BEAT_TO_BAR_SCALE] * attack_slots != quarters ||
        parameters[TA_RHYTHM_BLOCK_RESET_PHASE] > phases)
        ta_fail(c, TA_FAILED, "invalid rhythm block geometry");
    if (!valid_phrase_index(parameters[TA_RHYTHM_BLOCK_COMPLETED_BEAT_BIAS], 1, phases))
        ta_fail(c, TA_FAILED, "invalid rhythm block beat bias");
}

static void validate_opening_envelope_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_ENVELOPE_GROUP_BEATS,
                                     TA_OPENING_ENVELOPE_GROUP_LAST_DELTA, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_ENVELOPE_RATIO_SLOTS,
                                     TA_OPENING_ENVELOPE_STABILITY_LONG_DELTA, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_ENVELOPE_SHORT_DEVIATION_WIDTH,
                                     TA_OPENING_ENVELOPE_LONG_DEVIATION_WIDTH, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_ENVELOPE_DEVIATION_END_GUARD,
                                     TA_OPENING_ENVELOPE_SUCCESSOR_DISTANCE, UINT8_MAX);
    double ratios = parameters[TA_OPENING_ENVELOPE_RATIO_SLOTS];
    if (parameters[TA_OPENING_ENVELOPE_GROUP_LAST_DELTA] >= parameters[TA_OPENING_ENVELOPE_GROUP_BEATS] ||
        parameters[TA_OPENING_ENVELOPE_SCAN_SLOTS] > ratios || ratios >= parameters[TA_OPENING_RATIO_BARS] ||
        parameters[TA_OPENING_ENVELOPE_CHANGE_MIN] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening envelope geometry");
}

static void validate_opening_envelope_normalization(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_OPENING_ENVELOPE_PHASE_ORIGIN], 0, 1) ||
        parameters[TA_OPENING_ENVELOPE_NORMALIZE_NUMERATOR] <= 0 ||
        parameters[TA_OPENING_ENVELOPE_NORMALIZE_FALLBACK_TEST] < 0)
        ta_fail(c, TA_FAILED, "invalid opening envelope normalization");
}

static void validate_beat_band_geometry(ta_context *c, const double *parameters)
{
    double quarters = parameters[TA_BEAT_LOW_QUARTER_COUNT];
    double eighths = parameters[TA_BEAT_HIGH_EIGHTH_COUNT];
    if (!valid_phrase_index(quarters, 1, TA_BEAT_QUARTER_ENDPOINTS) ||
        quarters != parameters[TA_RHYTHM_BLOCK_POWER_SLOTS] || eighths != 2 * quarters ||
        !valid_phrase_index(parameters[TA_BEAT_HIGH_INTERIOR_ADVANCE], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid beat subdivision");
    validate_phrase_positive_indices(c, parameters, TA_BEAT_LOW_LEVEL_PERIOD_NUMERATOR,
                                     TA_BEAT_LOW_PEAK_PERIOD_NUMERATOR, UINT8_MAX);
    if (!valid_phrase_index(parameters[TA_BEAT_HIGH_PEAK_PERIOD_NUMERATOR], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid high beat period");
}

static void validate_beat_endpoints(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_BEAT_BLOCK_RESET_OFFSET_MS], 0, UINT16_MAX) ||
        parameters[TA_LOW_PEAK_FALL_FACTOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid beat endpoint or change factor");
    validate_phrase_positive_indices(c, parameters, TA_BEAT_BLOCK_LOW_SAMPLE_BACK,
                                     TA_BEAT_QUARTER_LAST_SAMPLE_BACK_3, UINT8_MAX);
}

static void validate_beat_grid_advances(ta_context *c, const double *parameters)
{
    double trailing = parameters[TA_PHRASE_COORD_GRID_TRAILING_COUNT];
    if (!valid_phrase_index(trailing, 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid beat coordinate grid extent");
    validate_phrase_positive_indices(c, parameters, TA_BEAT_LOW_GRID_ADVANCE,
                                     TA_BEAT_HIGH_GRID_ADVANCE, (int)trailing);
}

static void validate_beat_four_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BEAT_FOUR_ORDINARY_END_BACK,
                                     TA_BEAT_FOUR_MEAN_LAST_BACK, UINT8_MAX);
    if (parameters[TA_BEAT_FOUR_QUARTERS_PER_BEAT] != parameters[TA_BEAT_LOW_QUARTER_COUNT] ||
        parameters[TA_BEAT_FOUR_EQUAL_PEAK_SCORE] < 0 ||
        !valid_phrase_index(parameters[TA_BEAT_FOUR_ADJACENCY_MASK], 0, INT_MAX) ||
        !valid_phrase_index(parameters[TA_BEAT_FOUR_ADJACENCY_VALUE], 0, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid beat-four correspondence geometry");
}

static void validate_beat_four_stability(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BEAT_FOUR_STABILITY_TAIL_COUNT,
                                     TA_BEAT_FOUR_STABILITY_DIVISOR_BACK, UINT8_MAX);
    double minimum = parameters[TA_BEAT_FOUR_STABILITY_MIN_COUNT];
    if (parameters[TA_BEAT_FOUR_STABILITY_START] >= minimum ||
        parameters[TA_BEAT_FOUR_STABILITY_DIVISOR_BACK] >= minimum)
        ta_fail(c, TA_FAILED, "invalid beat-four stability geometry");
}

static void validate_opening_first_frame(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_OPENING_START_PHASE_ORIGIN], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_OPENING_START_OFFSET_MS], 0, UINT16_MAX) ||
        parameters[TA_OPENING_START_PERIOD_NUMERATOR] <= 0 || parameters[TA_OPENING_START_OFFSET_SCALE] <= 0 ||
        parameters[TA_OPENING_HOP_RATE] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening frame parameters");
}

static void validate_attack_window_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_DELAY_MIN_BARS,
                                     TA_ATTACK_DELAY_TRIAL_LIMIT, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_TERMINAL_INDEX_BACK,
                                     TA_ATTACK_ORDINARY_PERIOD_NUMERATOR, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_BAR_BEAT_DIVISOR,
                                     TA_ATTACK_COORDINATE_ORIGIN, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_BAR_PEAK_COUNT,
                                     TA_ATTACK_ORDINARY_FIRST_CARRY_BLOCK, UINT8_MAX);
    double beats = parameters[TA_ATTACK_BAR_BEAT_DIVISOR];
    if (beats != parameters[TA_BEAT_LOW_QUARTER_COUNT] ||
        parameters[TA_ATTACK_BAR_PEAK_COUNT] != parameters[TA_RHYTHM_BLOCK_ATTACK_SLOTS] ||
        parameters[TA_ATTACK_GRID_BAR_PERIOD_COUNT] != beats ||
        parameters[TA_ATTACK_ORDINARY_BAR_PERIOD_COUNT] != beats)
        ta_fail(c, TA_FAILED, "invalid attack bar geometry");
}

static void validate_attack_window_fractions(ta_context *c, const double *parameters)
{
    const int indices[] = {
        TA_ATTACK_GRID_QUARTER_FRACTION, TA_ATTACK_ORDINARY_QUARTER_FRACTION,
        TA_ATTACK_GRID_OFFSET_SCALE, TA_ATTACK_ORDINARY_OFFSET_SCALE,
        TA_ATTACK_GRID_OFFSET_ROUND_BIAS, TA_ATTACK_ORDINARY_ROUND_BIAS,
        TA_ATTACK_GRID_PEAK_ROUND_BIAS, TA_ATTACK_DELAY_RESULT_ROUND_BIAS,
        TA_ATTACK_DELAY_VALLEY_MIDPOINT_SCALE, TA_ATTACK_DELAY_BAR_POSITION_SCALE,
    };
    for (size_t i = 0; i < sizeof indices / sizeof *indices; i++) {
        double scale = parameters[indices[i]];
        if (scale <= 0 || scale > 1)
            ta_fail(c, TA_FAILED, "invalid attack window fraction");
    }
}

static void validate_rhythm_power_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_POWER_LOW_NEXT_MARGIN,
                                     TA_RHYTHM_POWER_SAMPLE_BACK_BYTES, UINT16_MAX);
    if (parameters[TA_RHYTHM_POWER_SAMPLE_BACK_BYTES] != sizeof(double) ||
        parameters[TA_RHYTHM_POWER_PERIOD_NUMERATOR] <= 0 ||
        parameters[TA_RHYTHM_POWER_SCALE] <= 0 ||
        parameters[TA_RHYTHM_POWER_QUARTER_FRACTION] <= 0 ||
        parameters[TA_RHYTHM_POWER_QUARTER_FRACTION] > 1 ||
        parameters[TA_RHYTHM_POWER_ROUND_BIAS] < 0 || parameters[TA_RHYTHM_POWER_ROUND_BIAS] >= 1)
        ta_fail(c, TA_FAILED, "invalid rhythm power window");
}

static void validate_attack_sample_rates(ta_context *c, const double *parameters)
{
    for (int i = TA_ATTACK_FULL_FIRST_RATE_MIN; i < TA_ATTACK_FULL_FILTER_COUNT; i += 2)
        if (parameters[i] <= 0 || parameters[i + 1] <= parameters[i])
            ta_fail(c, TA_FAILED, "invalid attack sample-rate range");
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_FULL_FILTER_COUNT,
                                     TA_ATTACK_FULL_FILTER_COUNT, TA_ATTACK_FILTER_SECTION_CAPACITY);
}

static void validate_short_break_positions(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SHORT_BREAK_SCAN_MIN_COUNT,
                                     TA_SHORT_BREAK_REMAP_BAR_BIAS, UINT16_MAX);
    double first = parameters[TA_SHORT_BREAK_SCAN_FIRST];
    double bias = parameters[TA_SHORT_BREAK_SCAN_RESULT_BIAS];
    double preceding = parameters[TA_SHORT_BREAK_PRECEDING_FIRST_BASE];
    double tail = parameters[TA_SHORT_BREAK_RATIO_END_TRIM];
    if (preceding >= first || parameters[TA_SHORT_BREAK_PRECEDING_NEXT_BACK] > preceding ||
        parameters[TA_SHORT_BREAK_RECOVERY_REFERENCE_BACK] > bias ||
        bias - first + parameters[TA_SHORT_BREAK_RECOVERY_LAST] > tail ||
        parameters[TA_SHORT_BREAK_EXCLUDED_PHASE_FIRST] + parameters[TA_SHORT_BREAK_EXCLUDED_PHASE_DELTA] >=
            parameters[TA_SHORT_BREAK_REMAP_BAR_WIDTH])
        ta_fail(c, TA_FAILED, "invalid short-break scan coordinates");
}

static void validate_hihat_islands(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HIHAT_ISLAND_FIRST_LENGTH,
                                     TA_HIHAT_ISLAND_SCAN_FIRST, UINT16_MAX);
    int previous = TA_HIHAT_ISLAND_PREVIOUS_OFFSET;
    double first = parameters[TA_HIHAT_ISLAND_SCAN_FIRST];
    if (!valid_phrase_index(parameters[previous], -(int)first, -1) ||
        parameters[TA_HIHAT_ISLAND_FIRST_MIN_REMAINDER] > parameters[TA_HIHAT_ISLAND_SECOND_MIN_REMAINDER] ||
        parameters[TA_HIHAT_ISLAND_SECOND_MIN_REMAINDER] > parameters[TA_HIHAT_ISLAND_THIRD_MIN_REMAINDER])
        ta_fail(c, TA_FAILED, "invalid hi-hat island coordinates");
}

static void validate_hihat_changes(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HIHAT_BEATS_PER_BAR,
                                     TA_HIHAT_CHANGE_FIRST_BAR, UINT16_MAX);
    int first = (int)parameters[TA_HIHAT_CHANGE_FIRST_BAR];
    if (!valid_phrase_index(parameters[TA_HIHAT_CHANGE_PREVIOUS_OFFSET], -first, -1) ||
        parameters[TA_HIHAT_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_HIHAT_CHANGE_MARK] > UINT8_MAX ||
        !valid_phrase_index(parameters[TA_HIHAT_SPARSE_LEVEL], 0, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid hi-hat change coordinates");
}

static void validate_hihat_envelopes(ta_context *c, const double *parameters)
{
    if (parameters[TA_HIHAT_INVERSE_RATE_NUMERATOR] <= 0 || parameters[TA_HIHAT_ENVELOPE_INITIAL] < 0 ||
        !valid_phrase_index(parameters[TA_HIHAT_FIRST_GAP_ORIGIN], 0, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_HIHAT_LAST_BEAT_BACK], 1, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_HIHAT_BEAT_HIT_INCREMENT], 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_HIHAT_HIT_POSITION_BIAS], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid hi-hat envelope or beat count");
}

static void validate_attack_peak_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_DELAY_CANDIDATE_BACK,
                                     TA_ATTACK_DELAY_FORWARD_STEP, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_DELAY_BACKWARD_STEP,
                                     TA_ATTACK_DELAY_REGION_EXTRA, UINT8_MAX);
    if (!valid_phrase_index(parameters[TA_ATTACK_DELAY_BACKWARD_PAIR_FIRST], -UINT8_MAX, -1))
        ta_fail(c, TA_FAILED, "invalid backward attack probe");
}

static void validate_attack_valley_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ATTACK_DELAY_FIRST_VALLEY_STEP,
                                     TA_ATTACK_DELAY_SEARCH_DISCARD_BARS, UINT16_MAX);
    if (parameters[TA_ATTACK_DELAY_VALLEY_PREVIOUS_BACK] <= parameters[TA_ATTACK_DELAY_VALLEY_FIRST_BACK] ||
        parameters[TA_ATTACK_DELAY_SEARCH_DISCARD_BARS] >= parameters[TA_ATTACK_DELAY_MIN_BARS] ||
        parameters[TA_ATTACK_DELAY_PERIOD_INVERSE_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid attack valley coordinates");
}

static void validate_spectral_vocal_scales(ta_context *c, const double *parameters)
{
    for (int i = TA_PHRASE_VOCAL_BIN_EXPONENT_SCALE; i <= TA_PHRASE_VOCAL_ROW_EXPONENT_SCALE; i++)
        if (parameters[i] <= 0)
            ta_fail(c, TA_FAILED, "invalid spectral vocal scale");
    if (!valid_phrase_index(parameters[TA_PHRASE_VOCAL_FILTER_SECTIONS], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid spectral vocal filter sections");
}

static void validate_boundary_source(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BOUNDARY_SOURCE_GROUP_BEATS,
                                     TA_BOUNDARY_SOURCE_TAIL_GROUPS, UINT16_MAX);
    if (parameters[TA_BOUNDARY_SOURCE_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_BOUNDARY_SOURCE_MIN_SAMPLES] < parameters[TA_BOUNDARY_SOURCE_GROUP_BEATS] ||
        parameters[TA_BOUNDARY_SOURCE_MEAN_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid boundary source windows");
}

static void validate_boundary_ratio_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BOUNDARY_NORMALIZATION_TAIL,
                                     TA_BOUNDARY_RATIO_PREVIOUS_BACK, UINT16_MAX);
    double first = parameters[TA_BOUNDARY_RATIO_FIRST];
    if (parameters[TA_BOUNDARY_RATIO_PREVIOUS_BACK] > first ||
        parameters[TA_BOUNDARY_RATIO_OMITTED] < first ||
        parameters[TA_BOUNDARY_RATIO_INVERSE_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid boundary ratio windows");
}

static void validate_noise_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_NOISE_GROUP_BEATS,
                                     TA_NOISE_POSITION_ADVANCE, UINT16_MAX);
    double last = parameters[TA_NOISE_RECOVERY_LAST];
    if (parameters[TA_NOISE_MEAN_MIN_INPUT] < parameters[TA_NOISE_GROUP_BEATS] ||
        parameters[TA_NOISE_RATIO_PREVIOUS_BACK] > parameters[TA_NOISE_RATIO_FIRST] ||
        parameters[TA_NOISE_SCAN_CONTRAST_OFFSET] > last || parameters[TA_NOISE_RECOVERY_FIRST] > last ||
        parameters[TA_NOISE_POSITION_ADVANCE] > last)
        ta_fail(c, TA_FAILED, "invalid noise analysis windows");
    for (int i = TA_NOISE_MEAN_SCALE; i <= TA_NOISE_RECOVERY_SCALE; i++)
        if (parameters[i] <= 0)
            ta_fail(c, TA_FAILED, "invalid noise analysis scale");
}

static void validate_local_boundary_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BOUNDARY_ADJACENT_DISTANCE,
                                     TA_BOUNDARY_FOLLOWING_LAST, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_BOUNDARY_LOCAL_FIRST_BAR_BACK,
                                     TA_BOUNDARY_LOCAL_ZERO_TRIM_LIMIT, UINT16_MAX);
    double trim = parameters[TA_BOUNDARY_LOCAL_EDGE_TRIM];
    double last = parameters[TA_BOUNDARY_FOLLOWING_LAST];
    if (last >= trim || parameters[TA_BOUNDARY_SHAPE_LAST] >= trim ||
        parameters[TA_BOUNDARY_LOCAL_REFERENCE_DELTA] >= trim ||
        parameters[TA_BOUNDARY_FOLLOWING_FIRST] > parameters[TA_BOUNDARY_FOLLOWING_SECOND] ||
        parameters[TA_BOUNDARY_FOLLOWING_SECOND] > last)
        ta_fail(c, TA_FAILED, "invalid local boundary offsets");
}

static void validate_local_boundary_scales(ta_context *c, const double *parameters)
{
    for (int i = TA_BOUNDARY_SHAPE_PAIR_SCALE; i <= TA_BOUNDARY_NEIGHBOR_LEVEL_ZERO; i++)
        if (parameters[i] <= 0)
            ta_fail(c, TA_FAILED, "invalid local boundary scale");
    if (parameters[TA_BOUNDARY_LOCAL_ZERO_MEAN] <= 0)
        ta_fail(c, TA_FAILED, "invalid local boundary normalization floor");
}

static void validate_first_activity_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FIRST_ACTIVITY_MIN_SCAN_SAMPLES,
                                     TA_FIRST_ACTIVITY_SCAN_TAIL, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_FIRST_ACTIVITY_DISPLACEMENT_BIAS,
                                     TA_FIRST_ACTIVITY_RESULT_BIAS, UINT16_MAX);
    double bias = parameters[TA_FIRST_ACTIVITY_COORDINATE_BIAS];
    if (parameters[TA_FIRST_ACTIVITY_BEAT_SECONDS] <= 0 || bias < 0 || bias >= 1 ||
        parameters[TA_FIRST_ACTIVITY_OFFSET_SCALE] <= 0 ||
        parameters[TA_FIRST_ACTIVITY_SCAN_TAIL] > parameters[TA_FIRST_ACTIVITY_MIN_SCAN_SAMPLES])
        ta_fail(c, TA_FAILED, "invalid first-activity scan geometry");
}

static void validate_activity_bar_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ALL_ACTIVITY_EXTENT_PAD,
                                     TA_ALL_ACTIVITY_TAIL_BARS, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_MIDDLE_PEAK_NORM_TAIL,
                                     TA_MIDDLE_PEAK_TAIL_BARS, UINT16_MAX);
    double origin = parameters[TA_REFERENCE_TIMING_PHASE_1];
    if (parameters[TA_ALL_ACTIVITY_PHASE_ORIGIN] != origin ||
        parameters[TA_MIDDLE_PEAK_PHASE_ORIGIN] != origin ||
        parameters[TA_MIDDLE_PEAK_NORM_TAIL] >= parameters[TA_MIDDLE_PEAK_MIN_NORM_COUNT])
        ta_fail(c, TA_FAILED, "invalid activity bar indices");
}

static void validate_activity_bar_scales(ta_context *c, const double *parameters)
{
    if (parameters[TA_ALL_ACTIVITY_BAR_MEAN] <= 0 || parameters[TA_MIDDLE_PEAK_BAR_MEAN] <= 0 ||
        parameters[TA_ALL_ACTIVITY_INVERSE_NUMERATOR] <= 0 || parameters[TA_MIDDLE_PEAK_INVERSE_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid activity bar scale");
}

static void validate_quarter_rounding(ta_context *c, const double *parameters, int scale, int bias)
{
    if (parameters[scale] <= 0 || parameters[scale] > 1 || parameters[bias] < 0 || parameters[bias] >= 1)
        ta_fail(c, TA_FAILED, "invalid rounded bar conversion");
}

static void validate_opening_level_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_LEVEL_BAR_DIVISOR,
                                     TA_OPENING_LEVEL_MIN_BEATS, UINT16_MAX);
    validate_phrase_positive_indices(c, parameters, TA_OPENING_LEVEL_BAR_ORDINAL_BIAS,
                                     TA_OPENING_LEVEL_ZERO_DIVISOR, UINT16_MAX);
    double width = parameters[TA_OPENING_LEVEL_BAR_DIVISOR];
    if (parameters[TA_OPENING_LEVEL_MIN_BEATS] < width ||
        parameters[TA_OPENING_LEVEL_FRAME_BAR_SCALE] * parameters[TA_OPENING_LEVEL_FRAME_INVERSE_NUMERATOR] != width ||
        parameters[TA_OPENING_LEVEL_FRAME_ROUND_BIAS] < 0 || parameters[TA_OPENING_LEVEL_FRAME_ROUND_BIAS] >= 1 ||
        parameters[TA_OPENING_LEVEL_BAR_ORDINAL_BIAS] != 1 ||
        parameters[TA_OPENING_LEVEL_NORMALIZE_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening frame windows");
}

static void validate_opening_presence_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_PRESENCE_GROUP_BEATS,
                                     TA_OPENING_PRESENCE_PREVIOUS_BACK, UINT8_MAX);
    if (parameters[TA_OPENING_PRESENCE_GROUP_BEATS] != parameters[TA_OPENING_LEVEL_BAR_DIVISOR] ||
        parameters[TA_OPENING_PRESENCE_MARK] != 1)
        ta_fail(c, TA_FAILED, "invalid opening presence geometry");
}

static void validate_band_change_coordinates(ta_context *c, const double *parameters)
{
    validate_quarter_rounding(c, parameters, TA_BAND_CHANGE_EXTENT_SCALE, TA_BAND_CHANGE_EXTENT_BIAS);
    validate_quarter_rounding(c, parameters, TA_BAND_CHANGE_START_SCALE, TA_BAND_CHANGE_START_BIAS);
    validate_phrase_positive_indices(c, parameters, TA_BAND_CHANGE_GROUP_BEATS,
                                     TA_BAND_CHANGE_FIRST_BAR, UINT8_MAX);
    if (parameters[TA_BAND_CHANGE_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_BAND_CHANGE_PRIOR_BACK] <= parameters[TA_BAND_CHANGE_CURRENT_BACK] ||
        !valid_phrase_index(parameters[TA_BAND_CHANGE_FIRST_STATE], 0, 1))
        ta_fail(c, TA_FAILED, "invalid band change coordinates");
}

static void validate_verse_score_geometry(ta_context *c, const double *parameters)
{
    validate_quarter_rounding(c, parameters, TA_VERSE_SCORE_BAR_SCALE, TA_VERSE_SCORE_BAR_BIAS);
    validate_phrase_positive_indices(c, parameters, TA_VERSE_SCORE_CURRENT_BACK,
                                     TA_VERSE_SCORE_BAR_LOOKBACK, UINT8_MAX);
    double oldest = -parameters[TA_VERSE_SCORE_MARK_OLDEST_OFFSET];
    if (!valid_phrase_index(oldest, 2, UINT8_MAX) || parameters[TA_VERSE_SCORE_PEAK_LOOKBACK] > oldest ||
        parameters[TA_VERSE_SCORE_OPENING_LOOKBACK] > oldest ||
        parameters[TA_VERSE_SCORE_PRIOR_BACK] > parameters[TA_VERSE_SCORE_OPENING_LOOKBACK] ||
        parameters[TA_VERSE_SCORE_CURRENT_BACK] >= parameters[TA_VERSE_SCORE_PRIOR_BACK] ||
        parameters[TA_VERSE_SCORE_CONTRAST_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid verse score geometry");
}

static void validate_all_band_coordinates(ta_context *c, const double *parameters)
{
    validate_quarter_rounding(c, parameters, TA_ALL_BAND_EXTENT_SCALE, TA_ALL_BAND_EXTENT_BIAS);
    validate_phrase_positive_indices(c, parameters, TA_ALL_BAND_GROUP_BEATS,
                                     TA_ALL_BAND_FLAG_MARK, UINT8_MAX);
    if (parameters[TA_ALL_BAND_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_ALL_BAND_PREVIOUS_BACK] <= parameters[TA_ALL_BAND_CURRENT_BACK])
        ta_fail(c, TA_FAILED, "invalid all-band sample coordinates");
}

static void validate_all_band_attempts(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_ALL_BAND_FIRST_ATTEMPT,
                                     TA_ALL_BAND_SECONDARY_ATTEMPT, UINT16_MAX);
    if (parameters[TA_ALL_BAND_PRIMARY_ATTEMPT] < parameters[TA_ALL_BAND_FIRST_ATTEMPT] ||
        parameters[TA_ALL_BAND_SECONDARY_ATTEMPT] <= parameters[TA_ALL_BAND_PRIMARY_ATTEMPT] ||
        parameters[TA_ALL_BAND_SECONDARY_ATTEMPT] >= parameters[TA_ALL_BAND_GAP_STOP_ATTEMPT] ||
        parameters[TA_ALL_BAND_BASELINE_WEIGHT] <= 0 || parameters[TA_ALL_BAND_MIDPOINT_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid all-band attempt configuration");
}

static void validate_block_means(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_BLOCK_MEAN_TERMINAL_ADVANCE,
                                     TA_ROUTE_INVERT_TAIL_GROUPS, UINT8_MAX);
    if (!valid_phrase_index(parameters[TA_BLOCK_MEAN_GROUP_BEATS], 1, UINT8_MAX) ||
        parameters[TA_BLOCK_MEAN_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_BLOCK_MEAN_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid phrase block mean parameters");
}

static void validate_inverted_route(ta_context *c, const double *parameters)
{
    double width = parameters[TA_ROUTE_INVERT_GROUP_BEATS];
    double tail = parameters[TA_ROUTE_INVERT_TAIL_GROUPS];
    if (parameters[TA_ROUTE_INVERT_PHASE_ORIGIN] != parameters[TA_REFERENCE_TIMING_PHASE_1] ||
        parameters[TA_ROUTE_INVERT_MIN_MEAN_SAMPLES] < width * (tail + 1) ||
        parameters[TA_ROUTE_INVERT_MEAN_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid phrase route mean parameters");
}

static void validate_opening_level_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_SETTLED_NEXT_OFFSET,
                                     TA_OPENING_REFINE_FIRST_LIMIT, UINT8_MAX);
    if (parameters[TA_OPENING_SETTLED_LAST_OFFSET] < parameters[TA_OPENING_SETTLED_NEXT_OFFSET] ||
        parameters[TA_OPENING_SETTLED_REQUIRED_AHEAD] <= parameters[TA_OPENING_SETTLED_LAST_OFFSET] ||
        parameters[TA_OPENING_SETTLED_SUPPORT_BACK] > parameters[TA_OPENING_SECOND_CANDIDATE] ||
        parameters[TA_OPENING_SETTLED_MIN_CANDIDATE] <= parameters[TA_OPENING_SECOND_CANDIDATE] ||
        parameters[TA_OPENING_REFINE_PREVIOUS_BACK] > parameters[TA_OPENING_SECOND_CANDIDATE] ||
        parameters[TA_OPENING_PAIR_MEAN_SCALE] <= 0 || parameters[TA_OPENING_LEVEL_RATIO_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening level parameters");
}

static void validate_opening_level_extents(ta_context *c, const double *parameters)
{
    double bars = parameters[TA_OPENING_RATIO_BARS];
    if (parameters[TA_OPENING_SECOND_CANDIDATE] < 2 ||
        parameters[TA_OPENING_SECOND_CANDIDATE] + parameters[TA_OPENING_SETTLED_NEXT_OFFSET] >= bars ||
        parameters[TA_OPENING_LEVEL_RANK_COUNT] + parameters[TA_OPENING_REFINE_NEXT_OFFSET] >= bars ||
        !valid_phrase_index(parameters[TA_OPENING_QUIET_LAST_BAR], 1, (int)bars - 1) ||
        parameters[TA_OPENING_QUIET_RATIO_REPLACEMENT] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening level extents");
}

static void validate_opening_level_neighbors(ta_context *c, const double *parameters)
{
    int bars = (int)parameters[TA_OPENING_RATIO_BARS];
    int margin = bars - (int)parameters[TA_OPENING_LEVEL_RANK_COUNT] - 1;
    if (!valid_phrase_index(parameters[TA_OPENING_LEVEL_ORDER_RATIO_BACK], 0, 1) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_NEIGHBOR_CENTER], 0, margin) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_NEIGHBOR_PREVIOUS_BACK], 0, bars - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_LEVEL_NEIGHBOR_NEXT_OFFSET], 0, margin))
        ta_fail(c, TA_FAILED, "invalid opening level neighbors");
}

static void validate_opening_history_window(ta_context *c, const double *parameters)
{
    int bars = (int)parameters[TA_OPENING_RATIO_BARS];
    double floor = parameters[TA_OPENING_SETTLED_MIN_CANDIDATE];
    double back = parameters[TA_OPENING_HISTORY_END_BACK];
    double early = parameters[TA_OPENING_HISTORY_EARLY_ORDINAL], late = parameters[TA_OPENING_HISTORY_LATE_ORDINAL];
    double first_end = ta_dmax(floor, parameters[TA_OPENING_HISTORY_MIN_CANDIDATE]) - back;
    double first_long_end = ta_dmax(first_end, parameters[TA_OPENING_HISTORY_SHORT_END] + 1 - back);
    if (!valid_phrase_index(back, 1, bars - 1) ||
        !valid_phrase_index(parameters[TA_OPENING_HISTORY_RATIO_INDEX], 0, bars - 1) ||
        !valid_phrase_index(early, 1, bars) || !valid_phrase_index(late, 1, bars) ||
        ta_dmax(early, late) > floor - back ||
        parameters[TA_OPENING_HISTORY_SHORT_COUNT] > first_end ||
        parameters[TA_OPENING_HISTORY_LONG_COUNT] > first_long_end)
        ta_fail(c, TA_FAILED, "invalid opening history window");
}

static void validate_opening_fallback(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_OPENING_FALLBACK_LOW_BACK,
                                     TA_OPENING_FALLBACK_END_BACK, UINT8_MAX);
    double end = parameters[TA_OPENING_FALLBACK_END_BACK];
    double start = parameters[TA_OPENING_FALLBACK_LOW_BACK];
    double mid = parameters[TA_OPENING_FALLBACK_MID_BACK];
    if (!valid_phrase_index(parameters[TA_OPENING_FALLBACK_CANDIDATE_FLOOR], (int)start - 1, UINT8_MAX) ||
        mid > start || end > mid || end < 2 || parameters[TA_OPENING_FALLBACK_LOW_SCALE] <= 0 ||
        parameters[TA_OPENING_FALLBACK_MID_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid opening fallback window");
}

static void validate_high_role_codes(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HIGH_ROLE_INTRO, TA_HIGH_ROLE_OUTRO, UINT16_MAX);
    if (parameters[TA_HIGH_ROLE_UNSET] != 0 ||
        parameters[TA_HIGH_ROLE_INTRO] != parameters[TA_HIGH_TWO_SECTION_INTRO] ||
        parameters[TA_HIGH_ROLE_OUTRO] != parameters[TA_HIGH_TWO_SECTION_OUTRO])
        ta_fail(c, TA_FAILED, "invalid high phrase role codes");
}

static void validate_high_short_roles(ta_context *c, const double *parameters)
{
    /* The two compact role records have fixed arities. */
    if (parameters[TA_HIGH_SHORT_SINGLE_COUNT] != 1 || parameters[TA_HIGH_SHORT_PAIR_COUNT] != 2 ||
        !valid_phrase_index(parameters[TA_HIGH_SHORT_SIMPLE_FLAG], 0, 1))
        ta_fail(c, TA_FAILED, "invalid short high-role layout");
    if (!valid_phrase_index(parameters[TA_HIGH_FALLING_BRIDGE_FINAL_BACK], 2, (int)parameters[TA_HIGH_SHORT_PAIR_COUNT] + 1))
        ta_fail(c, TA_FAILED, "invalid high-role bridge index");
}

static void validate_high_role_support(ta_context *c, const double *parameters)
{
    if (!valid_phrase_index(parameters[TA_HIGH_ROLE_SUPPORT_SKIP], 1, 1) ||
        !valid_phrase_index(parameters[TA_HIGH_ROLE_SUPPORT_MEAN_SHRINK], 1, 2) ||
        !valid_phrase_index(parameters[TA_HIGH_HOOK_FALLBACK_LAST_COUNTER], 0, UINT8_MAX) ||
        parameters[TA_HIGH_ROLE_SUPPORT_DEFAULT] <= 0)
        ta_fail(c, TA_FAILED, "invalid high phrase support window");
}

static void validate_high_snare_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_HIGH_BAND_PHASE_SAMPLES,
                                     TA_HIGH_BAND_BEAT_DIVISOR, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SNARE_SPECTRAL_TAIL_BARS,
                                     TA_SNARE_SPECTRAL_BAR_SAMPLES, UINT8_MAX);
    if (parameters[TA_HIGH_BAND_PHASE_SAMPLES] != parameters[TA_HIGH_BAND_PHASE_ORIGIN_SAMPLES] ||
        parameters[TA_HIGH_BAND_INVERSE_NUMERATOR] <= 0 || parameters[TA_SNARE_LEVEL_INVERSE_NUMERATOR] <= 0)
        ta_fail(c, TA_FAILED, "invalid high-band normalization parameters");
}

static void validate_snare_spectral_scan(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_SNARE_PERCENTILE_BAR_DIVISOR,
                                     TA_SNARE_SCAN_STABLE_AHEAD, UINT8_MAX);
    double lookahead = parameters[TA_SNARE_SCAN_STABLE_AHEAD];
    if (lookahead > parameters[TA_SNARE_SPECTRAL_TAIL_BARS] ||
        !valid_phrase_index(parameters[TA_SNARE_FIRST_POSITION_ADVANCE], 1, (int)lookahead) ||
        parameters[TA_SNARE_PERCENTILE_MIN_BARS] / parameters[TA_SNARE_PERCENTILE_BAR_DIVISOR] <
            parameters[TA_SNARE_PERCENTILE_RANK_ORIGIN] ||
        parameters[TA_SNARE_RATIO_NO_CHANGE] <= 0 || parameters[TA_SNARE_SELECT_NO_CHANGE] <= 0)
        ta_fail(c, TA_FAILED, "invalid snare scan parameters");
}

static void validate_fill_quiet_indices(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_CLEAN_FILL_QUIET_FIRST_BACK,
                                     TA_CLEAN_FILL_PREVIOUS_LAST_BACK, UINT8_MAX);
    double far = parameters[TA_CLEAN_FILL_QUIET_FIRST_BACK];
    double near = parameters[TA_CLEAN_FILL_QUIET_LAST_BACK];
    if (far < near || parameters[TA_CLEAN_FILL_PREVIOUS_FIRST_BACK] < parameters[TA_CLEAN_FILL_PREVIOUS_LAST_BACK] ||
        !valid_phrase_index(parameters[TA_CLEAN_FILL_QUIET_POWER_FIRST_BACK], (int)near, (int)far) ||
        !valid_phrase_index(parameters[TA_CLEAN_FILL_QUIET_POWER_SECOND_BACK], (int)near, (int)far))
        ta_fail(c, TA_FAILED, "invalid quiet fill window parameters");
}

static void validate_fill_cleanup_codes(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_CLEAN_FILL_HOOK_DECORATION_MIN_EXCLUSIVE,
                                     TA_CLEAN_FILL_POWER_STRIDE_BYTES, UINT16_MAX);
    double hook = parameters[TA_CLEAN_FILL_HOOK_DECORATION_MIN_EXCLUSIVE];
    double build = parameters[TA_CLEAN_FILL_BUILD_DECORATION_MIN_EXCLUSIVE];
    double intro = parameters[TA_CLEAN_FILL_INTRO_DECORATION_MIN];
    if (hook <= build || build < intro ||
        (int)parameters[TA_CLEAN_FILL_POWER_STRIDE_BYTES] % sizeof(double) != 0 ||
        parameters[TA_CLEAN_FILL_INVERSE_NUMERATOR] <= 0 ||
        !valid_phrase_index(parameters[TA_CLEAN_FILL_AUXILIARY_MARK], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid fill cleanup parameters");
}

static void validate_final_fill_windows(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FINAL_SHORT_HOOK_SPAN_MAX,
                                     TA_FINAL_FILL_TRIM_BACK, UINT8_MAX);
    double penultimate = parameters[TA_FINAL_PENULTIMATE_BACK];
    if (!valid_phrase_index(parameters[TA_FINAL_MIN_SECTIONS], 2, UINT8_MAX) ||
        parameters[TA_FINAL_LONG_BUILD_MIN_SECTIONS] <= penultimate ||
        parameters[TA_FINAL_HOOK_AND_BUILD_MIN_SECTIONS] <= penultimate ||
        parameters[TA_FINAL_FILL_TRIM_BACK] >= parameters[TA_FINAL_FILL_TRIM_MIN])
        ta_fail(c, TA_FAILED, "invalid final fill windows");
}

static void validate_final_roles(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_FINAL_ROLE_INTRO, TA_FINAL_ROLE_OUTRO_VARIANT, UINT16_MAX);
    if (parameters[TA_FINAL_ROLE_INTRO] != parameters[TA_HIGH_ROLE_INTRO] ||
        parameters[TA_FINAL_ROLE_BUILD] != parameters[TA_HIGH_ROLE_BUILD] ||
        parameters[TA_FINAL_ROLE_HOOK] != parameters[TA_HIGH_ROLE_HOOK] ||
        parameters[TA_FINAL_ROLE_BREAK] != parameters[TA_HIGH_ROLE_BREAK] ||
        parameters[TA_FINAL_ROLE_OUTRO] != parameters[TA_HIGH_ROLE_OUTRO])
        ta_fail(c, TA_FAILED, "invalid final fill role codes");
}

static void validate_rhythm_run_storage(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_RUN_ROLE_DIVISOR,
                                     TA_RHYTHM_RUN_DENSE_PACKED_PAIR, UINT16_MAX);
    double width = parameters[TA_RHYTHM_RUN_GROUP_ACTIVITY_SLOTS];
    double beat_slots = parameters[TA_RHYTHM_RUN_BEAT_ACTIVITY_SLOTS];
    enum { prototype_words = TA_RHYTHM_RUN_FOUR_PROTOTYPE_3 - TA_RHYTHM_RUN_FOUR_PROTOTYPE_0 + 1 };
    unsigned packed = (unsigned)parameters[TA_RHYTHM_RUN_DENSE_PACKED_PAIR];
    if (width != beat_slots * parameters[TA_RHYTHM_RUN_GROUP_BEATS] ||
        width != prototype_words * sizeof(uint32_t) ||
        beat_slots != parameters[TA_RHYTHM_RUN_ACTIVITY_ORIGIN_SLOTS] ||
        (packed & UINT8_MAX) != packed >> CHAR_BIT)
        ta_fail(c, TA_FAILED, "invalid rhythm run storage");
}

static void validate_rhythm_run_counters(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_RUN_COUNTER_BIAS,
                                     TA_RHYTHM_RUN_SINGLE_TAIL_COUNT, UINT8_MAX);
    double bias = parameters[TA_RHYTHM_RUN_COUNTER_BIAS];
    if (bias - parameters[TA_RHYTHM_RUN_FIRST_EIGHT_BACK] != 1 ||
        bias != parameters[TA_RHYTHM_RUN_PRECEDING_FOUR_BACK] ||
        bias <= parameters[TA_RHYTHM_RUN_TWO_EIGHTS_BACK] ||
        bias <= parameters[TA_RHYTHM_RUN_ONE_EIGHT_BACK] ||
        parameters[TA_RHYTHM_RUN_THIRD_EIGHT_NEXT] > parameters[TA_RHYTHM_RUN_CODE_PADDING])
        ta_fail(c, TA_FAILED, "invalid rhythm run counters");
}

static void validate_rhythm_growth_storage(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_RHYTHM_GROWTH_ROLE_DIVISOR,
                                     TA_RHYTHM_GROWTH_SIMPLE_ROLE, UINT16_MAX);
    double slots = parameters[TA_RHYTHM_GROWTH_BEAT_ACTIVITY_SLOTS];
    if (slots != parameters[TA_RHYTHM_GROWTH_ACTIVITY_ORIGIN_SLOTS] ||
        slots * parameters[TA_RHYTHM_GROWTH_GROUP_BEATS] != parameters[TA_RHYTHM_GROWTH_GROUP_ACTIVITY_SLOTS] ||
        !valid_phrase_index(parameters[TA_RHYTHM_GROWTH_RESTORE_ROLE_DIVISOR], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid rhythm growth storage");
}

static void validate_rhythm_growth_counts(ta_context *c, const double *parameters)
{
    double last = parameters[TA_RHYTHM_GROWTH_LAST_BACK];
    double late = -parameters[TA_RHYTHM_GROWTH_TWO_OFFSET];
    double middle = -parameters[TA_RHYTHM_GROWTH_FOUR_OFFSET];
    double early = -parameters[TA_RHYTHM_GROWTH_EIGHT_OFFSET];
    if (!valid_phrase_index(last, 1, UINT8_MAX) ||
        !valid_phrase_index(late, (int)last + 1, UINT8_MAX) ||
        !valid_phrase_index(middle, (int)late + 1, UINT8_MAX) ||
        !valid_phrase_index(early, (int)middle + 1, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_RHYTHM_GROWTH_SPREAD_TAIL], 1, (int)early))
        ta_fail(c, TA_FAILED, "invalid rhythm growth count windows");
}

static void validate_repeated_peak_geometry(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_REPEATED_PEAK_GROUP_BEATS,
                                     TA_REPEATED_UPPER_QUIET_LOOKBACK, UINT8_MAX);
    double slots = parameters[TA_REPEATED_PEAK_WINDOW_SAMPLES] / 2;
    if (parameters[TA_REPEATED_PEAK_AMPLITUDE_SLOTS] < slots ||
        parameters[TA_REPEATED_PEAK_SPACING_SLOTS] < slots ||
        parameters[TA_REPEATED_UPPER_QUIET_LOOKBACK] <= parameters[TA_REPEATED_UPPER_ACTIVE_LOOKBACK] ||
        !valid_phrase_index(parameters[TA_REPEATED_UPPER_MAX_QUIET_SCORE], 0, UINT8_MAX) ||
        !valid_phrase_index(parameters[TA_REPEATED_UPPER_START_BACK], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid repeated peak geometry");
}

static void validate_decimation_rates(ta_context *c, const double *parameters)
{
    validate_phrase_positive_indices(c, parameters, TA_DECIMATE_HIGH_DIVISOR, TA_DECIMATE_LOW_DIVISOR, UINT16_MAX);
    for (int band = 0; band <= TA_DECIMATE_LOW_DIVISOR - TA_DECIMATE_HIGH_DIVISOR; band++) {
        double divisor = parameters[TA_DECIMATE_HIGH_DIVISOR + band];
        if (parameters[TA_DECIMATE_HIGH_RATE_SCALE + band] != 1 / divisor)
            ta_fail(c, TA_FAILED, "invalid decimated analysis rates");
    }
}

void ta_model_phrase_parameters(ta_context *c, double parameters[TA_PHRASE_PARAMETER_COUNT])
{
    uint8_t *bytes = ta_model_open(c);
    memcpy(parameters, bytes + TA_MODEL_PHRASE_OFFSET, sizeof(double) * TA_PHRASE_PARAMETER_COUNT);
    if (c->windows_model)
        read_windows_reduction(c, bytes + TA_MODEL_WINDOWS_REDUCTION_OFFSET, parameters);
    ta_release(c, bytes);
    for (int i = 0; i < TA_PHRASE_PARAMETER_COUNT; ++i)
        if (!isfinite(parameters[i]))
            ta_fail(c, TA_FAILED, "invalid phrase analysis parameters");
    validate_phrase_scan(c, parameters);
    validate_phrase_block(c, parameters);
    validate_short_break_rank(c, parameters);
    validate_phrase_activity_geometry(c, parameters);
    validate_phrase_activity_extents(c, parameters);
    validate_boundary_window(c, parameters);
    validate_high_candidate_search(c, parameters);
    validate_verse_alternatives(c, parameters);
    validate_band_change(c, parameters);
    validate_phrase_timing_numbers(c, parameters);
    validate_phrase_vocal_bins(c, parameters);
    validate_phrase_fill_ranges(c, parameters);
    validate_opening_snap_bars(c, parameters);
    validate_rhythm_run_storage(c, parameters);
    validate_opening_rank(c, parameters);
    validate_opening_history(c, parameters);
    validate_snare_geometry(c, parameters);
    validate_attack_delay(c, parameters);
    validate_attack_marking(c, parameters);
    validate_decoration_windows(c, parameters);
    validate_hihat_levels(c, parameters);
    validate_phrase_outro(c, parameters);
    validate_variation_geometry(c, parameters);
    validate_rhythm_prototypes(c, parameters);
    validate_rhythm_masks(c, parameters);
    validate_rhythm_growth_storage(c, parameters);
    validate_rhythm_growth_counts(c, parameters);
    validate_rhythm_growth(c, parameters);
    validate_rhythm_tail(c, parameters);
    validate_non_high_kinds(c, parameters);
    validate_candidate_geometry(c, parameters);
    validate_repeat_rhythm_windows(c, parameters);
    validate_genre_snap_geometry(c, parameters);
    validate_reference_timing(c, parameters);
    validate_coordinate_parameters(c, parameters);
    validate_phrase_spectra(c, parameters);
    validate_opening_cqt(c, parameters);
    validate_opening_cqt_accumulation(c, parameters);
    validate_spectral_scales(c, parameters);
    validate_spectral_windows(c, parameters);
    validate_phrase_reductions(c, parameters);
    validate_phrase_positive_indices(c, parameters, TA_SIMPLE_LOOP_FIRST_INPUT_ROLE,
                                     TA_SIMPLE_LOOP_OUTPUT_ROLE, UINT16_MAX);
    validate_decimator_sections(c, parameters);
    validate_envelope_sections(c, parameters);
    validate_nominal_beat_geometry(c, parameters);
    validate_vocal_vote(c, parameters);
    validate_opening_padding(c, parameters);
    validate_opening_repeat(c, parameters);
    validate_power_and_tempo(c, parameters);
    validate_rhythm_secondary_votes(c, parameters);
    validate_high_boundary_flag_widths(c, parameters);
    validate_high_boundary_indices(c, parameters);
    validate_phrase_scan_starts(c, parameters);
    validate_fade_geometry(c, parameters);
    validate_mood_revision(c, parameters);
    validate_route_geometry(c, parameters);
    validate_compression_geometry(c, parameters);
    validate_slow_rock(c, parameters);
    validate_rhythm_pattern_scan(c, parameters);
    validate_rhythm_pattern_masks(c, parameters);
    validate_hook_windows(c, parameters);
    validate_boundary_adjustment(c, parameters);
    validate_secure_levels(c, parameters);
    validate_initial_alternatives(c, parameters);
    validate_recurrence_frames(c, parameters);
    validate_recurrence_score_model(c, parameters);
    validate_seed_segments(c, parameters);
    validate_voice_dense_windows(c, parameters);
    validate_voice_sustained_windows(c, parameters);
    validate_opening_voice_scores(c, parameters);
    validate_voice_bar_windows(c, parameters);
    validate_voice_sparse_windows(c, parameters);
    validate_opening_vote_counts(c, parameters);
    validate_opening_selection(c, parameters);
    validate_opening_anchor_phase(c, parameters);
    validate_opening_score_dispatch(c, parameters);
    validate_phrase_positive_indices(c, parameters, TA_HIGH_SINGLE_SECTION_INTRO,
                                     TA_HIGH_TWO_SECTION_OUTRO, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_TYPICAL_RHYTHM_TYPE,
                                     TA_NON_HIGH_SINGLE_TEMPORARY_ROLE, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_SNARE_KICK_REJECT_FOUR_FLAG,
                                     TA_SNARE_KICK_REJECT_MARK, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_LOOP_RESTORE_ROLE,
                                     TA_LOOP_RESTORE_MIN_SPAN, UINT8_MAX);
    validate_snare_support(c, parameters);
    validate_snare_pattern(c, parameters);
    validate_snare_rank(c, parameters);
    validate_vocal_entry_windows(c, parameters);
    validate_fill_attachment(c, parameters);
    validate_beat_four_decoration(c, parameters);
    validate_fill_growth_windows(c, parameters);
    validate_fill_cut_windows(c, parameters);
    validate_fine_fill_cut(c, parameters);
    validate_spectral_peak_model(c, parameters);
    validate_spectral_peak_geometry(c, parameters);
    validate_phrase_positive_indices(c, parameters, TA_MOOD_SLOW_ROCK_FLAG,
                                     TA_OTHER_CANDIDATE_ROUTE, UINT8_MAX);
    validate_phrase_positive_indices(c, parameters, TA_HIGH_CANDIDATE_BAR_DIVISOR,
                                     TA_OTHER_CANDIDATE_BAR_DIVISOR, UINT8_MAX);
    validate_high_role_scales(c, parameters);
    validate_high_role_dispatch(c, parameters);
    validate_phrase_positive_indices(c, parameters, TA_HIGH_EXPORT_KIND_INTRO,
                                     TA_HIGH_EXPORT_OUTRO_BEAT_STYLE, UINT16_MAX);
    if (!valid_phrase_index(parameters[TA_HIGH_TURN_SECTION_MIN], 3, UINT16_MAX) ||
        !valid_phrase_index(parameters[TA_HIGH_SIMPLE_TURN_COUNT_EXCLUSIVE], 1, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid high role turn counts");
    validate_outro_activity_windows(c, parameters);
    validate_outro_acceleration_window(c, parameters);
    validate_outro_snap_geometry(c, parameters);
    validate_rhythm_block_geometry(c, parameters);
    validate_opening_envelope_geometry(c, parameters);
    validate_opening_envelope_normalization(c, parameters);
    validate_phrase_positive_indices(c, parameters, TA_PHRASE_SOURCE_FRAME_TRIM,
                                     TA_PHRASE_MIN_ADJUSTED_FRAMES, UINT8_MAX);
    validate_beat_band_geometry(c, parameters);
    validate_beat_endpoints(c, parameters);
    validate_beat_grid_advances(c, parameters);
    validate_beat_four_geometry(c, parameters);
    validate_beat_four_stability(c, parameters);
    validate_opening_first_frame(c, parameters);
    validate_attack_window_geometry(c, parameters);
    validate_attack_window_fractions(c, parameters);
    validate_rhythm_power_windows(c, parameters);
    validate_attack_sample_rates(c, parameters);
    validate_short_break_positions(c, parameters);
    validate_hihat_islands(c, parameters);
    validate_hihat_changes(c, parameters);
    validate_hihat_envelopes(c, parameters);
    validate_attack_peak_geometry(c, parameters);
    validate_attack_valley_geometry(c, parameters);
    validate_spectral_vocal_scales(c, parameters);
    validate_boundary_source(c, parameters);
    validate_boundary_ratio_geometry(c, parameters);
    validate_noise_geometry(c, parameters);
    validate_local_boundary_geometry(c, parameters);
    validate_local_boundary_scales(c, parameters);
    validate_first_activity_geometry(c, parameters);
    validate_activity_bar_geometry(c, parameters);
    validate_activity_bar_scales(c, parameters);
    validate_opening_level_windows(c, parameters);
    validate_opening_presence_geometry(c, parameters);
    validate_band_change_coordinates(c, parameters);
    validate_verse_score_geometry(c, parameters);
    validate_all_band_coordinates(c, parameters);
    validate_all_band_attempts(c, parameters);
    validate_block_means(c, parameters);
    validate_inverted_route(c, parameters);
    validate_opening_level_geometry(c, parameters);
    validate_opening_level_extents(c, parameters);
    validate_opening_level_rank_slots(c, parameters);
    validate_opening_level_neighbors(c, parameters);
    validate_opening_history_window(c, parameters);
    validate_opening_fallback(c, parameters);
    validate_high_role_codes(c, parameters);
    validate_high_short_roles(c, parameters);
    validate_high_turn_indices(c, parameters);
    validate_high_role_support(c, parameters);
    validate_high_snare_geometry(c, parameters);
    validate_snare_spectral_scan(c, parameters);
    validate_fill_quiet_indices(c, parameters);
    validate_fill_cleanup_codes(c, parameters);
    validate_final_fill_windows(c, parameters);
    validate_final_roles(c, parameters);
    validate_rhythm_run_counters(c, parameters);
    validate_decimation_rates(c, parameters);
    validate_repeated_peak_geometry(c, parameters);
    if (!valid_phrase_index(parameters[TA_ROUTE_HISTOGRAM_DIVISOR], 2, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid route histogram split");
}

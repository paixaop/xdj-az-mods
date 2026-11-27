/* SPDX-License-Identifier: GPL-3.0-only */
#include "vocal-motion.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
               DBL_MAX_EXP == 1024, "vocal motion requires binary64");

enum {
    WEIGHTED = 1, MAGNITUDE, LOG_SUM, FLATNESS_MAGNITUDE, NEGATIVE, NONNEGATIVE,
    ENERGY, FLATNESS, CORRELATION, SELECTION, RECORD_COUNT
};

typedef struct {
    const uint8_t *bytes;
    size_t size;
} MotionRecord;

static unsigned motion_le16(const uint8_t *bytes)
{
    return bytes[0] | (unsigned)bytes[1] << 8;
}

static double motion_binary64(const uint8_t *bytes)
{
    uint64_t bits = 0;
    for (unsigned i = 0; i < sizeof bits; ++i)
        bits |= (uint64_t)bytes[i] << (8 * i);
    double result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static bool motion_records(const uint8_t *bytes, size_t size, MotionRecord *records)
{
    const size_t magic_size = sizeof("PVDIMOT1") - 1;
    if (size < magic_size || memcmp(bytes, "PVDIMOT1", magic_size))
        return false;
    unsigned found = 0;
    for (size_t offset = magic_size; offset < size;) {
        if (size - offset < 3)
            return false;
        unsigned type = bytes[offset], length = motion_le16(bytes + offset + 1);
        offset += 3;
        if (type == 0 || type >= RECORD_COUNT || records[type].bytes || length > size - offset)
            return false;
        records[type] = (MotionRecord){bytes + offset, length};
        offset += length;
        ++found;
    }
    return found == RECORD_COUNT - 1;
}

static bool motion_reduction_edges(ta_motion_reduction *out, const uint8_t *nodes, unsigned lanes)
{
    unsigned available = (1u << lanes) - 1u;
    for (unsigned i = 0; i < lanes - 1; ++i) {
        unsigned left = nodes[i * 2], right = nodes[i * 2 + 1];
        if (left >= lanes + i || right >= lanes + i || left == right)
            return false;
        unsigned inputs = (1u << left) | (1u << right);
        if ((available & inputs) != inputs)
            return false;
        available = (available & ~inputs) | (1u << (lanes + i));
        out->adds[i][0] = (uint8_t)left;
        out->adds[i][1] = (uint8_t)right;
    }
    unsigned root = nodes[2 * (lanes - 1)];
    return root == 2 * lanes - 2;
}

static bool motion_read_reduction(ta_motion_reduction *out, MotionRecord record, const uint8_t *operations)
{
    if (record.size < 7 || memcmp(record.bytes + 1, operations, 4))
        return false;
    unsigned lanes = record.bytes[0];
    if (lanes < 2 || lanes > TA_MOTION_LANES || record.bytes[5] != lanes - 1 ||
        record.size != 7 + 2 * (lanes - 1))
        return false;
    out->lanes = (uint8_t)lanes;
    return motion_reduction_edges(out, record.bytes + 6, lanes);
}

static bool motion_read_reductions(ta_vocal_motion *out, const MotionRecord *records)
{
    /* Update operation, first and second input tags, then scalar tail operation. */
    static const uint8_t operations[][4] = {
        {3, 1, 2, 0}, {1, 1, 0, 0}, {2, 3, 0, 0},
        {2, 1, 0, 0}, {3, 5, 4, 2}, {3, 5, 4, 1}
    };
    ta_motion_reduction *reductions[] = {
        &out->weighted, &out->magnitude, &out->logs, &out->flatness_magnitude,
        &out->negative, &out->nonnegative
    };
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i)
        if (!motion_read_reduction(reductions[i], records[WEIGHTED + i], operations[i]))
            return false;
    return out->width % out->weighted.lanes == 0 && out->width % out->magnitude.lanes == 0 &&
           out->width % out->logs.lanes == 0 && out->width % out->flatness_magnitude.lanes == 0;
}

static bool motion_positive(double value)
{
    return isfinite(value) && value > 0;
}

static bool motion_read_energy(ta_vocal_motion *out, MotionRecord record)
{
    const uint8_t *bytes = record.bytes;
    if (record.size != 20 || bytes[0] != 3 || bytes[1] != 1 || motion_le16(bytes + 2) != out->width)
        return false;
    out->energy_epsilon = motion_binary64(bytes + 4);
    out->energy_min = motion_binary64(bytes + 12);
    return motion_positive(out->energy_epsilon) && isfinite(out->energy_min);
}

static bool motion_read_flatness(ta_vocal_motion *out, MotionRecord record)
{
    const uint8_t *bytes = record.bytes;
    if (record.size != 38 || memcmp(bytes, "\x04\x05\x06\x02", 4) ||
        motion_le16(bytes + 4) != out->width)
        return false;
    out->log_epsilon = motion_binary64(bytes + 6);
    out->mean_scale = motion_binary64(bytes + 14);
    out->mean_epsilon = motion_binary64(bytes + 22);
    out->flatness_max = motion_binary64(bytes + 30);
    return motion_positive(out->log_epsilon) && motion_positive(out->mean_scale) &&
           motion_positive(out->mean_epsilon) && motion_positive(out->flatness_max);
}

static bool motion_lag_candidates(ta_vocal_motion *out, const uint8_t *bytes, int min_lag, int max_lag)
{
    int first = (int)motion_le16(bytes + 4), last = (int)motion_le16(bytes + 6);
    int center = (int)motion_le16(bytes + 8);
    if (last >= (int)out->width || first > center || center > last ||
        first - center != min_lag || last - center != max_lag)
        return false;
    unsigned count = (unsigned)(last - first + 1);
    if (count < TA_MOTION_SELECTION_LANES || count > TA_MOTION_CANDIDATES)
        return false;
    out->candidates = count;
    out->center = (unsigned)(center - first);
    out->normalized = motion_le16(bytes + 10);
    return out->normalized >> count == 0 && (out->normalized & (1u << out->center)) != 0;
}

static bool motion_read_correlation(ta_vocal_motion *out, MotionRecord record, int min_lag, int max_lag)
{
    const uint8_t *bytes = record.bytes;
    if (record.size != 20 || bytes[0] != 7 || bytes[1] != 8 || motion_le16(bytes + 2) != out->width ||
        !motion_lag_candidates(out, bytes, min_lag, max_lag))
        return false;
    out->reciprocal_numerator = motion_binary64(bytes + 12);
    return motion_positive(out->reciprocal_numerator);
}

static bool motion_selection_permutations(ta_vocal_motion *out, const uint8_t *bytes)
{
    unsigned lanes = out->selection_lanes;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        out->adjacent[lane] = bytes[lane * 2];
        out->halves[lane] = bytes[lane * 2 + 1];
        if (out->adjacent[lane] >= lanes || out->halves[lane] >= lanes)
            return false;
    }
    for (unsigned lane = 0; lane < lanes; ++lane) {
        unsigned half = out->halves[lane];
        unsigned reached = (1u << lane) | (1u << out->adjacent[lane]) |
                           (1u << half) | (1u << out->adjacent[half]);
        if (reached != (1u << lanes) - 1u)
            return false;
    }
    return (unsigned)bytes[8] + bytes[9] == 1;
}

static bool motion_read_selection(ta_vocal_motion *out, MotionRecord record)
{
    const uint8_t *bytes = record.bytes;
    if (record.size != 26 || bytes[0] != TA_MOTION_SELECTION_LANES || bytes[1] != 2 ||
        memcmp(bytes + 2, "\x01\x09\x0a", 3) || memcmp(bytes + 23, "\x0b\x0c\x0d", 3))
        return false;
    out->selection_lanes = bytes[0];
    out->selection_seed = motion_binary64(bytes + 5);
    return out->selection_seed < INFINITY && motion_selection_permutations(out, bytes + 13);
}

bool ta_vocal_motion_read(ta_vocal_motion *out, const uint8_t *bytes, size_t size,
                          unsigned width, int min_lag, int max_lag)
{
    MotionRecord records[RECORD_COUNT] = {0};
    if (width == 0 || width > UINT16_MAX || !motion_records(bytes, size, records))
        return false;
    out->width = width;
    return motion_read_reductions(out, records) && motion_read_energy(out, records[ENERGY]) &&
           motion_read_flatness(out, records[FLATNESS]) &&
           motion_read_correlation(out, records[CORRELATION], min_lag, max_lag) &&
           motion_read_selection(out, records[SELECTION]);
}

static double motion_ordered_sum(const ta_motion_reduction *reduction, double *slots)
{
    unsigned lanes = reduction->lanes;
    for (unsigned i = 0; i < lanes - 1; ++i)
        slots[lanes + i] = slots[reduction->adds[i][0]] + slots[reduction->adds[i][1]];
    return slots[2 * lanes - 2];
}

static double motion_sum(const ta_motion_reduction *reduction, const double *samples,
                         unsigned count, bool sample_first)
{
    double slots[TA_MOTION_LANES + TA_MOTION_ADDITIONS] = {0};
    unsigned lanes = reduction->lanes;
    for (unsigned i = 0; i < count; i += lanes)
        for (unsigned lane = 0; lane < lanes; ++lane) {
            double sample = samples[i + lane];
            slots[lane] = sample_first ? sample + slots[lane] : slots[lane] + sample;
        }
    return motion_ordered_sum(reduction, slots);
}

static double motion_products(const ta_motion_reduction *reduction, const double *first,
                              const double *second, unsigned count, bool reverse_tail)
{
    double slots[TA_MOTION_LANES + TA_MOTION_ADDITIONS] = {0};
    unsigned lanes = reduction->lanes, i = 0;
    for (; i + lanes <= count; i += lanes)
        for (unsigned lane = 0; lane < lanes; ++lane)
            slots[lane] = fma(first[i + lane], second[i + lane], slots[lane]);
    double sum = motion_ordered_sum(reduction, slots);
    for (; i < count; ++i) {
        double product = reverse_tail ? second[i] * first[i] : first[i] * second[i];
        sum = sum + product;
    }
    return sum;
}

bool ta_vocal_motion_gate(const ta_vocal_motion *motion, const double *spectrum,
                          const double *window, const double *logs)
{
    unsigned width = motion->width;
    double weighted = motion_products(&motion->weighted, spectrum, window, width, false);
    double magnitude = motion_sum(&motion->magnitude, spectrum, width, false);
    double energy = weighted / (motion->energy_epsilon + magnitude);
    if (!(energy > motion->energy_min))
        return false;
    double log_sum = motion_sum(&motion->logs, logs, width, true);
    double flatness_mag = motion_sum(&motion->flatness_magnitude, spectrum, width, true);
    double mean = fma(flatness_mag, motion->mean_scale, motion->mean_epsilon);
    return !(exp(log_sum * motion->mean_scale) / mean >= motion->flatness_max);
}

static double motion_correlation(const ta_vocal_motion *motion, const double *previous,
                                 const double *following, int lag)
{
    if (lag < 0) {
        unsigned distance = (unsigned)-lag;
        return motion_products(&motion->negative, following + distance, previous,
                               motion->width - distance, true);
    }
    unsigned distance = (unsigned)lag;
    return motion_products(&motion->nonnegative, following, previous + distance,
                           motion->width - distance, false);
}

static void motion_selection_reduce(const unsigned lanes, const double *scores,
                                    const uint8_t *permutation, double *results)
{
    for (unsigned lane = 0; lane < lanes; ++lane) {
        double shuffled = scores[permutation[lane]], incumbent = scores[lane];
        results[lane] = shuffled > incumbent ? shuffled : incumbent;
    }
}

static unsigned motion_select(const ta_vocal_motion *motion, const double *scores)
{
    const unsigned lanes = TA_MOTION_SELECTION_LANES;
    double best[TA_MOTION_SELECTION_LANES], adjacent[TA_MOTION_SELECTION_LANES];
    double all[TA_MOTION_SELECTION_LANES] = {0};
    unsigned indexes[TA_MOTION_SELECTION_LANES];
    for (unsigned lane = 0; lane < lanes; ++lane) {
        best[lane] = scores[lane];
        indexes[lane] = lane;
    }
    /* Each lane retains its incumbent on equal or unordered updates. A partial
     * final group only visits the valid lanes, as in the wide native routine. */
    for (unsigned i = lanes; i < motion->candidates; ++i) {
        unsigned lane = i % lanes;
        if (scores[i] > best[lane]) {
            best[lane] = scores[i];
            indexes[lane] = i;
        }
    }
    motion_selection_reduce(lanes, best, motion->adjacent, adjacent);
    motion_selection_reduce(lanes, adjacent, motion->halves, all);
    if (!(all[0] > motion->selection_seed))
        return 0;
    unsigned index = motion->candidates;
    for (unsigned lane = 0; lane < lanes; ++lane)
        if (best[lane] == all[0] && indexes[lane] < index)
            index = indexes[lane];
    return index;
}

int ta_vocal_motion_lag(const ta_vocal_motion *motion, const double *previous, const double *following)
{
    double scores[TA_MOTION_CANDIDATES] = {0};
    for (unsigned i = 0; i < motion->candidates; ++i)
        scores[i] = motion_correlation(motion, previous, following, (int)i - (int)motion->center);
    double inverse = motion->reciprocal_numerator / scores[motion->center];
    for (unsigned i = 0; i < motion->candidates; ++i)
        if (motion->normalized & (1u << i))
            scores[i] *= inverse;
    return (int)motion_select(motion, scores) - (int)motion->center;
}

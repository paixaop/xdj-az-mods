/* SPDX-License-Identifier: GPL-3.0-only */
#include "vocal_lines_intervals.h"
#include "../common/aztimestamp.h"
#include <math.h>
#include <stdlib.h>

static int parameters_valid(const az_vocal_interval *intervals, uint32_t count,
                            uint32_t rate, double gap) {
    return rate && isfinite(gap) && gap >= 0 && (intervals || !count);
}

static int interval_valid(const az_vocal_interval *interval, uint32_t rate) {
    int order = compare_timestamps(interval->start_s, interval->stop_s, 1, rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    return interval->start_s >= 0 && interval->stop_s > interval->start_s;
}

static int all_valid(const az_vocal_interval *intervals, uint32_t count, uint32_t rate) {
    for (uint32_t i = 0; i < count; i++)
        if (!interval_valid(&intervals[i], rate)) return 0;
    return 1;
}

/* Approximate equality is not transitive and cannot order a sort. Inputs have
 * already been checked for finite values. Equal starts need no tie-breaker. */
static int start_order(const void *left, const void *right) {
    const az_vocal_interval *a = left, *b = right;
    if (a->start_s < b->start_s) return -1;
    return a->start_s > b->start_s;
}

static int strictly_before(double first, double second, uint32_t rate) {
    int order = compare_timestamps(first, second, 1, rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    /* Preserve the strict user threshold even inside the source's one-sample
     * equality window: a positive gap below the setting still joins. */
    return order == AZ_TIME_BEFORE || (order == AZ_TIME_EQUAL && first < second);
}

static int joins(const az_vocal_interval *current, const az_vocal_interval *next,
                 double gap, uint32_t rate) {
    if (!strictly_before(current->stop_s, next->start_s, rate)) return 1;
    return strictly_before(next->start_s - current->stop_s, gap, rate);
}

static uint32_t consolidate(az_vocal_interval *intervals, uint32_t count,
                            double gap, uint32_t rate) {
    uint32_t written = 1;
    for (uint32_t i = 1; i < count; i++) {
        az_vocal_interval *current = &intervals[written - 1];
        if (joins(current, &intervals[i], gap, rate)) {
            if (strictly_before(current->stop_s, intervals[i].stop_s, rate))
                current->stop_s = intervals[i].stop_s;
        } else intervals[written++] = intervals[i];
    }
    return written;
}

int vocal_lines_merge(az_vocal_interval *intervals, uint32_t count,
                      uint32_t sample_rate, double gap_s,
                      uint32_t *merged_count) {
    if (!merged_count) return AZ_INVALID;
    *merged_count = 0;
    if (!parameters_valid(intervals, count, sample_rate, gap_s)) return AZ_INVALID;
    if (!all_valid(intervals, count, sample_rate)) return AZ_INVALID;
    if (!count) return AZ_OK;
    qsort(intervals, count, sizeof *intervals, start_order);
    *merged_count = consolidate(intervals, count, gap_s, sample_rate);
    return AZ_OK;
}

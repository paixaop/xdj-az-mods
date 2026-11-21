/* SPDX-License-Identifier: GPL-3.0-only */
#include "vocal_lines_intervals.h"
#include "../../src/common/aztimestamp.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void time_is(double actual, double expected, uint32_t rate) {
    int order = compare_timestamps(actual, expected, 1, rate, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
}

static void span_is(az_vocal_interval actual, double start, double stop, uint32_t rate) {
    time_is(actual.start_s, start, rate);
    time_is(actual.stop_s, stop, rate);
}

static void overlaps_and_chains(void) {
    az_vocal_interval input[] = {
        {12, 13}, {3, 5}, {1, 4}, {2, 3}, {1, 4}, {8, 10}, {5, 7}, {11, 12}};
    uint32_t count = 999;
    assert(vocal_lines_merge(input, 8, 22050, 0, &count) == AZ_OK);
    assert(count == 3);
    span_is(input[0], 1, 7, 22050);
    span_is(input[1], 8, 10, 22050);
    span_is(input[2], 11, 13, 22050);
    assert(vocal_lines_merge(input, count, 22050, 1.5, &count) == AZ_OK);
    assert(count == 1);
    span_is(input[0], 1, 13, 22050);

    az_vocal_interval shrinking[] = {{0, 20}, {1, 2}, {1, 19}, {21, 22}};
    assert(vocal_lines_merge(shrinking, 4, 48000, 0, &count) == AZ_OK);
    assert(count == 2);
    span_is(shrinking[0], 0, 20, 48000);
    span_is(shrinking[1], 21, 22, 48000);
}

static void threshold_boundaries(void) {
    const uint32_t rates[] = {22050, 44100, 48000, 96000};
    for (unsigned r = 0; r < sizeof rates / sizeof *rates; r++) {
        uint32_t rate = rates[r];
        for (int sample = -1; sample <= 1; sample++) {
            double start = 3 + (double) sample / rate;
            az_vocal_interval input[] = {{0, 1}, {start, 4}};
            uint32_t count;
            assert(vocal_lines_merge(input, 2, rate, 2, &count) == AZ_OK);
            assert(count == (sample < 0 ? 1u : 2u));
            span_is(input[0], 0, sample < 0 ? 4 : 1, rate);
        }
        /* A positive sub-sample gap must survive gap=0 and exactly equal gap;
         * a threshold even slightly above it must consolidate. */
        double gap = 0.25 / rate;
        az_vocal_interval tiny[] = {{0, 1}, {1 + gap, 2}};
        uint32_t count;
        assert(vocal_lines_merge(tiny, 2, rate, 0, &count) == AZ_OK);
        assert(count == 2);
        double actual_gap = tiny[1].start_s - tiny[0].stop_s;
        assert(vocal_lines_merge(tiny, 2, rate, actual_gap, &count) == AZ_OK);
        assert(count == 2);
        assert(vocal_lines_merge(tiny, 2, rate, nextafter(actual_gap, INFINITY), &count) == AZ_OK);
        assert(count == 1);
        span_is(tiny[0], 0, 2, rate);
    }
}

static void invalid_span(double start, double stop) {
    az_vocal_interval input[] = {{4, 5}, {start, stop}, {1, 2}};
    az_vocal_interval before[3];
    memcpy(before, input, sizeof input);
    uint32_t count = 123;
    assert(vocal_lines_merge(input, 3, 48000, 1, &count) == AZ_INVALID);
    assert(count == 0);
    assert(memcmp(input, before, sizeof input) == 0);
}

static void invalid_inputs(void) {
    invalid_span(NAN, 1);
    invalid_span(0, NAN);
    invalid_span(INFINITY, INFINITY);
    invalid_span(0, INFINITY);
    invalid_span(-INFINITY, 1);
    invalid_span(-0.25 / 48000, 1);
    invalid_span(3, 2);
    invalid_span(2, 2);
    az_vocal_interval input[] = {{3, 4}, {1, 2}};
    const double gaps[] = {-1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof gaps / sizeof *gaps; i++) {
        uint32_t count = 1;
        assert(vocal_lines_merge(input, 2, 48000, gaps[i], &count) == AZ_INVALID);
        assert(count == 0);
        span_is(input[0], 3, 4, 48000);
    }
    uint32_t count = 1;
    assert(vocal_lines_merge(input, 2, 0, 1, &count) == AZ_INVALID);
    assert(count == 0);
    assert(vocal_lines_merge(NULL, 1, 48000, 1, &count) == AZ_INVALID);
    assert(count == 0);
    assert(vocal_lines_merge(input, 2, 48000, 1, NULL) == AZ_INVALID);
    span_is(input[0], 3, 4, 48000);
}

static void empty_singleton_and_extremes(void) {
    uint32_t count = 9;
    assert(vocal_lines_merge(NULL, 0, 48000, 0, &count) == AZ_OK);
    assert(count == 0);
    az_vocal_interval single = {0, 1};
    assert(vocal_lines_merge(&single, 1, 48000, 0, &count) == AZ_OK);
    assert(count == 1);
    span_is(single, 0, 1, 48000);
    az_vocal_interval huge[] = {{DBL_MAX / 2, DBL_MAX}, {0, 1}};
    assert(vocal_lines_merge(huge, 2, UINT32_MAX, DBL_MAX, &count) == AZ_OK);
    assert(count == 1);
    span_is(huge[0], 0, DBL_MAX, UINT32_MAX);
    az_vocal_interval microscopic = {0, DBL_MIN};
    assert(vocal_lines_merge(&microscopic, 1, 22050, 0, &count) == AZ_OK);
    assert(count == 1);
}

static void full_worker_capacity(void) {
    static az_vocal_interval input[65536];
    uint32_t count;
    /* Reverse chronological order and touching edges span the entire input. */
    for (uint32_t i = 0; i < 65536; i++)
        input[i] = (az_vocal_interval) {65535 - i, 65536 - i};
    assert(vocal_lines_merge(input, 65536, 22050, 0, &count) == AZ_OK);
    assert(count == 1);
    span_is(input[0], 0, 65536, 22050);
    /* At the same capacity, no interval may disappear when gaps equal the
     * setting. The output needs no extra slot for the final span. */
    for (uint32_t i = 0; i < 65536; i++)
        input[i] = (az_vocal_interval) {3 * (65535 - i), 3 * (65535 - i) + 1};
    assert(vocal_lines_merge(input, 65536, 48000, 2, &count) == AZ_OK);
    assert(count == 65536);
    for (uint32_t i = 0; i < count; i++)
        span_is(input[i], 3 * i, 3 * i + 1, 48000);
}

int main(void) {
    overlaps_and_chains();
    threshold_boundaries();
    invalid_inputs();
    empty_singleton_and_extremes();
    full_worker_capacity();
    puts("PASS vocal interval union, strict gaps, source clocks and invalid inputs");
    return 0;
}

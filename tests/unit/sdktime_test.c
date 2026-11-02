/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static struct timespec clock_value;
static int clock_status;
static int test_clock_gettime(clockid_t id, struct timespec *out) {
    assert(id == CLOCK_MONOTONIC);
    *out = clock_value;
    return clock_status;
}
#define clock_gettime test_clock_gettime
#include "sdk_data_engine.h"
#ifdef INPUT_TIME_IMPL
#include INPUT_TIME_IMPL
#endif
#undef clock_gettime
static void expect_clock(uint64_t expected) {
    assert(st_now() == expected);
#ifdef INPUT_TIME_IMPL
    assert(sdk_monotonic_ticks() == expected);
#endif
}
static void clock_cases(void) {
    clock_value = (struct timespec){12, 345};
    clock_status = 0;
    expect_clock(UINT64_C(12000000345));
    clock_status = -1;
    expect_clock(0); /* Never use timespec contents after a failed call. */
    clock_status = 0;
    clock_value = (struct timespec){-1, 0};
    expect_clock(0);
    clock_value = (struct timespec){0, -1};
    expect_clock(0);
    clock_value = (struct timespec){0, 1000000000L};
    expect_clock(0);
    clock_value = (struct timespec){18446744073LL, 709551615L};
    expect_clock(UINT64_MAX);
    clock_value.tv_nsec++;
    expect_clock(0); /* Addition must not wrap. */
    clock_value = (struct timespec){18446744074LL, 0};
    expect_clock(0); /* Multiplication must not wrap. */
}
static void budget_cases(void) {
#ifdef INPUT_TIME_IMPL
    assert(sdk_budget_for_frequency(19200000) == 3840); /* Preserve fractional MHz. */
    assert(sdk_budget_for_frequency(24000000) == 4800);
    assert(sdk_budget_for_frequency(1000001) == 200);
    assert(sdk_budget_for_frequency(999999) == 199);
    assert(sdk_budget_for_frequency(0) == 0);
    assert(sdk_budget_for_frequency(UINT64_MAX) == UINT64_C(3689348814741910));
#endif
}
static void elapsed_cases(void) {
#ifdef INPUT_TIME_IMPL
    assert(sdk_elapsed_ticks(10, 20) == 10);
    assert(sdk_elapsed_ticks(10, 10) == 0);
    assert(sdk_elapsed_ticks(0, 20) == 0); /* Missing initial clock. */
    assert(sdk_elapsed_ticks(20, 0) == 0); /* Missing final clock. */
    assert(sdk_elapsed_ticks(20, 10) == 0); /* Backwards clock, not an enormous overrun. */
    assert(sdk_elapsed_ticks(1, UINT64_MAX) == UINT64_MAX - 1);
#endif
}
static void grid_cases(void) {
    az_beat beats[] = {{0,120,1,0},{.5,120,2,0},{1.5,60,3,0}};
    az_track_snapshot s = {.size=sizeof s, .version=AZ_DATA_VERSION, .beat_count=3, .beats=beats};
    s.grid_info.availability = AZ_AVAIL_READY;
    double out = 123;
    /* Stored grids have their own time resolution; preserve fractional beats
     * rather than imposing an unrelated audio or millisecond rounding step. */
    double t = 81673.0 / 44100;
    beats[2].time_s = t;
    assert(st_time_to_beat(&s, t, &out) == AZ_OK && out == 2);
    assert(st_beat_to_time(&s, 2, &out) == AZ_OK && out == t);
    assert(st_beat_to_time(&s, 1.25, &out) == AZ_OK);
    double back = 0;
    assert(st_time_to_beat(&s, out, &back) == AZ_OK && fabs(back - 1.25) < 1e-15);
    const double invalid[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i=0; i<3; i++) for (unsigned j=0; j<3; j++) {
        beats[0].time_s=0; beats[1].time_s=.5; beats[2].time_s=1.5;
        beats[i].time_s=invalid[j];
        out=123;
        assert(st_beat_to_time(&s, i == 2 ? 2 : i + .25, &out) == AZ_FAILED && out == 123);
    }
    beats[0].time_s=0; beats[1].time_s=.5; beats[2].time_s=INFINITY;
    out=123;
    assert(st_time_to_beat(&s, .25, &out) == AZ_FAILED && out == 123);
    beats[2].time_s=.5; /* A zero-length selected beat is unusable. */
    assert(st_beat_to_time(&s, 1.5, &out) == AZ_FAILED && out == 123);
    beats[2].time_s=.25; /* A reversed selected beat must not interpolate backwards. */
    assert(st_beat_to_time(&s, 1.5, &out) == AZ_FAILED && out == 123);
}
static void interval_cases(void) {
    az_time_interval out = {.size=sizeof out};
    assert(st_interval(NULL, 0, DBL_MIN, DBL_MAX, &out) == AZ_OK);
    assert((out.valid & AZ_INTERVAL_SOURCE) && compare_timestamps(out.source_s, 0., 1, 1000, 1) == AZ_TIME_EQUAL);
    assert((out.valid & AZ_INTERVAL_PLAYBACK) && compare_timestamps(out.playback_s, 0., 1, 1000, 1) == AZ_TIME_EQUAL); /* Sub-window endpoints form a zero interval. */
    assert(out.playback_rate == DBL_MAX);
    assert(st_interval(NULL, 0, 0, DBL_MAX, &out) == AZ_OK);
    assert((out.valid & AZ_INTERVAL_PLAYBACK) && compare_timestamps(out.playback_s, 0., 1, 1000, 1) == AZ_TIME_EQUAL);
    assert(st_interval(NULL, 0, 1, DBL_MIN, &out) == AZ_OK && (out.valid & AZ_INTERVAL_PLAYBACK));
    assert(isfinite(out.playback_s) && compare_timestamps(out.playback_s, 0., 1, 1000, 1) == AZ_TIME_AFTER);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "clock")) clock_cases();
    else if (!strcmp(argv[1], "budget")) budget_cases();
    else if (!strcmp(argv[1], "elapsed")) elapsed_cases();
    else if (!strcmp(argv[1], "grid")) grid_cases();
    else { assert(!strcmp(argv[1], "interval")); interval_cases(); }
    printf("SDK time: %s passed\n", argv[1]);
}

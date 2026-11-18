/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile the exact production arithmetic helper, without a native ELF/ABI. */
#include "aztimestamp.h"
#include <assert.h>
#include <stdbool.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>

#include SAMPLE_TIME_IMPL
static void cases() {
    assert(source_time_consistent_us(81673, 44100, 1852000)); /* Actual Deck2 IN. */
    assert(source_time_consistent_us(247048, 44100, 5601995)); /* Actual Deck2 OUT. */
    assert(source_time_consistent_us(143746, 44100, 3259546)); /* Actual Deck1 IN. */
    assert(source_time_consistent_us(247061, 44100, 5602290)); /* Actual Deck1 OUT. */
    assert(!source_time_consistent_us(81673, 44100, 1852)); /* Milliseconds mistaken for us. */
    assert(!source_time_consistent_us(81673, 44100, 1853000)); /* Millisecond-wide tolerance. */
    assert(!source_time_consistent_us(0, 0, 0));
    assert(source_time_consistent_us(0, 44100, 0));
    struct Case {
        uint32_t rate;
        uint64_t accepted, rejected;
    };
    {
        const struct Case values[] = {(struct Case) {44100, 23, 24}, (struct Case) {48000, 21, 22}, (struct Case) {96000, 10, 11},
                                      (struct Case) {192000, 5, 6}, (struct Case) {768000, 1, 2}, (struct Case) {2000000, 1, 2},
                                      (struct Case) {2147483646, 0, 1}};
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            struct Case c = values[i];
        /* Clock translation must not change the decision: cover zero, short
         * offsets and long tracks independently of any captured deck or track. */
            {
                const uint64_t values[] = {0u, 1u, 17u, 3600u, 86400u};
                for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
                    uint64_t seconds = values[i];
                    uint64_t samples = seconds * c.rate, us = seconds * 1000000;
                    assert(source_time_consistent_us(samples, c.rate, us + c.accepted));
                    assert(!source_time_consistent_us(samples, c.rate, us + c.rejected));
                    if (us >= c.rejected) {
                        assert(source_time_consistent_us(samples, c.rate, us - c.accepted));
                        assert(!source_time_consistent_us(samples, c.rate, us - c.rejected));
                    }
                }
            }
        }
    }
    assert(source_time_consistent_us(1, 128, 7812));
    assert(source_time_consistent_us(1, 128, 7813));
    assert(source_time_consistent_us(1, 128, 0)); /* One whole source sample, before cache rounding. */
    assert(!source_time_consistent_us(2, 128, 0)); /* Two source samples. */
    assert(source_time_consistent_us(UINT64_MAX, 1000000, UINT64_MAX));
    assert(!source_time_consistent_us(UINT64_MAX, 1000000, UINT64_MAX - 2));
    assert(!source_time_consistent_us(UINT64_MAX, 1, UINT64_MAX));
}
int main() {
    int saved = fegetround();
    {
        const int values[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            int mode = values[i];
            assert(!fesetround(mode));
            cases();
        }
    }
    assert(!fesetround(saved));
    puts("SDK sample time: captured loops, rate-dependent boundaries, wide arithmetic and FP modes passed");
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile the exact production arithmetic helper, without a native ELF/ABI. */
#include "aztimestamp.h"
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
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
    struct Case { std::uint32_t rate; std::uint64_t accepted, rejected; };
    for (Case c : {Case{44100,23,24}, Case{48000,21,22}, Case{96000,10,11},
                   Case{192000,5,6}, Case{768000,1,2}, Case{2000000,1,2},
                   Case{2147483646,0,1}}) {
        /* Clock translation must not change the decision: cover zero, short
         * offsets and long tracks independently of any captured deck or track. */
        for (std::uint64_t seconds : {0u, 1u, 17u, 3600u, 86400u}) {
            std::uint64_t samples = seconds * c.rate, us = seconds * 1000000;
            assert(source_time_consistent_us(samples, c.rate, us + c.accepted));
            assert(!source_time_consistent_us(samples, c.rate, us + c.rejected));
            if (us >= c.rejected) {
                assert(source_time_consistent_us(samples, c.rate, us - c.accepted));
                assert(!source_time_consistent_us(samples, c.rate, us - c.rejected));
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
    int saved = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        assert(!std::fesetround(mode));
        cases();
    }
    assert(!std::fesetround(saved));
    std::puts("SDK sample time: captured loops, rate-dependent boundaries, wide arithmetic and FP modes passed");
}

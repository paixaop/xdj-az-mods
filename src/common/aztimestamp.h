/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZTIMESTAMP_H
#define AZTIMESTAMP_H
#include <stdint.h>

enum az_timestamp_order {
    AZ_TIME_BEFORE = -1,
    AZ_TIME_EQUAL = 0,
    AZ_TIME_AFTER = 1,
    AZ_TIME_INVALID = 2
};

/* Both timestamps use units_per_second ticks/second in the same clock domain.
 * The observation window is window_samples/sample_rate seconds. Equality is
 * symmetric and strictly inside that window, including across bucket edges.
 * Missing clocks/identities remain the caller's validity checks. Approximate
 * equality is not transitive: this comparator is not a sorting predicate. */
static inline int az_compare_integer_timestamps(uint64_t first, uint64_t second,
                                                uint64_t units_per_second,
                                                uint64_t sample_rate,
                                                uint64_t window_samples) {
    if (!units_per_second || !sample_rate || !window_samples) return AZ_TIME_INVALID;
    uint64_t difference = first > second ? first - second : second - first;
    if ((__uint128_t) difference * sample_rate < (__uint128_t) window_samples * units_per_second)
        return AZ_TIME_EQUAL;
    return first < second ? AZ_TIME_BEFORE : AZ_TIME_AFTER;
}

/* Floating timestamps (normally seconds) keep their precision through the
 * difference calculation; integer clocks use the exact wide-integer path. */
static inline int az_compare_floating_timestamps(long double first, long double second,
                                                 uint64_t units_per_second,
                                                 uint64_t sample_rate,
                                                 uint64_t window_samples) {
    if (!units_per_second || !sample_rate || !window_samples ||
        !__builtin_isfinite(first) || !__builtin_isfinite(second)) return AZ_TIME_INVALID;
    long double difference = __builtin_fabsl(first - second);
    if (difference * sample_rate < (long double) window_samples * units_per_second)
        return AZ_TIME_EQUAL;
    return first < second ? AZ_TIME_BEFORE : AZ_TIME_AFTER;
}

/* Exact common-clock products can exceed 64 bits when two source rates differ. */
static inline int az_compare_wide_timestamps(__uint128_t first, __uint128_t second,
                                             uint64_t units_per_second, uint64_t sample_rate,
                                             uint64_t window_samples) {
    if (!units_per_second || !sample_rate || !window_samples) return AZ_TIME_INVALID;
    __uint128_t difference = first > second ? first - second : second - first;
    if (difference <= ~(__uint128_t) 0 / sample_rate &&
        difference * sample_rate < (__uint128_t) window_samples * units_per_second) return AZ_TIME_EQUAL;
    return first < second ? AZ_TIME_BEFORE : AZ_TIME_AFTER;
}

#ifdef __cplusplus
#include <type_traits>
/* Source/cache clock conversion can produce wider intermediate ticks. Avoid
 * multiplying an already-wide delta past 128 bits; an overflowing product is
 * necessarily outside the at-most-128-bit observation window. */
static inline int compare_timestamps(__uint128_t first, __uint128_t second,
                                     uint64_t units_per_second, uint64_t sample_rate,
                                     uint64_t window_samples) {
    return az_compare_wide_timestamps(first, second, units_per_second, sample_rate, window_samples);
}
template <class First, class Second>
static inline int compare_timestamps(First first, Second second,
                                     uint64_t units_per_second,
                                     uint64_t sample_rate,
                                     uint64_t window_samples) {
    static_assert(std::is_arithmetic<First>::value && std::is_arithmetic<Second>::value,
                  "timestamps must be arithmetic values");
    if (std::is_integral<First>::value && std::is_integral<Second>::value) {
        if ((std::is_signed<First>::value && first < 0) ||
            (std::is_signed<Second>::value && second < 0)) return AZ_TIME_INVALID;
        return az_compare_integer_timestamps(static_cast<uint64_t>(first), static_cast<uint64_t>(second),
                                             units_per_second, sample_rate, window_samples);
    }
    return az_compare_floating_timestamps(first, second, units_per_second, sample_rate, window_samples);
}
#else
/* Preserve each operand's signedness before the usual arithmetic conversions.
 * The selected constructor evaluates its argument once. Floating comparisons
 * deliberately retain signed coordinates, including mixed integer/float calls. */
struct az_timestamp_operand {
    __uint128_t integer;
    long double floating;
    int is_floating, negative;
};
static inline struct az_timestamp_operand az_timestamp_signed(int64_t value) {
    struct az_timestamp_operand out = {(uint64_t) value, (long double) value, 0, value < 0};
    return out;
}
static inline struct az_timestamp_operand az_timestamp_unsigned(uint64_t value) {
    struct az_timestamp_operand out = {value, (long double) value, 0, 0};
    return out;
}
static inline struct az_timestamp_operand az_timestamp_wide_unsigned(__uint128_t value) {
    struct az_timestamp_operand out = {value, (long double) value, 0, 0};
    return out;
}
static inline struct az_timestamp_operand az_timestamp_wide_signed(__int128_t value) {
    struct az_timestamp_operand out = {(__uint128_t) value, (long double) value, 0, value < 0};
    return out;
}
static inline struct az_timestamp_operand az_timestamp_floating(long double value) {
    struct az_timestamp_operand out = {0, value, 1, 0};
    return out;
}
static inline int az_compare_timestamp_operands(struct az_timestamp_operand first,
                                                struct az_timestamp_operand second,
                                                uint64_t units, uint64_t rate, uint64_t window) {
    if (first.is_floating || second.is_floating)
        return az_compare_floating_timestamps(first.floating, second.floating, units, rate, window);
    if (first.negative || second.negative) return AZ_TIME_INVALID;
    return az_compare_wide_timestamps(first.integer, second.integer, units, rate, window);
}
#define AZ_TIMESTAMP_OPERAND(value)                \
    _Generic((value),                              \
        float: az_timestamp_floating,              \
        double: az_timestamp_floating,             \
        long double: az_timestamp_floating,        \
        unsigned char: az_timestamp_unsigned,      \
        unsigned short: az_timestamp_unsigned,     \
        unsigned int: az_timestamp_unsigned,       \
        unsigned long: az_timestamp_unsigned,      \
        unsigned long long: az_timestamp_unsigned, \
        __uint128_t: az_timestamp_wide_unsigned,   \
        __int128_t: az_timestamp_wide_signed,      \
        default: az_timestamp_signed)(value)
#define compare_timestamps(first, second, units, rate, window)                               \
    az_compare_timestamp_operands(AZ_TIMESTAMP_OPERAND(first), AZ_TIMESTAMP_OPERAND(second), \
                                  (units), (rate), (window))
#endif
#endif

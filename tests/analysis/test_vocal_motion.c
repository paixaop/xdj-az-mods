/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/vocal-motion.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 8, LAG_MIN = -2, LAG_MAX = 2 };

typedef struct {
    uint8_t bytes[256];
    size_t used;
    uint8_t *records[11];
} RecipeFixture;

static void store16(uint8_t *out, unsigned value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void store64(uint8_t *out, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    for (unsigned i = 0; i < sizeof bits; ++i)
        out[i] = (uint8_t)(bits >> (8 * i));
}

static uint8_t *append(RecipeFixture *recipe, uint8_t tag, size_t size)
{
    assert(recipe->used + 3 + size <= sizeof recipe->bytes);
    uint8_t *record = recipe->bytes + recipe->used;
    record[0] = tag;
    store16(record + 1, (unsigned)size);
    recipe->used += 3 + size;
    return recipe->records[tag] = record + 3;
}

static void reductions(RecipeFixture *recipe)
{
    /* Independent generator for a left-fold sum over equal-size lanes. */
    static const uint8_t metadata[][4] = {
        {3, 1, 2, 0}, {1, 1, 0, 0}, {2, 3, 0, 0},
        {2, 1, 0, 0}, {3, 5, 4, 2}, {3, 5, 4, 1}
    };
    for (uint8_t tag = 1; tag <= 6; ++tag) {
        uint8_t lanes = tag == 3 || tag == 4 ? 4 : 8;
        uint8_t *record = append(recipe, tag, 7 + 2 * (lanes - 1));
        record[0] = lanes;
        memcpy(record + 1, metadata[tag - 1], sizeof metadata[0]);
        record[5] = lanes - 1;
        for (unsigned i = 0; i < (unsigned)lanes - 1; ++i) {
            record[6 + 2 * i] = i == 0 ? 0 : (uint8_t)(lanes + i - 1);
            record[7 + 2 * i] = (uint8_t)(i + 1);
        }
        record[4 + 2 * lanes] = (uint8_t)(2 * lanes - 2);
    }
}

static void fixture(RecipeFixture *recipe)
{
    memcpy(recipe->bytes, "PVDIMOT1", 8);
    recipe->used = 8;
    reductions(recipe);
    uint8_t *energy = append(recipe, 7, 20);
    energy[0] = 3;
    energy[1] = 1;
    store16(energy + 2, WIDTH);
    store64(energy + 4, 0.25);
    store64(energy + 12, 0.75);
    uint8_t *flatness = append(recipe, 8, 38);
    memcpy(flatness, "\x04\x05\x06\x02", 4);
    store16(flatness + 4, WIDTH);
    store64(flatness + 6, 0.125);
    store64(flatness + 14, 1.0 / WIDTH);
    store64(flatness + 22, 0.125);
    store64(flatness + 30, 0.25);
    uint8_t *correlation = append(recipe, 9, 20);
    correlation[0] = 7;
    correlation[1] = 8;
    store16(correlation + 2, WIDTH);
    store16(correlation + 4, WIDTH / 2 + LAG_MIN);
    store16(correlation + 6, WIDTH / 2 + LAG_MAX);
    store16(correlation + 8, WIDTH / 2);
    store16(correlation + 10, 0x1f);
    store64(correlation + 12, 2);
    uint8_t *selection = append(recipe, 10, 26);
    memcpy(selection, "\x04\x02\x01\x09\x0a", 5);
    store64(selection + 5, -INFINITY);
    memcpy(selection + 13, "\x02\x01\x03\x00\x00\x03\x01\x02\x01\x00", 10);
    memcpy(selection + 23, "\x0b\x0c\x0d", 3);
}

static bool load(ta_vocal_motion *motion, const RecipeFixture *recipe)
{
    return ta_vocal_motion_read(motion, recipe->bytes, recipe->used, WIDTH, LAG_MIN, LAG_MAX);
}

static void validate(RecipeFixture *recipe)
{
    ta_vocal_motion motion;
    assert(load(&motion, recipe));
    for (size_t length = 0; length < recipe->used; ++length)
        assert(!ta_vocal_motion_read(&motion, recipe->bytes, length, WIDTH, LAG_MIN, LAG_MAX));
    assert(!ta_vocal_motion_read(&motion, recipe->bytes, recipe->used, 0, LAG_MIN, LAG_MAX));
    assert(!ta_vocal_motion_read(&motion, recipe->bytes, recipe->used, UINT16_MAX + 1u, LAG_MIN, LAG_MAX));
    assert(!ta_vocal_motion_read(&motion, recipe->bytes, recipe->used, WIDTH, LAG_MIN - 1, LAG_MAX));
    assert(!ta_vocal_motion_read(&motion, recipe->bytes, recipe->used, WIDTH, LAG_MIN, LAG_MAX + 1));
    struct Mutation { uint8_t tag; int offset; uint8_t value; };
    static const struct Mutation mutations[] = {
        {0, 0, 0},       /* Invalid magic. */
        {1, -3, 0},      /* Invalid record type. */
        {2, -3, 1},      /* Duplicate record type. */
        {1, -2, 6},      /* Short reduction. */
        {1, 1, 1},       /* Unexpected update operation. */
        {1, 0, 1},       /* Unsupported lane count. */
        {1, 0, 9},       /* Unsupported lane count. */
        {1, 5, 3},       /* Wrong addition count. */
        {1, 6, 30},      /* Forward reduction input. */
        {1, 7, 0},       /* Same input twice. */
        {1, 8, 0},       /* Reused previous input. */
        {1, 20, 7},      /* Wrong tree root. */
        {7, 0, 1},       /* Unsupported epsilon placement. */
        {7, 2, 0},       /* Disagreeing band width. */
        {7, 11, 0xff},   /* Invalid epsilon. */
        {8, 2, 5},       /* Wrong mean operation. */
        {8, 5, 1},       /* Disagreeing band width. */
        {8, 13, 0xff},   /* Invalid log epsilon. */
        {9, 1, 7},       /* Unsupported center operation. */
        {9, 4, 5},       /* First candidate beyond center. */
        {9, 6, 8},       /* Candidate outside the band. */
        {9, 8, 7},       /* Center beyond last candidate. */
        {9, 10, 0x3f},   /* Mask normalizes an absent candidate. */
        {9, 10, 0x1b},   /* Mask leaves the center unnormalized. */
        {9, 19, 0xff},   /* Invalid reciprocal numerator. */
        {10, 0, 8},      /* Unsupported wide selector. */
        {10, 4, 2},      /* Unsupported equal-score rule. */
        {10, 12, 0x7f},  /* Invalid initial score. */
        {10, 13, 4},     /* Selector lane outside the vector. */
        {10, 14, 4},     /* Selector lane outside the vector. */
        {10, 14, 2},     /* Max operation cannot reach every lane. */
        {10, 21, 2},     /* Invalid narrow selector. */
        {10, 24, 2}      /* Unsupported final-group operation. */
    };
    for (size_t i = 0; i < sizeof mutations / sizeof *mutations; ++i) {
        const struct Mutation *patch = &mutations[i];
        uint8_t *record = patch->tag == 0 ? recipe->bytes : recipe->records[patch->tag];
        uint8_t *target = record + patch->offset;
        uint8_t saved = *target;
        *target = patch->value;
        assert(!load(&motion, recipe));
        *target = saved;
    }
    assert(load(&motion, recipe));
}

static void reordered_records(const RecipeFixture *recipe)
{
    uint8_t reordered[sizeof recipe->bytes];
    const uint8_t *last = recipe->records[10] - 3;
    size_t before = (size_t)(last - recipe->bytes) - 8;
    size_t tail = recipe->used - (size_t)(last - recipe->bytes);
    memcpy(reordered, recipe->bytes, 8);
    memcpy(reordered + 8, last, tail);
    memcpy(reordered + 8 + tail, recipe->bytes + 8, before);

    ta_vocal_motion motion;
    assert(ta_vocal_motion_read(&motion, reordered, recipe->used, WIDTH, LAG_MIN, LAG_MAX));
    assert(motion.energy_epsilon == 0.25 && motion.center == 2);
}

static void gates(const ta_vocal_motion *motion)
{
    const double spectrum[WIDTH] = { 8 }, window[WIDTH] = { 1 };
    const double zero[WIDTH] = { 0 }, logs[WIDTH] = { -16 }, unordered[WIDTH] = { NAN };
    assert(ta_vocal_motion_gate(motion, spectrum, window, logs));
    assert(!ta_vocal_motion_gate(motion, spectrum, window, zero));
    assert(!ta_vocal_motion_gate(motion, spectrum, zero, logs));
    assert(!ta_vocal_motion_gate(motion, unordered, window, logs));
    assert(ta_vocal_motion_gate(motion, spectrum, window, unordered));

    ta_vocal_motion equality = *motion;
    equality.energy_min = 8 / (equality.energy_epsilon + 8);
    assert(!ta_vocal_motion_gate(&equality, spectrum, window, logs));
    equality.energy_min = motion->energy_min;
    equality.flatness_max = 1 / fma(8, equality.mean_scale, equality.mean_epsilon);
    assert(!ta_vocal_motion_gate(&equality, spectrum, window, zero));
}

static void reduction_order(RecipeFixture *recipe)
{
    const double spectrum[WIDTH] = { 0x1p53, 1, 1, 1, 1, 1, 1, 1 };
    const double window[WIDTH] = { 1 }, logs[WIDTH] = { -16 };
    ta_vocal_motion motion;
    uint8_t *energy = recipe->records[7], *magnitude = recipe->records[2];
    store64(energy + 12, 0x1.fffffffffffffp-1);
    assert(load(&motion, recipe));
    assert(ta_vocal_motion_gate(&motion, spectrum, window, logs));

    const uint8_t balanced[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    memcpy(magnitude + 6, balanced, sizeof balanced);
    assert(load(&motion, recipe));
    assert(!ta_vocal_motion_gate(&motion, spectrum, window, logs));
}

static void lags(const ta_vocal_motion *motion)
{
    const double equal[WIDTH] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const double zero[WIDTH] = { 0 }, pulse[WIDTH] = { [2] = 3 };
    const double left[WIDTH] = { [1] = 3, [2] = 2 }, right[WIDTH] = { [2] = 2, [4] = 3 };
    const double tie[WIDTH] = { [1] = 3, [2] = 2, [3] = 3 };
    assert(ta_vocal_motion_lag(motion, equal, equal) == 0);
    assert(ta_vocal_motion_lag(motion, zero, zero) == LAG_MIN);
    assert(ta_vocal_motion_lag(motion, left, pulse) == -1);
    assert(ta_vocal_motion_lag(motion, right, pulse) == 2);
    assert(ta_vocal_motion_lag(motion, tie, pulse) == -1);

    ta_vocal_motion masked = *motion;
    const double small_right[WIDTH] = { [2] = 0.25, [4] = 0.5 };
    masked.normalized &= ~(1u << (masked.center + 2));
    assert(ta_vocal_motion_lag(motion, small_right, pulse) == 2);
    assert(ta_vocal_motion_lag(&masked, small_right, pulse) == 0);
    assert(ta_vocal_motion_lag(motion, pulse, right) == -2);
}

int main(void)
{
    RecipeFixture recipe = {0};
    ta_vocal_motion motion;
    fixture(&recipe);
    validate(&recipe);
    reordered_records(&recipe);
    assert(load(&motion, &recipe));
    gates(&motion);
    lags(&motion);
    reduction_order(&recipe);
    puts("Windows vocal motion recipe validation and replay passed");
    return 0;
}

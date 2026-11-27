/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_VOCAL_MOTION_H
#define TA_VOCAL_MOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TA_MOTION_LANES = 8,
    TA_MOTION_ADDITIONS = TA_MOTION_LANES - 1,
    TA_MOTION_SELECTION_LANES = 4,
    TA_MOTION_CANDIDATES = 16
};

typedef struct {
    uint8_t lanes, adds[TA_MOTION_ADDITIONS][2];
} ta_motion_reduction;

typedef struct {
    ta_motion_reduction weighted, magnitude, logs, flatness_magnitude, negative, nonnegative;
    double energy_epsilon, energy_min, log_epsilon, mean_scale, mean_epsilon, flatness_max;
    double reciprocal_numerator, selection_seed;
    unsigned width, candidates, center, normalized;
    unsigned selection_lanes;
    uint8_t adjacent[TA_MOTION_SELECTION_LANES], halves[TA_MOTION_SELECTION_LANES];
} ta_vocal_motion;

bool ta_vocal_motion_read(ta_vocal_motion *motion, const uint8_t *bytes, size_t size,
                          unsigned width, int min_lag, int max_lag);
bool ta_vocal_motion_gate(const ta_vocal_motion *motion, const double *spectrum,
                          const double *window, const double *logs);
int ta_vocal_motion_lag(const ta_vocal_motion *motion, const double *previous,
                        const double *following);

#endif

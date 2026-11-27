/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZANALYSIS_POLICY_H
#define AZANALYSIS_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum {
#define AZ_ANALYSIS_SETTING(name, value) AZ_ANALYSIS_POLICY_##name = value,
#include "analysis-policy.def"
#undef AZ_ANALYSIS_SETTING
};

static inline bool az_analysis_source_rate_supported(unsigned rate) {
    return rate == AZ_ANALYSIS_POLICY_ALLOWED_RATE_LOW || rate == AZ_ANALYSIS_POLICY_ALLOWED_RATE_HIGH;
}

static inline bool az_analysis_phase_supported(unsigned phase) {
    return phase >= AZ_ANALYSIS_POLICY_MINIMUM_PHASE && phase <= AZ_ANALYSIS_POLICY_MAXIMUM_PHASE;
}

static inline double az_analysis_offset_round_bias(void) {
    return (double) AZ_ANALYSIS_POLICY_OFFSET_ROUND_NUMERATOR / AZ_ANALYSIS_POLICY_OFFSET_ROUND_DENOMINATOR;
}

static inline double az_analysis_stored_time(double seconds) {
    return seconds * AZ_ANALYSIS_POLICY_STORED_MILLISECONDS_PER_SECOND + az_analysis_offset_round_bias();
}

static inline double az_analysis_maximum_stored_bpm(void) {
    return (double) UINT16_MAX / AZ_ANALYSIS_POLICY_TEMPO_UNITS_PER_BPM;
}

#endif

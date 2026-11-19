/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_ENCODING_H
#define AZ_ANALYSIS_ENCODING_H
#include "azmod-mod.h"
#include <stdint.h>
struct sa_tags {
    uint32_t gn, pn, vn, rate, frames, method;
    const uint8_t *grid, *phrase, *vocal;
};
int sa_result_valid(const az_analysis_result_v1 *result);
uint8_t *sa_encode_native(const az_analysis_result_v1 *result, struct sa_tags *tags);
#endif

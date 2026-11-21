/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_VOCAL_LINES_INTERVALS_H
#define AZ_VOCAL_LINES_INTERVALS_H
#include "azmod-mod.h"
/* Worker-only, in-place sorted union. Invalid input is unchanged. Positive gaps
 * join iff strictly below gap_s, including inside a sample equality window. */
int vocal_lines_merge(az_vocal_interval *intervals, uint32_t count,
                      uint32_t sample_rate, double gap_s, uint32_t *merged_count);
#endif

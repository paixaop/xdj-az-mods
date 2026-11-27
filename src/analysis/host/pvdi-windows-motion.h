/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_PVDI_WINDOWS_MOTION_H
#define AZ_PVDI_WINDOWS_MOTION_H

#include "../model-format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PVDI_WINDOWS_MOTION_BYTES = TA_MODEL_WINDOWS_MOTION_BYTES };

/* Extract the binary64 motion recipe from an authenticated Windows x64 PE.
 * Cohort indexes 0 through 3 correspond to 7.2.17.0 through 7.2.20.0.
 * On failure, a supplied recipe_size becomes zero and recipe stays untouched. */
bool pvdi_windows_motion_extract(const uint8_t *pe, size_t pe_size, unsigned cohort,
                                 uint8_t *recipe, size_t recipe_capacity, size_t *recipe_size);

#endif

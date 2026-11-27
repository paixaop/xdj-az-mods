/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_PVDI_WINDOWS_REDUCTION_H
#define AZ_PVDI_WINDOWS_REDUCTION_H

#include "../model-format.h"
#include <stdbool.h>

bool pvdi_windows_reduction_extract(const uint8_t *pe, size_t pe_size, size_t build,
                                    uint8_t output[TA_MODEL_WINDOWS_REDUCTION_BYTES]);

#endif

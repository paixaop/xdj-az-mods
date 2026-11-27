/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_PVDI_WINDOWS_SOURCE_H
#define AZ_PVDI_WINDOWS_SOURCE_H

#include "../model-format.h"
#include "pvdi-windows-motion.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PVDI_WINDOWS_GRAPH_BYTES = 6758758,
    PVDI_WINDOWS_CHEBYSHEV_COUNT = 240,
    PVDI_WINDOWS_AFFINE_COUNT = 29
};

struct pvdi_windows_vocal {
    double value[TA_VOCAL_PARAMETER_COUNT];
    bool present[TA_VOCAL_PARAMETER_COUNT];
    uint8_t motion[PVDI_WINDOWS_MOTION_BYTES];
};

struct pvdi_windows_phrase {
    double value[TA_PHRASE_PARAMETER_COUNT];
    bool present[TA_PHRASE_PARAMETER_COUNT];
    uint8_t reduction[TA_MODEL_WINDOWS_REDUCTION_BYTES];
};

struct pvdi_windows_network {
    double window[PVDI_WINDOWS_CHEBYSHEV_COUNT];
    double scale[PVDI_WINDOWS_AFFINE_COUNT];
    double offset[PVDI_WINDOWS_AFFINE_COUNT];
    float thresholds[TA_VOCAL_THRESHOLD_COUNT];
    uint32_t output_class;
};

struct pvdi_windows_source {
    const char *version;
    uint8_t *executable;
    uint32_t executable_bytes;
    uint8_t *graph;
    struct pvdi_windows_vocal vocal;
    struct pvdi_windows_phrase phrase;
    struct pvdi_windows_network network;
};

enum pvdi_windows_status {
    PVDI_WINDOWS_OK,
    PVDI_WINDOWS_EXECUTABLE_IO,
    PVDI_WINDOWS_UNSUPPORTED_EXECUTABLE,
    PVDI_WINDOWS_GRAPH_MISMATCH,
    PVDI_WINDOWS_INVALID_OPERAND
};

/* The caller owns both streams. Release a loaded source before reusing it. */
enum pvdi_windows_status pvdi_windows_load(FILE *executable, FILE *encrypted_graph,
                                          struct pvdi_windows_source *source);
void pvdi_windows_release(struct pvdi_windows_source *source);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_ANALYSIS_H
#define TA_ANALYSIS_H
#include "azmod-mod.h"
#include <stddef.h>
/* Analysis and all joined children stay inside core. Inputs remain
 * borrowed until the call returns; the result owns its typed output arrays. */
typedef int (*ta_cancel_fn)(void *user);
typedef enum { TA_STAGES_SEQUENTIAL, TA_STAGES_CONCURRENT } ta_stage_mode;
typedef struct {
    ta_stage_mode mode;
    unsigned phrase_threads, vocal_threads;
} ta_execution;
/* Diagnostics run on the corresponding stage thread. Arrays are borrowed only
 * during the callback. Cancellation callbacks must support concurrent calls. */
typedef struct {
    void (*profile)(void *user, const char *stage, double elapsed_seconds);
    void (*vocal_features)(void *user, const float (*features)[29], uint32_t count);
    void (*vocal_probabilities)(void *user, const float *probabilities, uint32_t count);
    void *user;
} ta_diagnostics;
typedef struct ta_request {
    const char *audio_path, *assets_path;
    const az_beat *beats;
    uint32_t beat_count, needed;
    double bpm;
    ta_cancel_fn cancelled;
    void *user;
    const ta_execution *execution; /* NULL: original TRACK_ANALYSIS_* environment defaults. */
    const ta_diagnostics *diagnostics;
} ta_request;
typedef struct ta_result ta_result;
enum { TA_OK = 0, TA_FAILED = -1, TA_CANCELLED = -2, TA_LIMIT = -3 };
int ta_execution_environment(uint32_t needed, ta_execution *execution, char *error, size_t size);
int ta_analyze(const ta_request *request, ta_result **result, char *error, size_t error_size);
const az_analysis_result_v1 *ta_result_view(const ta_result *result);
void ta_result_free(ta_result *result);
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#include "azmod-timestamps.h"
#include "phrase.h"
struct ta_result {
    az_analysis_result_v1 view;
    az_analysis_phrase_v1 *phrases;
    az_vocal_frame_v1 *vocals;
};
const az_analysis_result_v1 *ta_result_view(const ta_result *result)
{
    return result ? &result->view : NULL;
}
void ta_result_free(ta_result *result)
{
    if (!result)
        return;
    ta_free(result->phrases);
    ta_free(result->vocals);
    ta_free(result);
}
typedef struct {
    ta_result *result;
    unsigned rate;
    uint64_t frames;
} analysis_stages;
static int valid_native_beat(const az_beat *b)
{
    return !(!isfinite(b->time_s) || b->time_s < 0 || !isfinite(b->bpm) || b->bpm <= 0 ||
             b->bpm > 655.35 || b->beat_in_bar < 1 || b->beat_in_bar > 4 || b->reserved);
}

static void validate_beat_order(ta_context *c, const az_beat *b, const az_beat *previous, unsigned rate)
{
    int order = compare_timestamps(b->time_s, previous->time_s, 1, rate, 1);
    if (order == AZ_TIME_INVALID || order == AZ_TIME_EQUAL || order == AZ_TIME_BEFORE)
        ta_fail(c, TA_FAILED, "unordered native beat grid");
}

static void validate_native_grid(ta_context *c, const ta_request *request, unsigned rate)
{
    if (!request->beats || !request->beat_count || request->beat_count > 32768)
        ta_fail(c, TA_FAILED, "missing native beat grid");
    for (uint32_t i = 0; i < request->beat_count; i++) {
        const az_beat *b = &request->beats[i];
        if (!valid_native_beat(b)) ta_fail(c, TA_FAILED, "invalid native beat grid");
        if (i) validate_beat_order(c, b, &request->beats[i - 1], rate);
    }
}

static void retain_phrases(ta_context *c, ta_result *result, PhraseExport phrases, uint32_t beat_count)
{
    if (!phrases.map.count || phrases.map.count > 1024 || !phrases.map.end_beat ||
        phrases.map.end_beat > beat_count)
        ta_fail(c, TA_FAILED, "phrase result exceeds native grid");
    size_t size = phrases.map.count * sizeof *result->phrases;
    result->phrases = ta_malloc(size);
    if (!result->phrases)
        ta_fail(c, TA_LIMIT, "phrase result allocation failed");
    memcpy(result->phrases, phrases.map.entries, size);
    result->view.phrases = phrases.map;
    result->view.phrases.entries = result->phrases;
}

static void run_phrase_stage(ta_context *c, analysis_stages *job)
{
    const ta_request *request = c->request;
    ta_result *result = job->result;
    unsigned rate = job->rate;
    uint64_t frames = job->frames;
    ta_profile_begin(c);
    validate_native_grid(c, request, rate);
    const az_beat *first = request->beats;
    double bpm = isfinite(request->bpm) && request->bpm > 0 ? request->bpm : first->bpm;
    if (bpm > 655.35)
        ta_fail(c, TA_FAILED, "invalid native tempo");
    Timing timing = reference_timing(c, bpm, first->time_s, first->beat_in_bar, first->bpm);
    Samples samples = phrase_decode(c);
    if ((unsigned)samples.rate != rate || (uint64_t)samples.frames + 1 != frames)
        ta_fail(c, TA_FAILED, "source clock changed");
    ta_profile_mark(c, "decode_bands_grid");
    PhraseExport phrases = phrase_classify(c, samples, timing);
    retain_phrases(c, result, phrases, request->beat_count);
}

static void run_vocal_stage(ta_context *c, analysis_stages *job)
{
    const ta_request *request = c->request;
    ta_result *result = job->result;
    char model[1200];
    if (!request->assets_path || snprintf(model, sizeof model, "%s/pvdi-model.bin",
                                          request->assets_path) >= (int)sizeof model)
        ta_fail(c, TA_FAILED, "missing vocal model path");
    az_analysis_vocal_map_v1 vocals = ta_vocals(c, model);
    size_t size = vocals.count * sizeof *result->vocals;
    result->vocals = ta_malloc(size);
    if (!result->vocals)
        ta_fail(c, TA_LIMIT, "vocal result allocation failed");
    memcpy(result->vocals, vocals.frames, size);
    result->view.vocals = vocals;
    result->view.vocals.frames = result->vocals;
}

static void run_stage(ta_context *c, void *data, size_t stage, unsigned worker)
{
    (void)worker;
    if (stage == 0) run_phrase_stage(c, data);
    else run_vocal_stage(c, data);
    ta_cleanup(c);
}
static int concurrent_budget_exceeded(const ta_request *request, const ta_execution *execution)
{
    return execution->mode == TA_STAGES_CONCURRENT && (request->needed & AZ_DATA_PHRASES) &&
           (request->needed & AZ_ANALYSIS_VOCALS) && execution->phrase_threads + execution->vocal_threads > 3;
}

static const char *execution_error(const ta_request *request, const ta_execution *execution)
{
    if (execution->mode != TA_STAGES_SEQUENTIAL && execution->mode != TA_STAGES_CONCURRENT)
        return "invalidStageMode";
    else if (execution->phrase_threads < 1 || execution->phrase_threads > 3)
        return "invalidThreadCount";
    else if (execution->vocal_threads < 1 || execution->vocal_threads > 3)
        return "invalidVocalThreadCount";
    else if (concurrent_budget_exceeded(request, execution))
        return "concurrentThreadBudgetExceeded";
    return NULL;
}
static void report_error(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
}

static int validate_execution(const ta_request *request, const ta_execution *execution, char *error, size_t error_size)
{
    const char *invalid = execution_error(request, execution);
    if (invalid) {
        report_error(error, error_size, invalid);
        return TA_FAILED;
    }
    return TA_OK;
}
static void run_requested_stages(ta_context *c, analysis_stages *stages)
{
    const ta_request *request = c->request;
    if (c->execution.mode == TA_STAGES_CONCURRENT &&
        (request->needed & AZ_DATA_PHRASES) && (request->needed & AZ_ANALYSIS_VOCALS)) {
        ta_pair(c, run_stage, stages);
    } else {
        if (request->needed & AZ_DATA_PHRASES) run_stage(c, stages, 0, 0);
        if (request->needed & AZ_ANALYSIS_VOCALS) run_stage(c, stages, 1, 0);
    }
}
static void run_analysis(ta_context *c, ta_result *result)
{
    const ta_request *request = c->request;
    ta_check(c);
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(request->audio_path, &rate, &frames);
    if (!c->decoder || (rate != 44100 && rate != 48000) || frames < 2 ||
        frames > (uint64_t)rate * 1200)
        ta_fail(c, TA_FAILED, "unsupported source clock or duration");
    ta_decode_close(c->decoder);
    c->decoder = NULL;
    result->view = (az_analysis_result_v1){ .size = sizeof result->view,
                                            .version = AZ_DATA_VERSION,
                                            .source_rate = rate,
                                            .source_frames = frames };
    analysis_stages stages = { .result = result, .rate = rate, .frames = frames };
    run_requested_stages(c, &stages);
}
static int valid_request(const ta_request *request, ta_result **output)
{
    return !(!request || !output || !request->audio_path ||
             !(request->needed & (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)) ||
             (request->needed & ~(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)));
}
static void clear_analysis_output(ta_result **output, char *error, size_t error_size)
{
    if (output)
        *output = NULL;
    if (error && error_size)
        error[0] = 0;
}

int ta_analyze(const ta_request *request, ta_result **output, char *error, size_t error_size)
{
    clear_analysis_output(output, error, error_size);
    if (!valid_request(request, output)) {
        report_error(error, error_size, "invalid analysis request");
        return TA_FAILED;
    }
    ta_execution execution;
    if (request->execution) execution = *request->execution;
    else if (ta_execution_environment(request->needed, &execution, error, error_size)) return TA_FAILED;
    if (validate_execution(request, &execution, error, error_size)) return TA_FAILED;
    ta_context *c = ta_calloc(1, sizeof *c);
    ta_result *result = ta_calloc(1, sizeof *result);
    if (!c || !result) {
        ta_free(c);
        ta_free(result);
        return TA_LIMIT;
    }
    c->request = request;
    c->execution = execution;
    if (setjmp(c->failure)) {
        int status = c->status;
        report_error(error, error_size, c->error);
        ta_cleanup(c);
        ta_free(c);
        ta_result_free(result);
        return status;
    }
    run_analysis(c, result);
    ta_check(c);
    ta_cleanup(c);
    ta_free(c);
    *output = result;
    return TA_OK;
}

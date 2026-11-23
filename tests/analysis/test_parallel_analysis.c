/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
#include <assert.h>
#include <pthread.h>
typedef struct {
    float *features, *probabilities;
    uint32_t features_count, probability_count;
    atomic_uint profile_count, cancel_calls;
    unsigned cancel_after;
} capture;
static void features(void *user, const float (*values)[29], uint32_t count) {
    capture *c = user;
    c->features_count = count;
    c->features = malloc((size_t)count * sizeof *values);
    assert(c->features);
    memcpy(c->features, values, (size_t)count * sizeof *values);
}
static void probabilities(void *user, const float *values, uint32_t count) {
    capture *c = user;
    c->probability_count = count;
    c->probabilities = malloc((size_t)count * sizeof *values);
    assert(c->probabilities);
    memcpy(c->probabilities, values, (size_t)count * sizeof *values);
}
static void profile(void *user, const char *stage, double seconds) {
    capture *c = user;
    assert(stage && isfinite(seconds) && seconds >= 0);
    atomic_fetch_add(&c->profile_count, 1);
}
static int cancel(void *user) {
    capture *c = user;
    return atomic_fetch_add(&c->cancel_calls, 1) >= c->cancel_after;
}
static void put16(FILE *f, unsigned x) { fputc(x & 255, f); fputc((x >> 8) & 255, f); }
static void put32(FILE *f, unsigned x) { put16(f, x); put16(f, x >> 16); }
static uint64_t repeat_voice(const char *fixtures, char *path, unsigned *rate) {
    char source[1200];
    assert(snprintf(source, sizeof source, "%s/pvdi-voice.wav", fixtures) < (int)sizeof source);
    uint64_t frames;
    ta_decoder *decoder = ta_decode_open(source, rate, &frames);
    assert(decoder && frames < 1000000);
    int16_t *pcm = calloc((size_t)frames * 2, sizeof *pcm);
    assert(pcm);
    uint64_t received = 0;
    while (received < frames) {
        int64_t n = ta_decode_read(decoder, pcm + received * 2, frames - received);
        assert(n > 0);
        received += (uint64_t)n;
    }
    ta_decode_close(decoder);
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    assert(file);
    uint32_t bytes = (uint32_t)frames * 4 * 18;
    fwrite("RIFF", 1, 4, file); put32(file, 36 + bytes); fwrite("WAVEfmt ", 1, 8, file);
    put32(file, 16); put16(file, 1); put16(file, 2); put32(file, *rate); put32(file, *rate * 4);
    put16(file, 4); put16(file, 16); fwrite("data", 1, 4, file); put32(file, bytes);
    for (int repeat = 0; repeat < 18; repeat++)
        for (uint64_t i = 0; i < frames * 2; i++) put16(file, (unsigned)pcm[i]);
    assert(!fclose(file));
    free(pcm);
    return frames * 18;
}
static void same_result(const ta_result *first, const ta_result *second) {
    const az_analysis_result_v1 *a = ta_result_view(first), *b = ta_result_view(second);
    assert(a->source_rate == b->source_rate && a->source_frames == b->source_frames);
    assert(a->phrases.count == b->phrases.count && a->phrases.mood == b->phrases.mood &&
           a->phrases.end_beat == b->phrases.end_beat);
    assert(!memcmp(a->phrases.entries, b->phrases.entries, a->phrases.count * sizeof *a->phrases.entries));
    assert(a->vocals.count == b->vocals.count && a->vocals.sample_rate == b->vocals.sample_rate &&
           a->vocals.hop_samples == b->vocals.hop_samples && a->vocals.method == b->vocals.method);
    assert(!memcmp(a->vocals.frames, b->vocals.frames, a->vocals.count * sizeof *a->vocals.frames));
}
static void same_spectra(Spectra a, Spectra b) {
    assert(a.n == b.n);
    for (int i = 0; i < a.n; i++) {
        assert(a.rows[i].n == b.rows[i].n);
        assert(!memcmp(a.rows[i].v, b.rows[i].v, (size_t)a.rows[i].n * sizeof(double)));
    }
}
static void stage_arrays(const ta_request *request) {
    ta_context *c = calloc(1, sizeof *c);
    c->request = request;
    assert(!setjmp(c->failure));
    Samples baseline = phrase_decode(c);
    Timing timing = reference_timing(c, 120, 0, 1, 120);
    Coordinates coordinates = phrase_coordinates(c, baseline, timing);
    Spectra cqt = opening_spectra(c, baseline.middle, baseline.rate / 8);
    Spectra fft = phrase_spectra(c, baseline, timing, coordinates, false);
    Spectra recurrence = phrase_spectra(c, baseline, timing, coordinates, true);
    for (unsigned threads = 2; threads <= 3; threads++) {
        c->execution.phrase_threads = threads;
        Samples actual = phrase_decode(c);
        D expected_bands[] = {baseline.low, baseline.middle, baseline.high};
        D actual_bands[] = {actual.low, actual.middle, actual.high};
        for (int band = 0; band < 3; band++) {
            assert(expected_bands[band].n == actual_bands[band].n);
            assert(!memcmp(expected_bands[band].v, actual_bands[band].v,
                           (size_t)actual_bands[band].n * sizeof(double)));
            ta_release(c, actual_bands[band].v);
        }
        Spectra next = opening_spectra(c, baseline.middle, baseline.rate / 8);
        same_spectra(cqt, next); spectra_release(c, next);
        next = phrase_spectra(c, baseline, timing, coordinates, false);
        same_spectra(fft, next); spectra_release(c, next);
        next = phrase_spectra(c, baseline, timing, coordinates, true);
        same_spectra(recurrence, next); spectra_release(c, next);
    }
    ta_cleanup(c);
    assert(!c->budget || !atomic_load(&c->budget->used));
    free(c);
    puts("Every decimator sample, CQT bin and FFT/recurrence value matches across 1/2/3 threads");
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char audio[] = "/tmp/track-analysis-parallel-XXXXXX";
    unsigned rate;
    uint64_t frames = repeat_voice(argv[1], audio, &rate);
    unsigned count = (unsigned)ceil((double)frames / rate * 2) + 1;
    az_beat *beats = calloc(count, sizeof *beats);
    assert(beats);
    for (unsigned i = 0; i < count; i++)
        beats[i] = (az_beat){.time_s = i * 0.5, .bpm = 120, .beat_in_bar = i % 4 + 1};
    ta_request request = {.audio_path = audio, .assets_path = argv[2], .beats = beats,
                          .beat_count = count, .bpm = 120, .needed = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    stage_arrays(&request);
    const ta_execution modes[] = {
        {TA_STAGES_SEQUENTIAL, 1, 1},
        {TA_STAGES_SEQUENTIAL, 1, 2},
        {TA_STAGES_SEQUENTIAL, 1, 3},
        {TA_STAGES_SEQUENTIAL, 2, 1},
        {TA_STAGES_SEQUENTIAL, 2, 2},
        {TA_STAGES_SEQUENTIAL, 2, 3},
        {TA_STAGES_SEQUENTIAL, 3, 1},
        {TA_STAGES_SEQUENTIAL, 3, 2},
        {TA_STAGES_SEQUENTIAL, 3, 3},
        {TA_STAGES_CONCURRENT, 1, 1},
        {TA_STAGES_CONCURRENT, 1, 2},
        {TA_STAGES_CONCURRENT, 2, 1},
    };
    ta_result *reference = NULL;
    capture baseline = {0};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        capture current = {0};
        ta_diagnostics diagnostics = {profile, features, probabilities, &current};
        request.execution = &modes[i]; request.diagnostics = &diagnostics;
        ta_result *result = NULL;
        char error[160];
        int status = ta_analyze(&request, &result, error, sizeof error);
        if (status) fprintf(stderr, "mode %zu: %s\n", i, error);
        assert(status == TA_OK && current.probability_count > 2160 && atomic_load(&current.profile_count) >= 10);
        if (!reference) {
            reference = result;
            baseline.features = current.features; baseline.features_count = current.features_count;
            baseline.probabilities = current.probabilities; baseline.probability_count = current.probability_count;
        } else {
            same_result(reference, result);
            assert(current.features_count == baseline.features_count && current.probability_count == baseline.probability_count);
            assert(!memcmp(current.features, baseline.features, (size_t)current.features_count * 29 * sizeof(float)));
            assert(!memcmp(current.probabilities, baseline.probabilities, (size_t)current.probability_count * sizeof(float)));
            ta_result_free(result); free(current.features); free(current.probabilities);
        }
        printf("Mode %zu: complete typed results, feature bits and probability bits match\n", i);
        fflush(stdout);
    }
    request.diagnostics = NULL;
    for (unsigned i = 0; i < 3; i++) {
        capture cancelled = {.cancel_after = i == 0 ? 80 : i == 1 ? 800 : 8000};
        request.user = &cancelled; request.cancelled = cancel;
        ta_result *result = NULL; char error[160];
        assert(ta_analyze(&request, &result, error, sizeof error) == TA_CANCELLED && !result);
    }
    request.cancelled = NULL;
    const char *assets = request.assets_path;
    request.assets_path = "/nonexistent-model-directory";
    ta_result *result = NULL; char error[160];
    assert(ta_analyze(&request, &result, error, sizeof error) == TA_FAILED && !result);
    request.assets_path = assets;
    request.beat_count = 1; /* Force a phrase-stage failure after the sibling starts. */
    assert(ta_analyze(&request, &result, error, sizeof error) == TA_FAILED && !result);
    ta_result_free(reference); free(baseline.features); free(baseline.probabilities);
    free(beats); assert(!unlink(audio));
    puts("Concurrent stage cancellation and either-stage failure return no partial result");
}

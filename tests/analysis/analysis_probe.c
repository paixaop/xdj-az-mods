/* SPDX-License-Identifier: GPL-3.0-only */
/* Host/target test driver only; never packaged or launched by the mod.
 * Uses a constant 120 BPM typed grid for comparison with the retained worker. */
#include "internal.h"
#include <sys/resource.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: analysis_probe AUDIO ASSETS NEEDED RESULT.json\n"); return 2; }
    unsigned rate; uint64_t frames;
    ta_decoder *decoder = ta_decode_open(argv[1], &rate, &frames);
    if (!decoder) return 3;
    ta_decode_close(decoder);
    uint32_t count = (uint32_t)ceil((double)frames / rate * 2) + 1;
    az_beat *beats = calloc(count, sizeof *beats);
    if (!beats) return 4;
    for (uint32_t i = 0; i < count; i++)
        beats[i] = (az_beat){.time_s = (double)i * 0.5, .bpm = 120, .beat_in_bar = i % 4 + 1};
    ta_request request = {.audio_path = argv[1], .assets_path = argv[2], .beats = beats,
                          .beat_count = count, .bpm = 120, .needed = (uint32_t)strtoul(argv[3], NULL, 10)};
    ta_result *result = NULL; char error[160];
    double started = now();
    int status = ta_analyze(&request, &result, error, sizeof error);
    double elapsed = now() - started;
    free(beats);
    if (status) { fprintf(stderr, "%s\n", error); return 5; }
    const az_analysis_result_v1 *view = ta_result_view(result);
    FILE *file = fopen(argv[4], "w");
    if (!file) { ta_result_free(result); return 6; }
    fprintf(file, "{\"source_rate\":%u,\"source_frames\":%llu,\"mood\":%u,\"end_beat\":%u,\"phrases\":[",
            view->source_rate, (unsigned long long)view->source_frames, view->phrases.mood, view->phrases.end_beat);
    for (uint32_t i = 0; i < view->phrases.count; i++) {
        const az_analysis_phrase_v1 *p = view->phrases.entries + i;
        fprintf(file, "%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", i ? "," : "", p->start_beat,
                p->kind, p->variant1, p->variant2, p->variant3, p->structure, p->beat2, p->beat3,
                p->beat4, p->fill, p->fill_start_beat);
    }
    fputs("],\"vocals\":[", file);
    for (uint32_t i = 0; i < view->vocals.count; i++)
        fprintf(file, "%s%u", i ? "," : "", view->vocals.frames[i].activity);
    fputs("]}\n", file); int failed = fclose(file);
    ta_result_free(result);
    struct rusage usage; getrusage(RUSAGE_SELF, &usage);
    long rss = usage.ru_maxrss;
#ifdef __APPLE__
    rss /= 1024;
#endif
    fprintf(stderr, "elapsed_s=%.6f maxrss_kb=%ld\n", elapsed, rss);
    return failed ? 7 : 0;
}

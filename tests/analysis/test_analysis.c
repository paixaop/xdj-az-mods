/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/aztimestamp.h"
#include "internal.h"
#include <assert.h>
#include <sys/stat.h>
static void path(char out[1200], const char *root, const char *file)
{
    assert(snprintf(out, 1200, "%s/%s", root, file) < 1200);
}
static void decode_checks(const char *fixtures)
{
    static const struct {
        const char *file;
        unsigned rate;
        uint64_t frames, edit, presentation;
    } cases[] = {
        { "AAC/lc-44100-2.m4a", 44100, 45 * 1024, 1024, 44100 },
        { "AAC/lc-44100-2.aac", 44100, 45 * 1024, 0, 0 },
        { "AAC/lc-48000-1.m4a", 48000, 48 * 1024, 1024, 48000 },
        { "AAC/lc-48000-1.aac", 48000, 48 * 1024, 0, 0 },
        { "AAC/apple-lc-44100-2.m4a", 44100, 47 * 1024, 2112, 45056 },
        { "AAC/apple-lc-44100-2.aac", 44100, 47 * 1024, 0, 0 },
        { "NativePhraseMP3/cbr-info-44100-2.mp3", 44100, 89856, 0, 0 },
        { "NativePhraseMP3/cbr-info-48000-1.mp3", 48000, 99072, 0, 0 },
        { "NativePhraseMP3/no-info-44100-2.mp3", 44100, 89856, 0, 0 },
        { "NativePhraseMP3/no-info-48000-1.mp3", 48000, 97920, 0, 0 },
        { "NativePhraseMP3/vbr-xing-44100-2.mp3", 44100, 89856, 0, 0 },
        { "NativePhraseMP3/vbr-xing-48000-1.mp3", 48000, 99072, 0, 0 },
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        char file[1200];
        path(file, fixtures, cases[k].file);
        unsigned rate = 0;
        uint64_t frames = 0;
        ta_decoder *d = ta_decode_open(file, &rate, &frames);
        assert(d && rate == cases[k].rate && frames == cases[k].frames);
        int16_t *pcm = calloc(frames * 2, sizeof *pcm);
        assert(pcm);
        uint64_t at = 0;
        while (at < frames) {
            int64_t n = ta_decode_read(d, pcm + at * 2, ta_min(4093, (int)(frames - at)));
            assert(n > 0);
            at += (uint64_t)n;
        }
        int16_t block[2048];
        assert(ta_decode_read(d, block, 1) == 0);
        assert(ta_decode_seek(d, 1001) == 0 && ta_decode_read(d, block, 1024) == 1024);
        assert(!memcmp(block, pcm + 2002, sizeof block));
        assert(ta_decode_seek(d, 0) == 0 && ta_decode_read(d, block, 1024) == 1024);
        assert(!memcmp(block, pcm, sizeof block));
        if (k < 6) {
            ta_decode_clock clock = { 0 };
            ta_decode_get_clock(d, &clock);
            assert(clock.kind == TA_DECODE_AAC_ENCODED);
            assert(compare_timestamps(clock.decoder_leading_frames, UINT64_C(1024), rate, rate,
                                      1) == AZ_TIME_EQUAL);
            assert(clock.has_edit == (cases[k].edit != 0));
            if (clock.has_edit) {
                assert(compare_timestamps(clock.edit_start_frames, cases[k].edit, rate, rate, 1) ==
                       AZ_TIME_EQUAL);
                assert(clock.presentation_frames == cases[k].presentation);
            }
            for (int i = 0; i < 2048; i++)
                assert(pcm[i] == 0);
        }
        ta_decode_close(d);
        free(pcm);
    }
    const char *bad[] = { "AAC/unsupported-he-aac.m4a", "NativePhraseMP3/mixed-format.mp3" };
    for (unsigned i = 0; i < 2; i++) {
        char file[1200];
        path(file, fixtures, bad[i]);
        unsigned rate;
        uint64_t frames;
        assert(!ta_decode_open(file, &rate, &frames));
    }
    puts("AAC/MP3 clocks, priming, decode, seek replay and format refusal passed");
}
struct cancel_state {
    unsigned calls, after;
};
static int cancel(void *user)
{
    struct cancel_state *s = user;
    return ++s->calls > s->after;
}
static void vocals_check(const char *fixtures, const char *assets)
{
    char audio[1200], golden[1200];
    path(audio, fixtures, "pvdi-voice.wav");
    path(golden, fixtures, "pvdi-voice.PVDI");
    ta_request r = { .audio_path = audio, .assets_path = assets, .needed = AZ_ANALYSIS_VOCALS };
    ta_result *result = NULL;
    char error[160];
    int status = ta_analyze(&r, &result, error, sizeof error);
    if (status)
        fprintf(stderr, "vocals: %s\n", error);
    assert(status == TA_OK && result);
    const az_analysis_result_v1 *v = ta_result_view(result);
    assert(v->vocals.sample_rate == 22050 && v->vocals.hop_samples == 1024 &&
           v->vocals.method == AZ_VOCAL_LSTM);
    FILE *f = fopen(golden, "rb");
    assert(f);
    unsigned char header[24];
    assert(fread(header, 1, 24, f) == 24 && !memcmp(header, "PVDI", 4));
    assert(ta_be32(header + 20) == v->vocals.count);
    for (unsigned i = 0; i < v->vocals.count; i++)
        assert((unsigned)fgetc(f) == v->vocals.frames[i].activity);
    assert(fgetc(f) == EOF);
    fclose(f);
    ta_result_free(result);
    for (unsigned after = 0; after < 400; after += 79) {
        struct cancel_state state = { .after = after };
        r.cancelled = cancel;
        r.user = &state;
        result = NULL;
        assert(ta_analyze(&r, &result, error, sizeof error) == TA_CANCELLED && !result);
    }
    puts("Typed vocal frames match the frozen PVDI result; cancellation releases partial work");
}
static void put16(FILE *f, unsigned x)
{
    fputc(x & 255, f);
    fputc((x >> 8) & 255, f);
}
static void put32(FILE *f, unsigned x)
{
    put16(f, x & 65535);
    put16(f, x >> 16);
}
static void phrase_check(unsigned rate, const char *assets)
{
    char file[] = "/tmp/track-analysis-pcm-XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    assert(f);
    unsigned frames = rate * 64;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
    const double levels[] = { 0.1, 0.9, 0.3, 1 };
    for (unsigned sec = 0; sec < 64; sec++)
        for (unsigned i = 0; i < rate; i++) {
            double t = (double)i / rate, pulse = exp(-fmod(t, 0.5) * 35), level = levels[sec / 16];
            int sample = (int)round(level * (sin(t * 2 * M_PI * 60) * pulse * 18000 +
                                             sin(t * 2 * M_PI * (220 + sec % 4 * 55)) * 2000));
            put16(f, (unsigned)sample);
            put16(f, (unsigned)sample);
        }
    assert(!fclose(f));
    az_beat beats[256];
    for (int i = 0; i < 256; i++)
        beats[i] = (az_beat){ .time_s = i * 0.5, .bpm = 120, .beat_in_bar = i % 4 + 1 };
    ta_request r = { .audio_path = file,
                     .assets_path = assets,
                     .beats = beats,
                     .beat_count = 256,
                     .needed = AZ_DATA_PHRASES,
                     .bpm = 120 };
    ta_result *result = NULL;
    char error[160];
    int status = ta_analyze(&r, &result, error, sizeof error);
    if (status)
        fprintf(stderr, "phrases: %s\n", error);
    assert(status == TA_OK);
    const az_analysis_result_v1 *v = ta_result_view(result);
    assert(v->source_frames == frames && v->source_rate == rate && v->phrases.count == 4 &&
           v->phrases.mood == 2 && v->phrases.end_beat == 128);
    const unsigned starts[] = { 1, 37, 65, 97 };
    for (unsigned i = 0; i < 4; i++) {
        const az_analysis_phrase_v1 *p = v->phrases.entries + i;
        assert(p->start_beat == starts[i] && p->kind == (i ? 9u : 1u));
        assert(!p->variant1 && !p->variant2 && !p->variant3 && !p->structure && !p->beat2 &&
               !p->beat3 && !p->beat4 && !p->fill && !p->fill_start_beat);
    }
    ta_result_free(result);
    struct cancel_state state = { .after = 400 };
    r.cancelled = cancel;
    r.user = &state;
    result = NULL;
    assert(ta_analyze(&r, &result, error, sizeof error) == TA_CANCELLED && !result);
    r.cancelled = NULL;
    beats[1].time_s = beats[0].time_s;
    assert(ta_analyze(&r, &result, error, sizeof error) == TA_FAILED && !result);
    assert(!unlink(file));
    printf("%u Hz complete C phrase analysis matches the retained reference\n", rate);
}
static void execution_contracts(void)
{
    const ta_execution invalid[] = {
        { .mode = 99, .phrase_threads = 1, .vocal_threads = 1 },
        { .mode = TA_STAGES_SEQUENTIAL, .phrase_threads = 0, .vocal_threads = 1 },
        { .mode = TA_STAGES_SEQUENTIAL, .phrase_threads = 1, .vocal_threads = 0 },
        { .mode = TA_STAGES_CONCURRENT, .phrase_threads = 2, .vocal_threads = 2 }
    };
    const char *messages[] = { "invalidStageMode", "invalidThreadCount", "invalidVocalThreadCount", "concurrentThreadBudgetExceeded" };
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        ta_request request = { .audio_path = "unused.wav", .needed = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, .execution = &invalid[i] };
        ta_result *result = NULL;
        char error[160] = { 0 };
        assert(ta_analyze(&request, &result, error, sizeof error) == TA_FAILED);
        assert(!result && !strcmp(error, messages[i]));
    }
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    execution_contracts();
    decode_checks(argv[1]);
    vocals_check(argv[1], argv[2]);
    phrase_check(44100, argv[2]);
    phrase_check(48000, argv[2]);
    ta_context *c = calloc(1, sizeof *c);
    assert(c);
    if (!setjmp(c->failure)) {
        ta_alloc(c, TA_MEMORY_LIMIT, 2);
        assert(!"memory limit missed");
    }
    assert(c->status == TA_LIMIT);
    ta_cleanup(c);
    assert(!c->memory && !c->allocations && !c->decoder);
    free(c);
    puts("Analysis memory limit and cleanup passed");
    return 0;
}

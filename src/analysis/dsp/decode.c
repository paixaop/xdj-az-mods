/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../common/azanalysis_policy.h"
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_THREADING
#define MA_NO_RUNTIME_LINKING
#include "allocator.h"
#define MA_MALLOC(size) ta_malloc(size)
#define MA_REALLOC(pointer, size) ta_realloc(pointer, size)
#define MA_FREE(pointer) ta_free(pointer)
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "PortableDSP.h"
#include "aac.h"
struct ta_decoder {
    ma_decoder decoder;
    ta_aac *aac;
    uint64_t prefix, position;
};
struct ta_mpeg_frame {
    unsigned rate, mode, bytes;
};
static int ta_mpeg_header(const unsigned char h[4], struct ta_mpeg_frame *f)
{
    static const unsigned rates[] = { 44100, 48000, 32000 };
    static const unsigned kbps[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    unsigned rate = (h[2] >> 2) & 3, bit = h[2] >> 4;
    if (h[0] != 255 || (h[1] & 0xfe) != 0xfa || rate > 2 || !kbps[bit]) return 0;
    f->rate = rates[rate];
    f->mode = h[3] >> 6;
    f->bytes = 144000 * kbps[bit] / f->rate + ((h[2] >> 1) & 1);
    return 1;
}
/* The original reader uses the encoded frame clock, with gapless trimming off.
 * Its metadata frame contributes silence only when three physical frame headers
 * agree. Validate the complete stream before enabling this decoding contract. */
struct ta_mpeg_scan {
    uint64_t at, count, size;
    struct ta_mpeg_frame first, normal;
    int metadata, compatible;
};
static int ta_mpeg_scan_init(FILE *file, struct ta_mpeg_scan *scan)
{
    unsigned char head[10];
    if (fread(head, 1, 10, file) != 10) return 0;
    if (!memcmp(head, "ID3", 3)) {
        if ((head[6] | head[7] | head[8] | head[9]) & 0x80) return 0;
        scan->at = 10 + ((uint64_t)head[6] << 21 | (uint64_t)head[7] << 14 | (unsigned)head[8] << 7 | head[9]);
        if (head[5] & 0x10) scan->at += 10;
    }
    if (fseek(file, 0, SEEK_END)) return 0;
    long size = ftell(file);
    if (size < 0 || (uint64_t)size > UINT64_C(1024) * 1024 * 1024) return 0;
    scan->size = (uint64_t)size;
    return 1;
}
static int ta_mpeg_metadata(FILE *file, const unsigned char h[4], struct ta_mpeg_frame f, uint64_t at)
{
    unsigned offset = 4 + ((h[1] & 1) ? 0 : 2) + (f.mode == 3 ? 17 : 32);
    unsigned char marker[4];
    if (offset + 4 <= f.bytes && !fseek(file, (long)(at + offset), SEEK_SET) && fread(marker, 1, 4, file) == 4)
        return !memcmp(marker, "Info", 4) || !memcmp(marker, "Xing", 4);
    return 0;
}
static int ta_mpeg_same_format(struct ta_mpeg_frame a, struct ta_mpeg_frame b)
{
    return a.mode == b.mode && a.rate == b.rate;
}
static int ta_mpeg_valid_frame(const unsigned char h[4], struct ta_mpeg_frame *f, uint64_t remaining)
{
    return ta_mpeg_header(h, f) && f->bytes <= remaining && az_analysis_source_rate_supported(f->rate);
}
static int ta_mpeg_scan_frame(FILE *file, struct ta_mpeg_scan *scan, const unsigned char h[4])
{
    struct ta_mpeg_frame f;
    if (!ta_mpeg_valid_frame(h, &f, scan->size - scan->at)) return 0;
    if (!scan->count) {
        scan->first = f;
        scan->metadata = ta_mpeg_metadata(file, h, f, scan->at);
    }
    if (scan->metadata && scan->count < 3 && !ta_mpeg_same_format(f, scan->first)) scan->compatible = 0;
    if (scan->count == (unsigned)scan->metadata) scan->normal = f;
    if (scan->count >= (unsigned)scan->metadata && !ta_mpeg_same_format(f, scan->normal)) return 0;
    scan->count++;
    scan->at += f.bytes;
    return scan->count <= (uint64_t)f.rate * AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS / 1152 + 1;
}
static int ta_mpeg_scan_stream(FILE *file, struct ta_mpeg_scan *scan)
{
    while (scan->at < scan->size) {
        if (fseek(file, (long)scan->at, SEEK_SET)) return 0;
        unsigned char h[4];
        if (fread(h, 1, 4, file) != 4) return 0;
        if (scan->size - scan->at == 128 && !memcmp(h, "TAG", 3)) break;
        if (!ta_mpeg_scan_frame(file, scan, h)) return 0;
    }
    return scan->count > (unsigned)scan->metadata;
}
static int ta_mpeg_timeline(const char *path, uint64_t *frames, uint64_t *prefix)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    struct ta_mpeg_scan scan = { .compatible = 1 };
    int ok = ta_mpeg_scan_init(file, &scan) && ta_mpeg_scan_stream(file, &scan);
    fclose(file);
    if (!ok) return 0;
    *prefix = scan.metadata && scan.compatible && scan.count >= 3 ? 1152 : 0;
    *frames = (scan.count - (unsigned)scan.metadata) * 1152;
    return 1;
}
static int ta_decode_mpeg_clock(ta_decoder *d, const char *path)
{
    uint64_t raw = 0;
    if (!ta_mpeg_timeline(path, &raw, &d->prefix)) return 0;
    ma_mp3 *backend = (ma_mp3 *)d->decoder.pBackend;
    backend->dr.delayInPCMFrames = backend->dr.paddingInPCMFrames = 0;
    backend->dr.totalPCMFrameCount = raw;
    /* The backend owns the old gapless seek table; disable its borrowed lookup. */
    backend->dr.pSeekPoints = NULL;
    backend->dr.seekPointCount = 0;
    return ma_decoder_seek_to_pcm_frame(&d->decoder, 0) == MA_SUCCESS;
}
static int ta_decode_valid_length(ta_decoder *d, ma_uint64 *length)
{
    return ma_decoder_get_length_in_pcm_frames(&d->decoder, length) == MA_SUCCESS && *length &&
           *length <= (ma_uint64)d->decoder.outputSampleRate * AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS &&
           d->prefix <= (ma_uint64)d->decoder.outputSampleRate * AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS - *length;
}
static int ta_decode_miniaudio(ta_decoder *d, const char *path, ma_decoder_config config, unsigned *rate, uint64_t *frames)
{
    if (d->decoder.pBackendVTable == &g_ma_decoding_backend_vtable_mp3 && !ta_decode_mpeg_clock(d, path)) {
        ma_decoder_uninit(&d->decoder);
        return 0;
    }
    if (!az_analysis_source_rate_supported(d->decoder.outputSampleRate)) {
        ma_decoder_uninit(&d->decoder);
        config.sampleRate = AZ_ANALYSIS_POLICY_ALLOWED_RATE_LOW;
        if (ma_decoder_init_file(path, &config, &d->decoder) != MA_SUCCESS) return 0;
    }
    ma_uint64 length = 0;
    if (!ta_decode_valid_length(d, &length)) {
        ma_decoder_uninit(&d->decoder);
        return 0;
    }
    *rate = d->decoder.outputSampleRate;
    *frames = length + d->prefix;
    return 1;
}
ta_decoder *ta_decode_open(const char *path, unsigned *rate, uint64_t *frames)
{
    ta_decoder *d = ta_calloc(1, sizeof *d);
    if (!d) return NULL;
    /* MP3 probes scan for sync words inside arbitrary bytes. Recognize AAC
     * containers first, before compressed AAC payload can resemble MP3. */
    if (ta_aac_candidate(path)) {
        d->aac = ta_aac_open(path, rate, frames);
        if (d->aac) return d;
        ta_free(d);
        return NULL;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 2, 0);
    if (ma_decoder_init_file(path, &config, &d->decoder) != MA_SUCCESS) {
        d->aac = ta_aac_open(path, rate, frames);
        if (d->aac) return d;
        ta_free(d);
        return NULL;
    }
    if (!ta_decode_miniaudio(d, path, config, rate, frames)) {
        ta_free(d);
        return NULL;
    }
    return d;
}
int64_t ta_decode_read(ta_decoder *d, int16_t *stereo, uint64_t frames)
{
    if (d->aac) return ta_aac_read(d->aac, stereo, frames);
    uint64_t leading = d->position < d->prefix ? d->prefix - d->position : 0;
    if (leading > frames) leading = frames;
    memset(stereo, 0, (size_t)leading * 2 * sizeof *stereo);
    if (frames <= leading) {
        d->position += leading;
        return (int64_t)leading;
    }
    ma_uint64 read = 0;
    ma_result rc = ma_decoder_read_pcm_frames(&d->decoder, stereo + leading * 2, frames - leading, &read);
    d->position += leading + read;
    return rc == MA_SUCCESS || rc == MA_AT_END ? (int64_t)(read + leading) : -1;
}
int ta_decode_seek(ta_decoder *d, uint64_t frame)
{
    if (d->aac) return ta_aac_seek(d->aac, frame);
    uint64_t decoded = frame > d->prefix ? frame - d->prefix : 0;
    if (ma_decoder_seek_to_pcm_frame(&d->decoder, decoded) != MA_SUCCESS) return -1;
    d->position = frame;
    return 0;
}
void ta_decode_get_clock(ta_decoder *d, ta_decode_clock *clock)
{
    memset(clock, 0, sizeof *clock);
    if (d->aac) ta_aac_timeline(d->aac, clock);
}
void ta_decode_close(ta_decoder *d)
{
    if (d) {
        if (d->aac) ta_aac_close(d->aac);
        else ma_decoder_uninit(&d->decoder);
        ta_free(d);
    }
}

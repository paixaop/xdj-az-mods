#include "allocator.h"
/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stddef.h>
#include <neaacdec.h>
#include "aac.h"
#include "../common/aztimestamp.h"

/* Each MP4 parser has a separate 16 MiB allocation budget. It cannot consume
 * the mod's address space with hostile sample-table counts. The thread-local
 * context is only used during open; allocation headers retain their owner. */
union ta_allocation;
struct ta_budget {
    size_t used;
    union ta_allocation *head;
};
union ta_allocation {
    struct {
        struct ta_budget *owner;
        size_t size;
        union ta_allocation *previous, *next;
    } info;
    max_align_t alignment;
};
static _Thread_local struct ta_budget *ta_current_budget;
static void *ta_mp4_malloc(size_t size)
{
    struct ta_budget *owner = ta_current_budget;
    if (!owner || !size || size > 16 * 1024 * 1024 - owner->used) return NULL;
    union ta_allocation *p = ta_malloc(sizeof *p + size);
    if (!p) return NULL;
    p->info.owner = owner;
    p->info.size = size;
    owner->used += size;
    p->info.previous = NULL;
    p->info.next = owner->head;
    if (owner->head) owner->head->info.previous = p;
    owner->head = p;
    return p + 1;
}
static void ta_mp4_free(void *memory)
{
    if (!memory) return;
    union ta_allocation *p = (union ta_allocation *)memory - 1;
    p->info.owner->used -= p->info.size;
    if (p->info.previous) p->info.previous->info.next = p->info.next;
    else p->info.owner->head = p->info.next;
    if (p->info.next) p->info.next->info.previous = p->info.previous;
    ta_free(p);
}
static void *ta_mp4_realloc(void *memory, size_t size)
{
    if (!memory) return ta_mp4_malloc(size);
    if (!size) {
        ta_mp4_free(memory);
        return NULL;
    }
    union ta_allocation *p = (union ta_allocation *)memory - 1;
    struct ta_budget *owner = p->info.owner;
    size_t previous = p->info.size;
    if (size > 16 * 1024 * 1024 - (owner->used - previous)) return NULL;
    p = ta_realloc(p, sizeof *p + size);
    if (!p) return NULL;
    if (p->info.previous) p->info.previous->info.next = p;
    else owner->head = p;
    if (p->info.next) p->info.next->info.previous = p;
    p->info.size = size;
    owner->used += size;
    owner->used -= previous;
    return p + 1;
}
static void ta_mp4_release(struct ta_budget *budget)
{
    /* Upstream assigns some realloc results directly. Retain and release the
     * old allocation even if a failed realloc lost its table pointer. */
    while (budget->head) ta_mp4_free(budget->head + 1);
}
#define malloc ta_mp4_malloc
#define realloc ta_mp4_realloc
#define free ta_mp4_free
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#undef malloc
#undef realloc
#undef free

enum { TA_AAC_PACKET_MAX = 8192,
       TA_AAC_PACKETS_MAX = 56250 };
struct ta_aac_packet {
    uint64_t offset;
    unsigned size;
};
struct ta_aac {
    FILE *file;
    uint64_t size, position;
    unsigned rate, channels, packet_count, next_packet, buffered, consumed, parse_reads;
    unsigned char asc[64];
    unsigned asc_size;
    struct ta_aac_packet *packets;
    NeAACDecHandle codec;
    int16_t pcm[2048];
    ta_decode_clock clock;
};
static uint32_t ta_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}
static uint64_t ta_be64(const unsigned char *p) { return (uint64_t)ta_be32(p) << 32 | ta_be32(p + 4); }
static int ta_aac_bytes(int64_t offset, void *buffer, size_t size, void *token)
{
    ta_aac *a = token;
    if (ta_current_budget && ++a->parse_reads > 4 * 1024 * 1024) return 1;
    if (offset < 0 || (uint64_t)offset > a->size || size > a->size - (uint64_t)offset ||
        fseek(a->file, (long)offset, SEEK_SET)) return 1;
    return fread(buffer, 1, size, a->file) == size ? 0 : 1;
}
struct ta_box {
    uint64_t data, end;
    uint32_t type;
};
static int ta_aac_box_size(ta_aac *a, uint64_t offset, uint64_t end, unsigned char h[16], uint64_t *size, uint64_t *header)
{
    *size = ta_be32(h);
    *header = 8;
    if (*size == 1) {
        if (end - offset < 16 || ta_aac_bytes(offset + 8, h + 8, 8, a)) return 0;
        *size = ta_be64(h + 8);
        *header = 16;
    } else if (!*size) *size = end - offset;
    return *size >= *header && *size <= end - offset;
}
static int ta_aac_box(ta_aac *a, uint64_t offset, uint64_t end, struct ta_box *box)
{
    unsigned char h[16];
    if (offset > end || end - offset < 8 || ta_aac_bytes(offset, h, 8, a)) return 0;
    uint64_t size, header;
    if (!ta_aac_box_size(a, offset, end, h, &size, &header)) return 0;
    box->type = ta_be32(h + 4);
    box->data = offset + header;
    box->end = offset + size;
    return 1;
}
#define TA_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (unsigned)(c) << 8 | (d))
/* Upstream tolerates truncated table counts. Reject them before demuxing and
 * cap expanded stts rows, so malformed files cannot spend unbounded CPU even
 * when their table allocation would fit the memory budget. */
static int ta_aac_container(uint32_t type)
{
    switch (type) {
    case TA_FOURCC('m', 'o', 'o', 'v'):
    case TA_FOURCC('t', 'r', 'a', 'k'):
    case TA_FOURCC('m', 'd', 'i', 'a'):
    case TA_FOURCC('m', 'i', 'n', 'f'):
    case TA_FOURCC('s', 't', 'b', 'l'):
    case TA_FOURCC('e', 'd', 't', 's'): return 1;
    default: return 0;
    }
}
static unsigned ta_aac_table_width(uint32_t type)
{
    switch (type) {
    case TA_FOURCC('s', 't', 's', 'c'): return 12;
    case TA_FOURCC('s', 't', 'c', 'o'): return 4;
    case TA_FOURCC('s', 't', 't', 's'):
    case TA_FOURCC('c', 'o', '6', '4'): return 8;
    default: return 0;
    }
}
static int ta_aac_sample_sizes(ta_aac *a, struct ta_box b)
{
    unsigned char h[12];
    if (b.end - b.data < 12 || ta_aac_bytes(b.data, h, 12, a)) return 0;
    uint32_t size = ta_be32(h + 4), count = ta_be32(h + 8);
    return count && count <= TA_AAC_PACKETS_MAX && size <= TA_AAC_PACKET_MAX &&
           b.end - b.data == 12 + (size ? 0 : (uint64_t)count * 4);
}
static int ta_aac_expanded_timestamps(ta_aac *a, struct ta_box b, unsigned count)
{
    uint64_t total = 0;
    unsigned char h[8];
    for (unsigned i = 0; i < count; i++) {
        if (ta_aac_bytes(b.data + 8 + (uint64_t)i * 8, h, 8, a)) return 0;
        total += ta_be32(h);
        if (total > TA_AAC_PACKETS_MAX) return 0;
    }
    return 1;
}
static int ta_aac_table(ta_aac *a, struct ta_box b, unsigned width)
{
    unsigned char h[8];
    if (b.end - b.data < 8 || ta_aac_bytes(b.data, h, 8, a)) return 0;
    unsigned count = ta_be32(h + 4);
    if (!count || count > TA_AAC_PACKETS_MAX || b.end - b.data != 8 + (uint64_t)count * width) return 0;
    return b.type != TA_FOURCC('s', 't', 't', 's') || ta_aac_expanded_timestamps(a, b, count);
}
static int ta_aac_unsupported_box(uint32_t type)
{
    return type == TA_FOURCC('m', 'o', 'o', 'f') || type == TA_FOURCC('m', 'v', 'e', 'x') || type == TA_FOURCC('s', 't', 'z', '2');
}
static int ta_aac_box_counted(ta_aac *a, uint64_t at, uint64_t end, struct ta_box *b, unsigned *boxes)
{
    return ++*boxes <= 4096 && ta_aac_box(a, at, end, b);
}
static int ta_aac_leaf_box(ta_aac *a, struct ta_box b)
{
    if (b.type == TA_FOURCC('s', 't', 's', 'z')) return ta_aac_sample_sizes(a, b);
    unsigned width = ta_aac_table_width(b.type);
    return !width || ta_aac_table(a, b, width);
}
static int ta_aac_mp4_boxes(ta_aac *a, uint64_t start, uint64_t end, unsigned depth, unsigned *boxes)
{
    if (depth > 8) return 0;
    for (uint64_t at = start; at < end;) {
        struct ta_box b;
        if (!ta_aac_box_counted(a, at, end, &b, boxes) || ta_aac_unsupported_box(b.type)) return 0;
        if (ta_aac_container(b.type)) {
            if (!ta_aac_mp4_boxes(a, b.data, b.end, depth + 1, boxes)) return 0;
        } else if (!ta_aac_leaf_box(a, b)) return 0;
        at = b.end;
    }
    return 1;
}
/* A single forward media edit is descriptive metadata, never an implicit
 * decoder seek. Empty edits, speed changes and repeated edits need a timeline
 * composition layer and are rejected instead of producing misplaced cues. */
static int ta_aac_edit_header(const unsigned char h[28])
{
    return h[0] <= 1 && !h[1] && !h[2] && !h[3] && ta_be32(h + 4) == 1;
}
static int ta_aac_apply_edit(ta_aac *a, const unsigned char h[28], unsigned width, unsigned movie_scale)
{
    uint64_t duration = width == 8 ? ta_be64(h + 8) : ta_be32(h + 8);
    uint64_t media = width == 8 ? ta_be64(h + 16) : ta_be32(h + 12);
    if (media >> (width * 8 - 1) || !movie_scale || ta_be32(h + 8 + width * 2) != 0x00010000 ||
        media > (uint64_t)a->packet_count * 1024 || duration > (uint64_t)movie_scale * 1200) return 0;
    a->clock.has_edit = 1;
    a->clock.edit_start_frames = media;
    a->clock.presentation_frames = (duration * a->rate + movie_scale / 2) / movie_scale;
    return a->clock.presentation_frames <= (uint64_t)a->packet_count * 1024 - media;
}
static int ta_aac_edit(ta_aac *a, struct ta_box b, unsigned movie_scale)
{
    unsigned char h[28];
    uint64_t length = b.end - b.data;
    if (a->clock.has_edit || length < 20 || ta_aac_bytes(b.data, h, 20, a)) return 0;
    if (!ta_aac_edit_header(h)) return 0;
    unsigned width = h[0] ? 8 : 4;
    if (length != 12 + width * 2 || ta_aac_bytes(b.data, h, (size_t)length, a)) return 0;
    return ta_aac_apply_edit(a, h, width, movie_scale);
}
static int ta_aac_edits(ta_aac *a, uint64_t start, uint64_t end, unsigned movie_scale)
{
    unsigned boxes = 0;
    for (uint64_t at = start; at < end;) {
        struct ta_box b;
        if (!ta_aac_box_counted(a, at, end, &b, &boxes)) return 0;
        if (b.type == TA_FOURCC('e', 'l', 's', 't') && !ta_aac_edit(a, b, movie_scale)) return 0;
        at = b.end;
    }
    return 1;
}
static int ta_aac_track_edits(ta_aac *a, struct ta_box t, unsigned movie_scale, unsigned *boxes)
{
    for (uint64_t inner = t.data; inner < t.end;) {
        struct ta_box e;
        if (!ta_aac_box_counted(a, inner, t.end, &e, boxes)) return 0;
        if (e.type == TA_FOURCC('e', 'd', 't', 's') && !ta_aac_edits(a, e.data, e.end, movie_scale)) return 0;
        inner = e.end;
    }
    return 1;
}
static int ta_aac_movie_edits(ta_aac *a, struct ta_box b, unsigned selected, unsigned movie_scale, unsigned *tracks, unsigned *boxes)
{
    for (uint64_t child = b.data; child < b.end;) {
        struct ta_box t;
        if (!ta_aac_box_counted(a, child, b.end, &t, boxes)) return 0;
        if (t.type == TA_FOURCC('m', 'v', 'e', 'x')) return 0;
        if (t.type == TA_FOURCC('t', 'r', 'a', 'k') && (*tracks)++ == selected)
            if (!ta_aac_track_edits(a, t, movie_scale, boxes)) return 0;
        child = t.end;
    }
    return 1;
}
static int ta_aac_mp4_edits(ta_aac *a, unsigned selected, unsigned movie_scale)
{
    unsigned boxes = 0, tracks = 0;
    for (uint64_t at = 0; at < a->size;) {
        struct ta_box b;
        if (!ta_aac_box_counted(a, at, a->size, &b, &boxes)) return 0;
        if (b.type == TA_FOURCC('m', 'o', 'o', 'f')) return 0;
        if (b.type == TA_FOURCC('m', 'o', 'o', 'v') && !ta_aac_movie_edits(a, b, selected, movie_scale, &tracks, &boxes)) return 0;
        at = b.end;
    }
    return tracks > selected;
}
static unsigned ta_aac_audio_track(const MP4D_demux_t *mp4)
{
    if (mp4->track_count > 8) return UINT_MAX;
    unsigned selected = UINT_MAX;
    for (unsigned i = 0; i < mp4->track_count; i++)
        if (mp4->track[i].handler_type == MP4D_HANDLER_TYPE_SOUN) {
            if (selected != UINT_MAX) return UINT_MAX;
            selected = i;
        }
    return selected;
}
static int ta_aac_track_samples(const MP4D_track_t *track)
{
    return track->sample_count && track->sample_count <= TA_AAC_PACKETS_MAX &&
           track->sample_count <= track->timestamp_count && !track->comp_timestamp;
}
static int ta_aac_track_entry(const MP4D_track_t *track, size_t asc_capacity)
{
    return track->sample_entry == TA_FOURCC('m', 'p', '4', 'a') && track->object_type_indication == 0x40 &&
           track->dsi && track->dsi_bytes >= 2 && track->dsi_bytes <= asc_capacity && ta_aac_track_samples(track);
}
static int ta_aac_lc_config(const mp4AudioSpecificConfig *config)
{
    /* Only the documented value 1 enables extensions; plain char is unsigned on AArch64. */
    return config->objectTypeIndex == LC && !config->frameLengthFlag && !config->dependsOnCoreCoder &&
           config->sbr_present_flag != 1 && config->ps_present_flag != 1;
}
static int ta_aac_mp4_config(ta_aac *a, MP4D_track_t *track)
{
    mp4AudioSpecificConfig config = { 0 };
    if (NeAACDecAudioSpecificConfig(track->dsi, track->dsi_bytes, &config) != 0 || !ta_aac_lc_config(&config)) return 0;
    if (config.channelsConfiguration < 1 || config.channelsConfiguration > 2 ||
        (config.samplingFrequency != 44100 && config.samplingFrequency != 48000) ||
        track->timescale != config.samplingFrequency) return 0;
    a->rate = (unsigned)config.samplingFrequency;
    a->channels = config.channelsConfiguration;
    a->asc_size = track->dsi_bytes;
    memcpy(a->asc, track->dsi, a->asc_size);
    a->packet_count = track->sample_count;
    return (uint64_t)a->packet_count * 1024 <= (uint64_t)a->rate * 1200;
}
static int ta_aac_packet_range(const ta_aac *a, uint64_t offset, unsigned size, uint64_t previous_end)
{
    return size && size <= TA_AAC_PACKET_MAX && offset >= previous_end && offset <= a->size && size <= a->size - offset;
}
static int ta_aac_packet_clock(unsigned i, unsigned count, unsigned stamp, unsigned duration, unsigned rate)
{
    int order = compare_timestamps(stamp, (uint64_t)i * 1024, rate, rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL && duration && duration <= 1024 &&
           !(i + 1 < count && duration < 1024);
}
static int ta_aac_mp4_packets(ta_aac *a, MP4D_demux_t *mp4, unsigned selected)
{
    a->packets = ta_calloc(a->packet_count, sizeof *a->packets);
    if (!a->packets) return 0;
    uint64_t previous_end = 0;
    for (unsigned i = 0; i < a->packet_count; i++) {
        unsigned size, stamp, duration;
        uint64_t offset = MP4D_frame_offset(mp4, selected, i, &size, &stamp, NULL, &duration, NULL);
        if (!ta_aac_packet_range(a, offset, size, previous_end) || !ta_aac_packet_clock(i, a->packet_count, stamp, duration, a->rate)) return 0;
        a->packets[i] = (struct ta_aac_packet){ offset, size };
        previous_end = offset + size;
    }
    return 1;
}
static int ta_aac_mp4_track(ta_aac *a, MP4D_demux_t *mp4)
{
    unsigned selected = ta_aac_audio_track(mp4);
    if (selected == UINT_MAX) return 0;
    MP4D_track_t *track = mp4->track + selected;
    if (!ta_aac_track_entry(track, sizeof a->asc) || !ta_aac_mp4_config(a, track) || !ta_aac_mp4_packets(a, mp4, selected)) return 0;
    a->clock.kind = TA_DECODE_AAC_ENCODED;
    return ta_aac_mp4_edits(a, selected, mp4->timescale);
}
static int ta_aac_mp4(ta_aac *a)
{
    unsigned boxes = 0;
    if (!ta_aac_mp4_boxes(a, 0, a->size, 0, &boxes)) return 0;
    struct ta_budget budget = { 0 };
    MP4D_demux_t mp4 = { 0 };
    ta_current_budget = &budget;
    int opened = MP4D_open(&mp4, ta_aac_bytes, a, (int64_t)a->size);
    ta_current_budget = NULL;
    if (!opened) {
        ta_mp4_release(&budget);
        return 0;
    }
    int ok = ta_aac_mp4_track(a, &mp4);
    MP4D_close(&mp4);
    ta_mp4_release(&budget);
    return ok;
}
static int ta_aac_id3_offset(const unsigned char h[10], uint64_t *at)
{
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;
    *at = 10 + ((uint64_t)h[6] << 21 | (unsigned)h[7] << 14 | (unsigned)h[8] << 7 | h[9]);
    if (h[5] & 0x10) *at += 10;
    return 1;
}
static int ta_aac_adts_header(const unsigned char h[10], unsigned freq, unsigned channels)
{
    return h[0] == 255 && (h[1] & 0xf6) == 0xf0 && h[2] >> 6 == 1 &&
           (freq == 3 || freq == 4) && channels >= 1 && channels <= 2 && !(h[6] & 3);
}
static int ta_aac_adts_packet(ta_aac *a, const unsigned char h[10], uint64_t *at)
{
    unsigned freq = (h[2] >> 2) & 15, channels = (h[2] & 1) << 2 | h[3] >> 6;
    unsigned size = (h[3] & 3) << 11 | (unsigned)h[4] << 3 | h[5] >> 5;
    unsigned header = h[1] & 1 ? 7 : 9;
    if (!ta_aac_adts_header(h, freq, channels) || size <= header || size > a->size - *at) return 0;
    unsigned rate = freq == 3 ? 48000 : 44100;
    if (a->packet_count && (rate != a->rate || channels != a->channels)) return 0;
    a->rate = rate;
    a->channels = channels;
    /* Decode raw access units using the same ASC contract as MP4. */
    a->asc[0] = (2 << 3) | (freq >> 1);
    a->asc[1] = (freq << 7) | (channels << 3);
    a->asc_size = 2;
    a->packets[a->packet_count++] = (struct ta_aac_packet){ *at + header, size - header };
    *at += size;
    return 1;
}
static int ta_aac_adts_start(ta_aac *a, unsigned char h[10], uint64_t *at)
{
    if (!ta_aac_bytes(0, h, 10, a) && !memcmp(h, "ID3", 3)) return ta_aac_id3_offset(h, at);
    return 1;
}
static int ta_aac_adts(ta_aac *a)
{
    a->packets = ta_calloc(TA_AAC_PACKETS_MAX, sizeof *a->packets);
    if (!a->packets) return 0;
    unsigned char h[10];
    uint64_t at = 0;
    if (!ta_aac_adts_start(a, h, &at)) return 0;
    while (at < a->size) {
        if (a->packet_count >= TA_AAC_PACKETS_MAX || ta_aac_bytes(at, h, 7, a)) return 0;
        if (!ta_aac_adts_packet(a, h, &at)) return 0;
    }
    a->clock.kind = TA_DECODE_AAC_ENCODED;
    return a->packet_count && (uint64_t)a->packet_count * 1024 <= (uint64_t)a->rate * 1200;
}
static int ta_aac_reset(ta_aac *a)
{
    if (a->codec) NeAACDecClose(a->codec);
    a->codec = NeAACDecOpen();
    if (!a->codec) return 0;
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(a->codec);
    config->defObjectType = LC;
    config->outputFormat = FAAD_FMT_16BIT;
    config->dontUpSampleImplicitSBR = 1;
    unsigned long rate = 0;
    unsigned char channels = 0;
    if (!NeAACDecSetConfiguration(a->codec, config) ||
        NeAACDecInit2(a->codec, a->asc, a->asc_size, &rate, &channels) != 0 ||
        rate != a->rate || channels != a->channels) return 0;
    a->position = 0;
    a->next_packet = a->buffered = a->consumed = 0;
    return 1;
}
static int ta_aac_candidate_file(FILE *file)
{
    unsigned char header[10];
    if (fread(header, 1, sizeof header, file) != sizeof header) return 0;
    if (!memcmp(header + 4, "ftyp", 4)) return 1;
    if (!memcmp(header, "ID3", 3)) {
        uint64_t skip;
        if (!ta_aac_id3_offset(header, &skip)) return 0;
        if (skip > 128 * 1024 * 1024 || fseek(file, (long)skip, SEEK_SET) ||
            fread(header, 1, sizeof header, file) != sizeof header) return 0;
    }
    return header[0] == 255 && (header[1] & 0xf6) == 0xf0;
}
int ta_aac_candidate(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int candidate = ta_aac_candidate_file(file);
    fclose(file);
    return candidate;
}
static int ta_aac_open_file(ta_aac *a, const char *path)
{
    a->file = fopen(path, "rb");
    if (!a->file || fseek(a->file, 0, SEEK_END)) return 0;
    long size = ftell(a->file);
    if (size < 10 || size > 128 * 1024 * 1024) return 0;
    a->size = (uint64_t)size;
    return 1;
}
ta_aac *ta_aac_open(const char *path, unsigned *rate, uint64_t *frames)
{
    ta_aac *a = ta_calloc(1, sizeof *a);
    if (!a) return NULL;
    if (!ta_aac_open_file(a, path)) goto bad;
    unsigned char header[12];
    if (ta_aac_bytes(0, header, sizeof header, a)) goto bad;
    int is_mp4 = !memcmp(header + 4, "ftyp", 4);
    if (!(is_mp4 ? ta_aac_mp4(a) : ta_aac_adts(a)) || !ta_aac_reset(a)) goto bad;
    /* FAAD2 discards its first 1024-sample AAC-LC unit. Restore that slot as
     * silence so all returned positions are explicitly on the encoded clock. */
    a->clock.decoder_leading_frames = 1024;
    *rate = a->rate;
    *frames = (uint64_t)a->packet_count * 1024;
    return a;
bad:
    ta_aac_close(a);
    return NULL;
}
static int ta_aac_frame_valid(const ta_aac *a, const NeAACDecFrameInfo *info, unsigned size)
{
    return !info->error && info->channels == a->channels && info->samplerate == a->rate &&
           !info->sbr && info->bytesconsumed <= size &&
           (a->next_packet ? info->samples == a->channels * 1024 : info->samples == 0);
}
static int ta_aac_next(ta_aac *a)
{
    unsigned char packet[TA_AAC_PACKET_MAX];
    if (a->next_packet >= a->packet_count) return 0;
    struct ta_aac_packet p = a->packets[a->next_packet];
    if (ta_aac_bytes(p.offset, packet, p.size, a)) return -1;
    NeAACDecFrameInfo info = { 0 };
    int16_t *pcm = NeAACDecDecode(a->codec, &info, packet, p.size);
    if (!ta_aac_frame_valid(a, &info, p.size)) return -1;
    if (!a->next_packet) memset(a->pcm, 0, sizeof a->pcm);
    else {
        if (!pcm) return -1;
        for (unsigned i = 0; i < 1024; i++) {
            a->pcm[i * 2] = pcm[i * a->channels];
            a->pcm[i * 2 + 1] = pcm[i * a->channels + a->channels - 1];
        }
    }
    a->next_packet++;
    a->buffered = 1024;
    a->consumed = 0;
    return 1;
}
int64_t ta_aac_read(ta_aac *a, int16_t *stereo, uint64_t frames)
{
    uint64_t read = 0;
    while (read < frames) {
        if (a->consumed >= a->buffered) {
            int rc = ta_aac_next(a);
            if (rc < 0) return -1;
            if (!rc) break;
        }
        unsigned take = a->buffered - a->consumed;
        if (take > frames - read) take = (unsigned)(frames - read);
        memcpy(stereo + read * 2, a->pcm + a->consumed * 2, take * 2 * sizeof *stereo);
        read += take;
        a->consumed += take;
        a->position += take;
    }
    return (int64_t)read;
}
int ta_aac_seek(ta_aac *a, uint64_t frame)
{
    if (frame > (uint64_t)a->packet_count * 1024) return -1;
    if (frame < a->position && !ta_aac_reset(a)) return -1;
    /* Replay preserves AAC overlap state exactly, including backwards model
     * windows. It uses constant PCM memory and never spools decoded tracks. */
    int16_t discard[2048];
    while (a->position < frame) {
        uint64_t take = frame - a->position;
        if (take > 1024) take = 1024;
        if (ta_aac_read(a, discard, take) < (int64_t)take) return -1;
    }
    return 0;
}
void ta_aac_timeline(ta_aac *a, ta_decode_clock *clock) { *clock = a->clock; }
void ta_aac_close(ta_aac *a)
{
    if (!a) return;
    if (a->codec) NeAACDecClose(a->codec);
    if (a->file) fclose(a->file);
    ta_free(a->packets);
    ta_free(a);
}

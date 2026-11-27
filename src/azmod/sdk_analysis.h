/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded, allocation-free ANLZ parsing and grid timing. No native pointers or I/O. */
#ifndef AZMOD_SDK_ANALYSIS_H
#define AZMOD_SDK_ANALYSIS_H
#include "../common/aztimestamp.h"
#include "../common/azanalysis_policy.h"
#include "azmod-mod.h"
#include "hc_anlz.h"
#include <math.h>
#include <string.h>
#define ST_BEATS AZ_ANALYSIS_POLICY_MAXIMUM_GRID_BEATS
#define ST_CUES 256
#define ST_PHRASES AZ_ANALYSIS_POLICY_MAXIMUM_PHRASE_COUNT
#define ST_WAVE 4096
#define ST_FILE_MAX (16u * 1024u * 1024u)
struct st_data {
    az_track_snapshot view;
    az_wave_query wave_query;
    az_wave_snapshot wave_view;
    uint8_t detail[AZ_WAVE_WINDOW_MAX * 6];
    az_beat beats[ST_BEATS];
    int64_t *beat_ticks; /* optional status-owned exact PQTZ millisecond axis */
    az_cue_entry cues[ST_CUES];
    int64_t *cue_ticks; /* optional status-owned START/END millisecond pairs */
    az_phrase phrases[ST_PHRASES];
    uint8_t wave[ST_WAVE];
    char files[3][1024]; /* provider-owned freshness inputs, never public native pointers */
    char analysis_paths[2][1024]; /* source-relative bindings, including symlink aliases */
    uint64_t signatures[3];
    unsigned cue_lists; /* bit 0: memory, bit 1: hot, including zero-entry declarations */
    int grid_offset_ms; /* private PQTZ origin used by status; explicit ticket rows retain their timing */
};
static inline void st_data_init(struct st_data *d) {
    memset(d, 0, sizeof *d);
    d->view.size = sizeof d->view;
    d->view.version = AZ_DATA_VERSION;
    d->wave_view.size = sizeof d->wave_view;
    d->wave_view.version = AZ_DATA_VERSION;
    d->wave_view.columns = d->detail;
    d->view.metadata.size = sizeof d->view.metadata;
    d->view.metadata.version = AZ_DATA_VERSION;
    d->view.beats = d->beats;
    d->view.cues = d->cues;
    d->view.phrases = d->phrases;
    d->view.waveform.size = sizeof d->view.waveform;
    d->view.waveform.version = AZ_DATA_VERSION;
    d->view.waveform.samples = d->wave;
}
static inline uint32_t st_comment_codepoint(const uint8_t *p, size_t n, size_t *at) {
    uint32_t c = hc_be16(p + *at);
    if (c >= 0xd800 && c <= 0xdbff && *at + 3 < n) {
        uint32_t low = hc_be16(p + *at + 2);
        if (low >= 0xdc00 && low <= 0xdfff) {
            *at += 2;
            return 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
        }
        return 0xfffd;
    }
    return c >= 0xd800 && c <= 0xdfff ? 0xfffd : c;
}
static inline unsigned st_comment_width(uint32_t c) {
    return c < 0x80 ? 1 : c < 0x800 ? 2
                      : c < 0x10000 ? 3
                                    : 4;
}
static inline void st_comment_encode(char *out, uint32_t c, unsigned length) {
    if (length == 1) {
        *out = (char) c;
        return;
    }
    *out++ = (char) ((length == 2 ? 0xc0 : length == 3 ? 0xe0
                                                       : 0xf0) |
                     (c >> (6 * (length - 1))));
    for (unsigned j = length - 1; j; j--) *out++ = (char) (0x80 | ((c >> (6 * (j - 1))) & 63));
}
/* Fixed output UTF-16BE decoder. Surrogate pairs are preserved, invalid units use U+FFFD. */
static inline void st_comment(const uint8_t *p, size_t n, char *out, size_t cap) {
    size_t used = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = st_comment_codepoint(p, n, &i);
        if (!c) break;
        unsigned length = st_comment_width(c);
        if (used + length >= cap) break;
        st_comment_encode(out + used, c, length);
        used += length;
    }
    if (cap) out[used] = 0;
}
struct st_cue_record {
    const uint8_t *bytes;
    size_t size;
    uint32_t slot, type, start, end;
};
static inline int st_cue_record_framed(const uint8_t *e, size_t en, size_t remaining, int co2) {
    return !(en > remaining || en < 0x28u || hc_be32(e + 4) != (co2 ? 0x10u : 0x1cu) ||
             memcmp(e, co2 ? "PCP2" : "PCPT", 4));
}
static inline int st_cue_record_identity(const struct st_cue_record *row, unsigned kind) {
    return (!kind || (row->slot >= 1 && row->slot <= 8)) && (row->type == 1 || row->type == 2);
}
static inline int st_cue_record_values(const struct st_cue_record *row, unsigned kind) {
    if (!st_cue_record_identity(row, kind)) return 0;
    if (row->start == UINT32_MAX) return 0;
    if (row->type != 2) return 1;
    int order = compare_timestamps(row->end, row->start, 1000, 1000, 1);
    return row->end != UINT32_MAX && order != AZ_TIME_INVALID && order == AZ_TIME_AFTER;
}
static inline int st_cue_record_read(const uint8_t *e, size_t remaining, unsigned kind, int co2, struct st_cue_record *row) {
    if (remaining < 12) return AZ_FAILED;
    size_t en = hc_be32(e + 8);
    if (!st_cue_record_framed(e, en, remaining, co2)) return AZ_FAILED;
    *row = (struct st_cue_record) {e, en, hc_be32(e + 12), e[co2 ? 0x10 : 0x1c],
                                   hc_be32(e + (co2 ? 0x14 : 0x20)), hc_be32(e + (co2 ? 0x18 : 0x24))};
    return st_cue_record_values(row, kind) ? AZ_OK : AZ_FAILED;
}
static inline void st_cue_loop(const struct st_cue_record *row, int co2, az_cue_entry *c) {
    if (row->type != 2) return;
    c->valid |= AZ_CUE_END;
    c->end_s = row->end / 1000.0;
    if (!co2) {
        uint32_t status = hc_be32(row->bytes + 0x10);
        if (status == 0 || status == 1 || status == 4) {
            c->valid |= AZ_CUE_AUTO_ACTIVE;
            c->auto_active = status == 4;
        }
    }
}
static inline int st_cue_optional(const struct st_cue_record *row, unsigned kind, az_cue_entry *c) {
    const uint8_t *e = row->bytes;
    size_t en = row->size;
    /* Short PCP2 entries have no comment length field. */
    if (en < 0x2c) return AZ_OK;
    size_t nc = hc_be32(e + 0x28);
    if (nc > en - 0x2c || (nc & 1)) return AZ_FAILED;
    if (nc) {
        st_comment(e + 0x2c, nc, c->comment, sizeof c->comment);
        c->valid |= AZ_CUE_COMMENT;
    }
    if (kind && en - 0x2c - nc >= 4) {
        const uint8_t *rgb = e + 0x2c + nc;
        c->rgb = (uint32_t) rgb[1] << 16 | (uint32_t) rgb[2] << 8 | rgb[3];
        c->valid |= AZ_CUE_COLOR;
    }
    return AZ_OK;
}
static inline int st_cue_decode(const struct st_data *d, const struct st_cue_record *row, unsigned kind, int co2, az_cue_entry *c) {
    memset(c, 0, sizeof *c);
    c->size = sizeof *c;
    c->version = AZ_DATA_VERSION;
    c->classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
    c->entry_id = kind ? row->slot : d->view.cue_count + 1;
    c->slot = row->slot;
    c->type = row->type;
    c->valid = AZ_CUE_START | AZ_CUE_SAVED;
    c->start_s = row->start / 1000.0;
    c->saved = 1;
    c->modified = c->auto_active = -1;
    if (co2 && !kind) {
        c->color_id = row->bytes[0x1c];
        c->valid |= AZ_CUE_COLOR_ID;
    }
    st_cue_loop(row, co2, c);
    return co2 ? st_cue_optional(row, kind, c) : AZ_OK;
}
static inline uint32_t st_cue_destination(const struct st_data *d, unsigned kind, uint32_t slot) {
    if (kind)
        for (uint32_t i = 0; i < d->view.cue_count; i++)
            if (d->cues[i].classification == AZ_CUE_HOT && d->cues[i].slot == slot) return i;
    return d->view.cue_count;
}
static inline int st_cue_store(struct st_data *d, const struct st_cue_record *row, unsigned kind,
                               const az_cue_entry *cue, const uint8_t **origins) {
    uint32_t at = st_cue_destination(d, kind, row->slot);
    if (at >= ST_CUES) return AZ_LIMIT;
    d->cues[at] = *cue;
    if (d->cue_ticks) {
        d->cue_ticks[2 * at] = row->start;
        d->cue_ticks[2 * at + 1] = row->type == AZ_CUE_LOOP ? row->end : 0;
    }
    if (origins) origins[at] = row->bytes;
    if (at == d->view.cue_count) d->view.cue_count++;
    return AZ_OK;
}
static inline int st_cue_list_count(const uint8_t *t, size_t h, int co2) {
    if (h < (co2 ? 0x14u : 0x18u)) return -1;
    uint32_t kind = hc_be32(t + 12), count = hc_be16(t + (co2 ? 16 : 18));
    return kind > 1 || count > ST_CUES ? -1 : (int) count;
}
static inline int st_cue_list(struct st_data *d, const uint8_t *t, size_t h, size_t n, int co2, const uint8_t **origins) {
    int count = st_cue_list_count(t, h, co2);
    if (count < 0) return AZ_FAILED;
    uint32_t kind = hc_be32(t + 12);
    size_t offset = h;
    for (int i = 0; i < count; i++) {
        struct st_cue_record row;
        int rc = st_cue_record_read(t + offset, n - offset, kind, co2, &row);
        if (rc) return rc;
        az_cue_entry cue;
        rc = st_cue_decode(d, &row, kind, co2, &cue);
        if (rc) return rc;
        rc = st_cue_store(d, &row, kind, &cue, origins);
        if (rc) return rc;
        offset += row.size;
    }
    return AZ_OK;
}
struct st_saved_active {
    uint32_t kind, slot, start_ms, end_ms;
    int active;
};
struct st_active_history {
    struct st_saved_active saved[ST_CUES * 2];
    unsigned count, entries;
};
static inline void st_save_active(struct st_active_history *history, const struct st_cue_record *row, unsigned kind) {
    if (row->type != 2) return;
    uint32_t status = hc_be32(row->bytes + 0x10);
    int active = status == 4 ? 1 : status == 0 || status == 1 ? 0
                                                              : -1;
    history->saved[history->count++] = (struct st_saved_active) {kind, row->slot, row->start, row->end, active};
}
static inline int st_read_active_list(struct st_active_history *history, const uint8_t *tag, unsigned kind) {
    size_t header = hc_be32(tag + 4), length = hc_be32(tag + 8);
    if (header < 24) return 0;
    uint32_t count = hc_be16(tag + 18);
    if (count > ST_CUES || count > ST_CUES * 2u - history->entries) return 0;
    history->entries += count;
    size_t offset = header;
    for (uint32_t i = 0; i < count; i++) {
        struct st_cue_record row;
        if (st_cue_record_read(tag + offset, length - offset, kind, 0, &row) != AZ_OK) return 0;
        st_save_active(history, &row, kind);
        offset += row.size;
    }
    return 1;
}
static inline int st_read_active_file(struct st_active_history *history, const uint8_t *buffer, size_t size,
                                      int file, unsigned extended, unsigned dat_legacy) {
    if (!buffer) return 1;
    for (size_t off = hc_be32(buffer + 4); off < size; off += hc_be32(buffer + off + 8)) {
        const uint8_t *tag = buffer + off;
        if (memcmp(tag, "PCOB", 4)) continue;
        unsigned kind = hc_be32(tag + 12), bit = 1u << kind;
        if (!(extended & bit) || (!kind && file && (dat_legacy & bit))) continue;
        if (!st_read_active_list(history, tag, kind)) return 0;
    }
    return 1;
}
static inline int st_active_matches(const struct st_saved_active *saved, const az_cue_entry *cue, unsigned kind,
                                    uint64_t start_ms, uint64_t end_ms) {
    if (saved->kind != kind || (kind && saved->slot != cue->slot)) return 0;
    int start = compare_timestamps(saved->start_ms, start_ms, 1000, 1000, 1);
    int end = compare_timestamps(saved->end_ms, end_ms, 1000, 1000, 1);
    if (start == AZ_TIME_INVALID || end == AZ_TIME_INVALID) return 0;
    return start == AZ_TIME_EQUAL && end == AZ_TIME_EQUAL;
}
static inline void st_apply_saved_active(az_cue_entry *cue, const struct st_active_history *history, unsigned extended) {
    unsigned kind = cue->classification == AZ_CUE_HOT, bit = 1u << kind;
    /* Parsed rows originate in exact u32 milliseconds. */
    uint64_t start_ms = (uint64_t) llround(cue->start_s * 1000.0);
    uint64_t end_ms = (uint64_t) llround(cue->end_s * 1000.0);
    if (cue->type != AZ_CUE_LOOP || !(extended & bit)) return;
    unsigned matches = 0;
    int active = -1;
    for (unsigned i = 0; i < history->count; i++) {
        if (!st_active_matches(&history->saved[i], cue, kind, start_ms, end_ms)) continue;
        matches++;
        active = history->saved[i].active;
    }
    if (matches == 1 && active >= 0) {
        cue->valid |= AZ_CUE_AUTO_ACTIVE;
        cue->auto_active = active;
    }
}
/* PCP2 omits PCPT's saved Active Loop status. Carry it only across an exact,
 * unique saved-loop counterpart; malformed legacy data leaves PCP2 authoritative. */
static inline void st_enrich_active(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                    const unsigned co2[2], const unsigned cob[2]) {
    struct st_active_history history = {0};
    unsigned extended = co2[0] | co2[1];
    for (int file = 0; file < 2; file++)
        if (!st_read_active_file(&history, buffers[file], sizes[file], file, extended, cob[0])) return;
    for (uint32_t i = 0; i < d->view.cue_count; i++) st_apply_saved_active(&d->cues[i], &history, extended);
}
static inline uint16_t st_ps16(const uint8_t *t, size_t off, uint16_t count, int masked) {
    static const uint8_t key[19] = {0xcb, 0xe1, 0xee, 0xfa, 0xe5, 0xee, 0xad, 0xee, 0xe9, 0xd2,
                                    0xe9, 0xeb, 0xe1, 0xe9, 0xf3, 0xe8, 0xe9, 0xf4, 0xe1};
    uint8_t a = t[off], b = t[off + 1];
    if (masked) {
        a ^= (uint8_t) (key[(off - 18) % 19] + count);
        b ^= (uint8_t) (key[(off - 17) % 19] + count);
    }
    return (uint16_t) ((unsigned) a << 8 | b);
}
static inline int st_grid_row(struct st_data *d, const uint8_t *e, uint32_t i) {
    uint32_t milliseconds = hc_be32(e + 4);
    const unsigned units = AZ_ANALYSIS_POLICY_STORED_MILLISECONDS_PER_SECOND;
    if (i) {
        int order = compare_timestamps(milliseconds, hc_be32(e - 4), units, units, 1);
        if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return AZ_FAILED;
    }
    d->beats[i] = (az_beat) {milliseconds / (double)units,
                            hc_be16(e + 2) / (double)AZ_ANALYSIS_POLICY_TEMPO_UNITS_PER_BPM, hc_be16(e), 0};
    if (d->beat_ticks) d->beat_ticks[i] = milliseconds;
    return AZ_OK;
}
static inline int st_grid_section(struct st_data *d, const uint8_t *t, size_t h, size_t n) {
    if (h < 24) return AZ_FAILED;
    uint32_t count = hc_be32(t + 20);
    if (count > ST_BEATS || count > (n - h) / 8) return AZ_FAILED;
    unsigned encoded = hc_be16(t + 18);
    d->grid_offset_ms = encoded < 0x8000u ? (int) encoded : (int) encoded - 0x10000;
    for (uint32_t i = 0; i < count; i++) {
        int rc = st_grid_row(d, t + h + i * 8, i);
        if (rc) return rc;
    }
    d->view.beat_count = count;
    d->view.grid_info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    return AZ_OK;
}
static inline int st_phrase_rows(struct st_data *d, const uint8_t *t, size_t header, uint32_t stride, uint32_t count) {
    int masked = hc_be16(t + 18) > 20;
    uint16_t mood = st_ps16(t, 18, (uint16_t) count, masked), end = st_ps16(t, 26, (uint16_t) count, masked);
    if (mood < 1 || mood > 3) return AZ_FAILED;
    for (uint32_t i = 0; i < count; i++) {
        size_t off = header + i * stride;
        uint16_t start = st_ps16(t, off + 2, (uint16_t) count, masked);
        uint16_t next = i + 1 < count ? st_ps16(t, off + stride + 2, (uint16_t) count, masked) : end;
        if (!start || next <= start) return AZ_FAILED;
        d->phrases[i] = (az_phrase) {start - 1u, next - 1u, st_ps16(t, off + 4, (uint16_t) count, masked), mood, 0, 0};
    }
    d->view.phrase_count = count;
    d->view.phrases_info.availability = AZ_AVAIL_READY;
    return AZ_OK;
}
static inline int st_phrase_section(struct st_data *d, const uint8_t *t, size_t h, size_t n) {
    if (h < 32) return AZ_FAILED;
    uint32_t version = hc_be16(t + 12);
    /* Version zero and one retain a 16-bit stride; unfamiliar versions refuse. */
    if (version > 1) {
        d->view.phrases_info.availability = AZ_AVAIL_UNSUPPORTED;
        return AZ_OK;
    }
    uint32_t stride = hc_be16(t + 14), count = hc_be16(t + 16);
    if (stride < 24 || stride > 64 || count > ST_PHRASES || count > (n - h) / stride) return AZ_FAILED;
    if (!count) {
        d->view.phrases_info.availability = AZ_AVAIL_EMPTY;
        return AZ_OK;
    }
    return st_phrase_rows(d, t, h, stride, count);
}
static inline int st_preview_section(struct st_data *d, const uint8_t *t, size_t h, size_t n) {
    if (h < 20) return AZ_FAILED;
    uint32_t count = hc_be32(t + 12);
    if (count > ST_WAVE || count > n - h) return AZ_LIMIT;
    memcpy(d->wave, t + h, count);
    d->view.waveform.sample_count = count;
    d->view.waveform.representation = AZ_WAVE_PREVIEW;
    d->view.waveform_info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    return AZ_OK;
}
static inline int st_section(struct st_data *d, const uint8_t *t, size_t h, size_t n, uint32_t mask) {
    if ((mask & AZ_DATA_GRID) && !memcmp(t, "PQTZ", 4) && !d->view.beat_count) return st_grid_section(d, t, h, n);
    if ((mask & AZ_DATA_PHRASES) && !memcmp(t, "PSSI", 4) && !d->view.phrase_count) return st_phrase_section(d, t, h, n);
    if ((mask & AZ_DATA_WAVEFORM) && !memcmp(t, "PWAV", 4)) return st_preview_section(d, t, h, n);
    return AZ_OK;
}
struct st_parse_context {
    struct st_data *data;
    unsigned co2[2], cob[2], cue_kinds;
    uint32_t mask;
    const uint8_t **origins;
};
static inline int st_cue_section(const uint8_t *tag) {
    return !memcmp(tag, "PCO2", 4) || !memcmp(tag, "PCOB", 4);
}
static inline int st_record_cue_kind(struct st_parse_context *ctx, const uint8_t *tag, size_t header, int file) {
    if (!ctx->cue_kinds || !st_cue_section(tag)) return AZ_OK;
    if (header < 16 || hc_be32(tag + 12) > 1) return AZ_FAILED;
    unsigned bit = 1u << hc_be32(tag + 12);
    if (ctx->cue_kinds & bit) {
        if (tag[3] == '2') ctx->co2[file] |= bit;
        else ctx->cob[file] |= bit;
    }
    return AZ_OK;
}
static inline int st_container_header(const uint8_t *b, size_t n) {
    if (!b || n < 12 || n > ST_FILE_MAX || memcmp(b, "PMAI", 4)) return 0;
    size_t header = hc_be32(b + 4);
    return header >= 12 && header <= n && hc_be32(b + 8) == n;
}
static inline size_t st_chunk_length(const uint8_t *tag, size_t remaining) {
    if (remaining < 12) return 0;
    size_t header = hc_be32(tag + 4), length = hc_be32(tag + 8);
    return header >= 12 && header <= length && length <= remaining ? length : 0;
}
static inline int st_parse_container(struct st_parse_context *ctx, const uint8_t *b, size_t n, int file) {
    if (!b && !n) return AZ_OK;
    if (!st_container_header(b, n)) return AZ_FAILED;
    for (size_t off = hc_be32(b + 4); off < n;) {
        const uint8_t *tag = b + off;
        size_t length = st_chunk_length(tag, n - off);
        if (!length) return AZ_FAILED;
        size_t header = hc_be32(tag + 4);
        int rc = st_record_cue_kind(ctx, tag, header, file);
        if (rc) return rc;
        rc = st_section(ctx->data, tag, header, length, ctx->mask);
        if (rc) return rc;
        off += length;
    }
    return AZ_OK;
}
static inline int st_cue_selected(const struct st_parse_context *ctx, const uint8_t *tag, int file, unsigned bit) {
    if ((ctx->co2[0] | ctx->co2[1]) & bit)
        return tag[3] == '2' && (file == 1 || !(ctx->co2[1] & bit));
    /* Legacy hot slots span DAT and EXT; memory entries retain DAT precedence. */
    return tag[3] == 'B' && (bit == 2u || file == 0 || !(ctx->cob[0] & bit));
}
static inline int st_parse_cue_container(struct st_parse_context *ctx, const uint8_t *b, size_t n, int file) {
    if (!b) return AZ_OK;
    for (size_t off = hc_be32(b + 4); off < n; off += hc_be32(b + off + 8)) {
        const uint8_t *tag = b + off;
        if (!st_cue_section(tag)) continue;
        unsigned bit = 1u << hc_be32(tag + 12);
        if (!(ctx->cue_kinds & bit) || !st_cue_selected(ctx, tag, file, bit)) continue;
        int rc = st_cue_list(ctx->data, tag, hc_be32(tag + 4), hc_be32(tag + 8), tag[3] == '2', ctx->origins);
        if (rc) return rc;
    }
    return AZ_OK;
}
/* Validate the whole container before exposing any component. Extended lists take
 * precedence per classification, including an explicitly empty extended list. */
static inline int st_parse_origins(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                   uint32_t mask, unsigned cue_kinds, const uint8_t **origins) {
    struct st_parse_context ctx = {.data = d, .cue_kinds = cue_kinds, .mask = mask, .origins = origins};
    for (int file = 0; file < 2; file++) {
        int rc = st_parse_container(&ctx, buffers[file], sizes[file], file);
        if (rc) return rc;
    }
    if (!(mask & AZ_DATA_CUES)) return AZ_OK;
    for (int file = 0; file < 2; file++) {
        int rc = st_parse_cue_container(&ctx, buffers[file], sizes[file], file);
        if (rc) return rc;
    }
    d->cue_lists = ctx.co2[0] | ctx.co2[1] | ctx.cob[0] | ctx.cob[1];
    if (d->cue_lists) d->view.cues_info.availability = d->view.cue_count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    st_enrich_active(d, buffers, sizes, ctx.co2, ctx.cob);
    return AZ_OK;
}
static inline int st_parse_selected(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                    uint32_t mask, unsigned cue_kinds) {
    return st_parse_origins(d, buffers, sizes, mask, cue_kinds, NULL);
}
/* Preserve combined-ticket validation, including malformed cue tag headers. */
static inline int st_parse_pair(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2], uint32_t mask) {
    return st_parse_selected(d, buffers, sizes, mask, 3);
}
/* Only DAT supplies the saved grid and PWAV, only EXT supplies PSSI, and both
 * containers can contribute cue lists. Framing must be intact in each dependency. */
static inline int st_parse_component(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                     uint32_t component) {
    int dat = component != AZ_DATA_PHRASES;
    int ext = component == AZ_DATA_PHRASES || component == AZ_DATA_CUES;
    const uint8_t *input[] = {dat ? buffers[0] : NULL, ext ? buffers[1] : NULL};
    size_t lengths[] = {dat ? sizes[0] : 0, ext ? sizes[1] : 0};
    return st_parse_selected(d, input, lengths, component, component == AZ_DATA_CUES ? 3u : 0u);
}
/* No extrapolation beyond the captured grid; no constant-tempo or 4/4 assumption. */
static inline uint32_t st_record_size(const void *record) {
    uint32_t size;
    memcpy(&size, record, sizeof size); /* Short caller prefixes must be read without a full-record lvalue. */
    return size;
}
static inline int st_grid_available(const az_track_snapshot *s) {
    if (s->beat_count && !s->beats) return AZ_INVALID;
    return s->grid_info.availability != AZ_AVAIL_READY || s->beat_count < 2 ? AZ_UNSUPPORTED : AZ_OK;
}
struct st_grid_bracket {
    uint32_t lo, hi;
};
static inline int st_grid_extent(const az_beat *b, struct st_grid_bracket bracket, double time, int *end) {
    int extent = compare_timestamps(b[bracket.hi].time_s, b[bracket.lo].time_s, 1, 1000, 1);
    int start = compare_timestamps(time, b[bracket.lo].time_s, 1, 1000, 1);
    *end = compare_timestamps(time, b[bracket.hi].time_s, 1, 1000, 1);
    if (extent == AZ_TIME_INVALID || extent != AZ_TIME_AFTER || start == AZ_TIME_INVALID || *end == AZ_TIME_INVALID) return AZ_FAILED;
    return start == AZ_TIME_BEFORE || *end == AZ_TIME_AFTER ? AZ_NOT_FOUND : AZ_OK;
}
static inline int st_grid_bisect(const az_beat *b, double time, struct st_grid_bracket *bracket) {
    uint32_t mid = bracket->lo + (bracket->hi - bracket->lo) / 2;
    int lower = compare_timestamps(b[mid].time_s, b[bracket->lo].time_s, 1, 1000, 1);
    int upper = compare_timestamps(b[mid].time_s, b[bracket->hi].time_s, 1, 1000, 1);
    int target = compare_timestamps(b[mid].time_s, time, 1, 1000, 1);
    if (lower == AZ_TIME_INVALID || upper == AZ_TIME_INVALID || target == AZ_TIME_INVALID ||
        lower != AZ_TIME_AFTER || upper != AZ_TIME_BEFORE) return AZ_FAILED;
    if (target == AZ_TIME_BEFORE || target == AZ_TIME_EQUAL) bracket->lo = mid;
    else bracket->hi = mid;
    return AZ_OK;
}
static inline int st_grid_interpolate(const az_beat *b, struct st_grid_bracket bracket, double time, double *out) {
    double span = b[bracket.hi].time_s - b[bracket.lo].time_s;
    int order = compare_timestamps(span, 0.0, 1, 1000, 1);
    if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return AZ_FAILED;
    double value = bracket.lo + (time - b[bracket.lo].time_s) / span;
    if (!isfinite(value)) return AZ_FAILED;
    *out = value;
    return AZ_OK;
}
static inline int st_grid_time_to_beat(const az_track_snapshot *s, double time, double *out) {
    int rc = st_grid_available(s);
    if (rc != AZ_OK) return rc;
    struct st_grid_bracket bracket = {0, s->beat_count - 1};
    int end;
    rc = st_grid_extent(s->beats, bracket, time, &end);
    if (rc != AZ_OK) return rc;
    if (end == AZ_TIME_EQUAL) {
        *out = bracket.hi;
        return AZ_OK;
    }
    while (bracket.hi - bracket.lo > 1) {
        rc = st_grid_bisect(s->beats, time, &bracket);
        if (rc != AZ_OK) return rc;
    }
    return st_grid_interpolate(s->beats, bracket, time, out);
}
static inline int st_time_to_beat(const az_track_snapshot *s, double time, double *out) {
    if (!s || st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION || !out || !isfinite(time)) return AZ_INVALID;
    return st_grid_time_to_beat(s, time, out);
}
static inline int st_grid_beat_to_time(const az_track_snapshot *s, double beat, double *out) {
    if (beat < 0 || beat > s->beat_count - 1) return AZ_NOT_FOUND;
    uint32_t i = (uint32_t) beat;
    uint32_t lo = i + 1 < s->beat_count ? i : i - 1;
    double first = s->beats[lo].time_s, last = s->beats[lo + 1].time_s;
    double span = last - first;
    int order = compare_timestamps(last, first, 1, 1000, 1);
    if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER || !isfinite(span)) return AZ_FAILED;
    double value = i == lo ? first + (beat - i) * span : last;
    if (!isfinite(value)) return AZ_FAILED;
    *out = value;
    return AZ_OK;
}
static inline int st_beat_to_time(const az_track_snapshot *s, double beat, double *out) {
    if (!s || st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION || !out || !isfinite(beat)) return AZ_INVALID;
    int rc = st_grid_available(s);
    return rc == AZ_OK ? st_grid_beat_to_time(s, beat, out) : rc;
}
static inline void st_interval_beats(const az_track_snapshot *s, double a, double b, int source_order, az_time_interval *value) {
    double first, last;
    if (st_time_to_beat(s, a, &first) != AZ_OK || st_time_to_beat(s, b, &last) != AZ_OK) return;
    value->beats = source_order == AZ_TIME_EQUAL ? 0 : last - first;
    value->valid |= AZ_INTERVAL_BEATS;
}
static inline void st_interval_playback(double rate, int source_order, az_time_interval *value) {
    double playback = rate > 0 ? value->source_s / rate : 0;
    /* FP_ZERO detects representation underflow after division; source ordering
     * was already established on its millisecond clock. */
    if (rate > 0 && isfinite(playback) && (source_order == AZ_TIME_EQUAL || fpclassify(playback) != FP_ZERO)) {
        value->playback_rate = rate;
        value->playback_s = playback;
        value->valid |= AZ_INTERVAL_PLAYBACK;
    }
}
static inline int st_source_interval_order(double a, double b) {
    int start = compare_timestamps(a, 0.0, 1, 1000, 1);
    int source = compare_timestamps(b, a, 1, 1000, 1);
    if (start == AZ_TIME_INVALID || source == AZ_TIME_INVALID || start == AZ_TIME_BEFORE || source == AZ_TIME_BEFORE) return AZ_TIME_INVALID;
    return source;
}
static inline int st_interval_output_valid(const az_time_interval *out, double rate) {
    return out && st_record_size(out) >= sizeof *out && isfinite(rate) && rate >= 0;
}
static inline int st_interval(const az_track_snapshot *s, double a, double b, double rate, az_time_interval *out) {
    if (s && (st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION)) return AZ_INVALID;
    if (!st_interval_output_valid(out, rate)) return AZ_INVALID;
    int order = st_source_interval_order(a, b);
    if (order == AZ_TIME_INVALID) return AZ_INVALID;
    az_time_interval value = {0};
    value.size = out->size;
    value.version = AZ_DATA_VERSION;
    value.valid = AZ_INTERVAL_SOURCE;
    value.source_s = order == AZ_TIME_EQUAL ? 0 : b - a;
    st_interval_beats(s, a, b, order, &value);
    st_interval_playback(rate, order, &value);
    memcpy(out, &value, sizeof value);
    return AZ_OK;
}
#endif

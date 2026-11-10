/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded, allocation-free ANLZ parsing and grid timing. No native pointers or I/O. */
#ifndef AZMOD_SDK_ANALYSIS_H
#define AZMOD_SDK_ANALYSIS_H
#include "../common/aztimestamp.h"
#include "azmod-mod.h"
#include "hc_anlz.h"
#include <math.h>
#include <string.h>
#define ST_BEATS 32768
#define ST_CUES 256
#define ST_PHRASES 1024
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
/* Fixed output UTF-16BE decoder. Surrogate pairs are preserved, invalid units use U+FFFD. */
static inline void st_comment(const uint8_t *p, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = hc_be16(p + i);
        if (!c) break;
        if (c >= 0xd800 && c <= 0xdbff && i + 3 < n) {
            uint32_t lo = hc_be16(p + i + 2);
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + lo - 0xdc00;
                i += 2;
            } else c = 0xfffd;
        } else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        unsigned len = c < 0x80 ? 1 : c < 0x800 ? 2
                                  : c < 0x10000 ? 3
                                                : 4;
        if (k + len >= cap) break;
        if (len == 1) out[k++] = (char) c;
        else {
            out[k++] = (char) ((len == 2 ? 0xc0 : len == 3 ? 0xe0
                                                           : 0xf0) |
                               (c >> (6 * (len - 1))));
            for (unsigned j = len - 1; j; j--) out[k++] = (char) (0x80 | ((c >> (6 * (j - 1))) & 63));
        }
    }
    if (cap) out[k] = 0;
}
static inline int st_cue_list(struct st_data *d, const uint8_t *t, size_t h, size_t n, int co2, const uint8_t **origins) {
    if (h < (co2 ? 0x14u : 0x18u)) return AZ_FAILED;
    uint32_t kind = hc_be32(t + 12), count = hc_be16(t + (co2 ? 16 : 18));
    if (kind > 1 || count > ST_CUES) return AZ_FAILED;
    size_t off = h;
    for (uint32_t i = 0; i < count; i++) {
        if (n - off < 12) return AZ_FAILED;
        const uint8_t *e = t + off;
        size_t en = hc_be32(e + 8);
        if (en > n - off || en < 0x28u || hc_be32(e + 4) != (co2 ? 0x10u : 0x1cu) ||
            memcmp(e, co2 ? "PCP2" : "PCPT", 4)) return AZ_FAILED;
        uint32_t slot = hc_be32(e + 12), type = e[co2 ? 0x10 : 0x1c];
        if ((kind && (slot < 1 || slot > 8)) || (type != 1 && type != 2)) return AZ_FAILED;
        uint32_t a = hc_be32(e + (co2 ? 0x14 : 0x20));
        uint32_t b = hc_be32(e + (co2 ? 0x18 : 0x24));
        int order = compare_timestamps(b, a, 1000, 1000, 1);
        if (a == UINT32_MAX || (type == 2 && (b == UINT32_MAX || order == AZ_TIME_INVALID || order != AZ_TIME_AFTER))) return AZ_FAILED;
        az_cue_entry c = {0};
        c.size = sizeof c;
        c.version = AZ_DATA_VERSION;
        c.classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
        c.entry_id = kind ? slot : d->view.cue_count + 1;
        c.slot = slot;
        c.type = type;
        c.valid = AZ_CUE_START | AZ_CUE_SAVED;
        c.start_s = a / 1000.0;
        c.saved = 1;
        c.modified = c.auto_active = -1;
        if (co2 && !kind) {
            c.color_id = e[0x1c];
            c.valid |= AZ_CUE_COLOR_ID;
        }
        if (type == 2) {
            c.valid |= AZ_CUE_END;
            c.end_s = b / 1000.0;
            if (!co2) {
                uint32_t status = hc_be32(e + 0x10);
                if (status == 0 || status == 1 || status == 4) {
                    c.valid |= AZ_CUE_AUTO_ACTIVE;
                    c.auto_active = status == 4;
                }
            }
        }
        /* The optional comment-length field is absent in canonical short PCP2
         * entries. Its presence is established by the entry extent, not a cue value. */
        if (co2 && en >= 0x2c) {
            size_t nc = hc_be32(e + 0x28);
            if (nc > en - 0x2c || (nc & 1)) return AZ_FAILED;
            if (nc) {
                st_comment(e + 0x2c, nc, c.comment, sizeof c.comment);
                c.valid |= AZ_CUE_COMMENT;
            }
            if (kind && en - 0x2c - nc >= 4) {
                const uint8_t *rgb = e + 0x2c + nc;
                c.rgb = (uint32_t) rgb[1] << 16 | (uint32_t) rgb[2] << 8 | rgb[3];
                c.valid |= AZ_CUE_COLOR;
            }
        }
        int at = (int) d->view.cue_count;
        if (kind)
            for (uint32_t k = 0; k < d->view.cue_count; k++)
                if (d->cues[k].classification == AZ_CUE_HOT && d->cues[k].slot == slot) {
                    at = (int) k;
                    break;
                }
        if (at >= ST_CUES) return AZ_LIMIT;
        d->cues[at] = c;
        if (d->cue_ticks) {
            d->cue_ticks[2 * at] = a;
            d->cue_ticks[2 * at + 1] = type == AZ_CUE_LOOP ? b : 0;
        }
        if (origins) origins[at] = e;
        if ((uint32_t) at == d->view.cue_count) d->view.cue_count++;
        off += en;
    }
    return AZ_OK;
}
/* PCP2 omits PCPT's saved Active Loop status. Carry that property across only an
 * exact, unique saved-loop counterpart; do not turn positions into entry identity.
 * Malformed/oversized legacy annotations leave the extended data authoritative. */
static inline void st_enrich_active(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                    const unsigned co2[2], const unsigned cob[2]) {
    struct st_saved_active {
        uint32_t kind, slot, start_ms, end_ms;
        int active;
    } saved[ST_CUES * 2];
    unsigned saved_count = 0, entries = 0;
    for (int f = 0; f < 2; f++) {
        const uint8_t *b = buffers[f];
        if (!b) continue;
        for (size_t off = hc_be32(b + 4); off < sizes[f]; off += hc_be32(b + off + 8)) {
            const uint8_t *t = b + off;
            if (memcmp(t, "PCOB", 4)) continue;
            uint32_t kind = hc_be32(t + 12);
            unsigned bit = 1u << kind;
            if (!((co2[0] | co2[1]) & bit) || (!kind && f && (cob[0] & bit))) continue;
            size_t h = hc_be32(t + 4), n = hc_be32(t + 8);
            if (h < 24) return;
            uint32_t count = hc_be16(t + 18);
            if (count > ST_CUES || count > ST_CUES * 2u - entries) return;
            entries += count;
            size_t pos = h;
            for (uint32_t i = 0; i < count; i++) {
                if (n - pos < 12) return;
                const uint8_t *e = t + pos;
                size_t en = hc_be32(e + 8);
                if (memcmp(e, "PCPT", 4) || en < 0x28 || en > n - pos || hc_be32(e + 4) != 0x1c) return;
                uint32_t slot = hc_be32(e + 12), type = e[0x1c];
                uint32_t start = hc_be32(e + 0x20), end = hc_be32(e + 0x24);
                int order = compare_timestamps(end, start, 1000, 1000, 1);
                if ((kind && (slot < 1 || slot > 8)) || (type != 1 && type != 2) || start == UINT32_MAX ||
                    (type == 2 && (end == UINT32_MAX || order == AZ_TIME_INVALID || order != AZ_TIME_AFTER))) return;
                if (type == 2) {
                    uint32_t status = hc_be32(e + 0x10);
                    saved[saved_count++] = (struct st_saved_active) {kind, slot, start, end,
                                                                     status == 4 ? 1 : status == 0 || status == 1 ? 0
                                                                                                                  : -1};
                }
                pos += en;
            }
        }
    }
    for (uint32_t i = 0; i < d->view.cue_count; i++) {
        az_cue_entry *c = &d->cues[i];
        unsigned kind = c->classification == AZ_CUE_HOT, bit = 1u << kind;
        /* These parser outputs originate in u32 milliseconds. Compare on
         * that integer clock so adjacent ticks cannot collapse at a floating
         * representation boundary. */
        uint64_t start_ms = (uint64_t) llround(c->start_s * 1000.0);
        uint64_t end_ms = (uint64_t) llround(c->end_s * 1000.0);
        if (c->type != AZ_CUE_LOOP || !((co2[0] | co2[1]) & bit)) continue;
        unsigned matches = 0;
        int active = -1;
        for (unsigned j = 0; j < saved_count; j++) {
            if (saved[j].kind != kind || (kind && saved[j].slot != c->slot)) continue;
            int start_order = compare_timestamps(saved[j].start_ms, start_ms, 1000, 1000, 1);
            int end_order = compare_timestamps(saved[j].end_ms, end_ms, 1000, 1000, 1);
            if (start_order == AZ_TIME_INVALID || end_order == AZ_TIME_INVALID) continue;
            if (start_order == AZ_TIME_EQUAL && end_order == AZ_TIME_EQUAL) {
                matches++;
                active = saved[j].active;
            }
        }
        if (matches == 1 && active >= 0) {
            c->valid |= AZ_CUE_AUTO_ACTIVE;
            c->auto_active = active;
        }
    }
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
static inline int st_section(struct st_data *d, const uint8_t *t, size_t h, size_t n, uint32_t mask) {
    if ((mask & AZ_DATA_GRID) && !memcmp(t, "PQTZ", 4) && !d->view.beat_count) {
        if (h < 24) return AZ_FAILED;
        uint32_t count = hc_be32(t + 20);
        if (count > ST_BEATS || count > (n - h) / 8) return AZ_FAILED;
        unsigned encoded = hc_be16(t + 18);
        d->grid_offset_ms = encoded < 0x8000u ? (int) encoded : (int) encoded - 0x10000;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = t + h + i * 8;
            uint32_t milliseconds = hc_be32(e + 4);
            double time = milliseconds / 1000.0;
            if (i) {
                int order = compare_timestamps(milliseconds, hc_be32(e - 4), 1000, 1000, 1);
                if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return AZ_FAILED;
            }
            d->beats[i] = (az_beat) {time, hc_be16(e + 2) / 100.0, hc_be16(e), 0};
            if (d->beat_ticks) d->beat_ticks[i] = milliseconds;
        }
        d->view.beat_count = count;
        d->view.grid_info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    }
    if ((mask & AZ_DATA_PHRASES) && !memcmp(t, "PSSI", 4) && !d->view.phrase_count) {
        if (h < 32) return AZ_FAILED;
        uint32_t version = hc_be16(t + 12);
        /* The old 32-bit value 24 is version 0 plus a 16-bit stride. Version 1
         * retains these fields; unfamiliar versions are not parsed as version 0. */
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
        int masked = hc_be16(t + 18) > 20;
        uint16_t mood = st_ps16(t, 18, (uint16_t) count, masked), end = st_ps16(t, 26, (uint16_t) count, masked);
        if (mood < 1 || mood > 3) return AZ_FAILED;
        for (uint32_t i = 0; i < count; i++) {
            size_t off = h + i * stride;
            uint16_t start = st_ps16(t, off + 2, (uint16_t) count, masked);
            uint16_t next = i + 1 < count ? st_ps16(t, off + stride + 2, (uint16_t) count, masked) : end;
            if (!start || next <= start) return AZ_FAILED;
            d->phrases[i] = (az_phrase) {start - 1u, next - 1u, st_ps16(t, off + 4, (uint16_t) count, masked), mood, 0, 0};
        }
        d->view.phrase_count = count;
        d->view.phrases_info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    }
    if ((mask & AZ_DATA_WAVEFORM) && !memcmp(t, "PWAV", 4)) {
        if (h < 20) return AZ_FAILED;
        uint32_t count = hc_be32(t + 12);
        if (count > ST_WAVE || count > n - h) return AZ_LIMIT;
        memcpy(d->wave, t + h, count);
        d->view.waveform.sample_count = count;
        d->view.waveform.representation = AZ_WAVE_PREVIEW;
        d->view.waveform_info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    }
    return AZ_OK;
}
/* Validate the whole container before exposing any component. Extended lists take precedence
 * per classification, even when an extended list is empty. DAT grid, EXT phrases. */
static inline int st_parse_origins(struct st_data *d, const uint8_t *const buffers[2], const size_t sizes[2],
                                   uint32_t mask, unsigned cue_kinds, const uint8_t **origins) {
    unsigned co2[2] = {0}, cob[2] = {0};
    for (int f = 0; f < 2; f++) {
        const uint8_t *b = buffers[f];
        size_t n = sizes[f];
        if (!b && !n) continue;
        if (!b || n < 12 || n > ST_FILE_MAX || memcmp(b, "PMAI", 4)) return AZ_FAILED;
        size_t off = hc_be32(b + 4);
        if (off < 12 || off > n || hc_be32(b + 8) != n) return AZ_FAILED;
        while (off < n) {
            if (n - off < 12) return AZ_FAILED;
            const uint8_t *t = b + off;
            size_t h = hc_be32(t + 4), len = hc_be32(t + 8);
            if (h < 12 || h > len || len > n - off) return AZ_FAILED;
            if (cue_kinds && (!memcmp(t, "PCO2", 4) || !memcmp(t, "PCOB", 4))) {
                if (h < 16 || hc_be32(t + 12) > 1) return AZ_FAILED;
                unsigned bit = 1u << hc_be32(t + 12);
                if (cue_kinds & bit) {
                    if (t[3] == '2') co2[f] |= bit;
                    else cob[f] |= bit;
                }
            }
            int rc = st_section(d, t, h, len, mask);
            if (rc) return rc;
            off += len;
        }
    }
    if (mask & AZ_DATA_CUES) {
        for (int f = 0; f < 2; f++) {
            const uint8_t *b = buffers[f];
            size_t n = sizes[f];
            if (!b) continue;
            for (size_t off = hc_be32(b + 4); off < n; off += hc_be32(b + off + 8)) {
                const uint8_t *t = b + off;
                if (memcmp(t, "PCO2", 4) && memcmp(t, "PCOB", 4)) continue;
                unsigned bit = 1u << hc_be32(t + 12);
                if (!(cue_kinds & bit)) continue;
                /* Legacy hot slots can be split across DAT and EXT. Memory lists retain
                 * DAT precedence: entry positions alone are not a stable identity. */
                int selected = (co2[0] | co2[1]) & bit ? (t[3] == '2' && (f == 1 || !(co2[1] & bit)))
                                                       : (t[3] == 'B' && (bit == 2u || f == 0 || !(cob[0] & bit)));
                if (selected) {
                    int rc = st_cue_list(d, t, hc_be32(t + 4), hc_be32(t + 8), t[3] == '2', origins);
                    if (rc) return rc;
                }
            }
        }
        d->cue_lists = co2[0] | co2[1] | cob[0] | cob[1];
        if (d->cue_lists) d->view.cues_info.availability = d->view.cue_count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
        st_enrich_active(d, buffers, sizes, co2, cob);
    }
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
static inline int st_time_to_beat(const az_track_snapshot *s, double time, double *out) {
    if (!s || st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION || !out || !isfinite(time)) return AZ_INVALID;
    if (s->beat_count && !s->beats) return AZ_INVALID;
    if (s->grid_info.availability != AZ_AVAIL_READY || s->beat_count < 2) return AZ_UNSUPPORTED;
    const az_beat *b = s->beats;
    uint32_t lo = 0, hi = s->beat_count - 1;
    int extent = compare_timestamps(b[hi].time_s, b[lo].time_s, 1, 1000, 1);
    int start = compare_timestamps(time, b[lo].time_s, 1, 1000, 1);
    int end = compare_timestamps(time, b[hi].time_s, 1, 1000, 1);
    if (extent == AZ_TIME_INVALID || extent != AZ_TIME_AFTER || start == AZ_TIME_INVALID || end == AZ_TIME_INVALID) return AZ_FAILED;
    if (start == AZ_TIME_BEFORE || end == AZ_TIME_AFTER) return AZ_NOT_FOUND;
    if (end == AZ_TIME_EQUAL) {
        *out = hi;
        return AZ_OK;
    }
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        int lower = compare_timestamps(b[mid].time_s, b[lo].time_s, 1, 1000, 1);
        int upper = compare_timestamps(b[mid].time_s, b[hi].time_s, 1, 1000, 1);
        int target = compare_timestamps(b[mid].time_s, time, 1, 1000, 1);
        if (lower == AZ_TIME_INVALID || upper == AZ_TIME_INVALID || target == AZ_TIME_INVALID ||
            lower != AZ_TIME_AFTER || upper != AZ_TIME_BEFORE) return AZ_FAILED;
        if (target == AZ_TIME_BEFORE || target == AZ_TIME_EQUAL) lo = mid;
        else hi = mid;
    }
    double span = b[hi].time_s - b[lo].time_s;
    int span_order = compare_timestamps(span, 0.0, 1, 1000, 1);
    if (span_order == AZ_TIME_INVALID || span_order != AZ_TIME_AFTER) return AZ_FAILED;
    double value = lo + (time - b[lo].time_s) / span;
    if (!isfinite(value)) return AZ_FAILED;
    *out = value;
    return AZ_OK;
}
static inline int st_beat_to_time(const az_track_snapshot *s, double beat, double *out) {
    if (!s || st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION || !out || !isfinite(beat)) return AZ_INVALID;
    if (s->beat_count && !s->beats) return AZ_INVALID;
    if (s->grid_info.availability != AZ_AVAIL_READY || s->beat_count < 2) return AZ_UNSUPPORTED;
    if (beat < 0 || beat > s->beat_count - 1) return AZ_NOT_FOUND;
    uint32_t i = (uint32_t) beat;
    uint32_t lo = i + 1 < s->beat_count ? i : i - 1;
    double first = s->beats[lo].time_s, last = s->beats[lo + 1].time_s;
    double span = last - first;
    int span_order = compare_timestamps(last, first, 1, 1000, 1);
    if (span_order == AZ_TIME_INVALID || span_order != AZ_TIME_AFTER || !isfinite(span)) return AZ_FAILED;
    double value = i == lo ? first + (beat - i) * span : last;
    if (!isfinite(value)) return AZ_FAILED;
    *out = value;
    return AZ_OK;
}
static inline int st_interval(const az_track_snapshot *s, double a, double b, double rate, az_time_interval *out) {
    if (s && (st_record_size(s) < sizeof *s || s->version != AZ_DATA_VERSION)) return AZ_INVALID;
    if (!out || st_record_size(out) < sizeof *out || !isfinite(rate) || rate < 0) return AZ_INVALID;
    int start_order = compare_timestamps(a, 0.0, 1, 1000, 1);
    int source_order = compare_timestamps(b, a, 1, 1000, 1);
    if (start_order == AZ_TIME_INVALID || source_order == AZ_TIME_INVALID ||
        start_order == AZ_TIME_BEFORE || source_order == AZ_TIME_BEFORE) return AZ_INVALID;
    az_time_interval v = {0};
    v.size = out->size;
    v.version = AZ_DATA_VERSION;
    v.valid = AZ_INTERVAL_SOURCE;
    v.source_s = source_order == AZ_TIME_EQUAL ? 0 : b - a;
    double x, y;
    if (st_time_to_beat(s, a, &x) == AZ_OK && st_time_to_beat(s, b, &y) == AZ_OK) {
        v.beats = source_order == AZ_TIME_EQUAL ? 0 : y - x;
        v.valid |= AZ_INTERVAL_BEATS;
    }
    double playback = rate > 0 ? v.source_s / rate : 0;
    /* FP_ZERO detects representation underflow after division; timestamp
     * equality and ordering were already decided by compare_timestamps. */
    if (rate > 0 && isfinite(playback) && (source_order == AZ_TIME_EQUAL || fpclassify(playback) != FP_ZERO)) {
        v.playback_rate = rate;
        v.playback_s = playback;
        v.valid |= AZ_INTERVAL_PLAYBACK;
    }
    memcpy(out, &v, sizeof v);
    return AZ_OK;
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded, format-only ANLZ edit. Unknown tags and header bytes survive verbatim. */
#ifndef SDK_ANALYSIS_WRITE_H
#define SDK_ANALYSIS_WRITE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define SA_FILE_MAX (16u * 1024u * 1024u)
static inline uint32_t sa_u32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}
static inline unsigned sa_u16(const uint8_t *p) { return (unsigned) p[0] << 8 | p[1]; }
static inline void sa_put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
/* A checked chunk length; zero denotes invalid framing. */
static inline size_t sa_chunk_length(const uint8_t *b, size_t remaining) {
    if (remaining < 12) return 0;
    size_t header = sa_u32(b + 4), length = sa_u32(b + 8);
    return header >= 12 && header <= length && length <= remaining ? length : 0;
}
static inline int sa_container_header(const uint8_t *b, size_t n) {
    return n >= 12 && n <= SA_FILE_MAX && !memcmp(b, "PMAI", 4) && sa_u32(b + 8) == n;
}
/* 1 = framed, 0 = invalid. A null/zero file is an absent container. */
static inline int sa_framed(const uint8_t *b, size_t n) {
    if (!b) return !n;
    if (!sa_container_header(b, n)) return 0;
    size_t at = sa_u32(b + 4);
    if (at < 12 || at > n) return 0;
    while (at < n) {
        size_t length = sa_chunk_length(b + at, n - at);
        if (!length) return 0;
        at += length;
    }
    return 1;
}
static inline int sa_grid_count(const uint8_t *tag, size_t header, size_t length) {
    if (header < 24) return -1;
    uint32_t count = sa_u32(tag + 20);
    return count <= 32768 && count <= (length - header) / 8 ? (int) count : -1;
}
static inline int sa_phrase_count(const uint8_t *tag, size_t header, size_t length) {
    if (header < 32 || sa_u16(tag + 12) > 1 || sa_u16(tag + 14) != 24) return -1;
    unsigned count = sa_u16(tag + 16);
    return count <= 1024 && count <= (length - header) / 24 ? (int) count : -1;
}
static inline int sa_vocal_scores_valid(const uint8_t *scores, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        if (scores[i] > 4) return 0;
    return 1;
}
static inline int sa_vocal_header_valid(const uint8_t *tag, size_t header, size_t length) {
    return header == 24 && length >= 24 && sa_u16(tag + 18) == 1 && sa_u32(tag + 12) &&
           sa_u16(tag + 16) && sa_u32(tag + 12) <= sa_u16(tag + 16);
}
static inline int sa_vocal_count(const uint8_t *tag, size_t header, size_t length) {
    if (!sa_vocal_header_valid(tag, header, length)) return -1;
    uint32_t count = sa_u32(tag + 20);
    if (count > 262144 || count != length - header) return -1;
    return sa_vocal_scores_valid(tag + header, count) ? (int) count : -1;
}
static inline int sa_supported_tag(const char *tag) {
    return tag && (!memcmp(tag, "PQTZ", 4) || !memcmp(tag, "PSSI", 4) ||
                   !memcmp(tag, "PVDI", 4) || !memcmp(tag, "AZVD", 4));
}
static inline int sa_chunk_count(const uint8_t *tag) {
    size_t header = sa_u32(tag + 4), length = sa_u32(tag + 8);
    if (!memcmp(tag, "PQTZ", 4)) return sa_grid_count(tag, header, length);
    if (!memcmp(tag, "PSSI", 4)) return sa_phrase_count(tag, header, length);
    if (!memcmp(tag, "PVDI", 4)) return sa_vocal_count(tag, header, length);
    /* AZVD provenance can be refreshed only alongside new PVDI. */
    return header == 32 && length == 32 ? 0 : -1;
}
/* Only absent or explicitly empty PQTZ/PSSI may be replaced. Unknown versions,
 * malformed records and duplicate tags refuse the edit. Returns -1 on refusal. */
static inline int sa_tag_count(const uint8_t *b, size_t n, const char *tag) {
    if (!sa_supported_tag(tag) || !sa_framed(b, n)) return -1;
    if (!b) return 0;
    int result = 0, found = 0;
    for (size_t at = sa_u32(b + 4); at < n; at += sa_u32(b + at + 8)) {
        if (memcmp(b + at, tag, 4)) continue;
        if (found++) return -1;
        result = sa_chunk_count(b + at);
        if (result < 0) return -1;
    }
    return result;
}
static inline size_t sa_replaced_size(const uint8_t *file, size_t n, const uint8_t *tag) {
    if (!file) return 0;
    for (size_t at = sa_u32(file + 4); at < n; at += sa_u32(file + at + 8))
        if (!memcmp(file + at, tag, 4)) return sa_u32(file + at + 8);
    return 0;
}
static inline size_t sa_copy_preserved(uint8_t *result, const uint8_t *file, size_t n, const uint8_t *tag, size_t header) {
    if (!file) {
        memcpy(result, "PMAI", 4);
        sa_put32(result + 4, (uint32_t) header);
        return header;
    }
    memcpy(result, file, header);
    size_t dest = header;
    for (size_t at = header; at < n; at += sa_u32(file + at + 8)) {
        size_t length = sa_u32(file + at + 8);
        if (!memcmp(file + at, tag, 4)) continue;
        memcpy(result + dest, file + at, length);
        dest += length;
    }
    return dest;
}
static inline int sa_merge_tag_valid(const uint8_t *tag, size_t n) {
    return tag && n >= 12 && n <= SA_FILE_MAX && sa_u32(tag + 8) == n &&
           sa_u32(tag + 4) >= 12 && sa_u32(tag + 4) <= n;
}
/* The caller validates the supplied tag's rows before merging. */
static inline uint8_t *sa_merge(const uint8_t *file, size_t n, const uint8_t *tag, size_t tn, size_t *out_n) {
    if (!sa_merge_tag_valid(tag, tn) || sa_tag_count(file, n, (const char *) tag) != 0) return NULL;
    size_t header = file ? sa_u32(file + 4) : 28;
    size_t size = (file ? n : header) - sa_replaced_size(file, n, tag) + tn;
    if (size > SA_FILE_MAX) return NULL;
    uint8_t *result = calloc(1, size);
    if (!result) return NULL;
    size_t dest = sa_copy_preserved(result, file, n, tag, header);
    memcpy(result + dest, tag, tn);
    sa_put32(result + 8, (uint32_t) size);
    *out_n = size;
    return result;
}
#endif

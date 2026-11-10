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
/* 1 = framed, 0 = invalid. A null/zero file is an absent container. */
static inline int sa_framed(const uint8_t *b, size_t n) {
    if (!b) return !n;
    if (n < 12 || n > SA_FILE_MAX || memcmp(b, "PMAI", 4) || sa_u32(b + 8) != n) return 0;
    size_t at = sa_u32(b + 4);
    if (at < 12 || at > n) return 0;
    while (at < n) {
        if (n - at < 12) return 0;
        size_t h = sa_u32(b + at + 4), len = sa_u32(b + at + 8);
        if (h < 12 || h > len || len > n - at) return 0;
        at += len;
    }
    return 1;
}
/* Only absent or explicitly empty PQTZ/PSSI may be replaced. Unknown versions,
 * malformed records and duplicate tags refuse the edit. Returns -1 on refusal. */
static inline int sa_tag_count(const uint8_t *b, size_t n, const char *tag) {
    if (!tag || (memcmp(tag, "PQTZ", 4) && memcmp(tag, "PSSI", 4))) return -1;
    if (!sa_framed(b, n)) return -1;
    int result = 0, found = 0;
    if (!b) return 0;
    for (size_t at = sa_u32(b + 4); at < n; at += sa_u32(b + at + 8)) {
        if (memcmp(b + at, tag, 4)) continue;
        if (found++) return -1;
        size_t h = sa_u32(b + at + 4), len = sa_u32(b + at + 8);
        if (!memcmp(tag, "PQTZ", 4)) {
            if (h < 24) return -1;
            result = (int) sa_u32(b + at + 20);
            if (result < 0 || (unsigned) result > 32768 || (unsigned) result > (len - h) / 8) return -1;
        } else if (!memcmp(tag, "PSSI", 4)) {
            if (h < 32 || sa_u16(b + at + 12) > 1 || sa_u16(b + at + 14) != 24) return -1;
            result = (int) sa_u16(b + at + 16);
            if (result > 1024 || (unsigned) result > (len - h) / 24) return -1;
        } else return -1;
    }
    return result;
}
/* The caller validates the supplied tag's rows before merging. */
static inline uint8_t *sa_merge(const uint8_t *file, size_t n, const uint8_t *tag, size_t tn, size_t *out_n) {
    if (!tag || tn < 12 || tn > SA_FILE_MAX || sa_u32(tag + 8) != tn ||
        sa_u32(tag + 4) < 12 || sa_u32(tag + 4) > tn || sa_tag_count(file, n, (const char *) tag) != 0) return NULL;
    size_t old = 0, header = file ? sa_u32(file + 4) : 28;
    if (file)
        for (size_t at = header; at < n; at += sa_u32(file + at + 8))
            if (!memcmp(file + at, tag, 4)) old = sa_u32(file + at + 8);
    size_t size = (file ? n : header) - old + tn;
    if (size > SA_FILE_MAX) return NULL;
    uint8_t *result = calloc(1, size);
    if (!result) return NULL;
    if (file) memcpy(result, file, header);
    else {
        memcpy(result, "PMAI", 4);
        sa_put32(result + 4, (uint32_t) header);
    }
    size_t dest = header;
    if (file)
        for (size_t at = header; at < n; at += sa_u32(file + at + 8)) {
            size_t len = sa_u32(file + at + 8);
            if (!memcmp(file + at, tag, 4)) continue;
            memcpy(result + dest, file + at, len);
            dest += len;
        }
    memcpy(result + dest, tag, tn);
    sa_put32(result + 8, (uint32_t) size);
    *out_n = size;
    return result;
}
#endif

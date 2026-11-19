/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUETEXT_H
#define AZCUETEXT_H
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
static inline unsigned azcue_utf8_width(unsigned char first) {
    if (first < 0x80) return 1;
    if (first >= 0xc2 && first <= 0xdf) return 2;
    if (first >= 0xe0 && first <= 0xef) return 3;
    return first >= 0xf0 && first <= 0xf4 ? 4 : 0;
}
static inline bool azcue_utf8_scalar(const char *text, unsigned width) {
    static const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    uint32_t code = (unsigned char) text[0] & (width == 1 ? 0x7f : (1u << (7 - width)) - 1);
    for (unsigned i = 1; i < width; ++i) {
        unsigned char next = (unsigned char) text[i];
        if ((next & 0xc0) != 0x80) return false;
        code = (code << 6) | (next & 0x3f);
    }
    return code >= minimum[width] && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
}
static inline void azcue_copy_utf8_prefix(char out[128], const char *text) {
    unsigned at = 0;
    while (at < 127 && text[at]) {
        unsigned width = azcue_utf8_width((unsigned char) text[at]);
        if (!width || width > 127 - at || !azcue_utf8_scalar(text + at, width)) break;
        memcpy(out + at, text + at, width);
        at += width;
    }
    out[at] = 0;
}
#endif

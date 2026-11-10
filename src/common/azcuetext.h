/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUETEXT_H
#define AZCUETEXT_H
#include <stdint.h>
#include <cstring>
static inline void azcue_copy_utf8_prefix(char (&out)[128], const char *text) {
    unsigned at = 0;
    while (at < 127 && text[at]) {
        unsigned char first = static_cast<unsigned char>(text[at]);
        unsigned width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                        : first >= 0xe0 && first <= 0xef   ? 3
                                        : first >= 0xf0 && first <= 0xf4   ? 4
                                                                           : 0;
        if (!width || width > 127 - at) break;
        uint32_t code = first & (width == 1 ? 0x7f : (1u << (7 - width)) - 1);
        bool valid = true;
        for (unsigned i = 1; i < width; ++i) {
            unsigned char next = static_cast<unsigned char>(text[at + i]);
            if ((next & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (next & 0x3f);
        }
        if (!valid || (width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
            (width == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) break;
        std::memcpy(out + at, text + at, width);
        at += width;
    }
    out[at] = 0;
}
#endif

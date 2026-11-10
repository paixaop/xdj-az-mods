/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_ANALYSIS_ORIGIN_H
#define AZSTATUS_ANALYSIS_ORIGIN_H
#include <stddef.h>
#include <stdint.h>
#include "azstatus_file_input.h"
enum {
    AZN_FILE_PQTZ = 0x5051545a,
    AZN_FILE_PQT2 = 0x50515432,
    AZN_FILE_PSSI = 0x50535349,
    AZN_FILE_MAP_TAGS = 3
};
/* The original typed atom owns the encoded bytes [first,total). Header and
 * total are copied from that atom's validated original framing. A producer
 * must prove the retained slice, including its encoded/decrypted boundary. */
struct azn_file_tag {
    uint32_t name, header, total, first;
    char digest[65];
};
struct azn_file_map_callbacks {
    uint32_t size, version;
    uint64_t (*begin)(const char *root, size_t root_n, const uint16_t *audio, size_t audio_n,
                      const uint16_t *analysis, size_t analysis_n);
    void (*complete)(uint64_t read, const struct azn_file_tag *tags, size_t count,
                     uint64_t tokens[AZN_FILE_MAP_TAGS]);
    void (*hold)(const uint64_t *tokens, size_t count, int acquire);
    /* Synchronous borrowed actual native input fd. Returns DAT1 or EXT2. */
    unsigned (*opened)(uint64_t read, int descriptor);
    const struct azn_file_input_api *(*inputs)(void);
};
#endif

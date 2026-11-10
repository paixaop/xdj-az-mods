/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_CUE_ORIGIN_H
#define AZSTATUS_CUE_ORIGIN_H
#include <stddef.h>
#include <stdint.h>
#include "azstatus_file_input.h"
/* Private, copied FILE-import evidence. The original PcptAtomData and
 * Pcp2AtomData own bytes [14,total) of their respective physical entries.
 * Each entry type occupies its named member. Zero total means absent. */
struct azn_file_atom {
    uint32_t header, total;
    char digest[65];
};
struct azn_file_cue_row {
    struct azn_file_atom pcpt, pcp2;
};
enum {
    AZN_FILE_CUE_MAX = 18,
    AZN_FILE_CUE_BYTES = 8192
};
/* Version 2 requires the owning fresh physical-input operation. Begin runs
 * before its original file open/read, with that input's authenticated paths.
 * A cache hit can reuse only its retained original input token. It cannot
 * mint a current edition from a new outer getter and equal cached atom bytes.
 * The caller owns strings until begin returns. Complete consumes the read
 * token even on failure (count zero). A row token is never an entry ID. */
struct azn_file_cue_callbacks {
    uint32_t size, version;
    uint64_t (*begin)(const char *root, size_t root_n, const uint16_t *audio, size_t audio_n,
                      const uint16_t *analysis, size_t analysis_n);
    void (*complete)(uint64_t read, const struct azn_file_cue_row *rows, size_t count,
                     uint64_t tokens[AZN_FILE_CUE_MAX]);
    /* Optional tail. Genuine retained Content ownership, balanced once for
     * each acquisition/retirement. Audio writers can call this callback.
     * It must not block, allocate, perform file work or invoke user code. */
    void (*hold)(const uint64_t *row_tokens, size_t count, int acquire);
    /* Synchronous borrowed actual native input fd. Returns DAT1 or EXT2. */
    unsigned (*opened)(uint64_t read, int descriptor);
    /* Common fresh physical inception, retained across getter families. */
    const struct azn_file_input_api *(*inputs)(void);
};
enum {
    AZN_FILE_SEND = 1,
    AZN_FILE_WIRE,
    AZN_FILE_SERVER,
    AZN_FILE_CLIENT
};
/* Optional synchronous observer. OPENING precedes the native fopen call,
 * OPENED lends its real fd until the callback returns. CACHED reports a tag
 * participant that has no fresh-input witness in this getter. */
enum {
    AZN_INPUT_OPENING = 1,
    AZN_INPUT_OPENED,
    AZN_INPUT_CACHED
};
typedef void (*azn_file_input_observer_fn)(unsigned stage, int descriptor);
/* Optional service reads on a live captured property only. An evicted,
 * unmatched, modified, duplicated or expired association returns zero. */
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_FILE_INPUT_H
#define AZSTATUS_FILE_INPUT_H
#include <stddef.h>
#include <stdint.h>
/* Common native FileData inception. Every participant names the actual
 * retained FileData/control incarnation, independent of getter family. */
struct azn_file_input_api {
    uint32_t size,version;
    /* A common physical opener can omit audio only when this exact mounted
     * analysis path resolves to one committed row. The worker rejects shared
     * paths; downstream load admission still verifies the full source/row. */
    uint64_t (*physical_begin)(const char *absolute_path,size_t bytes,const uint16_t *audio,size_t units);
    unsigned (*opened)(uint64_t read,int descriptor);
    /* Always consumes read. Success requires the original fresh parse result.
     * Returns one held reference, adopted by the original FileData owner. */
    uint64_t (*seal)(uint64_t read,int original_success);
    /* Balanced custody, atomic and nonblocking on native writers. */
    void (*hold)(const uint64_t *inputs,size_t count,int acquire);
    /* Original DAT and EXT witnesses, respectively. Zero means absent.
     * Returns a read consumed by the typed cue/map complete callback. */
    uint64_t (*join)(const uint64_t inputs[2]);
};
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/host/pvdi-windows-motion.h"
#include "../../src/common/azsha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify_unchanged(const uint8_t *output)
{
    for (size_t i = 0; i < PVDI_WINDOWS_MOTION_BYTES; ++i)
        assert(output[i] == 0xa5);
}

static void verify_arguments(const uint8_t *image, size_t size)
{
    size_t length = SIZE_MAX;
    uint8_t output[PVDI_WINDOWS_MOTION_BYTES];
    memset(output, 0xa5, sizeof output);
    assert(!pvdi_windows_motion_extract(image, size, 3, output, sizeof output, NULL));
    assert(!pvdi_windows_motion_extract(image, size, 3, NULL, sizeof output, &length));
    assert(length == 0);
    assert(!pvdi_windows_motion_extract(NULL, size, 3, output, sizeof output, &length));
    assert(!pvdi_windows_motion_extract(image, size, 4, output, sizeof output, &length));
    assert(!pvdi_windows_motion_extract(image, size - 1, 3, output, sizeof output, &length));
    assert(!pvdi_windows_motion_extract(image, size, 3, output, sizeof output - 1, &length));
    assert(length == 0);
    verify_unchanged(output);
}

struct patch { size_t offset, count; uint8_t data[4]; };

static void verify_unexpected_native(uint8_t *image, size_t size)
{
    /* Raw code addresses of the 7.2.20.0 fixture. Each mutation invalidates a
     * different source instruction or native operand used by the extractor. */
    const struct patch cases[] = {
        {0x13ad5a8, 1, {0}},                 /* Energy loop clears its accumulators. */
        {0x13aafb5, 4, {0}},                 /* Energy call window cannot be empty. */
        {0x13aafb5, 4, {0, 0, 1, 0}},        /* Energy window must fit the recipe. */
        {0x13ad5f1, 1, {0}},                 /* Horizontal half extraction. */
        {0x13ad5ed, 2, {0xc5, 1}},            /* Horizontal add reads initialized lanes at the expected width. */
        {0x13ad5c0, 1, {0}},                 /* Weighted energy requires a fused accumulation. */
        {0x13ad5d1, 1, {0xc1}},              /* Magnitude update must write its own accumulator. */
        {0x13ad565, 1, {0}},                 /* Epsilon must have a valid constant load instruction. */
        {0x13ad569, 4, {0xff, 0xff, 0xff, 0x7f}}, /* RIP displacement outside the data section. */
        {0x13aaff6, 1, {0}},                 /* Log requires pre-applied epsilon. */
        {0x13ab02b, 1, {0}},                 /* Flatness vector count. */
        {0x13ac210, 1, {0}},                 /* Negative lag load order. */
        {0x13ac22f, 1, {0}},                 /* Correlation vector count. */
        {0x13ac1ae, 1, {0}},                 /* Center is encoded by half the width. */
        {0x13ad93a, 4, {0}},                 /* Caller must branch to the sourced correlation helper. */
        {0x13ad917, 4, {0}},                 /* Last candidate precedes first. */
        {0x13ad94a, 4, {0}},                 /* Center denominator lies outside candidate array. */
        {0x13ad953, 1, {0}},                 /* Candidate normalization misses a group. */
        {0x13ad9a7, 1, {0}},                 /* Branch to runtime argmax. */
        {0x13ad9a8, 4, {0xff, 0xff, 0xff, 0x7f}}, /* Argmax branch leaves the code section. */
        {0x28d94ef, 1, {0x85}}               /* All three selection branches share the starting score. */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        const struct patch *change = &cases[i];
        uint8_t saved[sizeof(change->data)], output[PVDI_WINDOWS_MOTION_BYTES];
        size_t length = SIZE_MAX;
        memset(output, 0xa5, sizeof output);
        memcpy(saved, image + change->offset, change->count);
        memcpy(image + change->offset, change->data, change->count);
        bool accepted = pvdi_windows_motion_extract(image, size, 3, output, sizeof output, &length);
        memcpy(image + change->offset, saved, change->count);
        if (accepted)
            fprintf(stderr, "Accepted corrupt Windows source at offset 0x%zx\n", change->offset);
        assert(!accepted && length == 0);
        verify_unchanged(output);
    }
}

static void verify_recipe(const uint8_t *image, size_t size, unsigned cohort)
{
    uint8_t output[PVDI_WINDOWS_MOTION_BYTES];
    size_t length = 0;
    assert(pvdi_windows_motion_extract(image, size, cohort, output, sizeof output, &length));
    assert(length == sizeof output && memcmp(output, "PVDIMOT1", 8) == 0);
    char digest[65];
    azsha256_hex(output, length, digest);
    assert(strcmp(digest, "79f10e58d1ba2544c9e6a31e1f629f90774fef20b2289273acce823791abd047") == 0);
}

static void verify_executable(const char *path, unsigned cohort)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *image = malloc(size);
    assert(image && fread(image, 1, size, file) == size);
    assert(fclose(file) == 0);
    verify_recipe(image, size, cohort);
    if (cohort == 3) {
        verify_arguments(image, size);
        verify_unexpected_native(image, size);
        verify_recipe(image, size, cohort);
    }
    free(image);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s REKORDBOX_7_2_17_EXE REKORDBOX_7_2_18_EXE REKORDBOX_7_2_19_EXE REKORDBOX_7_2_20_EXE\n",
                argv[0]);
        return 2;
    }
    for (unsigned cohort = 0; cohort < 4; ++cohort)
        verify_executable(argv[cohort + 1], cohort);
    puts("Windows vocal motion recipe verified for four executables");
    return 0;
}

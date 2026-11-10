/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_analysis_write.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t original[52] = {0}, grid[32] = {0};
    memcpy(original, "PMAI", 4);
    sa_put32(original + 4, 28);
    sa_put32(original + 8, sizeof original);
    original[12] = 0x91; /* opaque PMAI extension survives */
    memcpy(original + 28, "PCOB", 4);
    sa_put32(original + 32, 24);
    sa_put32(original + 36, 24);
    original[48] = 0x71; /* opaque cue data survives */
    memcpy(grid, "PQTZ", 4);
    sa_put32(grid + 4, 24);
    sa_put32(grid + 8, 32);
    sa_put32(grid + 20, 1);
    size_t count = 0;
    uint8_t *result = sa_merge(original, sizeof original, grid, sizeof grid, &count);
    assert(result && count == 84 && sa_framed(result, count));
    assert(result[12] == 0x91 && !memcmp(result + 28, original + 28, 24));
    assert(sa_tag_count(result, count, "PQTZ") == 1);
    size_t ignored;
    assert(!sa_merge(result, count, grid, sizeof grid, &ignored));
    free(result);
    /* Explicitly empty map replacement preserves all other bytes. */
    sa_put32(grid + 8, 24);
    sa_put32(grid + 20, 0);
    result = sa_merge(original, sizeof original, grid, 24, &count);
    assert(result && count == 76);
    sa_put32(grid + 8, 32);
    sa_put32(grid + 20, 1);
    uint8_t *replaced = sa_merge(result, count, grid, 32, &ignored);
    assert(replaced && ignored == 84 && !memcmp(replaced + 28, original + 28, 24));
    free(replaced);
    free(result);
    result = sa_merge(NULL, 0, grid, 32, &count);
    assert(result && count == 60 && sa_framed(result, count));
    /* Malformed framing, count overflow, duplicate and unknown-version tags refuse. */
    sa_put32(result + 28 + 20, UINT32_MAX);
    assert(sa_tag_count(result, count, "PQTZ") == -1);
    free(result);
    for (size_t i = 1; i < sizeof original; i++) assert(!sa_framed(original, i));
    uint8_t malformed[52];
    memcpy(malformed, original, sizeof malformed);
    sa_put32(malformed + 36, 0);
    assert(!sa_merge(malformed, sizeof malformed, grid, 32, &count));
    puts("ANLZ insertion, preservation, empty replacement and malformed refusal passed");
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include "vocal_lines_projection.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const az_vocal_interval rows[] = {{0, 1}, {2, 3}, {8, 12}, {20, 22}};
    uint8_t out[VOCAL_LINES_WIDTH];
    assert(vocal_lines_project(rows, 4, 22050, -5, 15, 100, out));
    for (unsigned x = 0; x < VOCAL_LINES_WIDTH; x++)
        assert(out[x] == ((x >= 25 && x < 30) || (x >= 35 && x < 40) || (x >= 65 && x < 85)));
    assert(vocal_lines_project(rows, 4, 48000, 1, 9, 80, out));
    for (unsigned x = 0; x < VOCAL_LINES_WIDTH; x++)
        assert(out[x] == ((x >= 10 && x < 20) || (x >= 70 && x < 80)));
    assert(!vocal_lines_project(rows, 4, 48000, -10, -1, 100, out));
    assert(!vocal_lines_project(rows, 4, 48000, 22, 25, 100, out));
    assert(!vocal_lines_project(NULL, 0, 48000, 0, 25, 100, out));
    assert(!vocal_lines_project(NULL, 1, 48000, 0, 25, 100, out));
    assert(!vocal_lines_project(rows, 4, 48000, 0, 25, 100, NULL));
    const double invalid[][2] = {{0, 0}, {2, 1}, {NAN, 1}, {0, INFINITY}, {-DBL_MAX, DBL_MAX}};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        memset(out, 1, sizeof out);
        assert(!vocal_lines_project(rows, 4, 48000, invalid[i][0], invalid[i][1], 100, out));
        assert(!memchr(out, 1, sizeof out));
    }
    assert(!vocal_lines_project(rows, 4, 0, 0, 25, 100, out));
    assert(!vocal_lines_project(rows, 4, 48000, 0, 25, 0, out));
    assert(!vocal_lines_project(rows, 4, 48000, 0, 25, 1281, out));
    const az_vocal_interval tiny = {1.25, 1.75};
    assert(vocal_lines_project(&tiny, 1, 44100, 0, 10, 100, out));
    for (unsigned x = 0; x < VOCAL_LINES_WIDTH; x++) assert(out[x] == (x >= 12 && x < 18));
    puts("PASS vocal line negative viewports, clipping, source clocks and invalid bounds");
    return 0;
}

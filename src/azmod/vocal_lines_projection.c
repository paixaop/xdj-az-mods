/* SPDX-License-Identifier: GPL-3.0-only */
#include "vocal_lines_projection.h"
#include "../common/aztimestamp.h"
#include <math.h>
#include <string.h>

static int viewport_valid(double start, double stop, int width, uint32_t rate) {
    if (width < 1 || width > VOCAL_LINES_WIDTH || !isfinite(stop - start)) return 0;
    int order = compare_timestamps(start, stop, 1, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE;
}
static uint32_t first_visible(const az_vocal_interval *rows, uint32_t count, double start) {
    uint32_t low = 0, high = count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (rows[mid].stop_s <= start) low = mid + 1;
        else high = mid;
    }
    return low;
}
static void mark(az_vocal_interval span, double start, double stop, int width, uint8_t *out) {
    double first = fmax(span.start_s, start), last = fmin(span.stop_s, stop);
    int x0 = (int) floor((first - start) / (stop - start) * width);
    int x1 = (int) ceil((last - start) / (stop - start) * width);
    if (x0 < 0) x0 = 0;
    if (x1 > width) x1 = width;
    if (x1 > x0) memset(out + x0, 1, (size_t) (x1 - x0));
}
int vocal_lines_project(const az_vocal_interval *rows, uint32_t count, uint32_t rate,
                        double start, double stop, int width, uint8_t out[VOCAL_LINES_WIDTH]) {
    if (!out) return 0;
    memset(out, 0, VOCAL_LINES_WIDTH);
    if (!viewport_valid(start, stop, width, rate) || (!rows && count)) return 0;
    uint32_t first = first_visible(rows, count, start);
    for (uint32_t i = first; i < count && rows[i].start_s < stop; i++) mark(rows[i], start, stop, width, out);
    return memchr(out, 1, (size_t) width) != NULL;
}

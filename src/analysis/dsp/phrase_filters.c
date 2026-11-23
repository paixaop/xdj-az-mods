/* SPDX-License-Identifier: GPL-3.0-only */
#include "PortableDSP.h"
/* Preserve explicit fma operations; the build disables implicit contraction. */

void ta_phrase_biquad_process(ta_phrase_biquad *s, const double *input,
                             double *output, size_t count, int rectify) {
    double z1 = s->z1, z2 = s->z2;
    const double b0 = s->b0, b1 = s->b1, b2 = s->b2, a1 = s->a1, a2 = s->a2;
    if (s->first_order) {
        for (size_t i = 0; i < count; ++i) {
            double x = rectify ? fabs(input[i]) : input[i];
            double y = fma(x, b0, z1);
            z1 = fma(x, b1, -y * a1);
            output[i] = y;
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            double x = rectify ? fabs(input[i]) : input[i];
            double y = fma(x, b0, z1);
            z1 = fma(-y, a1, fma(x, b1, z2));
            z2 = fma(x, b2, -y * a2);
            output[i] = y;
        }
    }
    s->z1 = z1;
    s->z2 = z2;
}

void ta_phrase_decimate(ta_phrase_biquad *sections, size_t section_count,
                        const double *input, double *output, size_t count,
                        size_t divisor) {
    /* Each caller owns its states and scratch. Small section-major tiles keep
     * the cascade in cache without full-block intermediate allocations. The
     * decimation phase resets only at the original PCM block boundary. */
    double scratch[1024];
    size_t written = 0, next = divisor - 1;
    for (size_t start = 0; start < count; start += 1024) {
        size_t length = count - start < 1024 ? count - start : 1024;
        const double *source = input + start;
        for (size_t s = 0; s < section_count; ++s) {
            ta_phrase_biquad_process(sections + s, source, scratch, length, 0);
            source = scratch;
        }
        for (; next < start + length; next += divisor)
            output[written++] = source[next - start];
    }
}

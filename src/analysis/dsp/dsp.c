/* SPDX-License-Identifier: GPL-3.0-only */
#include "PortableDSP.h"
#include <math.h>
#include <stdlib.h>
#define DEFINE_FFT(NAME, TYPE, COS, SIN)                                    \
void NAME(TYPE *r, TYPE *im, size_t n) {                                   \
    for (size_t i = 1, j = 0; i < n; i++) {                               \
        size_t bit = n >> 1;                                              \
        for (; j & bit; bit >>= 1) j ^= bit;                              \
        j ^= bit;                                                        \
        if (i < j) {                                                     \
            TYPE t = r[i]; r[i] = r[j]; r[j] = t;                         \
            t = im[i]; im[i] = im[j]; im[j] = t;                          \
        }                                                                \
    }                                                                    \
    for (size_t len = 2; len <= n; len <<= 1) {                           \
        double angle = -6.2831853071795864769 / (double)len;              \
        double cr = cos(angle), ci = sin(angle);                          \
        for (size_t start = 0; start < n; start += len) {                 \
            double wr = 1, wi = 0;                                       \
            for (size_t j = 0; j < len / 2; j++) {                       \
                size_t a = start + j, b = a + len / 2;                   \
                TYPE vr = r[b] * wr - im[b] * wi;                        \
                TYPE vi = r[b] * wi + im[b] * wr;                        \
                r[b] = r[a] - vr; im[b] = im[a] - vi;                     \
                r[a] += vr; im[a] += vi;                                 \
                double next = wr * cr - wi * ci;                        \
                wi = wr * ci + wi * cr; wr = next;                       \
            }                                                            \
        }                                                                \
    }                                                                    \
}
DEFINE_FFT(ta_fft_f, float, cosf, sinf)
DEFINE_FFT(ta_fft_d, double, cos, sin)

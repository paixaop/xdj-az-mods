/* SPDX-License-Identifier: GPL-3.0-only */
#include "PortableDSP.h"
#include <assert.h>
#include <stdio.h>

static void source(double *real, double *imaginary, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        real[i] = ((int) (i * 17 % 13) - 6) * 0.125;
        imaginary[i] = ((int) (i * 11 % 17) - 8) * 0.125;
    }
}
static void reference(const double *real, const double *imaginary, size_t count,
                      double *expected_real, double *expected_imaginary) {
    const long double tau = 6.283185307179586476925286766559005768L;
    for (size_t k = 0; k < count; ++k) {
        long double r = 0, im = 0;
        for (size_t j = 0; j < count; ++j) {
            long double angle = tau * (long double) (j * k) / count;
            r += real[j] * cosl(angle) + imaginary[j] * sinl(angle);
            im += imaginary[j] * cosl(angle) - real[j] * sinl(angle);
        }
        expected_real[k] = (double) r;
        expected_imaginary[k] = (double) im;
    }
}
static void check(size_t count) {
    double real[129], imaginary[129], expected_real[128], expected_imaginary[128];
    float real_f[129], imaginary_f[129];
    source(real, imaginary, count);
    reference(real, imaginary, count, expected_real, expected_imaginary);
    for (size_t i = 0; i < count; ++i) {
        real_f[i] = (float) real[i];
        imaginary_f[i] = (float) imaginary[i];
    }
    real[count] = imaginary[count] = real_f[count] = imaginary_f[count] = 12345;
    ta_fft_f(real_f, imaginary_f, count);
    ta_fft_d(real, imaginary, count);
    for (size_t i = 0; i < count; ++i) {
        assert(fabs(real_f[i] - expected_real[i]) < 5e-6 * count);
        assert(fabs(imaginary_f[i] - expected_imaginary[i]) < 5e-6 * count);
        assert(fabs(real[i] - expected_real[i]) < 4e-12 * count);
        assert(fabs(imaginary[i] - expected_imaginary[i]) < 4e-12 * count);
    }
    assert(real[count] == 12345 && imaginary[count] == 12345);
    assert(real_f[count] == 12345 && imaginary_f[count] == 12345);
}
int main(void) {
    ta_fft_f(NULL, NULL, 0);
    ta_fft_d(NULL, NULL, 0);
    for (size_t count = 1; count <= 128; count *= 2) check(count);
    puts("Float and double FFT match independent complex DFT, including degenerate sizes and buffer guards");
    return 0;
}

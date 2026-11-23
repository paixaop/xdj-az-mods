/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen scalar frontend is the operation-order oracle for the planned NEON FFT. */
#include "../../src/analysis/vocals.c"
#include <assert.h>
static void reference_frame(const Frontend *f, int frame, double *output) {
    float real[N], imag[N];
        for (int i = 0; i < N; i++) {
            long j = (long)(frame)*H + i - H;
            if (j < 0)
                j = -1 - j;
            if (j >= (long)(f->count))
                j = 2 * (long)(f->count) - 1 - j;
            real[i] = (j >= 0 && j < (long)(f->count) ? f->samples[j] : 0) * f->window[i];
            imag[i] = 0;
        }
        for (int i = 0; i < N; i++) {
            int r = 0;
            for (int b = 0; b < 11; b++)
                r = (r << 1) | ((i >> b) & 1);
            if (i < r) {
                float tmp = real[i];
                real[i] = real[r];
                real[r] = tmp;
                tmp = imag[i];
                imag[i] = imag[r];
                imag[r] = tmp;
            }
        }
        for (int half = 1; half < N; half *= 2)
            for (int k = 0; k < half; k++) {
                int a = k * N / (2 * half);
                float c = f->sine[a + N / 4], s = f->sine[a];
                for (int i = k; i < N; i += 2 * half) {
                    int j = i + half;
                    float re = fmaf(real[j], c, imag[j] * s), im = fmaf(imag[j], c, -(real[j] * s));
                    real[j] = real[i] - re;
                    real[i] = re + real[i];
                    imag[j] = imag[i] - im;
                    imag[i] = imag[i] + im;
                }
            }
        for (int i = 0; i <= N / 2; i++)
            output[i] = (double)(fmaf(imag[i], imag[i], real[i] * real[i]));
}
static uint32_t random_state = 17;
static float random_value(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return (float)((int32_t)(random_state >> 8) - 8388608) * 0x1p-23f;
}
int main(void) {
    ta_context *c = calloc(1, sizeof *c);
    assert(c && !setjmp(c->failure));
    unsigned cases = 0;
    const int counts[] = {1, 1023, 1024, 2047, 2048, 4097, 22050};
    for (int rate = 44100; rate <= 48000; rate += 3900) {
        Frontend *f = frontend_init(c, rate, (uint64_t)rate);
        for (int mode = 0; mode < 6; mode++) {
            for (int i = 0; i < 22050; i++) {
                float x = mode == 0 ? 0.0f : mode == 1 ? (i == 0 ? 1.0f : 0.0f) :
                          mode == 2 ? (float)sin((double)i * 0.07) : random_value();
                if (mode == 4) x *= 0x1p-120f;
                if (mode == 5) x = (i & 1) ? -0.0f : 0x1p-149f;
                f->samples[i] = x;
            }
            for (unsigned n = 0; n < sizeof counts / sizeof *counts; n++) {
                f->count = counts[n];
                for (int frame = 0; frame < (f->count + H - 1) / H; frame++) {
                    double expected[N / 2 + 1], actual[N / 2 + 1];
                    reference_frame(f, frame, expected);
                    frontend_frame(f, frame, actual);
                    assert(!memcmp(actual, expected, sizeof actual));
                    cases++;
                }
            }
        }
        ta_cleanup(c);
    }
    free(c);
    printf("%u planned FFT frames match scalar power bits, including reflection, tails and subnormals\n", cases);
}

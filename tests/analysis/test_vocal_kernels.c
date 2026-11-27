/* SPDX-License-Identifier: GPL-3.0-only */
/* Private kernels, with a scalar FMA oracle independent of packed storage. */
#include "../../src/analysis/vocals.c"
#include <assert.h>
static uint32_t seed = 17;
static float value(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)((int32_t)(seed >> 8) - 8388608) * 0x1p-24f;
}
int main(void) {
    const int shapes[][2] = {{285, 1024}, {512, 1024}, {512, 64}, {64, 2}, {21, 12}};
    ta_context *c = calloc(1, sizeof *c);
    assert(c && !setjmp(c->failure));
    for (size_t shape = 0; shape < sizeof shapes / sizeof *shapes; shape++) {
        int rows = shapes[shape][0], columns = shapes[shape][1];
        float *weights = ta_alloc(c, rows * columns, sizeof(float));
        float *bias = ta_alloc(c, columns, sizeof(float));
        float *input = ta_alloc(c, WINDOW_BATCH * rows, sizeof(float));
        for (int i = 0; i < rows * columns; i++) weights[i] = value();
        for (int i = 0; i < columns; i++) bias[i] = value();
        for (int i = 0; i < WINDOW_BATCH * rows; i++) input[i] = value();
        input[0] = -0.0f; input[1] = 0x1p-149f; input[2] = -0x1p-149f;
        Matrix matrix;
        matrix_init(c, &matrix, weights, rows, columns);
        for (int batch = 1; batch <= WINDOW_BATCH; batch++) {
            float actual[WINDOW_BATCH * 1024], expected[WINDOW_BATCH * 1024];
            for (size_t i = 0; i < sizeof actual / sizeof *actual; i++)
                actual[i] = expected[i] = -1234;
            for (int b = 0; b < batch; b++)
                for (int j = 0; j < columns; j++) {
                    float sum = 0;
                    for (int i = 0; i < rows; i++) sum = fmaf(input[b * rows + i], weights[i * columns + j], sum);
                    expected[b * columns + j] = sum + bias[j];
                }
            matrix_windows(&matrix, input, bias, actual, batch);
            assert(!memcmp(actual, expected, sizeof actual));
        }
        ta_cleanup(c);
    }
    free(c);
    puts("PVDI packed weights, NEON/scalar FMA, batches 1-8, tails and untouched slots passed");
}

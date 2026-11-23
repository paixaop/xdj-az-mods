/* SPDX-License-Identifier: GPL-3.0-only */
/* C reconstruction of the PVDI frontend and original trained LSTM. Keep
 * -ffp-contract=off: only the explicit fma/fmaf operations are fused. */
#include "internal.h"
#include <sys/resource.h>
#if defined(__linux__)
/* The retained deck worker used these musl activation functions. */
float ta_musl_expf(float);
float ta_musl_tanhf(float);
#define expf ta_musl_expf
#define tanhf ta_musl_tanhf
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#define MIN(a, b) ((b) < (a) ? (b) : (a))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
enum { N = 2048,
       H = 1024,
       WINDOW_BATCH = 8 };
typedef struct {
    int rows, columns;
    float *packed;
} Matrix;
typedef struct {
    double cheb[240], scale[29], offset[29];
    Matrix matrices[6];
    float *biases[6];
} Model;
typedef struct {
    float *output, *joined, *gates;
} Scratch;
typedef struct {
    float b0, b1, b2, a1, a2, z1, z2, ring[5];
    int rp;
    unsigned sample_rate;
    int count;
    double phase, ratio;
    float *samples, sine[N + N / 4], window[N];
    /* Immutable per-job FFT plan, shared by every analysis frame. */
    uint16_t permutation[N];
    float fft_cos[N - 1], fft_sin[N - 1];
} Frontend;
typedef struct {
    const Model *model;
    double *mel, *motion;
    double filters[128][1025], dct[18][128], bartlett[240];
    double fraction[540], previous[11][240];
    int left[540], firstBin[128], lastBin[128], count;
    bool gate[11];
    double maximum;
} Features;
static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }
static void matrix_init(ta_context *c, Matrix *m, const float *p, int r, int cols)
{
    m->rows = r;
    m->columns = cols;
    m->packed = ta_alloc(c, (size_t)r * cols, sizeof(float));
    if (cols % 8) {
        memcpy(m->packed, p, (size_t)r * cols * sizeof(float));
        return;
    }
    for (int j = 0; j < cols; j += 8)
        for (int i = 0; i < r; i++)
            memcpy(m->packed + j * r + i * 8, p + i * cols + j, 8 * sizeof(float));
}
static void matrix_unpacked(const Matrix *m, const float *x, const float *bias, float *y)
{
    for (int j = 0; j < m->columns; j++) {
        float s = 0;
        for (int i = 0; i < m->rows; i++)
            s = fmaf(x[i], m->packed[i * m->columns + j], s);
        y[j] = s + bias[j];
    }
}
static void matrix_multiply(const Matrix *m, const float *x, const float *bias, float *y)
{
    if (m->columns % 8) {
        matrix_unpacked(m, x, bias, y);
        return;
    }
    for (int j = 0; j < m->columns; j += 8) {
        const float *w = m->packed + j * m->rows;
#if defined(__aarch64__)
        float32x4_t a0 = vdupq_n_f32(0), a1 = a0;
        for (int i = 0; i < m->rows; i++, w += 8) {
            float v = x[i];
            a0 = vfmaq_n_f32(a0, vld1q_f32(w), v);
            a1 = vfmaq_n_f32(a1, vld1q_f32(w + 4), v);
        }
        vst1q_f32(y + j, vaddq_f32(a0, vld1q_f32(bias + j)));
        vst1q_f32(y + j + 4, vaddq_f32(a1, vld1q_f32(bias + j + 4)));
#else
        float sums[8] = { 0 };
        for (int i = 0; i < m->rows; i++, w += 8)
            for (int k = 0; k < 8; k++)
                sums[k] = fmaf(x[i], w[k], sums[k]);
        for (int k = 0; k < 8; k++)
            y[j + k] = sums[k] + bias[j + k];
#endif
    }
}
#if defined(__aarch64__)
/* Eight output columns by up to eight windows use at most sixteen NEON
 * accumulators. Reusing weights across eight windows reduces memory traffic. */
static void matrix_group_2(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(2)][2];
        for (int b = 0; b < (2); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (2); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (2); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_3(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(3)][2];
        for (int b = 0; b < (3); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (3); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (3); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_4(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(4)][2];
        for (int b = 0; b < (4); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (4); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (4); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_5(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(5)][2];
        for (int b = 0; b < (5); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (5); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (5); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_6(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(6)][2];
        for (int b = 0; b < (6); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (6); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (6); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_7(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(7)][2];
        for (int b = 0; b < (7); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (7); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (7); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}

static void matrix_group_8(const Matrix *m, const float *x, const float *bias, float *y) {
/* Independent windows share each weight load and keep accumulators in NEON registers. */
    for (int j = 0; j < m->columns; j += 8) {
        float32x4_t sums[(8)][2];
        for (int b = 0; b < (8); b++)
            for (int k = 0; k < 2; k++)
                sums[b][k] = vdupq_n_f32(0);
        const float *w = m->packed + j * m->rows;
        for (int i = 0; i < m->rows; i++, w += 8) {
        /* Prefetch eight rows ahead without leaving the packed block. */
            if (i + 8 < m->rows) __builtin_prefetch(w + 64, 0, 3);
            float32x4_t w0 = vld1q_f32(w), w1 = vld1q_f32(w + 4);
            for (int b = 0; b < (8); b++) {
                float v = x[b * 512 + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (8); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * 1024 + j + k * 4,
                          vaddq_f32(sums[b][k], vld1q_f32(bias + j + k * 4)));
    }
}
#endif
static void matrix_windows(const Matrix *m, const float *x, const float *bias, float *y,
                           int batch)
{
#if defined(__aarch64__)
    static void (*const groups[])(const Matrix *, const float *, const float *, float *) = {
        matrix_group_2, matrix_group_3, matrix_group_4, matrix_group_5,
        matrix_group_6, matrix_group_7, matrix_group_8
    };
    if (batch >= 2 && batch <= 8) {
        groups[batch - 2](m, x, bias, y);
        return;
    }
#endif
    for (int b = 0; b < batch; b++)
        matrix_multiply(m, x + b * 512, bias, y + b * 1024);
}
static void model_read(ta_context *c, const char *path, uint8_t *bytes, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        ta_fail(c, TA_FAILED, "PVDI model open failed");
    int valid = fread(bytes, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    if (fclose(f))
        valid = 0;
    if (!valid)
        ta_fail(c, TA_FAILED, "PVDI model size");
}
static void model_validate(ta_context *c, const uint8_t *bytes, size_t size, size_t payload)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 32; i < size; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    if (memcmp(bytes, "AZPVDI01", 8) || ta_le32(bytes + 8) != payload ||
        ta_le32(bytes + 12) != hash)
        ta_fail(c, TA_FAILED, "PVDI model checksum");
    for (int i = 16; i < 32; i++)
        if (bytes[i])
            ta_fail(c, TA_FAILED, "PVDI model schema");
}
static Model *model_load(ta_context *c, const char *path)
{
    const size_t payload = 298 * 8 + 1669314 * 4, size = payload + 32;
    Model *m = ta_alloc(c, 1, sizeof *m);
    uint8_t *bytes = ta_alloc(c, size, 1);
    model_read(c, path, bytes, size);
    model_validate(c, bytes, size, payload);
    memcpy(m->cheb, bytes + 32, sizeof m->cheb);
    memcpy(m->scale, bytes + 32 + sizeof m->cheb, sizeof m->scale);
    memcpy(m->offset, bytes + 32 + sizeof m->cheb + sizeof m->scale, sizeof m->offset);
    const float *p = (const float *)(bytes + 32 + 298 * 8);
    const int rows[] = { 285, 512, 285, 512, 512, 64 };
    for (int i = 0; i < 6; i++) {
        int cols = i < 4 ? 1024 : i == 4 ? 64
                                         : 2;
        matrix_init(c, &m->matrices[i], p, rows[i], cols);
        p += rows[i] * cols;
        m->biases[i] = ta_alloc(c, cols, sizeof(float));
        memcpy(m->biases[i], p, cols * sizeof(float));
        p += cols;
    }
    ta_release(c, bytes);
    return m;
}
typedef struct {
    float hidden[WINDOW_BATCH][2][256], cell[WINDOW_BATCH][2][256];
} RecurrentState;
static void infer_join(Scratch *scratch, const RecurrentState *state, const float *features,
                       const int *lengths, int batch, int group, int step, int direction, int layer)
{
    int width = layer ? 256 : 29;
    for (int b = 0; b < batch; b++) {
        if (step >= lengths[b]) continue;
        int t = direction ? lengths[b] - 1 - step : step;
        const float *input = layer ? state->hidden[b][0] : features + (group + b * 180 + t) * 29;
        memcpy(scratch->joined + b * 512, input, width * sizeof(float));
        memcpy(scratch->joined + b * 512 + width, state->hidden[b][layer], 256 * sizeof(float));
    }
}
static void infer_cells(RecurrentState *state, const Scratch *scratch, const int *lengths,
                        int batch, int step, int layer)
{
    for (int b = 0; b < batch; b++) {
        if (step >= lengths[b]) continue;
        const float *z = scratch->gates + b * 1024;
        for (int u = 0; u < 256; u++) {
            float c = state->cell[b][layer][u] * sigmoid(z[512 + u] + 1.0f) +
                      sigmoid(z[u]) * tanhf(z[256 + u]);
            state->cell[b][layer][u] = c;
            state->hidden[b][layer][u] = tanhf(c) * sigmoid(z[768 + u]);
        }
    }
}
static void infer_save(Scratch *scratch, const RecurrentState *state, const int *lengths,
                       int batch, int step, int direction)
{
    for (int b = 0; b < batch; b++)
        if (step < lengths[b]) {
            int t = direction ? lengths[b] - 1 - step : step;
            memcpy(scratch->output + (b * 360 + t) * 512 + direction * 256, state->hidden[b][1], 256 * sizeof(float));
        }
}
static void infer_direction(ta_context *c, const Model *m, const float *features, const int *lengths,
                            int batch, int group, int direction, Scratch *scratch)
{
    RecurrentState state = { 0 };
    for (int step = 0; step < lengths[0]; step++) {
        ta_check(c);
        int active = batch;
        while (active && step >= lengths[active - 1]) active--;
        for (int layer = 0; layer < 2; layer++) {
            int index = direction * 2 + layer;
            infer_join(scratch, &state, features, lengths, batch, group, step, direction, layer);
            matrix_windows(&m->matrices[index], scratch->joined, m->biases[index], scratch->gates, active);
            infer_cells(&state, scratch, lengths, batch, step, layer);
        }
        infer_save(scratch, &state, lengths, batch, step, direction);
    }
}
static void infer_probabilities(const Model *m, const Scratch *scratch, const int *lengths,
                                int batch, int group, float *probabilities)
{
    for (int b = 0; b < batch; b++)
        for (int t = 0; t < lengths[b]; t++) {
            float dense[64], logits[2];
            matrix_multiply(&m->matrices[4], scratch->output + (b * 360 + t) * 512, m->biases[4], dense);
            for (int i = 0; i < 64; i++) dense[i] = MAX(0.0f, dense[i]);
            matrix_multiply(&m->matrices[5], dense, m->biases[5], logits);
            float maximum = MAX(logits[0], logits[1]);
            float a = expf(logits[0] - maximum), p = expf(logits[1] - maximum);
            probabilities[(group / 180 + b) * 360 + t] = p / (a + p);
        }
}
static void infer_group(ta_context *c, const Model *m, const float *features, int count, int group,
                        Scratch *scratch, float *probabilities)
{
    int batch = MIN(WINDOW_BATCH, (count - group + 179) / 180), lengths[WINDOW_BATCH] = { 0 };
    for (int b = 0; b < batch; b++) lengths[b] = MIN(360, count - group - b * 180);
    for (int direction = 0; direction < 2; direction++)
        infer_direction(c, m, features, lengths, batch, group, direction, scratch);
    infer_probabilities(m, scratch, lengths, batch, group, probabilities);
}
typedef struct {
    const Model *model;
    const float *features;
    int count;
    Scratch scratch[3];
    float *probabilities;
} inference_job;
static void infer_task(ta_context *c, void *data, size_t group, unsigned worker)
{
    inference_job *job = data;
    infer_group(c, job->model, job->features, job->count, (int)group * (WINDOW_BATCH * 180),
                &job->scratch[worker], job->probabilities);
}
static float *model_infer(ta_context *c, const Model *m, const float *features, int count)
{
    int windows = (count + 179) / 180;
    float *probabilities = ta_alloc(c, (size_t)windows * 360, sizeof(float));
    unsigned groups = (unsigned)((windows + WINDOW_BATCH - 1) / WINDOW_BATCH);
    unsigned threads = c->execution.vocal_threads ? c->execution.vocal_threads : 1;
    if (threads > groups) threads = groups;
    inference_job job = { .model = m, .features = features, .count = count, .probabilities = probabilities };
    /* Allocate private scratch before launching, as in the original worker. */
    for (unsigned i = 0; i < threads; i++)
        job.scratch[i] = (Scratch){ ta_alloc(c, WINDOW_BATCH * 360 * 512, sizeof(float)),
                                    ta_alloc(c, WINDOW_BATCH * 512, sizeof(float)),
                                    ta_alloc(c, WINDOW_BATCH * 1024, sizeof(float)) };
    ta_parallel(c, groups, threads, infer_task, &job);
    float *scores = ta_alloc(c, count, sizeof(float));
    uint8_t *contributors = ta_alloc(c, count, 1);
    for (int w = 0; w < windows; w++) {
        int first = w * 180, length = MIN(360, count - first);
        for (int t = 0; t < length; t++) {
            scores[first + t] += probabilities[w * 360 + t];
            contributors[first + t]++;
        }
    }
    for (int i = 0; i < count; i++)
        scores[i] /= (float)contributors[i];
    ta_release(c, probabilities);
    ta_release(c, contributors);
    for (unsigned i = 0; i < threads; i++) {
        ta_release(c, job.scratch[i].output);
        ta_release(c, job.scratch[i].joined);
        ta_release(c, job.scratch[i].gates);
    }
    return scores;
}
static float lagrange(const float *r, int p, float t)
{
    int a = (p + 1) % 5, b = (p + 2) % 5, c = (p + 3) % 5, d = (p + 4) % 5;
    float third = t * (float)(1.0 / 3), quarter = t * 0.25f, half = t * 0.5f;
    float mid = 1.0f - t;
    mid = fmaf(t, mid, mid);
    mid = fmaf(half, mid, mid);
    mid = fmaf(-half, mid, mid);
    float left1 = ((half - 0.5f) * ((float)(2.0 / 3) - third)) * r[a];
    left1 = (t + 2.0f) * left1;
    float left2 = ((third - (float)(1.0 / 3)) * (0.5f - quarter)) * r[p];
    left2 = (-1.0f - t) * left2;
    float right1 = (2.0f - t) * ((third + (float)(2.0 / 3)) * (half + 0.5f));
    right1 = right1 * r[c];
    float right2 = (t - 1.0f) * ((quarter + 0.5f) * (third + (float)(1.0 / 3)));
    right2 = right2 * r[d];
    float side = (right1 + left1) + (right2 + left2) * 0.5f;
    return fmaf(side, t, mid * r[b]);
}
static Frontend *frontend_init(ta_context *c, unsigned rate, uint64_t frames)
{
    Frontend *f = ta_alloc(c, 1, sizeof *f);
    f->sample_rate = rate;
    f->phase = 1;
    f->ratio = (double)rate * 4.5351473922902495e-05;
    f->samples = ta_alloc(c, (size_t)(frames * 22050 / rate), sizeof(float));

    double k = 1 / tan(M_PI * 11025 / rate), kk = k * k, q = k * sqrt(2.0),
           norm = 1 / ((q + 1) + kk), twice = norm + norm;
    f->b0 = f->b2 = (float)(norm);
    f->b1 = (float)(twice);
    f->a1 = (float)(fma(-kk, twice, twice));
    f->a2 = (float)(norm * ((1 - q) + kk));
    float s = (float)(sin(M_PI / N));
    float sq = s * s, step = sq * 4.0f, cd = sq + sq;
    float sd = (float)(sqrt((2 - (double)(cd)) * (double)(cd))), cv = 1, sv = 0;
    f->sine[0] = 0;
    f->sine[N / 4] = 1;
    for (int i = 1; i < N / 8; i++) {
        cv = cv - cd;
        cd = fmaf(cv, step, cd);
        sv = sv + sd;
        sd = fmaf(-sv, step, sd);
        f->sine[i] = sv;
        f->sine[N / 4 - i] = cv;
    }
    f->sine[N / 8] = (float)(0.7071067811865476);
    for (int i = 0; i < N / 4; i++)
        f->sine[N / 2 - i] = f->sine[i];
    for (int i = 0; i < N * 3 / 4; i++)
        f->sine[i + N / 2] = -f->sine[i];
    double scale = (2 * M_PI) / (N + 1);
    for (int i = 0; i < N; i++)
        f->window[i] = (float)(fma(-cos(scale * (i + 1)), 0.5, 0.5));
    for (int i = 0; i < N; i++) {
        unsigned reversed = 0;
        for (unsigned bit = 0; bit < 11; bit++)
            reversed = (reversed << 1) | (((unsigned)i >> bit) & 1u);
        f->permutation[i] = (uint16_t)reversed;
    }
    for (int half = 1; half < N; half *= 2)
        for (int k = 0; k < half; k++) {
            int a = k * N / (2 * half);
            f->fft_cos[half - 1 + k] = f->sine[a + N / 4];
            f->fft_sin[half - 1 + k] = f->sine[a];
        }
    return f;
}
static void frontend_add(ta_context *c, Frontend *f, const float *in, int n)
{
    float *x = ta_alloc(c, n, sizeof(float));
    for (int i = 0; i < n; i++) {
        float a = in[i] * (1.0f / 32768.0f), y = fmaf(a, f->b0, f->z1);
        x[i] = y;
        f->z1 = fmaf(-y, f->a1, fmaf(a, f->b1, f->z2));
        f->z2 = fmaf(a, f->b2, -(y * f->a2));
    }
    int pos = 0, out = (int)(((double)(n) * 22050) / f->sample_rate);
    for (int i = 0; i < out; i++) {
        while (f->phase >= 1) {
            f->ring[f->rp] = x[pos++];
            f->rp = (f->rp + 1) % 5;
            f->phase -= 1;
        }
        f->samples[f->count++] = lagrange(f->ring, f->rp, (float)(f->phase));
        f->phase += f->ratio;
    }
    ta_release(c, x);
}
static void features_mel_filters(Features *f)
{
    double hz[130], maxmel = log(22050.0 * 0.0005) * 14.54507850578556 + 14.999999999999998;
    for (int i = 0; i < 130; i++) {
        double v = maxmel * i;
        hz[i] = v * 0.007751937984496124 < 14.999999999999998
                    ? v * 0.516795865633075
                    : exp(v * 0.0005329595148910785 - 1.0312766613142368) * 1000.0;
    }
    for (int b = 0; b < 128; b++)
        for (int k = 0; k < 1025; k++) {
            double frequency = k * 11025.0 / 1024.0, span = 1.0 / (hz[b + 2] - hz[b]);
            f->filters[b][k] =
                MAX(0.0, MIN((frequency - hz[b]) * (1.0 / (hz[b + 1] - hz[b])),
                             (hz[b + 2] - frequency) * (1.0 / (hz[b + 2] - hz[b + 1])))) *
                (span + span);
        }
}
static void features_mel_support(Features *f)
{
    // Mel triangles have compact support. Omitting exact-zero terms keeps
    // the same summation order while avoiding most of the 128x1025 products.
    for (int b = 0; b < 128; b++) {
        f->firstBin[b] = 0;
        f->lastBin[b] = 1025;
        while (f->firstBin[b] < 1025 && f->filters[b][f->firstBin[b]] == 0)
            f->firstBin[b]++;
        while (f->lastBin[b] > f->firstBin[b] && f->filters[b][f->lastBin[b] - 1] == 0)
            f->lastBin[b]--;
    }
}
static void features_motion_basis(Features *f)
{
    for (int c = 0; c < 18; c++)
        for (int b = 0; b < 128; b++)
            f->dct[c][b] =
                cos(c * (b * 2 + 1) * (M_PI / 256.0)) * (c ? 0.125 : 0.08838834764831845);
    for (int i = 0; i < 240; i++)
        f->bartlett[i] = fma(-fabs(i - 119.5), 0.008368200836820083, 0.9999999999999999);
    for (int k = 0; k < 540; k++) {
        double hz = 233.08188075904496 * exp2(k * 0.008333333333333333);
        f->left[k] = (int)(ceil(hz / (11025.0 / 1024.0))) - 1;
        f->fraction[k] = (hz - f->left[k] * (11025.0 / 1024.0)) / (11025.0 / 1024.0);
    }
}
static Features *features_init(ta_context *c, const Model *model, int count)
{
    Features *f = ta_alloc(c, 1, sizeof *f);
    f->model = model;
    f->mel = ta_alloc(c, (size_t)count * 128, sizeof(double));
    f->motion = ta_alloc(c, (size_t)count * 11, sizeof(double));

    features_mel_filters(f);
    features_mel_support(f);
    features_motion_basis(f);
    return f;
}
static void features_mel_add(Features *f, const double *power)
{
    for (int b = 0; b < 128; b++) {
        double sum = 0;
        for (int k = f->firstBin[b]; k < f->lastBin[b]; k++)
            sum += power[k] * f->filters[b][k];
        f->mel[f->count * 128 + b] = sum;
        f->maximum = MAX(f->maximum, sum);
    }
}
static void features_log_spectrum(const Features *f, const double *power, double logmag[540], double logsByBin[540])
{
    const double eps = 2.220446049250313e-16;
    double mag[1025];
    for (int i = 0; i < 1025; i++)
        mag[i] = sqrt(power[i]);
    for (int i = 0; i < 540; i++) {
        logmag[i] = f->fraction[i] * (mag[f->left[i] + 1] - mag[f->left[i]]) + mag[f->left[i]];
        // The eleven overlapping bands reuse the same 540 values. Keep
        // each band's original addition order, but evaluate libm once per
        // bin instead of 2,640 times per frame.
        logsByBin[i] = log(logmag[i] + eps);
    }
}
static void features_correlations(const Features *f, int band, const double weighted[240], double correlations[11])
{
    double denom = 0;
    for (int k = 0; k < 240; k++)
        denom += f->previous[band][k] * weighted[k];
    for (int lag = -5; lag <= 5; lag++) {
        double s = 0;
        for (int k = MAX(0, -lag); k < MIN(240, 240 - lag); k++)
            s += f->previous[band][k + lag] * weighted[k];
        correlations[lag + 5] = lag ? s / denom : 1.0;
    }
}
static int features_best_lag(const double correlations[11])
{
    int best = 0;
    double correlation_max = correlations[0];
    for (int i = 1; i < 11; i++) {
        double old = correlation_max;
        if (!(correlations[i] <= old))
            best = i;
        if (old <= correlations[i])
            correlation_max = correlations[i];
    }
    return best - 5;
}
static void features_motion_add(Features *f, int band, int row, const double logmag[540], const double logsByBin[540])
{
    const double eps = 2.220446049250313e-16;
    double weighted[240], sum = 0, energy = 0, logs = 0;
    for (int k = 0; k < 240; k++) {
        double v = logmag[band * 30 + k];
        weighted[k] = v * f->bartlett[k];
        sum += v;
        energy += v * f->model->cheb[k];
        logs += logsByBin[band * 30 + k];
    }
    if (row && f->gate[band]) {
        double correlations[11];
        features_correlations(f, band, weighted, correlations);
        f->motion[row - 11 + band] = features_best_lag(correlations);
    }
    f->gate[band] = energy / (sum + eps) > 0.1 &&
                    exp(logs * (1.0 / 240.0)) / (sum * (1.0 / 240.0) + eps) < 0.6;
    memcpy(f->previous[band], weighted, 240 * sizeof(double));
}
static void features_add(Features *f, const double *power)
{
    features_mel_add(f, power);
    double logmag[540], logsByBin[540];
    features_log_spectrum(f, power, logmag, logsByBin);
    int row = f->count * 11;
    for (int band = 0; band < 11; band++) features_motion_add(f, band, row, logmag, logsByBin);
    f->count++;
}
/* All butterflies in a stage are independent. Visit contiguous butterflies
 * together while retaining each scalar multiply, explicit FMA and addition. */
static void frontend_fft(const Frontend *f, float *real, float *imag)
{
    for (int half = 1; half < N; half *= 2) {
        const float *cosine = f->fft_cos + half - 1;
        const float *sine = f->fft_sin + half - 1;
        for (int base = 0; base < N; base += 2 * half) {
            int k = 0;
#if defined(__aarch64__)
            for (; k + 4 <= half; k += 4) {
                int i = base + k, j = i + half;
                float32x4_t ar = vld1q_f32(real + i), ai = vld1q_f32(imag + i);
                float32x4_t br = vld1q_f32(real + j), bi = vld1q_f32(imag + j);
                float32x4_t cv = vld1q_f32(cosine + k), sv = vld1q_f32(sine + k);
                float32x4_t re = vfmaq_f32(vmulq_f32(bi, sv), br, cv);
                float32x4_t im = vfmaq_f32(vnegq_f32(vmulq_f32(br, sv)), bi, cv);
                vst1q_f32(real + j, vsubq_f32(ar, re));
                vst1q_f32(real + i, vaddq_f32(re, ar));
                vst1q_f32(imag + j, vsubq_f32(ai, im));
                vst1q_f32(imag + i, vaddq_f32(ai, im));
            }
#endif
            for (; k < half; k++) {
                int i = base + k, j = i + half;
                float re = fmaf(real[j], cosine[k], imag[j] * sine[k]);
                float im = fmaf(imag[j], cosine[k], -(real[j] * sine[k]));
                real[j] = real[i] - re;
                real[i] = re + real[i];
                imag[j] = imag[i] - im;
                imag[i] = imag[i] + im;
            }
        }
    }
}
static void frontend_frame(const Frontend *f, int frame, double *output)
{
    float real[N], imag[N] = { 0 };
    /* Window directly into bit-reversed order, eliminating a second traversal. */
    for (int i = 0; i < N; i++) {
        long j = (long)frame * H + i - H;
        if (j < 0) j = -1 - j;
        if (j >= (long)f->count) j = 2 * (long)f->count - 1 - j;
        real[f->permutation[i]] = (j >= 0 && j < (long)f->count ? f->samples[j] : 0) * f->window[i];
    }
    frontend_fft(f, real, imag);
    int i = 0;
#if defined(__aarch64__)
    for (; i + 4 <= N / 2 + 1; i += 4) {
        float32x4_t r = vld1q_f32(real + i), im = vld1q_f32(imag + i);
        float32x4_t power = vfmaq_f32(vmulq_f32(r, r), im, im);
        vst1q_f64(output + i, vcvt_f64_f32(vget_low_f32(power)));
        vst1q_f64(output + i + 2, vcvt_f64_f32(vget_high_f32(power)));
    }
#endif
    for (; i <= N / 2; i++)
        output[i] = (double)fmaf(imag[i], imag[i], real[i] * real[i]);
}
static void frontend_power(ta_context *c, const Frontend *f, Features *features)
{
    double output[N / 2 + 1];
    int count = (int)((f->count + H - 1) / H);
    for (int frame = 0; frame < count; frame++) {
        ta_check(c);
        frontend_frame(f, frame, output);
        features_add(features, output);
    }
}
static float *features_finish(ta_context *c, const Features *f)
{
    int count = f->count;
    if (count < 9)
        ta_fail(c, TA_FAILED, "PVDI requires at least nine frames");
    double *cepstrum = ta_alloc(c, (size_t)count * 18, sizeof(double));
    double ref = log10(MAX(f->maximum, 1e-10));
    for (int t = 0; t < count; t++) {
        double db[128];
        for (int k = 0; k < 128; k++)
            db[k] = MAX(-80.0, (log10(MAX(f->mel[t * 128 + k], 1e-10)) - ref) * 10.0);
        for (int c = 0; c < 18; c++) {
            double s = 0;
            for (int k = 0; k < 128; k++)
                s += db[k] * f->dct[c][k];
            cepstrum[t * 18 + c] = s;
        }
    }
    float *result = ta_alloc(c, (size_t)count * 29, sizeof(float));
    for (int t = 0; t < count; t++) {
        int center = MAX(4, MIN(count - 5, t));
        for (int component = 0; component < 29; component++) {
            double value;
            if (component < 18) {
#define at(k) cepstrum[(center + (k)) * 18 + component]
                double a = at(2) - at(-2);
                value =
                    ((at(1) - at(-1)) + a + a + (at(3) - at(-3)) * 3.0 + (at(4) - at(-4)) * 4.0) *
                    0.016666666666666666;
            } else
                value = f->motion[t * 11 + component - 18];
            result[t * 29 + component] =
                (float)(fma(value, f->model->scale[component], f->model->offset[component]));
        }
    }
    ta_release(c, cepstrum);
    return result;
#undef at
}
static void diagnostic_floats(const char *setting, const float *values, size_t count)
{
    const char *path = getenv(setting);
    if (path) {
        FILE *file = fopen(path, "wb");
        if (file) {
            (void)fwrite(values, sizeof *values, count, file);
            fclose(file);
        }
    }
}
static double elapsed(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) * 1e-9;
}
static double cpu_seconds(struct timeval value)
{
    return (double)value.tv_sec + (double)value.tv_usec * 1e-6;
}
static uint64_t vocals_decode_chunk(ta_context *c, int16_t *pcm, uint64_t need)
{
    uint64_t have = 0;
    while (have < need) {
        int64_t n = ta_decode_read(c->decoder, pcm + have * 2, need - have);
        if (n <= 0)
            ta_fail(c, TA_FAILED, "PVDI truncated decode");
        have += (uint64_t)n;
    }
    return have;
}
static void vocals_decode(ta_context *c, Frontend *frontend, unsigned rate, uint64_t frames)
{
    int16_t *pcm = ta_alloc(c, rate * 4, sizeof(int16_t));
    float *mono = ta_alloc(c, rate * 2, sizeof(float));
    uint64_t received = 0;
    while (received < frames) {
        ta_check(c);
        uint64_t need = MIN((uint64_t)rate * 2, frames - received), have = 0;
        have = vocals_decode_chunk(c, pcm, need);
        for (size_t i = 0; i < have; i++)
            mono[i] = ((float)pcm[i * 2] + (float)pcm[i * 2 + 1]) * 0.5f;
        frontend_add(c, frontend, mono, (int)have);
        received += have;
    }
    int16_t tail[2];
    if (ta_decode_read(c->decoder, tail, 1) != 0)
        ta_fail(c, TA_FAILED, "PVDI decoded count changed");
    ta_decode_close(c->decoder);
    c->decoder = NULL;
    ta_release(c, pcm);
    ta_release(c, mono);
}
static void vocals_diagnostics(ta_context *c, const float *normalized, const float *probabilities, int count)
{
    const ta_diagnostics *diagnostics = c->request->diagnostics;
    if (diagnostics && diagnostics->vocal_features)
        diagnostics->vocal_features(diagnostics->user, (const float (*)[29])normalized, (uint32_t)count);
    if (diagnostics && diagnostics->vocal_probabilities)
        diagnostics->vocal_probabilities(diagnostics->user, probabilities, (uint32_t)count);
    diagnostic_floats("TRACK_ANALYSIS_PVDI_FEATURES", normalized, (size_t)count * 29);
    diagnostic_floats("TRACK_ANALYSIS_PVDI_PROBABILITIES", probabilities, (size_t)count);
}
static az_analysis_vocal_map_v1 vocals_result(ta_context *c, const float *probabilities, int count)
{
    az_vocal_frame_v1 *frames_out = ta_alloc(c, count, sizeof *frames_out);
    az_analysis_vocal_map_v1 result = { .sample_rate = 22050,
                                        .hop_samples = 1024,
                                        .method = AZ_VOCAL_LSTM,
                                        .count = (uint32_t)count,
                                        .frames = frames_out };
    for (int i = 0; i < count; i++) {
        float p = probabilities[i];
        if (!isfinite(p))
            ta_fail(c, TA_FAILED, "nonfinite PVDI probability");
        frames_out[i].activity = (p > 0.2f) + (p > 0.4f) + (p > 0.6f) + (p > 0.8f);
    }
    return result;
}
static void vocals_profile(ta_context *c, struct timespec started, struct timespec decoded, struct timespec extracted, struct timespec inferred, struct rusage initial, uint64_t frames, unsigned rate, int count)
{
    struct rusage usage = { 0 };
    getrusage(RUSAGE_SELF, &usage);
    long rss = usage.ru_maxrss;
#ifdef __APPLE__
    rss /= 1024;
#endif
    unsigned threads = c->execution.vocal_threads ? c->execution.vocal_threads : 1;
    if (threads > (unsigned)(count + 719) / 720) threads = (unsigned)(count + 719) / 720;
    /* In-process resource counters cover the containing process, including
     * unrelated firmware threads. Label them explicitly instead of claiming
     * that they measure only this analysis. Stage elapsed times remain local. */
    fprintf(stderr,
            "PVDI threads=%u duration=%.6f frames=%d decode_s=%.6f features_s=%.6f "
            "inference_s=%.6f total_s=%.6f process_user_s=%.6f process_system_s=%.6f "
            "process_maxrss_kb=%ld\n",
            threads, (double)frames / rate, count,
            elapsed(started, decoded), elapsed(decoded, extracted), elapsed(extracted, inferred),
            elapsed(started, inferred), cpu_seconds(usage.ru_utime) - cpu_seconds(initial.ru_utime),
            cpu_seconds(usage.ru_stime) - cpu_seconds(initial.ru_stime), rss);
}
az_analysis_vocal_map_v1 ta_vocals(ta_context *c, const char *model_path)
{
    clock_gettime(CLOCK_MONOTONIC, &c->profile_previous);
    struct timespec started = c->profile_previous;
    struct rusage initial = { 0 };
    getrusage(RUSAGE_SELF, &initial);
    c->phrase_profile = false;
    Model *model = model_load(c, model_path);
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(c->request->audio_path, &rate, &frames);
    if (!c->decoder || (rate != 44100 && rate != 48000) || frames > (uint64_t)rate * 1200 ||
        frames * 22050 / rate < 8193)
        ta_fail(c, TA_FAILED, "PVDI source rate or duration");
    Frontend *frontend = frontend_init(c, rate, frames);
    vocals_decode(c, frontend, rate, frames);
    ta_profile_mark(c, "vocal_decode");
    struct timespec decoded = c->profile_previous;
    Features *features = features_init(c, model, (frontend->count + H - 1) / H);
    frontend_power(c, frontend, features);
    float *normalized = features_finish(c, features);
    int count = features->count;
    ta_release(c, features->mel);
    ta_release(c, features->motion);
    ta_release(c, features);
    ta_release(c, frontend->samples);
    ta_release(c, frontend);
    ta_profile_mark(c, "vocal_features");
    struct timespec extracted = c->profile_previous;
    float *probabilities = model_infer(c, model, normalized, count);
    ta_profile_mark(c, "vocal_inference");
    struct timespec inferred = c->profile_previous;
    vocals_diagnostics(c, normalized, probabilities, count);
    az_analysis_vocal_map_v1 result = vocals_result(c, probabilities, count);
    ta_release(c, probabilities);
    ta_release(c, normalized);
    for (int i = 0; i < 6; i++) {
        ta_release(c, model->matrices[i].packed);
        ta_release(c, model->biases[i]);
    }
    ta_release(c, model);
    vocals_profile(c, started, decoded, extracted, inferred, initial, frames, rate, count);
    return result;
}
#undef MIN
#undef MAX

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azanalysis_policy.h"
/* C reconstruction of the PVDI frontend and original trained LSTM. Keep
 * -ffp-contract=off: only the explicit fma/fmaf operations are fused. */
#include "internal.h"
#include "vocal-motion.h"
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
enum { WINDOW_BATCH = 8 };
typedef struct {
    int rows, columns;
    float *packed;
} Matrix;
typedef struct {
    int units, gate_offsets[TA_VOCAL_GATE_ROLES];
    float initial_cell, initial_hidden, forget_bias;
} RecurrentCell;
typedef struct {
    int cells[TA_VOCAL_RECURRENT_CELL_COUNT], length, output_offset;
    bool reversed;
} RecurrentBranch;
typedef struct {
    double *cheb, *scale, *offset;
    ta_vocal_motion *windows_motion;
    double frontend[TA_VOCAL_PARAMETER_COUNT];
    float activity[TA_VOCAL_THRESHOLD_COUNT];
    RecurrentCell recurrent[TA_VOCAL_RECURRENT_CELL_COUNT];
    RecurrentBranch branches[TA_VOCAL_RECURRENT_CELL_COUNT];
    Matrix matrices[TA_VOCAL_MATRIX_COUNT];
    float *biases[TA_VOCAL_MATRIX_COUNT];
    int feature_columns, vocal_class, max_rows, max_gates, max_units;
    int branch_count, max_layers;
} Model;
typedef struct {
    float *output, *joined, *gates;
    float *hidden[TA_VOCAL_RECURRENT_CELL_COUNT], *cell[TA_VOCAL_RECURRENT_CELL_COUNT];
    float *dense, *logits;
} Scratch;
typedef struct {
    const double *parameters;
    float b0, b1, b2, a1, a2, z1, z2;
    int rp, fft_size;
    unsigned sample_rate;
    int count;
    double phase, ratio;
    float *samples, *sine, *window;
    /* Immutable per-job FFT plan, shared by every analysis frame. */
    uint16_t *permutation;
    float *fft_cos, *fft_sin;
    float ring[];
} Frontend;
typedef struct {
    const Model *model;
    double *mel, *motion, *filters, *dct;
    double *bartlett, *fraction, *previous, *weighted;
    double *magnitude, *log_magnitude, *log_bins;
    int *left, *firstBin, *lastBin, count, motion_rows;
    bool *gate;
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (2); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (3); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (4); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (5); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (6); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (7); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
                float v = x[b * m->rows + i];
                sums[b][0] = vfmaq_n_f32(sums[b][0], w0, v);
                sums[b][1] = vfmaq_n_f32(sums[b][1], w1, v);
            }
        }
        for (int b = 0; b < (8); b++)
            for (int k = 0; k < 2; k++)
                vst1q_f32(y + b * m->columns + j + k * 4,
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
    if (batch >= 2 && batch <= WINDOW_BATCH && m->columns % 8 == 0) {
        groups[batch - 2](m, x, bias, y);
        return;
    }
#endif
    for (int b = 0; b < batch; b++)
        matrix_multiply(m, x + b * m->rows, bias, y + b * m->columns);
}
static void model_activity(ta_context *c, Model *m, const uint8_t *bytes)
{
    memcpy(m->activity, bytes + TA_MODEL_THRESHOLD_OFFSET, sizeof m->activity);
    float previous = 0;
    for (size_t i = 0; i < TA_VOCAL_THRESHOLD_COUNT; ++i) {
        if (!(m->activity[i] > previous && m->activity[i] < 1))
            ta_fail(c, TA_FAILED, "invalid vocal activity thresholds");
        previous = m->activity[i];
    }
}

static bool valid_motion_lag(double lag, int minimum, int maximum)
{
    return lag >= minimum && lag <= maximum && trunc(lag) == lag;
}

static void model_motion_geometry(ta_context *c, const double *parameters)
{
    double grid = parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT];
    double width = parameters[TA_VOCAL_MOTION_BAND_WIDTH];
    double bands = parameters[TA_VOCAL_MOTION_BAND_COUNT];
    double stride_bytes = parameters[TA_VOCAL_MOTION_BAND_STRIDE_BYTES];
    double stride = stride_bytes / sizeof(double);
    if (!valid_motion_lag(width, 1, UINT16_MAX) ||
        !valid_motion_lag(stride, 1, (int)grid) || width + (bands - 1) * stride > grid ||
        parameters[TA_VOCAL_MOTION_MEAN_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid vocal motion geometry");
    if (!valid_motion_lag(parameters[TA_VOCAL_MOTION_MIN_LAG], 1 - (int)width, 0) ||
        !valid_motion_lag(parameters[TA_VOCAL_MOTION_MAX_LAG], 0, (int)width - 1))
        ta_fail(c, TA_FAILED, "invalid vocal motion lags");
}

static void validate_vocal_fft_shape(ta_context *c, const double *parameters)
{
    double extent = parameters[TA_VOCAL_FRONTEND_FFT_SIZE];
    if (!valid_motion_lag(extent, 8, UINT16_MAX))
        ta_fail(c, TA_FAILED, "invalid vocal FFT extent");
    unsigned size = (unsigned)extent;
    if ((size & (size - 1)) != 0 ||
        !valid_motion_lag(parameters[TA_VOCAL_FEATURE_HOP], 1, (int)size))
        ta_fail(c, TA_FAILED, "invalid vocal FFT shape");
}

static void validate_vocal_frame_geometry(ta_context *c, const double *parameters)
{
    int extent = (int)parameters[TA_VOCAL_FRONTEND_FFT_SIZE];
    int hop = (int)parameters[TA_VOCAL_FEATURE_HOP];
    if (!valid_motion_lag(parameters[TA_VOCAL_FRAME_CENTER], 0, extent) ||
        !valid_motion_lag(parameters[TA_VOCAL_FRAME_LEFT_BACK], 0, extent) ||
        !valid_motion_lag(parameters[TA_VOCAL_FRAME_RIGHT_BACK], 0, extent) ||
        !valid_motion_lag(parameters[TA_VOCAL_FRAME_RIGHT_SCALE], 1, UINT8_MAX) ||
        !valid_motion_lag(parameters[TA_VOCAL_FRAME_ROUNDING_BIAS], 1, hop))
        ta_fail(c, TA_FAILED, "invalid vocal frame geometry");
}

static void validate_vocal_fft_tables(ta_context *c, const double *parameters)
{
    double half = parameters[TA_VOCAL_SINE_HALF_DIVISOR];
    double quarter = parameters[TA_VOCAL_SINE_QUARTER_DIVISOR];
    double eighth = parameters[TA_VOCAL_SINE_EIGHTH_DIVISOR];
    if (half != 2 || quarter != 2 * half || eighth != 2 * quarter ||
        parameters[TA_VOCAL_WINDOW_DENOMINATOR_BIAS] <= 0 ||
        parameters[TA_VOCAL_WINDOW_FIRST_ORDINAL] < 0 ||
        parameters[TA_VOCAL_WINDOW_GAIN] <= 0 || parameters[TA_VOCAL_SINE_ANGLE] <= 0)
        ta_fail(c, TA_FAILED, "invalid vocal FFT table parameters");
}

static void validate_motion_dimensions(ta_context *c, const Model *m)
{
    const double *parameters = m->frontend;
    double bands = parameters[TA_VOCAL_MOTION_BAND_COUNT];
    if (!valid_motion_lag(parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT], 1, UINT16_MAX) ||
        parameters[TA_VOCAL_MOTION_POWER_BINS] != parameters[TA_VOCAL_FRONTEND_FFT_SIZE] / 2 + 1 ||
        !valid_motion_lag(bands, 1, m->feature_columns) ||
        !valid_motion_lag(parameters[TA_VOCAL_MOTION_ROW_STRIDE], (int)bands, m->feature_columns) ||
        parameters[TA_VOCAL_MOTION_LAG_INVERSE_NUMERATOR] <= 0 ||
        !valid_motion_lag(parameters[TA_VOCAL_MOTION_FINAL_ROW_TRIM], 1, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid vocal motion dimensions");
}

static void validate_mfcc_window(ta_context *c, const double *parameters)
{
    double values = parameters[TA_VOCAL_MIN_MFCC_VALUES];
    double radius = parameters[TA_VOCAL_DELTA_RADIUS];
    double columns = parameters[TA_VOCAL_MFCC_COLUMNS];
    if (!valid_motion_lag(radius, 4, UINT8_MAX) ||
        values <= 2 * radius * columns || values > UINT16_MAX || trunc(values) != values)
        ta_fail(c, TA_FAILED, "invalid vocal MFCC window");
}

static void validate_mfcc_taps(ta_context *c, const double *parameters)
{
    int outer = (int)parameters[TA_VOCAL_DELTA_RADIUS];
    double third = parameters[TA_VOCAL_DELTA_THIRD_RADIUS];
    double second = parameters[TA_VOCAL_DELTA_SECOND_RADIUS];
    double first = parameters[TA_VOCAL_DELTA_FIRST_RADIUS];
    double additions = parameters[TA_VOCAL_DELTA_SECOND_ADDITIONS];
    if (!valid_motion_lag(third, 1, outer - 1) ||
        !valid_motion_lag(second, 1, (int)third - 1) ||
        !valid_motion_lag(first, 1, (int)second - 1) ||
        !valid_motion_lag(additions, 1, UINT8_MAX) ||
        parameters[TA_VOCAL_DELTA_FIRST_WEIGHT] <= 0)
        ta_fail(c, TA_FAILED, "invalid vocal MFCC taps");
}

static void validate_vocal_feature_shape(ta_context *c, const Model *m)
{
    const double *parameters = m->frontend;
    double columns = parameters[TA_VOCAL_MFCC_COLUMNS];
    if (!valid_motion_lag(parameters[TA_VOCAL_MEL_FILTER_COUNT], 1, UINT8_MAX) ||
        !valid_motion_lag(columns, 1, UINT8_MAX) ||
        columns + parameters[TA_VOCAL_MOTION_ROW_STRIDE] != m->feature_columns ||
        parameters[TA_VOCAL_DCT_DC_SCALE] <= 0 || parameters[TA_VOCAL_DCT_AC_SCALE] <= 0)
        ta_fail(c, TA_FAILED, "invalid vocal MFCC shape");
}

static void validate_vocal_resampling(ta_context *c, const double *parameters)
{
    double target_rate = parameters[TA_VOCAL_TARGET_SAMPLE_RATE];
    double step = parameters[TA_VOCAL_RESAMPLE_INPUT_PHASE];
    if (!valid_motion_lag(target_rate, 1, UINT16_MAX) ||
        parameters[TA_VOCAL_SOURCE_RATE_RECIPROCAL] != 1 / target_rate ||
        parameters[TA_VOCAL_PCM_AMPLITUDE_SCALE] <= 0 || step <= 0 ||
        parameters[TA_VOCAL_RESAMPLE_PHASE_BACK] != -step ||
        parameters[TA_VOCAL_RESAMPLE_INITIAL_PHASE] != step ||
        !valid_motion_lag(parameters[TA_VOCAL_LAGRANGE_RING_LENGTH], 5, UINT8_MAX))
        ta_fail(c, TA_FAILED, "invalid vocal resampling parameters");
}

static void validate_vocal_pcm_and_windows(ta_context *c, const double *parameters)
{
    double window = parameters[TA_VOCAL_INFERENCE_WINDOW];
    double hop = parameters[TA_VOCAL_INFERENCE_HOP];
    if (!valid_motion_lag(parameters[TA_VOCAL_SOURCE_BLOCK_SECONDS], 1, UINT8_MAX) ||
        parameters[TA_VOCAL_STEREO_MEAN_SCALE] <= 0 ||
        !valid_motion_lag(window, 1, UINT16_MAX) ||
        !valid_motion_lag(hop, 1, (int)window) || window > UINT8_MAX * hop)
        ta_fail(c, TA_FAILED, "invalid vocal PCM or window geometry");
}

static void validate_motion_reduction(ta_context *c, const double *parameters)
{
    double scalar_width = parameters[TA_VOCAL_MOTION_REDUCTION_WIDTH];
    if (!valid_motion_lag(scalar_width, 2, TA_VOCAL_MOTION_REDUCE_PAIRS * 2))
        ta_fail(c, TA_FAILED, "invalid vocal motion reduction width");
    unsigned width = (unsigned)scalar_width, pairs = width / 2;
    if ((width & (width - 1)) != 0)
        ta_fail(c, TA_FAILED, "invalid vocal motion reduction width");
    unsigned seen = 0;
    for (unsigned slot = 0; slot < pairs; ++slot) {
        double group = parameters[TA_VOCAL_MOTION_REDUCE_PAIR_0 + slot];
        if (!valid_motion_lag(group, 0, (int)pairs - 1) || (seen & (1u << (unsigned)group)) != 0)
            ta_fail(c, TA_FAILED, "invalid vocal motion reduction order");
        seen |= 1u << (unsigned)group;
    }
    if (!valid_motion_lag(parameters[TA_VOCAL_MOTION_ENERGY_EPSILON_LANE], 0, (int)width - 1))
        ta_fail(c, TA_FAILED, "invalid vocal motion epsilon placement");
}

static void model_motion(ta_context *c, Model *m, const uint8_t *bytes)
{
    const double *parameters = m->frontend;
    if (memcmp(bytes, TA_MODEL_WINDOWS_MAGIC, sizeof(TA_MODEL_WINDOWS_MAGIC) - 1) != 0) {
        validate_motion_reduction(c, parameters);
        return;
    }
    m->windows_motion = ta_alloc(c, 1, sizeof *m->windows_motion);
    if (!ta_vocal_motion_read(m->windows_motion, bytes + TA_MODEL_BYTES, TA_MODEL_WINDOWS_MOTION_BYTES,
                              (unsigned)parameters[TA_VOCAL_MOTION_BAND_WIDTH],
                              (int)parameters[TA_VOCAL_MOTION_MIN_LAG],
                              (int)parameters[TA_VOCAL_MOTION_MAX_LAG]))
        ta_fail(c, TA_FAILED, "invalid Windows vocal motion recipe");
}

static void model_frontend(ta_context *c, Model *m, const uint8_t *bytes)
{
    memcpy(m->frontend, bytes + TA_MODEL_VOCAL_OFFSET, sizeof m->frontend);
    for (size_t i = 0; i < TA_VOCAL_PARAMETER_COUNT; ++i)
        if (!isfinite(m->frontend[i]))
            ta_fail(c, TA_FAILED, "invalid vocal frontend parameters");
    if (!(m->frontend[TA_VOCAL_DB_POWER_FLOOR] > 0))
        ta_fail(c, TA_FAILED, "invalid vocal power floor");
    double cutoff = m->frontend[TA_VOCAL_INPUT_LOWPASS_HZ];
    if (cutoff <= 0 || cutoff >= AZ_ANALYSIS_POLICY_ALLOWED_RATE_LOW / 2.0)
        ta_fail(c, TA_FAILED, "invalid vocal input low-pass cutoff");
    validate_vocal_fft_shape(c, m->frontend);
    validate_vocal_frame_geometry(c, m->frontend);
    validate_vocal_fft_tables(c, m->frontend);
    validate_motion_dimensions(c, m);
    model_motion_geometry(c, m->frontend);
    model_motion(c, m, bytes);
    validate_vocal_resampling(c, m->frontend);
    validate_vocal_pcm_and_windows(c, m->frontend);
    validate_vocal_feature_shape(c, m);
    validate_mfcc_window(c, m->frontend);
    validate_mfcc_taps(c, m->frontend);
}

static void model_cell_gates(ta_context *c, RecurrentCell *cell, const uint8_t *bytes, int columns)
{
    uint32_t count = ta_le32(bytes + TA_VOCAL_GATE_COUNT * 4);
    if (count != TA_VOCAL_GATE_ROLES || columns % (int)count != 0)
        ta_fail(c, TA_FAILED, "invalid vocal recurrent gate count");
    cell->units = columns / (int)count;
    unsigned seen = 0;
    for (size_t role = 0; role < TA_VOCAL_GATE_ROLES; ++role) {
        uint32_t port = ta_le32(bytes + role * 4);
        if (port >= count || (seen & (1u << port)) != 0)
            ta_fail(c, TA_FAILED, "invalid vocal recurrent gate port");
        seen |= 1u << port;
        cell->gate_offsets[role] = (int)port * cell->units;
    }
}

static void model_cell_state(ta_context *c, RecurrentCell *cell, const uint8_t *bytes, const uint8_t *bias)
{
    memcpy(&cell->forget_bias, bias, sizeof(float));
    memcpy(&cell->initial_cell, bytes + TA_VOCAL_INITIAL_CELL * 4, sizeof(float));
    memcpy(&cell->initial_hidden, bytes + TA_VOCAL_INITIAL_HIDDEN * 4, sizeof(float));
    if (!isfinite(cell->forget_bias) || !isfinite(cell->initial_cell) || !isfinite(cell->initial_hidden))
        ta_fail(c, TA_FAILED, "invalid vocal recurrent state");
}

static const float *model_matrix(ta_context *c, Model *m, size_t i, const uint8_t *shape,
                                 const float *weights, size_t available)
{
    uint32_t rows = ta_le32(shape), columns = ta_le32(shape + 4);
    uint64_t kernel = (uint64_t)rows * columns;
    if (rows == 0 || columns == 0 || kernel > available || columns > available - kernel)
        ta_fail(c, TA_FAILED, "invalid vocal tensor extent");
    matrix_init(c, &m->matrices[i], weights, (int)rows, (int)columns);
    m->biases[i] = ta_alloc(c, columns, sizeof(float));
    memcpy(m->biases[i], weights + kernel, columns * sizeof(float));
    return weights + kernel + columns;
}

static void read_recurrent_branch(ta_context *c, RecurrentBranch *branch, const uint8_t *definition,
                                  const uint8_t *inputs, unsigned *seen)
{
    uint32_t current = ta_le32(definition), reversed = ta_le32(definition + 4);
    if (current == 0 || reversed > 1)
        ta_fail(c, TA_FAILED, "invalid vocal recurrent output");
    branch->reversed = reversed != 0;
    unsigned begin = TA_VOCAL_RECURRENT_CELL_COUNT;
    while (current != 0) {
        if (begin == 0 || current > TA_VOCAL_RECURRENT_CELL_COUNT || (*seen & (1u << (current - 1))) != 0)
            ta_fail(c, TA_FAILED, "invalid vocal recurrent connection");
        *seen |= 1u << (current - 1);
        int index = (int)current - 1;
        branch->cells[--begin] = index;
        current = ta_le32(inputs + index * 4);
    }
    branch->length = TA_VOCAL_RECURRENT_CELL_COUNT - (int)begin;
    memmove(branch->cells, branch->cells + begin, (size_t)branch->length * sizeof *branch->cells);
}

static int recurrent_branch_width(ta_context *c, const Model *m, const RecurrentBranch *branch)
{
    int input = m->feature_columns;
    for (int layer = 0; layer < branch->length; ++layer) {
        int index = branch->cells[layer];
        int units = m->recurrent[index].units;
        if (m->matrices[index].rows != input + units)
            ta_fail(c, TA_FAILED, "inconsistent vocal recurrent widths");
        input = units;
    }
    return input;
}

static void validate_dense_graph(ta_context *c, const Model *m, int output_width)
{
    const Matrix *dense = &m->matrices[TA_VOCAL_DENSE_HIDDEN];
    const Matrix *output = &m->matrices[TA_VOCAL_DENSE_OUTPUT];
    if (dense->rows != output_width || output->rows != dense->columns ||
        m->vocal_class >= output->columns)
        ta_fail(c, TA_FAILED, "inconsistent vocal dense widths or output class");
}

static void model_topology(ta_context *c, Model *m, const uint8_t *bytes)
{
    uint32_t count = ta_le32(bytes + TA_MODEL_RECURRENT_BRANCH_COUNT_OFFSET);
    if (count == 0 || count > TA_VOCAL_RECURRENT_CELL_COUNT)
        ta_fail(c, TA_FAILED, "invalid vocal recurrent output count");
    m->branch_count = (int)count;
    const uint8_t *inputs = bytes + TA_MODEL_RECURRENT_INPUTS_OFFSET;
    const uint8_t *outputs = bytes + TA_MODEL_RECURRENT_BRANCHES_OFFSET;
    int output_width = 0;
    unsigned seen = 0;
    for (size_t i = 0; i < count; ++i) {
        RecurrentBranch *branch = &m->branches[i];
        read_recurrent_branch(c, branch, outputs + i * TA_MODEL_RECURRENT_BRANCH_BYTES, inputs, &seen);
        branch->output_offset = output_width;
        output_width += recurrent_branch_width(c, m, branch);
        m->max_layers = MAX(m->max_layers, branch->length);
    }
    if (seen != (1u << TA_VOCAL_RECURRENT_CELL_COUNT) - 1)
        ta_fail(c, TA_FAILED, "unreachable vocal recurrent cells");
    validate_dense_graph(c, m, output_width);
}

static void model_dimensions(ta_context *c, Model *m, const uint8_t *bytes)
{
    uint32_t width = ta_le32(bytes + TA_MODEL_FEATURE_COLUMNS_OFFSET);
    uint32_t vocal_class = ta_le32(bytes + TA_MODEL_OUTPUT_CLASS_OFFSET);
    if (width == 0 || width > UINT16_MAX || vocal_class > INT_MAX)
        ta_fail(c, TA_FAILED, "invalid vocal input width or output class");
    m->feature_columns = (int)width;
    m->vocal_class = (int)vocal_class;
}

static size_t model_weight_offset(const Model *m)
{
    return ta_model_weights_offset((unsigned)m->frontend[TA_VOCAL_MOTION_BAND_WIDTH],
                                   (unsigned)m->feature_columns);
}

static void model_graph(ta_context *c, Model *m, const uint8_t *bytes)
{
    const float *weights = (const float *)(bytes + model_weight_offset(m));
    const float *end = (const float *)(bytes + TA_MODEL_LEGACY_BYTES);
    const uint8_t *shape = bytes + TA_MODEL_MATRIX_SHAPES_OFFSET;
    for (size_t i = 0; i < TA_VOCAL_MATRIX_COUNT; ++i)
        weights = model_matrix(c, m, i, shape + i * 8, weights, (size_t)(end - weights));
    if (weights != end)
        ta_fail(c, TA_FAILED, "invalid vocal tensor data length");
}

static void model_recurrent(ta_context *c, Model *m, const uint8_t *bytes)
{
    const uint8_t *cells = bytes + TA_MODEL_RECURRENT_CELLS_OFFSET;
    const uint8_t *bias = bytes + TA_MODEL_FORGET_BIAS_OFFSET;
    for (size_t i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i) {
        RecurrentCell *cell = &m->recurrent[i];
        const Matrix *matrix = &m->matrices[i];
        const uint8_t *definition = cells + i * TA_VOCAL_CELL_FIELDS * 4;
        model_cell_gates(c, cell, definition, matrix->columns);
        model_cell_state(c, cell, definition, bias + i * 4);
        m->max_rows = MAX(m->max_rows, matrix->rows);
        m->max_gates = MAX(m->max_gates, matrix->columns);
        m->max_units = MAX(m->max_units, cell->units);
    }
    model_topology(c, m, bytes);
}

static void model_coefficients(ta_context *c, Model *m, const uint8_t *bytes)
{
    size_t count = (model_weight_offset(m) - TA_MODEL_HEADER_BYTES) / sizeof(double);
    m->cheb = ta_alloc(c, count, sizeof *m->cheb);
    memcpy(m->cheb, bytes + TA_MODEL_HEADER_BYTES, count * sizeof *m->cheb);
    m->scale = m->cheb + (unsigned)m->frontend[TA_VOCAL_MOTION_BAND_WIDTH];
    m->offset = m->scale + m->feature_columns;
}

static Model *model_load(ta_context *c)
{
    Model *m = ta_alloc(c, 1, sizeof *m);
    uint8_t *bytes = ta_model_open(c);
    model_activity(c, m, bytes);
    model_dimensions(c, m, bytes);
    model_frontend(c, m, bytes);
    model_graph(c, m, bytes);
    model_recurrent(c, m, bytes);
    model_coefficients(c, m, bytes);
    ta_release(c, bytes);
    return m;
}

static int infer_frame(int step, int length, bool reversed)
{
    return reversed ? length - 1 - step : step;
}

static void infer_join(Scratch *scratch, const Model *m, const float *features, const int *lengths,
                       int batch, int group, int step, const RecurrentBranch *branch, int layer)
{
    int index = branch->cells[layer];
    int rows = m->matrices[index].rows;
    int units = m->recurrent[index].units, width = rows - units;
    int hop = (int)m->frontend[TA_VOCAL_INFERENCE_HOP];
    for (int b = 0; b < batch; b++) {
        int t = infer_frame(step, lengths[b], branch->reversed);
        const float *input = layer ? scratch->hidden[layer - 1] + b * width :
                                     features + (group + b * hop + t) * width;
        float *joined = scratch->joined + b * rows;
        memcpy(joined, input, width * sizeof(float));
        memcpy(joined + width, scratch->hidden[layer] + b * units, units * sizeof(float));
    }
}

static void infer_cells(Scratch *scratch, const RecurrentCell *cell, int columns, int batch, int layer)
{
    int units = cell->units;
    for (int b = 0; b < batch; b++) {
        const float *z = scratch->gates + b * columns;
        const float *input = z + cell->gate_offsets[TA_VOCAL_GATE_INPUT];
        const float *candidate = z + cell->gate_offsets[TA_VOCAL_GATE_CANDIDATE];
        const float *forget = z + cell->gate_offsets[TA_VOCAL_GATE_FORGET];
        const float *output = z + cell->gate_offsets[TA_VOCAL_GATE_OUTPUT];
        float *previous = scratch->cell[layer] + b * units;
        float *hidden = scratch->hidden[layer] + b * units;
        for (int u = 0; u < units; u++) {
            float c = previous[u] * sigmoid(forget[u] + cell->forget_bias) +
                      sigmoid(input[u]) * tanhf(candidate[u]);
            previous[u] = c;
            hidden[u] = tanhf(c) * sigmoid(output[u]);
        }
    }
}

static void infer_save(Scratch *scratch, const Model *m, const int *lengths, int batch,
                       int step, const RecurrentBranch *branch)
{
    int window = (int)m->frontend[TA_VOCAL_INFERENCE_WINDOW];
    int top = branch->length - 1;
    int units = m->recurrent[branch->cells[top]].units;
    int width = m->matrices[TA_VOCAL_DENSE_HIDDEN].rows;
    for (int b = 0; b < batch; b++) {
        int t = infer_frame(step, lengths[b], branch->reversed);
        memcpy(scratch->output + ((size_t)b * window + t) * width + branch->output_offset,
               scratch->hidden[top] + b * units, units * sizeof(float));
    }
}

static void infer_initial_state(Scratch *scratch, const RecurrentCell *cells,
                                const RecurrentBranch *branch, int batch)
{
    for (int layer = 0; layer < branch->length; ++layer) {
        const RecurrentCell *cell = &cells[branch->cells[layer]];
        for (int unit = 0; unit < batch * cell->units; ++unit) {
            scratch->cell[layer][unit] = cell->initial_cell;
            scratch->hidden[layer][unit] = cell->initial_hidden;
        }
    }
}

static void infer_direction(ta_context *c, const Model *m, const float *features, const int *lengths,
                            int batch, int group, const RecurrentBranch *branch, Scratch *scratch)
{
    infer_initial_state(scratch, m->recurrent, branch, batch);
    for (int step = 0; step < lengths[0]; step++) {
        ta_check(c);
        int active = batch;
        while (active && step >= lengths[active - 1]) active--;
        for (int layer = 0; layer < branch->length; layer++) {
            int index = branch->cells[layer];
            const Matrix *matrix = &m->matrices[index];
            infer_join(scratch, m, features, lengths, active, group, step, branch, layer);
            matrix_windows(matrix, scratch->joined, m->biases[index], scratch->gates, active);
            infer_cells(scratch, &m->recurrent[index], matrix->columns, active, layer);
        }
        infer_save(scratch, m, lengths, active, step, branch);
    }
}

static float infer_softmax(float *logits, int columns, int vocal_class)
{
    float maximum = logits[0];
    for (int i = 1; i < columns; ++i)
        maximum = MAX(maximum, logits[i]);
    for (int i = 0; i < columns; ++i)
        logits[i] = expf(logits[i] - maximum);
    float sum = logits[0];
    for (int i = 1; i < columns; ++i)
        sum += logits[i];
    return logits[vocal_class] / sum;
}

static void infer_probabilities(const Model *m, Scratch *scratch, const int *lengths,
                                int batch, int group, float *probabilities)
{
    int hop = (int)m->frontend[TA_VOCAL_INFERENCE_HOP];
    int window = (int)m->frontend[TA_VOCAL_INFERENCE_WINDOW];
    const Matrix *dense = &m->matrices[TA_VOCAL_DENSE_HIDDEN];
    const Matrix *output = &m->matrices[TA_VOCAL_DENSE_OUTPUT];
    for (int b = 0; b < batch; b++)
        for (int t = 0; t < lengths[b]; t++) {
            const float *row = scratch->output + ((size_t)b * window + t) * dense->rows;
            matrix_multiply(dense, row, m->biases[TA_VOCAL_DENSE_HIDDEN], scratch->dense);
            for (int i = 0; i < dense->columns; i++)
                scratch->dense[i] = MAX(0.0f, scratch->dense[i]);
            matrix_multiply(output, scratch->dense, m->biases[TA_VOCAL_DENSE_OUTPUT], scratch->logits);
            probabilities[(group / hop + b) * window + t] =
                infer_softmax(scratch->logits, output->columns, m->vocal_class);
        }
}
static void infer_group(ta_context *c, const Model *m, const float *features, int count, int group,
                        Scratch *scratch, float *probabilities)
{
    int hop = (int)m->frontend[TA_VOCAL_INFERENCE_HOP];
    int window = (int)m->frontend[TA_VOCAL_INFERENCE_WINDOW];
    int batch = MIN(WINDOW_BATCH, (count - group + hop - 1) / hop), lengths[WINDOW_BATCH] = { 0 };
    for (int b = 0; b < batch; b++) lengths[b] = MIN(window, count - group - b * hop);
    for (int branch = 0; branch < m->branch_count; ++branch)
        infer_direction(c, m, features, lengths, batch, group, &m->branches[branch], scratch);
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
    int hop = (int)job->model->frontend[TA_VOCAL_INFERENCE_HOP];
    infer_group(c, job->model, job->features, job->count, (int)group * (WINDOW_BATCH * hop),
                &job->scratch[worker], job->probabilities);
}

static void infer_scratch_alloc(ta_context *c, const Model *m, Scratch *scratch, int window)
{
    scratch->output = ta_alloc(c, (size_t)WINDOW_BATCH * window * m->matrices[TA_VOCAL_DENSE_HIDDEN].rows,
                               sizeof(float));
    scratch->joined = ta_alloc(c, WINDOW_BATCH * m->max_rows, sizeof(float));
    scratch->gates = ta_alloc(c, WINDOW_BATCH * m->max_gates, sizeof(float));
    scratch->dense = ta_alloc(c, m->matrices[TA_VOCAL_DENSE_HIDDEN].columns, sizeof(float));
    scratch->logits = ta_alloc(c, m->matrices[TA_VOCAL_DENSE_OUTPUT].columns, sizeof(float));
    for (int layer = 0; layer < m->max_layers; ++layer) {
        scratch->hidden[layer] = ta_alloc(c, WINDOW_BATCH * m->max_units, sizeof(float));
        scratch->cell[layer] = ta_alloc(c, WINDOW_BATCH * m->max_units, sizeof(float));
    }
}

static void infer_scratch_release(ta_context *c, const Model *m, Scratch *scratch)
{
    ta_release(c, scratch->output);
    ta_release(c, scratch->joined);
    ta_release(c, scratch->gates);
    ta_release(c, scratch->dense);
    ta_release(c, scratch->logits);
    for (int layer = 0; layer < m->max_layers; ++layer) {
        ta_release(c, scratch->hidden[layer]);
        ta_release(c, scratch->cell[layer]);
    }
}

static float *model_infer(ta_context *c, const Model *m, const float *features, int count)
{
    int hop = (int)m->frontend[TA_VOCAL_INFERENCE_HOP];
    int window = (int)m->frontend[TA_VOCAL_INFERENCE_WINDOW];
    int windows = (count + hop - 1) / hop;
    float *probabilities = ta_alloc(c, (size_t)windows * window, sizeof(float));
    unsigned groups = (unsigned)((windows + WINDOW_BATCH - 1) / WINDOW_BATCH);
    unsigned threads = c->execution.vocal_threads ? c->execution.vocal_threads : 1;
    if (threads > groups) threads = groups;
    inference_job job = { .model = m, .features = features, .count = count, .probabilities = probabilities };
    /* Allocate private scratch before launching, as in the original worker. */
    for (unsigned i = 0; i < threads; i++)
        infer_scratch_alloc(c, m, &job.scratch[i], window);
    ta_parallel(c, groups, threads, infer_task, &job);
    float *scores = ta_alloc(c, count, sizeof(float));
    uint8_t *contributors = ta_alloc(c, count, 1);
    for (int w = 0; w < windows; w++) {
        int first = w * hop, length = MIN(window, count - first);
        for (int t = 0; t < length; t++) {
            scores[first + t] += probabilities[w * window + t];
            contributors[first + t]++;
        }
    }
    for (int i = 0; i < count; i++)
        scores[i] /= (float)contributors[i];
    ta_release(c, probabilities);
    ta_release(c, contributors);
    for (unsigned i = 0; i < threads; i++)
        infer_scratch_release(c, m, &job.scratch[i]);
    return scores;
}
static inline int next_ring_index(int current, int size)
{
    int next = current + 1;
    return next == size ? 0 : next;
}

static float lagrange(const float *r, int p, float t, const double *parameters)
{
    int size = (int)parameters[TA_VOCAL_LAGRANGE_RING_LENGTH];
    int a = next_ring_index(p, size), b = next_ring_index(a, size);
    int c = next_ring_index(b, size), d = next_ring_index(c, size);
    float third_scale = (float)parameters[TA_VOCAL_LAGRANGE_THIRD];
    float half_offset = (float)parameters[TA_VOCAL_LAGRANGE_HALF_OFFSET];
    float half_scale = (float)parameters[TA_VOCAL_LAGRANGE_HALF_MULTIPLIER];
    float positive_outer = (float)parameters[TA_VOCAL_LAGRANGE_OUTER_POSITIVE];
    float negative_outer = (float)parameters[TA_VOCAL_LAGRANGE_OUTER_NEGATIVE];
    float two_thirds = (float)parameters[TA_VOCAL_LAGRANGE_TWO_THIRDS];
    float third = t * third_scale, quarter = t * (float)parameters[TA_VOCAL_LAGRANGE_QUARTER];
    float half = t * half_scale;
    float mid = (float)parameters[TA_VOCAL_LAGRANGE_CENTER] - t;
    mid = fmaf(t, mid, mid);
    mid = fmaf(half, mid, mid);
    mid = fmaf(-half, mid, mid);
    float left1 = ((half - half_offset) * (two_thirds - third)) * r[a];
    left1 = (t + positive_outer) * left1;
    float left2 = ((third - third_scale) * (half_offset - quarter)) * r[p];
    left2 = (negative_outer - t) * left2;
    float right1 = (positive_outer - t) * ((third + two_thirds) * (half + half_offset));
    right1 = right1 * r[c];
    float right2 = (t + negative_outer) * ((quarter + half_offset) * (third + third_scale));
    right2 = right2 * r[d];
    float side = (right1 + left1) + (right2 + left2) * half_scale;
    return fmaf(side, t, mid * r[b]);
}
static uint64_t vocal_resampled_count(uint64_t frames, unsigned rate, const double *parameters)
{
    return frames * (unsigned)parameters[TA_VOCAL_TARGET_SAMPLE_RATE] / rate;
}

static void frontend_sine_table(Frontend *f)
{
    const double *parameters = f->parameters;
    int size = f->fft_size;
    int half = size / (int)parameters[TA_VOCAL_SINE_HALF_DIVISOR];
    int quarter = size / (int)parameters[TA_VOCAL_SINE_QUARTER_DIVISOR];
    int eighth = size / (int)parameters[TA_VOCAL_SINE_EIGHTH_DIVISOR];
    float unity = (float)parameters[TA_VOCAL_SINE_UNITY];
    float s = (float)sin(parameters[TA_VOCAL_SINE_ANGLE] / size);
    float sq = s * s, step = sq * (float)parameters[TA_VOCAL_SINE_RECURSION_GAIN], cd = sq + sq;
    float sd = (float)sqrt((parameters[TA_VOCAL_SINE_RECURSION_BIAS] - (double)cd) * (double)cd);
    float cv = unity, sv = 0;
    f->sine[0] = 0;
    f->sine[quarter] = unity;
    for (int i = 1; i < eighth; i++) {
        cv = cv - cd;
        cd = fmaf(cv, step, cd);
        sv = sv + sd;
        sd = fmaf(-sv, step, sd);
        f->sine[i] = sv;
        f->sine[quarter - i] = cv;
    }
    f->sine[eighth] = (float)parameters[TA_VOCAL_SINE_DIAGONAL];
    for (int i = 0; i < quarter; i++)
        f->sine[half - i] = f->sine[i];
    for (int i = 0; i < half + quarter; i++)
        f->sine[i + half] = -f->sine[i];
}

static void frontend_fft_plan(Frontend *f)
{
    const double *parameters = f->parameters;
    int size = f->fft_size;
    int quarter = size / (int)parameters[TA_VOCAL_SINE_QUARTER_DIVISOR];
    double step = parameters[TA_VOCAL_WINDOW_ANGLE] / (size + parameters[TA_VOCAL_WINDOW_DENOMINATOR_BIAS]);
    double ordinal = parameters[TA_VOCAL_WINDOW_FIRST_ORDINAL];
    double gain = parameters[TA_VOCAL_WINDOW_GAIN];
    for (int i = 0; i < size; i++)
        f->window[i] = (float)fma(-cos(step * (i + ordinal)), gain, gain);
    unsigned bits = 0;
    for (unsigned remaining = (unsigned)size; remaining > 1; remaining >>= 1)
        bits++;
    for (int i = 0; i < size; i++) {
        unsigned reversed = 0;
        for (unsigned bit = 0; bit < bits; bit++)
            reversed = (reversed << 1) | (((unsigned)i >> bit) & 1u);
        f->permutation[i] = (uint16_t)reversed;
    }
    for (int half = 1; half < size; half *= 2)
        for (int k = 0; k < half; k++) {
            int a = k * size / (2 * half);
            f->fft_cos[half - 1 + k] = f->sine[a + quarter];
            f->fft_sin[half - 1 + k] = f->sine[a];
        }
}

static Frontend *frontend_init(ta_context *c, unsigned rate, uint64_t frames, const double *parameters)
{
    unsigned ring_length = (unsigned)parameters[TA_VOCAL_LAGRANGE_RING_LENGTH];
    Frontend *f = ta_alloc(c, 1, sizeof *f + ring_length * sizeof f->ring[0]);
    f->parameters = parameters;
    f->sample_rate = rate;
    f->phase = parameters[TA_VOCAL_RESAMPLE_INITIAL_PHASE];
    f->ratio = (double)rate * parameters[TA_VOCAL_SOURCE_RATE_RECIPROCAL];
    f->samples = ta_alloc(c, (size_t)vocal_resampled_count(frames, rate, parameters), sizeof(float));
    int size = (int)parameters[TA_VOCAL_FRONTEND_FFT_SIZE];
    f->fft_size = size;
    f->sine = ta_alloc(c, size + size / (int)parameters[TA_VOCAL_SINE_QUARTER_DIVISOR], sizeof(float));
    f->window = ta_alloc(c, size, sizeof(float));
    f->permutation = ta_alloc(c, size, sizeof(uint16_t));
    f->fft_cos = ta_alloc(c, size - 1, sizeof(float));
    f->fft_sin = ta_alloc(c, size - 1, sizeof(float));

    double lowpass_hz = parameters[TA_VOCAL_INPUT_LOWPASS_HZ];
    double k = 1 / tan(M_PI * lowpass_hz / rate), kk = k * k, q = k * sqrt(2.0),
           norm = 1 / ((q + 1) + kk), twice = norm + norm;
    f->b0 = f->b2 = (float)(norm);
    f->b1 = (float)(twice);
    f->a1 = (float)(fma(-kk, twice, twice));
    f->a2 = (float)(norm * ((1 - q) + kk));
    frontend_sine_table(f);
    frontend_fft_plan(f);
    return f;
}
static void frontend_add(ta_context *c, Frontend *f, const float *in, int n)
{
    const double *parameters = f->parameters;
    float scale = (float)parameters[TA_VOCAL_PCM_AMPLITUDE_SCALE];
    float *x = ta_alloc(c, n, sizeof(float));
    for (int i = 0; i < n; i++) {
        float a = in[i] * scale, y = fmaf(a, f->b0, f->z1);
        x[i] = y;
        f->z1 = fmaf(-y, f->a1, fmaf(a, f->b1, f->z2));
        f->z2 = fmaf(a, f->b2, -(y * f->a2));
    }
    int pos = 0, out = (int)vocal_resampled_count((unsigned)n, f->sample_rate, parameters);
    int ring_length = (int)parameters[TA_VOCAL_LAGRANGE_RING_LENGTH];
    double trigger = parameters[TA_VOCAL_RESAMPLE_INPUT_PHASE];
    for (int i = 0; i < out; i++) {
        while (f->phase >= trigger) {
            if (pos >= n)
                ta_fail(c, TA_FAILED, "vocal resampling exceeded its source block");
            f->ring[f->rp] = x[pos++];
            f->rp = next_ring_index(f->rp, ring_length);
            f->phase += parameters[TA_VOCAL_RESAMPLE_PHASE_BACK];
        }
        f->samples[f->count++] = lagrange(f->ring, f->rp, (float)f->phase, parameters);
        f->phase += f->ratio;
    }
    ta_release(c, x);
}
static void features_mel_filters(Features *f)
{
    const double *parameters = f->model->frontend;
    int bands = (int)parameters[TA_VOCAL_MEL_FILTER_COUNT];
    int bins = (int)parameters[TA_VOCAL_MOTION_POWER_BINS];
    int edge_count = bands + 2; /* Triangles use a left, center and right vertex. */
    double hz[edge_count];
    double sample_rate = parameters[TA_VOCAL_TARGET_SAMPLE_RATE];
    double half_size = parameters[TA_VOCAL_FRONTEND_FFT_SIZE] / 2;
    double maxmel = log(sample_rate * parameters[TA_VOCAL_MEL_SAMPLE_FACTOR]) *
                    parameters[TA_VOCAL_MEL_LOG_SLOPE] + parameters[TA_VOCAL_MEL_BREAK_VALUE];
    for (int i = 0; i < edge_count; i++) {
        double v = maxmel * i;
        hz[i] = v * parameters[TA_VOCAL_MEL_INDEX_SCALE] < parameters[TA_VOCAL_MEL_BREAK_VALUE]
                    ? v * parameters[TA_VOCAL_MEL_LOW_SCALE]
                    : exp(v * parameters[TA_VOCAL_MEL_HIGH_SCALE] +
                          parameters[TA_VOCAL_MEL_HIGH_OFFSET]) * parameters[TA_VOCAL_MEL_BREAK_HZ];
    }
    for (int b = 0; b < bands; b++)
        for (int k = 0; k < bins; k++) {
            double frequency = k * (sample_rate / 2) / half_size, span = 1.0 / (hz[b + 2] - hz[b]);
            f->filters[b * bins + k] =
                MAX(0.0, MIN((frequency - hz[b]) * (1.0 / (hz[b + 1] - hz[b])),
                             (hz[b + 2] - frequency) * (1.0 / (hz[b + 2] - hz[b + 1])))) *
                (span + span);
        }
}
static void features_mel_support(Features *f)
{
    // Mel triangles have compact support. Omitting exact-zero terms keeps
    // the same summation order while avoiding most of the 128x1025 products.
    int bands = (int)f->model->frontend[TA_VOCAL_MEL_FILTER_COUNT];
    int bins = (int)f->model->frontend[TA_VOCAL_MOTION_POWER_BINS];
    for (int b = 0; b < bands; b++) {
        const double *filter = f->filters + b * bins;
        f->firstBin[b] = 0;
        f->lastBin[b] = bins;
        while (f->firstBin[b] < bins && filter[f->firstBin[b]] == 0)
            f->firstBin[b]++;
        while (f->lastBin[b] > f->firstBin[b] && filter[f->lastBin[b] - 1] == 0)
            f->lastBin[b]--;
    }
}
static void features_log_bins(ta_context *c, Features *f)
{
    const double *parameters = f->model->frontend;
    const double bin_width = parameters[TA_VOCAL_TARGET_SAMPLE_RATE] / parameters[TA_VOCAL_FRONTEND_FFT_SIZE];
    double last_bin = parameters[TA_VOCAL_MOTION_POWER_BINS] - 1;
    int bins = (int)parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT];
    for (int k = 0; k < bins; k++) {
        double hz = parameters[TA_VOCAL_LOG_BIN_BASE] * exp2(k * parameters[TA_VOCAL_LOG_BIN_STEP]);
        double upper_bin = ceil(hz / bin_width);
        if (!(upper_bin >= 1 && upper_bin <= last_bin))
            ta_fail(c, TA_FAILED, "vocal log-frequency bin is out of range");
        f->left[k] = (int)upper_bin - 1;
        f->fraction[k] = (hz - f->left[k] * bin_width) / bin_width;
    }
}

static void features_motion_basis(ta_context *c, Features *f)
{
    const double *parameters = f->model->frontend;
    int bands = (int)parameters[TA_VOCAL_MEL_FILTER_COUNT];
    int columns = (int)parameters[TA_VOCAL_MFCC_COLUMNS];
    double angle = M_PI / (2 * bands);
    for (int column = 0; column < columns; column++) {
        double scale = parameters[column ? TA_VOCAL_DCT_AC_SCALE : TA_VOCAL_DCT_DC_SCALE];
        for (int b = 0; b < bands; b++)
            f->dct[column * bands + b] = cos(column * (b * 2 + 1) * angle) * scale;
    }
    for (int i = 0; i < (int)parameters[TA_VOCAL_MOTION_BAND_WIDTH]; i++)
        f->bartlett[i] = fma(-fabs(i + parameters[TA_VOCAL_BARTLETT_CENTER]),
                            parameters[TA_VOCAL_BARTLETT_SLOPE], parameters[TA_VOCAL_BARTLETT_INTERCEPT]);
    features_log_bins(c, f);
}

static void features_alloc_motion(ta_context *c, Features *f, int count)
{
    const double *parameters = f->model->frontend;
    size_t bands = (size_t)parameters[TA_VOCAL_MOTION_BAND_COUNT];
    size_t width = (size_t)parameters[TA_VOCAL_MOTION_BAND_WIDTH];
    size_t grid = (size_t)parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT];
    size_t bins = (size_t)parameters[TA_VOCAL_MOTION_POWER_BINS];
    size_t row = (size_t)parameters[TA_VOCAL_MOTION_ROW_STRIDE];
    size_t motion_values = (size_t)count * row;
    f->motion = ta_alloc(c, motion_values, sizeof(double));
    f->motion_rows = MAX(0, count - (int)parameters[TA_VOCAL_MOTION_FINAL_ROW_TRIM]);
    for (size_t i = 0; i < motion_values; ++i)
        f->motion[i] = parameters[TA_VOCAL_MOTION_UNWRITTEN_VALUE];
    f->bartlett = ta_alloc(c, width, sizeof(double));
    f->previous = ta_alloc(c, bands * width, sizeof(double));
    f->weighted = ta_alloc(c, width, sizeof(double));
    f->gate = ta_alloc(c, bands, sizeof(bool));
    f->left = ta_alloc(c, grid, sizeof(int));
    f->fraction = ta_alloc(c, grid, sizeof(double));
    f->magnitude = ta_alloc(c, bins, sizeof(double));
    f->log_magnitude = ta_alloc(c, grid, sizeof(double));
    f->log_bins = ta_alloc(c, grid, sizeof(double));
}

static Features *features_init(ta_context *c, const Model *model, int count)
{
    Features *f = ta_alloc(c, 1, sizeof *f);
    f->model = model;
    size_t bands = (size_t)model->frontend[TA_VOCAL_MEL_FILTER_COUNT];
    size_t bins = (size_t)model->frontend[TA_VOCAL_MOTION_POWER_BINS];
    size_t columns = (size_t)model->frontend[TA_VOCAL_MFCC_COLUMNS];
    f->mel = ta_alloc(c, (size_t)count * bands, sizeof(double));
    f->filters = ta_alloc(c, bands * bins, sizeof(double));
    f->dct = ta_alloc(c, columns * bands, sizeof(double));
    f->firstBin = ta_alloc(c, bands, sizeof(int));
    f->lastBin = ta_alloc(c, bands, sizeof(int));
    features_alloc_motion(c, f, count);
    features_mel_filters(f);
    features_mel_support(f);
    features_motion_basis(c, f);
    return f;
}
static void features_mel_add(Features *f, const double *power)
{
    int bands = (int)f->model->frontend[TA_VOCAL_MEL_FILTER_COUNT];
    int bins = (int)f->model->frontend[TA_VOCAL_MOTION_POWER_BINS];
    for (int b = 0; b < bands; b++) {
        const double *filter = f->filters + b * bins;
        double sum = 0;
        for (int k = f->firstBin[b]; k < f->lastBin[b]; k++)
            sum += power[k] * filter[k];
        f->mel[f->count * bands + b] = sum;
        f->maximum = MAX(f->maximum, sum);
    }
}
static void features_log_spectrum(Features *f, const double *power)
{
    const double *parameters = f->model->frontend;
    double eps = f->model->windows_motion ? f->model->windows_motion->log_epsilon :
                                           parameters[TA_VOCAL_MOTION_LOG_EPSILON];
    int power_bins = (int)parameters[TA_VOCAL_MOTION_POWER_BINS];
    double *mag = f->magnitude;
    int grid_bins = (int)parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT];
    for (int i = 0; i < power_bins; i++)
        mag[i] = sqrt(power[i]);
    for (int i = 0; i < grid_bins; i++) {
        f->log_magnitude[i] = f->fraction[i] * (mag[f->left[i] + 1] - mag[f->left[i]]) + mag[f->left[i]];
        /* Overlapping bands share these logarithms. */
        f->log_bins[i] = log(f->log_magnitude[i] + eps);
    }
}
static double motion_dot(const double *previous, const double *current, int width, int lag)
{
    double sum = 0;
    int first = MAX(0, -lag), end = MIN(width, width - lag);
    for (int k = first; k < end; k++)
        sum += previous[k + lag] * current[k];
    return sum;
}

static void motion_choose_lag(double correlation, int lag, double *maximum, int *best)
{
    if (!(correlation <= *maximum))
        *best = lag;
    *maximum = fmax(*maximum, correlation);
}

static int features_best_lag(const double *previous, const double *current, const double *parameters)
{
    int first = (int)parameters[TA_VOCAL_MOTION_MIN_LAG];
    int last = (int)parameters[TA_VOCAL_MOTION_MAX_LAG];
    int width = (int)parameters[TA_VOCAL_MOTION_BAND_WIDTH];
    double zero_lag = parameters[TA_VOCAL_MOTION_ZERO_LAG];
    double inverse = parameters[TA_VOCAL_MOTION_LAG_INVERSE_NUMERATOR] /
                     motion_dot(previous, current, width, 0);
    int best = first;
    double maximum = first ? motion_dot(previous, current, width, first) * inverse : zero_lag;
    for (int lag = first + 1; lag <= last; lag++) {
        double correlation = lag ? motion_dot(previous, current, width, lag) * inverse : zero_lag;
        motion_choose_lag(correlation, lag, &maximum, &best);
    }
    return best;
}

static double motion_reduce_lanes(const double *lanes, const double *parameters)
{
    int pairs = (int)parameters[TA_VOCAL_MOTION_REDUCTION_WIDTH] / 2;
    const double *order = parameters + TA_VOCAL_MOTION_REDUCE_PAIR_0;
    int first = 2 * (int)order[0];
    double even = lanes[first], odd = lanes[first + 1];
    for (int i = 1; i < pairs; ++i) {
        int pair = 2 * (int)order[i];
        even += lanes[pair];
        odd += lanes[pair + 1];
    }
    return even + odd;
}

static double motion_energy_ratio(const double *spectrum, const double *window, int width,
                                  const double *parameters)
{
    int lanes = (int)parameters[TA_VOCAL_MOTION_REDUCTION_WIDTH];
    double magnitude[lanes], energy[lanes];
    memset(magnitude, 0, sizeof magnitude);
    memset(energy, 0, sizeof energy);
    int seed = (int)parameters[TA_VOCAL_MOTION_ENERGY_EPSILON_LANE];
    magnitude[seed] = parameters[TA_VOCAL_MOTION_ENERGY_EPSILON];
    int lane_mask = lanes - 1;
    for (int i = 0; i < width; i++) {
        int lane = i & lane_mask;
        magnitude[lane] += spectrum[i];
        energy[lane] = fma(window[i], spectrum[i], energy[lane]);
    }
    return motion_reduce_lanes(energy, parameters) / motion_reduce_lanes(magnitude, parameters);
}

static bool motion_flatness_ok(const double *spectrum, const double *log_bins, int width,
                               const double *parameters)
{
    int lanes = (int)parameters[TA_VOCAL_MOTION_REDUCTION_WIDTH];
    double magnitude[lanes], logs[lanes];
    memset(magnitude, 0, sizeof magnitude);
    memset(logs, 0, sizeof logs);
    int lane_mask = lanes - 1;
    for (int i = 0; i < width; i++) {
        int lane = i & lane_mask;
        magnitude[lane] += spectrum[i];
        logs[lane] += log_bins[i];
    }
    double mean_scale = parameters[TA_VOCAL_MOTION_MEAN_SCALE];
    double mean = fma(motion_reduce_lanes(magnitude, parameters), mean_scale, parameters[TA_VOCAL_MOTION_MEAN_EPSILON]);
    double flatness = exp(motion_reduce_lanes(logs, parameters) * mean_scale) / mean;
    return !(flatness >= parameters[TA_VOCAL_MOTION_FLATNESS_MAX]);
}

static int model_motion_lag(const Model *model, const double *previous, const double *following)
{
    if (model->windows_motion)
        return ta_vocal_motion_lag(model->windows_motion, previous, following);
    return features_best_lag(previous, following, model->frontend);
}

static bool model_motion_gate(const Model *model, const double *spectrum, const double *logs, int width)
{
    if (model->windows_motion)
        return ta_vocal_motion_gate(model->windows_motion, spectrum, model->cheb, logs);
    double energy = motion_energy_ratio(spectrum, model->cheb, width, model->frontend);
    return energy > model->frontend[TA_VOCAL_MOTION_ENERGY_MIN] &&
           motion_flatness_ok(spectrum, logs, width, model->frontend);
}

static void features_motion_add(Features *f, int band, int row)
{
    const double *parameters = f->model->frontend;
    int width = (int)parameters[TA_VOCAL_MOTION_BAND_WIDTH];
    int stride = (int)parameters[TA_VOCAL_MOTION_BAND_STRIDE_BYTES] / (int)sizeof(double);
    int start = band * stride;
    const double *spectrum = f->log_magnitude + start;
    double *previous = f->previous + band * width;
    for (int k = 0; k < width; k++)
        f->weighted[k] = spectrum[k] * f->bartlett[k];
    if (f->count > 0 && f->count - 1 < f->motion_rows && f->gate[band])
        f->motion[row - (int)parameters[TA_VOCAL_MOTION_ROW_STRIDE] + band] =
            model_motion_lag(f->model, previous, f->weighted);
    f->gate[band] = model_motion_gate(f->model, spectrum, f->log_bins + start, width);
    memcpy(previous, f->weighted, (size_t)width * sizeof(double));
}
static void features_add(Features *f, const double *power)
{
    features_mel_add(f, power);
    features_log_spectrum(f, power);
    const double *parameters = f->model->frontend;
    int row = f->count * (int)parameters[TA_VOCAL_MOTION_ROW_STRIDE];
    int bands = (int)parameters[TA_VOCAL_MOTION_BAND_COUNT];
    for (int band = 0; band < bands; band++)
        features_motion_add(f, band, row);
    f->count++;
}
/* All butterflies in a stage are independent. Visit contiguous butterflies
 * together while retaining each scalar multiply, explicit FMA and addition. */
static void frontend_fft(const Frontend *f, float *real, float *imag)
{
    int size = f->fft_size;
    for (int half = 1; half < size; half *= 2) {
        const float *cosine = f->fft_cos + half - 1;
        const float *sine = f->fft_sin + half - 1;
        for (int base = 0; base < size; base += 2 * half) {
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
static void frontend_window_input(const Frontend *f, int frame, float *real)
{
    const double *parameters = f->parameters;
    long start = (long)frame * (int)parameters[TA_VOCAL_FEATURE_HOP] - (int)parameters[TA_VOCAL_FRAME_CENTER];
    long left = -(int)parameters[TA_VOCAL_FRAME_LEFT_BACK];
    long right = (long)f->count * (int)parameters[TA_VOCAL_FRAME_RIGHT_SCALE] -
                 (int)parameters[TA_VOCAL_FRAME_RIGHT_BACK];
    /* Window directly into bit-reversed order, eliminating a second traversal. */
    for (int i = 0; i < f->fft_size; i++) {
        long j = start + i;
        if (j < 0) j = left - j;
        if (j >= (long)f->count) j = right - j;
        real[f->permutation[i]] = (j >= 0 && j < (long)f->count ? f->samples[j] : 0) * f->window[i];
    }
}

static void frontend_frame(const Frontend *f, int frame, double *output)
{
    int size = f->fft_size;
    int bins = (int)f->parameters[TA_VOCAL_MOTION_POWER_BINS];
    float real[size], imag[size];
    memset(imag, 0, sizeof imag);
    frontend_window_input(f, frame, real);
    frontend_fft(f, real, imag);
    int i = 0;
#if defined(__aarch64__)
    for (; i + 4 <= bins; i += 4) {
        float32x4_t r = vld1q_f32(real + i), im = vld1q_f32(imag + i);
        float32x4_t power = vfmaq_f32(vmulq_f32(r, r), im, im);
        vst1q_f64(output + i, vcvt_f64_f32(vget_low_f32(power)));
        vst1q_f64(output + i + 2, vcvt_f64_f32(vget_high_f32(power)));
    }
#endif
    for (; i < bins; i++)
        output[i] = (double)fmaf(imag[i], imag[i], real[i] * real[i]);
}

static int frontend_frame_count(const Frontend *f)
{
    int hop = (int)f->parameters[TA_VOCAL_FEATURE_HOP];
    int bias = (int)f->parameters[TA_VOCAL_FRAME_ROUNDING_BIAS];
    return (f->count + hop - bias) / hop;
}

static void frontend_power(ta_context *c, const Frontend *f, Features *features)
{
    double output[(int)f->parameters[TA_VOCAL_MOTION_POWER_BINS]];
    memset(output, 0, sizeof output);
    int count = frontend_frame_count(f);
    for (int frame = 0; frame < count; frame++) {
        ta_check(c);
        frontend_frame(f, frame, output);
        features_add(features, output);
    }
}

static void frontend_release(ta_context *c, Frontend *f)
{
    ta_release(c, f->samples);
    ta_release(c, f->sine);
    ta_release(c, f->window);
    ta_release(c, f->permutation);
    ta_release(c, f->fft_cos);
    ta_release(c, f->fft_sin);
    ta_release(c, f);
}
typedef struct {
    int first, second, third, outer, second_additions;
    double first_weight, third_weight, fourth_weight, scale;
} DeltaKernel;

static DeltaKernel delta_kernel(const double *parameters, int columns)
{
    return (DeltaKernel) {
        .first = columns * (int)parameters[TA_VOCAL_DELTA_FIRST_RADIUS],
        .second = columns * (int)parameters[TA_VOCAL_DELTA_SECOND_RADIUS],
        .third = columns * (int)parameters[TA_VOCAL_DELTA_THIRD_RADIUS],
        .outer = columns * (int)parameters[TA_VOCAL_DELTA_RADIUS],
        .second_additions = (int)parameters[TA_VOCAL_DELTA_SECOND_ADDITIONS],
        .first_weight = parameters[TA_VOCAL_DELTA_FIRST_WEIGHT],
        .third_weight = parameters[TA_VOCAL_DELTA_THIRD_WEIGHT],
        .fourth_weight = parameters[TA_VOCAL_DELTA_FOURTH_WEIGHT],
        .scale = parameters[TA_VOCAL_DELTA_SCALE]
    };
}

static double mfcc_delta(const DeltaKernel *kernel, const double *center)
{
    double sum = (center[kernel->first] - center[-kernel->first]) * kernel->first_weight;
    double second = center[kernel->second] - center[-kernel->second];
    for (int i = 0; i < kernel->second_additions; ++i)
        sum += second;
    sum += (center[kernel->third] - center[-kernel->third]) * kernel->third_weight;
    sum += (center[kernel->outer] - center[-kernel->outer]) * kernel->fourth_weight;
    return sum * kernel->scale;
}

static float *features_finish(ta_context *c, const Features *f)
{
    const double *parameters = f->model->frontend;
    int count = f->count;
    int columns = (int)parameters[TA_VOCAL_MFCC_COLUMNS];
    int bands = (int)parameters[TA_VOCAL_MEL_FILTER_COUNT];
    if (count * columns < parameters[TA_VOCAL_MIN_MFCC_VALUES])
        ta_fail(c, TA_FAILED, "PVDI input is shorter than the MFCC window");
    double *cepstrum = ta_alloc(c, (size_t)count * columns, sizeof(double));
    int radius = (int)parameters[TA_VOCAL_DELTA_RADIUS];
    DeltaKernel delta = delta_kernel(parameters, columns);
    int motion_stride = (int)parameters[TA_VOCAL_MOTION_ROW_STRIDE];
    double floor_power = parameters[TA_VOCAL_DB_POWER_FLOOR];
    double ref = log10(MAX(f->maximum, floor_power));
    for (int t = 0; t < count; t++) {
        double db[bands];
        for (int k = 0; k < bands; k++)
            db[k] = MAX(parameters[TA_VOCAL_DB_FLOOR],
                        (log10(MAX(f->mel[t * bands + k], floor_power)) - ref) * parameters[TA_VOCAL_DB_SCALE]);
        for (int column = 0; column < columns; column++) {
            const double *weights = f->dct + column * bands;
            double s = 0;
            for (int k = 0; k < bands; k++)
                s += db[k] * weights[k];
            cepstrum[t * columns + column] = s;
        }
    }
    int features_per_frame = columns + motion_stride;
    float *result = ta_alloc(c, (size_t)count * features_per_frame, sizeof(float));
    for (int t = 0; t < count; t++) {
        int center = MAX(radius, MIN(count - radius - 1, t));
        for (int component = 0; component < features_per_frame; component++) {
            double value = component < columns ? mfcc_delta(&delta, cepstrum + center * columns + component) :
                                                 f->motion[t * motion_stride + component - columns];
            result[t * features_per_frame + component] =
                (float)(fma(value, f->model->scale[component], f->model->offset[component]));
        }
    }
    ta_release(c, cepstrum);
    return result;
}

static void features_release(ta_context *c, Features *f)
{
    ta_release(c, f->mel);
    ta_release(c, f->motion);
    ta_release(c, f->bartlett);
    ta_release(c, f->previous);
    ta_release(c, f->weighted);
    ta_release(c, f->gate);
    ta_release(c, f->left);
    ta_release(c, f->fraction);
    ta_release(c, f->magnitude);
    ta_release(c, f->log_magnitude);
    ta_release(c, f->log_bins);
    ta_release(c, f->filters);
    ta_release(c, f->dct);
    ta_release(c, f->firstBin);
    ta_release(c, f->lastBin);
    ta_release(c, f);
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
    const double *parameters = frontend->parameters;
    size_t block = rate * (size_t)parameters[TA_VOCAL_SOURCE_BLOCK_SECONDS];
    float mean_scale = (float)parameters[TA_VOCAL_STEREO_MEAN_SCALE];
    int16_t *pcm = ta_alloc(c, block * 2, sizeof(int16_t));
    float *mono = ta_alloc(c, block, sizeof(float));
    uint64_t received = 0;
    while (received < frames) {
        ta_check(c);
        uint64_t need = MIN((uint64_t)block, frames - received), have = 0;
        have = vocals_decode_chunk(c, pcm, need);
        for (size_t i = 0; i < have; i++)
            mono[i] = ((float)pcm[i * 2] + (float)pcm[i * 2 + 1]) * mean_scale;
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
static void vocals_diagnostics(ta_context *c, const Model *model, const float *normalized,
                              const float *probabilities, int count)
{
    const ta_diagnostics *diagnostics = c->request->diagnostics;
    if (diagnostics && diagnostics->vocal_features)
        diagnostics->vocal_features(diagnostics->user, normalized, (uint32_t)count,
                                    (uint32_t)model->feature_columns);
    if (diagnostics && diagnostics->vocal_probabilities)
        diagnostics->vocal_probabilities(diagnostics->user, probabilities, (uint32_t)count);
    diagnostic_floats("TRACK_ANALYSIS_PVDI_FEATURES", normalized, (size_t)count * model->feature_columns);
    diagnostic_floats("TRACK_ANALYSIS_PVDI_PROBABILITIES", probabilities, (size_t)count);
}
static az_analysis_vocal_map_v1 vocals_result(ta_context *c, const Model *model,
                                            const float *probabilities, int count)
{
    az_vocal_frame_v1 *frames_out = ta_alloc(c, count, sizeof *frames_out);
    az_analysis_vocal_map_v1 result = { .sample_rate = (unsigned)model->frontend[TA_VOCAL_TARGET_SAMPLE_RATE],
                                        .hop_samples = (unsigned)model->frontend[TA_VOCAL_FEATURE_HOP],
                                        .method = AZ_VOCAL_LSTM,
                                        .count = (uint32_t)count,
                                        .frames = frames_out };
    for (int i = 0; i < count; i++) {
        float p = probabilities[i];
        if (!isfinite(p))
            ta_fail(c, TA_FAILED, "nonfinite PVDI probability");
        unsigned activity = 0;
        for (int j = 0; j < TA_VOCAL_THRESHOLD_COUNT; ++j)
            activity += p > model->activity[j];
        frames_out[i].activity = activity;
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
static unsigned vocal_minimum_samples(const Model *model)
{
    unsigned values = (unsigned)model->frontend[TA_VOCAL_MIN_MFCC_VALUES];
    unsigned columns = (unsigned)model->frontend[TA_VOCAL_MFCC_COLUMNS];
    unsigned rows = (values + columns - 1) / columns;
    return (rows - 1) * (unsigned)model->frontend[TA_VOCAL_FEATURE_HOP] +
           (unsigned)model->frontend[TA_VOCAL_FRAME_ROUNDING_BIAS];
}

az_analysis_vocal_map_v1 ta_vocals(ta_context *c)
{
    clock_gettime(CLOCK_MONOTONIC, &c->profile_previous);
    struct timespec started = c->profile_previous;
    struct rusage initial = { 0 };
    getrusage(RUSAGE_SELF, &initial);
    c->phrase_profile = false;
    Model *model = model_load(c);
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(c->request->audio_path, &rate, &frames);
    if (!c->decoder || !az_analysis_source_rate_supported(rate) ||
        frames > (uint64_t)rate * AZ_ANALYSIS_POLICY_MAXIMUM_SOURCE_SECONDS ||
        vocal_resampled_count(frames, rate, model->frontend) < vocal_minimum_samples(model))
        ta_fail(c, TA_FAILED, "PVDI source rate or duration");
    Frontend *frontend = frontend_init(c, rate, frames, model->frontend);
    vocals_decode(c, frontend, rate, frames);
    ta_profile_mark(c, "vocal_decode");
    struct timespec decoded = c->profile_previous;
    Features *features = features_init(c, model, frontend_frame_count(frontend));
    frontend_power(c, frontend, features);
    float *normalized = features_finish(c, features);
    int count = features->count;
    features_release(c, features);
    frontend_release(c, frontend);
    ta_profile_mark(c, "vocal_features");
    struct timespec extracted = c->profile_previous;
    float *probabilities = model_infer(c, model, normalized, count);
    ta_profile_mark(c, "vocal_inference");
    struct timespec inferred = c->profile_previous;
    vocals_diagnostics(c, model, normalized, probabilities, count);
    az_analysis_vocal_map_v1 result = vocals_result(c, model, probabilities, count);
    ta_release(c, probabilities);
    ta_release(c, normalized);
    for (int i = 0; i < TA_VOCAL_MATRIX_COUNT; i++) {
        ta_release(c, model->matrices[i].packed);
        ta_release(c, model->biases[i]);
    }
    ta_release(c, model->cheb);
    ta_release(c, model);
    vocals_profile(c, started, decoded, extracted, inferred, initial, frames, rate, count);
    return result;
}
#undef MIN
#undef MAX

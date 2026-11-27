/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen scalar frontend is the operation-order oracle for the planned NEON FFT. */
#include "../../src/analysis/vocals.c"
#include <assert.h>
enum { motion_fixture_width = 240 };
enum { N = 2048 };
static void reference_frame(const Frontend *f, int frame, double *output) {
    float real[N], imag[N];
    const int hop = 1024;
        for (int i = 0; i < N; i++) {
            long j = (long)frame * hop + i - N / 2;
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
static void test_motion_lag_model(Model *model)
{
    const double previous[motion_fixture_width] = { [10] = 1, [11] = 3 };
    const double current[motion_fixture_width] = { [10] = 1 };
    const double *parameters = model->frontend;
    assert(features_best_lag(previous, current, parameters) == 1);
    assert(vocal_minimum_samples(model) == 8193);

    double changed[TA_VOCAL_PARAMETER_COUNT];
    memcpy(changed, parameters, sizeof changed);
    changed[TA_VOCAL_MOTION_MAX_LAG] = 0;
    assert(features_best_lag(previous, current, changed) == 0);
    changed[TA_VOCAL_MOTION_MAX_LAG] = parameters[TA_VOCAL_MOTION_MAX_LAG];
    changed[TA_VOCAL_MOTION_ZERO_LAG] = 4;
    assert(features_best_lag(previous, current, changed) == 0);

    model->frontend[TA_VOCAL_MIN_MFCC_VALUES] += 1;
    assert(vocal_minimum_samples(model) == 9217);
    model->frontend[TA_VOCAL_MIN_MFCC_VALUES] -= 1;
}

static void test_motion_lag_selection(void)
{
    const double correlations[] = { NAN, .8, .1, 0, .3, 1, 0, .4, 0, .3, .5 };
    double maximum = correlations[0];
    int best = 0;
    for (size_t i = 1; i < sizeof correlations / sizeof *correlations; i++)
        motion_choose_lag(correlations[i], (int)i, &maximum, &best);
    assert(best == 5 && maximum == 1);
    motion_choose_lag(1, 1, &maximum, &best);
    assert(best == 5);
}

static void test_motion_energy_reduction(const Model *model)
{
    const double *parameters = model->frontend;
    double spectrum[motion_fixture_width] = { [0] = 0x1.1fd007feaae38p-4, [67] = 0x1.2de998a746f83p+0 };
    double epsilon = parameters[TA_VOCAL_MOTION_ENERGY_EPSILON];
    double energy = model->cheb[0] * spectrum[0] + model->cheb[67] * spectrum[67];
    double scalar = energy / (spectrum[0] + spectrum[67] + epsilon);
    assert(scalar > parameters[TA_VOCAL_MOTION_ENERGY_MIN]);
    double ratio = motion_energy_ratio(spectrum, model->cheb, motion_fixture_width, parameters);
    assert(ratio == energy / ((spectrum[0] + epsilon) + spectrum[67]));
    assert(ratio <= parameters[TA_VOCAL_MOTION_ENERGY_MIN]);
}

static void test_motion_flatness_reduction(const Model *model)
{
    double spectrum[motion_fixture_width] = { 0x1.f22983759f22ap-50 };
    double logs[motion_fixture_width] = { 0 };
    double parameters[TA_VOCAL_PARAMETER_COUNT];
    memcpy(parameters, model->frontend, sizeof parameters);
    double scale = parameters[TA_VOCAL_MOTION_MEAN_SCALE];
    double epsilon = parameters[TA_VOCAL_MOTION_MEAN_EPSILON];
    double cutoff = 1 / fma(spectrum[0], scale, epsilon);
    assert(1 / (spectrum[0] * scale + epsilon) < cutoff);
    parameters[TA_VOCAL_MOTION_FLATNESS_MAX] = cutoff;
    assert(!motion_flatness_ok(spectrum, logs, motion_fixture_width, parameters));
    parameters[TA_VOCAL_MOTION_FLATNESS_MAX] = nextafter(cutoff, INFINITY);
    assert(motion_flatness_ok(spectrum, logs, motion_fixture_width, parameters));
}

static void test_motion_operand_order(const Model *model)
{
    double parameters[TA_VOCAL_PARAMETER_COUNT];
    memcpy(parameters, model->frontend, sizeof parameters);
    const double lanes[] = {1e16, 0, -1e16, 0, 1, 0, 0, 0};
    assert(motion_reduce_lanes(lanes, parameters) == 1);
    parameters[TA_VOCAL_MOTION_REDUCE_PAIR_0] = 0;
    parameters[TA_VOCAL_MOTION_REDUCE_PAIR_1] = 2;
    parameters[TA_VOCAL_MOTION_REDUCE_PAIR_2] = 1;
    assert(motion_reduce_lanes(lanes, parameters) == 0);

    memcpy(parameters, model->frontend, sizeof parameters);
    double spectrum[] = {1, 0, 0x1p-53}, window[] = {1, 0, 0};
    parameters[TA_VOCAL_MOTION_ENERGY_EPSILON] = 0x1p-53;
    assert(motion_energy_ratio(spectrum, window, 3, parameters) == 1);
    parameters[TA_VOCAL_MOTION_ENERGY_EPSILON_LANE] = 2;
    assert(motion_energy_ratio(spectrum, window, 3, parameters) == 1 / (1 + 0x1p-52));
}

static void test_frontend_reflection(const Model *model)
{
    double parameters[TA_VOCAL_PARAMETER_COUNT];
    memcpy(parameters, model->frontend, sizeof parameters);
    parameters[TA_VOCAL_FRAME_CENTER] = 2;
    float samples[] = {5, 7, 11, 13}, window[] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint16_t position[] = {0, 1, 2, 3, 4, 5, 6, 7};
    Frontend f = { .parameters = parameters, .samples = samples, .window = window,
                   .permutation = position, .fft_size = 8, .count = 4 };
    float real[8];
    frontend_window_input(&f, 0, real);
    const float expected[] = {7, 5, 5, 7, 11, 13, 13, 11};
    assert(memcmp(real, expected, sizeof real) == 0);
    parameters[TA_VOCAL_FRAME_LEFT_BACK] = 0;
    parameters[TA_VOCAL_FRAME_RIGHT_BACK] = 0;
    parameters[TA_VOCAL_FRAME_RIGHT_SCALE] = 1;
    frontend_window_input(&f, 0, real);
    const float changed[] = {11, 7, 5, 7, 11, 13, 5, 0};
    assert(memcmp(real, changed, sizeof real) == 0);

    f.count = 5;
    parameters[TA_VOCAL_FEATURE_HOP] = 2;
    assert(frontend_frame_count(&f) == 3);
    parameters[TA_VOCAL_FRAME_ROUNDING_BIAS] = 2;
    assert(frontend_frame_count(&f) == 2);
}

static void test_model_motion_sizes(ta_context *c, const Model *model)
{
    Model smaller = *model;
    double *parameters = smaller.frontend;
    parameters[TA_VOCAL_MOTION_LOG_GRID_COUNT] = 96;
    parameters[TA_VOCAL_MOTION_BAND_WIDTH] = 16;
    parameters[TA_VOCAL_MOTION_BAND_COUNT] = 2;
    parameters[TA_VOCAL_MOTION_BAND_STRIDE_BYTES] = 8 * sizeof(double);
    parameters[TA_VOCAL_MOTION_MEAN_SCALE] = 1.0 / 16;
    parameters[TA_VOCAL_MOTION_ENERGY_MIN] = -1;
    parameters[TA_VOCAL_MOTION_FLATNESS_MAX] = 2;
    validate_motion_dimensions(c, &smaller);
    model_motion_geometry(c, parameters);
    Features *features = features_init(c, &smaller, 2);
    double power[N / 2 + 1];
    for (size_t i = 0; i < sizeof power / sizeof *power; ++i)
        power[i] = 1;
    features_add(features, power);
    features_add(features, power);
    assert(features->count == 2 && features->gate[0] && features->gate[1]);
    assert(features->previous[31] == features->bartlett[15] && features->previous[31] != 0);
    assert(features->log_magnitude[95] == 1);
    assert(features->motion[2] == 0 && features->motion[21] == 0);
    features_release(c, features);
}

static void test_model_motion_tail(ta_context *c, const Model *model)
{
    Model changed = *model;
    changed.frontend[TA_VOCAL_MOTION_FINAL_ROW_TRIM] = 2;
    changed.frontend[TA_VOCAL_MOTION_UNWRITTEN_VALUE] = -7;
    changed.frontend[TA_VOCAL_MOTION_ENERGY_MIN] = -1;
    changed.frontend[TA_VOCAL_MOTION_FLATNESS_MAX] = 2;
    Features *features = features_init(c, &changed, 3);
    int stride = (int)changed.frontend[TA_VOCAL_MOTION_ROW_STRIDE];
    double power[N / 2 + 1];
    for (size_t i = 0; i < sizeof power / sizeof *power; ++i)
        power[i] = 1;
    for (int i = 0; i < 3; ++i)
        features_add(features, power);
    assert(features->motion_rows == 1 && features->motion[0] == 0);
    assert(features->motion[stride] == -7 && features->motion[2 * stride] == -7);
    features_release(c, features);
}

static void test_mfcc_model_taps(const Model *model)
{
    const double cepstra[] = {1, 3, 6, 10, 15, 21, 28, 36, 45};
    double parameters[TA_VOCAL_PARAMETER_COUNT];
    memcpy(parameters, model->frontend, sizeof parameters);
    double scale = parameters[TA_VOCAL_DELTA_SCALE];
    DeltaKernel kernel = delta_kernel(parameters, 1);
    assert(mfcc_delta(&kernel, cepstra + 4) == 330 * scale);
    parameters[TA_VOCAL_DELTA_SECOND_ADDITIONS] = 1;
    parameters[TA_VOCAL_DELTA_FIRST_WEIGHT] = 2;
    kernel = delta_kernel(parameters, 1);
    assert(mfcc_delta(&kernel, cepstra + 4) == 319 * scale);
}

static void test_invalid_frontend_parameter(uint8_t *bytes, size_t index, double value, const char *message)
{
    ta_context *c = calloc(1, sizeof *c);
    assert(c);
    uint8_t *field = bytes + TA_MODEL_VOCAL_OFFSET + index * sizeof value;
    double saved;
    memcpy(&saved, field, sizeof saved);
    memcpy(field, &value, sizeof value);
    if (setjmp(c->failure) == 0) {
        Model model = { 0 };
        model_dimensions(c, &model, bytes);
        model_frontend(c, &model, bytes);
        assert(!"invalid frontend parameter was accepted");
    }
    assert(c->status == TA_FAILED && strcmp(c->error, message) == 0);
    memcpy(field, &saved, sizeof saved);
    free(c);
}

static void test_model_resampling(ta_context *c, const Model *model)
{
    float source[160];
    for (size_t i = 0; i < sizeof source / sizeof *source; i++) source[i] = 16000;
    Frontend *original = frontend_init(c, 48000, 160, model->frontend);
    assert(original->ratio == 48000 * (1.0 / 22050));
    frontend_add(c, original, source, 160);
    assert(original->count == 73 && original->samples[72] != 0);

    double changed[TA_VOCAL_PARAMETER_COUNT];
    memcpy(changed, model->frontend, sizeof changed);
    changed[TA_VOCAL_PCM_AMPLITUDE_SCALE] *= .5;
    Frontend *modified = frontend_init(c, 48000, 160, changed);
    frontend_add(c, modified, source, 160);
    assert(modified->count == original->count && modified->samples[72] == .5f * original->samples[72]);
}

static void test_model_dct_weights(ta_context *c, const Model *model)
{
    Features *original = features_init(c, model, 1);
    int bands = (int)model->frontend[TA_VOCAL_MEL_FILTER_COUNT];
    assert(original->dct[0] == 0.08838834764831845 && original->dct[bands] > 0);

    Model changed = *model;
    changed.frontend[TA_VOCAL_DCT_AC_SCALE] *= .5;
    Features *modified = features_init(c, &changed, 1);
    assert(modified->dct[0] == original->dct[0]);
    assert(modified->dct[bands] == .5 * original->dct[bands]);
    features_release(c, modified);
    features_release(c, original);
}

static void test_model_window_gain(ta_context *c, const Model *model)
{
    Frontend *original = frontend_init(c, 48000, 160, model->frontend);
    assert(original->fft_size == N);
    assert(original->sine[N / 8] == 0x1.6a09e6p-1f);

    double changed[TA_VOCAL_PARAMETER_COUNT];
    memcpy(changed, model->frontend, sizeof changed);
    changed[TA_VOCAL_WINDOW_GAIN] *= .5;
    Frontend *modified = frontend_init(c, 48000, 160, changed);
    assert(original->window[N / 2] > 0);
    assert(modified->window[N / 2] == .5f * original->window[N / 2]);
    frontend_release(c, modified);
    frontend_release(c, original);
}

static void test_reordered_graph(ta_context *c, const Model *source, uint8_t *bytes)
{
    uint8_t *outputs = bytes + TA_MODEL_RECURRENT_BRANCHES_OFFSET;
    uint8_t first[TA_MODEL_RECURRENT_BRANCH_BYTES];
    memcpy(first, outputs, sizeof first);
    memcpy(outputs, outputs + sizeof first, sizeof first);
    memcpy(outputs + sizeof first, first, sizeof first);

    Model changed = *source;
    memset(changed.branches, 0, sizeof changed.branches);
    model_topology(c, &changed, bytes);
    assert(changed.branches[0].reversed && changed.branches[0].cells[0] == 2);
    assert(!changed.branches[1].reversed && changed.branches[1].cells[0] == 0);
    assert(changed.branches[0].output_offset == 0 && changed.branches[1].output_offset == 256);
    int lengths[] = { 2 };
    float hidden[256] = { 3 }, output[2 * 512] = { 0 };
    changed.frontend[TA_VOCAL_INFERENCE_WINDOW] = 2;
    Scratch scratch = { .output = output, .hidden = {NULL, hidden} };
    infer_save(&scratch, &changed, lengths, 1, 0, &changed.branches[0]);
    infer_save(&scratch, &changed, lengths, 1, 0, &changed.branches[1]);
    assert(output[0] == 0 && output[256] == 3 && output[512] == 3 && output[768] == 0);

    memcpy(outputs + sizeof first, outputs, sizeof first);
    memcpy(outputs, first, sizeof first);
}

static void test_model_graph(uint8_t *bytes)
{
    ta_context *c = calloc(1, sizeof *c);
    assert(c && !setjmp(c->failure));
    Model model = { 0 };
    model_dimensions(c, &model, bytes);
    model_frontend(c, &model, bytes);
    model_graph(c, &model, bytes);
    model_recurrent(c, &model, bytes);
    assert(model.feature_columns == 29 && model.vocal_class == 1);
    assert(model.matrices[0].rows == 285 && model.matrices[0].columns == 1024);
    assert(model.matrices[TA_VOCAL_DENSE_HIDDEN].rows == 512);
    assert(model.matrices[TA_VOCAL_DENSE_HIDDEN].columns == 64);
    assert(model.matrices[TA_VOCAL_DENSE_OUTPUT].columns == 2);
    const RecurrentCell *cell = &model.recurrent[0];
    assert(cell->units == 256 && cell->gate_offsets[TA_VOCAL_GATE_FORGET] == 512);
    assert(cell->initial_cell == 0 && cell->initial_hidden == 0 && cell->forget_bias == 1);
    assert(model.branch_count == 2 && model.max_layers == 2);
    assert(model.branches[0].cells[0] == 0 && model.branches[0].cells[1] == 1 && !model.branches[0].reversed);
    assert(model.branches[1].cells[0] == 2 && model.branches[1].cells[1] == 3 && model.branches[1].reversed);
    test_reordered_graph(c, &model, bytes);
    ta_cleanup(c);
    free(c);
}

static void test_invalid_model_graph(uint8_t *bytes, size_t offset, uint8_t value, const char *message)
{
    ta_context *c = calloc(1, sizeof *c);
    assert(c);
    uint8_t saved = bytes[offset];
    bytes[offset] = value;
    if (setjmp(c->failure) == 0) {
        Model model = { 0 };
        model_dimensions(c, &model, bytes);
        model_frontend(c, &model, bytes);
        model_graph(c, &model, bytes);
        model_recurrent(c, &model, bytes);
        assert(!"invalid graph was accepted");
    }
    assert(c->status == TA_FAILED && strcmp(c->error, message) == 0);
    bytes[offset] = saved;
    ta_cleanup(c);
    free(c);
}

static void test_model_coefficient_shape(ta_context *c)
{
    const double coefficients[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t bytes[TA_MODEL_HEADER_BYTES + sizeof coefficients] = {0};
    memcpy(bytes + TA_MODEL_HEADER_BYTES, coefficients, sizeof coefficients);
    Model model = {.feature_columns = 3};
    model.frontend[TA_VOCAL_MOTION_BAND_WIDTH] = 2;
    assert(model_weight_offset(&model) == sizeof bytes);
    model_coefficients(c, &model, bytes);
    assert(model.cheb[1] == 2 && model.scale[0] == 3 && model.scale[2] == 5);
    assert(model.offset[0] == 6 && model.offset[2] == 8);
    ta_release(c, model.cheb);
}

static void test_inference_uses_graph_fields(void)
{
    float hidden[] = { 0, 0 }, previous[] = { 0, 0 };
    float gates[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    Scratch scratch = { .gates = gates, .hidden = {hidden, hidden + 1}, .cell = {previous, previous + 1} };
    const RecurrentCell cells[] = {
        { .units = 1, .gate_offsets = {3, 0, 2, 1}, .initial_cell = .25f, .initial_hidden = .5f },
        { .units = 1, .initial_cell = .75f, .initial_hidden = .125f }
    };
    const RecurrentBranch branch = { .cells = {0, 1}, .length = 2 };
    infer_initial_state(&scratch, cells, &branch, 1);
    assert(previous[0] == .25f && previous[1] == .75f && hidden[0] == .5f && hidden[1] == .125f);
    infer_cells(&scratch, cells, 4, 1, 0);
    float expected = .25f * sigmoid(3) + sigmoid(4) * tanhf(1);
    assert(previous[0] == expected && hidden[0] == tanhf(expected) * sigmoid(2));

    float logits[] = { 1, 2, 0 };
    float other[] = { 1, 2, 0 };
    float denominator = expf(-1) + 1 + expf(-2);
    assert(infer_softmax(logits, 3, 0) == expf(-1) / denominator);
    assert(infer_softmax(other, 3, 1) == 1 / denominator);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const ta_request request = { .assets_path = argv[1] };
    ta_context *c = calloc(1, sizeof *c);
    assert(c && !setjmp(c->failure));
    c->request = &request;
    uint8_t *bytes = ta_model_open(c);
    Model model = {0};
    model_dimensions(c, &model, bytes);
    model_frontend(c, &model, bytes);
    assert(model.frontend[TA_VOCAL_MOTION_BAND_WIDTH] == motion_fixture_width);
    model_coefficients(c, &model, bytes);
    test_model_coefficient_shape(c);
    test_model_graph(bytes);
    test_invalid_model_graph(bytes, TA_MODEL_FEATURE_COLUMNS_OFFSET, 0, "invalid vocal input width or output class");
    test_invalid_model_graph(bytes, TA_MODEL_FEATURE_COLUMNS_OFFSET, 28, "invalid vocal MFCC shape");
    test_invalid_model_graph(bytes, TA_MODEL_MATRIX_SHAPES_OFFSET + 3, 0xff, "invalid vocal tensor extent");
    test_invalid_model_graph(bytes, TA_MODEL_OUTPUT_CLASS_OFFSET, 2, "inconsistent vocal dense widths or output class");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_CELLS_OFFSET + TA_VOCAL_GATE_COUNT * 4, 0,
                             "invalid vocal recurrent gate count");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_CELLS_OFFSET + TA_VOCAL_GATE_INPUT * 4, 2,
                             "invalid vocal recurrent gate port");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_INPUTS_OFFSET, 1, "invalid vocal recurrent connection");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_BRANCH_COUNT_OFFSET, 0, "invalid vocal recurrent output count");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_BRANCH_COUNT_OFFSET, 1, "unreachable vocal recurrent cells");
    test_invalid_model_graph(bytes, TA_MODEL_RECURRENT_BRANCHES_OFFSET + 4, 2, "invalid vocal recurrent output");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_LOG_GRID_COUNT, 539, "invalid vocal motion geometry");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_ROW_STRIDE, 10, "invalid vocal motion dimensions");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_REDUCTION_WIDTH, 7, "invalid vocal motion reduction width");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_REDUCE_PAIR_1, 1, "invalid vocal motion reduction order");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_ENERGY_EPSILON_LANE, 8,
                                    "invalid vocal motion epsilon placement");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_FRAME_CENTER, 2049, "invalid vocal frame geometry");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_FRAME_ROUNDING_BIAS, 0, "invalid vocal frame geometry");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_MOTION_FINAL_ROW_TRIM, 0, "invalid vocal motion dimensions");
    test_invalid_frontend_parameter(bytes, TA_VOCAL_DELTA_THIRD_RADIUS, 2, "invalid vocal MFCC taps");
    test_inference_uses_graph_fields();
    ta_release(c, bytes);
    test_motion_lag_model(&model);
    test_motion_lag_selection();
    test_motion_energy_reduction(&model);
    test_motion_flatness_reduction(&model);
    test_motion_operand_order(&model);
    test_frontend_reflection(&model);
    test_model_motion_sizes(c, &model);
    test_model_motion_tail(c, &model);
    test_mfcc_model_taps(&model);
    test_model_resampling(c, &model);
    test_model_dct_weights(c, &model);
    test_model_window_gain(c, &model);
    unsigned cases = 0;
    const int counts[] = {1, 1023, 1024, 2047, 2048, 4097, 22050};
    for (int rate = 44100; rate <= 48000; rate += 3900) {
        Frontend *f = frontend_init(c, rate, (uint64_t)rate, model.frontend);
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
                assert(frontend_frame_count(f) == (f->count + 1023) / 1024);
                for (int frame = 0; frame < frontend_frame_count(f); frame++) {
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

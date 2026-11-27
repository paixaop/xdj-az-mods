/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/phrase_spectra.c"
#include "phrase-model-fixture.h"

static void test_model_peak_width(ta_context *c, const double *model)
{
    double samples[] = { 0, 4, 0 };
    Peaks peak = (spectral_peaks)(c, (D){ samples, 3 }, model);
    assert(peak.n == 1 && peak.v[0].position == 2 && peak.v[0].width == 1);
    ta_release(c, peak.v);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SPECTRAL_PEAK_WIDTH_PROMINENCE_SCALE] = -0.25;
    peak = (spectral_peaks)(c, (D){ samples, 3 }, changed);
    assert(peak.n == 1 && peak.v[0].width == 0.5);
    ta_release(c, peak.v);

    changed[TA_SPECTRAL_PEAK_FALLBACK_COUNT] = 2;
    peak = (spectral_peaks)(c, (D){ samples, 1 }, changed);
    assert(peak.n == 2 && peak.v[0].height == 0 && peak.v[1].height == 0);
    ta_release(c, peak.v);
}

static void test_spectral_vocal_row_scales(const double *model)
{
    double bins[] = { 70, 3, 17, 70 };
    D values = { bins, sizeof bins / sizeof *bins };
    double changed[TA_PHRASE_PARAMETER_COUNT];
    double reference = spectral_vocal_row_power(values, 1, 3, model);
    for (int i = TA_PHRASE_VOCAL_BIN_EXPONENT_SCALE; i <= TA_PHRASE_VOCAL_ROW_EXPONENT_SCALE; i++) {
        memcpy(changed, model, sizeof changed);
        changed[i] *= 2;
        assert(spectral_vocal_row_power(values, 1, 3, changed) != reference);
    }
}

static void test_opening_frame_rounding(ta_context *c, const double *model)
{
    double values[25] = { [12] = 1 };
    D counts = { values, sizeof values / sizeof *values };
    D levels = opening_levels(c, counts, 0, 24, counts.n, model);
    assert(levels.n == 6 && levels.v[2] == 1 && levels.v[3] == 0);
    OpeningFeatures flags = opening_presence(c, levels, levels.n, model);
    int width = (int)model[TA_OPENING_PRESENCE_GROUP_BEATS];
    assert(flags.presence.v[2] == model[TA_OPENING_PRESENCE_MARK]);
    assert(flags.envelope.v[2 * width] == 1 && flags.envelope.v[3 * width] == 0);

    values[12] = 0;
    levels = opening_levels(c, counts, 0, 24, counts.n, model);
    assert(levels.v[2] == 0);
}

static void test_windows_opening_frame_rounding(ta_context *c, const double *model)
{
    double values[61] = { [30] = 1 };
    D counts = { values, sizeof values / sizeof *values };
    D arm = opening_levels(c, counts, 0, 56, counts.n, model);
    c->windows_model = true;
    D windows = opening_levels(c, counts, 0, 56, counts.n, model);
    c->windows_model = false;
    assert(arm.n == 14 && arm.v[6] == 1 && arm.v[7] == 0);
    assert(windows.n == 14 && windows.v[6] == 0 && windows.v[7] == 1);
    ta_release(c, arm.v);
    ta_release(c, windows.v);
}

static void test_window_and_db_parameters(ta_context *c, const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double real[] = {1, 1, 1}, imag[] = {1, 1, 1};
    cqt_window((D){real, 3}, (D){imag, 3}, parameters);
    assert(real[0] == real[2] && real[1] == 1 && imag[1] == 1);
    parameters[TA_OPENING_CQT_HANN_GAIN] = 0.25;
    double smaller[] = {1, 1, 1};
    cqt_window((D){smaller, 3}, (D){imag, 3}, parameters);
    assert(smaller[0] == real[0] * 0.5 && smaller[1] == real[1] && smaller[2] == real[2] * 0.5);

    double samples[] = {2, 0, 0, 0}, window[] = {1, 1, 1, 1}, rr[4], ii[4];
    fft_job job = { .size = 4, .output_bins = 2, .window = {window, 4},
                    .db_scale = model[TA_VOCAL_FFT_DB_SCALE] };
    D original = fft_row(c, &job, (D){samples, 4}, (D){rr, 4}, (D){ii, 4}, 0, 4);
    job.db_scale *= 0.5;
    D modified = fft_row(c, &job, (D){samples, 4}, (D){rr, 4}, (D){ii, 4}, 0, 4);
    assert(original.n == 2 && original.v[0] == 20 * log10(2) && modified.v[0] == original.v[0] * 0.5);
    ta_release(c, original.v);
    ta_release(c, modified.v);
}

static void test_spectral_peak_geometry(ta_context *c, const double *model)
{
    double values[] = {0, 4, 4, 0};
    D input = {values, sizeof values / sizeof *values};
    Peaks baseline = (spectral_peaks)(c, input, model);
    assert(baseline.n == 1 && baseline.v[0].position == 2 && baseline.v[0].width == 2);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_SPECTRAL_PEAK_POSITION_ORIGIN] = 2;
    changed[TA_SPECTRAL_PEAK_SCAN_SLOPE_THRESHOLD] = 0.25;
    Peaks later = (spectral_peaks)(c, input, changed);
    assert(later.n == 1 && later.v[0].position == 4 && later.v[0].width == baseline.v[0].width);

    changed[TA_SPECTRAL_PEAK_WIDTH_LEFT_BIAS] = 2;
    changed[TA_SPECTRAL_PEAK_WIDTH_LEFT_PAIR_BACK] = -2;
    Peaks narrower = (spectral_peaks)(c, input, changed);
    assert(narrower.v[0].width == baseline.v[0].width - 1);
    ta_release(c, narrower.v);
    ta_release(c, later.v);
    ta_release(c, baseline.v);
}

static void test_fft_window_offsets(const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double baseline[4], changed[4];
    phrase_fft_window((D){baseline, 4}, model);
    assert(baseline[0] == baseline[3] && baseline[1] == baseline[2]);

    parameters[TA_PHRASE_FFT_WINDOW_TERMINAL_BACK] = 0;
    phrase_fft_window((D){changed, 4}, parameters);
    assert(changed[2] == 1 && changed[3] < changed[2]);

    parameters[TA_PHRASE_FFT_WINDOW_TERMINAL_BACK] = model[TA_PHRASE_FFT_WINDOW_TERMINAL_BACK];
    parameters[TA_PHRASE_FFT_WINDOW_DENOMINATOR_BIAS] *= 2;
    phrase_fft_window((D){changed, 4}, parameters);
    assert(changed[0] > baseline[0] && changed[0] == changed[3]);
}

static void test_cqt_window_split(const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double even_real[] = {1, 1, 1, 1}, even_imag[] = {1, 1, 1, 1};
    parameters[TA_OPENING_CQT_HANN_EVEN_SPLIT] = 3;
    cqt_window((D){even_real, 4}, (D){even_imag, 4}, parameters);
    assert(even_real[1] == 1 && even_real[2] == 1 && even_real[0] == even_real[3]);

    double odd_real[] = {1, 1, 1, 1, 1}, odd_imag[] = {1, 1, 1, 1, 1};
    parameters[TA_OPENING_CQT_HANN_ODD_SPLIT] = 3;
    parameters[TA_OPENING_CQT_HANN_ODD_CENTER_GAIN] = 0.25;
    cqt_window((D){odd_real, 5}, (D){odd_imag, 5}, parameters);
    assert(odd_real[1] > 0 && odd_real[1] < 0.5 && odd_real[2] == 1);
    assert(odd_real[3] == 1 && odd_real[0] == odd_real[4]);
}

static void test_cqt_mirror(const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double real[] = {1, 1, 1, 1}, imag[] = {1, 1, 1, 1};
    parameters[TA_OPENING_CQT_HANN_MIRROR_BACK] = 2;
    cqt_window((D){real, 4}, (D){imag, 4}, parameters);
    assert(real[0] == real[2] && real[0] < 1 && real[3] == 1);
    assert(!memcmp(real, imag, sizeof real));
}

static void test_cqt_center(ta_context *c, const double *model)
{
    double values[] = {1, 2, 3, 4}, one = 1, zero = 0;
    D samples = {values, 4}, real = {&one, 1}, imag = {&zero, 1};
    cqt_geometry geometry = opening_cqt_geometry(c, model);
    assert(cqt_frame(c, samples, real, imag, 4, 0, &geometry) == 12);
    geometry.center_fraction = 0.25;
    assert(cqt_frame(c, samples, real, imag, 4, 0, &geometry) == 8);
}

static void test_cqt_accumulation(ta_context *c, const double *model)
{
    double values[] = {1, 1, 1, 1, 1, 1, 1, 1};
    double filter[] = {0x1p60, 0, -0x1p60, 0, 1, 0, 0, 0}, zeros[8] = {0};
    D samples = {values, 8}, nonzero = {filter, 8}, zero = {zeros, 8};
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    cqt_geometry geometry = opening_cqt_geometry(c, parameters);
    assert(cqt_frame(c, samples, nonzero, zero, 8, 0, &geometry) == 1);
    assert(cqt_frame(c, samples, zero, nonzero, 8, 0, &geometry) == 1);

    parameters[TA_OPENING_CQT_REAL_PAIR_0] = 2;
    parameters[TA_OPENING_CQT_REAL_PAIR_2] = 1;
    geometry = opening_cqt_geometry(c, parameters);
    assert(cqt_frame(c, samples, nonzero, zero, 8, 0, &geometry) == 0);
    assert(cqt_frame(c, samples, zero, nonzero, 8, 0, &geometry) == 1);

    parameters[TA_OPENING_CQT_REAL_PAIR_0] = model[TA_OPENING_CQT_REAL_PAIR_0];
    parameters[TA_OPENING_CQT_REAL_PAIR_2] = model[TA_OPENING_CQT_REAL_PAIR_2];
    parameters[TA_OPENING_CQT_FMA_GROUP] = 4;
    geometry = opening_cqt_geometry(c, parameters);
    assert(cqt_frame(c, samples, nonzero, zero, 8, 0, &geometry) == 0);
}

static void test_windows_cqt_accumulation(ta_context *c, const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    parameters[TA_OPENING_CQT_FMA_GROUP] = 4;
    const double steps[] = { 0, 0, 1, 1 };
    memcpy(parameters + TA_OPENING_CQT_REAL_PAIR_0, steps, sizeof steps);
    memcpy(parameters + TA_OPENING_CQT_IMAG_PAIR_0, steps, sizeof steps);
    c->windows_model = true;
    cqt_geometry geometry = opening_cqt_geometry(c, parameters);
    c->windows_model = false;

    double values[] = { 1, 1, 1 + 0x1p-27, 1, 1 + 0x1p-27 };
    double filter[] = { -1, 0, 1 - 0x1p-27, 0, 1 - 0x1p-27 }, zeros[5] = {0};
    D samples = { values, 5 }, nonzero = { filter, 3 }, zero = { zeros, 3 };
    assert(cqt_frame(c, samples, nonzero, zero, 3, 0, &geometry) == 0);
    assert(cqt_frame(c, samples, zero, nonzero, 3, 0, &geometry) == 0);
    nonzero.n = zero.n = 4;
    assert(cqt_frame(c, samples, nonzero, zero, 4, 0, &geometry) == 0x1p-54);
    assert(cqt_frame(c, samples, zero, nonzero, 4, 0, &geometry) == 0x1p-54);
    nonzero.n = zero.n = 5;
    assert(cqt_frame(c, samples, nonzero, zero, 5, 0, &geometry) == 1);
}

static void test_fft_bar_geometry(ta_context *c, const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    double values[16] = { [0] = 2, [5] = 1 };
    int coordinates[] = {1, 3, 5, 7, 9, 11, 13, 15};
    Samples samples = {.middle = {values, 16}, .rate = 8};
    Coordinates beats = {.middle = {coordinates, 8}, .n = 8};
    Timing timing = {.phase = 1, .bpm = 120};
    Spectra baseline = phrase_spectra(c, samples, timing, beats, false, model);
    assert(baseline.n == 8 && baseline.rows[0].n == model[TA_VOCAL_FFT_OUTPUT_BINS]);

    parameters[TA_VOCAL_FFT_ROWS_PER_BAR] = 1;
    parameters[TA_VOCAL_FFT_BEATS_PER_BAR] = 2;
    parameters[TA_VOCAL_FFT_SAMPLE_ORIGIN] = 0;
    parameters[TA_VOCAL_FFT_OUTPUT_BINS] = 1;
    Spectra narrower = phrase_spectra(c, samples, timing, beats, false, parameters);
    assert(narrower.n == 4 && narrower.rows[0].n == 1);
    assert(narrower.rows[0].v[0] == model[TA_VOCAL_FFT_DB_FLOOR]);
    assert(baseline.rows[0].v[0] > narrower.rows[0].v[0]);
    spectra_release(c, narrower);
    spectra_release(c, baseline);
}

static void test_nominal_beat_parameters(const double *model)
{
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    memcpy(parameters, model, sizeof parameters);
    assert(nominal_beats(120, 2, 500, parameters) == 69);
    parameters[TA_NOMINAL_BEAT_OFFSET_SCALE] = 0.003;
    assert(nominal_beats(120, 2, 500, parameters) == 65);
}

static void test_opening_first_frame(ta_context *c, const double *model)
{
    Timing timing = {.bpm = 120, .phase = 2};
    double rate = 5512.5;
    assert(opening_first_frame(c, rate, timing, model) == 11);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_OPENING_START_OFFSET_MS] = 500;
    assert(opening_first_frame(c, rate, timing, changed) == 22);
    changed[TA_OPENING_START_PHASE_ORIGIN] = 2;
    assert(opening_first_frame(c, rate, timing, changed) == 11);
}

static void test_spectral_vocal_filter_sections(ta_context *c, const double *model)
{
    double values[] = {1, 0, 0, 0};
    D input = {values, sizeof values / sizeof *values};
    D original = spectral_vocal_filter(c, input, model);
    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_PHRASE_VOCAL_FILTER_SECTIONS] = 2;
    D filtered = spectral_vocal_filter(c, input, changed);
    assert(original.n == filtered.n && filtered.v[0] == original.v[0] * original.v[0]);
    assert(original.v[0] != filtered.v[0]);
    ta_release(c, filtered.v);
    ta_release(c, original.v);
}

void test_phrase_edges_spectra(ta_context *c)
{
    const double *model = fixture_phrase_parameters();
    test_model_peak_width(c, model);
    test_spectral_vocal_row_scales(model);
    test_opening_frame_rounding(c, model);
    test_windows_opening_frame_rounding(c, model);
    test_window_and_db_parameters(c, model);
    test_spectral_peak_geometry(c, model);
    test_fft_window_offsets(model);
    test_cqt_window_split(model);
    test_nominal_beat_parameters(model);
    test_opening_first_frame(c, model);
    test_spectral_vocal_filter_sections(c, model);
    test_cqt_mirror(model);
    test_cqt_center(c, model);
    test_cqt_accumulation(c, model);
    test_windows_cqt_accumulation(c, model);
    test_fft_bar_geometry(c, model);
    int onset = (int)model[TA_VOCAL_ENTRY_LONG_SILENCE_COUNT] + 2;
    uint8_t activity[128] = { 0 }, entries[128] = { 0 };
    assert(onset + 3 < (int)sizeof activity);
    B mask = { activity, sizeof activity }, selected = { entries, sizeof entries };
    activity[onset] = 1;
    activity[onset + 2] = 1;
    long_silence_entries(mask, selected, mask.n, model);
    assert(entries[onset + 1] == 1);

    double changed[TA_PHRASE_PARAMETER_COUNT];
    memcpy(changed, model, sizeof changed);
    changed[TA_VOCAL_ENTRY_LONG_RESUME_LAST_DELTA] = 1;
    memset(entries, 0, sizeof entries);
    long_silence_entries(mask, selected, mask.n, changed);
    assert(entries[onset + 1] == 0);
    changed[TA_VOCAL_ENTRY_LONG_RESUME_LAST_DELTA] = 2;
    changed[TA_VOCAL_ENTRY_LONG_MARKER_OFFSET] = 2;
    long_silence_entries(mask, selected, mask.n, changed);
    assert(entries[onset + 1] == 0 && entries[onset + 2] == 1);
}

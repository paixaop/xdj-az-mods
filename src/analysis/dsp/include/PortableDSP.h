/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TRACK_ANALYSIS_DSP_H
#define TRACK_ANALYSIS_DSP_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
void ta_fft_f(float *real, float *imaginary, size_t count);
void ta_fft_d(double *real, double *imaginary, size_t count);
/* Internal causal double filters. process supports exact in-place use;
 * other overlaps are unsupported. decimate requires divisor > 0, disjoint
 * input/output, and count/divisor output slots; it consumes even the tail. */
typedef struct ta_phrase_biquad {
    double b0, b1, b2, a1, a2, z1, z2;
    int first_order;
} ta_phrase_biquad;
void ta_phrase_biquad_process(ta_phrase_biquad *section, const double *input,
                             double *output, size_t count, int rectify);
void ta_phrase_decimate(ta_phrase_biquad *sections, size_t section_count,
                        const double *input, double *output, size_t count,
                        size_t divisor);
typedef struct ta_decoder ta_decoder;
enum { TA_DECODE_DEFAULT = 0, TA_DECODE_AAC_ENCODED = 1 };
typedef struct ta_decode_clock {
    uint32_t kind, has_edit;
    uint64_t decoder_leading_frames, edit_start_frames, presentation_frames;
} ta_decode_clock;
ta_decoder *ta_decode_open(const char *path, unsigned *rate, uint64_t *frames);
void ta_decode_get_clock(ta_decoder *decoder, ta_decode_clock *clock);
int64_t ta_decode_read(ta_decoder *decoder, int16_t *stereo, uint64_t frames);
int ta_decode_seek(ta_decoder *decoder, uint64_t frame);
void ta_decode_close(ta_decoder *decoder);
#endif

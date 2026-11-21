/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_VOCAL_LINES_H
#define AZ_VOCAL_LINES_H
#include <stdint.h>
#define VOCAL_LINES_WIDTH 1280
/* Lifecycle calls are serialized. Stop joins IO and must run off UI/native
 * threads. Start preserves load notifications/configuration received earlier. */
int vocal_lines_start(void);
void vocal_lines_stop(void);
/* Ordered native load notifications; old and duplicate orders are ignored. */
void vocal_lines_load(unsigned deck, uint64_t order, uint64_t epoch, int loaded);
void vocal_lines_config(unsigned gap_ms, int enabled);
/* Nonblocking paint access. The caller provides original-track viewport bounds,
 * including negative starts while scrolling before zero. Coverage never changes
 * the viewport scale. Returns zero and clears output when data is unavailable. */
int vocal_lines_pixels(unsigned deck, uint64_t epoch, double start_s, double stop_s,
                       int width, uint8_t out[VOCAL_LINES_WIDTH]);
uint32_t vocal_lines_sample_rate(unsigned deck, uint64_t epoch);
uint64_t vocal_lines_revision(unsigned deck);
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_VOCAL_LINES_PROJECTION_H
#define AZ_VOCAL_LINES_PROJECTION_H
#include "vocal_lines.h"
#include "azmod-mod.h"
/* Input spans are validated, sorted and consolidated by vocal_lines_merge. */
int vocal_lines_project(const az_vocal_interval *intervals, uint32_t count,
                        uint32_t rate, double start, double stop, int width,
                        uint8_t out[VOCAL_LINES_WIDTH]);
#endif

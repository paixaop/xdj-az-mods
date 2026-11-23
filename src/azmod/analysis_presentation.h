/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_PRESENTATION_H
#define AZ_ANALYSIS_PRESENTATION_H
#include "azmod-mod.h"
#include <stdio.h>

/* Presentation consumes a copied observation and never advances analysis. */
static inline const char *analysis_result_message(int result) {
    switch (result) {
    case AZ_OK: return "Track analysis ready";
    case AZ_NOT_FOUND: return "Phrases and vocals ready";
    case AZ_UNSUPPORTED: return "Unsupported audio or media source";
    case AZ_STALE: return "Source changed or analysis cancelled";
    default: return "Analysis failed; see core log";
    }
}
#endif

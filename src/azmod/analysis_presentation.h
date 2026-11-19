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
static inline const char *analysis_status_message(const az_analysis_status_v1 *status, int rc) {
    if (rc == AZ_BUSY) return "Updating analysis status";
    if (rc != AZ_OK) return "Analysis service unavailable";
    switch (status->state) {
    case 0: return "Waiting for a USB or Beatport track";
    case AZ_ANALYSIS_QUEUED: return "Checking phrases and vocals";
    case AZ_ANALYSIS_RUNNING: return "Analysing phrases and vocals";
    case AZ_ANALYSIS_SAVED: return "Analysis saved; waiting for library linking";
    case AZ_ANALYSIS_CANCELLED: return "Source changed or analysis cancelled";
    default: return analysis_result_message(status->result);
    }
}
#endif

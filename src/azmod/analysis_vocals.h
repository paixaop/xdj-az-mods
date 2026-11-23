/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_VOCALS_H
#define AZ_ANALYSIS_VOCALS_H
#include "vd_anlz.h"
/* Private worker read for the upstream indicator. Independent of the analysis
 * enable switch; qualifies USB/Beatport paths against the current native load.
 * AZ_OK transfers the allocated levels to the caller. Failure leaves outputs
 * untouched. AZ_NOT_FOUND means absent/empty PVDI, not an instrumental map. */
int analysis_vocal_track_loaded(unsigned deck, uint64_t load, struct vd_track *out,
                                unsigned *length_s);
__attribute__((visibility("hidden"))) void vocal_indicator_events_init(void);
#endif

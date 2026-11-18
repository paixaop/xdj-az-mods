/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUERECALLBRIDGE_H
#define AZCUERECALLBRIDGE_H
#include "azexit.h"
/* Copied on the DSP owner frame, published with its exact terminal token.
 * No native object or counted source reference crosses this handoff. */
struct azexit_cue_stage_sample {
    uint64_t in_sample, out_sample;
    uint32_t sample_rate, out_valid;
    unsigned char source_id[12];
};
#define AZ_RECALL_NOEXCEPT
int azsdk_exit_recall_submit(const struct azexit_cue_request *) AZ_RECALL_NOEXCEPT;
int azsdk_exit_cue_stage_submit(const struct azexit_cue_request *) AZ_RECALL_NOEXCEPT;
int azsdk_exit_cue_stage_capture(unsigned, uint64_t, struct azexit_cue_stage_sample *) AZ_RECALL_NOEXCEPT;
int azsdk_exit_recall_cancel(unsigned, uint64_t) AZ_RECALL_NOEXCEPT;
int azsdk_exit_recall_quiescent(unsigned, uint64_t) AZ_RECALL_NOEXCEPT;
int azsdk_exit_recall_take(unsigned, struct azexit_result *) AZ_RECALL_NOEXCEPT;
int azsdk_exit_recall_poll(unsigned, struct azexit_result *) AZ_RECALL_NOEXCEPT;
int azsdk_exit_recall_release(unsigned, uint64_t) AZ_RECALL_NOEXCEPT;
#undef AZ_RECALL_NOEXCEPT
#endif

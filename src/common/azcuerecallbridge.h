/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUERECALLBRIDGE_H
#define AZCUERECALLBRIDGE_H
#include "azexit.h"
#ifdef __cplusplus
/* Copied on the DSP owner frame, published with its exact terminal token.
 * No native object or counted source reference crosses this handoff. */
struct azexit_cue_stage_sample {
    uint64_t in_sample, out_sample;
    uint32_t sample_rate, out_valid;
    unsigned char source_id[12];
};
extern "C" {
int azsdk_exit_recall_submit(const azexit_cue_request *) noexcept;
int azsdk_exit_cue_stage_submit(const azexit_cue_request *) noexcept;
int azsdk_exit_cue_stage_capture(unsigned, uint64_t, azexit_cue_stage_sample *) noexcept;
int azsdk_exit_recall_cancel(unsigned, uint64_t) noexcept;
int azsdk_exit_recall_quiescent(unsigned, uint64_t) noexcept;
int azsdk_exit_recall_poll(unsigned, azexit_result *) noexcept;
int azsdk_exit_recall_release(unsigned, uint64_t) noexcept;
}
#endif
#endif

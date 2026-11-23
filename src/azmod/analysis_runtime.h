/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_RUNTIME_H
#define AZ_ANALYSIS_RUNTIME_H
#include "analysis_source.h"
/* One core-owned coordinator and a bounded global queue. Source acquisition,
 * publication and cleanup run on the coordinator, never the UI/native thread. */
int analysis_runtime_start(const char *assets);
void analysis_runtime_shutdown(void); /* off UI; joins coordinator and children */
int analysis_runtime_request(az_analysis_owner owner, const az_analysis_request_v1 *request,
                             uint64_t *request_id);
int analysis_runtime_status(az_analysis_owner owner, uint64_t request_id, az_analysis_status_v1 *out);
int analysis_runtime_cancel(az_analysis_owner owner, uint64_t request_id);
int analysis_runtime_release(az_analysis_owner owner, uint64_t request_id);
int analysis_runtime_result(az_analysis_owner owner, uint64_t request_id, az_analysis_result_v1 *out,
                            az_analysis_phrase_v1 *phrases, uint32_t phrase_capacity,
                            az_vocal_frame_v1 *vocals, uint32_t vocal_capacity);
void analysis_runtime_retire(az_analysis_owner owner);
void analysis_runtime_wake(void);
int analysis_runtime_capabilities(az_analysis_capabilities_v1 *out);
/* Notifications are core load reducer observations, not public subscriptions.
 * Core enablement affects automatic requests only. Explicit SDK requests remain
 * admitted until their own owner retires or the service shuts down. */
void analysis_runtime_enable(az_analysis_owner owner, int enabled);
void analysis_runtime_components(uint32_t components);
void analysis_runtime_load(unsigned deck, uint64_t order, uint64_t epoch, int loaded, const az_track_ref *track);
int analysis_runtime_deck_status(unsigned deck, az_analysis_status_v1 *out);
#endif

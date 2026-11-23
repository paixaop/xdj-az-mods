/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_COMMAND_SERVICE_H
#define SDK_COMMAND_SERVICE_H
#include "sdk_command_engine.h"
#include "../common/azexit.h"
/* The host validates callers and callback code addresses before admission.
 * Only this service owns retained requests, native leases and delivery tokens. */
uint64_t sdk_command_capabilities(void);
void sdk_command_observe_native(const struct azexit_service *);
int sdk_command_native_bound(void);
int sdk_command_start(const struct azexit_service *);
int sdk_command_ui_fd(void);
void sdk_command_owner_init(int);
void sdk_command_owner_ready(int);
void sdk_command_retire_owner(int);
int sdk_exit_cancel_request(int, uint64_t, az_command_result *);
int sdk_exit_submit(int, const az_command_request *, az_command_result *);
int sdk_exit_submit_async(int, const az_command_request *, az_command_completion_fn, void *, az_command_result *);
int sdk_exit_submit_batch(int, const az_cue_batch_request *, az_command_completion_fn, void *, az_command_result *);
void sdk_command_cue_presentation_enable(void);
uint64_t sdk_command_cue_presentation_hold(unsigned deck);
int sdk_command_cue_presentation_current(unsigned deck, uint64_t load);
void sdk_cue_tick(void);
int sdk_native_cues(int, az_native_cue_snapshot *);
unsigned sdk_command_ui_drain(unsigned);
int sdk_command_read(int, uint64_t, az_command_result *);
int sdk_command_drop(int, uint64_t);
int sdk_command_ack(int, uint64_t);
/* UI invokes readiness under the command mutex, then delivery after unlock.
 * These scoped callbacks cannot inspect or retain command store internals. */
int sdk_command_record_callback(int, int (*)(int), void (*)(int, const az_command_result *));
#endif

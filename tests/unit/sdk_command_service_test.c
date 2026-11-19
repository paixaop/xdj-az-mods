/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_command_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned ticks, reads, delivered;
static void tick(void) { ticks++; }
static int snapshot(unsigned deck, az_native_cue_snapshot *out) {
    reads++;
    out->deck = deck;
    out->revision = 19;
    return AZ_OK;
}
static int ready(int owner) {
    assert(owner == 1);
    return 1;
}
static void receive(int owner, const az_command_result *result) {
    (void) owner;
    (void) result;
    delivered++;
}
int main(void) {
    assert(sdk_command_start(NULL) == AZ_UNSUPPORTED);
    sdk_command_owner_init(1);
    sdk_command_owner_ready(1);
    struct azexit_service native = {.size = sizeof native, .version = 1, .cue_tick = tick, .native_cues = snapshot};
    sdk_command_observe_native(&native);
    assert(!sdk_command_native_bound() && !sdk_command_capabilities());
    sdk_cue_tick();
    az_native_cue_snapshot cues = {.size = sizeof cues};
    assert(sdk_native_cues(2, &cues) == AZ_OK && cues.deck == 2 && cues.revision == 19 && reads == 1 && ticks == 1);
    az_command_request request = {.size = sizeof request, .version = AZ_DATA_VERSION, .operation = AZ_CMD_LOOP_EXIT, .request_id = 1, .flags = AZ_COMMAND_NATIVE_LOAD, .expected_load_generation = 1, .deadline_ns = UINT64_MAX};
    az_command_result result = {.size = sizeof result};
    assert(sdk_exit_submit(1, &request, &result) == AZ_UNSUPPORTED);
    assert(sdk_command_read(1, 1, &result) == AZ_STALE);
    assert(sdk_exit_cancel_request(1, 1, &result) == AZ_STALE);
    assert(sdk_command_drop(1, 1) == AZ_STALE && sdk_command_ack(1, 1) == AZ_STALE);
    assert(sdk_command_record_callback(0, ready, receive) == 1 && !delivered);
    assert(sdk_command_ui_drain(8) == 0);
    sdk_command_retire_owner(1);
    sdk_command_observe_native(NULL);
    assert(sdk_native_cues(2, &cues) == AZ_UNSUPPORTED);
    puts("PASS independently linked command service: copied native observations, unsupported admission and isolated owner bookkeeping");
    return 0;
}

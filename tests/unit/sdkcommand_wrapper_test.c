/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual SDK wrappers and owner helper, with synthetic module/engine state only. */
#define _POSIX_C_SOURCE 200809L
#include "azmod-mod.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int unused;
} cm_mod;
static cm_mod g_cm[2];
static int g_cm_in_frame;
static uintptr_t fixture_caller = 0x1234;
#define CM_RA() fixture_caller
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *service) {
    assert(service);
    if (!id || caller != 0x1234) return NULL;
    if (!strcmp(id, "alpha")) return &g_cm[0];
    if (!strcmp(id, "beta")) return &g_cm[1];
    return NULL;
}
#include SDK_COMMAND_OWNER_INC
#ifndef SDK_COMMAND_IMPL
#define SDK_COMMAND_IMPL "sdk_command.c"
#endif
#include SDK_COMMAND_IMPL
#include SDK_COMMAND_SUBMIT_INC

static int fake_arguments(void *context, const az_command_request *r, const struct scmd_target *target) {
    (void) context;
    (void) r;
    (void) target;
    return AZ_OK;
}
static int no_native_begin(void *context, const az_command_request *r, struct scmd_target *out) {
    (void) context;
    (void) r;
    (void) out;
    assert(!"No native adapter may be called by an unsupported SDK submission");
    return AZ_UNSUPPORTED;
}
static int no_native_dispatch(void *context, const az_command_request *r, uint64_t token, struct scmd_completion *out) {
    (void) context;
    (void) r;
    (void) token;
    (void) out;
    assert(!"No native action is allowed in the wrapper fixture");
    return AZ_UNSUPPORTED;
}
static void no_native_end(void *context, uint32_t deck) {
    (void) context;
    (void) deck;
    assert(!"No native lease exists in the wrapper fixture");
}
static az_command_request request(uint64_t id) {
    return (az_command_request) {.size = sizeof(az_command_request), .version = AZ_DATA_VERSION, .operation = AZ_CMD_LOOP_EXIT, .request_id = id, .expected_load_generation = 1, .deadline_ns = UINT64_MAX};
}
static az_command_result result(void) { return (az_command_result) {.size = sizeof(az_command_result)}; }
static void auth_rejections(void) {
    az_command_request r = request(1);
    az_command_result out = result();
    az_command_result before = out;
    assert(sdk_command_submit("unknown", &r, &out) == AZ_INVALID);
    assert(sdk_command_poll("unknown", 1, &out) == AZ_INVALID);
    assert(sdk_command_cancel("unknown", 1, &out) == AZ_INVALID);
    assert(sdk_command_release("unknown", 1) == AZ_INVALID);
    assert(!memcmp(&out, &before, sizeof out));
    fixture_caller = 0x5678;
    assert(sdk_command_submit("alpha", &r, &out) == AZ_INVALID);
    assert(sdk_command_poll("alpha", 1, &out) == AZ_INVALID);
    assert(sdk_command_cancel("alpha", 1, &out) == AZ_INVALID);
    assert(sdk_command_release("alpha", 1) == AZ_INVALID);
    fixture_caller = 0x1234;
    g_cm_in_frame = 1;
    assert(sdk_command_submit("alpha", &r, &out) == AZ_INVALID);
    assert(sdk_command_poll("alpha", 1, &out) == AZ_INVALID);
    assert(sdk_command_cancel("alpha", 1, &out) == AZ_INVALID);
    assert(sdk_command_release("alpha", 1) == AZ_INVALID);
    g_cm_in_frame = 0;
    assert(!memcmp(&out, &before, sizeof out));
}
static void malformed_submissions(void) {
    az_command_request r = request(1);
    for (unsigned field = 0; field < 17; field++) {
        az_command_request bad = r;
        switch (field) {
        case 0: bad.size--; break;
        case 1: bad.version++; break;
        case 2: bad.operation = 0; break;
        case 3: bad.operation = AZ_CMD_CUE_LOOP_SET + 1; break;
        case 4: bad.deck = 4; break;
        case 5: bad.request_id = 0; break;
        case 6: bad.expected_load_generation = 0; break;
        case 7: bad.deadline_ns = 0; break;
        case 8: bad.quantization = AZ_QUANTIZE_ON + 1; break;
        case 9: bad.flags = 4; break;
        case 10: bad.reserved = 1; break;
        case 11: bad.position_s = NAN; break;
        case 12: bad.end_s = INFINITY; break;
        case 13: bad.beats = -INFINITY; break;
        case 14: memset(bad.comment, 'x', sizeof bad.comment); break;
        case 15: bad.classification = AZ_CUE_MEMORY + 1; break;
        case 16: bad.rgb = 0xff000000u; break;
        }
        az_command_result out;
        memset(&out, 0xa5, sizeof out);
        out.size = sizeof out;
        az_command_result before = out;
        assert(sdk_command_submit("alpha", &bad, &out) == AZ_INVALID);
        assert(!memcmp(&out, &before, sizeof out));
    }
    uint32_t *tiny = malloc(sizeof *tiny);
    assert(tiny);
    *tiny = sizeof *tiny;
    az_command_result out = result();
    assert(sdk_command_submit("alpha", (const void *) tiny, &out) == AZ_INVALID);
    assert(sdk_command_submit("alpha", &r, (void *) tiny) == AZ_INVALID);
    assert(sdk_command_poll("alpha", 1, (void *) tiny) == AZ_INVALID);
    assert(sdk_command_cancel("alpha", 1, (void *) tiny) == AZ_INVALID && *tiny == sizeof *tiny);
    free(tiny);
    assert(sdk_command_submit("alpha", NULL, &out) == AZ_INVALID);
    assert(sdk_command_submit("alpha", &r, NULL) == AZ_INVALID);
    assert(sdk_command_poll("alpha", 0, &out) == AZ_INVALID);
    assert(sdk_command_cancel("alpha", 0, &out) == AZ_INVALID);
    assert(sdk_command_release("alpha", 0) == AZ_INVALID);
}
static void refusal_and_shared_state(void) {
    az_command_request r = request(1);
    struct {
        az_command_result value;
        uint64_t guard;
    } out = {.value = {.size = sizeof out}, .guard = UINT64_MAX};
    r.size += 32;
    assert(sdk_command_submit("alpha", &r, &out.value) == AZ_UNSUPPORTED);
    assert(out.value.outcome == AZ_COMMAND_REJECTED && out.value.reason == AZ_UNSUPPORTED && out.value.request_id == 1);
    assert(out.value.observed_ns && !out.value.revision && out.value.size == sizeof out && out.guard == UINT64_MAX);
    assert(sdk_command_poll("alpha", 1, &out.value) == AZ_STALE);
    assert(sdk_command_cancel("alpha", 1, &out.value) == AZ_STALE);
    assert(sdk_command_release("alpha", 1) == AZ_STALE);
    struct scmd_change event = {.size = sizeof event};
    assert(scmd_event(&g_sdk_commands, 0, 0, &event) == AZ_EMPTY);

    /* Test-only direct engine admission proves wrappers share real state. This
     * adapter is never reachable from sdk_command_submit or any firmware path. */
    const struct scmd_adapter adapter = {NULL, fake_arguments, no_native_begin, no_native_dispatch, no_native_end};
    const struct scmd_target target = {1, 10, UINT64_C(1) << AZ_CMD_LOOP_EXIT, 0, 0, SCMD_TARGET_LOAD | SCMD_TARGET_REVISION, 1};
    assert(scmd_submit(&g_sdk_commands, 0, &r, &target, &adapter, &out.value, NULL) == AZ_OK);
    assert(sdk_command_poll("alpha", 1, &out.value) == AZ_OK && out.value.outcome == AZ_COMMAND_ACCEPTED);
    assert(out.value.size == sizeof out && out.guard == UINT64_MAX);
    az_command_result before = out.value;
    assert(sdk_command_poll("beta", 1, &out.value) == AZ_STALE && !memcmp(&out.value, &before, sizeof before));
    assert(sdk_command_cancel("beta", 1, &out.value) == AZ_STALE);
    assert(sdk_command_release("beta", 1) == AZ_STALE);
    assert(sdk_command_poll("alpha", 1, &out.value) == AZ_OK && out.value.outcome == AZ_COMMAND_ACCEPTED);
    assert(sdk_command_cancel("alpha", 1, &out.value) == AZ_OK && out.value.outcome == AZ_COMMAND_CANCELLED && out.value.reason == AZ_CANCELLED);
    assert(out.value.size == sizeof out && out.guard == UINT64_MAX);
    before = out.value;
    assert(sdk_command_poll("alpha", 1, &out.value) == AZ_OK && !memcmp(&out.value, &before, sizeof before));
    assert(sdk_command_release("alpha", 1) == AZ_OK && sdk_command_release("alpha", 1) == AZ_STALE);
    assert(sdk_command_poll("alpha", 1, &out.value) == AZ_STALE);
    assert(scmd_destroy(&g_sdk_commands) == AZ_OK);
}
int main(void) {
    assert(sdk_command_capabilities() == 0);
    sdk_command_retire_owner(-1);
    auth_rejections();
    puts("PASS sdk command wrappers: module/caller/frame authentication");
    malformed_submissions();
    puts("PASS sdk command wrappers: full envelope and real short prefixes");
    refusal_and_shared_state();
    puts("PASS sdk command wrappers: unsupported submission; shared owner-scoped result/cancel/release");
    return 0;
}

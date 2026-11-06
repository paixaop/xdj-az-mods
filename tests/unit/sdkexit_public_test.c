/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual authenticated public wrappers and portable engine; native service is
 * the production C++ mailbox, driven separately by genuine fixture owner calls. */
#include "azmod-mod.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct {
    int unused;
} cm_mod;
static cm_mod g_cm[2];
static int g_cm_in_frame;
static uintptr_t fixture_caller = 0x1234;
#define CM_RA() fixture_caller
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *service) {
    (void) service;
    if (!id || caller != 0x1234) return NULL;
    if (!strcmp(id, "one")) return &g_cm[0];
    if (!strcmp(id, "two")) return &g_cm[1];
    return NULL;
}
#include SDK_EXIT_OWNER_INC
#include "sdk_command.c"
#include SDK_EXIT_SUBMIT_INC
void sdk_exit_public_bind(const struct azexit_service *api) {
    const struct azexit_delivery_v2 *delivery = sdk_exit_delivery(api);
    struct azexit_binding binding = {sizeof binding, AZEXIT_DELIVERY_VERSION, SCMD_SLOTS, SCMD_PER_OWNER};
    if (delivery && delivery->bind(&binding) != AZ_OK) delivery = NULL;
    g_sdk_commands.delivery = delivery;
    atomic_store_explicit(&g_sdk_exit_service, api, memory_order_release);
    atomic_store_explicit(&g_sdk_exit_bound, delivery, memory_order_release);
}
void sdk_exit_public_retire(int owner) {
    sdk_command_retire_owner(owner);
    sdk_command_drive();
    sdk_command_owner_init(owner);
    sdk_command_owner_ready(owner);
}
static uint64_t test_clock_value;
static uint64_t test_clock(void *unused) {
    (void) unused;
    return test_clock_value;
}
void sdk_exit_public_clock(uint64_t time) {
    test_clock_value = time;
    g_sdk_commands.clock = time ? test_clock : scmd_monotonic;
}
uint64_t sdk_exit_public_capabilities(void) { return sdk_command_capabilities(); }
int sdk_exit_public_submit(const char *owner, const az_command_request *r, az_command_result *out) {
    int rc = sdk_command_submit(owner, r, out);
    sdk_command_drive(); /* deterministic fixture dispatcher; production uses descriptors */
    return rc;
}
int sdk_exit_public_poll(const char *owner, uint64_t id, az_command_result *out) {
    sdk_command_drive();
    return sdk_command_poll(owner, id, out);
}
int sdk_exit_public_cancel(const char *owner, uint64_t id, az_command_result *out) {
    int rc = sdk_command_cancel(owner, id, out);
    sdk_command_drive();
    return rc;
}
int sdk_exit_public_release(const char *owner, uint64_t id) {
    sdk_command_drive();
    return sdk_command_release(owner, id);
}
void sdk_exit_public_auth(void) {
    az_command_request r = {.size = sizeof r, .version = 1, .operation = AZ_CMD_LOOP_EXIT, .request_id = 1, .flags = AZ_COMMAND_NATIVE_LOAD, .expected_load_generation = 1, .deadline_ns = UINT64_MAX};
    for (unsigned operation = AZ_CMD_LOOP_IN; operation <= AZ_CMD_CUE_COLOR; ++operation) {
        r.operation = operation;
        az_command_result out = {.size = sizeof out}, before = out;
        assert(sdk_command_submit("unknown", &r, &out) == AZ_INVALID && !memcmp(&out, &before, sizeof out));
        g_cm_in_frame = 1;
        assert(sdk_command_submit("one", &r, &out) == AZ_INVALID);
        assert(sdk_command_poll("one", 1, &out) == AZ_INVALID);
        g_cm_in_frame = 0;
        fixture_caller = 1;
        assert(sdk_command_submit("one", &r, &out) == AZ_INVALID);
        assert(sdk_command_cancel("one", 1, &out) == AZ_INVALID);
        fixture_caller = 0x1234;
    }
}

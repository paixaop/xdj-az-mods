/* SPDX-License-Identifier: GPL-3.0-only */
#include "azmod-mod.h"
#include "azexit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
struct scmd_target;
#include "exit_contracts.inc"

/* Admission examines descriptors without invoking any advertised callback. */
static int event_fd(void) { abort(); }
static int consume(struct azexit_receipt *out) {
    (void) out;
    abort();
}
static int reclaim(const struct azexit_receipt *receipt) {
    (void) receipt;
    abort();
}
static int control(unsigned deck, uint64_t token, uint32_t reasons, uint64_t now_ns, struct azexit_receipt *winner) {
    (void) deck;
    (void) token;
    (void) reasons;
    (void) now_ns;
    (void) winner;
    abort();
}
static int bind_core(const struct azexit_binding *core) {
    (void) core;
    abort();
}
static int reserve(const struct azexit_request *request) {
    (void) request;
    abort();
}
static void drive(void) { abort(); }
static uint64_t ready(unsigned deck) {
    (void) deck;
    abort();
}
static void test_delivery(void) {
    const struct azexit_delivery_v2 good = {sizeof good, AZEXIT_DELIVERY_VERSION, event_fd, consume, reclaim, control, bind_core, reserve, reclaim, drive, ready};
    struct azexit_delivery_v2 delivery = good;
    struct azexit_service api = {.size = sizeof api, .version = 1, .delivery = &delivery};
    assert(!sdk_exit_delivery(NULL));
    assert(sdk_exit_delivery(&api) == &delivery);
    api.version = 2;
    assert(!sdk_exit_delivery(&api));
    api.version = 1;
    api.size = AZEXIT_SERVICE_DELIVERY_SIZE - 1;
    assert(!sdk_exit_delivery(&api));
    api.size = sizeof api;
    api.delivery = NULL;
    assert(!sdk_exit_delivery(&api));
    api.delivery = &delivery;
    delivery.size = sizeof delivery - 1;
    assert(!sdk_exit_delivery(&api));
    delivery = good;
    delivery.version = AZEXIT_DELIVERY_VERSION + 1;
    assert(!sdk_exit_delivery(&api));
    for (unsigned missing = 0; missing < 9; ++missing) {
        delivery = good;
        switch (missing) {
        case 0: delivery.event_fd = NULL; break;
        case 1: delivery.consume = NULL; break;
        case 2: delivery.reclaim = NULL; break;
        case 3: delivery.control = NULL; break;
        case 4: delivery.bind = NULL; break;
        case 5: delivery.reserve = NULL; break;
        case 6: delivery.close_local = NULL; break;
        case 7: delivery.drive = NULL; break;
        case 8: delivery.ready = NULL; break;
        }
        assert(!sdk_exit_delivery(&api));
    }
    delivery = good;
    assert(sdk_exit_delivery(&api) == &delivery);
}
static void test_beats(void) {
    const struct {
        double beats;
        uint32_t encoded;
    } values[] = {
        {1.5, UINT32_C(0x00020003)}, {1.0 / 3.0, UINT32_C(0x00030001)}, {1.0 / 65535.0, UINT32_C(0xffff0001)}, {65535.0, UINT32_C(0x0001ffff)}, {32767.5, UINT32_C(0x0002ffff)}, {1.0 / 64.0, UINT32_C(0x00400001)}};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        uint32_t encoded = 0;
        assert(azexit_pack_beat(values[i].beats, &encoded) && encoded == values[i].encoded);
    }
    const double refused[] = {NAN, INFINITY, -INFINITY, 0, -1, 65536.0, 1.0 / 65536.0, 0x1.0000000000001p0};
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        uint32_t encoded = UINT32_C(0xfeed1234);
        assert(!azexit_pack_beat(refused[i], &encoded) && encoded == UINT32_C(0xfeed1234));
    }
    assert(!azexit_pack_beat(1.0, NULL));
}
static void test_arguments(void) {
    az_command_request request = {.operation = AZ_CMD_LOOP_LENGTH, .flags = AZ_COMMAND_NATIVE_LOAD, .beats = 1.5};
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_OK);
    request.operation = 0;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_UNSUPPORTED);
    request.operation = AZ_CMD_CUE_LOOP_SET + 1;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_UNSUPPORTED);
    request.operation = AZ_CMD_LOOP_LENGTH;
    request.flags = 0;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_UNSUPPORTED);
    request.flags = AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_PERSIST;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request.flags = AZ_COMMAND_NATIVE_LOAD;
    request.quantization = AZ_QUANTIZE_NATIVE + 1;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request.quantization = AZ_QUANTIZE_NATIVE;
    request.expected_revision = 1;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request.expected_revision = 0;
    request.beats = 0x1.0000000000001p0;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request.operation = AZ_CMD_CUE_LOOP_SET;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_OK); /* cue adapter owns its selector/value contract */
    request = (az_command_request) {.operation = AZ_CMD_LOOP_MOVE, .flags = AZ_COMMAND_NATIVE_LOAD, .position_s = -0.25};
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_OK);
    request.end_s = 0.5;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request.operation = AZ_CMD_LOOP_ADJUST_OUT;
    request.position_s = 0;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_OK);
    request.end_s = INFINITY;
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
    request = (az_command_request) {.operation = AZ_CMD_LOOP_IN, .flags = AZ_COMMAND_NATIVE_LOAD};
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_OK);
    request.comment[0] = 'x';
    assert(sdk_exit_arguments(NULL, &request, NULL) == AZ_INVALID);
}
int main(void) {
    test_delivery();
    test_beats();
    test_arguments();
    puts("PASS exit contracts: ABI prefix and all required callbacks, exact bounded beat packing, copied native operation and argument admission");
    return 0;
}

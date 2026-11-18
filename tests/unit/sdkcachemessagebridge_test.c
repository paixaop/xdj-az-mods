/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual C bridge and copied context, with caller-owned native-shaped storage. */
#include "azcachemessage.h"
#include "azcachebirth.h"
#include "azcacheentry.h"
#include "native_abi_fixture.h"
#include "cache_message_fixture.h"
#include <stdio.h>
#define REQUIRE(x, why)                                        \
    do {                                                       \
        if (!(x)) {                                            \
            fprintf(stderr, "FAIL message-bridge: %s\n", why); \
            abort();                                           \
        }                                                      \
    } while (0)
/* Any accidental cache dispatch fails instead of supplying an observer model. */
void azcb_retire_forward(struct azcb_forward f) {
    (void) f;
    abort();
}
void azcb_selection_forward(struct azcb_forward f) {
    (void) f;
    abort();
}
void azcb_observe_construction_write_owned(const struct azcb_write_borrow *b, struct azcb_forward f) {
    (void) b;
    (void) f;
    abort();
}
void azcb_observe_factory_owned(const struct azcb_factory_borrow *b, struct azcb_forward f) {
    (void) b;
    (void) f;
    abort();
}
void azsdk_cache_invoke_original(struct azce_call *c) {
    (void) c;
    abort();
}
typedef void (*Original)(void *, void *, unsigned);
void azsdk_sources_forward_owned_message(int, void *, void *, unsigned, Original) __attribute__((weak));
static unsigned original_calls, expected_parameter, deck;
static void *expected_server, *expected_message;
static struct fixture_message *message;
static bool throwing;
static int build;
static void original(void *server, void *input, unsigned parameter) {
    ++original_calls;
    REQUIRE(server == expected_server && input == expected_message && parameter == expected_parameter, "native arguments unchanged");
    REQUIRE(((struct fixture_message *) input)->begin[1].value == 117, "native original sees genuine unchanged input");
    puts("NATIVE_C_MESSAGE_FORWARD_OK args=3 content=117");
    fflush(stdout);
    if (throwing) fixture_raise();
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(server, &e), "missing actual C bridge owned message frame");
    REQUIRE(e.fields[1] == 117 && e.parameter == expected_parameter && e.client == deck && e.selector == deck + 1, "bridge forwards configured build and original callback inside exact copied scope");
    message_delete(message);
    message = NULL;
    REQUIRE(azcm_current(server, &e) && e.fields[1] == 117, "C bridge never rereads freed message");
}
static void forward(void *unused) {
    (void) unused;
    if (azsdk_sources_forward_owned_message) azsdk_sources_forward_owned_message(build, expected_server, message, expected_parameter, original);
    else original(expected_server, message, expected_parameter);
}
static void absent(void *server) {
    struct azcm_envelope e = {.fields = {[1] = 0xcafe}};
    REQUIRE(!azcm_current(server, &e) && e.fields[1] == 0xcafe, "no leaked older frame");
}
int main(void) {
    static int server;
    for (build = 1; build <= 2; ++build)
        for (deck = 0; deck < 4; ++deck) {
            message = message_new(deck, 99);
            expected_server = &server;
            expected_message = message;
            expected_parameter = 0x80000001u + deck;
            original_calls = 0;
            throwing = false;
            forward(NULL);
            REQUIRE(original_calls == 1 && !message, "C original exactly once");
            absent(&server);
            message = message_new(deck, 99);
            expected_message = message;
            original_calls = 0;
            throwing = true;
            unsigned deletions = fixture_exception_deletions;
            REQUIRE(!az_native_try(forward, NULL) && fixture_exception_deletions == deletions + 1 && original_calls == 1, "native unwind propagates through C bridge");
            message_delete(message);
            message = NULL;
            absent(&server);
        }
    puts("PASS message-bridge: actual C forwarding, copied context, cleanup and native unwind restoration;4contexts/bothprofiles");
}

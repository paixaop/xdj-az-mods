/* SPDX-License-Identifier: GPL-3.0-only */
/* Only the preexisting onMessage tail is extracted. The actual C bridge is
 * linked unchanged; the original destroys its caller-owned native input. */
#include "azcachemessage.h"
#include "azsources.h"
#include "native_abi_fixture.h"
#include "cache_message_fixture.h"
#include <stdio.h>
#define REQUIRE(x, why)                                      \
    do {                                                     \
        if (!(x)) {                                          \
            fprintf(stderr, "FAIL message-hook: %s\n", why); \
            abort();                                         \
        }                                                    \
    } while (0)
static int g_sdk_sources_on, g_ep147_build, stage, source_calls, pst_calls;
static unsigned context_id;
static bool throwing;
static void *expected_server, *expected_message;
static void original(void *server, void *message, unsigned parameter) {
    REQUIRE(server == expected_server && message == expected_message && parameter == 37 && stage == 0, "actual native args/order unchanged");
    REQUIRE(((struct fixture_message *) message)->begin[1].value == 117, "original still receives unchanged live input");
    puts("NATIVE_HOOK_FORWARD_OK args=3 content=117");
    fflush(stdout);
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(server, &e) == (bool) g_sdk_sources_on, "native hook must enter copied message bridge before original");
    if (g_sdk_sources_on) REQUIRE(e.fields[1] == 117 && e.client == context_id && e.selector == context_id + 1, "configured copied scalars inside original");
    message_delete(message);
    expected_message = NULL;
    stage = 1;
    if (g_sdk_sources_on) REQUIRE(azcm_current(server, &e) && e.fields[1] == 117, "frame survives input destruction without reread");
    if (throwing) fixture_raise();
}
static azsdk_source_message_original g_orig_mlserver_onmsg = original;
int azsdk_sources_observe_owned_message(void *server, void *out, size_t size) {
    struct azcm_envelope e = {0};
    REQUIRE(server == expected_server && !out && !size && stage == 1 && !azcm_current(server, &e), "source post-observation after original scope cleanup");
    ++source_calls;
    stage = 2;
    return AZS_OK;
}
static void pst_msg_done(void *server) {
    REQUIRE(server == expected_server && stage == (g_sdk_sources_on ? 2 : 1), "playlist post-step keeps original order");
    ++pst_calls;
    stage = 3;
}
static void hook_tail(void *server, void *msg, unsigned param) {
/* ACTUAL_NATIVE_HOOK_TAIL */
    static void hook_forward(void *unused) {
        (void) unused;
        hook_tail(expected_server, expected_message, 37);
    }
    int main(void) {
        for (g_ep147_build = 1; g_ep147_build <= 2; ++g_ep147_build)
            for (context_id = 0; context_id < 4; ++context_id)
                for (g_sdk_sources_on = 0; g_sdk_sources_on < 2; ++g_sdk_sources_on)
                    for (int fail = 0; fail < 2; ++fail) {
                        int server;
                        expected_server = &server;
                        expected_message = message_new(context_id, 13);
                        throwing = fail;
                        stage = source_calls = pst_calls = 0;
                        unsigned deletions = fixture_exception_deletions;
                        bool caught = !az_native_try(hook_forward, NULL);
                        struct azcm_envelope e = {0};
                        REQUIRE(caught == throwing && fixture_exception_deletions == deletions + (unsigned) throwing && !expected_message && !azcm_current(expected_server, &e), "native unwind/normal cleanup remains transparent");
                        REQUIRE(throwing ? (stage == 1 && !source_calls && !pst_calls) : (stage == 3 && source_calls == g_sdk_sources_on && pst_calls == 1), "post-steps only after normal native return");
                    }
        puts("PASS message-hook: actual tail enters one bridge, preserves input/unwind/postorder;4contexts/bothprofiles");
    }

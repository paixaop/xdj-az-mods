/* SPDX-License-Identifier: GPL-3.0-only */
/* Real copied-context helper plus a missing C/native forwarding bridge.
 * No native source association, transport key or firmware code is simulated. */
#include "azcachemessage.h"
#include "azcachebirth.h"
#include "azcacheentry.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <vector>
#define REQUIRE(x, why)                                             \
    do {                                                            \
        if (!(x)) {                                                 \
            std::fprintf(stderr, "FAIL message-bridge: %s\n", why); \
            std::abort();                                           \
        }                                                           \
    } while (0)
/* This fixture links only the message bridge. Any accidental generic cache
 * dispatch is a test failure, never a silently successful retirement model. */
namespace azsdk_cache_birth {
void retire_forward(Forward) { std::abort(); }
void selection_forward(Forward) { std::abort(); }
void observe_construction_write_owned(const WriteBorrow &, Forward) { std::abort(); }
void observe_factory_owned(const FactoryBorrow &, Forward) { std::abort(); }
} // namespace azsdk_cache_birth
extern "C" void azsdk_cache_invoke_original(azce_call *) { std::abort(); }
using Original = void (*)(void *, void *, unsigned);
extern "C" void azsdk_sources_forward_owned_message(int, void *, void *, unsigned, Original) __attribute__((weak));
namespace {
namespace cm = azsdk_cache_message;
struct alignas(8) Arg {
    std::uint32_t tag = 1, pad = 0, value = 0;
    unsigned char tail[36] = {};
};
struct Msg {
    std::uint16_t kind = 0x2102, reserved = 0;
    std::uint32_t magic = 0x872349ae, sequence = 99, pad = 0;
    std::uint64_t unused[3] = {};
    std::vector<Arg> args;
    Msg(unsigned position, unsigned content) : args(2) {
        args[0].value = position;
        args[1].value = content;
    }
};
std::function<void()> action;
unsigned original_calls;
void *expected_server;
void *expected_message;
unsigned expected_parameter;
void original(void *server, void *message, unsigned parameter) {
    ++original_calls;
    REQUIRE(server == expected_server && message == expected_message && parameter == expected_parameter, "native arguments unchanged");
    REQUIRE(static_cast<Msg *>(message)->args[1].value == 117, "native original sees genuine unchanged input");
    std::puts("NATIVE_C_MESSAGE_FORWARD_OK args=3 content=117");
    std::fflush(stdout);
    action();
}
void forward(int build, void *server, Msg *message, unsigned parameter) {
    if (azsdk_sources_forward_owned_message) azsdk_sources_forward_owned_message(build, server, message, parameter, original);
    else original(server, message, parameter); // old bridge has no alternate observer/model
}
void absent(void *server) {
    cm::Envelope e;
    e.fields[1] = 0xcafe;
    REQUIRE(!cm::current(server, e) && e.fields[1] == 0xcafe, "no leaked older frame");
}
} // namespace
int main() {
    static_assert(sizeof(Msg) == 64 && sizeof(Arg) == 48, "native message fixture prefix");
    static int server;
    for (int build = 1; build <= 2; ++build)
        for (unsigned deck = 0; deck < 4; ++deck) {
            auto message = new Msg((deck << 24) | ((deck + 1) << 16), 117);
            expected_server = &server;
            expected_message = message;
            expected_parameter = 0x80000001u + deck;
            original_calls = 0;
            action = [&] {cm::Envelope e;REQUIRE(cm::current(&server,e),"missing actual C bridge owned message frame");
   REQUIRE(e.fields[1]==117&&e.parameter==expected_parameter&&e.client==deck&&e.selector==deck+1,"bridge forwards configured build and original callback inside exact copied scope");
   delete message;message=nullptr;REQUIRE(cm::current(&server,e)&&e.fields[1]==117,"C bridge never rereads freed message"); };
            forward(build, &server, message, expected_parameter);
            REQUIRE(original_calls == 1 && !message, "C original exactly once");
            absent(&server);
            message = new Msg((deck << 24) | ((deck + 1) << 16), 117);
            expected_message = message;
            original_calls = 0;
            action = [] { throw std::runtime_error("native C callback exception"); };
            bool caught = false;
            try {
                forward(build, &server, message, expected_parameter);
            } catch (const std::runtime_error &) { caught = true; }
            REQUIRE(caught && original_calls == 1, "native exception propagates through C bridge");
            delete message;
            absent(&server);
        }
    std::puts("PASS message-bridge: actual C forwarding, copied context, cleanup and exception restoration;4contexts/bothprofiles");
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual dispatcher/SourceCpp, inherited frame-only machine boundary. Native
 * bodies/types below are synthetic ABI fixtures, not recovered nominal weak
 * specializations, installed hooks, transport/source or load association. */
#include "../../src/common/azcacheentry.h"
#define SDK_CACHE_NATIVE_MAIN sdkcachereply_native_baseline
#include "sdkcachenative_test.cpp"
#undef SDK_CACHE_NATIVE_MAIN

#if defined(__linux__)
#include "../../src/common/azcachereply.h"
#if defined(__has_include)
#if __has_include("../../src/common/azcachereplynative.h")
#include "../../src/common/azcachereplynative.h"
#define HAVE_CACHE_REPLY_NATIVE 1
#endif
#endif
#ifndef HAVE_CACHE_REPLY_NATIVE
namespace azsdk_cache_reply {
struct CallbackReceipt {
    Receipt selected;
    std::uint32_t pack_id = 0;
    std::uintptr_t borrowed_stocker = 0;
};
}
#endif
namespace azsdk_cache_reply {
bool current_callback_owned(CallbackReceipt &) noexcept __attribute__((weak));
}
extern "C" void azsdk_cache_test_reply_profile(unsigned, std::uintptr_t, std::uintptr_t,
                                               std::size_t, const char *) noexcept __attribute__((weak));
extern "C" void azsdk_reply_fixture_hash(const void *, std::size_t, char *);
#ifndef AZCE_KIND_REPLY_TICKET
#define AZCE_KIND_REPLY_TICKET 21
#endif
#ifndef AZCE_KIND_REPLY_RECEPTION
#define AZCE_KIND_REPLY_RECEPTION 22
#endif

namespace reply_fixture {
namespace cr = azsdk_cache_reply;
bool capture_ready = true;
unsigned packets = 0, receivers = 0, tickets = 0;
struct Packet {
    std::uint16_t kind = 0x4001;
    unsigned char opaque[6] = {};
    std::uint32_t sequence = 0;
    std::uint32_t opaque0c = 0;
    unsigned *fixture_destroyed = nullptr; // fixture-only tail, not a native field claim
    unsigned char tail[0x40 - 24] = {};
    explicit Packet(std::uint32_t seq) : sequence(seq) { ++packets; }
    ~Packet() { --packets; if (fixture_destroyed) ++*fixture_destroyed; }
};
static_assert(sizeof(Packet) == 0x40 && offsetof(Packet, sequence) == 8, "received native-shaped packet prefix");
template<class T> std::uintptr_t control(const std::shared_ptr<T> &owner) {
    static_assert(sizeof owner == 16, "actual GNU strong owner pair");
    std::uintptr_t words[2]; std::memcpy(words, &owner, sizeof words); return words[1];
}
bool receipt(cr::CallbackReceipt &out) {
    auto getter = cr::current_callback_owned;
    return getter && getter(out);
}
bool equal_request(const cr::Request &a, const cr::Request &b) {
    return a.request_id[0] == b.request_id[0] && a.request_id[1] == b.request_id[1] &&
           a.sequence == b.sequence && a.reply_kind == b.reply_kind;
}
bool same_receipt(const cr::CallbackReceipt &a, const cr::CallbackReceipt &b) {
    return equal_request(a.selected.request, b.selected.request) &&
           !a.selected.reception.owner_before(b.selected.reception) &&
           !b.selected.reception.owner_before(a.selected.reception) &&
           a.pack_id == b.pack_id && a.borrowed_stocker == b.borrowed_stocker;
}
void absent() {
    cr::CallbackReceipt out;
    out.selected.request.request_id[1] = UINT64_C(0xdead1234abcd5678);
    out.pack_id = 0x98765432; out.borrowed_stocker = 0x12345678;
    const auto before = out;
    REQUIRE(!receipt(out) && same_receipt(out, before), "callback receipt unavailable outside actual native callback scope");
}
}

// Actual main-ELF RTTI names/vtable widths expected by the production guards.
// Weak<Reception> is known ONLY for this fixture's explicit C++ declaration.
namespace trackinfo_stocker {
class LoadInfoRequestHandler {
public:
    class Reception {
    public:
        virtual ~Reception() { --reply_fixture::receivers; }
        virtual void replyLoadInfo(const void *) {}
        std::uint32_t pack_id = 0, opaque = 0xaabbccdd;
        void *stocker = nullptr;
        Reception(std::uint32_t pack, void *borrowed) : pack_id(pack), stocker(borrowed) { ++reply_fixture::receivers; }
    };
};
}
namespace music_library {
class LoadInfoRequestTicket {
public:
    virtual ~LoadInfoRequestTicket() { delete packet; --reply_fixture::tickets; }
    virtual void slot2() {}
    virtual void *slot3() { std::abort(); }
    virtual void slot4() {} virtual void slot5() {} virtual void slot6() {}
    virtual void slot7() {} virtual void slot8() {} virtual void slot9() {}
    virtual void slot10() {} virtual void slot11() {} virtual void slot12() {} virtual void slot13() {}
    std::uint64_t request_id[2] = {};
    unsigned char opaque18[8] = {};
    std::uint32_t expected_sequence = 0, opaque24 = 0;
    reply_fixture::Packet *packet = nullptr;
    unsigned char opaque30[0x41 - 0x30] = {};
    unsigned char state = 2;
    unsigned char opaque42[0x80 - 0x42] = {};
    std::uint32_t tag = 2, opaque84 = 0;
    std::weak_ptr<trackinfo_stocker::LoadInfoRequestHandler::Reception> receiver;
    explicit LoadInfoRequestTicket(const std::shared_ptr<trackinfo_stocker::LoadInfoRequestHandler::Reception> &owner,
                                  unsigned context) : receiver(owner) {
        ++reply_fixture::tickets;
        request_id[0] = UINT64_C(0x8070605040302000) + context;
        request_id[1] = UINT64_C(0xfedcba9876543200) + context;
        expected_sequence = 0xfffffff0U + context;
        const std::uint16_t expected_kind = 0x4001;
        std::memcpy(opaque18 + 2, &expected_kind, sizeof expected_kind);
        REQUIRE(offset(this, &request_id) == 8 && offset(this, &packet) == 0x28 &&
                offset(this, &state) == 0x41 && offset(this, &tag) == 0x80 && offset(this, &receiver) == 0x88,
                "genuine GNU native-shaped Ticket owner/request/received-packet fields");
        packet = new reply_fixture::Packet(expected_sequence);
    }
};
}

extern "C" int azsdk_cache_capture_ready(void) { return reply_fixture::capture_ready ? 1 : 0; }

namespace reply_fixture {
using Reception = trackinfo_stocker::LoadInfoRequestHandler::Reception;
using Ticket = music_library::LoadInfoRequestTicket;
struct Call {
    std::uint64_t stocker_marker = 0;
    std::shared_ptr<Reception> owner;
    std::unique_ptr<Ticket> ticket;
    cr::Request expected;
    cr::CallbackReceipt observed;
    std::function<void()> before_promotion, inside_callback, after_callback;
    unsigned forwarded = 0, callbacks = 0, packet_destroyed = 0;
    bool got = false, drop_ticket = false;
    std::uintptr_t callback_link = 0, callback_control = 0;
    std::uint32_t ticket_reserved = 0, reception_reserved = 0;
    std::uint64_t ticket_caller_reserved = 0, reception_caller_reserved = 0;
    explicit Call(unsigned context, std::shared_ptr<Reception> actual = {})
        : owner(actual ? actual : std::make_shared<Reception>(0x88776650U + context, &stocker_marker)),
          ticket(new Ticket(owner, context)) {
        REQUIRE(offset(owner.get(), &owner->pack_id) == 8 && offset(owner.get(), &owner->stocker) == 0x10,
                "native Reception scalar prefix is packu32+8/raw Stocker+10");
        std::memcpy(expected.request_id, ticket->request_id, sizeof expected.request_id);
        expected.sequence = ticket->packet->sequence; expected.reply_kind = ticket->packet->kind;
        ticket->packet->fixture_destroyed = &packet_destroyed;
        observed.selected.request.request_id[1] = UINT64_C(0xdeadbeef00112233);
        observed.pack_id = 0x10293847; observed.borrowed_stocker = 0x87654321;
        callback_link = code_begin() + 4;
    }
    ~Call() { ticket.reset(); }
    static std::uintptr_t code_begin() { return reinterpret_cast<std::uintptr_t>(&ticket_original); }
    static void profile(bool bad_hash = false, unsigned profile_build = 0) {
        char hash[65];
        // This is a SYNTHETIC exact RX span in the test main ELF, never a
        // native slot13/FDE proof. No bytes are changed or executed as an oracle.
        REQUIRE(source_state().main_image.contains(code_begin(), 16, kRead | kExecute),
                "test promotion profile refers to actual owned main-ELF RX bytes");
        azsdk_reply_fixture_hash(reinterpret_cast<const void *>(code_begin()), 16, hash);
        if (bad_hash) hash[0] = hash[0] == '0' ? '1' : '0';
        if (azsdk_cache_test_reply_profile)
            azsdk_cache_test_reply_profile(profile_build ? profile_build : build_id, code_begin(), code_begin() + 4, 16, hash);
    }
    static void receiver_original(azce_call *frame) {
        auto &self = *reinterpret_cast<Call *>(frame->args[7]);
        ++self.callbacks;
        REQUIRE(frame->args[0] && frame->args[1] && frame->caller_x21 &&
                frame->caller_link == self.callback_link,
                "native Reception original receives unchanged owned receiver/payload/X21/LR");
        const auto before = self.observed;
        self.got = receipt(self.observed);
        REQUIRE(self.got || same_receipt(self.observed, before), "refused native callback receipt preserves output");
        if (self.inside_callback) self.inside_callback();
        frame->result[0] = UINT64_C(0x1345abcd87659870);
        frame->result[1] = UINT64_C(0xc001d00d88997766);
    }
    // The synthetic code-profile fixture must meet the ARM production
    // profile's instruction alignment even when compiled for an x86 host.
    __attribute__((aligned(16))) static void ticket_original(azce_call *frame) {
        auto &self = *reinterpret_cast<Call *>(frame->args[7]);
        ++self.forwarded;
        Ticket &native = *self.ticket;
        REQUIRE(frame->args[0] == reinterpret_cast<std::uintptr_t>(&native), "actual selected Ticket receiver unchanged");
        std::unique_ptr<Packet> packet(native.packet); native.packet = nullptr;
        if (self.before_promotion) self.before_promotion();
        if (native.state != 3) {
            // Native chooses its receiver NOW. The SDK must not pre-promote the
            // earlier Ticket weak owner or keep a cancelled handler alive.
            if (auto lease = native.receiver.lock()) {
                alignas(8) unsigned char borrowed_info[0x100] = {};
                azce_call receiver = {};
                receiver.kind = AZCE_KIND_REPLY_RECEPTION;
                receiver.reserved = self.reception_reserved;
                receiver.caller_reserved = self.reception_caller_reserved;
                receiver.args[0] = reinterpret_cast<std::uintptr_t>(lease.get());
                receiver.args[1] = reinterpret_cast<std::uintptr_t>(borrowed_info);
                receiver.args[7] = reinterpret_cast<std::uintptr_t>(&self);
                receiver.caller_x21 = self.callback_control ? self.callback_control : control(lease);
                receiver.caller_link = self.callback_link;
                receiver.original = reinterpret_cast<std::uintptr_t>(&receiver_original);
                const auto original = receiver;
                azsdk_cache_dispatch_call(&receiver);
                REQUIRE(std::memcmp(receiver.args, original.args, sizeof receiver.args) == 0 &&
                        receiver.caller_x21 == original.caller_x21 && receiver.caller_link == original.caller_link &&
                        receiver.result[0] == UINT64_C(0x1345abcd87659870) &&
                        receiver.result[1] == UINT64_C(0xc001d00d88997766),
                        "Reception dispatcher preserves native input frame/results");
                absent();
                if (self.after_callback) self.after_callback();
            }
        }
        native.state = 3;
        packet.reset();
        if (self.drop_ticket) self.ticket.reset();
        frame->result[0] = UINT64_C(0x2345abcd12349876);
        frame->result[1] = UINT64_C(0x9988776655443322);
    }
    void run() {
        cr::CallbackReceipt enclosing;
        const bool inside_enclosing_callback = receipt(enclosing);
        azce_call frame = {};
        frame.kind = AZCE_KIND_REPLY_TICKET;
        frame.reserved = ticket_reserved;
        frame.caller_reserved = ticket_caller_reserved;
        frame.args[0] = reinterpret_cast<std::uintptr_t>(ticket.get());
        frame.args[7] = reinterpret_cast<std::uintptr_t>(this);
        frame.original = reinterpret_cast<std::uintptr_t>(&ticket_original);
        const auto input = frame;
        azsdk_cache_dispatch_call(&frame);
        REQUIRE(forwarded == 1 && std::memcmp(frame.args, input.args, sizeof frame.args) == 0 &&
                frame.result[0] == UINT64_C(0x2345abcd12349876) &&
                frame.result[1] == UINT64_C(0x9988776655443322) && packet_destroyed == 1,
                "actual native Ticket original/result/packet cleanup completes once");
        if (inside_enclosing_callback) {
            cr::CallbackReceipt restored;
            REQUIRE(receipt(restored) && same_receipt(restored, enclosing),
                    "nested Ticket forwarding restores the still-active enclosing callback receipt");
        } else absent();
        std::printf("NATIVE_REPLY_FORWARD_OK build=%u callbacks=%u\n", build_id, callbacks); std::fflush(stdout);
    }
    void check() const {
        REQUIRE(got && equal_request(observed.selected.request, expected) &&
                !observed.selected.reception.owner_before(owner) && !owner.owner_before(observed.selected.reception) &&
                observed.selected.reception.lock().get() == owner.get() &&
                observed.pack_id == owner->pack_id && observed.borrowed_stocker == reinterpret_cast<std::uintptr_t>(owner->stocker),
                "missing actual Ticket dispatch to native-owned callback receipt");
        REQUIRE(owner.use_count() == 1, "actual dispatcher retains no strong Reception after original");
    }
};
void positive(unsigned context, bool baseline = false) {
    Call call(context); call.drop_ticket = true; call.run();
    REQUIRE(call.callbacks == 1 && !call.ticket && !tickets, "native alone cleans Ticket/weak holder before observer returns");
    if (baseline) {
        std::printf("NATIVE_REPLY_OWNER_BASELINE_OK build=%u\n", build_id); std::fflush(stdout);
    } else call.check();
}
void refused() {
    for (unsigned mode = 0; mode != 11; ++mode) {
        Call::profile(mode == 1, mode == 2 ? (build_id == 1 ? 2u : 1u) : 0);
        Call call(mode);
        auto foreign_owner = std::make_shared<Reception>(0, nullptr);
        if (mode == 0) call.callback_link += 4;
        if (mode == 3) call.callback_control = control(foreign_owner);
        if (mode == 4) capture_ready = false;
        if (mode == 5) call.ticket->tag = 1; // still a live genuine weak member, unsupported variant
        if (mode == 6 && azsdk_cache_test_reply_profile) azsdk_cache_test_reply_profile(0, 0, 0, 0, nullptr);
        if (mode == 7) call.ticket_reserved = 1;
        if (mode == 8) call.reception_reserved = 1;
        if (mode == 9) call.ticket_caller_reserved = 1;
        if (mode == 10) call.reception_caller_reserved = 1;
        call.run(); capture_ready = true;
        if (mode >= 7)
            REQUIRE(call.callbacks == 1 && !call.got,
                    "incompatible native frame forwards but must not expose an owned reply receipt");
        else REQUIRE(call.callbacks == 1 && !call.got, "unqualified code/PC/control/variant/startup still forwards without receipt");
    }
    Call::profile();
}
void cancellation_and_replacement() {
    Call cancel(0); cancel.before_promotion = [&] { cancel.owner.reset(); };
    cancel.run(); REQUIRE(!cancel.callbacks && !cancel.got, "SDK snapshot does not rescue native-expired receiver");
    Call cancelled(1); cancelled.ticket->state = 3;
    cancelled.run(); REQUIRE(!cancelled.callbacks && !cancelled.got, "native cancellation remains callback-free");
    Call replace(2); auto next = std::make_shared<Reception>(replace.owner->pack_id, replace.owner->stocker);
    replace.before_promotion = [&] { replace.ticket->receiver = next; };
    replace.run(); REQUIRE(replace.callbacks == 1 && !replace.got, "replacement with same scalars has different actual GNU owner");
    std::unique_ptr<Reception> pointee(new Reception(0, nullptr));
    auto a = std::shared_ptr<Reception>(pointee.get(), [](Reception *) {});
    auto b = std::shared_ptr<Reception>(pointee.get(), [](Reception *) {});
    Call alias(3, a); alias.before_promotion = [&] { alias.ticket->receiver = b; };
    alias.run(); REQUIRE(alias.callbacks == 1 && !alias.got, "same alias cannot replace actual native promoted control identity");
}
void nested_and_throw() {
    Call outer(0), inner(1);
    outer.inside_callback = [&] {
        cr::CallbackReceipt prior; REQUIRE(receipt(prior), "outer callback receipt exists before nested native dispatch");
        inner.run(); inner.check();
        cr::CallbackReceipt restored;
        REQUIRE(receipt(restored) && same_receipt(prior, restored), "nested callback restores exact actual outer receipt");
    };
    outer.run(); outer.check();
    Call throwing(2); bool caught = false;
    throwing.inside_callback = [] { throw std::runtime_error("native Reception original"); };
    try { throwing.run(); } catch (const std::runtime_error &e) { caught = !std::strcmp(e.what(), "native Reception original"); }
    REQUIRE(caught && throwing.forwarded == 1 && throwing.callbacks == 1 && !packets && throwing.owner.use_count() == 1,
            "native callback exception propagates once with native packet/strong owner cleanup");
    absent();
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!std::strcmp(argv[1], "1") || !std::strcmp(argv[1], "2")), "build arg");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK, "actual SourceState main-ELF guards initialized");
    reply_fixture::Call::profile(); reply_fixture::absent();
    reply_fixture::positive(0, true);
    for (unsigned i = 0; i != 4; ++i) reply_fixture::positive(i);
    reply_fixture::refused(); reply_fixture::cancellation_and_replacement(); reply_fixture::nested_and_throw();
    REQUIRE(!reply_fixture::packets && !reply_fixture::tickets && !reply_fixture::receivers,
            "native callers alone clean all Ticket/Reception/packet fixture owners");
    std::printf("PASS native-cache-reply build=%u: actual dispatcher/scoped owner receipt only\n", build_id);
}
#endif

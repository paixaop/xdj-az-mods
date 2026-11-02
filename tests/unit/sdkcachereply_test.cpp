/* SPDX-License-Identifier: GPL-3.0-only */
/* GNU ownership fixture for the retained synchronous Ticket slot3 -> slot13
 * -> Reception callback. These are native-shaped test types, not declarations
 * of native nominal C++ classes. No server-to-client transport/source join,
 * installed hook, TrackID, pack ID or player LoadRequestID is modeled here.
 *
 * Native glue must capture ORIGINAL incoming X21 and LR at Reception entry:
 * retained slot13 holds its promoted strong control in X21 across the callback.
 * The original caller PC must be bound to the exact supported code/profile.
 * A raw callback receiver or wire sequence cannot supply this promotion proof.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#if !defined(__GLIBCXX__)
#error "Reply fixture requires real GNU libstdc++ weak/shared ownership"
#endif
#if defined(__has_include)
#if __has_include("../../src/common/azcachereply.h")
#include "../../src/common/azcachereply.h"
#define HAVE_CACHE_REPLY 1
#endif
#endif
#ifndef HAVE_CACHE_REPLY
/* Proposed private seam. Absence forwards the real fixture native call; it
 * never fabricates an observation or promotes a receiver before native does. */
namespace azsdk_cache_reply {
struct Request {
    std::uint64_t request_id[2] = {};
    std::uint32_t sequence = 0;
    std::uint16_t reply_kind = 0;
};
struct Promotion { std::uintptr_t control = 0, caller_link = 0; };
struct Receipt {
    Request request;
    std::weak_ptr<void> reception;
};
class Scope {
public:
    template<class T> Scope(const std::weak_ptr<T> &, const Request &, std::uintptr_t) noexcept {}
    template<class F> auto forward(F &&original) -> decltype(original()) {
        return std::forward<F>(original)();
    }
    template<class T> bool receive_owned(const Promotion &, T *, Receipt &) noexcept {
        return false;
    }
};
inline bool receive_current_owned(const Promotion &, const void *, Receipt &) noexcept { return false; }
}
#endif
#define CHECK(x, why) do { if (!(x)) { std::fprintf(stderr, "FAIL cache-reply: %s\n", why); std::abort(); } } while (0)
namespace cr = azsdk_cache_reply;
static unsigned packets = 0, receptions = 0, native_calls = 0, callback_calls = 0;
static std::uintptr_t callback_return = 0;
// Read a real GNU owner object representation solely for an equality witness.
// No refcount/pointee dereference or shared_ptr construction from these bits.
template<class T> static std::uintptr_t control_of(const std::shared_ptr<T> &owner) {
    static_assert(sizeof owner == 16, "reviewed GNU shared owner width");
    std::uintptr_t words[2];
    std::memcpy(words, &owner, sizeof words);
    return words[1];
}
static cr::Promotion promotion(std::uintptr_t control, std::uintptr_t link = callback_return) {
    cr::Promotion p; p.control = control; p.caller_link = link; return p;
}
struct Packet {
    std::uint16_t kind;
    std::uint32_t sequence;
    explicit Packet(std::uint32_t seq) : kind(0x4001), sequence(seq) { ++packets; }
    ~Packet() { --packets; }
};
struct Receiver {
    std::function<void(Receiver *)> callback;
    Receiver() { ++receptions; }
    ~Receiver() { --receptions; }
    void reply() { ++callback_calls; if (callback) callback(this); }
};
struct Ticket {
    unsigned char prefix[0x80] = {};
    std::uint32_t tag = 2, padding = 0;
    std::weak_ptr<Receiver> receiver;
    std::unique_ptr<Packet> reply;
    cr::Request request;
    unsigned state = 1;
    Ticket(const std::shared_ptr<Receiver> &owner, unsigned context) : receiver(owner) {
        request.request_id[0] = 0x12340000000000ULL + context;
        request.request_id[1] = 0xfedcba9880000000ULL + context;
        request.sequence = 0xfffffff0U + context;
        request.reply_kind = 0x4001;
        CHECK(reinterpret_cast<const char *>(&receiver) - reinterpret_cast<const char *>(this) == 0x88,
              "native-shaped tag2 genuine weak owner begins at Ticket+88");
        static_assert(sizeof receiver == 16, "genuine GNU weak pair width");
    }
    // The test owns this complete Ticket for the whole call. Native slot3 owns
    // the received packet and destroys it after synchronous listener delivery.
    int process(const std::function<void(const cr::Promotion &, Receiver *)> &entry) {
        ++native_calls;
        std::unique_ptr<Packet> local = std::move(reply);
        if (state == 3 || !local) return 7;
        state = 2;
        // This is native's own lock, not an SDK pre-promotion. The scalar
        // control witness comes from that real strong owner, like native X21.
        if (auto lease = receiver.lock()) {
            entry(promotion(control_of(lease)), lease.get());
            lease->reply();
        }
        state = 3;
        return 7;
    }
};
static bool same_request(const cr::Request &a, const cr::Request &b) {
    return a.request_id[0] == b.request_id[0] && a.request_id[1] == b.request_id[1] &&
           a.sequence == b.sequence && a.reply_kind == b.reply_kind;
}
static bool same_owner(const std::weak_ptr<void> &a, const std::weak_ptr<void> &b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
static cr::Receipt sentinel(const std::shared_ptr<Receiver> &owner) {
    cr::Receipt out;
    out.request.request_id[0] = 0xfeed1234;
    out.request.request_id[1] = 0x12345678aabbccddULL;
    out.request.sequence = 0x778899aa;
    out.request.reply_kind = 0x7777;
    out.reception = owner;
    return out;
}
static bool unchanged(const cr::Receipt &a, const cr::Receipt &b) {
    return same_request(a.request, b.request) && same_owner(a.reception, b.reception) &&
           a.reception.lock().get() == b.reception.lock().get();
}
static void native_baseline() {
    auto handler = std::make_shared<Receiver>();
    Ticket ticket(handler, 0);
    ticket.reply.reset(new Packet(ticket.request.sequence));
    const unsigned before = native_calls;
    int callbacks = 0;
    handler->callback = [&](Receiver *self) { CHECK(self == handler.get(), "typed native receiver"); ++callbacks; };
    CHECK(ticket.process([&](const cr::Promotion &native, Receiver *self) {
        CHECK(native.control == control_of(handler) && native.caller_link == callback_return && self == handler.get() && handler.use_count() == 2, "native alone promotes real weak receiver");
    }) == 7 && native_calls == before + 1 && callbacks == 1 && !packets && handler.use_count() == 1,
          "native owning Ticket forwards once with caller-only packet/receiver cleanup");
    std::puts("NATIVE_GNU_REPLY_BASELINE_OK");
    std::fflush(stdout);
}
static void first_observation(unsigned context) {
    auto handler = std::make_shared<Receiver>();
    Ticket ticket(handler, context);
    cr::Scope scope(ticket.receiver, ticket.request, callback_return);
    CHECK(handler.use_count() == 1, "scope copies weak owner without pre-promoting Reception");
    cr::Receipt out;
    bool observed = false;
    ticket.reply.reset(new Packet(ticket.request.sequence));
    CHECK(scope.forward([&] { return ticket.process([&](const cr::Promotion &native, Receiver *self) {
        const cr::Receipt before = out;
        CHECK(!scope.receive_owned(promotion(native.control, callback_return + 4), self, out) && unchanged(out, before),
              "wrong native caller PC refuses unchanged output despite actual receiver owner");
        CHECK(!scope.receive_owned(promotion(0), self, out) && unchanged(out, before),
              "null native promotion refuses unchanged output");
        CHECK(!scope.receive_owned(native, static_cast<Receiver *>(nullptr), out) && unchanged(out, before),
              "null native callback alias refuses unchanged output");
        bool foreign_thread = true;
        std::thread other([&] { foreign_thread = cr::receive_current_owned(native, self, out); });
        other.join();
        CHECK(!foreign_thread && unchanged(out, before), "thread context alone cannot acquire another native invocation");
        observed = cr::receive_current_owned(native, self, out);
        CHECK(handler.use_count() == 2, "temporary SDK promotion released before callback original");
    }); }) == 7, "reply wrapper preserves native return");
    CHECK(observed && same_request(out.request, ticket.request) &&
          same_owner(out.reception, std::weak_ptr<void>(handler)) && out.reception.lock().get() == handler.get(),
          "missing selected Ticket to native-owned Reception receipt");
    CHECK(!packets && handler.use_count() == 1, "receipt retains no packet or strong receiver");
    const cr::Receipt saved = out;
    CHECK(!scope.receive_owned(promotion(control_of(handler)), handler.get(), out) && unchanged(saved, out),
          "late callback outside original invocation is refused without changing output");
    CHECK(!cr::receive_current_owned(promotion(control_of(handler)), handler.get(), out) && unchanged(saved, out),
          "current callback API declines outside native forwarding scope");
    bool renewed = false;
    ticket.state = 1;
    ticket.reply.reset(new Packet(ticket.request.sequence));
    CHECK(scope.forward([&] { return ticket.process([&](const cr::Promotion &native, Receiver *self) {
        renewed = scope.receive_owned(native, self, out);
    }); }) == 7 && !renewed && unchanged(saved, out),
          "reusing completed scope cannot renew receipt even for identical native owner/request scalars");
    handler.reset();
    CHECK(out.reception.expired(), "copied receipt cannot keep a retired Reception alive");
}
static void refusal_and_cancel() {
    auto first = std::make_shared<Receiver>();
    auto replacement = std::make_shared<Receiver>();
    Ticket ticket(first, 1);
    cr::Scope scope(ticket.receiver, ticket.request, callback_return);
    auto out = sentinel(first), before = out;
    ticket.reply.reset(new Packet(ticket.request.sequence));
    // Same Ticket/sequence/request scalars, but a new genuine weak owner.
    ticket.receiver = replacement;
    CHECK(scope.forward([&] { return ticket.process([&](const cr::Promotion &native, Receiver *self) {
        CHECK(!scope.receive_owned(native, self, out) && unchanged(out, before), "replacement owner cannot inherit old Ticket receipt");
        CHECK(!scope.receive_owned(promotion(0), self, out), "missing native promoted control witness declines");
        CHECK(!scope.receive_owned(native, first.get(), out), "foreign callback alias declines");
    }); }) == 7 && !packets, "refusal still forwards and cleans native result once");
    Ticket cancelled(first, 2);
    cr::Scope cancelled_scope(cancelled.receiver, cancelled.request, callback_return);
    cancelled.reply.reset(new Packet(cancelled.request.sequence));
    bool called = false;
    first.reset();
    CHECK(cancelled_scope.forward([&] { return cancelled.process([&](const cr::Promotion &, Receiver *) { called = true; }); }) == 7 && !called,
          "SDK weak snapshot must not revive native-cancelled receiver");
    Ticket explicit_cancel(replacement, 3);
    cr::Scope cancel_scope(explicit_cancel.receiver, explicit_cancel.request, callback_return);
    explicit_cancel.reply.reset(new Packet(explicit_cancel.request.sequence));
    explicit_cancel.state = 3;
    called = false;
    CHECK(cancel_scope.forward([&] { return explicit_cancel.process([&](const cr::Promotion &, Receiver *) { called = true; }); }) == 7 && !called && !packets,
          "native cancellation suppresses callback without changing original cleanup");
}
static void same_alias_distinct_owner() {
    Receiver receiver;
    // Live, caller-owned pointee; two real GNU control blocks intentionally
    // alias it. This is not a dangling pointer or a fabricated native owner.
    auto a = std::shared_ptr<Receiver>(&receiver, [](Receiver *) {});
    auto b = std::shared_ptr<Receiver>(&receiver, [](Receiver *) {});
    Ticket ticket(a, 0);
    cr::Scope scope(ticket.receiver, ticket.request, callback_return);
    ticket.receiver = b;
    ticket.reply.reset(new Packet(ticket.request.sequence));
    auto out = sentinel(a), before = out;
    scope.forward([&] { ticket.process([&](const cr::Promotion &native, Receiver *self) {
        CHECK(self == a.get() && !scope.receive_owned(native, self, out) && unchanged(out, before),
              "same receiver address and request cannot substitute a different GNU owner control");
    }); });
}
static void nesting_and_throw() {
    auto owner = std::make_shared<Receiver>();
    Ticket outer(owner, 0), inner(owner, 0); // identical scalar AND receiver owner
    cr::Scope outer_scope(outer.receiver, outer.request, callback_return), inner_scope(inner.receiver, inner.request, callback_return);
    outer.reply.reset(new Packet(outer.request.sequence));
    inner.reply.reset(new Packet(inner.request.sequence));
    auto out = sentinel(owner), before = out;
    bool nested = false, restored = false;
    CHECK(outer_scope.forward([&] {
        return outer.process([&](const cr::Promotion &native, Receiver *self) {
            try {
                inner_scope.forward([&] {
                    inner.process([&](const cr::Promotion &inner_native, Receiver *inner_self) {
                        CHECK(!outer_scope.receive_owned(native, self, out) && unchanged(out, before),
                              "nested invocation masks older scope even for same owner and wire scalars");
                        nested = cr::receive_current_owned(inner_native, inner_self, out);
                        CHECK(same_request(out.request, inner.request), "current callback API selects actual nested receipt");
                    });
                    cr::Scope unknown(std::weak_ptr<Receiver>(), inner.request, callback_return);
                    const cr::Receipt prior = out;
                    unknown.forward([&] {
                        CHECK(!cr::receive_current_owned(native, self, out) && unchanged(out, prior) &&
                              !outer_scope.receive_owned(native, self, out),
                              "unqualified nested invocation masks older valid receiver scope");
                    });
                    throw std::runtime_error("original nested failure");
                });
            } catch (const std::runtime_error &) {
                restored = cr::receive_current_owned(native, self, out);
            }
        });
    }) == 7 && nested && restored && !packets, "nested native result/exception restores only actual outer invocation");
    Ticket throwing(owner, 1);
    cr::Scope throwing_scope(throwing.receiver, throwing.request, callback_return);
    throwing.reply.reset(new Packet(throwing.request.sequence));
    owner->callback = [](Receiver *) { throw std::runtime_error("native callback"); };
    const unsigned calls = native_calls;
    bool caught = false;
    try { throwing_scope.forward([&] { return throwing.process([](const cr::Promotion &, Receiver *) {}); }); }
    catch (const std::runtime_error &) { caught = true; }
    before = out;
    CHECK(caught && native_calls == calls + 1 && !packets && owner.use_count() == 1 &&
          !throwing_scope.receive_owned(promotion(control_of(owner)), owner.get(), out) && unchanged(out, before),
          "native callback exception propagates once and retires scoped admission with native cleanup");
}
int main(int argc, char **argv) {
    unsigned profile = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 1;
    CHECK(profile == 1 || profile == 2, "supported fixture profile");
    callback_return = profile == 1 ? 0x10b408c : 0x1144490;
    native_baseline();
    for (unsigned i = 0; i < 4; ++i) first_observation(i);
    refusal_and_cancel();
    same_alias_distinct_owner();
    nesting_and_throw();
    CHECK(!packets && !receptions, "all native fixture owners cleaned by callers");
    std::printf("CACHE_REPLY_GNU_OWNERS_GREEN profile=%u\n", profile);
}

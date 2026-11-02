/* SPDX-License-Identifier: GPL-3.0-only */
/* Host fixture only: no EP147, native task allocation, queue, or facade calls. */
#include "aznativecommands.h"
#include "sdk_command_engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL nativecommands: %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
using azsdk_native::OwnerGate;

#ifdef AZSDK_NATIVE_COMMANDS_TESTING
namespace azsdk_native {
struct OwnerGateTestAccess {
    static void exhaust_after_one(OwnerGate &g) { g.last_generation_ = UINT64_MAX - 1; }
    static bool retiring(const OwnerGate &g) { return (g.state_.load(std::memory_order_acquire) & OwnerGate::Retiring) != 0; }
    static uint64_t thread_token() { return OwnerGate::thread_cookie(); }
    static void last_thread_token() { OwnerGate::next_thread_token_.store(UINT64_MAX - 1, std::memory_order_relaxed); }
};
}
#endif

template<class Predicate> static void until(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static uint64_t clock_now(void *context) { return *static_cast<uint64_t *>(context); }
static az_command_request request(unsigned operation = AZ_CMD_LOOP_EXIT, uint64_t id = 1) {
    az_command_request r = {};
    r.size = sizeof r;
    r.version = AZ_DATA_VERSION;
    r.operation = operation;
    r.request_id = id;
    r.expected_load_generation = 42;
    r.expected_revision = 7;
    r.deadline_ns = 10000;
    return r;
}
static scmd_target target(uint64_t commands = SCMD_ALL_COMMANDS) {
    scmd_target t = {};
    t.load_generation = 42;
    t.revision = 7;
    t.commands = commands;
    t.revision_commands = commands;
    t.valid = SCMD_TARGET_LOAD | SCMD_TARGET_REVISION;
    t.loaded = 1;
    return t;
}
static void unavailable() {
    CHECK(azsdk_native_commands_capabilities() == 0);
    azsdk_native_commands_adapter(nullptr);
    scmd_adapter a = {};
    azsdk_native_commands_adapter(&a);
    CHECK(a.context == nullptr && a.arguments && a.begin && a.dispatch && a.end);
    scmd_engine engine = {};
    CHECK(pthread_mutex_init(&engine.mu, nullptr) == 0);
    uint64_t now = 100;
    engine.clock = clock_now;
    engine.clock_context = &now;
    for (unsigned op = AZ_CMD_LOOP_IN; op <= AZ_CMD_CUE_COLOR; ++op) {
        for (unsigned flags : {0u, AZ_COMMAND_PERSIST}) {
            auto r = request(op);
            r.flags = flags;
            auto t = target(), before = t;
            scmd_completion completion;
            std::memset(&completion, 0xa5, sizeof completion);
            auto completion_before = completion;
            CHECK(a.arguments(a.context, &r, &t) == AZ_UNSUPPORTED);
            CHECK(a.begin(a.context, &r, &t) == AZ_UNSUPPORTED);
            CHECK(std::memcmp(&t, &before, sizeof t) == 0);
            CHECK(a.dispatch(a.context, &r, 1, &completion) == AZ_UNSUPPORTED);
            CHECK(std::memcmp(&completion, &completion_before, sizeof completion) == 0);
            a.end(a.context, r.deck);
            az_command_result result = {};
            result.size = sizeof result;
            scmd_detail detail = SCMD_DETAIL_NONE;
            CHECK(scmd_submit(&engine, 0, &r, &t, &a, &result, &detail) == AZ_UNSUPPORTED);
            CHECK(result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_UNSUPPORTED);
            CHECK(engine.next_token == 0 && engine.sequence == 0);
            for (unsigned d = 0; d < SCMD_DECKS; ++d) CHECK(engine.active[d] == 0);
        }
    }
    CHECK(a.arguments(nullptr, nullptr, nullptr) == AZ_UNSUPPORTED);
    CHECK(a.begin(nullptr, nullptr, nullptr) == AZ_UNSUPPORTED);
    CHECK(a.dispatch(nullptr, nullptr, 0, nullptr) == AZ_UNSUPPORTED);
    CHECK(pthread_mutex_destroy(&engine.mu) == 0);
    CHECK(azsdk_native_commands_capabilities() == 0);
}

struct Owner {
    explicit Owner(bool any = false) : thread(std::this_thread::get_id()), any_thread(any) {}
    std::thread::id thread;
    bool any_thread;
    std::atomic<unsigned> checks {0}, touches {0};
    std::atomic<bool> alive {true};
    int check_mode = 0;
    OwnerGate *gate = nullptr;
    uint64_t epoch = 0;
    scmd_target view = target(UINT64_C(1) << AZ_CMD_LOOP_EXIT);
    static bool check(void *opaque) {
        Owner &o = *static_cast<Owner *>(opaque);
        CHECK(o.alive.load(std::memory_order_acquire));
        o.checks.fetch_add(1, std::memory_order_relaxed);
        if (o.check_mode == 1) throw std::runtime_error("synthetic predicate failure");
        if (o.check_mode == 2) CHECK(o.gate->retire(o.epoch) == AZ_BUSY);
        return o.any_thread || o.thread == std::this_thread::get_id();
    }
    void touch() {
        CHECK(alive.load(std::memory_order_acquire));
        touches.fetch_add(1, std::memory_order_relaxed);
    }
};
static void lifecycle() {
    OwnerGate gate;
    Owner one, two;
    OwnerGate::Lease lease, second;
    uint64_t epoch = UINT64_C(0xa5a5a5a5);
    CHECK(gate.generation() == 0);
    CHECK(gate.enter(lease) == AZ_UNSUPPORTED);
    CHECK(lease.owner() == nullptr && lease.generation() == 0 && lease.end() == AZ_INVALID);
    CHECK(gate.publish(nullptr, Owner::check, &epoch) == AZ_INVALID && epoch == UINT64_C(0xa5a5a5a5));
    CHECK(gate.publish(&one, nullptr, &epoch) == AZ_INVALID);
    CHECK(gate.publish(&one, Owner::check, nullptr) == AZ_INVALID);
    CHECK(gate.retire(0) == AZ_INVALID);
    CHECK(gate.publish(&one, Owner::check, &epoch) == AZ_OK && epoch != 0);
    CHECK(gate.generation() == epoch);
    uint64_t unchanged = 123;
    CHECK(gate.publish(&two, Owner::check, &unchanged) == AZ_BUSY && unchanged == 123);
    CHECK(gate.enter(lease) == AZ_OK && lease.owner() == &one && lease.generation() == epoch);
    CHECK(gate.enter(lease) == AZ_BUSY && lease.owner() == &one);
    CHECK(gate.enter(second) == AZ_BUSY && second.owner() == nullptr);
    std::thread foreign([&] {
        CHECK(lease.owner() == nullptr && lease.generation() == 0);
        CHECK(lease.end() == AZ_INVALID);
    });
    foreign.join();
    CHECK(lease.owner() == &one);
    CHECK(gate.retire(epoch + 1) == AZ_STALE);
    CHECK(lease.end() == AZ_OK);
    CHECK(lease.owner() == nullptr && lease.generation() == 0 && lease.end() == AZ_INVALID);
    std::thread wrong_owner([&] {
        OwnerGate::Lease wrong;
        CHECK(gate.enter(wrong) == AZ_INVALID);
        CHECK(wrong.owner() == nullptr && wrong.generation() == 0);
    });
    wrong_owner.join();
    CHECK(gate.enter(lease) == AZ_OK); /* failed owner check released admission */
    CHECK(gate.retire(epoch) == AZ_BUSY); /* never wait on our own lease */
    CHECK(gate.enter(second) == AZ_BUSY);
    CHECK(lease.end() == AZ_OK);
    CHECK(gate.enter(second) == AZ_UNSUPPORTED); /* reentrant close stays closed */
    CHECK(gate.retire(epoch) == AZ_OK && gate.generation() == 0);
    CHECK(gate.retire(epoch) == AZ_STALE);
    uint64_t replacement = 0;
    CHECK(gate.publish(&two, Owner::check, &replacement) == AZ_OK && replacement > epoch);
    CHECK(gate.retire(epoch) == AZ_STALE);
    CHECK(gate.enter(lease) == AZ_OK && lease.owner() == &two && lease.generation() == replacement);
    CHECK(lease.end() == AZ_OK && gate.retire(replacement) == AZ_OK);
    CHECK(azsdk_native_commands_capabilities() == 0); /* a synthetic gate certifies nothing */
}
static void predicate_failures() {
    OwnerGate gate;
    Owner owner;
    owner.gate = &gate;
    uint64_t epoch = 0;
    CHECK(gate.publish(&owner, Owner::check, &epoch) == AZ_OK);
    owner.epoch = epoch;
    OwnerGate::Lease lease;
    owner.check_mode = 1;
    CHECK(gate.enter(lease) == AZ_FAILED && lease.owner() == nullptr);
    owner.check_mode = 0;
    CHECK(gate.enter(lease) == AZ_OK); /* exception also releases admission */
    CHECK(lease.end() == AZ_OK);
    owner.check_mode = 2;
    CHECK(gate.enter(lease) == AZ_STALE && lease.owner() == nullptr);
    CHECK(gate.enter(lease) == AZ_UNSUPPORTED);
    CHECK(gate.retire(epoch) == AZ_OK);
}
static void destructor_release() {
    OwnerGate gate;
    Owner owner;
    uint64_t epoch = 0;
    CHECK(gate.publish(&owner, Owner::check, &epoch) == AZ_OK);
    {
        OwnerGate::Lease lease;
        CHECK(gate.enter(lease) == AZ_OK);
    }
    CHECK(gate.retire(epoch) == AZ_OK);
    /* A quiescent gate also retires on its own destruction. */
    OwnerGate other;
    CHECK(other.publish(&owner, Owner::check, &epoch) == AZ_OK);
}
static void retirement_waits() {
    OwnerGate gate;
    Owner *owner = new Owner(true);
    uint64_t epoch = 0;
    CHECK(gate.publish(owner, Owner::check, &epoch) == AZ_OK);
    std::atomic<bool> entered {false}, touch {false}, touched {false}, release {false}, retired {false};
    std::thread reader([&] {
        OwnerGate::Lease lease;
        CHECK(gate.enter(lease) == AZ_OK);
        CHECK(lease.owner() == owner);
        entered.store(true, std::memory_order_release);
        until([&] { return touch.load(std::memory_order_acquire); });
        static_cast<Owner *>(lease.owner())->touch();
        touched.store(true, std::memory_order_release);
        until([&] { return release.load(std::memory_order_acquire); });
        CHECK(lease.end() == AZ_OK);
    });
    until([&] { return entered.load(std::memory_order_acquire); });
    std::thread retirer([&] {
        CHECK(gate.retire(epoch) == AZ_OK);
        owner->alive.store(false, std::memory_order_release);
        delete owner;
        retired.store(true, std::memory_order_release);
    });
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    until([&] { return retired.load(std::memory_order_acquire) || azsdk_native::OwnerGateTestAccess::retiring(gate); });
    CHECK(azsdk_native::OwnerGateTestAccess::retiring(gate));
#endif
    CHECK(!retired.load(std::memory_order_acquire));
    OwnerGate::Lease cannot_enter;
    CHECK(gate.enter(cannot_enter) == AZ_BUSY);
    CHECK(!cannot_enter.owner());
    touch.store(true, std::memory_order_release);
    until([&] { return touched.load(std::memory_order_acquire); });
    CHECK(!retired.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    reader.join();
    retirer.join();
    CHECK(retired.load(std::memory_order_acquire) && gate.generation() == 0);
    CHECK(gate.enter(cannot_enter) == AZ_UNSUPPORTED);
}
static void repeated_teardown() {
    OwnerGate gate;
    std::atomic<bool> stop {false};
    std::vector<std::thread> readers;
    for (unsigned n = 0; n < 4; ++n) readers.emplace_back([&] {
        while (!stop.load(std::memory_order_acquire)) {
            OwnerGate::Lease lease;
            int status = gate.enter(lease);
            if (status == AZ_OK) static_cast<Owner *>(lease.owner())->touch();
            else CHECK(status == AZ_BUSY || status == AZ_UNSUPPORTED || status == AZ_STALE);
        }
    });
    uint64_t previous = 0;
    for (unsigned n = 0; n < 200; ++n) {
        std::unique_ptr<Owner> owner(new Owner(true));
        uint64_t epoch = 0;
        CHECK(gate.publish(owner.get(), Owner::check, &epoch) == AZ_OK && epoch > previous);
        until([&] { return owner->touches.load(std::memory_order_acquire) != 0; });
        CHECK(gate.retire(epoch) == AZ_OK);
        owner->alive.store(false, std::memory_order_release);
        owner.reset();
        CHECK(gate.retire(epoch) == AZ_STALE);
        previous = epoch;
    }
    stop.store(true, std::memory_order_release);
    for (auto &reader : readers) reader.join();
}

/* This adapter is entirely synthetic. It exercises pairing with the portable
 * policy; it cannot enable the default native adapter or certify EP147. */
struct FixtureAdapter {
    OwnerGate gate;
    OwnerGate::Lease lease;
    unsigned begins = 0, dispatches = 0, ends = 0;
    az_command_request captured = {};
    static int arguments(void *, const az_command_request *r, const scmd_target *) {
        return r->operation == AZ_CMD_LOOP_EXIT && r->quantization == AZ_QUANTIZE_NATIVE && !r->flags ? AZ_OK : AZ_UNSUPPORTED;
    }
    static int begin(void *context, const az_command_request *, scmd_target *out) {
        auto &self = *static_cast<FixtureAdapter *>(context);
        int rc = self.gate.enter(self.lease);
        if (rc != AZ_OK) return rc;
        ++self.begins;
        *out = static_cast<Owner *>(self.lease.owner())->view;
        return AZ_OK;
    }
    static int dispatch(void *context, const az_command_request *r, uint64_t token, scmd_completion *out) {
        auto &self = *static_cast<FixtureAdapter *>(context);
        auto *owner = static_cast<Owner *>(self.lease.owner());
        CHECK(owner && token && owner->view.load_generation == r->expected_load_generation);
        owner->touch();
        ++self.dispatches;
        self.captured = *r;
        *out = scmd_completion {AZ_COMMAND_APPLIED, AZ_OK, owner->view.load_generation, ++owner->view.revision, 1};
        return AZ_OK;
    }
    static void end(void *context, uint32_t deck) {
        auto &self = *static_cast<FixtureAdapter *>(context);
        CHECK(deck == 0 && self.lease.owner());
        ++self.ends;
        CHECK(self.lease.end() == AZ_OK);
    }
    scmd_adapter adapter() { return {this, arguments, begin, dispatch, end}; }
};
static void engine_pairing() {
    FixtureAdapter fixture;
    Owner owner;
    uint64_t epoch = 0;
    CHECK(fixture.gate.publish(&owner, Owner::check, &epoch) == AZ_OK);
    auto adapter = fixture.adapter();
    scmd_engine engine = {};
    CHECK(pthread_mutex_init(&engine.mu, nullptr) == 0);
    uint64_t now = 100;
    engine.clock = clock_now;
    engine.clock_context = &now;
    auto r = request();
    auto before = owner.view;
    az_command_result result = {};
    result.size = sizeof result;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, nullptr) == AZ_OK);
    std::thread wrong([&] { CHECK(scmd_step(&engine, 0, &adapter) == AZ_INVALID); });
    wrong.join();
    CHECK(fixture.begins == 0 && fixture.dispatches == 0 && fixture.ends == 0);
    CHECK(scmd_poll(&engine, 0, 1, &result) == AZ_OK && result.outcome == AZ_COMMAND_ACCEPTED);
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(fixture.begins == 1 && fixture.dispatches == 1 && fixture.ends == 1);
    CHECK(scmd_poll(&engine, 0, 1, &result) == AZ_OK && result.outcome == AZ_COMMAND_APPLIED && result.revision == 8);
    CHECK(scmd_request_equal(&fixture.captured, &r));
    r.request_id = 2;
    r.expected_revision = 8;
    before = owner.view;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, nullptr) == AZ_OK);
    ++owner.view.load_generation; /* replacement between acceptance and owner-thread admission */
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(scmd_poll(&engine, 0, 2, &result) == AZ_OK && result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_STALE);
    CHECK(fixture.begins == 2 && fixture.dispatches == 1 && fixture.ends == 2);
    r.request_id = 3;
    r.expected_load_generation = owner.view.load_generation;
    before = owner.view;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, nullptr) == AZ_OK);
    CHECK(fixture.gate.retire(epoch) == AZ_OK);
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(scmd_poll(&engine, 0, 3, &result) == AZ_OK && result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_UNSUPPORTED);
    CHECK(fixture.begins == 2 && fixture.dispatches == 1 && fixture.ends == 2);
    CHECK(pthread_mutex_destroy(&engine.mu) == 0);
    CHECK(azsdk_native_commands_capabilities() == 0);
}
static void exhaustion() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    OwnerGate gate;
    Owner owner;
    azsdk_native::OwnerGateTestAccess::exhaust_after_one(gate);
    uint64_t epoch = 0;
    CHECK(gate.publish(&owner, Owner::check, &epoch) == AZ_OK && epoch == UINT64_MAX);
    CHECK(gate.retire(epoch) == AZ_OK);
    epoch = 123;
    CHECK(gate.publish(&owner, Owner::check, &epoch) == AZ_LIMIT && epoch == 123);
    CHECK(gate.generation() == 0);
    OwnerGate::Lease lease;
    CHECK(gate.enter(lease) == AZ_UNSUPPORTED);
#endif
}
static void thread_identity() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    std::vector<uint64_t> identities;
    for (unsigned n = 0; n < 128; ++n) {
        uint64_t identity = 0;
        std::thread later([&] {
            identity = azsdk_native::OwnerGateTestAccess::thread_token();
            CHECK(identity != 0 && identity == azsdk_native::OwnerGateTestAccess::thread_token());
        });
        later.join();
        identities.push_back(identity);
    }
    std::sort(identities.begin(), identities.end());
    CHECK(std::adjacent_find(identities.begin(), identities.end()) == identities.end());
#endif
}
static void thread_exhaustion() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    OwnerGate gate;
    Owner owner(true);
    uint64_t epoch = 0;
    CHECK(gate.publish(&owner, Owner::check, &epoch) == AZ_OK);
    azsdk_native::OwnerGateTestAccess::last_thread_token();
    std::thread last([&] {
        OwnerGate::Lease lease;
        CHECK(gate.enter(lease) == AZ_OK && lease.owner() == &owner);
        CHECK(azsdk_native::OwnerGateTestAccess::thread_token() == UINT64_MAX);
    });
    last.join();
    std::thread exhausted([&] {
        OwnerGate::Lease lease;
        CHECK(gate.enter(lease) == AZ_BUSY && !lease.owner());
        CHECK(azsdk_native::OwnerGateTestAccess::thread_token() == 0);
    });
    exhausted.join();
    CHECK(owner.checks.load(std::memory_order_relaxed) == 1);
    CHECK(gate.retire(epoch) == AZ_OK);
#endif
}
int main() {
    unavailable();
    lifecycle();
    predicate_failures();
    destructor_release();
    retirement_waits();
    repeated_teardown();
    engine_pairing();
    exhaustion();
    thread_identity();
    thread_exhaustion();
    std::puts("PASS: native command boundary, admission, retirement and policy pairing (synthetic)");
}

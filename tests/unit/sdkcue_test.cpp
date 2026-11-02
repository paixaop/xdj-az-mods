/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic native weak-listener ABI; real observer only when present. No cue
 * kind/edit operation, persistence, source, or track/load identity is modeled. */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(__has_include)
#if __has_include("azcueobserver.h")
#include "azcueobserver.h"
#define HAVE_CUE_HEADER 1
#endif
#endif
#ifndef HAVE_CUE_HEADER
// Proposed private scalar contract, solely so absence reaches a runtime RED.
struct azsdk_cue_sample {
    std::uint64_t unit_generation, revision, observed_ns;
    std::uint32_t changed_index_mask, reserved;
};
#endif
extern "C" {
void azsdk_cue_configure(int, int) noexcept __attribute__((weak));
int azsdk_cue_register(int, void *) noexcept __attribute__((weak));
void azsdk_cue_retire(int) noexcept __attribute__((weak));
int azsdk_cue_read(int, azsdk_cue_sample *) noexcept __attribute__((weak));
int azsdk_cue_take(int, azsdk_cue_sample *) noexcept __attribute__((weak));
unsigned azsdk_cue_capabilities() noexcept __attribute__((weak));
// Test-only controls touch actual observer atomics, never a receipt model.
int azsdk_cue_test_control(int, unsigned, std::uint64_t) noexcept __attribute__((weak));
void azsdk_cue_test_pending_hook(void (*)(int)) noexcept __attribute__((weak));
void azsdk_cue_test_collect_hook(void (*)(int)) noexcept __attribute__((weak));
}
#if !defined(__linux__)
int main() { std::puts("SKIP: cue fixture needs GNU Linux main-ELF guards"); return 77; }
#else
#define CHECK(x, why) do { if (!(x)) { std::fprintf(stderr, "FAIL cue: %s\n", why); std::abort(); } } while (0)
struct alignas(8) CueRecord { unsigned char bytes[0x100] = {}; };
static CueRecord record(unsigned index, std::uint32_t revision = 0) {
    CueRecord value;
    std::memcpy(value.bytes, &index, 4);
    std::uint32_t padding = 0x98765432;
    std::memcpy(value.bytes + 4, &padding, 4); // never an index/mode/player
    std::memcpy(value.bytes + 0x10, &revision, 4); // opaque native u32 revision
    return value;
}
class ListenerMirror {
public:
    virtual ~ListenerMirror() {}
    virtual void callback2(const CueRecord &) = 0;
    virtual void callback3(const CueRecord &, unsigned) = 0;
};
using Lease = std::shared_ptr<ListenerMirror>;
static void emit(const Lease &lease, const CueRecord &value, unsigned player, bool routed = true) {
    std::uintptr_t *table;
    std::memcpy(&table, lease.get(), sizeof table);
    using Plain = void (*)(void *, const CueRecord &);
    using Routed = void (*)(void *, const CueRecord &, unsigned);
    reinterpret_cast<Plain>(table[2])(lease.get(), value);
    if (routed) reinterpret_cast<Routed>(table[3])(lease.get(), value, player);
}
namespace dj_player {
class StateNotificationServer {
public:
    virtual ~StateNotificationServer() {}
    virtual void r2() {} virtual void r3() {} virtual void r4() {}
    virtual void r5() {} virtual void r6() {} virtual void r7() {}
    virtual void addObserver(std::weak_ptr<ListenerMirror> listener) {
        listeners.push_back(listener);
        if (on_register) on_register();
        if (fail_after) throw std::runtime_error("native registration failure");
    }
    virtual void r9() {} virtual void r10() {} virtual void r11() {} virtual void r12() {}
    virtual void r13() {} virtual void r14() {} virtual void r15() {} virtual void r16() {}
    virtual void r17() {} virtual void r18() {} virtual void r19() {} virtual void r20() {}
    virtual void r21() {} virtual void r22() {} virtual void r23() {} virtual void r24() {}
    virtual void r25() {}
    unsigned char native_prefix[0x1550 - 8] = {};
    std::uint32_t native_routing_tag = 0x93510000;
    std::vector<std::weak_ptr<ListenerMirror>> listeners;
    std::function<void()> on_register;
    bool fail_after = false;
};
class UnitStateServer {
public:
    virtual ~UnitStateServer() {}
    unsigned char padding[0x1f28 - 8] = {};
    StateNotificationServer *server;
    explicit UnitStateServer(StateNotificationServer *s) : server(s) {}
};
}
struct Unit {
    alignas(8) unsigned char bytes[0x1a8] = {};
    explicit Unit(dj_player::UnitStateServer *publisher) { std::memcpy(bytes + 0x1a0, &publisher, 8); }
};
struct Fixture {
    dj_player::StateNotificationServer server;
    dj_player::UnitStateServer publisher {&server};
    Unit unit {&publisher};
    Fixture() {
        static std::atomic<std::uint32_t> next {0x93510000};
        server.native_routing_tag = next.fetch_add(1);
        CHECK(reinterpret_cast<const char *>(&server.native_routing_tag) - reinterpret_cast<const char *>(&server) == 0x1550,
              "opaque routing scalar occupies actual channel+48/server+1550 prefix");
    }
    unsigned routing_tag() const { return server.native_routing_tag; }
    Lease lease() { CHECK(!server.listeners.empty(), "native weak registration exists"); return server.listeners.back().lock(); }
};
static void configure(int on, int build) { if (azsdk_cue_configure) azsdk_cue_configure(on, build); }
static int submit(unsigned deck, Fixture &fixture) { return azsdk_cue_register ? azsdk_cue_register(int(deck), &fixture.unit) : 0; }
static void retire(unsigned deck) { if (azsdk_cue_retire) azsdk_cue_retire(int(deck)); }
static int read(unsigned deck, azsdk_cue_sample *out, bool take = false) {
    auto fn = take ? azsdk_cue_take : azsdk_cue_read;
    return fn ? fn(int(deck), out) : 0;
}
static azsdk_cue_sample sample(unsigned deck, bool take = false) {
    struct Guarded { std::uint64_t before; azsdk_cue_sample value; std::uint64_t after; } out = {};
    out.before = UINT64_C(0x123456789abcdef0); out.after = UINT64_C(0xfedcba9876543210);
    CHECK(read(deck, &out.value, take), "missing actual native cue callback to collected observation");
    CHECK(out.before == UINT64_C(0x123456789abcdef0) && out.after == UINT64_C(0xfedcba9876543210) &&
          out.value.unit_generation && out.value.revision && out.value.observed_ns &&
          out.value.changed_index_mask && !(out.value.changed_index_mask & ~0x1ffu) && !out.value.reserved,
          "bounded coherent cue receipt and caller output guards");
    return out.value;
}
static void unchanged(unsigned deck, bool take = false) {
    azsdk_cue_sample out, before;
    std::memset(&out, 0xa5, sizeof out); before = out;
    CHECK(!read(deck, &out, take) && !std::memcmp(&out, &before, sizeof out), "unavailable/busy reader leaves caller output unchanged");
}
class Baseline final : public ListenerMirror {
public:
    unsigned plain = 0, routed = 0;
    void callback2(const CueRecord &value) override {
        unsigned index; std::memcpy(&index, value.bytes, 4); CHECK(index == 8, "typed native index prefix"); ++plain;
    }
    void callback3(const CueRecord &, unsigned player) override { CHECK(player == 3, "typed native routed scalar"); ++routed; }
};
class NativeCounter final : public ListenerMirror {
public:
    unsigned plain = 0, routed = 0;
    void callback2(const CueRecord &) override { ++plain; }
    void callback3(const CueRecord &, unsigned) override { ++routed; }
};
static void native_baseline() {
    Fixture fixture; auto listener = std::make_shared<Baseline>();
    fixture.server.addObserver(std::weak_ptr<ListenerMirror>(listener));
    auto held = fixture.lease(); emit(held, record(8, UINT32_MAX), 3);
    CHECK(listener->plain == 1 && listener->routed == 1 && held.use_count() == 2, "genuine native weak promotion and typed callback baseline");
    std::puts("NATIVE_CUE_CALLBACK_BASELINE_OK weak16=16 index=8 player=3 property_revision_u32"); std::fflush(stdout);
}
static void basic(unsigned build) {
    configure(1, int(build)); Fixture contexts[4]; Lease leases[4];
    for (unsigned d = 0; d != 4; ++d) {
        auto original = std::make_shared<NativeCounter>();
        contexts[d].server.addObserver(std::weak_ptr<ListenerMirror>(original));
        const auto baseline_count = contexts[d].server.listeners.size();
        (void)submit(d, contexts[d]);
        // The missing implementation adds no listener/model. Native typed
        // work has already run; the intended RED is the missing receipt below.
        auto held = contexts[d].server.listeners.size() > baseline_count ? contexts[d].lease() : Lease();
        leases[d] = held;
        emit(original, record(d), contexts[d].routing_tag());
        if (held) emit(held, record(d), contexts[d].routing_tag());
        CHECK(original->plain == 1 && original->routed == 1, "same unit native typed callbacks still execute before observation");
        std::printf("NATIVE_CUE_UNIT_FORWARD_OK build=%u deck=%u\n", build, d); std::fflush(stdout);
        auto value = sample(d, true);
        CHECK(value.changed_index_mask == (1u << d), "each logical deck receives only its own native cue index");
        auto latest = sample(d);
        CHECK(!std::memcmp(&value, &latest, sizeof value), "read returns last GUI-collected receipt");
        unchanged(d, true);
        emit(held, record(8), contexts[d].routing_tag(), false); unchanged(d, true); // callback2 must not double count
        emit(held, record(9), contexts[d].routing_tag()); emit(held, record(UINT32_MAX), contexts[d].routing_tag()); unchanged(d, true);
        emit(held, record(8), contexts[d].routing_tag() ^ 1u); unchanged(d, true);
        emit(held, record(0), contexts[d].routing_tag()); emit(held, record(8, UINT32_MAX), contexts[d].routing_tag()); emit(held, record(0, 0), contexts[d].routing_tag());
        auto batch = sample(d, true);
        CHECK(batch.changed_index_mask == 0x101 && batch.unit_generation == value.unit_generation &&
              batch.revision > value.revision && batch.observed_ns >= value.observed_ns,
              "bounded index mask coalesces pending changes without interpreting native revision wrap");
    }
    CHECK(azsdk_cue_capabilities && azsdk_cue_capabilities() == 15, "submitted mask contains all four units");
    auto old = contexts[0].lease(); auto previous = sample(0);
    retire(0); unchanged(0); emit(old, record(6), contexts[0].routing_tag()); unchanged(0, true);
    CHECK(submit(0, contexts[0]), "replacement listener submitted");
    auto fresh = contexts[0].lease(); emit(old, record(5), contexts[0].routing_tag()); unchanged(0, true);
    emit(fresh, record(2), contexts[0].routing_tag()); auto next = sample(0, true);
    CHECK(next.unit_generation != previous.unit_generation && next.changed_index_mask == 4,
          "old held callback cannot publish into replacement generation");
    configure(0, int(build));
    leases[0] = fresh;
    for (unsigned d = 0; d != 4; ++d) { unchanged(d); emit(leases[d], record(1), contexts[d].routing_tag()); unchanged(d, true); }
    CHECK(!azsdk_cue_capabilities(), "disable retires pending and collected publication");
}
static std::atomic<bool> entered {false}, proceed {false};
static Lease collection_emitter;
static unsigned collection_route;
static void during_collection(int) { emit(collection_emitter, record(5), collection_route); }
static void pending_barrier(int) {
    entered.store(true, std::memory_order_release);
    while (!proceed.load(std::memory_order_acquire)) std::this_thread::yield();
}
static void registration_reentry(unsigned build) {
    for (bool throwing : {false, true}) {
        configure(1, int(build)); Fixture outer, replacement; Lease fresh;
        outer.server.fail_after = throwing;
        outer.server.on_register = [&] {
            retire(0);
            CHECK(submit(0, replacement), "native registration can reenter retire/register without SDK lock");
            fresh = replacement.lease(); emit(fresh, record(4), replacement.routing_tag());
        };
        CHECK(!submit(0, outer) && fresh, "old register does not acknowledge its reentrant replacement");
        CHECK(sample(0, true).changed_index_mask == 16 && azsdk_cue_capabilities() == 1,
              "old normal/throwing completion cannot invalidate replacement slot or pending callbacks");
        retire(0);
    }
    configure(1, int(build)); Fixture disabled; Lease escaped;
    disabled.server.on_register = [&] { escaped = disabled.lease(); configure(0, int(build)); };
    CHECK(!submit(0, disabled) && escaped, "native registration can reenter disable without SDK lock");
    emit(escaped, record(6), disabled.routing_tag()); unchanged(0, true);
    CHECK(!azsdk_cue_capabilities(), "old registration finish cannot reenable disabled generation");
}
static void races_and_failures(unsigned build) {
    configure(1, int(build)); Fixture fixture;
    CHECK(submit(0, fixture), "race listener submitted"); auto held = fixture.lease();
    CHECK(azsdk_cue_test_control && azsdk_cue_test_pending_hook && azsdk_cue_test_collect_hook, "test controls are linked only in dedicated fixture");
    CHECK(azsdk_cue_test_control(0, 1, 1), "claim real GUI collector busy gate");
    emit(held, record(7), fixture.routing_tag()); unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 1, 0), "release real GUI collector gate");
    CHECK(sample(0, true).changed_index_mask == 128, "busy GUI take cannot lose a pending native callback");
    CHECK(azsdk_cue_test_control(0, 2, 1), "force actual clock failure path");
    emit(held, record(6), fixture.routing_tag()); unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 2, 0), "restore collection clock");
    CHECK(sample(0, true).changed_index_mask == 64, "clock failure preserves pending callback");
    std::thread workers[4];
    for (unsigned w = 0; w != 4; ++w) workers[w] = std::thread([&, w] { for (unsigned n = 0; n != 1000; ++n) emit(held, record((w + n) % 9), fixture.routing_tag()); });
    for (auto &worker : workers) worker.join();
    CHECK(sample(0, true).changed_index_mask == 0x1ff, "contending callbacks never drop pending cue indices");
    collection_emitter = held; collection_route = fixture.routing_tag(); emit(held, record(4), fixture.routing_tag());
    azsdk_cue_test_collect_hook(during_collection); // only after the real atomic drain
    CHECK(sample(0, true).changed_index_mask == 16, "collector returns exactly its drained batch");
    azsdk_cue_test_collect_hook(nullptr); collection_emitter.reset();
    CHECK(sample(0, true).changed_index_mask == 32, "callback during collection remains pending for next GUI tick");
    entered.store(false); proceed.store(false); azsdk_cue_test_pending_hook(pending_barrier);
    std::thread old([&] { emit(held, record(8), fixture.routing_tag()); });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    retire(0); CHECK(submit(0, fixture), "new listener while old callback lease is paused");
    auto fresh = fixture.lease(); proceed.store(true, std::memory_order_release); old.join();
    azsdk_cue_test_pending_hook(nullptr); unchanged(0, true);
    emit(fresh, record(3), fixture.routing_tag()); CHECK(sample(0, true).changed_index_mask == 8, "stale in-flight OR cannot reach replacement slot");
    retire(0); fixture.server.fail_after = true; Lease escaped;
    fixture.server.on_register = [&] { escaped = fixture.lease(); };
    CHECK(!submit(0, fixture) && escaped, "failed native registration keeps only its escaped callback lease");
    emit(escaped, record(4), fixture.routing_tag()); unchanged(0, true);
    fixture.server.fail_after = false; fixture.server.on_register = {};
    CHECK(submit(0, fixture), "register after native exception"); fresh = fixture.lease();
    emit(fresh, record(1), fixture.routing_tag()); (void)sample(0, true);
    CHECK(azsdk_cue_test_control(0, 3, UINT64_MAX), "seed actual collection revision exhaustion");
    emit(fresh, record(2), fixture.routing_tag()); unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 4, 0) == 4, "revision exhaustion leaves pending mask intact");
    Fixture another;
    CHECK(azsdk_cue_test_control(0, 5, (UINT64_C(1) << 55) - 1), "seed actual packed-generation upper bound");
    CHECK(!submit(1, another) && another.server.listeners.empty(), "generation never wraps or shifts away high bits into a reused identity");
    configure(0, int(build));
}
static void capacity(unsigned build) {
    configure(1, int(build)); Fixture fixture; unsigned accepted = 0;
    for (unsigned n = 0; n != 256; ++n) {
        retire(0); if (!submit(0, fixture)) break; ++accepted;
    }
    CHECK(accepted && accepted < 256 && !submit(0, fixture), "bounded generation slots refuse exhaustion without reuse");
    configure(0, int(build));
}
int main(int argc, char **argv) {
    CHECK(argc >= 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "separate profile process");
    const unsigned build = unsigned(argv[1][0] - '0');
    static_assert(sizeof(std::weak_ptr<ListenerMirror>) == 16 && sizeof(CueRecord) == 0x100, "native fixture widths");
    native_baseline(); basic(build);
    if (argc == 3 && !std::strcmp(argv[2], "capacity")) capacity(build);
    else { registration_reentry(build); races_and_failures(build); }
    std::puts("PASS cue: actual weak callbacks, per-unit pending/collected receipts, generation retirement and bounded draining");
}
#endif

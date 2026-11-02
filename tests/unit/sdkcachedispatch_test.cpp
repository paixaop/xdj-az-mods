/* SPDX-License-Identifier: GPL-3.0-only */
/* Real cache/source helpers and real generic C dispatcher. The explicit frame
 * backend below replaces only the assembly register bridge, not retirement or
 * source state. It is NOT a native prototype or an installed firmware hook. */
#define main sdkcachebirth_baseline_main
#include "sdkcachebirth_test.cpp"
#undef main

#if !defined(__linux__)
int main() { return sdkcachebirth_baseline_main(); }
#else
#include "../../src/common/azcacheentry.h"

namespace {
struct RawFactoryForward {
    const azsdk_cache_birth::FactoryBorrow &borrow;
    azsdk_cache_birth::Forward original;
    unsigned forwarded = 0;
    RawFactoryForward(const azsdk_cache_birth::FactoryBorrow &b, azsdk_cache_birth::Forward f)
        : borrow(b), original(f) {}
};
void raw_factory_original(azce_call *call) {
    auto &forward = *reinterpret_cast<RawFactoryForward *>(call->args[7]);
    REQUIRE(call->args[0] == reinterpret_cast<std::uintptr_t>(forward.borrow.receiver_creator) &&
            call->args[1] == reinterpret_cast<std::uintptr_t>(forward.borrow.borrowed_pool),
            "native factory receiver/pool registers unchanged");
    ++forward.forwarded;
    forward.original.call(forward.original.context);
    call->result[0] = reinterpret_cast<std::uintptr_t>(*forward.borrow.native_result);
}
void dispatch_native_factory(const azsdk_cache_birth::FactoryBorrow &borrow,
                             azsdk_cache_birth::Forward original) {
    RawFactoryForward forward(borrow, original);
    azce_call call = {};
    call.kind = borrow.kind == azsdk_cache_birth::PrimaryFactory ? AZCE_KIND_PRIMARY_FACTORY :
                                                                 AZCE_KIND_STANDARD_FACTORY;
    call.args[0] = reinterpret_cast<std::uintptr_t>(borrow.receiver_creator);
    call.args[1] = reinterpret_cast<std::uintptr_t>(borrow.borrowed_pool);
    call.args[7] = reinterpret_cast<std::uintptr_t>(&forward);
    call.original = reinterpret_cast<std::uintptr_t>(raw_factory_original);
    try { azsdk_cache_dispatch_call(&call); }
    catch (...) { REQUIRE(forward.forwarded == 1, "throwing native factory forwards once"); throw; }
    REQUIRE(forward.forwarded == 1 && call.result[0] == reinterpret_cast<std::uintptr_t>(*borrow.native_result),
            "production factory adapter preserves actual raw result and calls original once");
}
struct DispatchFrame {
    azce_call call = {};
    azce_call input = {};
    struct Output {
        std::uint64_t before = UINT64_C(0x01abcdef23456789);
        std::uint64_t value = 0;
        std::uint64_t after = UINT64_C(0xfedcba9876543210);
    } output;
    unsigned calls = 0;
    std::function<void()> action;

    explicit DispatchFrame(unsigned kind, std::function<void()> work = [] {}) : action(work) {
        call.kind = kind;
        call.reserved = 0x9a1b2c3d;
        call.caller_link = UINT64_C(0x123456789abcdef0);
        call.original = reinterpret_cast<std::uintptr_t>(&original);
        for (unsigned n = 0; n != 9; ++n) call.args[n] = UINT64_C(0x73600000) + n;
        call.args[7] = reinterpret_cast<std::uintptr_t>(this);
        call.args[8] = reinterpret_cast<std::uintptr_t>(&output.value);
        for (unsigned n = 0; n != 8; ++n) {
            call.vector_args[n].words[0] = UINT64_C(0x100000000) + n;
            call.vector_args[n].words[1] = UINT64_C(0x200000000) + n;
        }
        input = call;
    }
    void check_input() const {
        REQUIRE(std::memcmp(call.args, input.args, sizeof call.args) == 0 &&
                std::memcmp(call.vector_args, input.vector_args, sizeof call.vector_args) == 0 &&
                call.original == input.original && call.kind == input.kind &&
                call.reserved == input.reserved && call.caller_link == input.caller_link,
                "dispatcher preserves original frame arguments and forwarding identity");
    }
    static void original(azce_call *call) {
        auto &self = *reinterpret_cast<DispatchFrame *>(call->args[7]);
        REQUIRE(call == &self.call, "dispatcher forwards the exact caller-owned register frame");
        self.check_input(); ++self.calls;
        call->result[0] = UINT64_C(0x836acced01234567);
        call->result[1] = UINT64_C(0x76543210fedcba98);
        for (unsigned n = 0; n != 4; ++n) {
            call->vector_results[n].words[0] = UINT64_C(0x300000000) + n;
            call->vector_results[n].words[1] = UINT64_C(0x400000000) + n;
        }
        *reinterpret_cast<std::uint64_t *>(call->args[8]) = UINT64_C(0x1298abcdef765430);
        std::printf("NATIVE_DISPATCH_FORWARD_OK build=%u kind=%u\n", build_id, call->kind);
        std::fflush(stdout);
        self.action();
    }
    void check_return() const {
        check_input();
        REQUIRE(calls == 1 && output.before == UINT64_C(0x01abcdef23456789) &&
                output.value == UINT64_C(0x1298abcdef765430) &&
                output.after == UINT64_C(0xfedcba9876543210) &&
                call.result[0] == UINT64_C(0x836acced01234567) &&
                call.result[1] == UINT64_C(0x76543210fedcba98),
                "original forwards exactly once with unchanged indirect result and integer results");
        for (unsigned n = 0; n != 4; ++n)
            REQUIRE(call.vector_results[n].words[0] == UINT64_C(0x300000000) + n &&
                    call.vector_results[n].words[1] == UINT64_C(0x400000000) + n,
                    "dispatcher preserves original SIMD result frame");
    }
};

void forwarding_only() {
    reset(); World world; auto born = birth(world.a); observed(world.a, born, query(world.a));
    const auto generation = api().counters().generation;
    const unsigned kinds[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE, AZCE_KIND_QUERY,
                              AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO,
                              AZCE_KIND_PRIMARY_FACTORY, AZCE_KIND_STANDARD_FACTORY,
                              AZCE_KIND_COUNT, UINT32_MAX};
    for (unsigned kind : kinds) {
        DispatchFrame frame(kind, [&] {
            observed(world.a, born, query(world.a));
            REQUIRE(!api().counters().active && api().counters().generation == generation,
                    "unqualified routes remain forwarding-only without guessed native borrows");
        });
        azsdk_cache_dispatch_call(&frame.call); frame.check_return();
    }
    std::printf("DISPATCH_FORWARD_BASELINE_OK build=%u\n", build_id); std::fflush(stdout);
}

void mutation(unsigned kind, bool throwing) {
    reset(); World world;
    for (auto context : {&world.a, &world.b, &world.c, &world.d}) {
        auto born = birth(*context); observed(*context, born, query(*context));
    }
    const auto generation = api().counters().generation;
    DispatchFrame frame(kind, [&] {
        // On old production this is the first failure, AFTER genuine native
        // creation/query and register-frame original forwarding all succeeded.
        auto q = query(world.a);
        REQUIRE(q.status != AZS_OK && !q.witness.birth_epoch,
                "missing mutation retirement before native original");
        REQUIRE(api().counters().active == 1 && api().counters().generation == generation + 1,
                "real mutation fence is active before the original callback");
        for (auto context : {&world.b, &world.c, &world.d}) unavailable(query(*context));
        if (throwing) throw std::runtime_error("native dispatcher mutation exception");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&frame.call); }
    catch (const std::runtime_error &e) {
        caught = std::strcmp(e.what(), "native dispatcher mutation exception") == 0;
    }
    REQUIRE(caught == throwing, "dispatcher preserves the exact native exception");
    frame.check_return();
    REQUIRE(!api().counters().active && api().counters().generation == generation + 1,
            "mutation retirement unwinds its real active depth on return or exception");
    for (auto context : {&world.a, &world.b, &world.c, &world.d}) unavailable(query(*context));
}

void nested_mutation(bool throwing) {
    reset(); World world; auto born = birth(world.a); observed(world.a, born, query(world.a));
    const auto generation = api().counters().generation;
    DispatchFrame inner(AZCE_KIND_COLLECTOR_DELETE, [&] {
        REQUIRE(api().counters().active == 2 && api().counters().generation == generation + 2,
                "nested original observes two real retirement scopes");
        unavailable(query(world.a));
        if (throwing) throw std::runtime_error("nested native dispatcher exception");
    });
    DispatchFrame outer(AZCE_KIND_SERVER_RESET, [&] {
        REQUIRE(api().counters().active == 1, "outer mutation is active before nested dispatch");
        try { azsdk_cache_dispatch_call(&inner.call); }
        catch (...) {
            REQUIRE(api().counters().active == 1, "inner exception restores outer retirement scope");
            throw;
        }
        REQUIRE(api().counters().active == 1, "inner return restores outer retirement scope");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&outer.call); }
    catch (const std::runtime_error &e) {
        caught = std::strcmp(e.what(), "nested native dispatcher exception") == 0;
    }
    REQUIRE(caught == throwing, "nested native exception crosses both dispatch frames unchanged");
    inner.check_return(); outer.check_return();
    REQUIRE(!api().counters().active && api().counters().generation == generation + 2,
            "all nested real retirement scopes finish without resetting generation");
    unavailable(query(world.a));
}
void selection_publication(unsigned kind, bool throwing) {
    reset(); World world;
    auto a = birth(world.a), b = birth(world.b), c = birth(world.c), d = birth(world.d);
    const auto generation = api().counters().generation;
    DispatchFrame frame(kind, [&] {
        unavailable(query(world.a));
        REQUIRE(!api().counters().active && api().counters().generation == generation,
                "selection storage fencing does not retire cache layout provenance");
        if (throwing) throw std::runtime_error("native selected store exception");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&frame.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native selected store exception") == 0; }
    REQUIRE(caught == throwing, "selection store preserves native exceptions");
    frame.check_return();
    observed(world.a, a, query(world.a)); observed(world.b, b, query(world.b));
    observed(world.c, c, query(world.c)); observed(world.d, d, query(world.d));
}
void selection_releases_storage() {
    reset(); World world; auto born = birth(world.a);
    FixtureAdapter adapter(born.cache.get()); Guarded<NativeRows> rows; Witness witness;
    QueryCall call {rows.bytes, cb_native_query, &adapter, 0, 1, &world.a.list.mount};
    inside_query = [&] {
        DispatchFrame store(AZCE_KIND_SELECTION_CLEAR, [&] {
            std::vector<std::vector<CacheOwner>> empty;
            world.a.list.clients.swap(empty); // destroys the actual borrowed holder storage
        });
        azsdk_cache_dispatch_call(&store.call); store.check_return();
    };
    const auto status = api().query(world.a.scope(), call, &witness);
    inside_query = {}; rows.check(); rows.value().~NativeRows();
    REQUIRE(status != AZS_OK && !witness.birth_epoch,
            "selection fence rejects before rereading destroyed vector holder storage");
    world.a.list.clients.assign(4, std::vector<CacheOwner>(9)); world.a.selected() = born.cache;
    observed(world.a, born, query(world.a));
}
} // namespace

extern "C" void azsdk_cache_invoke_original(azce_call *call) {
    REQUIRE(call && call->original, "fixture backend requires a real caller-owned original frame");
    reinterpret_cast<void (*)(azce_call *)>(call->original)(call);
}
// Link the real C dispatcher table, but never pretend these stubs are native
// assembly entries. The existing entry suite tests the actual AAPCS64 bridge.
#define UNUSED_ENTRY(n) extern "C" void azsdk_cache_entry_##n() { std::abort(); }
UNUSED_ENTRY(0) UNUSED_ENTRY(1) UNUSED_ENTRY(2) UNUSED_ENTRY(3) UNUSED_ENTRY(4) UNUSED_ENTRY(5)
UNUSED_ENTRY(6) UNUSED_ENTRY(7) UNUSED_ENTRY(8) UNUSED_ENTRY(9) UNUSED_ENTRY(10)
UNUSED_ENTRY(11) UNUSED_ENTRY(12) UNUSED_ENTRY(13) UNUSED_ENTRY(14) UNUSED_ENTRY(15)
UNUSED_ENTRY(16) UNUSED_ENTRY(17) UNUSED_ENTRY(18) UNUSED_ENTRY(19) UNUSED_ENTRY(20)
#undef UNUSED_ENTRY

#ifndef SDK_CACHE_DISPATCH_MAIN
#define SDK_CACHE_DISPATCH_MAIN main
#endif
int SDK_CACHE_DISPATCH_MAIN(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0),
            "separate process per supported build fixture");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK && api().version == 1,
            "actual source/cache helpers configured without physical policy");
    cache_factory_fixture::factory_entry = dispatch_native_factory;
    first_birth(); forwarding_only();
    // Both native-shaped factory bodies now enter the production register
    // adapter inside the real owned-source birth helper, including its actual
    // final-write receipt. No test factory/source acceptance model is linked.
    factory_write_receipts(); factory_cleanup_mutation(); native_exceptions();
    for (unsigned kind : {AZCE_KIND_SELECTION_STORE, AZCE_KIND_SELECTION_CLEAR}) {
        selection_publication(kind, false); selection_publication(kind, true);
    }
    selection_releases_storage();
    for (unsigned kind : {AZCE_KIND_SETUP, AZCE_KIND_LAYOUT, AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW,
                         AZCE_KIND_COLLECTOR_DISPOSE,
                         AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY, AZCE_KIND_LOADINFO_DESTROY,
                         AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE}) {
        mutation(kind, false); mutation(kind, true);
    }
    nested_mutation(false); nested_mutation(true);
    REQUIRE(!ListCache::alive && !ListCacheCollector::alive && !FixturePool::alive && !NativeRow::alive,
            "dispatcher retains no native owners or rows");
    std::printf("PASS cache-dispatch build=%u: real retirement before originals; results, exceptions, nested scopes and forwarding-only routes\n", build_id);
    return 0;
}
#endif

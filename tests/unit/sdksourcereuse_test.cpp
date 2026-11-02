/* SPDX-License-Identifier: GPL-3.0-only */
/* Deterministic scheduling around the actual source-key lookup. The runner
 * inserts callbacks only into a temporary copy of the real Source.cpp; neither
 * callback supplies a receipt, changes lookup decisions or models a registry. */
#include <chrono>
namespace source_reuse_fixture { void lookup_boundary(unsigned); }
#define main sdkcachebirth_baseline_main
#include "sdkcachebirth_test.cpp"
#undef main

#define REUSE_REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
    "FAIL source-reuse: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
    std::abort(); } } while (0)

#if defined(__linux__)
namespace source_reuse_fixture {
std::function<void(unsigned)> boundary;
void lookup_boundary(unsigned phase) { if (boundary) boundary(phase); }
template<class P> void wait_until(P predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        REUSE_REQUIRE(std::chrono::steady_clock::now() < deadline,
                      "actual close scheduling reached its bounded synchronization deadline");
        std::this_thread::yield();
    }
}
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0),
            "separate process per build profile");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK,
            "real existing source registry starts without physical policy");
    REQUIRE(api().version == 1, "existing cache observer test contract");
    first_birth(); // Real native creation/query, GNU owners and exact ARM X8 baseline.
    reset();
    {
        World world; // Four independent genuine native-shaped owner contexts.
        auto known = birth(world.b);
        observed(world.b, known, query(world.b));
        const auto previous_epoch = world.b.source.owner_epoch;
        const auto previous_revision = world.b.source.revision;
        const auto previous_next_epoch = source_state().next_epoch;
        const auto mount_count = world.b.list.mount.use_count();
        const auto db_count = world.b.mount().database.use_count();
        azsdk_connection_epoch::Receipt before {}, after {};
        REUSE_REQUIRE(source_state().connection_epochs.read(&world.b.connection, before) && before.generation,
                      "B has a genuine observed connection before the lookup window");
        std::atomic<bool> active {false}, finish {false};
        unsigned boundaries[2] = {}, closes = 0;
        int result = -1, native_errno = -1;
        std::thread closer;
        source_reuse_fixture::boundary = [&](unsigned phase) {
            REUSE_REQUIRE(phase < 2 && !boundaries[phase]++, "one scheduling boundary at each real lookup edge");
            if (!phase) {
                closer = std::thread([&] {
                    result = source_state().connection_epochs.forward_close(&world.a.connection, [&] {
                        ++closes;
                        active.store(true, std::memory_order_release);
                        source_reuse_fixture::wait_until([&] { return finish.load(std::memory_order_acquire); });
                        errno = EBUSY;
                        return 5; // Actual BUSY close result still retires only A.
                    });
                    native_errno = errno;
                });
                source_reuse_fixture::wait_until([&] { return active.load(std::memory_order_acquire); });
                azsdk_connection_epoch::Receipt untouched {UINT64_C(0xabcdef)};
                REUSE_REQUIRE(!source_state().connection_epochs.read(&world.b.connection, untouched) &&
                              untouched.generation == UINT64_C(0xabcdef),
                              "actual globally active close makes B's scalar receipt temporarily unavailable");
            } else {
                finish.store(true, std::memory_order_release);
                REUSE_REQUIRE(closer.joinable(), "the genuine close is running during the matching-key loop");
                closer.join();
                REUSE_REQUIRE(source_state().connection_epochs.read(&world.b.connection, after) &&
                              after.generation == before.generation,
                              "the same real B incarnation is readable before final source validation");
            }
        };
        world.b.capture();
        source_reuse_fixture::boundary = {};
        REUSE_REQUIRE(boundaries[0] == 1 && boundaries[1] == 1 && !closer.joinable() &&
                      closes == 1 && result == 5 && native_errno == EBUSY,
                      "one genuine close completes with unchanged result and errno before source publication");
        std::printf("SOURCE_REUSE_LOOKUP_OK build=%u native_close=%u result=%d generation=%llu owner16=%zu rows24=%zu\n",
                    build_id, closes, result, static_cast<unsigned long long>(after.generation),
                    sizeof(CacheOwner), sizeof(NativeRows));
        std::fflush(stdout);
        REUSE_REQUIRE(world.b.source.owner_epoch == previous_epoch,
                      "lookup-only unrelated close must reuse the exact existing source epoch");
        REUSE_REQUIRE(source_state().next_epoch == previous_next_epoch && world.b.source.revision > previous_revision,
                      "same-key publication advances only its receipt revision, not source identity");
        REUSE_REQUIRE(world.b.list.mount.use_count() == mount_count &&
                      world.b.mount().database.use_count() == db_count,
                      "fresh source validation releases its temporary real owner pins");
        observed(world.b, known, query(world.b));
        azsdk_connection_epoch::Receipt retired {UINT64_C(0xabcdef)};
        REUSE_REQUIRE(!source_state().connection_epochs.read(&world.a.connection, retired) &&
                      retired.generation == UINT64_C(0xabcdef),
                      "the actual A close remains retired instead of restoring it to preserve B");
        for (auto other : {&world.c, &world.d}) {
            azsdk_source_sample sample {};
            REUSE_REQUIRE(azsdk_sources_read(other->source.owner_epoch, &sample, sizeof sample) == AZS_OK &&
                          sample.owner_epoch == other->source.owner_epoch,
                          "other independent source contexts retain their exact identities");
        }
    }
    REUSE_REQUIRE(!ListCache::alive && !ListCacheCollector::alive && !FixturePool::alive && !NativeRow::alive,
                  "only weak/scalar records survive native fixture owners");
    std::printf("PASS source-reuse build=%u: real close lookup window preserves source and cache identity\n", build_id);
    return 0;
}
#else
int main() { std::puts("SKIP: requires Linux ELF and GNU shared ownership"); return 77; }
#endif

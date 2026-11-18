/* SPDX-License-Identifier: GPL-3.0-only */
/* Deterministic scheduling at the actual source lookup, without a registry
 * model or replacement receipt. The runner modifies only a temporary copy. */
static void source_reuse_lookup_boundary(unsigned);
#define main sdkcachebirth_baseline_main
#include "sdkcachebirth_test.c"
#undef main
#include "../../src/common/aztimestamp.h"
#define REUSE_REQUIRE(x, why)                                                                    \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            fprintf(stderr, "FAIL source-reuse: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                             \
        }                                                                                        \
    } while (0)
struct SourceReuse {
    World *world;
    _Atomic(bool) active, finish;
    pthread_t closer;
    bool joinable;
    unsigned boundaries[2], closes;
    int result, native_errno;
    struct az_connection_receipt before, after;
};
static struct SourceReuse *reuse_current;
static uint64_t reuse_clock(void) {
    struct timespec ts;
    REUSE_REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &ts), "monotonic race clock");
    return (uint64_t) ts.tv_sec * 1000000000 + (uint64_t) ts.tv_nsec;
}
static void wait_flag(_Atomic(bool) *flag) {
    uint64_t deadline = reuse_clock() + 5000000000;
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        int order = compare_timestamps(reuse_clock(), deadline, 1000000000, 1000000000, 1);
        REUSE_REQUIRE(order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE, "actual close scheduling reached its bounded synchronization deadline");
        sched_yield();
    }
}
static int reuse_close_original(void *v) {
    struct SourceReuse *r = v;
    ++r->closes;
    atomic_store_explicit(&r->active, true, memory_order_release);
    wait_flag(&r->finish);
    errno = EBUSY;
    return 5;
}
static void *reuse_close(void *v) {
    struct SourceReuse *r = v;
    r->result = az_connection_forward_close(&source_state().connection_epochs, &r->world->a.connection, reuse_close_original, r);
    r->native_errno = errno;
    return NULL;
}
static void source_reuse_lookup_boundary(unsigned phase) {
    struct SourceReuse *r = reuse_current;
    if (!r) return;
    REUSE_REQUIRE(phase < 2 && !r->boundaries[phase]++, "one scheduling boundary at each real lookup edge");
    if (!phase) {
        REUSE_REQUIRE(!pthread_create(&r->closer, NULL, reuse_close, r), "start native close");
        r->joinable = true;
        wait_flag(&r->active);
        struct az_connection_receipt untouched = {UINT64_C(0xabcdef)};
        REUSE_REQUIRE(!az_connection_read(&source_state().connection_epochs, &r->world->b.connection, &untouched) && untouched.generation == UINT64_C(0xabcdef), "actual globally active close makes B's scalar receipt temporarily unavailable");
    } else {
        atomic_store_explicit(&r->finish, true, memory_order_release);
        REUSE_REQUIRE(r->joinable && !pthread_join(r->closer, NULL), "the genuine close is running during the matching-key loop");
        r->joinable = false;
        REUSE_REQUIRE(az_connection_read(&source_state().connection_epochs, &r->world->b.connection, &r->after) && r->after.generation == r->before.generation, "the same real B incarnation is readable before final source validation");
    }
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate process per build profile");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "real existing source registry starts without physical policy");
    REQUIRE(api()->version == 1, "existing cache observer test contract");
    first_birth();
    reset();
    {
        WORLD(world);
        Born known BORN = birth(&world.b);
        observe_query(&world.b, &known);
        uint64_t previous_epoch = world.b.source.owner_epoch, previous_revision = world.b.source.revision, previous_next_epoch = source_state().next_epoch;
        unsigned mount_count = owner_count(world.b.list.mount), db_count = owner_count(context_mount(&world.b)->database);
        struct SourceReuse r = {.world = &world, .result = -1, .native_errno = -1};
        REUSE_REQUIRE(az_connection_read(&source_state().connection_epochs, &world.b.connection, &r.before) && r.before.generation, "B has a genuine observed connection before the lookup window");
        reuse_current = &r;
        context_capture(&world.b);
        reuse_current = NULL;
        REUSE_REQUIRE(r.boundaries[0] == 1 && r.boundaries[1] == 1 && !r.joinable && r.closes == 1 && r.result == 5 && r.native_errno == EBUSY, "one genuine close completes with unchanged result and errno before source publication");
        printf("SOURCE_REUSE_LOOKUP_OK build=%u native_close=%u result=%d generation=%llu owner16=%zu rows24=%zu\n", build_id, r.closes, r.result, (unsigned long long) r.after.generation, sizeof(CacheOwner), sizeof(NativeRows));
        fflush(stdout);
        REUSE_REQUIRE(world.b.source.owner_epoch == previous_epoch, "lookup-only unrelated close must reuse the exact existing source epoch");
        REUSE_REQUIRE(source_state().next_epoch == previous_next_epoch && world.b.source.revision > previous_revision, "same-key publication advances only its receipt revision, not source identity");
        REUSE_REQUIRE(owner_count(world.b.list.mount) == mount_count && owner_count(context_mount(&world.b)->database) == db_count, "fresh source validation releases its temporary real owner pins");
        observe_query(&world.b, &known);
        struct az_connection_receipt retired = {UINT64_C(0xabcdef)};
        REUSE_REQUIRE(!az_connection_read(&source_state().connection_epochs, &world.a.connection, &retired) && retired.generation == UINT64_C(0xabcdef), "the actual A close remains retired instead of restoring it to preserve B");
        Context *others[] = {&world.c, &world.d};
        for (size_t i = 0; i < 2; ++i) {
            struct azsdk_source_sample sample = {0};
            REUSE_REQUIRE(azsdk_sources_read(others[i]->source.owner_epoch, &sample, sizeof sample) == AZS_OK && sample.owner_epoch == others[i]->source.owner_epoch, "other independent source contexts retain their exact identities");
        }
    }
    REUSE_REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "only weak/scalar records survive native fixture owners");
    printf("PASS source-reuse build=%u: real close lookup window preserves source and cache identity\n", build_id);
    return 0;
}

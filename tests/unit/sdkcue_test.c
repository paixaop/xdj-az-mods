/* SPDX-License-Identifier: GPL-3.0-only */
/* Native weak-listener ABI exercised entirely from C. */
#include "cue_native_fixture.h"
#include "azcueobserver.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
extern void azsdk_cue_configure(int, int) __attribute__((weak));
extern int azsdk_cue_register(int, void *) __attribute__((weak));
extern void azsdk_cue_retire(int) __attribute__((weak));
extern int azsdk_cue_read(int, struct azsdk_cue_sample *) __attribute__((weak));
extern int azsdk_cue_take(int, struct azsdk_cue_sample *) __attribute__((weak));
extern unsigned azsdk_cue_capabilities(void) __attribute__((weak));
extern int azsdk_cue_test_control(int, unsigned, uint64_t) __attribute__((weak));
extern void azsdk_cue_test_pending_hook(void (*)(int)) __attribute__((weak));
extern void azsdk_cue_test_collect_hook(void (*)(int)) __attribute__((weak));
static void configure(int on, int build) {
    if (azsdk_cue_configure) azsdk_cue_configure(on, build);
}
static int submit(unsigned deck, Fixture *fixture) { return azsdk_cue_register ? azsdk_cue_register((int) (deck), &fixture->unit) : 0; }
static void retire(unsigned deck) {
    if (azsdk_cue_retire) azsdk_cue_retire((int) (deck));
}
static int read(unsigned deck, struct azsdk_cue_sample *out, bool take) {
    __auto_type fn = take ? azsdk_cue_take : azsdk_cue_read;
    return fn ? fn((int) (deck), out) : 0;
}
static struct azsdk_cue_sample sample(unsigned deck, bool take) {
    struct Guarded {
        uint64_t before;
        struct azsdk_cue_sample value;
        uint64_t after;
    } out = {0};
    out.before = UINT64_C(0x123456789abcdef0);
    out.after = UINT64_C(0xfedcba9876543210);
    CHECK(read(deck, &out.value, take), "missing actual native cue callback to collected observation");
    CHECK(out.before == UINT64_C(0x123456789abcdef0) && out.after == UINT64_C(0xfedcba9876543210) &&
              out.value.unit_generation && out.value.revision && out.value.observed_ns &&
              out.value.changed_index_mask && !(out.value.changed_index_mask & ~0x1ffu) && !out.value.reserved,
          "bounded coherent cue receipt and caller output guards");
    return out.value;
}
static void unchanged(unsigned deck, bool take) {
    struct azsdk_cue_sample out, before;
    memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!read(deck, &out, take) && !memcmp(&out, &before, sizeof out), "unavailable/busy reader leaves caller output unchanged");
}
static void native_baseline() {
    Fixture fixture __attribute__((cleanup(fixture_finish)));
    fixture_init(&fixture);
    Lease listener __attribute__((cleanup(fixture_drop))) = counter_create(true);
    server_add(&fixture.server, &listener);
    Lease held __attribute__((cleanup(fixture_drop))) = get_lease(&fixture);
    emit(held, record(8, UINT32_MAX), 3, true);
    CHECK(((struct Counter *) listener.object)->plain == 1 && ((struct Counter *) listener.object)->routed == 1 && held.control->strong == 2, "genuine native weak promotion and typed callback baseline");
    puts("NATIVE_CUE_CALLBACK_BASELINE_OK weak16=16 index=8 player=3 property_revision_u32");
    fflush(stdout);
}
static void basic(unsigned build) {
    configure(1, (int) (build));
    Fixture contexts[4] __attribute__((cleanup(fixtures_drop))) = {0};
    for (unsigned d = 0; d < 4; ++d) fixture_init(&contexts[d]);
    Lease leases[4] __attribute__((cleanup(leases_drop))) = {0};
    for (unsigned d = 0; d != 4; ++d) {
        Lease original __attribute__((cleanup(fixture_drop))) = counter_create(false);
        server_add(&contexts[d].server, &original);
        __auto_type baseline_count = contexts[d].server.count;
        (void) submit(d, &contexts[d]);
        // The missing implementation adds no listener/model. Native typed
        // work has already run; the intended RED is the missing receipt below.
        Lease held __attribute__((cleanup(fixture_drop))) = contexts[d].server.count > baseline_count ? get_lease(&contexts[d]) : (Lease) {0};
        leases[d] = fixture_copy(held);
        emit(original, record(d, 0), contexts[d].server.native_routing_tag, true);
        if (held.object) emit(held, record(d, 0), contexts[d].server.native_routing_tag, true);
        CHECK(((struct Counter *) original.object)->plain == 1 && ((struct Counter *) original.object)->routed == 1, "same unit native typed callbacks still execute before observation");
        printf("NATIVE_CUE_UNIT_FORWARD_OK build=%u deck=%u\n", build, d);
        fflush(stdout);
        __auto_type value = sample(d, true);
        CHECK(value.changed_index_mask == (1u << d), "each logical deck receives only its own native cue index");
        __auto_type latest = sample(d, false);
        CHECK(!memcmp(&value, &latest, sizeof value), "read returns last GUI-collected receipt");
        unchanged(d, true);
        emit(held, record(8, 0), contexts[d].server.native_routing_tag, false);
        unchanged(d, true); // callback2 must not double count
        emit(held, record(9, 0), contexts[d].server.native_routing_tag, true);
        emit(held, record(UINT32_MAX, 0), contexts[d].server.native_routing_tag, true);
        unchanged(d, true);
        emit(held, record(8, 0), contexts[d].server.native_routing_tag ^ 1u, true);
        unchanged(d, true);
        emit(held, record(0, 0), contexts[d].server.native_routing_tag, true);
        emit(held, record(8, UINT32_MAX), contexts[d].server.native_routing_tag, true);
        emit(held, record(0, 0), contexts[d].server.native_routing_tag, true);
        __auto_type batch = sample(d, true);
        CHECK(batch.changed_index_mask == 0x101 && batch.unit_generation == value.unit_generation &&
                  batch.revision > value.revision && batch.observed_ns >= value.observed_ns,
              "bounded index mask coalesces pending changes without interpreting native revision wrap");
    }
    CHECK(azsdk_cue_capabilities && azsdk_cue_capabilities() == 15, "submitted mask contains all four units");
    Lease old __attribute__((cleanup(fixture_drop))) = get_lease(&contexts[0]);
    __auto_type previous = sample(0, false);
    retire(0);
    unchanged(0, false);
    emit(old, record(6, 0), contexts[0].server.native_routing_tag, true);
    unchanged(0, true);
    CHECK(submit(0, &contexts[0]), "replacement listener submitted");
    Lease fresh __attribute__((cleanup(fixture_drop))) = get_lease(&contexts[0]);
    emit(old, record(5, 0), contexts[0].server.native_routing_tag, true);
    unchanged(0, true);
    emit(fresh, record(2, 0), contexts[0].server.native_routing_tag, true);
    __auto_type next = sample(0, true);
    CHECK(next.unit_generation != previous.unit_generation && next.changed_index_mask == 4,
          "old held callback cannot publish into replacement generation");
    configure(0, (int) (build));
    fixture_drop(&leases[0]);
    leases[0] = fixture_copy(fresh);
    for (unsigned d = 0; d != 4; ++d) {
        unchanged(d, false);
        emit(leases[d], record(1, 0), contexts[d].server.native_routing_tag, true);
        unchanged(d, true);
    }
    CHECK(!azsdk_cue_capabilities(), "disable retires pending and collected publication");
}
static _Atomic(bool) entered = false, proceed = false;
static Lease collection_emitter;
static unsigned collection_route;
static void during_collection(int deck __attribute__((unused))) { emit(collection_emitter, record(5, 0), collection_route, true); }
static void pending_barrier(int deck __attribute__((unused))) {
    atomic_store_explicit(&(entered), true, memory_order_release);
    while (!atomic_load_explicit(&(proceed), memory_order_acquire)) sched_yield();
}
struct reentry_context {
    Fixture *fixture;
    Lease *lease;
};
static void reenter_registration(void *opaque) {
    struct reentry_context *context = opaque;
    retire(0);
    CHECK(submit(0, context->fixture), "native registration can reenter retire/register without SDK lock");
    *context->lease = get_lease(context->fixture);
    emit(*context->lease, record(4, 0), context->fixture->server.native_routing_tag, true);
}
struct disable_context {
    Fixture *fixture;
    Lease *lease;
    unsigned build;
};
static void disable_registration(void *opaque) {
    struct disable_context *context = opaque;
    *context->lease = get_lease(context->fixture);
    configure(0, (int) context->build);
}
static void escape_registration(void *opaque) {
    struct reentry_context *context = opaque;
    *context->lease = get_lease(context->fixture);
}
struct worker_context {
    Lease lease;
    unsigned route, offset, count;
};
static void *emit_worker(void *opaque) {
    struct worker_context *context = opaque;
    for (unsigned n = 0; n < context->count; ++n)
        emit(context->lease, record((context->offset + n) % 9, 0), context->route, true);
    return NULL;
}
static void registration_reentry(unsigned build) {
    for (unsigned throwing = 0; throwing < 2; ++throwing) {
        configure(1, (int) (build));
        Fixture outer __attribute__((cleanup(fixture_finish))), replacement __attribute__((cleanup(fixture_finish)));
        fixture_init(&outer);
        fixture_init(&replacement);
        Lease fresh __attribute__((cleanup(fixture_drop))) = {0};
        outer.server.fail_after = throwing;
        struct reentry_context context = {&replacement, &fresh};
        outer.server.on_register = reenter_registration;
        outer.server.context = &context;
        CHECK((throwing ? fixture_registration_unwind(azsdk_cue_register, 0, &outer.unit) : !submit(0, &outer)) && fresh.object, "old register does not acknowledge its reentrant replacement");
        CHECK(sample(0, true).changed_index_mask == 16 && azsdk_cue_capabilities() == 1,
              "old normal/throwing completion cannot invalidate replacement slot or pending callbacks");
        retire(0);
    }
    configure(1, (int) (build));
    Fixture disabled __attribute__((cleanup(fixture_finish)));
    fixture_init(&disabled);
    Lease escaped __attribute__((cleanup(fixture_drop))) = {0};
    struct disable_context context = {&disabled, &escaped, build};
    disabled.server.on_register = disable_registration;
    disabled.server.context = &context;
    CHECK(!submit(0, &disabled) && escaped.object, "native registration can reenter disable without SDK lock");
    emit(escaped, record(6, 0), disabled.server.native_routing_tag, true);
    unchanged(0, true);
    CHECK(!azsdk_cue_capabilities(), "old registration finish cannot reenable disabled generation");
}
static void races_and_failures(unsigned build) {
    configure(1, (int) (build));
    Fixture fixture __attribute__((cleanup(fixture_finish)));
    fixture_init(&fixture);
    CHECK(submit(0, &fixture), "race listener submitted");
    Lease held __attribute__((cleanup(fixture_drop))) = get_lease(&fixture);
    CHECK(azsdk_cue_test_control && azsdk_cue_test_pending_hook && azsdk_cue_test_collect_hook, "test controls are linked only in dedicated fixture");
    CHECK(azsdk_cue_test_control(0, 1, 1), "claim real GUI collector busy gate");
    emit(held, record(7, 0), fixture.server.native_routing_tag, true);
    unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 1, 0), "release real GUI collector gate");
    CHECK(sample(0, true).changed_index_mask == 128, "busy GUI take cannot lose a pending native callback");
    CHECK(azsdk_cue_test_control(0, 2, 1), "force actual clock failure path");
    emit(held, record(6, 0), fixture.server.native_routing_tag, true);
    unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 2, 0), "restore collection clock");
    CHECK(sample(0, true).changed_index_mask == 64, "clock failure preserves pending callback");
    pthread_t workers[4];
    struct worker_context worker_data[4];
    for (unsigned w = 0; w != 4; ++w) {
        worker_data[w] = (struct worker_context) {held, fixture.server.native_routing_tag, w, 1000};
        CHECK(!pthread_create(&workers[w], NULL, emit_worker, &worker_data[w]), "start concurrent callbacks");
    }
    for (unsigned w = 0; w != 4; ++w) CHECK(!pthread_join(workers[w], NULL), "join concurrent callbacks");
    CHECK(sample(0, true).changed_index_mask == 0x1ff, "contending callbacks never drop pending cue indices");
    collection_emitter = fixture_copy(held);
    collection_route = fixture.server.native_routing_tag;
    emit(held, record(4, 0), fixture.server.native_routing_tag, true);
    azsdk_cue_test_collect_hook(during_collection); // only after the real atomic drain
    CHECK(sample(0, true).changed_index_mask == 16, "collector returns exactly its drained batch");
    azsdk_cue_test_collect_hook(NULL);
    fixture_drop(&collection_emitter);
    CHECK(sample(0, true).changed_index_mask == 32, "callback during collection remains pending for next GUI tick");
    atomic_store_explicit(&(entered), false, memory_order_seq_cst);
    atomic_store_explicit(&(proceed), false, memory_order_seq_cst);
    azsdk_cue_test_pending_hook(pending_barrier);
    pthread_t old;
    struct worker_context old_data = {held, fixture.server.native_routing_tag, 8, 1};
    CHECK(!pthread_create(&old, NULL, emit_worker, &old_data), "start paused callback");
    while (!atomic_load_explicit(&(entered), memory_order_acquire)) sched_yield();
    retire(0);
    CHECK(submit(0, &fixture), "new listener while old callback lease is paused");
    Lease fresh __attribute__((cleanup(fixture_drop))) = get_lease(&fixture);
    atomic_store_explicit(&(proceed), true, memory_order_release);
    CHECK(!pthread_join(old, NULL), "join paused callback");
    azsdk_cue_test_pending_hook(NULL);
    unchanged(0, true);
    emit(fresh, record(3, 0), fixture.server.native_routing_tag, true);
    CHECK(sample(0, true).changed_index_mask == 8, "stale in-flight OR cannot reach replacement slot");
    retire(0);
    fixture.server.fail_after = true;
    Lease escaped __attribute__((cleanup(fixture_drop))) = {0};
    struct reentry_context escape_context = {&fixture, &escaped};
    fixture.server.on_register = escape_registration;
    fixture.server.context = &escape_context;
    CHECK(fixture_registration_unwind(azsdk_cue_register, 0, &fixture.unit) && escaped.object, "failed native registration keeps only its escaped callback lease");
    emit(escaped, record(4, 0), fixture.server.native_routing_tag, true);
    unchanged(0, true);
    fixture.server.fail_after = false;
    fixture.server.on_register = NULL;
    CHECK(submit(0, &fixture), "register after native exception");
    fixture_drop(&fresh);
    fresh = get_lease(&fixture);
    emit(fresh, record(1, 0), fixture.server.native_routing_tag, true);
    (void) sample(0, true);
    CHECK(azsdk_cue_test_control(0, 3, UINT64_MAX), "seed actual collection revision exhaustion");
    emit(fresh, record(2, 0), fixture.server.native_routing_tag, true);
    unchanged(0, true);
    CHECK(azsdk_cue_test_control(0, 4, 0) == 4, "revision exhaustion leaves pending mask intact");
    Fixture another __attribute__((cleanup(fixture_finish)));
    fixture_init(&another);
    CHECK(azsdk_cue_test_control(0, 5, (UINT64_C(1) << 55) - 1), "seed actual packed-generation upper bound");
    CHECK(!submit(1, &another) && !another.server.count, "generation never wraps or shifts away high bits into a reused identity");
    configure(0, (int) (build));
}
static void capacity(unsigned build) {
    configure(1, (int) (build));
    Fixture fixture __attribute__((cleanup(fixture_finish)));
    fixture_init(&fixture);
    unsigned accepted = 0;
    for (unsigned n = 0; n != 256; ++n) {
        retire(0);
        if (!submit(0, &fixture)) break;
        ++accepted;
    }
    CHECK(accepted && accepted < 256 && !submit(0, &fixture), "bounded generation slots refuse exhaustion without reuse");
    configure(0, (int) (build));
}
int main(int argc, char **argv) {
    CHECK(argc >= 2 && (strcmp(argv[1], "1") == 0 || strcmp(argv[1], "2") == 0), "separate profile process");
    const unsigned build = (unsigned) (argv[1][0] - '0');
    _Static_assert(sizeof(struct fixture_owner) == 16 && sizeof(CueRecord) == 0x100, "native fixture widths");
    native_baseline();
    basic(build);
    if (argc == 3 && !strcmp(argv[2], "capacity")) capacity(build);
    else {
        registration_reentry(build);
        races_and_failures(build);
    }
    puts("PASS cue: actual weak callbacks, per-unit pending/collected receipts, generation retirement and bounded draining");
}

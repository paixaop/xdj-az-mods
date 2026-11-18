/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual source/cache state and C dispatcher. Only the machine register bridge
 * is replaced by an explicit caller-owned frame for these integration cases. */
#define main sdkcachebirth_baseline_main
#include "sdkcachebirth_test.c"
#undef main
#include "../../src/common/azcacheentry.h"
typedef struct azce_call azce_call;
struct RawFactoryForward {
    const struct azcb_factory_borrow *borrow;
    struct azcb_forward original;
    unsigned forwarded;
};
static void raw_factory_original(azce_call *call) {
    struct RawFactoryForward *f = (void *) call->args[7];
    REQUIRE(call->args[0] == (uintptr_t) f->borrow->receiver_creator && call->args[1] == (uintptr_t) f->borrow->borrowed_pool, "native factory receiver/pool registers unchanged");
    ++f->forwarded;
    f->original.call(f->original.context);
    call->result[0] = (uintptr_t) *f->borrow->native_result;
}
static void raw_factory_check(struct RawFactoryForward *f) { REQUIRE(f->forwarded == 1, "throwing native factory forwards once"); }
static void dispatch_native_factory(const struct azcb_factory_borrow *borrow, struct azcb_forward original) {
    struct RawFactoryForward f __attribute__((cleanup(raw_factory_check))) = {borrow, original, 0};
    azce_call call = {0};
    call.kind = borrow->kind == AZCB_PRIMARY_FACTORY ? AZCE_KIND_PRIMARY_FACTORY : AZCE_KIND_STANDARD_FACTORY;
    call.args[0] = (uintptr_t) borrow->receiver_creator;
    call.args[1] = (uintptr_t) borrow->borrowed_pool;
    call.args[7] = (uintptr_t) &f;
    call.original = (uintptr_t) raw_factory_original;
    azsdk_cache_dispatch_call(&call);
    REQUIRE(f.forwarded == 1 && call.result[0] == (uintptr_t) *borrow->native_result, "production factory adapter preserves actual raw result and calls original once");
}
typedef struct DispatchFrame {
    azce_call call, input;
    struct {
        uint64_t before, value, after;
    } output;
    unsigned calls;
    Action action;
} DispatchFrame;
static void dispatch_input(const DispatchFrame *f) {
    REQUIRE(!memcmp(f->call.args, f->input.args, sizeof f->call.args) && !memcmp(f->call.vector_args, f->input.vector_args, sizeof f->call.vector_args) && f->call.original == f->input.original && f->call.kind == f->input.kind && f->call.reserved == f->input.reserved && f->call.caller_link == f->input.caller_link, "dispatcher preserves original frame arguments and forwarding identity");
}
static void dispatch_original(azce_call *call) {
    DispatchFrame *f = (void *) call->args[7];
    REQUIRE(call == &f->call, "dispatcher forwards the exact caller-owned register frame");
    dispatch_input(f);
    ++f->calls;
    call->result[0] = UINT64_C(0x836acced01234567);
    call->result[1] = UINT64_C(0x76543210fedcba98);
    for (unsigned i = 0; i < 4; ++i) {
        call->vector_results[i].words[0] = UINT64_C(0x300000000) + i;
        call->vector_results[i].words[1] = UINT64_C(0x400000000) + i;
    }
    *(uint64_t *) call->args[8] = UINT64_C(0x1298abcdef765430);
    printf("NATIVE_DISPATCH_FORWARD_OK build=%u kind=%u\n", build_id, call->kind);
    fflush(stdout);
    action_run(f->action);
}
static void dispatch_init(DispatchFrame *f, unsigned kind, Action action) {
    *f = (DispatchFrame) {.output = {UINT64_C(0x01abcdef23456789), 0, UINT64_C(0xfedcba9876543210)}, .action = action};
    f->call.kind = kind;
    f->call.reserved = 0x9a1b2c3d;
    f->call.caller_link = UINT64_C(0x123456789abcdef0);
    f->call.original = (uintptr_t) dispatch_original;
    for (unsigned i = 0; i < 9; ++i) f->call.args[i] = UINT64_C(0x73600000) + i;
    f->call.args[7] = (uintptr_t) f;
    f->call.args[8] = (uintptr_t) &f->output.value;
    for (unsigned i = 0; i < 8; ++i) {
        f->call.vector_args[i].words[0] = UINT64_C(0x100000000) + i;
        f->call.vector_args[i].words[1] = UINT64_C(0x200000000) + i;
    }
    f->input = f->call;
}
static void dispatch_return(const DispatchFrame *f) {
    dispatch_input(f);
    REQUIRE(f->calls == 1 && f->output.before == UINT64_C(0x01abcdef23456789) && f->output.value == UINT64_C(0x1298abcdef765430) && f->output.after == UINT64_C(0xfedcba9876543210) && f->call.result[0] == UINT64_C(0x836acced01234567) && f->call.result[1] == UINT64_C(0x76543210fedcba98), "original forwards exactly once with unchanged indirect result and integer results");
    for (unsigned i = 0; i < 4; ++i) REQUIRE(f->call.vector_results[i].words[0] == UINT64_C(0x300000000) + i && f->call.vector_results[i].words[1] == UINT64_C(0x400000000) + i, "dispatcher preserves original SIMD result frame");
}
static void dispatch_frame(void *v) {
    DispatchFrame *f = v;
    azsdk_cache_dispatch_call(&f->call);
}
struct DispatchCheck {
    World *world;
    const Born *born;
    uint64_t generation;
    bool throwing;
};
static void forwarding_original(void *v) {
    struct DispatchCheck *c = v;
    observe_query(&c->world->a, c->born);
    REQUIRE(!api()->counters().active && api()->counters().generation == c->generation, "unqualified routes remain forwarding-only without guessed native borrows");
}
static void forwarding_only(void) {
    reset();
    WORLD(world);
    Born born BORN = birth(&world.a);
    observe_query(&world.a, &born);
    struct DispatchCheck c = {&world, &born, api()->counters().generation, false};
    const unsigned kinds[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE, AZCE_KIND_QUERY, AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO, AZCE_KIND_PRIMARY_FACTORY, AZCE_KIND_STANDARD_FACTORY, AZCE_KIND_REPLY_TICKET, AZCE_KIND_REPLY_RECEPTION, AZCE_KIND_COUNT, UINT32_MAX};
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        DispatchFrame frame;
        dispatch_init(&frame, kinds[i], (Action) {forwarding_original, &c});
        dispatch_frame(&frame);
        dispatch_return(&frame);
    }
    printf("DISPATCH_FORWARD_BASELINE_OK build=%u\n", build_id);
    fflush(stdout);
}
static void mutation_original(void *v) {
    struct DispatchCheck *c = v;
    Queried q QUERIED = query(&c->world->a);
    REQUIRE(q.status != AZS_OK && !q.witness.birth_epoch, "missing mutation retirement before native original");
    REQUIRE(api()->counters().active == 1 && api()->counters().generation == c->generation + 1, "real mutation fence is active before the original callback");
    Context *others[] = {&c->world->b, &c->world->c, &c->world->d};
    for (size_t i = 0; i < 3; ++i) unavailable(query(others[i]));
    if (c->throwing) fixture_raise();
}
static void mutation(unsigned kind, bool throwing) {
    reset();
    WORLD(world);
    Context *contexts[] = {&world.a, &world.b, &world.c, &world.d};
    for (size_t i = 0; i < 4; ++i) {
        Born born BORN = birth(contexts[i]);
        observe_query(contexts[i], &born);
    }
    struct DispatchCheck c = {&world, NULL, api()->counters().generation, throwing};
    DispatchFrame frame;
    dispatch_init(&frame, kind, (Action) {mutation_original, &c});
    bool caught = catches(dispatch_frame, &frame);
    REQUIRE(caught == throwing, "dispatcher preserves the exact native unwind");
    dispatch_return(&frame);
    REQUIRE(!api()->counters().active && api()->counters().generation == c.generation + 1, "mutation retirement unwinds its real active depth on return or exception");
    for (size_t i = 0; i < 4; ++i) unavailable(query(contexts[i]));
}
struct DispatchNested {
    struct DispatchCheck check;
    DispatchFrame inner;
};
static void nested_mutation_original(void *v) {
    struct DispatchNested *n = v;
    REQUIRE(api()->counters().active == 2 && api()->counters().generation == n->check.generation + 2, "nested original observes two real retirement scopes");
    unavailable(query(&n->check.world->a));
    if (n->check.throwing) fixture_raise();
}
static void nested_dispatch_restore(unsigned *unused) {
    (void) unused;
    REQUIRE(api()->counters().active == 1, "inner return/unwind restores outer retirement scope");
}
static void outer_mutation_original(void *v) {
    struct DispatchNested *n = v;
    REQUIRE(api()->counters().active == 1, "outer mutation is active before nested dispatch");
    unsigned guard __attribute__((cleanup(nested_dispatch_restore))) = 0;
    dispatch_frame(&n->inner);
}
static void nested_mutation(bool throwing) {
    reset();
    WORLD(world);
    Born born BORN = birth(&world.a);
    observe_query(&world.a, &born);
    struct DispatchNested n = {.check = {&world, &born, api()->counters().generation, throwing}};
    dispatch_init(&n.inner, AZCE_KIND_COLLECTOR_DELETE, (Action) {nested_mutation_original, &n});
    DispatchFrame outer;
    dispatch_init(&outer, AZCE_KIND_SERVER_RESET, (Action) {outer_mutation_original, &n});
    bool caught = catches(dispatch_frame, &outer);
    REQUIRE(caught == throwing, "nested native unwind crosses both dispatch frames unchanged");
    dispatch_return(&n.inner);
    dispatch_return(&outer);
    REQUIRE(!api()->counters().active && api()->counters().generation == n.check.generation + 2, "all nested real retirement scopes finish without resetting generation");
    unavailable(query(&world.a));
}
static void selection_original(void *v) {
    struct DispatchCheck *c = v;
    unavailable(query(&c->world->a));
    REQUIRE(!api()->counters().active && api()->counters().generation == c->generation, "selection storage fencing does not retire cache layout provenance");
    if (c->throwing) fixture_raise();
}
static void selection_publication(unsigned kind, bool throwing) {
    reset();
    WORLD(world);
    Born a BORN = birth(&world.a), b BORN = birth(&world.b), c BORN = birth(&world.c), d BORN = birth(&world.d);
    struct DispatchCheck check = {&world, NULL, api()->counters().generation, throwing};
    DispatchFrame frame;
    dispatch_init(&frame, kind, (Action) {selection_original, &check});
    bool caught = catches(dispatch_frame, &frame);
    REQUIRE(caught == throwing, "selection store preserves native unwinding");
    dispatch_return(&frame);
    observe_query(&world.a, &a);
    observe_query(&world.b, &b);
    observe_query(&world.c, &c);
    observe_query(&world.d, &d);
}
static void destroy_selected_storage(void *v) {
    Context *c = v;
    clients_drop(&c->list.clients);
}
static void clear_selected_storage(void *v) {
    DispatchFrame store;
    dispatch_init(&store, AZCE_KIND_SELECTION_CLEAR, (Action) {destroy_selected_storage, v});
    dispatch_frame(&store);
    dispatch_return(&store);
}
static void selection_releases_storage(void) {
    reset();
    WORLD(world);
    Born born BORN = birth(&world.a);
    FixtureAdapter adapter = {.vtable = cbf_object_slots, .cache = born.cache.object};
    GUARD_ROWS(rows);
    Witness witness WITNESS = {0};
    QueryCall call = {rows.bytes, cb_native_query, &adapter, 0, 1, &world.a.list.mount, NULL, NULL};
    Scope scope = context_scope(&world.a);
    inside_query = (Action) {clear_selected_storage, &world.a};
    int status = api()->query(&scope, &call, &witness);
    inside_query = (Action) {0};
    GuardedRows_check(&rows);
    rows_drop(GuardedRows_value(&rows));
    REQUIRE(status != AZS_OK && !witness.birth_epoch, "selection fence rejects before rereading destroyed vector holder storage");
    clients_init(&world.a.list.clients);
    owner_set(context_selected(&world.a), born.cache);
    observe_query(&world.a, &born);
}
void azsdk_cache_invoke_original(azce_call *call) {
    REQUIRE(call && call->original, "fixture backend requires a real caller-owned original frame");
    ((void (*)(azce_call *)) call->original)(call);
}
#define UNUSED_ENTRY(n) \
    void azsdk_cache_entry_##n(void) { abort(); }
UNUSED_ENTRY(0)
UNUSED_ENTRY(1)
UNUSED_ENTRY(2) UNUSED_ENTRY(3) UNUSED_ENTRY(4) UNUSED_ENTRY(5) UNUSED_ENTRY(6) UNUSED_ENTRY(7) UNUSED_ENTRY(8) UNUSED_ENTRY(9) UNUSED_ENTRY(10) UNUSED_ENTRY(11) UNUSED_ENTRY(12) UNUSED_ENTRY(13) UNUSED_ENTRY(14) UNUSED_ENTRY(15) UNUSED_ENTRY(16) UNUSED_ENTRY(17) UNUSED_ENTRY(18) UNUSED_ENTRY(19) UNUSED_ENTRY(20) UNUSED_ENTRY(21) UNUSED_ENTRY(22)
#undef UNUSED_ENTRY
#ifndef SDK_CACHE_DISPATCH_MAIN
#define SDK_CACHE_DISPATCH_MAIN main
#endif
    int SDK_CACHE_DISPATCH_MAIN(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate process per supported build fixture");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK && api()->version == 1, "actual source/cache helpers configured without physical policy");
    factory_entry = dispatch_native_factory;
    first_birth();
    forwarding_only();
    factory_write_receipts();
    factory_cleanup_mutation();
    native_exceptions();
    for (unsigned kind = AZCE_KIND_SELECTION_STORE; kind <= AZCE_KIND_SELECTION_CLEAR; ++kind) {
        selection_publication(kind, false);
        selection_publication(kind, true);
    }
    selection_releases_storage();
    const unsigned kinds[] = {AZCE_KIND_SETUP, AZCE_KIND_LAYOUT, AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW, AZCE_KIND_COLLECTOR_DISPOSE, AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY, AZCE_KIND_LOADINFO_DESTROY, AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE};
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        mutation(kinds[i], false);
        mutation(kinds[i], true);
    }
    nested_mutation(false);
    nested_mutation(true);
    REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "dispatcher retains no native owners or rows");
    printf("PASS cache-dispatch build=%u: real retirement before originals; results, native unwinding, nested scopes and forwarding-only routes\n", build_id);
    return 0;
}

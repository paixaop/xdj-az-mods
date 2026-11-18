/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual source/cache helpers with independent C native owner/result fixtures. */
#include "cache_birth_fixture.h"
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.c"
#endif
#if defined(AZSDK_CACHE_RECLAIM_TESTING)
static void (*cache_reclaim_test_boundary)(unsigned);
#endif
#include SOURCES_INC
static void absent_reset(const Limits *limits) { (void) limits; }
static int absent_birth(const Scope *s, const BirthCall *c, Witness *out) {
    (void) s;
    (void) out;
    birth_call_forward(c);
    return AZS_UNSUPPORTED;
}
static int absent_query(const Scope *s, const QueryCall *c, Witness *out) {
    (void) s;
    (void) out;
    query_call_forward(c);
    return AZS_UNSUPPORTED;
}
static void absent_mutation(Mutation kind, void (*original)(void *), void *context) {
    (void) kind;
    original(context);
}
static Counters absent_counters(void) { return (Counters) {0}; }
static const Api absent = {1, absent_reset, absent_birth, absent_query, absent_mutation, absent_counters};
static const Api *api(void) {
    const Api *(*getter)(void) = azsdk_cache_birth_test_api_v1;
    return getter ? getter() : &absent;
}
#define GUARDED_TYPE(name, type)                                            \
    typedef struct name {                                                   \
        uint64_t before;                                                    \
        _Alignas(type) unsigned char bytes[sizeof(type)];                   \
        uint64_t after;                                                     \
    } name;                                                                 \
    static inline void name##_init(name *p) {                               \
        memset(p, 0, sizeof *p);                                            \
        p->before = UINT64_C(0x1032547698badcfe);                           \
        p->after = UINT64_C(0xfedcba9876543210);                            \
    }                                                                       \
    static inline type *name##_value(name *p) { return (type *) p->bytes; } \
    static inline void name##_check(name *p) { REQUIRE(p->before == UINT64_C(0x1032547698badcfe) && p->after == UINT64_C(0xfedcba9876543210), "original sret output guards"); }
GUARDED_TYPE(GuardedOwner, CacheOwner)
GUARDED_TYPE(GuardedRows, NativeRows)
#undef GUARDED_TYPE
#define GUARD_OWNER(name)                                           \
    GuardedOwner name __attribute__((cleanup(GuardedOwner_check))); \
    GuardedOwner_init(&name)
#define GUARD_ROWS(name)                                          \
    GuardedRows name __attribute__((cleanup(GuardedRows_check))); \
    GuardedRows_init(&name)
typedef struct Handler {
    unsigned char pad[0x30];
    InformationUpdater *updater;
} Handler;
typedef struct Dispatcher {
    unsigned char pad[0x20];
    Handler *handler;
} Dispatcher;
typedef struct Context {
    MusicLibraryServer server;
    InformationUpdater updater;
    Handler handler;
    Dispatcher dispatcher;
    ListServer list;
    unsigned bank;
    _Alignas(8) uint64_t connection, alternate_connection;
    struct azsdk_source_sample source;
} Context;
static inline SqliteMountPoint *context_mount(Context *c) { return c->list.mount.object; }
static inline SqliteDataBase *context_database(Context *c) { return context_mount(c)->database.object; }
static inline ListCacheCollector *context_collector(Context *c) { return c->list.collector.object; }
static inline CacheOwner *context_selected(Context *c) { return c->list.clients.begin[c->bank].begin; }
static Scope context_scope(Context *c) { return (Scope) {build_id, c->bank, &c->server, &c->list, c->source.owner_epoch}; }
static void context_capture(Context *c) {
    Scope scope = context_scope(c);
    Build profile = {builds[build_id].dispatcher, load(uintptr_t, (uintptr_t) c->server.vtable + 5 * 8) - source_state().main_image.base};
    SqlProfile sql = {0};
    (void) scope;
    REQUIRE(observe((uintptr_t) &c->server, &profile, &sql, &metadata_ops, &c->source, sizeof c->source) == AZS_OK, "existing real production source capture");
}
static void context_init(Context *c, unsigned bank, unsigned tag) {
    *c = (Context) {.bank = bank};
    c->server.vtable = cbf_server_table.slots;
    c->server.worker = pthread_self();
    c->server.worker_valid = true;
    c->updater.vtable = cbf_updater_table.slots;
    c->handler.updater = &c->updater;
    c->dispatcher.handler = &c->handler;
    void *d = &c->dispatcher, *worker = &c->server.worker;
    memcpy((char *) &c->server + builds[build_id].dispatcher, &d, sizeof d);
    memcpy((char *) &c->server + 0x28, &worker, sizeof worker);
    ((unsigned char *) &c->server)[0xd2] = 1;
    c->updater.mount = cbf_mount_new(0x301, &c->connection, tag);
    list_init(&c->list);
    owner_set(&c->list.mount, c->updater.mount);
    native_connection_event(&c->connection, 1);
    context_capture(c);
}
static void context_drop(Context *c) {
    native_connection_event(&c->connection, 0);
    list_drop(&c->list);
    fixture_drop(&c->updater.mount);
}
static void context_same_source(Context *c, Context *other) {
    owner_set(&c->updater.mount, other->updater.mount);
    owner_set(&c->list.mount, c->updater.mount);
    context_capture(c);
}
typedef struct World {
    Context a, b, c, d;
} World;
static void world_init(World *w) {
    context_init(&w->a, 0, 101);
    context_init(&w->b, 3, 202);
    context_init(&w->c, 1, 303);
    context_init(&w->d, 2, 404);
}
static void world_drop(World *w) {
    context_drop(&w->d);
    context_drop(&w->c);
    context_drop(&w->b);
    context_drop(&w->a);
}
#define WORLD(name)                                  \
    World name __attribute__((cleanup(world_drop))); \
    world_init(&name)
typedef struct Born {
    CacheOwner cache;
    Witness witness;
    int status;
} Born;
static void born_drop(Born *b) {
    witness_drop(&b->witness);
    fixture_drop(&b->cache);
}
#define BORN __attribute__((cleanup(born_drop)))
typedef struct Queried {
    Witness witness;
    int status;
    unsigned value;
} Queried;
static void queried_drop(Queried *q) { witness_drop(&q->witness); }
#define QUERIED __attribute__((cleanup(queried_drop)))
static Born birth_full(Context *context, unsigned content, Action action, unsigned factory_kind, unsigned fault, unsigned mode) {
    CreatorOwner creator CREATOR = creator_new();
    owner_set(&creator->database, context_mount(context)->database);
    creator->content = content;
    creator->inside_make = action;
    creator->factory_kind = factory_kind;
    creator->fault = fault;
    creator->mode = mode;
    GUARD_OWNER(result);
    Witness witness = {0};
    Scope scope = context_scope(context);
    BirthCall call = {result.bytes, cb_native_create, context_collector(context), &creator};
    unsigned prior = creator->forwarded_calls, database_count = owner_count(creator->database), mount_count = owner_count(context->list.mount), collector_count = owner_count(context->list.collector);
    /* Nothing owns result storage until native forwarding completes normally. */
    int status = api()->birth(&scope, &call, &witness);
    GuardedOwner_check(&result);
    REQUIRE(creator->forwarded_calls == prior + 1, "original creation forwarded exactly once");
    REQUIRE(creator->result_destination == (uintptr_t) result.bytes, "original shared owner hidden destination unchanged");
    REQUIRE(owner_count(creator->database) == database_count, "observer must not retain a strong Creator DB owner");
    REQUIRE(owner_count(context->list.mount) == mount_count && owner_count(context->list.collector) == collector_count, "birth observer retains only genuine weak source/Collector witnesses");
    CacheOwner value = *GuardedOwner_value(&result);
    *GuardedOwner_value(&result) = (CacheOwner) {0};
    REQUIRE(value.object && owner_count(value) == 2, "exact native returned/Collector owner counts");
    owner_set(context_selected(context), value);
    return (Born) {value, witness, status};
}
static Born birth(Context *c) { return birth_full(c, 11, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0); }
static Queried query(Context *c) {
    FixtureAdapter adapter = {.vtable = cbf_object_slots, .cache = context_selected(c)->object};
    GUARD_ROWS(output);
    Scope scope = context_scope(c);
    QueryCall call = {output.bytes, cb_native_query, &adapter, 0, 1, &c->list.mount, NULL, NULL};
    unsigned cache_count = owner_count(*context_selected(c)), collector_count = owner_count(c->list.collector), mount_count = owner_count(c->list.mount), database_count = owner_count(context_mount(c)->database), prior = queries, destroyed = row_destroyed;
    Witness witness = {0};
    int status = api()->query(&scope, &call, &witness);
    GuardedRows_check(&output);
    REQUIRE(queries == prior + 1, "original row query forwarded exactly once");
    REQUIRE(adapter.result_destination == (uintptr_t) output.bytes, "original 24-byte hidden destination unchanged without moving result");
    NativeRows *rows = GuardedRows_value(&output);
    REQUIRE(row_alive == 1 && rows->end == rows->begin + 1, "row remains owned by native result caller");
    unsigned value = rows->begin[0]->value;
    rows_drop(rows);
    REQUIRE(!row_alive && row_destroyed == destroyed + 1, "native row caller alone releases row");
    REQUIRE(owner_count(*context_selected(c)) == cache_count && owner_count(c->list.collector) == collector_count, "query pins end with native callback");
    REQUIRE(owner_count(c->list.mount) == mount_count && owner_count(context_mount(c)->database) == database_count, "query source owner pins end with native callback");
    return (Queried) {witness, status, value};
}
static bool owners_equal(CacheOwner weak, CacheOwner strong) { return weak.control == strong.control; }
static void observed(Context *c, const Born *b, const Queried *q) {
    REQUIRE(b->status == AZS_OK && q->status == AZS_OK && b->witness.birth_epoch && q->witness.birth_epoch, "missing observed cache birth and real ownership result");
    REQUIRE(b->witness.birth_epoch == q->witness.birth_epoch && q->witness.source_epoch == c->source.owner_epoch, "birth/source epoch follows exact actual cache owner");
    REQUIRE(owners_equal(q->witness.cache, b->cache) && owners_equal(q->witness.collector, c->list.collector) && owners_equal(q->witness.mount, c->list.mount) && owners_equal(q->witness.database, context_mount(c)->database), "witness contains genuine source/cache/Collector weak owners");
    REQUIRE(q->witness.row_value == q->value && q->value == context_database(c)->fixture_layout, "copy actual row scalar while native row is alive");
}
static void observe_query(Context *c, const Born *b) {
    Queried q QUERIED = query(c);
    observed(c, b, &q);
}
static void unavailable(Queried q) {
    REQUIRE(q.status != AZS_OK && !q.witness.birth_epoch, "no stale/partial owner publication");
    queried_drop(&q);
}
static void thunk(void *v) { action_run(*(Action *) v); }
static void mutate(Mutation kind, Action action) { api()->mutation(kind, thunk, &action); }
static void reset_limits(Limits limits) {
    azsdk_sources_sweep();
    after_owner = inside_query = factory_tail = inside_final_write = (Action) {0};
    api()->reset(&limits);
}
static void reset(void) { reset_limits((Limits) LIMITS_INIT); }
static void first_birth(void) {
    reset();
    WORLD(w);
    Born b BORN = birth(&w.a);
    Queried q QUERIED = query(&w.a);
    printf("NATIVE_FORWARD_OK build=%u owner16=%zu rows24=%zu creates=%u queries=%u\n", build_id, sizeof(CacheOwner), sizeof(NativeRows), creates, queries);
    fflush(stdout);
    observed(&w.a, &b, &q);
    Context *others[] = {&w.b, &w.c, &w.d};
    for (size_t i = 0; i < 3; ++i) {
        Born other BORN = birth(others[i]);
        observe_query(others[i], &other);
    }
}
static void refused_unchanged(Context *context, const Born *known, Scope scope, const char *reason, Action copy_boundary, Action restore) {
    FixtureAdapter adapter = {.vtable = cbf_object_slots, .cache = context_selected(context)->object};
    GUARD_ROWS(output);
    QueryCall call = {output.bytes, cb_native_query, &adapter, 0, 1, &context->list.mount, copy_boundary.call, copy_boundary.context};
    Witness witness WITNESS = {0};
    witness_copy(&witness, &known->witness);
    witness.birth_epoch = UINT64_C(0x13579bdf);
    witness.layout_generation = UINT64_C(0x2468ace0);
    witness.source_epoch = UINT64_C(0x1020304050607080);
    witness.row_value = 0xcafe;
    unsigned prior = queries, destroyed = row_destroyed, cache_count = owner_count(*context_selected(context)), collector_count = owner_count(context->list.collector), mount_count = owner_count(context->list.mount), database_count = owner_count(context_mount(context)->database);
    int status = api()->query(&scope, &call, &witness);
    action_run(restore);
    GuardedRows_check(&output);
    REQUIRE(queries == prior + 1, "refused observation still forwards original query exactly once");
    REQUIRE(cb_native_rows_destination == (uintptr_t) output.bytes, "refusal preserves original native 24-byte hidden destination without a container move");
    REQUIRE(status != AZS_OK && witness.birth_epoch == UINT64_C(0x13579bdf) && witness.layout_generation == UINT64_C(0x2468ace0) && witness.source_epoch == UINT64_C(0x1020304050607080) && witness.row_value == 0xcafe && owners_equal(witness.cache, known->cache) && owners_equal(witness.collector, context->list.collector) && owners_equal(witness.mount, context->list.mount) && owners_equal(witness.database, context_mount(context)->database), reason);
    NativeRows *rows = GuardedRows_value(&output);
    REQUIRE(row_alive == 1 && rows->end == rows->begin + 1, "refusal leaves the genuine returned row with its native caller");
    rows_drop(rows);
    REQUIRE(!row_alive && row_destroyed == destroyed + 1 && owner_count(*context_selected(context)) == cache_count && owner_count(context->list.collector) == collector_count, "refusal releases scoped pins and never retains a native row");
    REQUIRE(owner_count(context->list.mount) == mount_count && owner_count(context_mount(context)->database) == database_count, "refusal releases scoped source owner pins");
}
static CacheOwner native_hit(Context *context, unsigned content) {
    CreatorOwner unused CREATOR = creator_new();
    owner_set(&unused->database, context_mount(context)->database);
    unused->content = content;
    unsigned prior = creates;
    OwnerVector *caches = &context_collector(context)->caches;
    for (CacheOwner *cache = caches->begin; cache != caches->end; ++cache) {
        ListCache *p = cache->object;
        if (p->condition->content == content && p->condition->mode == unused->mode) {
            CacheOwner hit = fixture_copy(*cache);
            owner_set(context_selected(context), hit);
            REQUIRE(creates == prior, "native condition hit skips creation");
            return hit;
        }
    }
    REQUIRE(false, "fixture must hit the actual retained condition/cache owner");
    return (CacheOwner) {0};
}
static void close_context(void *v) {
    Context *c = v;
    native_connection_event(&c->connection, 0);
}
static void sql_connection_lifetime(void) {
    reset();
    WORLD(w);
    native_connection_event(&w.a.connection, 0);
    Born unknown BORN = birth(&w.a);
    REQUIRE(unknown.status != AZS_OK && !unknown.witness.birth_epoch, "unobserved/retired SQLite connection cannot qualify cache birth");
    unavailable(query(&w.a));
    native_connection_event(&w.a.connection, 1);
    context_capture(&w.a);
    Born known BORN = birth(&w.a);
    observe_query(&w.a, &known);
    uint64_t epoch = w.a.source.owner_epoch;
    native_connection_event(&w.a.connection, 0);
    struct azsdk_source_sample retired = {0};
    REQUIRE(azsdk_sources_read(epoch, &retired, sizeof retired) == AZS_STALE && !(retired.flags & AZS_CURRENT), "observed close invalidates source despite unchanged real weak owners and raw connection");
    unavailable(query(&w.a));
    native_connection_event(&w.a.connection, 1);
    context_capture(&w.a);
    REQUIRE(w.a.source.owner_epoch != epoch, "fresh observed allocation at same raw address gets new source epoch");
    Born fresh BORN = birth(&w.a);
    observe_query(&w.a, &fresh);
    REQUIRE(fresh.witness.source_epoch != known.witness.source_epoch, "cache source cannot adopt a reused SQLite address");
    inside_query = (Action) {close_context, &w.a};
    unavailable(query(&w.a));
    inside_query = (Action) {0};
    native_connection_event(&w.a.connection, 1);
    context_capture(&w.a);
    Born copied BORN = birth(&w.a);
    observe_query(&w.a, &copied);
    refused_unchanged(&w.a, &copied, context_scope(&w.a), "copier-close invalidates actual connection generation before output", (Action) {close_context, &w.a}, (Action) {0});
}
struct Isolation {
    Context *context;
    uint64_t epoch;
    bool sweep;
};
static bool source_retained(uint64_t epoch) {
    for (size_t i = 0; i < kSlots; ++i)
        if (source_state().entries[i].sample.owner_epoch == epoch) return !source_state().entries[i].retired;
    return false;
}
static int isolation_original(void *v) {
    struct Isolation *i = v;
    if (i->sweep) {
        azsdk_sources_sweep();
        REQUIRE(source_retained(i->epoch), "sweep cannot reclaim source hidden only by unrelated active connection");
    } else {
        Context *c = i->context;
        Build profile = {builds[build_id].dispatcher, load(uintptr_t, (uintptr_t) c->server.vtable + 5 * 8) - source_state().main_image.base};
        SqlProfile sql = {0};
        struct azsdk_source_sample output = {.owner_epoch = 0xcafe};
        int status = observe((uintptr_t) &c->server, &profile, &sql, &metadata_ops, &output, sizeof output);
        REQUIRE(status != AZS_OK && output.owner_epoch == 0xcafe, "unrelated active operation refuses current publication without partial output");
        REQUIRE(source_retained(i->epoch), "unrelated active connection cannot permanently retire another source");
    }
    return 0;
}
static void sql_connection_isolation(void) {
    reset();
    WORLD(w);
    Born known BORN = birth(&w.b);
    observe_query(&w.b, &known);
    uint64_t epoch = w.b.source.owner_epoch;
    struct Isolation i = {&w.b, epoch, false};
    az_connection_forward_close(&source_state().connection_epochs, &w.a.connection, isolation_original, &i);
    struct azsdk_source_sample current = {0};
    REQUIRE(azsdk_sources_read(epoch, &current, sizeof current) == AZS_OK && (current.flags & AZS_CURRENT), "unchanged source receipt recovers after unrelated lifecycle operation");
    observe_query(&w.b, &known);
    i.sweep = true;
    az_connection_forward_close(&source_state().connection_epochs, &w.c.connection, isolation_original, &i);
    observe_query(&w.b, &known);
    context_capture(&w.b);
    REQUIRE(w.b.source.owner_epoch == epoch, "unrelated operation cannot mint a duplicate source key");
}
static void hit_and_sources(void) {
    reset();
    WORLD(w);
    Born b BORN = birth(&w.a);
    observe_query(&w.a, &b);
    context_same_source(&w.c, &w.a);
    REQUIRE(w.c.source.owner_epoch == w.a.source.owner_epoch, "same real source owner shares existing epoch");
    owner_set(&w.a.list.mount, w.b.list.mount);
    uint64_t before = api()->counters().next_birth;
    CacheOwner hit OWNED = native_hit(&w.a, 11);
    REQUIRE(owners_equal(b.witness.cache, hit) && api()->counters().next_birth == before, "condition hit retains the actual owner and never allocates a new birth epoch");
    Scope frame = {build_id, w.a.bank, &w.b.server, &w.a.list, w.b.source.owner_epoch};
    FixtureAdapter adapter = {.vtable = cbf_object_slots, .cache = b.cache.object};
    GUARD_ROWS(rows);
    Witness witness WITNESS = {0};
    QueryCall call = {rows.bytes, cb_native_query, &adapter, 0, 1, &w.a.list.mount, NULL, NULL};
    int status = api()->query(&frame, &call, &witness);
    GuardedRows_check(&rows);
    rows_drop(GuardedRows_value(&rows));
    REQUIRE(status != AZS_OK && !witness.birth_epoch, "cache hit cannot relabel birth with another live DB/source");
    owner_set(&w.a.list.mount, w.a.updater.mount);
    observe_query(&w.a, &b);
    Born c BORN = birth(&w.c);
    observe_query(&w.c, &c);
    REQUIRE(!owners_equal(c.witness.cache, b.cache) && !owners_equal(c.witness.collector, w.a.list.collector), "client/source ownership contexts remain independent");
}
static void raise_action(void *v) {
    (void) v;
    fixture_raise();
}
struct BirthAction {
    Context *context;
    Action inside;
};
static void birth_action(void *v) {
    struct BirthAction *a = v;
    Born result BORN = birth_full(a->context, 11, a->inside, AZCB_PRIMARY_FACTORY, 0, 0);
}
static void query_action(void *v) { Queried result QUERIED = query(v); }
static bool catches(void (*call)(void *), void *v) {
    unsigned before = fixture_exception_deletions;
    bool caught = !az_native_try(call, v);
    REQUIRE(fixture_exception_deletions == before + (unsigned) caught, "native unwind payload reaches its handler once");
    return caught;
}
static void native_exceptions(void) {
    reset();
    WORLD(w);
    struct BirthAction a = {&w.a, {raise_action, NULL}};
    bool caught = catches(birth_action, &a);
    REQUIRE(caught && api()->counters().retained == 0 && !api()->counters().active, "creator exception propagates with no partial birth");
    after_owner = (Action) {raise_action, NULL};
    a.inside = (Action) {0};
    caught = catches(birth_action, &a);
    after_owner = (Action) {0};
    REQUIRE(caught && !owner_vector_size(&context_collector(&w.a)->caches), "native temporary owner cleanup on unwind");
    Born b BORN = birth(&w.a);
    observe_query(&w.a, &b);
    inside_query = (Action) {raise_action, NULL};
    caught = catches(query_action, &w.a);
    inside_query = (Action) {0};
    REQUIRE(caught && !api()->counters().active && !row_alive, "row query exception propagates without stranded scope");
    Born next BORN = birth(&w.b);
    observe_query(&w.b, &next);
}
struct ReentrantBirth {
    Context *context;
    Born nested;
};
static void reentrant_birth_action(void *v) {
    struct ReentrantBirth *r = v;
    r->nested = birth(r->context);
    observe_query(r->context, &r->nested);
}
static void reentrant_birth(void) {
    reset();
    WORLD(w);
    struct ReentrantBirth r = {.context = &w.b};
    Born outer BORN = birth_full(&w.a, 11, (Action) {reentrant_birth_action, &r}, AZCB_PRIMARY_FACTORY, 0, 0);
    REQUIRE(outer.status != AZS_OK && !outer.witness.birth_epoch, "nested construction permanently poisons its surrounding factory");
    unavailable(query(&w.a));
    observe_query(&w.b, &r.nested);
    Born fresh BORN = birth(&w.c);
    observe_query(&w.c, &fresh);
    REQUIRE(fresh.witness.source_epoch != r.nested.witness.source_epoch && fresh.witness.birth_epoch != r.nested.witness.birth_epoch, "nested refusal restores the prior frame before the next independent construction");
    born_drop(&r.nested);
}
static void mutate_setter(void *v) {
    (void) v;
    mutate(setter, (Action) {0});
}
static void factory_cleanup_mutation(void) {
    reset();
    WORLD(w);
    Context *contexts[] = {&w.a, &w.b, &w.c, &w.d};
    for (size_t i = 0; i < 4; ++i) {
        after_owner = (Action) {mutate_setter, NULL};
        Born value BORN = birth(contexts[i]);
        after_owner = (Action) {0};
        REQUIRE(value.status != AZS_OK && !value.witness.birth_epoch, "post-factory mutation must not acquire a new birth stamp");
        unavailable(query(contexts[i]));
    }
}
static void factory_write_receipts(void) {
    reset();
    WORLD(w);
    Context *contexts[] = {&w.a, &w.b, &w.c, &w.d};
    for (size_t i = 0; i < 4; ++i) {
        Context *c = contexts[i];
        Born primary BORN = birth(c);
        observe_query(c, &primary);
        Born fallback BORN = birth_full(c, 81, (Action) {0}, AZCB_STANDARD_FALLBACK_FACTORY, 0, 0);
        observe_query(c, &fallback);
        REQUIRE(fallback.witness.layout_generation > primary.witness.layout_generation, "complete fallback writes retain their final successful generation");
        owner_set(context_selected(c), primary.cache);
        unavailable(query(c));
        owner_set(context_selected(c), fallback.cache);
        observe_query(c, &fallback);
        for (unsigned fault = 1; fault <= 6; ++fault) {
            Born bad BORN = birth_full(c, 81 + fault, (Action) {0}, AZCB_STANDARD_FALLBACK_FACTORY, fault, 0);
            REQUIRE(bad.status != AZS_OK && !bad.witness.birth_epoch, "wrong setup/status or missing/repeated/extra/failing setter sequence declines");
            unavailable(query(c));
        }
        factory_tail = (Action) {mutate_setter, NULL};
        Born after_final BORN = birth_full(c, 91, (Action) {0}, AZCB_STANDARD_FALLBACK_FACTORY, 0, 0);
        factory_tail = (Action) {0};
        REQUIRE(after_final.status != AZS_OK && !after_final.witness.birth_epoch, "factory cleanup mutation cannot replace final-setter stamp");
        inside_final_write = (Action) {mutate_setter, NULL};
        Born nested BORN = birth_full(c, 92, (Action) {0}, AZCB_STANDARD_FALLBACK_FACTORY, 0, 0);
        inside_final_write = (Action) {0};
        REQUIRE(nested.status != AZS_OK && !nested.witness.birth_epoch, "nested mutation inside final setter cannot be adopted as its own fence");
        Born unsupported BORN = birth_full(c, 93, (Action) {0}, 99, 0, 0);
        REQUIRE(unsupported.status != AZS_OK && !unsupported.witness.birth_epoch, "unreviewed factory mode still forwards but cannot publish");
        Born wrong_mode BORN = birth_full(c, 94, (Action) {0}, AZCB_STANDARD_FALLBACK_FACTORY, 0, 1);
        REQUIRE(wrong_mode.status != AZS_OK && !wrong_mode.witness.birth_epoch, "unsupported standard Creator mode must forward without a birth claim");
    }
}
static void nested_mutation_inner(void *v) {
    REQUIRE(api()->counters().active >= 2, "nested mutation guard remains active");
    unavailable(query(v));
}
static void nested_mutation_outer(void *v) {
    unavailable(query(v));
    mutate(setter, (Action) {nested_mutation_inner, v});
}
static void throwing_mutation(void *v) {
    (void) v;
    mutate(setup, (Action) {raise_action, NULL});
}
static void mutation_guards(void) {
    reset();
    WORLD(w);
    for (Mutation kind = setup; kind <= dispose; ++kind) {
        Born b BORN = birth(&w.a);
        observe_query(&w.a, &b);
        uint64_t before = api()->counters().generation;
        mutate(kind, (Action) {nested_mutation_outer, &w.a});
        REQUIRE(!api()->counters().active && api()->counters().generation > before, "retirement precedes original and unwinds depth");
        unavailable(query(&w.a));
    }
    Born b BORN = birth(&w.b);
    observe_query(&w.b, &b);
    bool caught = catches(throwing_mutation, NULL);
    REQUIRE(caught && !api()->counters().active, "mutation exception preserves native exception and clears active guard");
    unavailable(query(&w.b));
    Born renewed BORN = birth(&w.c);
    observe_query(&w.c, &renewed);
    inside_query = (Action) {mutate_setter, NULL};
    unavailable(query(&w.c));
    inside_query = (Action) {0};
}
static void bounded_nonwrap(void) {
    Limits limits = LIMITS_INIT;
    limits.slots = 2;
    reset_limits(limits);
    WORLD(w);
    Born a BORN = birth(&w.a);
    observe_query(&w.a, &a);
    Born b BORN = birth(&w.b);
    observe_query(&w.b, &b);
    Born c BORN = birth(&w.c);
    REQUIRE(c.status != AZS_OK && !c.witness.birth_epoch, "bounded birth table fails unavailable");
    unavailable(query(&w.c));
    REQUIRE(api()->counters().retained <= 2, "no hidden unbounded owner registry");
    limits.next_birth = UINT64_MAX - 1;
    reset_limits(limits);
    Born last BORN = birth(&w.d);
    observe_query(&w.d, &last);
    REQUIRE(last.witness.birth_epoch == UINT64_MAX, "final nonzero birth epoch");
    Born exhausted BORN = birth_full(&w.c, 22, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    REQUIRE(exhausted.status != AZS_OK && !exhausted.witness.birth_epoch, "birth epoch exhaustion never wraps or reuses");
    REQUIRE(api()->counters().next_birth == UINT64_MAX, "exhausted birth counter remains saturated");
    limits.next_birth = 0;
    limits.generation = UINT64_MAX;
    reset_limits(limits);
    Born g BORN = birth_full(&w.a, 23, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &g);
    mutate(setter, (Action) {0});
    unavailable(query(&w.a));
    REQUIRE(api()->counters().generation == UINT64_MAX && api()->counters().saturated, "layout generation never wraps");
    limits.generation = 1;
    limits.depth = 1;
    reset_limits(limits);
    mutate(setup, (Action) {mutate_setter, NULL});
    REQUIRE(!api()->counters().active && api()->counters().saturated, "bounded nesting poisons without depth wrap");
}

static void weak_lifetimes_and_reuse(void) {
    reset();
    WORLD(w);
    cache_reuse_next = true;
    Born old BORN = birth(&w.a);
    observe_query(&w.a, &old);
    void *address = old.cache.object;
    CacheOwner old_weak WEAK = fixture_weak(old.witness.cache);
    fixture_drop(context_selected(&w.a));
    owner_vector_clear(&context_collector(&w.a)->caches);
    fixture_drop(&old.cache);
    REQUIRE(fixture_expired(old_weak), "birth table retains no strong cache/row owner");
    cache_reuse_next = true;
    Born fresh BORN = birth_full(&w.a, 22, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &fresh);
    REQUIRE(fresh.cache.object == address && !owners_equal(old_weak, fresh.cache) && fresh.witness.birth_epoch != old.witness.birth_epoch, "same cache address is a different real owner");
    fixture_drop(&fresh.cache);
    fixture_drop(context_selected(&w.a));
    owner_vector_clear(&context_collector(&w.a)->caches);
    fixture_drop(&w.a.list.collector);
    collector_reuse_next = true;
    w.a.list.collector = collector_new();
    Born first BORN = birth_full(&w.a, 31, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &first);
    void *collector_address = w.a.list.collector.object;
    CacheOwner weak_collector WEAK = fixture_weak(first.witness.collector);
    fixture_drop(&first.cache);
    fixture_drop(context_selected(&w.a));
    fixture_drop(&w.a.list.collector);
    REQUIRE(fixture_expired(weak_collector), "birth table cannot prolong Collector/pool lifetime");
    collector_reuse_next = true;
    w.a.list.collector = collector_new();
    Born next BORN = birth_full(&w.a, 32, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &next);
    REQUIRE(w.a.list.collector.object == collector_address && !owners_equal(weak_collector, w.a.list.collector), "Collector address reuse cannot select an old pool owner");
}
struct ReleasePins {
    Context *context;
    Born *born;
    CacheOwner cache, collector;
};
static void release_pins(void *v) {
    struct ReleasePins *p = v;
    owner_vector_clear(&context_collector(p->context)->caches);
    fixture_drop(context_selected(p->context));
    fixture_drop(&p->born->cache);
    fixture_drop(&p->context->list.collector);
    REQUIRE(!fixture_expired(p->cache) && !fixture_expired(p->collector) && pool_alive == 4, "actual query frame pins cache AND its owning Collector/pool");
}
static void release_during_query(void *v) { mutate(clear, (Action) {release_pins, v}); }
static void actual_query_pins_and_output(void) {
    reset();
    WORLD(w);
    Born b BORN = birth(&w.a);
    observe_query(&w.a, &b);
    CacheOwner cache WEAK = fixture_weak(b.witness.cache), collector WEAK = fixture_weak(b.witness.collector);
    FixtureAdapter adapter = {.vtable = cbf_object_slots, .cache = b.cache.object};
    GUARD_ROWS(output);
    Scope scope = context_scope(&w.a);
    QueryCall call = {output.bytes, cb_native_query, &adapter, 0, 1, &w.a.list.mount, NULL, NULL};
    Witness sentinel WITNESS = {0};
    witness_copy(&sentinel, &b.witness);
    sentinel.row_value = 0xf00d;
    uint64_t birth_epoch = sentinel.birth_epoch, source_epoch = sentinel.source_epoch;
    struct ReleasePins p = {&w.a, &b, cache, collector};
    inside_query = (Action) {release_during_query, &p};
    int status = api()->query(&scope, &call, &sentinel);
    inside_query = (Action) {0};
    GuardedRows_check(&output);
    REQUIRE(status != AZS_OK && sentinel.birth_epoch == birth_epoch && sentinel.source_epoch == source_epoch && sentinel.row_value == 0xf00d && owners_equal(sentinel.cache, cache), "retired query leaves existing nontrivial output unchanged");
    REQUIRE(fixture_expired(cache) && fixture_expired(collector) && pool_alive == 3, "scoped query pins release before returning to native caller");
    REQUIRE(row_alive == 1, "native caller still exclusively owns returned row after owner retirement");
    rows_drop(GuardedRows_value(&output));
    REQUIRE(!row_alive, "no row retained by observer");
}
static void existing_source_retirement(void) {
    reset();
    WORLD(w);
    Born b BORN = birth(&w.a);
    observe_query(&w.a, &b);
    uint64_t before = w.a.source.owner_epoch;
    REQUIRE(!pthread_mutex_lock(&source_state().registry_mutex), "source retirement locks actual registry");
    retire_server((uintptr_t) &w.a.server);
    REQUIRE(!pthread_mutex_unlock(&source_state().registry_mutex), "source retirement unlocks actual registry");
    struct azsdk_source_sample old = {0};
    REQUIRE(azsdk_sources_read(before, &old, sizeof old) == AZS_STALE, "real source helper marks the exact epoch stale");
    unavailable(query(&w.a));
    context_capture(&w.a);
    Born fresh BORN = birth_full(&w.a, 51, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &fresh);
    REQUIRE(fresh.witness.source_epoch != before, "retired source epoch cannot be revived by a cache hit");
}
static void receipt_revision_and_cold_cache(void) {
    reset();
    WORLD(w);
    Born known BORN = birth(&w.a);
    observe_query(&w.a, &known);
    uint64_t epoch = w.a.source.owner_epoch, receipt = w.a.source.revision;
    context_capture(&w.a);
    REQUIRE(w.a.source.owner_epoch == epoch && w.a.source.revision > receipt, "same actual source owners keep their epoch across a new receipt revision");
    observe_query(&w.a, &known);
    uint64_t next_birth = api()->counters().next_birth;
    CreatorOwner creator CREATOR = creator_new();
    owner_set(&creator->database, context_mount(&w.a)->database);
    creator->content = ((ListCache *) known.cache.object)->condition->content;
    GUARD_OWNER(output);
    BirthCall native_only = {output.bytes, cb_native_create, context_collector(&w.a), &creator};
    unsigned prior = creates;
    birth_call_forward(&native_only);
    GuardedOwner_check(&output);
    REQUIRE(creates == prior + 1, "cold native cache is genuinely created outside observer birth capture");
    REQUIRE(cb_native_owner_destination == (uintptr_t) output.bytes, "cold creation keeps original owner hidden destination");
    CacheOwner cold OWNED = *GuardedOwner_value(&output);
    *GuardedOwner_value(&output) = (CacheOwner) {0};
    ListCache *c = cold.object, *k = known.cache.object;
    REQUIRE(c && owner_count(cold) == 2 && c != k && c->condition->content == k->condition->content && c->condition->mode == k->condition->mode, "cold cache shares content/media/source but has a distinct real native owner");
    owner_set(context_selected(&w.a), cold);
    refused_unchanged(&w.a, &known, context_scope(&w.a), "cold same-content/media cache must decline unchanged output", (Action) {0}, (Action) {0});
    refused_unchanged(&w.a, &known, context_scope(&w.a), "repeated cold query cannot create or renew a birth witness", (Action) {0}, (Action) {0});
    REQUIRE(api()->counters().next_birth == next_birth, "query cannot allocate a missing cache birth epoch");
}
static void live_context_guard_refusals(void) {
    reset();
    WORLD(w);
    Born wrong_epoch BORN = birth_full(&w.a, 61, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &wrong_epoch);
    Scope scope = context_scope(&w.a);
    scope.source_epoch = w.b.source.owner_epoch;
    REQUIRE(scope.source_epoch != w.a.source.owner_epoch, "wrong epoch belongs to another real live source");
    refused_unchanged(&w.a, &wrong_epoch, scope, "wrong source_epoch with real a-context must decline unchanged output", (Action) {0}, (Action) {0});
    Born no_marker BORN = birth_full(&w.a, 62, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &no_marker);
    ((unsigned char *) &w.a.server)[0xd2] = 0;
    refused_unchanged(&w.a, &no_marker, context_scope(&w.a), "missing invocation marker on live receiver must decline observation", (Action) {0}, (Action) {0});
    ((unsigned char *) &w.a.server)[0xd2] = 1;
    Born bad_marker BORN = birth_full(&w.a, 63, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &bad_marker);
    ((unsigned char *) &w.a.server)[0xd2] = 2;
    refused_unchanged(&w.a, &bad_marker, context_scope(&w.a), "unsupported invocation marker on live receiver must decline observation", (Action) {0}, (Action) {0});
    ((unsigned char *) &w.a.server)[0xd2] = 1;
    Born wrong_worker BORN = birth_full(&w.a, 64, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &wrong_worker);
    w.a.server.worker_valid = false;
    refused_unchanged(&w.a, &wrong_worker, context_scope(&w.a), "worker-affinity refusal on live receiver still forwards original", (Action) {0}, (Action) {0});
    w.a.server.worker_valid = true;
}
struct TupleChange {
    Context *context;
    SqliteDataBase *database;
    void *connection;
    uint16_t db_media, mount_media;
    unsigned mask, changed;
    bool connection_change;
};
static void change_tuple(void *v) {
    struct TupleChange *t = v;
    ++t->changed;
    if (t->connection_change) t->database->connection = &t->context->alternate_connection;
    else {
        if (t->mask & 1) t->database->media = 0x401;
        if (t->mask & 2) context_mount(t->context)->media = 0x401;
    }
}
static void restore_tuple(void *v) {
    struct TupleChange *t = v;
    t->database->connection = t->connection;
    t->database->media = t->db_media;
    context_mount(t->context)->media = t->mount_media;
}
static void live_source_tuple_changes(void) {
    reset();
    WORLD(w);
    Born known BORN = birth(&w.a);
    observe_query(&w.a, &known);
    uint64_t epoch = w.a.source.owner_epoch, revision = w.a.source.revision;
    CacheOwner database OWNED = fixture_copy(context_mount(&w.a)->database);
    SqliteDataBase *db = database.object;
    struct TupleChange t = {&w.a, db, db->connection, db->media, context_mount(&w.a)->media, 0, 0, true};
    REQUIRE(t.connection != &w.a.alternate_connection && !((uintptr_t) &w.a.alternate_connection & 7u), "replacement connection is another aligned live fixture object");
    for (unsigned stage = 0; stage < 3; ++stage) {
        t.changed = 0;
        t.connection_change = true;
        if (!stage) change_tuple(&t);
        if (stage == 1) inside_query = (Action) {change_tuple, &t};
        refused_unchanged(&w.a, &known, context_scope(&w.a), "same live DB owner with a changed connection cannot qualify the old source epoch", stage == 2 ? (Action) {change_tuple, &t} : (Action) {0}, (Action) {restore_tuple, &t});
        inside_query = (Action) {0};
        REQUIRE(t.changed == 1, "connection changed at the intended real invocation boundary");
        observe_query(&w.a, &known);
        for (t.mask = 1; t.mask < 4; ++t.mask) {
            t.changed = 0;
            t.connection_change = false;
            if (!stage) change_tuple(&t);
            if (stage == 1) inside_query = (Action) {change_tuple, &t};
            refused_unchanged(&w.a, &known, context_scope(&w.a), "changed DB/Mount media tuple cannot qualify a same-owner old source epoch", stage == 2 ? (Action) {change_tuple, &t} : (Action) {0}, (Action) {restore_tuple, &t});
            inside_query = (Action) {0};
            REQUIRE(t.changed == 1, "media changed at the intended real invocation boundary");
            observe_query(&w.a, &known);
        }
    }
    REQUIRE(w.a.source.owner_epoch == epoch && w.a.source.revision == revision, "tuple refusal does not rely on another capture or a fabricated source entry");
}
struct BorrowChange {
    Context *first, *second;
    const Born *other;
    CacheOwner collector, selected, mount, database;
    unsigned kind, changed;
};
static void change_borrow(void *v) {
    struct BorrowChange *b = v;
    ++b->changed;
    switch (b->kind) {
    case 0: owner_set(&b->first->list.collector, b->second->list.collector); break;
    case 1: owner_set(context_selected(b->first), b->other->cache); break;
    case 2: owner_set(&b->first->list.mount, b->second->list.mount); break;
    case 3: owner_set(&context_mount(b->first)->database, context_mount(b->second)->database); break;
    }
}
static void restore_borrow(void *v) {
    struct BorrowChange *b = v;
    owner_set(&b->first->list.collector, b->collector);
    owner_set(context_selected(b->first), b->selected);
    owner_set(&b->first->list.mount, b->mount);
    owner_set(&context_mount(b->first)->database, b->database);
}
static void copy_boundary_borrow_changes(void) {
    reset();
    WORLD(w);
    Born known BORN = birth(&w.a), other BORN = birth(&w.b);
    observe_query(&w.a, &known);
    observe_query(&w.b, &other);
    uint64_t next_birth = api()->counters().next_birth;
    CacheOwner collector OWNED = fixture_copy(w.a.list.collector), selected OWNED = fixture_copy(*context_selected(&w.a)), mount OWNED = fixture_copy(w.a.list.mount), database OWNED = fixture_copy(context_mount(&w.a)->database);
    struct BorrowChange b = {&w.a, &w.b, &other, collector, selected, mount, database, 0, 0};
    for (b.kind = 0; b.kind < 4; ++b.kind) {
        b.changed = 0;
        refused_unchanged(&w.a, &known, context_scope(&w.a), "copier reentrancy changing a real cache/Collector/Mount/DB borrow must decline unchanged", (Action) {change_borrow, &b}, (Action) {restore_borrow, &b});
        REQUIRE(b.changed == 1, "borrow changes after the genuine native row scalar was read");
        observe_query(&w.a, &known);
    }
    REQUIRE(api()->counters().next_birth == next_birth, "borrow-change refusal cannot renew a cache birth");
}
static void source_disable_reenable(void) {
    reset();
    WORLD(w);
    Born old BORN = birth(&w.a);
    observe_query(&w.a, &old);
    uint64_t epoch = w.a.source.owner_epoch;
    REQUIRE(azsdk_sources_configure(0, (int) build_id, 0) == AZS_UNSUPPORTED, "real source disable retires bindings");
    refused_unchanged(&w.a, &old, context_scope(&w.a), "disabled source must decline unchanged while native query forwards", (Action) {0}, (Action) {0});
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "real source provider reenables without epoch reset");
    context_capture(&w.a);
    REQUIRE(w.a.source.owner_epoch != epoch, "reenabled identical real owners receive a fresh non-reused source epoch");
    struct azsdk_source_sample retired = {0};
    REQUIRE(azsdk_sources_read(epoch, &retired, sizeof retired) == AZS_STALE, "disable/reenable leaves the old source epoch retired");
    uint64_t before_hit = api()->counters().next_birth;
    CacheOwner hit OWNED = native_hit(&w.a, ((ListCache *) old.cache.object)->condition->content);
    REQUIRE(owners_equal(old.witness.cache, hit), "reenabled native condition lookup still returns the old retained cache owner");
    refused_unchanged(&w.a, &old, context_scope(&w.a), "reenabled source cannot renew an old native cache birth", (Action) {0}, (Action) {0});
    REQUIRE(api()->counters().next_birth == before_hit, "reenable/query cannot manufacture a new birth for an old hit");
    Born fresh BORN = birth_full(&w.a, 71, (Action) {0}, AZCB_PRIMARY_FACTORY, 0, 0);
    observe_query(&w.a, &fresh);
    REQUIRE(fresh.witness.source_epoch == w.a.source.owner_epoch && fresh.witness.source_epoch != epoch, "only an actual new birth can bind the new live source epoch");
}

#if defined(AZSDK_CACHE_RECLAIM_TESTING)
struct ReclaimRace {
    World *world;
    _Atomic(bool) active, finish;
    pthread_t closer;
};
static struct ReclaimRace *reclaim_race;
static int reclaim_close_original(void *v) {
    struct ReclaimRace *r = v;
    atomic_store_explicit(&r->active, true, memory_order_release);
    while (!atomic_load_explicit(&r->finish, memory_order_acquire)) sched_yield();
    return 0;
}
static void *reclaim_close(void *v) {
    struct ReclaimRace *r = v;
    az_connection_forward_close(&source_state().connection_epochs, &r->world->a.connection, reclaim_close_original, r);
    return NULL;
}
static void reclaim_boundary(unsigned boundary) {
    struct ReclaimRace *r = reclaim_race;
    if (!boundary) {
        REQUIRE(!pthread_create(&r->closer, NULL, reclaim_close, r), "start actual close race");
        while (!atomic_load_explicit(&r->active, memory_order_acquire)) sched_yield();
    } else {
        atomic_store_explicit(&r->finish, true, memory_order_release);
        REQUIRE(!pthread_join(r->closer, NULL), "complete actual close race");
    }
}
static void sql_cache_reclamation_isolation(void) {
    Limits limits = LIMITS_INIT;
    limits.slots = 1;
    reset_limits(limits);
    WORLD(w);
    Born retained BORN = birth(&w.b);
    observe_query(&w.b, &retained);
    struct ReclaimRace r = {.world = &w};
    reclaim_race = &r;
    cache_reclaim_test_boundary = reclaim_boundary;
    Born competing BORN = birth(&w.c);
    cache_reclaim_test_boundary = NULL;
    reclaim_race = NULL;
    REQUIRE(competing.status != AZS_OK && !competing.witness.birth_epoch, "receipt-only refusal cannot reclaim another live cache binding");
    observe_query(&w.b, &retained);
}
#endif

int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate process per build profile");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "existing source registry starts without physical policy");
    REQUIRE(api()->version == 1, "reviewed test-contract version");
    first_birth();
#if defined(AZSDK_CACHE_RECLAIM_TESTING)
    sql_cache_reclamation_isolation();
    puts("PASS cache-reclamation: unrelated lifecycle activity never evicts a still-owned binding");
    return 0;
#endif
    void (*cases[])(void) = {sql_connection_lifetime, sql_connection_isolation, hit_and_sources, native_exceptions, reentrant_birth, factory_cleanup_mutation, factory_write_receipts, mutation_guards, bounded_nonwrap, weak_lifetimes_and_reuse, actual_query_pins_and_output, existing_source_retirement, receipt_revision_and_cold_cache, live_context_guard_refusals, live_source_tuple_changes, copy_boundary_borrow_changes, source_disable_reenable};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        cases[i]();
        REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "only weak/copy records survive native callbacks");
    }
    REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "only weak/copy records survive native callbacks");
    puts("PASS cache-birth: C native owners, exact hidden destinations, native rows, all source lifecycle and retirement scenarios");
    return 0;
}

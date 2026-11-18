/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CACHE_BIRTH_FIXTURE_H
#define CACHE_BIRTH_FIXTURE_H
/* Independent C models of retained native layouts. Ownership controls and
 * native result thunks belong to the fixture, not to the implementation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_abi_fixture.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include "../../src/common/azcachebirth.h"
#define REQUIRE(x, why)                                                                         \
    do {                                                                                        \
        if (!(x)) {                                                                             \
            fprintf(stderr, "FAIL cache-birth: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                            \
        }                                                                                       \
    } while (0)
typedef struct fixture_owner CacheOwner;
typedef struct cbf_action {
    void (*call)(void *);
    void *context;
} Action;
static inline void action_run(Action a) {
    if (a.call) a.call(a.context);
}
static inline void owner_set(CacheOwner *dst, CacheOwner src) {
    CacheOwner copy = fixture_copy(src);
    fixture_drop(dst);
    *dst = copy;
}
static inline void weak_set(CacheOwner *dst, CacheOwner src) {
    CacheOwner copy = fixture_weak(src);
    fixture_weak_drop(dst);
    *dst = copy;
}
static inline unsigned owner_count(CacheOwner p) { return p.control ? (unsigned) __atomic_load_n(&p.control->strong, __ATOMIC_ACQUIRE) : 0; }
#define OWNED __attribute__((cleanup(fixture_drop)))
#define WEAK __attribute__((cleanup(fixture_weak_drop)))
#define NATIVE_OWNER(p) ((const struct az_native_owner *) (p))
typedef struct SqliteDataBase {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x38 - 10];
    void *connection;
    uint32_t fixture_layout;
} SqliteDataBase;
typedef struct SqliteMountPoint {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x30 - 10];
    CacheOwner database;
} SqliteMountPoint;
typedef SqliteMountPoint MountPoint;
typedef struct InformationUpdater {
    const void *vtable;
    CacheOwner mount;
} InformationUpdater;
typedef struct MusicLibraryServer {
    const void *vtable;
    unsigned char pad[0x1d0 - 8];
    pthread_t worker;
    bool worker_valid;
} MusicLibraryServer;
static bool cbf_needs_async(void *v) {
    MusicLibraryServer *s = v;
    return !s->worker_valid || !pthread_equal(s->worker, pthread_self());
}
struct cbf_type {
    const void *vtable;
    const char *name;
};
struct cbf_table {
    uintptr_t offset;
    const struct cbf_type *type;
    const void *slots[11];
};
static const struct cbf_type cbf_db_type = {NULL, "N13music_library14SqliteDataBaseE"}, cbf_mount_type = {NULL, "N13music_library16SqliteMountPointE"}, cbf_updater_type = {NULL, "N13music_library18InformationUpdaterE"}, cbf_server_type = {NULL, "N13music_library18MusicLibraryServerE"};
static const struct cbf_table cbf_db_table = {0, &cbf_db_type, {0}}, cbf_mount_table = {0, &cbf_mount_type, {0}}, cbf_updater_table = {0, &cbf_updater_type, {0}}, cbf_server_table = {0, &cbf_server_type, {[5] = cbf_needs_async}};
FIXTURE_VTABLE(cbf_db_control, fixture_noop);
static void cbf_mount_dispose(void *v) {
    SqliteMountPoint *m = (void *) ((struct fixture_control *) v + 1);
    fixture_drop(&m->database);
}
FIXTURE_VTABLE(cbf_mount_control, cbf_mount_dispose);
static inline CacheOwner cbf_mount_new(uint16_t media, void *connection, unsigned layout) {
    CacheOwner db = fixture_make(sizeof(SqliteDataBase), cbf_db_control);
    *(SqliteDataBase *) db.object = (SqliteDataBase) {.vtable = cbf_db_table.slots, .media = media, .connection = connection, .fixture_layout = layout};
    CacheOwner mount = fixture_make(sizeof(SqliteMountPoint), cbf_mount_control);
    *(SqliteMountPoint *) mount.object = (SqliteMountPoint) {.vtable = cbf_mount_table.slots, .media = media, .database = db};
    return mount;
}
typedef struct OwnerVector {
    CacheOwner *begin, *end, *capacity;
} OwnerVector;
typedef struct ClientVector {
    OwnerVector *begin, *end, *capacity;
} ClientVector;
static inline size_t owner_vector_size(const OwnerVector *v) { return v->begin ? (size_t) (v->end - v->begin) : 0; }
static inline void owner_vector_clear(OwnerVector *v) {
    for (CacheOwner *p = v->begin; p != v->end; ++p) fixture_drop(p);
    v->end = v->begin;
}
static inline void owner_vector_drop(OwnerVector *v) {
    owner_vector_clear(v);
    free(v->begin);
    *v = (OwnerVector) {0};
}
static inline void owner_vector_push(OwnerVector *v, CacheOwner owner) {
    size_t n = owner_vector_size(v), capacity = v->begin ? (size_t) (v->capacity - v->begin) : 0;
    if (n == capacity) {
        capacity = capacity ? capacity * 2 : 4;
        CacheOwner *items = realloc(v->begin, capacity * sizeof *items);
        REQUIRE(items, "native fixture vector growth");
        v->begin = items;
        v->end = items + n;
        v->capacity = items + capacity;
    }
    *v->end++ = fixture_copy(owner);
}
static inline void clients_drop(ClientVector *v) {
    for (OwnerVector *p = v->begin; p != v->end; ++p) owner_vector_drop(p);
    free(v->begin);
    *v = (ClientVector) {0};
}
static inline void clients_init(ClientVector *v) {
    *v = (ClientVector) {0};
    v->begin = calloc(4, sizeof *v->begin);
    REQUIRE(v->begin, "native client vector allocation");
    v->end = v->capacity = v->begin + 4;
    for (unsigned i = 0; i < 4; ++i) {
        OwnerVector *p = v->begin + i;
        p->begin = calloc(9, sizeof *p->begin);
        REQUIRE(p->begin, "native selection vector allocation");
        p->end = p->capacity = p->begin + 9;
    }
}
typedef struct FixturePool {
    unsigned unused;
} FixturePool;
typedef struct FixtureCondition {
    unsigned content;
    unsigned char mode;
} FixtureCondition;
typedef struct FixtureLayout {
    unsigned tag;
    FixturePool *borrowed_pool;
} FixtureLayout;
typedef struct ListCache {
    const void *vtable;
    uint32_t item_count, pad0;
    FixtureCondition *condition;
    CacheOwner temporary_database;
    uint32_t native_counter, pad1;
    FixtureLayout *layout;
} ListCache;
typedef struct ListCacheCreator {
    const void *vtable;
    unsigned char pad[0x30 - 8];
    CacheOwner database;
    uint32_t content;
    unsigned char mode, qualifier;
    Action inside_make;
    unsigned factory_kind, fault, forwarded_calls;
    uintptr_t result_destination;
} ListCacheCreator;
typedef ListCacheCreator *CreatorOwner;
typedef struct ListCacheCollector {
    const void *vtable;
    unsigned char pad[0x28 - 8];
    OwnerVector caches;
    FixturePool *pool;
    uint32_t native_counter, pad1;
} ListCacheCollector;
typedef struct ListServer {
    const void *vtable;
    unsigned char pad[0x28 - 8];
    CacheOwner mount;
    uint64_t schema_pad;
    CacheOwner collector;
    ClientVector clients;
} ListServer;
typedef struct FixtureAdapter {
    const void *vtable;
    ListCache *cache;
    uintptr_t result_destination;
} FixtureAdapter;
static unsigned cache_alive, collector_alive, pool_alive, row_alive, row_destroyed;
static bool cache_reuse_next, collector_reuse_next;
static _Alignas(ListCache) unsigned char cache_arena[sizeof(ListCache)];
static _Alignas(ListCacheCollector) unsigned char collector_arena[sizeof(ListCacheCollector)];
static const void *const cbf_object_slots[3] = {fixture_noop, fixture_noop, fixture_noop};
static inline void cache_delete(ListCache *p) {
    if (!p) return;
    --cache_alive;
    free(p->condition);
    fixture_drop(&p->temporary_database);
    free(p->layout);
    if (p != (void *) cache_arena) free(p);
}
static inline void cache_ptr_drop(ListCache **p) {
    cache_delete(*p);
    *p = NULL;
}
static inline ListCache *cache_new(unsigned content, FixturePool *pool, unsigned tag) {
    ListCache *p = cache_reuse_next ? (void *) cache_arena : malloc(sizeof *p);
    REQUIRE(p, "cache allocation");
    cache_reuse_next = false;
    *p = (ListCache) {.vtable = cbf_object_slots, .item_count = 16};
    p->condition = calloc(1, sizeof *p->condition);
    p->layout = calloc(1, sizeof *p->layout);
    REQUIRE(p->condition && p->layout, "cache owned fields");
    *p->condition = (FixtureCondition) {content, 0};
    *p->layout = (FixtureLayout) {tag, pool};
    ++cache_alive;
    return p;
}
static inline void cache_setup(ListCache *p, unsigned count, FixturePool *pool) {
    p->item_count = count;
    free(p->layout);
    p->layout = calloc(1, sizeof *p->layout);
    REQUIRE(p->layout, "setup layout allocation");
    p->layout->borrowed_pool = pool;
}
static void cbf_cache_dispose(void *v) {
    ListCache **p = (void *) ((struct fixture_control *) v + 1);
    cache_delete(*p);
}
FIXTURE_VTABLE(cbf_cache_control, cbf_cache_dispose);
static inline CacheOwner cbf_cache_owner(ListCache *p) {
    CacheOwner owner = fixture_make(sizeof p, cbf_cache_control);
    *(ListCache **) owner.object = p;
    owner.object = p;
    return owner;
}
static void cbf_collector_dispose(void *v) {
    ListCacheCollector *p = *(ListCacheCollector **) ((struct fixture_control *) v + 1);
    owner_vector_drop(&p->caches);
    free(p->pool);
    --pool_alive;
    --collector_alive;
    if (p != (void *) collector_arena) free(p);
}
FIXTURE_VTABLE(cbf_collector_control, cbf_collector_dispose);
static inline CacheOwner collector_new(void) {
    ListCacheCollector *p = collector_reuse_next ? (void *) collector_arena : malloc(sizeof *p);
    REQUIRE(p, "collector allocation");
    collector_reuse_next = false;
    *p = (ListCacheCollector) {.vtable = cbf_object_slots, .pool = calloc(1, sizeof(FixturePool))};
    REQUIRE(p->pool, "collector pool allocation");
    ++collector_alive;
    ++pool_alive;
    CacheOwner owner = fixture_make(sizeof p, cbf_collector_control);
    *(ListCacheCollector **) owner.object = p;
    owner.object = p;
    return owner;
}
static inline CreatorOwner creator_new(void) {
    CreatorOwner p = calloc(1, sizeof *p);
    REQUIRE(p, "creator allocation");
    p->vtable = cbf_object_slots;
    p->content = 11;
    p->factory_kind = AZCB_PRIMARY_FACTORY;
    return p;
}
static inline void creator_drop(CreatorOwner *p) {
    if (*p) {
        fixture_drop(&(*p)->database);
        free(*p);
        *p = NULL;
    }
}
#define CREATOR __attribute__((cleanup(creator_drop)))
static inline void list_init(ListServer *p) {
    *p = (ListServer) {.vtable = cbf_object_slots, .collector = collector_new()};
    clients_init(&p->clients);
}
static inline void list_drop(ListServer *p) {
    clients_drop(&p->clients);
    fixture_drop(&p->collector);
    fixture_drop(&p->mount);
}
static Action factory_tail, inside_final_write, after_owner, inside_query;
typedef void (*FactoryEntry)(const struct azcb_factory_borrow *, struct azcb_forward);
static FactoryEntry factory_entry;
struct cbf_write {
    ListCacheCreator *creator;
    ListCache *cache;
    FixturePool *pool;
    unsigned kind, argument, index;
    int result;
};
static void cbf_write_original(void *v) {
    struct cbf_write *w = v;
    ListCacheCreator *c = w->creator;
    if (w->kind == AZCB_SETUP_WRITE) {
        cache_setup(w->cache, w->argument, w->pool);
        w->result = c->fault == 2 ? 7 : 0;
    } else {
        w->cache->layout->tag = ((SqliteDataBase *) c->database.object)->fixture_layout;
        if (w->index == 15) action_run(inside_final_write);
        w->result = c->fault == 6 && w->index == 7 ? 5 : 0;
    }
}
static inline void cbf_write(ListCacheCreator *c, ListCache *cache, unsigned kind, unsigned argument, unsigned index, FixturePool *pool) {
    struct cbf_write w = {c, cache, pool, kind, argument, index, -1};
    struct azcb_write_borrow b = {cache, kind, argument, pool, &w.result};
    azcb_observe_construction_write_owned(&b, (struct azcb_forward) {cbf_write_original, &w});
}
struct cbf_factory {
    ListCacheCreator *creator;
    FixturePool *pool;
    const void *result;
};
static void cbf_factory_original(void *v) {
    struct cbf_factory *f = v;
    ListCacheCreator *c = f->creator;
    action_run(c->inside_make);
    ListCache *value __attribute__((cleanup(cache_ptr_drop))) = cache_new(c->content, f->pool, ((SqliteDataBase *) c->database.object)->fixture_layout);
    if (c->factory_kind == AZCB_STANDARD_FALLBACK_FACTORY) {
        cbf_write(c, value, AZCB_SETUP_WRITE, c->fault == 1 ? 15 : 16, 0, f->pool);
        unsigned setters = c->fault == 4 ? 15 : c->fault == 5 ? 17
                                                              : 16;
        for (unsigned index = 0; index < setters; ++index) cbf_write(c, value, AZCB_LAYOUT_WRITE, c->fault == 3 && index == 9 ? 8 : index, index, NULL);
    }
    action_run(factory_tail);
    f->result = value;
    value = NULL;
}
static inline ListCache *creator_make(ListCacheCreator *c, FixturePool *pool) {
    struct cbf_factory f = {c, pool, NULL};
    struct azcb_factory_borrow b = {c, c->factory_kind, pool, &f.result};
    struct azcb_forward original = {cbf_factory_original, &f};
    if (factory_entry) factory_entry(&b, original);
    else azcb_observe_factory_owned(&b, original);
    return (ListCache *) f.result;
}
typedef struct NativeRow {
    uint32_t value;
    unsigned char pad[0x98 - 4];
} NativeRow;
typedef struct NativeRows {
    NativeRow **begin, **end, **capacity;
} NativeRows;
static inline void rows_init(NativeRows *rows, unsigned value) {
    *rows = (NativeRows) {0};
    rows->begin = calloc(1, sizeof *rows->begin);
    REQUIRE(rows->begin, "native row vector");
    rows->begin[0] = calloc(1, sizeof(NativeRow));
    REQUIRE(rows->begin[0], "native row allocation");
    rows->begin[0]->value = value;
    rows->end = rows->capacity = rows->begin + 1;
    ++row_alive;
}
static inline void rows_drop(NativeRows *rows) {
    for (NativeRow **p = rows->begin; p != rows->end; ++p) {
        free(*p);
        --row_alive;
        ++row_destroyed;
    }
    free(rows->begin);
    *rows = (NativeRows) {0};
}
_Static_assert(sizeof(CacheOwner) == 16 && sizeof(NativeRows) == 24 && sizeof(NativeRow) == 0x98, "native owned result shapes");
_Static_assert(offsetof(ListCacheCreator, database) == 0x30 && offsetof(ListCacheCreator, mode) == 0x44, "native creator fields");
_Static_assert(offsetof(ListCacheCollector, caches) == 0x28 && offsetof(ListCacheCollector, pool) == 0x40, "native collector fields");
_Static_assert(offsetof(ListServer, mount) == 0x28 && offsetof(ListServer, collector) == 0x40 && offsetof(ListServer, clients) == 0x50, "native selected storage");
/* These names identify ABI thunks, never C small-struct result prototypes. */
typedef void (*NativeCreate)(void);
typedef void (*NativeQuery)(void);
void cb_native_create(void);
void cb_native_query(void);
void cb_forward_owner(void *, NativeCreate, ListCacheCollector *, CreatorOwner *);
void cb_forward_rows(void *, NativeQuery, FixtureAdapter *, unsigned, unsigned, const CacheOwner *);
uintptr_t cb_native_owner_destination, cb_native_rows_destination;
static unsigned build_id, creates, queries;
void cb_native_create_impl(ListCacheCollector *collector, CreatorOwner *creator, CacheOwner *out) {
    ++creates;
    ++(*creator)->forwarded_calls;
    (*creator)->result_destination = cb_native_owner_destination;
    CacheOwner result OWNED = cbf_cache_owner(creator_make(*creator, collector->pool));
    action_run(after_owner);
    ((ListCache *) result.object)->native_counter = ++collector->native_counter;
    owner_vector_push(&collector->caches, result);
    *out = result;
    result = (CacheOwner) {0};
}
void cb_native_query_impl(FixtureAdapter *adapter, unsigned start, unsigned count, const CacheOwner *mount, NativeRows *out) {
    ++queries;
    REQUIRE(start == 0 && count == 1 && mount->object && adapter->cache, "genuine native query arguments");
    adapter->result_destination = cb_native_rows_destination;
    REQUIRE(pool_alive && adapter->cache->layout->borrowed_pool, "native owned query keeps actual pool alive");
    action_run(inside_query);
    rows_init(out, adapter->cache->layout->tag);
}
/* Invented TEST ONLY contract. The adapter delegates all provenance decisions
 * to the included production implementation and has no replacement registry. */
typedef struct Scope {
    unsigned build, client_bank;
    MusicLibraryServer *owned_server;
    ListServer *list_server;
    uint64_t source_epoch;
} Scope;
typedef struct Witness {
    uint64_t birth_epoch, layout_generation, source_epoch;
    CacheOwner mount, database, collector, cache;
    unsigned row_value;
} Witness;
static inline void witness_drop(Witness *w) {
    fixture_weak_drop(&w->cache);
    fixture_weak_drop(&w->collector);
    fixture_weak_drop(&w->database);
    fixture_weak_drop(&w->mount);
}
static inline void witness_copy(Witness *dst, const Witness *src) {
    if (dst == src) return;
    weak_set(&dst->mount, src->mount);
    weak_set(&dst->database, src->database);
    weak_set(&dst->collector, src->collector);
    weak_set(&dst->cache, src->cache);
    dst->birth_epoch = src->birth_epoch;
    dst->layout_generation = src->layout_generation;
    dst->source_epoch = src->source_epoch;
    dst->row_value = src->row_value;
}
#define WITNESS __attribute__((cleanup(witness_drop)))
typedef struct BirthCall {
    void *result_storage;
    NativeCreate original;
    ListCacheCollector *collector;
    CreatorOwner *creator;
} BirthCall;
typedef struct QueryCall {
    void *result_storage;
    NativeQuery original;
    FixtureAdapter *adapter;
    unsigned start, count;
    const CacheOwner *mount;
    void (*copy_boundary)(void *);
    void *copy_context;
} QueryCall;
static inline void birth_call_forward(const BirthCall *c) { cb_forward_owner(c->result_storage, c->original, c->collector, c->creator); }
static inline void query_call_forward(const QueryCall *c) { cb_forward_rows(c->result_storage, c->original, c->adapter, c->start, c->count, c->mount); }
typedef enum Mutation {
    setup,
    setter,
    records_grow,
    clear,
    dispose
} Mutation;
typedef struct Limits {
    unsigned slots, depth;
    uint64_t next_birth, generation;
} Limits;
#define LIMITS_INIT {4, 4, 0, 1}
typedef struct Counters {
    unsigned retained, active;
    uint64_t next_birth, generation;
    bool saturated;
} Counters;
typedef struct Api {
    unsigned version;
    void (*reset)(const Limits *);
    int (*birth)(const Scope *, const BirthCall *, Witness *);
    int (*query)(const Scope *, const QueryCall *, Witness *);
    void (*mutation)(Mutation, void (*)(void *), void *);
    Counters (*counters)(void);
} Api;
const Api *azsdk_cache_birth_test_api_v1(void) __attribute__((weak));
void azsdk_cache_test_connection_event(void *, int) __attribute__((weak));
static inline void native_connection_event(void *connection, int open) {
    if (azsdk_cache_test_connection_event) azsdk_cache_test_connection_event(connection, open);
}
#endif

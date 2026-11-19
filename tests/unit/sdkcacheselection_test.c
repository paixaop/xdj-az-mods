/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C ownership and vector layout fixtures. The production resolver
 * supplies every selection/fence decision; the absent path only forwards. */
#include "native_abi_fixture.h"
#include <stdio.h>
#include <pthread.h>
#include "../../src/common/aznative_c.h"
#define REQUIRE(x, why)                                                                             \
    do {                                                                                            \
        if (!(x)) {                                                                                 \
            fprintf(stderr, "FAIL cache-selection: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                                \
        }                                                                                           \
    } while (0)
#ifdef AZSDK_HAVE_CACHE_SELECTION
#include "../../src/common/azcacheselection.h"
#endif
struct Vector {
    void *begin, *end, *capacity;
};
struct Life {
    bool alive;
};
struct Pool {
    struct fixture_owner life;
};
struct Database {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x38 - 10];
    void *connection;
};
struct Mount {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x30 - 10];
    struct fixture_owner database;
};
struct Cache {
    unsigned value;
    struct fixture_owner pool_life;
};
struct Collector {
    const void *vtable;
    unsigned char pad[0x28 - 8];
    struct Vector caches;
    struct Pool *pool;
    uint64_t tail;
};
struct Root {
    const void *vtable;
    unsigned char pad[0x28 - 8];
    struct fixture_owner mount;
    uint64_t gap;
    struct fixture_owner collector;
    struct Vector clients;
};
static unsigned cache_alive, pool_alive, profile;
static bool cache_arena_next, cache_arena_live, root_arena_next, root_arena_live;
static _Alignas(8) unsigned char cache_arena[sizeof(struct Cache)], root_arena[sizeof(struct Root)];
static const char supported_mount_type, base_mount_type;
FIXTURE_VTABLE(life_vtable, fixture_noop);
FIXTURE_VTABLE(database_vtable, fixture_noop);
static void owner_set(struct fixture_owner *p, struct fixture_owner value) {
    struct fixture_owner copy = fixture_copy(value);
    fixture_drop(p);
    *p = copy;
}
static void weak_set(struct fixture_owner *p, struct fixture_owner value) {
    struct fixture_owner copy = fixture_weak(value);
    fixture_weak_drop(p);
    *p = copy;
}
static size_t vector_count(struct Vector v, size_t stride) { return v.begin ? ((uintptr_t) v.end - (uintptr_t) v.begin) / stride : 0; }
static void vector_reserve(struct Vector *v, size_t capacity, size_t stride) {
    size_t n = vector_count(*v, stride);
    void *p = calloc(capacity, stride);
    REQUIRE(p, "fixture vector allocation");
    if (n) memcpy(p, v->begin, n * stride);
    free(v->begin);
    *v = (struct Vector) {p, (char *) p + n * stride, (char *) p + capacity * stride};
}
static void owners_clear(struct Vector *v) {
    for (struct fixture_owner *p = v->begin; p != (struct fixture_owner *) v->end; ++p) fixture_drop(p);
    v->end = v->begin;
}
static void owner_append(struct Vector *v, struct fixture_owner owner) {
    size_t n = vector_count(*v, 16), capacity = v->begin ? ((uintptr_t) v->capacity - (uintptr_t) v->begin) / 16 : 0;
    if (n == capacity) vector_reserve(v, n + 8, 16);
    ((struct fixture_owner *) v->begin)[n] = fixture_copy(owner);
    v->end = (char *) v->begin + (n + 1) * 16;
}
static void mount_dispose(void *control) {
    struct Mount *m = (void *) ((struct fixture_control *) control + 1);
    fixture_drop(&m->database);
}
FIXTURE_VTABLE(mount_vtable, mount_dispose);
static struct fixture_owner mount_new(void *connection, bool supported) {
    struct fixture_owner owner = fixture_make(sizeof(struct Mount), mount_vtable);
    struct Mount *m = owner.object;
    m->vtable = supported ? &supported_mount_type : &base_mount_type;
    m->media = 0x301;
    if (supported) {
        m->database = fixture_make(sizeof(struct Database), database_vtable);
        struct Database *d = m->database.object;
        d->media = 0x301;
        d->connection = connection;
    }
    return owner;
}
/* Separate control and pointee allocations exercise actual address reuse while
 * old weak controls still exist. Disposal always precedes control destruction. */
struct RawControl {
    struct fixture_control base;
    void *object;
};
static void cache_dispose(void *control) {
    struct Cache *c = ((struct RawControl *) control)->object;
    struct fixture_owner life = fixture_lock(c->pool_life);
    REQUIRE(life.object && ((struct Life *) life.object)->alive, "cache destruction precedes actual owning Collector pool destruction");
    fixture_drop(&life);
    fixture_weak_drop(&c->pool_life);
    --cache_alive;
    if ((void *) c == cache_arena) cache_arena_live = false;
    else free(c);
}
FIXTURE_VTABLE(cache_vtable, cache_dispose);
static struct fixture_owner raw_owner(void *object, const void *const *vtable) {
    struct RawControl *c = calloc(1, sizeof *c);
    REQUIRE(c, "raw control allocation");
    c->base = (struct fixture_control) {vtable, 1, 1};
    c->object = object;
    return (struct fixture_owner) {object, &c->base};
}
static struct fixture_owner cache_new(unsigned value, struct Pool *pool) {
    struct Cache *c;
    if (cache_arena_next) {
        REQUIRE(!cache_arena_live, "raw cache address cannot be reused while real scoped pin still owns it");
        cache_arena_next = false;
        cache_arena_live = true;
        c = (void *) cache_arena;
        memset(c, 0, sizeof *c);
    } else {
        c = calloc(1, sizeof *c);
        REQUIRE(c, "cache allocation");
    }
    c->value = value;
    c->pool_life = fixture_weak(pool->life);
    ++cache_alive;
    return raw_owner(c, cache_vtable);
}
static void collector_dispose(void *control) {
    struct Collector *c = (void *) ((struct fixture_control *) control + 1);
    owners_clear(&c->caches);
    free(c->caches.begin);
    ((struct Life *) c->pool->life.object)->alive = false;
    fixture_drop(&c->pool->life);
    free(c->pool);
    --pool_alive;
}
FIXTURE_VTABLE(collector_vtable, collector_dispose);
static struct fixture_owner collector_new(void) {
    struct fixture_owner owner = fixture_make(sizeof(struct Collector), collector_vtable);
    struct Collector *c = owner.object;
    c->pool = calloc(1, sizeof *c->pool);
    REQUIRE(c->pool, "pool allocation");
    c->pool->life = fixture_make(sizeof(struct Life), life_vtable);
    ((struct Life *) c->pool->life.object)->alive = true;
    ++pool_alive;
    return owner;
}
static void root_dispose(void *control) {
    struct Root *r = ((struct RawControl *) control)->object;
    for (struct Vector *v = r->clients.begin; v != (struct Vector *) r->clients.end; v++) {
        owners_clear(v);
        free(v->begin);
    }
    free(r->clients.begin);
    fixture_drop(&r->collector);
    fixture_drop(&r->mount);
    if ((void *) r == root_arena) root_arena_live = false;
    else free(r);
}
FIXTURE_VTABLE(root_vtable, root_dispose);
static struct fixture_owner root_new(void) {
    struct Root *r;
    if (root_arena_next) {
        REQUIRE(!root_arena_live, "only an actually destroyed native root can reuse its fixture address");
        root_arena_next = false;
        root_arena_live = true;
        r = (void *) root_arena;
        memset(r, 0, sizeof *r);
    } else {
        r = calloc(1, sizeof *r);
        REQUIRE(r, "root allocation");
    }
    r->collector = collector_new();
    vector_reserve(&r->clients, 4, 24);
    r->clients.end = (char *) r->clients.begin + 4 * 24;
    for (unsigned i = 0; i < 4; i++) {
        struct Vector *v = &((struct Vector *) r->clients.begin)[i];
        vector_reserve(v, 9, 16);
        v->end = (char *) v->begin + 9 * 16;
    }
    return raw_owner(r, root_vtable);
}
struct Position {
    uint32_t outer, selector;
};
struct Witness {
    uint64_t invocation;
    struct fixture_owner mount, database, collector, cache;
    uint32_t value;
};
static void witness_drop(struct Witness *w) {
    fixture_weak_drop(&w->cache);
    fixture_weak_drop(&w->collector);
    fixture_weak_drop(&w->database);
    fixture_weak_drop(&w->mount);
}
static void witness_copy(struct Witness *d, const struct Witness *s) {
    if (d == s) return;
    weak_set(&d->mount, s->mount);
    weak_set(&d->database, s->database);
    weak_set(&d->collector, s->collector);
    weak_set(&d->cache, s->cache);
    d->invocation = s->invocation;
    d->value = s->value;
}
struct State {
#ifdef AZSDK_HAVE_CACHE_SELECTION
    struct azsel_state fence;
#else
    unsigned unused;
#endif
};
static void state_init(struct State *s) {
#ifdef AZSDK_HAVE_CACHE_SELECTION
    azsel_state_init(&s->fence, 4, 1, 0);
#else
    *s = (struct State) {0};
#endif
}
static unsigned active(const struct State *s) {
#ifdef AZSDK_HAVE_CACHE_SELECTION
    return atomic_load_explicit(&s->fence.active, memory_order_acquire);
#else
    (void) s;
    return 0;
#endif
}
struct Context {
    struct State *state;
    struct fixture_owner root;
    struct Position position;
    unsigned tag, marker, caller_depth;
    bool mount_supported, worker_valid;
    pthread_t worker;
    uint64_t connection;
};
static struct Root *root(struct Context *c) { return c->root.object; }
static struct fixture_owner *database(struct Context *c) { return &((struct Mount *) root(c)->mount.object)->database; }
static struct fixture_owner *selected(struct Context *c) {
    struct Vector *v = &((struct Vector *) root(c)->clients.begin)[c->position.outer];
    return &((struct fixture_owner *) v->begin)[c->position.selector - 1];
}
static void context_add(struct Context *c, struct Position p, unsigned value) {
    struct Collector *collector = root(c)->collector.object;
    struct fixture_owner owner = cache_new(value, collector->pool);
    owner_append(&collector->caches, owner);
    struct Vector *v = &((struct Vector *) root(c)->clients.begin)[p.outer];
    owner_set(&((struct fixture_owner *) v->begin)[p.selector - 1], owner);
    fixture_drop(&owner);
}
static void context_rebuild(struct Context *c) {
    fixture_drop(&c->root);
    c->root = root_new();
    root(c)->mount = mount_new(&c->connection, true);
    context_add(c, c->position, c->tag);
}
static void context_init(struct Context *c, struct State *s, unsigned outer, unsigned selector, unsigned value) {
    *c = (struct Context) {.state = s, .position = {outer, selector}, .tag = value, .marker = 1, .mount_supported = true, .worker_valid = true, .worker = pthread_self()};
    context_rebuild(c);
}
struct World {
    struct State state;
    struct Context a, b, c, d;
};
static void world_init(struct World *w) {
    memset(w, 0, sizeof *w);
    state_init(&w->state);
    context_init(&w->a, &w->state, 0, 1, 101);
    context_init(&w->b, &w->state, 3, 9, 202);
    context_init(&w->c, &w->state, 1, 4, 303);
    context_init(&w->d, &w->state, 2, 7, 404);
}
static void world_drop(struct World *w) {
    fixture_drop(&w->d.root);
    fixture_drop(&w->c.root);
    fixture_drop(&w->b.root);
    fixture_drop(&w->a.root);
}
#define WORLD                                            \
    struct World w __attribute__((cleanup(world_drop))); \
    world_init(&w)
#ifdef AZSDK_HAVE_CACHE_SELECTION
static bool owned(void *opaque) {
    struct Context *c = opaque;
    return c->root.object && c->caller_depth && c->marker == 1 && c->worker_valid && pthread_equal(c->worker, pthread_self());
}
static bool sqlite(void *opaque, const void *mount) {
    struct Context *c = opaque;
    return c->mount_supported && ((const struct Mount *) mount)->vtable == &supported_mount_type;
}
#endif
struct Call;
typedef void (*Action)(struct Call *, void *);
struct Call {
    struct Context *context;
    struct Root *original_root;
    struct Position position;
    uint64_t before;
    uint32_t value;
    uint64_t after;
    unsigned forwards, copies;
    Action inside_original, inside_copy;
    void *action_context;
};
static void call_init(struct Call *c, struct Context *context, struct Position pos) { *c = (struct Call) {.context = context, .original_root = root(context), .position = pos, .before = UINT64_C(0x123456789abcdef0), .after = UINT64_C(0xfedcba9876543210)}; }
static void call_check(struct Call *c) {
    REQUIRE(c->before == UINT64_C(0x123456789abcdef0) && c->after == UINT64_C(0xfedcba9876543210), "actual native output guards remain intact");
    REQUIRE(c->forwards == 1, "resolver forwards real native query exactly once, including native unwind");
}
static unsigned native_query(struct Root *r, struct Position p) {
    if (!r || p.outer >= vector_count(r->clients, 24) || !p.selector || p.selector > 9) return 0;
    struct Vector v = ((struct Vector *) r->clients.begin)[p.outer];
    if (p.selector > vector_count(v, 16)) return 0;
    struct fixture_owner owner = ((struct fixture_owner *) v.begin)[p.selector - 1];
    return owner.object ? ((struct Cache *) owner.object)->value : 0;
}
static void forward(struct Call *c) {
    ++c->forwards;
    unsigned value = native_query(c->original_root, c->position);
    if (c->inside_original) c->inside_original(c, c->action_context);
    c->value = value;
}
struct Caller {
    struct Call *call;
};
static void caller_drop(struct Caller *c) {
    --c->call->context->caller_depth;
    call_check(c->call);
}
#ifdef AZSDK_HAVE_CACHE_SELECTION
struct Copy {
    struct Call *call;
    uint32_t value;
};
static void copy_call(void *opaque) {
    struct Copy *copy = opaque;
    struct Call *c = copy->call;
    ++c->copies;
    REQUIRE(c->forwards == 1, "scalar copied only after genuine original forwarding");
    copy->value = c->value;
    if (c->inside_copy) c->inside_copy(c, c->action_context);
}
static struct fixture_owner fixture_view(struct az_native_owner p) {
    struct fixture_owner o;
    _Static_assert(sizeof o == sizeof p, "owner ABI");
    memcpy(&o, &p, sizeof o);
    return o;
}
#endif
static int observe(struct Call *c, struct Witness *out) {
    struct Caller caller __attribute__((cleanup(caller_drop))) = {c};
    ++c->context->caller_depth;
#ifdef AZSDK_HAVE_CACHE_SELECTION
    struct azsel_invocation invocation __attribute__((cleanup(azsel_invocation_drop)));
    azsel_invocation_init(&invocation, &c->context->state->fence);
    struct azsel_borrow b = {profile, root(c->context), c->context, owned, sqlite};
    struct azsel_position p = {c->position.outer, c->position.selector};
    struct azsel_resolution r __attribute__((cleanup(azsel_resolution_drop))) = {0};
    azsel_resolve_owned(&invocation, &b, p, &r);
    forward(c);
    if (r.status != AZSEL_OK) return r.status;
    if (!azsel_validate_fresh(&invocation, &b, p, &r)) return AZSEL_CHANGED;
    struct Copy copy = {c, 0};
    if (!fixture_native_try(copy_call, &copy)) return AZSEL_GUARD;
    if (!azsel_validate_fresh(&invocation, &b, p, &r)) return AZSEL_CHANGED;
    if (out) {
        struct Witness result __attribute__((cleanup(witness_drop))) = {.invocation = r.invocation, .value = copy.value};
        result.mount = fixture_weak(fixture_view(r.pins.mount));
        result.database = fixture_weak(fixture_view(r.pins.database));
        result.collector = fixture_weak(fixture_view(r.pins.collector));
        result.cache = fixture_weak(fixture_view(r.pins.cache));
        witness_copy(out, &result);
    }
    return AZSEL_OK;
#else
    (void) out;
    forward(c);
    return -1;
#endif
}
static struct Witness sentinel(struct Context *c) { return (struct Witness) {UINT64_C(0x1020304050607080), fixture_weak(root(c)->mount), fixture_weak(*database(c)), fixture_weak(root(c)->collector), fixture_weak(*selected(c)), 0xcafe}; }
static bool unchanged(const struct Witness *a, const struct Witness *b) { return a->invocation == b->invocation && a->value == b->value && a->mount.control == b->mount.control && a->database.control == b->database.control && a->collector.control == b->collector.control && a->cache.control == b->cache.control; }
static bool same(struct fixture_owner weak, struct fixture_owner owner) {
    struct fixture_owner pin = fixture_lock(weak);
    bool result = weak.control == owner.control && pin.object == owner.object;
    fixture_drop(&pin);
    return result;
}
static struct Witness observed(struct Context *c) {
    struct Call call;
    call_init(&call, c, c->position);
    struct Witness w = {0};
    int mc = root(c)->mount.control->strong, dc = database(c)->control->strong, cc = root(c)->collector.control->strong, kc = selected(c)->control->strong;
    int status = observe(&call, &w);
    printf("NATIVE_SELECTION_FORWARD_OK build=%u shared16=%zu vector24=%zu forwards=%u value=%u\n", profile, sizeof(struct fixture_owner), sizeof(struct Vector), call.forwards, call.value);
    fflush(stdout);
    REQUIRE(!status && w.invocation && same(w.mount, root(c)->mount) && same(w.database, *database(c)) && same(w.collector, root(c)->collector) && same(w.cache, *selected(c)), "missing fresh selected-owner pins and real ownership result");
    REQUIRE(call.copies == 1 && w.value == c->tag && w.value == call.value, "actual native selected-cache scalar copied once inside owned callback");
    REQUIRE(root(c)->mount.control->strong == mc && database(c)->control->strong == dc && root(c)->collector.control->strong == cc && selected(c)->control->strong == kc, "all strong source/cache/Collector pins end with native callback");
    REQUIRE(!active(c->state), "caller-owned invocation state leaves no active frame");
    return w;
}
static void observed_drop(struct Context *c) {
    struct Witness w = observed(c);
    witness_drop(&w);
}
static void refused(struct Call *c, const struct Witness *before) {
    struct Witness out __attribute__((cleanup(witness_drop))) = {0};
    witness_copy(&out, before);
    unsigned prior = active(c->context->state);
    REQUIRE(observe(c, &out) != 0 && unchanged(&out, before), "refusal leaves every existing weak/scalar output unchanged");
    REQUIRE(active(c->context->state) == prior, "refusal restores prior actual invocation depth");
}
static void first_observation(void) {
    WORLD;
    struct Context *contexts[] = {&w.a, &w.b, &w.c, &w.d};
    for (unsigned i = 0; i < 4; i++) observed_drop(contexts[i]);
}
static void positions_and_guards(void) {
    WORLD;
    for (unsigned i = 1; i <= 9; i++) {
        w.a.position.selector = i;
        context_add(&w.a, w.a.position, w.a.tag);
        observed_drop(&w.a);
    }
    struct Witness before __attribute__((cleanup(witness_drop))) = sentinel(&w.a);
    const struct Position bad[] = {{4, 1}, {UINT32_MAX, 1}, {0, 0}, {0, 10}, {0, 257}};
    for (unsigned i = 0; i < 5; i++) {
        struct Call c;
        call_init(&c, &w.a, bad[i]);
        refused(&c, &before);
    }
    struct Vector *v = root(&w.a)->clients.begin;
    for (unsigned i = 1; i < 9; i++) fixture_drop(&((struct fixture_owner *) v[0].begin)[i]);
    v[0].end = (char *) v[0].begin + 16;
    struct Call c;
    call_init(&c, &w.a, (struct Position) {0, 9});
    refused(&c, &before);
    w.a.position.selector = 1;
    struct fixture_owner owner __attribute__((cleanup(fixture_drop))) = fixture_copy(*selected(&w.a));
    fixture_drop(selected(&w.a));
    call_init(&c, &w.a, w.a.position);
    refused(&c, &before);
    owner_set(selected(&w.a), owner);
    for (unsigned kind = 0; kind < 3; kind++) {
        if (!kind) w.a.marker = 0;
        if (kind == 1) w.a.worker_valid = false;
        if (kind == 2) w.a.mount_supported = false;
        call_init(&c, &w.a, w.a.position);
        refused(&c, &before);
        w.a.marker = 1;
        w.a.worker_valid = w.a.mount_supported = true;
    }
    struct fixture_owner supported __attribute__((cleanup(fixture_drop))) = fixture_copy(root(&w.a)->mount);
    fixture_drop(&root(&w.a)->mount);
    root(&w.a)->mount = mount_new(NULL, false);
    call_init(&c, &w.a, w.a.position);
    refused(&c, &before);
    owner_set(&root(&w.a)->mount, supported);
}
struct Change {
    struct World *world;
    unsigned kind;
    bool realloc;
};
static void change_owner(struct Call *call, void *opaque) {
    (void) call;
    struct Change *x = opaque;
    struct Context *a = &x->world->a, *b = &x->world->b;
    if (x->realloc) {
        struct Vector *outer = &root(a)->clients;
        uintptr_t old_outer = (uintptr_t) outer->begin, old_inner = (uintptr_t) ((struct Vector *) outer->begin)[0].begin;
        vector_reserve(outer, vector_count(*outer, 24) + 7, 24);
        struct Vector *inner = outer->begin;
        vector_reserve(inner, vector_count(*inner, 16) + 11, 16);
        REQUIRE((uintptr_t) outer->begin != old_outer && (uintptr_t) inner->begin != old_inner, "real outer and inner vector storage reallocated");
    }
    if (x->kind == 0) owner_set(&root(a)->mount, root(b)->mount);
    if (x->kind == 1) owner_set(database(a), *database(b));
    if (x->kind == 2) owner_set(&root(a)->collector, root(b)->collector);
    if (x->kind == 3) owner_set(selected(a), *selected(b));
}
static void reallocation_and_actual_owners(void) {
    WORLD;
    struct Witness before __attribute__((cleanup(witness_drop))) = sentinel(&w.a);
    for (unsigned stage = 0; stage < 2; stage++) {
        struct fixture_owner saved __attribute__((cleanup(fixture_drop))) = fixture_copy(*selected(&w.a));
        struct Change x = {&w, 3, true};
        struct Call c;
        call_init(&c, &w.a, w.a.position);
        c.action_context = &x;
        if (!stage) c.inside_original = change_owner;
        else c.inside_copy = change_owner;
        refused(&c, &before);
        owner_set(selected(&w.a), saved);
        observed_drop(&w.a);
    }
    for (unsigned stage = 0; stage < 2; stage++)
        for (unsigned kind = 0; kind < 4; kind++) {
            struct fixture_owner m = fixture_copy(root(&w.a)->mount), d = fixture_copy(*database(&w.a)), collector = fixture_copy(root(&w.a)->collector), cache = fixture_copy(*selected(&w.a));
            struct Change x = {&w, kind, false};
            struct Call c;
            call_init(&c, &w.a, w.a.position);
            c.action_context = &x;
            if (!stage) c.inside_original = change_owner;
            else c.inside_copy = change_owner;
            refused(&c, &before);
            owner_set(&root(&w.a)->mount, m);
            owner_set(database(&w.a), d);
            owner_set(&root(&w.a)->collector, collector);
            owner_set(selected(&w.a), cache);
            fixture_drop(&cache);
            fixture_drop(&collector);
            fixture_drop(&d);
            fixture_drop(&m);
            observed_drop(&w.a);
        }
}
static void event(struct Call *c, unsigned kind, Action action, void *context) {
#ifdef AZSDK_HAVE_CACHE_SELECTION
    struct azsel_mutation mutation __attribute__((cleanup(azsel_mutation_drop)));
    azsel_mutation_init(&mutation, &c->context->state->fence, kind);
#else
    (void) kind;
#endif
    action(c, context);
}
struct Taint {
    struct World *world;
    const struct Witness *before;
    struct fixture_owner selected;
    unsigned mode;
};
static void taint_action(struct Call *c, void *opaque) {
    struct Taint *t = opaque;
    if (t->mode == 0) {
        owner_set(selected(&t->world->a), *selected(&t->world->b));
        owner_set(selected(&t->world->a), t->selected);
    } else if (t->mode == 1) {
        struct Call nested;
        call_init(&nested, c->context, c->position);
        refused(&nested, t->before);
    } else fixture_raise();
}
static void taint(struct Call *c, void *opaque) { event(c, 1, taint_action, opaque); }
struct ObserveCall {
    struct Call *call;
    struct Witness *out;
};
static void observe_call(void *opaque) {
    struct ObserveCall *c = opaque;
    (void) observe(c->call, c->out);
}
static void positive_taint_and_nesting(void) {
    WORLD;
    struct Witness before __attribute__((cleanup(witness_drop))) = sentinel(&w.a);
    struct fixture_owner saved __attribute__((cleanup(fixture_drop))) = fixture_copy(*selected(&w.a));
    struct Taint t = {&w, &before, saved, 0};
    for (unsigned stage = 0; stage < 2; stage++) {
        struct Call c;
        call_init(&c, &w.a, w.a.position);
        c.action_context = &t;
        if (!stage) c.inside_original = taint;
        else c.inside_copy = taint;
        refused(&c, &before);
        observed_drop(&w.a);
    }
    t.mode = 1;
    struct Call c;
    call_init(&c, &w.a, w.a.position);
    c.inside_original = taint;
    c.action_context = &t;
    refused(&c, &before);
    observed_drop(&w.a);
    t.mode = 2;
    call_init(&c, &w.a, w.a.position);
    c.inside_original = taint;
    c.action_context = &t;
    struct Witness out __attribute__((cleanup(witness_drop))) = {0};
    witness_copy(&out, &before);
    struct ObserveCall oc = {&c, &out};
    REQUIRE(!fixture_native_try(observe_call, &oc), "native unwind must propagate");
    REQUIRE(unchanged(&out, &before) && !active(&w.state), "native unwind restores embedded fence state and leaves output unchanged");
    observed_drop(&w.a);
}
static void revoke_action(struct Call *c, void *opaque) {
    struct Witness *before = opaque;
    c->context->marker = 0;
    fixture_drop(&c->context->root);
    REQUIRE(!fixture_expired(before->cache) && !fixture_expired(before->collector) && !fixture_expired(before->mount) && !fixture_expired(before->database), "actual scoped pins retain cache AND Collector pool plus Mount/DB during native callback");
}
static void revoke(struct Call *c, void *opaque) { event(c, 2, revoke_action, opaque); }
static void clear_caches(struct Context *c) {
    fixture_drop(selected(c));
    owners_clear(&((struct Collector *) root(c)->collector.object)->caches);
}
static void lifecycle_pins_and_reuse(void) {
    WORLD;
    struct Witness before __attribute__((cleanup(witness_drop))) = sentinel(&w.a);
    struct Call c;
    call_init(&c, &w.a, w.a.position);
    c.inside_original = revoke;
    c.action_context = &before;
    refused(&c, &before);
    REQUIRE(fixture_expired(before.cache) && fixture_expired(before.collector) && fixture_expired(before.mount) && fixture_expired(before.database) && pool_alive == 3, "only weak owners survive lost callback; cache drops before Collector pool");
    w.a.marker = 1;
    context_rebuild(&w.a);
    observed_drop(&w.a);
    clear_caches(&w.a);
    cache_arena_next = true;
    context_add(&w.a, w.a.position, w.a.tag);
    struct Witness old __attribute__((cleanup(witness_drop))) = observed(&w.a);
    void *address = selected(&w.a)->object;
    clear_caches(&w.a);
    REQUIRE(fixture_expired(old.cache) && !cache_arena_live, "old actual cache destroyed before address reuse");
    cache_arena_next = true;
    context_add(&w.a, w.a.position, w.a.tag);
    struct Witness fresh __attribute__((cleanup(witness_drop))) = observed(&w.a);
    REQUIRE(selected(&w.a)->object == address && old.cache.control != fresh.cache.control, "same object address with new real GNU control is a different selection owner");
}
struct Alias {
    struct fixture_owner collector, cache;
};
static void alias_dispose(void *control) {
    struct Alias *a = (void *) ((struct fixture_control *) control + 1);
    fixture_drop(&a->cache);
    fixture_drop(&a->collector);
}
FIXTURE_VTABLE(alias_vtable, alias_dispose);
static void alias_change(struct Call *c, void *opaque) { owner_set(selected(c->context), *(struct fixture_owner *) opaque); }
static void root_reuse_action(struct Call *c, void *opaque) {
    struct Context *a = c->context;
    void *address = opaque;
    struct fixture_owner m = fixture_copy(root(a)->mount), collector = fixture_copy(root(a)->collector), cache = fixture_copy(*selected(a));
    fixture_drop(&a->root);
    REQUIRE(!root_arena_live, "old complete root actually destroyed");
    root_arena_next = true;
    context_rebuild(a);
    clear_caches(a);
    owner_set(&root(a)->mount, m);
    owner_set(&root(a)->collector, collector);
    owner_set(selected(a), cache);
    fixture_drop(&cache);
    fixture_drop(&collector);
    fixture_drop(&m);
    REQUIRE(root(a) == address, "new complete object at exact former root address");
}
static void root_reuse(struct Call *c, void *opaque) { event(c, 2, root_reuse_action, opaque); }
static void exact_alias_owner_and_root_aba(void) {
    WORLD;
    struct Witness before __attribute__((cleanup(witness_drop))) = sentinel(&w.a);
    struct fixture_owner retained __attribute__((cleanup(fixture_drop))) = fixture_make(sizeof(struct Alias), alias_vtable);
    struct Alias *a = retained.object;
    a->cache = fixture_copy(*selected(&w.a));
    a->collector = fixture_copy(root(&w.a)->collector);
    struct fixture_owner alias __attribute__((cleanup(fixture_drop))) = fixture_copy(retained);
    alias.object = a->cache.object;
    struct Call c;
    call_init(&c, &w.a, w.a.position);
    c.inside_original = alias_change;
    c.action_context = &alias;
    refused(&c, &before);
    owner_set(selected(&w.a), a->cache);
    fixture_drop(&w.a.root);
    root_arena_next = true;
    context_rebuild(&w.a);
    witness_drop(&before);
    before = sentinel(&w.a);
    void *address = root(&w.a);
    call_init(&c, &w.a, w.a.position);
    c.inside_original = root_reuse;
    c.action_context = address;
    refused(&c, &before);
    observed_drop(&w.a);
}
int main(int argc, char **argv) {
#ifdef AZSDK_HAVE_CACHE_SELECTION
    struct azsel_vector empty = {0};
    uintptr_t count = 123;
    REQUIRE(!azsel_count(&empty, 0, &count) && count == 123, "zero element width refuses without output");
    REQUIRE(azsel_count(&empty, 16, &count) && !count, "canonical empty vector contains zero owners");
    empty.end = 16;
    REQUIRE(!azsel_count(&empty, 16, &count), "empty storage cannot have a nonempty end");
    empty.end = 0;
    empty.capacity = 16;
    REQUIRE(!azsel_count(&empty, 16, &count), "empty storage cannot retain a capacity address");
#endif
    _Static_assert(sizeof(struct fixture_owner) == 16 && sizeof(struct Vector) == 24, "native owner and vector widths");
    _Static_assert(offsetof(struct Root, mount) == 0x28 && offsetof(struct Root, collector) == 0x40 && offsetof(struct Root, clients) == 0x50 && offsetof(struct Mount, database) == 0x30 && offsetof(struct Database, connection) == 0x38, "owned native root/Mount/DB prefixes");
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate process per supplied build profile");
    profile = (unsigned) (argv[1][0] - '0');
    first_observation();
    void (*tests[])(void) = {positions_and_guards, reallocation_and_actual_owners, positive_taint_and_nesting, lifecycle_pins_and_reuse, exact_alias_owner_and_root_aba};
    for (unsigned i = 0; i < 5; i++) {
        tests[i]();
        REQUIRE(!cache_alive && !pool_alive && !cache_arena_live && !root_arena_live, "native fixture lifetimes end; no resolver strong owner escapes callback");
    }
    printf("PASS cache-selection build=%u: real GNU owners, fresh vector storage, positive invocation fences; no native hook ABI claim\n", profile);
}

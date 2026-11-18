/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C owner/holder layouts; no native source or entry admission. */
#include "native_abi_fixture.h"
#include "../../src/common/azcachebirth.h"
#include "../../src/common/azsources.h"
#include <stdio.h>
#ifndef AZCACHE_PIN_HEADER
#define AZCACHE_PIN_HEADER "../../src/common/azcachepin.h"
#endif
#if __has_include(AZCACHE_PIN_HEADER)
#include AZCACHE_PIN_HEADER
#define HAVE_CACHE_PIN 1
#endif
#define CHECK(x, why)                                     \
    do {                                                  \
        if (!(x)) {                                       \
            fprintf(stderr, "FAIL cache-pin: %s\n", why); \
            abort();                                      \
        }                                                 \
    } while (0)
static unsigned pools, collectors, caches, creators, databases, mounts;
static char cleanup[1024];
static size_t cleanup_count;
static void record(char c) {
    CHECK(cleanup_count < sizeof cleanup, "cleanup record quota");
    cleanup[cleanup_count++] = c;
}
struct Pool {
    unsigned marker;
};
struct DB {
    unsigned identity;
};
struct Collector {
    const void *vtable;
    unsigned char prefix[0x40 - 8];
    struct Pool *pool;
};
struct Cache {
    struct Pool *pool;
};
struct Creator {
    const void *vtable;
    unsigned char prefix[0x30 - 8];
    struct fixture_owner database;
};
static struct Pool *pool_new(void) {
    struct Pool *p = calloc(1, sizeof *p);
    CHECK(p, "pool allocation");
    ++pools;
    return p;
}
static void pool_drop(struct Pool *p) {
    if (p) {
        --pools;
        record('P');
        free(p);
    }
}
static void db_dispose(void *p) {
    (void) p;
    --databases;
}
static void mount_dispose(void *p) {
    (void) p;
    --mounts;
}
static void collector_dispose(void *p) {
    struct Collector *c = (void *) ((struct fixture_control *) p + 1);
    --collectors;
    record('C');
    pool_drop(c->pool);
}
static void cache_dispose(void *p) {
    struct Cache *c = (void *) ((struct fixture_control *) p + 1);
    CHECK(!c->pool || pools, "native caller cleans cache while its pool still lives");
    --caches;
    record('R');
}
FIXTURE_VTABLE(db_vtable, db_dispose);
FIXTURE_VTABLE(mount_vtable, mount_dispose);
FIXTURE_VTABLE(collector_vtable, collector_dispose);
FIXTURE_VTABLE(cache_vtable, cache_dispose);
FIXTURE_VTABLE(borrow_vtable, fixture_noop);
static struct fixture_owner mount_new(void) {
    ++mounts;
    return fixture_make(1, mount_vtable);
}
static struct fixture_owner db_new(unsigned id) {
    struct fixture_owner o = fixture_make(sizeof(struct DB), db_vtable);
    ((struct DB *) o.object)->identity = id;
    ++databases;
    return o;
}
static struct fixture_owner collector_new(void) {
    struct fixture_owner o = fixture_make(sizeof(struct Collector), collector_vtable);
    ((struct Collector *) o.object)->pool = pool_new();
    ++collectors;
    return o;
}
static struct fixture_owner cache_new(struct Pool *pool) {
    struct fixture_owner o = fixture_make(sizeof(struct Cache), cache_vtable);
    ((struct Cache *) o.object)->pool = pool;
    ++caches;
    return o;
}
static struct Creator *creator_new(struct fixture_owner db) {
    struct Creator *c = calloc(1, sizeof *c);
    CHECK(c, "creator allocation");
    c->database = fixture_copy(db);
    ++creators;
    return c;
}
static void creator_drop(struct Creator *c) {
    if (c) {
        fixture_drop(&c->database);
        --creators;
        free(c);
    }
}
static void owner_set(struct fixture_owner *p, struct fixture_owner value) {
    struct fixture_owner next = fixture_copy(value);
    fixture_drop(p);
    *p = next;
}
static const struct az_native_owner *view(const struct fixture_owner *p) { return (const struct az_native_owner *) p; }
struct Context {
    int server;
    unsigned build;
    uint64_t epoch;
    struct fixture_owner *mount, *database, *collector;
    struct Creator **creator;
};
static void context_holders(struct Context *c, unsigned index) {
    c->mount = malloc(sizeof *c->mount);
    c->database = malloc(sizeof *c->database);
    c->collector = malloc(sizeof *c->collector);
    c->creator = malloc(sizeof *c->creator);
    CHECK(c->mount && c->database && c->collector && c->creator, "real native holder allocation");
    *c->mount = mount_new();
    *c->database = db_new(index);
    *c->collector = collector_new();
    *c->creator = creator_new(*c->database);
}
static void context_init(struct Context *c, unsigned build, unsigned index) {
    *c = (struct Context) {.server = (int) index, .build = build, .epoch = 100 + index};
    context_holders(c, index);
}
static void context_drop(struct Context *c) {
    if (c->creator) {
        creator_drop(*c->creator);
        free(c->creator);
        c->creator = NULL;
    }
    if (c->database) {
        fixture_drop(c->database);
        free(c->database);
        c->database = NULL;
    }
    if (c->mount) {
        fixture_drop(c->mount);
        free(c->mount);
        c->mount = NULL;
    }
    if (c->collector) {
        fixture_drop(c->collector);
        free(c->collector);
        c->collector = NULL;
    }
}
static struct azcb_source_borrow source(struct Context *c) { return (struct azcb_source_borrow) {&c->server, c->build, c->epoch, view(c->mount), view(c->database)}; }
struct Result {
    uint64_t before;
    struct fixture_owner owner;
    uint64_t after;
    bool initialized;
};
static void result_init(struct Result *r) { *r = (struct Result) {.before = UINT64_C(0xcafed00d), .after = UINT64_C(0xabcdef01)}; }
static void result_guards(const struct Result *r) { CHECK(r->before == UINT64_C(0xcafed00d) && r->after == UINT64_C(0xabcdef01), "genuine result guards unchanged"); }
static void result_clear(struct Result *r) {
    if (r->initialized) {
        fixture_drop(&r->owner);
        r->initialized = false;
    }
}
typedef int (*Observer)(const struct azcb_birth_borrow *, struct azcb_forward, void *);
static int forward(const struct azcb_source_borrow *s, const struct fixture_owner *collector, const void *receiver, const void *creator, struct Result *r, struct azcb_forward original, Observer observe, void *context, bool missing) {
#ifdef HAVE_CACHE_PIN
    return azcp_forward_owned(s, view(collector), receiver, creator, missing ? NULL : view(&r->owner), original, observe, context);
#else
    (void) s;
    (void) collector;
    (void) receiver;
    (void) creator;
    (void) r;
    (void) observe;
    (void) context;
    (void) missing;
    if (!original.call) return AZS_INVALID;
    original.call(original.context);
    return AZS_UNSUPPORTED;
#endif
}
static bool same(const struct az_native_owner *a, struct fixture_owner weak) {
    struct fixture_owner pin = fixture_lock(weak);
    bool result = a->object == pin.object && (void *) a->control == (void *) pin.control;
    fixture_drop(&pin);
    return result;
}
struct Native {
    struct Context *context;
    struct Result *result;
    struct Collector *receiver;
    struct Pool *pool, *moved_pool;
    unsigned originals, observed, index;
    bool release, replace_pool, throwing;
    struct azcb_source_borrow source;
    struct Creator *creator;
    struct fixture_owner mount, database, collector;
    int status;
};
static void original(void *opaque) {
    struct Native *n = opaque;
    ++n->originals;
    if (n->throwing) {
        if (n->release) context_drop(n->context);
        fixture_raise();
    }
    n->result->owner = cache_new(n->pool);
    n->result->initialized = true;
    if (n->replace_pool) {
        n->moved_pool = n->receiver->pool;
        n->receiver->pool = pool_new();
    }
    if (n->release) context_drop(n->context);
    if (n->replace_pool) context_holders(n->context, n->index + 100);
}
static int observer(const struct azcb_birth_borrow *b, struct azcb_forward work, void *opaque) {
    struct Native *n = opaque;
    ++n->observed;
    CHECK(b->source.server == n->source.server && b->source.build == n->context->build && b->source.source_epoch == n->source.source_epoch, "source callback/profile/epoch scalars preserved without admission claim");
    CHECK(b->source.mount != n->source.mount && b->source.database != n->source.database && b->collector != view(n->context->collector) && b->creator_database != view(&n->creator->database), "all owner holder storage is local before native forwarding");
    CHECK(same(b->source.mount, n->mount) && same(b->source.database, n->database) && same(b->creator_database, n->database) && same(b->collector, n->collector) && b->receiver_collector == n->receiver && b->receiver_creator == n->creator && b->borrowed_pool == n->pool && b->native_result == view(&n->result->owner), "exact real owner aliases and original result destination");
    work.call(work.context);
    result_guards(n->result);
    CHECK(!fixture_expired(n->mount) && !fixture_expired(n->database) && !fixture_expired(n->collector) && same(b->source.mount, n->mount) && same(b->creator_database, n->database) && b->collector->object == n->receiver && b->borrowed_pool == n->pool && b->native_result->object, "local snapshots survive released/replaced native holders without post-native raw reads");
    result_clear(n->result);
    return AZS_OK;
}
static void pin_call(void *opaque) {
    struct Native *n = opaque;
    n->status = forward(&n->source, n->context->collector, n->receiver, n->context->creator, n->result, (struct azcb_forward) {original, n}, observer, n, false);
}
static void native_baseline(void) {
    struct Context c __attribute__((cleanup(context_drop)));
    context_init(&c, 1, 0);
    struct Result r __attribute__((cleanup(result_clear)));
    result_init(&r);
    struct Native n = {.context = &c, .result = &r, .pool = ((struct Collector *) c.collector->object)->pool};
    original(&n);
    result_guards(&r);
    CHECK(n.originals == 1 && r.owner.control->strong == 1 && caches == 1, "actual caller-owned GNU result baseline");
    result_clear(&r);
    CHECK(!caches, "only native result caller destroys returned owner");
    puts("NATIVE_GNU_PIN_BASELINE_OK owner16=16 creator_holder8=8");
    fflush(stdout);
}
static void pinned(unsigned build, unsigned index, bool release, bool replace_pool, bool throwing) {
    struct Context c __attribute__((cleanup(context_drop)));
    context_init(&c, build, index);
    struct Result r __attribute__((cleanup(result_clear)));
    result_init(&r);
    size_t begin = cleanup_count;
    struct Native n = {.context = &c, .result = &r, .receiver = c.collector->object, .pool = ((struct Collector *) c.collector->object)->pool, .index = index, .release = release, .replace_pool = replace_pool, .throwing = throwing, .source = source(&c), .creator = *c.creator, .mount = fixture_weak(*c.mount), .database = fixture_weak(*c.database), .collector = fixture_weak(*c.collector), .status = AZS_UNSUPPORTED};
    unsigned deletions = fixture_exception_deletions;
    bool caught = !fixture_native_try(pin_call, &n);
    result_guards(&r);
    CHECK(n.originals == 1 && n.observed == 1 && (throwing ? caught : n.status == AZS_OK), "missing observed stable cache/source ownership pins");
    CHECK(caught == throwing && !r.initialized && fixture_exception_deletions == deletions + (throwing ? 1u : 0u), "native unwind/result cleanup is untouched");
    if (release) CHECK(fixture_expired(n.mount) && fixture_expired(n.database) && fixture_expired(n.collector), "adapter retains no strong owners after callback");
    if (release && !throwing) {
        size_t row = cleanup_count, collect = cleanup_count;
        for (size_t i = begin; i < cleanup_count; i++) {
            if (cleanup[i] == 'R' && row == cleanup_count) row = i;
            if (cleanup[i] == 'C' && collect == cleanup_count) collect = i;
        }
        CHECK(row < collect && collect < cleanup_count, "caller result cleanup precedes last Collector/pool pin release");
    }
    pool_drop(n.moved_pool);
    fixture_weak_drop(&n.collector);
    fixture_weak_drop(&n.database);
    fixture_weak_drop(&n.mount);
}
static int unexpected(const struct azcb_birth_borrow *b, struct azcb_forward f, void *opaque) {
    (void) b;
    (void) f;
    ++*(unsigned *) opaque;
    return AZS_OK;
}
static void refused(unsigned kind) {
    struct Context c __attribute__((cleanup(context_drop))), other __attribute__((cleanup(context_drop)));
    context_init(&c, 1, kind + 20);
    context_init(&other, 2, kind + 40);
    struct Result r __attribute__((cleanup(result_clear)));
    result_init(&r);
    struct azcb_source_borrow s = source(&c);
    const struct fixture_owner *collector = c.collector;
    const void *receiver = c.collector->object, *creator = c.creator;
    switch (kind) {
    case 0: s.mount = NULL; break;
    case 1: s.database = NULL; break;
    case 2: collector = NULL; break;
    case 3: receiver = other.collector->object; break;
    case 4: creator = NULL; break;
    case 5:
        creator_drop(*c.creator);
        *c.creator = NULL;
        break;
    case 6: fixture_drop(&(*c.creator)->database); break;
    case 7: owner_set(&(*c.creator)->database, *other.database); break;
    case 8:
        owner_set(&(*c.creator)->database, *c.database);
        (*c.creator)->database.object = other.database->object;
        break;
    case 9:
        fixture_drop(&(*c.creator)->database);
        (*c.creator)->database = fixture_make(0, borrow_vtable);
        (*c.creator)->database.object = c.database->object;
        break;
    case 10:
        pool_drop(((struct Collector *) c.collector->object)->pool);
        ((struct Collector *) c.collector->object)->pool = NULL;
        break;
    case 11: s.source_epoch = 0; break;
    case 12: s.server = NULL; break;
    case 13: fixture_drop(c.collector); break;
    case 14: fixture_drop(c.mount); break;
    case 15: fixture_drop(c.database); break;
    case 16: break;
    }
    struct Native n = {.context = &c, .result = &r};
    unsigned observed = 0;
    int status = forward(&s, collector, receiver, creator, &r, (struct azcb_forward) {original, &n}, unexpected, &observed, kind == 16);
    result_guards(&r);
    CHECK(status != AZS_OK && n.originals == 1 && !observed && r.owner.control->strong == 1, "null/mismatched real inputs decline observation while native result forwards once");
    result_clear(&r);
}
struct Nested {
    struct Context *outer, *inner;
    struct Result *a, *b;
    unsigned originals, observed;
};
static void inner_original(void *opaque) {
    struct Nested *n = opaque;
    ++n->originals;
    n->b->owner = cache_new(((struct Collector *) n->inner->collector->object)->pool);
    n->b->initialized = true;
}
static int inner_observe(const struct azcb_birth_borrow *b, struct azcb_forward work, void *opaque) {
    struct Nested *n = opaque;
    ++n->observed;
    CHECK(b->source.source_epoch == n->inner->epoch, "nested source is independent");
    work.call(work.context);
    result_clear(n->b);
    return AZS_OK;
}
static void outer_original(void *opaque) {
    struct Nested *n = opaque;
    ++n->originals;
    struct azcb_source_borrow s = source(n->inner);
    int status = forward(&s, n->inner->collector, n->inner->collector->object, n->inner->creator, n->b, (struct azcb_forward) {inner_original, n}, inner_observe, n, false);
    CHECK(status == AZS_OK, "nested helper forwarded actual native result");
    n->a->owner = cache_new(((struct Collector *) n->outer->collector->object)->pool);
    n->a->initialized = true;
}
static int outer_observe(const struct azcb_birth_borrow *b, struct azcb_forward work, void *opaque) {
    struct Nested *n = opaque;
    ++n->observed;
    struct az_native_owner database = {0};
    az_native_owner_copy(&database, b->creator_database);
    work.call(work.context);
    CHECK(b->source.source_epoch == n->outer->epoch && b->creator_database->object == database.object && b->creator_database->control == database.control, "reentry never overwrites outer local holders");
    result_clear(n->a);
    az_native_owner_reset(&database);
    return AZS_OK;
}
static void nested(unsigned build) {
    struct Context outer __attribute__((cleanup(context_drop))), inner __attribute__((cleanup(context_drop)));
    context_init(&outer, build, 70);
    context_init(&inner, build, 71);
    struct Result a __attribute__((cleanup(result_clear))), b __attribute__((cleanup(result_clear)));
    result_init(&a);
    result_init(&b);
    struct Nested n = {&outer, &inner, &a, &b, 0, 0};
    struct azcb_source_borrow s = source(&outer);
    int status = forward(&s, outer.collector, outer.collector->object, outer.creator, &a, (struct azcb_forward) {outer_original, &n}, outer_observe, &n, false);
    CHECK(status == AZS_OK && n.originals == 2 && n.observed == 2, "independent nested forwarding calls exactly once");
}
static void no_original(void) {
    struct Context c __attribute__((cleanup(context_drop)));
    context_init(&c, 1, 90);
    struct Result r __attribute__((cleanup(result_clear)));
    result_init(&r);
    unsigned observed = 0;
    struct azcb_source_borrow s = source(&c);
    int status = forward(&s, c.collector, c.collector->object, c.creator, &r, (struct azcb_forward) {NULL, NULL}, unexpected, &observed, false);
    CHECK(status == AZS_INVALID && !observed && !r.initialized, "null original cannot create observed work or touch output");
    result_guards(&r);
}
int main(void) {
    _Static_assert(sizeof(struct fixture_owner) == 16 && sizeof(struct Creator *) == 8, "native owner/Creator-holder widths");
    _Static_assert(offsetof(struct Collector, pool) == 0x40 && offsetof(struct Creator, database) == 0x30, "reviewed Collector pool and Creator DB prefixes");
    native_baseline();
    for (unsigned build = 1; build <= 2; build++)
        for (unsigned context = 0; context < 4; context++) {
            pinned(build, context, false, false, false);
            pinned(build, context, true, false, false);
            pinned(build, context, true, true, false);
            pinned(build, context, true, false, true);
        }
    for (unsigned kind = 0; kind < 17; kind++) refused(kind);
    nested(1);
    nested(2);
    no_original();
    CHECK(!pools && !collectors && !caches && !creators && !databases && !mounts, "no pin owner/result leaks");
    puts("PASS cache-pin: C local owners; holder replacement, raw result lifetime, native unwind and reentry; four contexts/both profiles");
}

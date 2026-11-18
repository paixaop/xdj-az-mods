/* SPDX-License-Identifier: GPL-3.0-only */
/* Connection bookkeeping only: native forwarding uses independent C callbacks
 * and counted-owner ABI fixtures. No database, key, hook or device is accessed. */
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "native_abi_fixture.h"
#define REQUIRE(x, why)                                                                              \
    do {                                                                                             \
        if (!(x)) {                                                                                  \
            fprintf(stderr, "FAIL connection-epoch: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                                 \
        }                                                                                            \
    } while (0)
#ifndef AZCONNECTION_EPOCH_HEADER
#define AZCONNECTION_EPOCH_HEADER "../../src/common/azconnectionepoch.h"
#endif
#if __has_include(AZCONNECTION_EPOCH_HEADER)
#include AZCONNECTION_EPOCH_HEADER
#define EPOCH_IMPLEMENTATION_PRESENT 1
#else
#define EPOCH_IMPLEMENTATION_PRESENT 0
struct az_connection_receipt {
    uint64_t generation;
};
struct az_connection_state {
    unsigned unused;
};
#define AZ_CONNECTION_STATE_INIT {0}
#define AZ_CONNECTION_STATE_BOUNDS(a, b, c, d) {0}
static void az_connection_state_drop(struct az_connection_state *s) { (void) s; }
static int az_connection_forward_open(struct az_connection_state *s, const void *out, int (*f)(void *), void *ctx, struct az_connection_receipt *r) {
    (void) s;
    (void) out;
    (void) r;
    return f(ctx);
}
static int az_connection_forward_close(struct az_connection_state *s, const void *connection, int (*f)(void *), void *ctx) {
    (void) s;
    (void) connection;
    return f(ctx);
}
static bool az_connection_read(struct az_connection_state *s, const void *p, struct az_connection_receipt *r) {
    (void) s;
    (void) p;
    (void) r;
    return false;
}
#endif
#define STATE(name) struct az_connection_state name __attribute__((cleanup(az_connection_state_drop))) = AZ_CONNECTION_STATE_INIT
#define BOUNDED(name, c, g, s, a) struct az_connection_state name __attribute__((cleanup(az_connection_state_drop))) = AZ_CONNECTION_STATE_BOUNDS(c, g, s, a)
struct Connection {
    unsigned fixture_id;
};
struct Database {
    struct Connection *connection;
};
struct SourceContext {
    struct fixture_owner database, weak;
};
FIXTURE_VTABLE(database_vtable, fixture_noop);
static struct SourceContext source_context(struct Connection *connection) {
    struct SourceContext c = {0};
    c.database = fixture_make(sizeof(struct Database), database_vtable);
    ((struct Database *) c.database.object)->connection = connection;
    c.weak = fixture_weak(c.database);
    return c;
}
static unsigned profile;
static struct az_connection_receipt known(struct az_connection_state *s, const void *p) {
    struct az_connection_receipt r = {0};
    REQUIRE(az_connection_read(s, p, &r) && r.generation, "actual observed live connection receipt");
    return r;
}
static void unknown(struct az_connection_state *s, const void *p, const char *why) {
    struct az_connection_receipt r = {UINT64_C(0xabcdef)};
    REQUIRE(!az_connection_read(s, p, &r) && r.generation == UINT64_C(0xabcdef), why);
}
struct NativeCall {
    struct az_connection_state *state;
    struct Connection *connection, **output;
    int result, error;
    unsigned *forwards;
    void (*during)(struct NativeCall *);
    void *context;
    bool raises;
};
static int native_call(void *opaque) {
    struct NativeCall *c = opaque;
    if (c->forwards) ++*c->forwards;
    if (c->during) c->during(c);
    if (c->output) *c->output = c->connection;
    errno = c->error;
    if (c->raises) fixture_raise();
    return c->result;
}
static int open_call(struct NativeCall *c, struct az_connection_receipt *r) {
    return az_connection_forward_open(c->state, c->output, native_call, c, r);
}
static int close_call(struct NativeCall *c) {
    return az_connection_forward_close(c->state, c->connection, native_call, c);
}
static int close_plain(struct az_connection_state *s, struct Connection *p) {
    struct NativeCall c = {.state = s, .connection = p};
    return close_call(&c);
}
static struct az_connection_receipt birth(struct az_connection_state *s, struct Connection *p) {
    struct Connection *output = NULL;
    struct az_connection_receipt r = {0};
    unsigned forwards = 0;
    struct NativeCall c = {.state = s, .connection = p, .output = &output, .forwards = &forwards};
    int rc = open_call(&c, &r);
    REQUIRE(rc == 0 && forwards == 1 && output == p, "birth forwards actual result/output exactly once");
    REQUIRE(r.generation == known(s, p).generation, "native open observation publishes the read receipt");
    return r;
}
static void retiring(struct NativeCall *c) { unknown(c->state, c->connection, "generation retires BEFORE original close executes"); }
static void four_owner_contexts(void) {
    STATE(state);
    struct Connection pool[4] = {{1}, {2}, {3}, {4}};
    struct SourceContext contexts[4];
    struct az_connection_receipt receipts[4] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        contexts[i] = source_context(&pool[i]);
        struct SourceContext *context = &contexts[i];
        struct Connection *connection = ((struct Database *) context->database.object)->connection;
        unknown(&state, connection, "pre-observation connection is unknown");
        int strong = context->database.control->strong;
        unsigned forwards = 0;
        struct Connection *output = NULL;
        struct NativeCall c = {.state = &state, .connection = connection, .output = &output, .forwards = &forwards, .error = EINTR};
        int rc = open_call(&c, &receipts[i]);
        REQUIRE(rc == 0 && forwards == 1 && output == &pool[i] && errno == EINTR, "unchanged native result, output and errno before the first observer assertion");
        printf("NATIVE_FORWARD_OK build=%u context=%u implementation=%d\n", profile, i, EPOCH_IMPLEMENTATION_PRESENT);
        fflush(stdout);
        REQUIRE(receipts[i].generation && az_connection_read(&state, output, &receipts[i]), "missing observed successful connection birth");
        REQUIRE(context->database.control->strong == strong && !fixture_expired(context->weak), "connection receipt does not acquire or replace the genuine GNU owner");
        if (i) REQUIRE(receipts[i].generation > receipts[i - 1].generation, "four independent source contexts receive nonreused generations");
    }
    for (unsigned i = 0; i < 4; ++i) {
        struct SourceContext *context = &contexts[i];
        struct az_connection_receipt old = receipts[i];
        unsigned forwards = 0;
        int result = i == 0 ? 5 : i == 1 ? 21
                              : i == 2   ? -7
                                         : 0;
        struct NativeCall c = {.state = &state, .connection = ((struct Database *) context->database.object)->connection, .result = result, .error = EBUSY, .forwards = &forwards, .during = retiring};
        REQUIRE(close_call(&c) == result && forwards == 1 && errno == EBUSY, "BUSY/error/zombie/success result and errno forward unchanged");
        unknown(&state, &pool[i], "no close result revives its retired generation");
        REQUIRE(birth(&state, &pool[i]).generation > old.generation && !fixture_expired(context->weak), "same pointer and live native owner require a fresh observed connection generation");
        for (unsigned j = i + 1; j < 4; ++j) REQUIRE(known(&state, &pool[j]).generation == receipts[j].generation, "retiring one source preserves independent connection receipts");
    }
    for (unsigned i = 0; i < 4; ++i) {
        fixture_drop(&contexts[i].database);
        fixture_weak_drop(&contexts[i].weak);
    }
}
static void refused_opens_and_duplicates(void) {
    STATE(state);
    struct Connection a = {1}, b = {2};
    const int results[] = {1, 5, 7, -1};
    for (unsigned i = 0; i < 4; ++i) {
        struct Connection *output = NULL;
        struct az_connection_receipt r = {81};
        unsigned forwards = 0;
        struct NativeCall c = {.state = &state, .connection = &a, .output = &output, .result = results[i], .error = EIO, .forwards = &forwards};
        REQUIRE(open_call(&c, &r) == results[i] && forwards == 1 && output == &a && errno == EIO && r.generation == 81, "failed nonnull open is forwarded but cannot publish or alter a receipt");
        unknown(&state, &a, "failed nonnull native open is ineligible");
    }
    struct Connection *output = NULL;
    struct az_connection_receipt r = {82};
    struct NativeCall c = {.state = &state, .output = &output};
    REQUIRE(open_call(&c, &r) == 0 && r.generation == 82, "successful null output cannot publish a birth");
    c.output = NULL;
    c.result = 7;
    REQUIRE(open_call(&c, &r) == 7 && r.generation == 82, "null caller output is not dereferenced by bookkeeping");
    struct az_connection_receipt old = birth(&state, &a);
    output = &b;
    r.generation = 83;
    c = (struct NativeCall) {.state = &state, .connection = &a, .output = &output};
    open_call(&c, &r);
    REQUIRE(r.generation == 83, "duplicate live pointer has no verified retirement and is refused");
    unknown(&state, &a, "ambiguous duplicate live pointer retires the previous receipt");
    close_plain(&state, &a);
    REQUIRE(birth(&state, &a).generation > old.generation, "only a subsequent actual open can create a new incarnation");
}
static void nested_close(struct NativeCall *c) {
    struct Connection *p = c->context;
    unknown(c->state, p, "scalar reads refuse while native lifecycle work is active");
    struct NativeCall nested = {.state = c->state, .connection = p, .result = 5, .during = retiring};
    close_call(&nested);
}
static void nested_open(struct NativeCall *c) {
    struct Connection *output = NULL;
    struct az_connection_receipt inner = {93};
    struct NativeCall nested = {.state = c->state, .connection = c->context, .output = &output};
    open_call(&nested, &inner);
    REQUIRE(inner.generation == 93, "nested open is not an independent observed birth");
}
struct UnwindCall {
    struct NativeCall native;
    struct az_connection_receipt *receipt;
    bool closing;
};
static void unwind_call(void *opaque) {
    struct UnwindCall *c = opaque;
    if (c->closing) close_call(&c->native);
    else open_call(&c->native, c->receipt);
    REQUIRE(false, "native unwind must propagate to its caller");
}
static void nesting_and_exceptions(void) {
    STATE(state);
    struct Connection a = {1}, b = {2}, c = {3};
    birth(&state, &a);
    struct Connection *output = NULL;
    struct az_connection_receipt outer = {91};
    struct NativeCall call = {.state = &state, .connection = &b, .output = &output, .during = nested_close, .context = &a};
    open_call(&call, &outer);
    REQUIRE(outer.generation == 91, "close during open invalidates its post-return admission");
    unknown(&state, &a, "nested BUSY cannot revive the old receipt");
    unknown(&state, &b, "open overlapping close is unqualified");
    output = NULL;
    outer.generation = 92;
    call.during = nested_open;
    call.context = &c;
    open_call(&call, &outer);
    REQUIRE(outer.generation == 92, "nested open invalidates the outer birth too");
    unknown(&state, &b, "outer overlapping open remains unknown");
    unknown(&state, &c, "inner overlapping open remains unknown");
    birth(&state, &a);
    call = (struct NativeCall) {.state = &state, .connection = &a, .during = nested_open, .context = &b};
    close_call(&call);
    unknown(&state, &b, "closing scope cannot admit a nested open");
    struct az_connection_receipt before = birth(&state, &a);
    unsigned forwards = 0, deletions = fixture_exception_deletions;
    struct UnwindCall unwinding = {.native = {.state = &state, .connection = &a, .forwards = &forwards, .error = ERANGE, .raises = true}, .closing = true};
    REQUIRE(!fixture_native_try(unwind_call, &unwinding) && forwards == 1 && errno == ERANGE && fixture_exception_deletions == deletions + 1, "close unwind identity, errno and single forwarding survive bookkeeping cleanup");
    unknown(&state, &a, "exception cannot undo entry retirement");
    output = NULL;
    outer.generation = 95;
    forwards = 0;
    unwinding = (struct UnwindCall) {.native = {.state = &state, .connection = &b, .output = &output, .forwards = &forwards, .error = EDOM, .raises = true}, .receipt = &outer};
    REQUIRE(!fixture_native_try(unwind_call, &unwinding) && forwards == 1 && errno == EDOM && outer.generation == 95 && fixture_exception_deletions == deletions + 2, "unwinding open never publishes and preserves the native failure and errno");
    unknown(&state, &b, "written output before exception is not a completed birth");
    REQUIRE(birth(&state, &a).generation > before.generation, "normal operation resumes after balanced exception unwind");
}
static void saturate_nested(struct NativeCall *c) { close_plain(c->state, c->connection); }
static void quota_and_saturation(void) {
    struct Connection a = {1}, b = {2}, c = {3};
    BOUNDED(quota, 2, UINT64_MAX, UINT64_MAX, 32);
    struct az_connection_receipt first = birth(&quota, &a), second = birth(&quota, &b), refused = {101};
    struct Connection *output = NULL;
    unsigned forwards = 0;
    struct NativeCall call = {.state = &quota, .connection = &c, .output = &output, .forwards = &forwards};
    open_call(&call, &refused);
    REQUIRE(forwards == 1 && refused.generation == 101 && known(&quota, &a).generation == first.generation && known(&quota, &b).generation == second.generation, "quota cannot evict a live generation");
    unknown(&quota, &c, "quota-refused native result remains unknown");
    close_plain(&quota, &a);
    REQUIRE(birth(&quota, &c).generation > second.generation, "retired slots hold fresh generations without identity reuse");
    BOUNDED(generations, 2, 2, UINT64_MAX, 32);
    birth(&generations, &a);
    close_plain(&generations, &a);
    REQUIRE(birth(&generations, &a).generation == 2, "last finite generation is usable");
    close_plain(&generations, &a);
    output = NULL;
    refused.generation = 102;
    call = (struct NativeCall) {.state = &generations, .connection = &a, .output = &output};
    open_call(&call, &refused);
    REQUIRE(refused.generation == 102, "generation exhaustion cannot wrap");
    unknown(&generations, &a, "exhausted generation publication stays unavailable");
    BOUNDED(sequences, 2, UINT64_MAX, 2, 32);
    birth(&sequences, &a);
    close_plain(&sequences, &a);
    output = NULL;
    refused.generation = 103;
    call.state = &sequences;
    open_call(&call, &refused);
    REQUIRE(refused.generation == 103, "operation sequence exhaustion refuses admission");
    unknown(&sequences, &a, "operation sequence never wraps into an older admission token");
    BOUNDED(depth, 2, UINT64_MAX, UINT64_MAX, 1);
    output = NULL;
    refused.generation = 104;
    call.state = &depth;
    call.during = saturate_nested;
    open_call(&call, &refused);
    REQUIRE(refused.generation == 104, "active-depth saturation invalidates admission");
    output = NULL;
    call.connection = &b;
    call.during = NULL;
    open_call(&call, &refused);
    REQUIRE(refused.generation == 104, "depth saturation remains fail closed after unwind");
}
static void stall(struct NativeCall *c) {
    atomic_uint *stage = c->context;
    atomic_store_explicit(stage, 1, memory_order_release);
    while (atomic_load_explicit(stage, memory_order_acquire) != 2) sched_yield();
}
static void resume_close(struct NativeCall *c) {
    retiring(c);
    atomic_store_explicit((atomic_uint *) c->context, 2, memory_order_release);
}
struct ThreadCall {
    struct NativeCall native;
    struct az_connection_receipt *receipt;
    bool closing;
};
static void *forward_worker(void *opaque) {
    struct ThreadCall *c = opaque;
    if (c->closing) close_call(&c->native);
    else open_call(&c->native, c->receipt);
    return NULL;
}
struct Reader {
    struct az_connection_state *state;
    struct Connection *connection;
    atomic_bool *done;
    uint64_t minimum;
};
static void *read_worker(void *opaque) {
    struct Reader *c = opaque;
    while (!atomic_load_explicit(c->done, memory_order_acquire)) {
        struct az_connection_receipt r = {0};
        if (az_connection_read(c->state, c->connection, &r)) REQUIRE(r.generation >= c->minimum, "concurrent scalar readers never observe an old or zero generation");
    }
    return NULL;
}
static void concurrent_overlap_and_reads(void) {
    STATE(state);
    struct Connection a = {1}, b = {2};
    struct az_connection_receipt old = birth(&state, &a), candidate = {111};
    atomic_uint stage = 0;
    struct Connection *output = NULL;
    struct ThreadCall thread = {.native = {.state = &state, .connection = &b, .output = &output, .during = stall, .context = &stage}, .receipt = &candidate};
    pthread_t worker;
    REQUIRE(!pthread_create(&worker, NULL, forward_worker, &thread), "start opening worker");
    while (atomic_load_explicit(&stage, memory_order_acquire) != 1) sched_yield();
    struct NativeCall close = {.state = &state, .connection = &a, .during = resume_close, .context = &stage};
    close_call(&close);
    REQUIRE(!pthread_join(worker, NULL), "join opening worker");
    REQUIRE(candidate.generation == 111, "concurrent close invalidates an overlapping open");
    unknown(&state, &a, "concurrent retirement stays retired");
    unknown(&state, &b, "overlapping open is not admitted after close finishes");
    REQUIRE(birth(&state, &a).generation > old.generation, "subsequent nonoverlapping open is fresh");
    atomic_store_explicit(&stage, 0, memory_order_release);
    candidate.generation = 112;
    thread = (struct ThreadCall) {.native = {.state = &state, .connection = &a, .during = stall, .context = &stage}, .closing = true};
    REQUIRE(!pthread_create(&worker, NULL, forward_worker, &thread), "start closing worker");
    while (atomic_load_explicit(&stage, memory_order_acquire) != 1) sched_yield();
    output = NULL;
    struct NativeCall open = {.state = &state, .connection = &a, .output = &output};
    open_call(&open, &candidate);
    atomic_store_explicit(&stage, 2, memory_order_release);
    REQUIRE(!pthread_join(worker, NULL), "join closing worker");
    REQUIRE(candidate.generation == 112, "same-address reopen before old close returns is conservatively refused");
    unknown(&state, &a, "old close completion cannot publish or revive a reused address");
    struct az_connection_receipt fresh = birth(&state, &a);
    atomic_bool done = false;
    struct Reader reader = {&state, &a, &done, fresh.generation};
    REQUIRE(!pthread_create(&worker, NULL, read_worker, &reader), "start reader");
    for (unsigned i = 0; i < 200; ++i) {
        close_plain(&state, &a);
        birth(&state, &a);
    }
    atomic_store_explicit(&done, true, memory_order_release);
    REQUIRE(!pthread_join(worker, NULL), "join reader");
}
#if EPOCH_IMPLEMENTATION_PRESENT
static void status_nested_open(struct NativeCall *c) {
    struct Connection *output = NULL;
    struct NativeCall nested = {.state = c->state, .connection = c->context, .output = &output};
    open_call(&nested, NULL);
}
struct StatusClose {
    struct Connection *other;
    uint64_t expected;
};
static void status_retiring(struct NativeCall *c) {
    struct StatusClose *v = c->context;
    REQUIRE(!az_connection_status_generation(c->state, c->connection) && az_connection_status_generation(c->state, v->other) == v->expected, "retire closing connection at entry, preserve other source");
}
static void status_two_sources(void) {
    STATE(state);
    struct Connection a = {1}, b = {2}, unrelated = {3}, *one = NULL, *two = NULL;
    struct NativeCall c = {.state = &state, .connection = &a, .output = &one, .during = status_nested_open, .context = &b};
    open_call(&c, NULL);
    uint64_t first = az_connection_status_generation(&state, &a), second = az_connection_status_generation(&state, &b);
    REQUIRE(first && second && first != second, "distinct opened status connections can overlap");
    struct StatusClose check = {&a, first};
    c = (struct NativeCall) {.state = &state, .connection = &b, .during = status_retiring, .context = &check};
    close_call(&c);
    c = (struct NativeCall) {.state = &state, .connection = &b, .output = &two, .during = nested_close, .context = &unrelated};
    open_call(&c, NULL);
    REQUIRE(!az_connection_status_generation(&state, &b), "an intervening close refuses a new overlapping status birth");
    c.during = NULL;
    open_call(&c, NULL);
    REQUIRE(az_connection_status_generation(&state, &b) && az_connection_status_generation(&state, &b) != second, "new status generation after close");
}
#endif
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "one independent process per configured build profile");
    profile = (unsigned) (argv[1][0] - '0');
    four_owner_contexts();
    refused_opens_and_duplicates();
    nesting_and_exceptions();
    quota_and_saturation();
    concurrent_overlap_and_reads();
#if EPOCH_IMPLEMENTATION_PRESENT
    status_two_sources();
#endif
    printf("PASS connection-epoch build=%u: four GNU owner contexts, native-operation forwarding, retirement, reuse and overlap\n", profile);
    return 0;
}

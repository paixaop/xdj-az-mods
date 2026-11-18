/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable routing spies check call order and register frames only. */
#include "../../src/common/azcachebirth.h"
#include "../../src/common/azcacheentry.h"
#include "native_abi_fixture.h"
#include <stdio.h>
#define CHECK(x, why)                                                  \
    do {                                                               \
        if (!(x)) {                                                    \
            fprintf(stderr, "FAIL cache-dispatch-routing: %s\n", why); \
            abort();                                                   \
        }                                                              \
    } while (0)
typedef struct azce_call azce_call;
typedef struct Frame Frame;
static unsigned retirement_calls, retirement_depth, factory_calls, factory_depth, write_calls, write_depth;
static const int *active_status;
static bool factory_entry(unsigned kind) { return kind == AZCE_KIND_PRIMARY_FACTORY || kind == AZCE_KIND_STANDARD_FACTORY; }
static bool write_entry(unsigned kind) { return kind == AZCE_KIND_SETUP || kind == AZCE_KIND_LAYOUT; }
static struct {
    const void *receiver, *pool;
    unsigned kind, argument;
    uint32_t status;
} last_write;
static struct {
    const void *receiver, *pool, *result;
    unsigned kind;
} last_factory;
static void (*frame_original(unsigned kind))(azce_call *);
struct Frame {
    azce_call call, before;
    uint64_t output[3];
    unsigned forwards, expected_depth;
    bool null_factory;
    uint32_t status_bits;
    void (*action)(Frame *);
    Frame *inner;
    bool throwing;
};
static void frame_init(Frame *self, unsigned kind, unsigned depth) {
    *self = (Frame) {.output = {UINT64_C(0xcafefeed), 0, UINT64_C(0xfedcba98)}, .expected_depth = depth, .status_bits = UINT32_C(0x56789abc)};

    self->call.kind = kind;
    self->call.reserved = 0x8a734951;
    self->call.caller_link = UINT64_C(0x0123456789abcdef);
    self->call.original = ((uintptr_t) (frame_original(kind)));
    for (unsigned i = 0; i != 9; ++i) self->call.args[i] = UINT64_C(0x1302000000) + i;
    self->call.args[7] = ((uintptr_t) (self));
    self->call.args[8] = ((uintptr_t) (&self->output[1]));
    if (factory_entry(kind)) {
        self->call.args[0] = ((uintptr_t) (self));
        self->call.args[1] = ((uintptr_t) (&self->output[0]));
    }
    if (write_entry(kind)) {
        self->call.args[0] = ((uintptr_t) (self));
        self->call.args[1] = UINT64_C(0xfedcba9800000010);
        self->call.args[2] = kind == AZCE_KIND_SETUP ? ((uintptr_t) (&self->output[0])) : UINT64_C(0x987654320000ab23);
        self->call.args[3] = UINT64_C(0x876543210000cd45);
    }
    for (unsigned i = 0; i != 8; ++i) {
        self->call.vector_args[i].words[0] = UINT64_C(0x1234567800000000) + i;
        self->call.vector_args[i].words[1] = UINT64_C(0xabcdef0100000000) + i;
    }
    self->before = self->call;
}
static uint64_t frame_result0(const Frame *self) {
    if (factory_entry(self->call.kind)) return self->null_factory ? 0 : ((uintptr_t) (&self->output[2]));
    return write_entry(self->call.kind) ? UINT64_C(0xabcd123400000000) | self->status_bits : UINT64_C(0xabcd123456789abc);
}
static void frame_inputs(const Frame *self) {
    CHECK(memcmp(self->call.args, self->before.args, sizeof self->call.args) == 0 &&
              memcmp(self->call.vector_args, self->before.vector_args, sizeof self->call.vector_args) == 0 &&
              self->call.kind == self->before.kind && self->call.reserved == self->before.reserved &&
              self->call.original == self->before.original && self->call.caller_link == self->before.caller_link,
          "original arguments, X8 and caller frame unchanged");
}
static void frame_original_call(azce_call *call) {
    Frame *self = (Frame *) call->args[7];
    CHECK(&self->call == call, "original receives exact caller-owned frame");
    frame_inputs(self);
    ++self->forwards;
    call->result[0] = frame_result0(self);
    call->result[1] = UINT64_C(0x87364951abcdef01);
    for (unsigned i = 0; i != 4; ++i) {
        call->vector_results[i].words[0] = UINT64_C(0x3736000000) + i;
        call->vector_results[i].words[1] = UINT64_C(0x8273000000) + i;
    }
    *((uint64_t *) (call->args[8])) = UINT64_C(0xfdecab0198765432);
    printf("ORIGINAL_FRAME_FORWARD_OK kind=%u depth=%u\n", call->kind, retirement_depth);
    fflush(stdout);
    CHECK(write_entry(call->kind) || retirement_depth == self->expected_depth,
          "missing mutation retirement routing before original");
    if (self->action) self->action(self);
}
static void frame_results(const Frame *self) {
    frame_inputs(self);
    CHECK(self->forwards == 1 && self->output[0] == UINT64_C(0xcafefeed) &&
              self->output[1] == UINT64_C(0xfdecab0198765432) && self->output[2] == UINT64_C(0xfedcba98) &&
              self->call.result[0] == frame_result0(self) &&
              self->call.result[1] == UINT64_C(0x87364951abcdef01),
          "exactly one original and unchanged result frame");
    for (unsigned i = 0; i != 4; ++i)
        CHECK(self->call.vector_results[i].words[0] == UINT64_C(0x3736000000) + i &&
                  self->call.vector_results[i].words[1] == UINT64_C(0x8273000000) + i,
              "all SIMD result bits preserved");
}
#define DISTINCT(n)                                                              \
    static void distinct_##n(azce_call *call) {                                  \
        CHECK(call->kind == n, "each entry forwards its own distinct original"); \
        frame_original_call(call);                                               \
    }
DISTINCT(0)
DISTINCT(1)
DISTINCT(2) DISTINCT(3) DISTINCT(4) DISTINCT(5) DISTINCT(6) DISTINCT(7) DISTINCT(8) DISTINCT(9) DISTINCT(10) DISTINCT(11) DISTINCT(12) DISTINCT(13) DISTINCT(14) DISTINCT(15) DISTINCT(16) DISTINCT(17) DISTINCT(18) DISTINCT(19) DISTINCT(20) DISTINCT(21) DISTINCT(22)
#undef DISTINCT
    static void (*frame_original(unsigned kind))(azce_call *) {
    static void (*const originals[])(azce_call *) = {distinct_0, distinct_1, distinct_2, distinct_3, distinct_4, distinct_5, distinct_6, distinct_7, distinct_8, distinct_9, distinct_10, distinct_11, distinct_12, distinct_13, distinct_14, distinct_15, distinct_16, distinct_17, distinct_18, distinct_19, distinct_20, distinct_21, distinct_22};
    return kind < 23 ? originals[kind] : frame_original_call;
}
static void dispatch(void *p) {
    Frame *f = p;
    azsdk_cache_dispatch_call(&f->call);
}
static void maybe_throw(Frame *f) {
    if (f->throwing) fixture_raise();
}
static void checked_dispatch(Frame *f, bool throwing) {
    unsigned before = fixture_exception_deletions;
    bool caught = !az_native_try(dispatch, f);
    CHECK(caught == throwing && fixture_exception_deletions == before + (unsigned) throwing, "original native unwind propagates once");
}
static void run(unsigned kind, bool mutation, bool throwing) {
    unsigned prior = retirement_calls, prior_factory = factory_calls;
    Frame frame;
    frame_init(&frame, kind, mutation ? 1 : 0);
    frame.action = maybe_throw;
    frame.throwing = throwing;
    checked_dispatch(&frame, throwing);
    frame_results(&frame);
    CHECK(factory_calls == prior_factory + (factory_entry(kind) ? 1u : 0u), "missing native factory entry adaptation");
    if (factory_entry(kind) && !throwing) CHECK(last_factory.receiver == (void *) frame.call.args[0] && last_factory.pool == (void *) frame.call.args[1] && last_factory.result == (void *) frame.call.result[0] && last_factory.kind == (kind == AZCE_KIND_PRIMARY_FACTORY ? AZCB_PRIMARY_FACTORY : AZCB_STANDARD_FALLBACK_FACTORY), "factory adapter copies proved receiver/pool/raw-result registers only");
    CHECK(!factory_depth, "factory scope restores on return and native unwind");
    CHECK(!retirement_depth && retirement_calls == prior + (mutation ? 1u : 0u), "only known mutations use one retirement forwarding scope");
}
static void entry_table(void) {
    const unsigned ids[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE, AZCE_KIND_QUERY, AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO, AZCE_KIND_SETUP, AZCE_KIND_LAYOUT, AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW, AZCE_KIND_PRIMARY_FACTORY, AZCE_KIND_STANDARD_FACTORY, AZCE_KIND_COLLECTOR_DISPOSE, AZCE_KIND_SELECTION_STORE, AZCE_KIND_SELECTION_CLEAR, AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY, AZCE_KIND_LOADINFO_DESTROY, AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE, AZCE_KIND_REPLY_TICKET, AZCE_KIND_REPLY_RECEPTION};
    CHECK(AZCE_KIND_COUNT == sizeof ids / sizeof *ids, "one independent original slot per entry");
    void *entries[AZCE_KIND_COUNT];
    for (unsigned kind = 0; kind < AZCE_KIND_COUNT; ++kind) {
        CHECK(ids[kind] == kind, "private numeric entry IDs preserve all existing values and append in order");
        entries[kind] = azsdk_cache_entry(kind);
        CHECK(entries[kind], kind < 11 ? "existing entry remains available" : "missing distinct appended cache entries and original slots");
        for (unsigned prior = 0; prior < kind; ++prior) CHECK(entries[prior] != entries[kind], "appended entry aliases an existing native redirect");
        if (kind == 10) {
            puts("LEGACY_ENTRY_TABLE_OK entries=11");
            fflush(stdout);
        }
        azsdk_cache_entry_originals[kind] = frame_original(kind);
    }
    for (unsigned kind = 0; kind < AZCE_KIND_COUNT; ++kind) {
        CHECK(azsdk_cache_entry_originals[kind] == (void *) frame_original(kind), "distinct originals cannot overwrite each other");
        for (unsigned prior = 0; prior < kind; ++prior) CHECK(frame_original(prior) != frame_original(kind), "test originals have distinct identities");
        azsdk_cache_entry_originals[kind] = NULL;
    }
    CHECK(!azsdk_cache_entry(AZCE_KIND_COUNT) && !azsdk_cache_entry(UINT32_MAX), "unknown entries remain unavailable");
}
static void null_factory(unsigned kind) {
    unsigned prior = factory_calls;
    Frame f;
    frame_init(&f, kind, 0);
    f.null_factory = true;
    dispatch(&f);
    frame_results(&f);
    CHECK(factory_calls == prior + 1 && !last_factory.result && !factory_depth, "null factory result forwards unchanged without a fabricated owner");
}
/* Cleanup assertions run during the original foreign unwind, without replacing
 * or swallowing its payload at an inner scope. */
struct NestedCheck {
    unsigned mode;
    const int *status;
};
static void nested_check(struct NestedCheck *c) {
    if (c->mode == 0) CHECK(retirement_depth == 1, "inner return/unwind restores outer scope");
    else if (c->mode == 1) CHECK(factory_depth == 1, "nested factory return/unwind restores outer scope");
    else CHECK(write_depth == 1 && retirement_depth == 1 && active_status == c->status && *c->status == -1, "nested write return/unwind restores original outer local status");
}
static void nested_original(Frame *f) {
    Frame forwarded;
    frame_init(&forwarded, AZCE_KIND_QUERY, 1);
    dispatch(&forwarded);
    frame_results(&forwarded);
    struct NestedCheck check __attribute__((cleanup(nested_check))) = {0, NULL};
    dispatch(f->inner);
}
static void nested_factory_inner(Frame *f) {
    CHECK(factory_depth == 2, "nested factory forwarding owns independent local raw-result slot");
    maybe_throw(f);
}
static void nested_factory_outer(Frame *f) {
    CHECK(factory_depth == 1, "outer factory wraps only its genuine original");
    struct NestedCheck check __attribute__((cleanup(nested_check))) = {1, NULL};
    dispatch(f->inner);
}
static void nested(bool throwing, bool factory) {
    unsigned prior = factory ? factory_calls : retirement_calls;
    Frame inner, outer;
    frame_init(&inner, factory ? AZCE_KIND_STANDARD_FACTORY : AZCE_KIND_COLLECTOR_DELETE, factory ? 0 : 2);
    frame_init(&outer, factory ? AZCE_KIND_PRIMARY_FACTORY : AZCE_KIND_SERVER_RESET, factory ? 0 : 1);
    inner.throwing = throwing;
    inner.action = factory ? nested_factory_inner : maybe_throw;
    outer.inner = &inner;
    outer.action = factory ? nested_factory_outer : nested_original;
    checked_dispatch(&outer, throwing);
    frame_results(&inner);
    frame_results(&outer);
    CHECK(factory ? (!factory_depth && factory_calls == prior + 2) : (!retirement_depth && retirement_calls == prior + 2), "nested routing scopes finish exactly once");
    if (factory && !throwing) CHECK(last_factory.receiver == (void *) outer.call.args[0] && last_factory.result == (void *) outer.call.result[0], "inner result cannot replace outer factory raw result");
}
static void write_original(Frame *f) {
    if (active_status) CHECK(*active_status == -1, "status copied only after normal original return");
    maybe_throw(f);
}
static void write_case(unsigned kind, uint32_t status, uint32_t argument, bool throwing) {
    unsigned prior = write_calls, prior_retirement = retirement_calls;
    Frame f;
    frame_init(&f, kind, 1);
    f.action = write_original;
    f.throwing = throwing;
    f.status_bits = status;
    f.call.args[1] = UINT64_C(0xfedcba9800000000) | argument;
    f.before = f.call;
    checked_dispatch(&f, throwing);
    frame_results(&f);
    printf("WRITE_NATIVE_FORWARD_OK kind=%u status=%08x\n", kind, status);
    fflush(stdout);
    CHECK(write_calls == prior + 1, "missing native construction write entry adaptation");
    CHECK(!write_depth && !active_status && !retirement_depth && retirement_calls == prior_retirement + 1, "one construction write retirement scope restores on normal and exceptional return");
    if (!throwing) CHECK(last_write.receiver == (void *) f.call.args[0] && last_write.pool == (kind == AZCE_KIND_SETUP ? (void *) f.call.args[2] : NULL) && last_write.kind == (kind == AZCE_KIND_SETUP ? AZCB_SETUP_WRITE : AZCB_LAYOUT_WRITE) && last_write.argument == argument && last_write.status == status, "write adapter copies only exact receiver, low W1, setup pool and signed W0 bits");
}
static void nested_write_inner(Frame *f) {
    CHECK(write_depth == 2 && retirement_depth == 2 && active_status && *active_status == -1, "nested write keeps its separate unpopulated status until return");
    maybe_throw(f);
}
static void nested_write_outer(Frame *f) {
    CHECK(write_depth == 1 && retirement_depth == 1 && active_status && *active_status == -1, "outer write wraps genuine native execution before copying status");
    struct NestedCheck check __attribute__((cleanup(nested_check))) = {2, active_status};
    dispatch(f->inner);
}
static void nested_write(bool throwing) {
    unsigned prior = write_calls, prior_retirement = retirement_calls;
    Frame inner, outer;
    frame_init(&inner, AZCE_KIND_LAYOUT, 2);
    frame_init(&outer, AZCE_KIND_SETUP, 1);
    inner.action = nested_write_inner;
    inner.throwing = throwing;
    inner.status_bits = UINT32_C(0x80000000);
    outer.action = nested_write_outer;
    outer.inner = &inner;
    outer.status_bits = UINT32_C(0x7fffffff);
    checked_dispatch(&outer, throwing);
    frame_results(&inner);
    frame_results(&outer);
    CHECK(write_calls == prior + 2 && retirement_calls == prior_retirement + 2 && !write_depth && !retirement_depth && !active_status, "nested write scopes finish exactly once");
    if (!throwing) CHECK(last_write.receiver == (void *) outer.call.args[0] && last_write.status == outer.status_bits, "outer status remains independent of nested return");
}
/* Transparent routing spies; these make no source/ownership acceptance claim. */
void azcb_forward_library_owned(unsigned build, void *server, struct azcb_forward original) {
    (void) build;
    (void) server;
    original.call(original.context);
}
void azcb_observe_entry_owned(azce_call *c, struct azcb_forward original) {
    (void) c;
    original.call(original.context);
}
static void depth_end(unsigned **depth) { --**depth; }
void azcb_retire_forward(struct azcb_forward original) {
    CHECK(original.call, "routing supplies a forwarding closure");
    ++retirement_calls;
    ++retirement_depth;
    unsigned *depth __attribute__((cleanup(depth_end))) = &retirement_depth;
    original.call(original.context);
}
void azcb_selection_forward(struct azcb_forward original) { azcb_retire_forward(original); }
void azcb_observe_factory_owned(const struct azcb_factory_borrow *b, struct azcb_forward original) {
    CHECK(original.call && b->native_result && !*b->native_result, "factory raw-result slot is observer-local and initially empty");
    ++factory_calls;
    ++factory_depth;
    unsigned *depth __attribute__((cleanup(depth_end))) = &factory_depth;
    original.call(original.context);
    last_factory.receiver = b->receiver_creator;
    last_factory.pool = b->borrowed_pool;
    last_factory.result = *b->native_result;
    last_factory.kind = b->kind;
}
struct WriteScope {
    const int *previous, *status;
    bool returned;
};
static void write_scope_end(struct WriteScope *scope) {
    if (!scope->returned) CHECK(*scope->status == -1, "throwing original never fabricates a returned status");
    active_status = scope->previous;
    --write_depth;
}
void azcb_observe_construction_write_owned(const struct azcb_write_borrow *b, struct azcb_forward original) {
    CHECK(original.call && b->native_status && *b->native_status == -1, "write status is a separate observer-local int initialized before native call");
    CHECK(b->kind == AZCB_SETUP_WRITE || b->kind == AZCB_LAYOUT_WRITE, "only reviewed construction write kinds");
    const Frame *frame = b->receiver_cache;
    CHECK(frame && b->native_status != (const int *) &frame->call.result[0], "write status never aliases native register result storage");
    ++write_calls;
    struct WriteScope scope __attribute__((cleanup(write_scope_end))) = {active_status, b->native_status, false};
    active_status = b->native_status;
    ++write_depth;
    azcb_retire_forward(original);
    scope.returned = true;
    last_write.receiver = b->receiver_cache;
    last_write.pool = b->borrowed_pool;
    last_write.kind = b->kind;
    last_write.argument = b->argument;
    memcpy(&last_write.status, b->native_status, sizeof last_write.status);
}
void azsdk_cache_invoke_original(azce_call *call) {
    CHECK(call && call->original, "explicit frame backend has genuine original");
    ((void (*)(azce_call *)) call->original)(call);
}
#define ENTRY(n) \
    void azsdk_cache_entry_##n(void) { abort(); }
ENTRY(0)
ENTRY(1)
ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6) ENTRY(7) ENTRY(8) ENTRY(9) ENTRY(10) ENTRY(11)
    ENTRY(12) ENTRY(13) ENTRY(14) ENTRY(15) ENTRY(16) ENTRY(17) ENTRY(18) ENTRY(19) ENTRY(20) ENTRY(21) ENTRY(22)
#undef ENTRY
        int main(void) {
    const unsigned forwarded[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE, AZCE_KIND_QUERY, AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO, AZCE_KIND_REPLY_TICKET, AZCE_KIND_REPLY_RECEPTION, AZCE_KIND_COUNT, UINT32_MAX};
    for (size_t i = 0; i < sizeof forwarded / sizeof *forwarded; ++i) {
        run(forwarded[i], false, false);
        run(forwarded[i], false, true);
    }
    puts("ROUTING_FORWARD_BASELINE_OK");
    fflush(stdout);
    entry_table();
    for (unsigned kind = AZCE_KIND_PRIMARY_FACTORY; kind <= AZCE_KIND_STANDARD_FACTORY; ++kind) {
        run(kind, false, false);
        run(kind, false, true);
        null_factory(kind);
    }
    const uint32_t statuses[] = {0, 1, UINT32_C(0x80000000), UINT32_MAX};
    for (unsigned kind = AZCE_KIND_SETUP; kind <= AZCE_KIND_LAYOUT; ++kind)
        for (size_t i = 0; i < 4; ++i) {
            write_case(kind, statuses[i], statuses[i], false);
            write_case(kind, statuses[i], statuses[i], true);
        }
    nested_write(false);
    nested_write(true);
    const unsigned mutations[] = {AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW, AZCE_KIND_COLLECTOR_DISPOSE, AZCE_KIND_SELECTION_STORE, AZCE_KIND_SELECTION_CLEAR, AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY, AZCE_KIND_LOADINFO_DESTROY, AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE};
    for (size_t i = 0; i < sizeof mutations / sizeof *mutations; ++i) {
        run(mutations[i], true, false);
        run(mutations[i], true, true);
    }
    nested(false, false);
    nested(true, false);
    nested(false, true);
    nested(true, true);
    puts("PASS cache-dispatch-routing: factory/write adapters, retirement, exact frames/status bits, native unwinding and reentry");
}

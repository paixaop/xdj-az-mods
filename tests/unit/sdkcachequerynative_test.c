/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual dispatcher/cache helpers with C native row carriers and register frames. */
#define SDK_CACHE_NATIVE_MAIN sdkcachequery_native_baseline
#include "sdkcachenative_test.c"
#undef SDK_CACHE_NATIVE_MAIN
#include "../../src/common/azcachequery.h"
bool azcq_current_owned(struct azcq_evidence *) __attribute__((weak));
static bool evidence(struct azcq_evidence *out) {
    bool (*reader)(struct azcq_evidence *) = azcq_current_owned;
    return reader && reader(out);
}
static void query_evidence_absent(void) {
    struct azcq_evidence out __attribute__((cleanup(azcq_evidence_drop))) = {0};
    out.sequence = 0x12345678;
    out.cache.source_epoch = UINT64_C(0xabcdef0123456789);
    REQUIRE(!evidence(&out) && out.sequence == 0x12345678 && out.cache.source_epoch == UINT64_C(0xabcdef0123456789), "unavailable query evidence leaves output unchanged and masks older observations");
}
typedef struct NativeQueryCall {
    Context *context;
    FixtureAdapter adapter;
    GuardedRows rows;
    const CacheOwner *actual_mount;
    Action inside_adapter, after_adapter;
    unsigned wrappers, adapters, sequence, parameter, first, count, selector, initial_destroyed, initial_alive;
    bool initialized;
} NativeQueryCall;
static void native_query_init(NativeQueryCall *q, Context *c, unsigned sequence) {
    *q = (NativeQueryCall) {.context = c, .adapter = {.vtable = cbf_object_slots, .cache = context_selected(c)->object}, .actual_mount = &c->list.mount, .sequence = sequence, .parameter = 0x80000000u | sequence, .count = 1, .selector = 1, .initial_destroyed = row_destroyed, .initial_alive = row_alive};
    GuardedRows_init(&q->rows);
    REQUIRE(offsetof(FixtureAdapter, cache) == 8 && q->adapter.cache, "actual adapter borrows selected cache at+8");
}
static void native_query_cleanup(NativeQueryCall *q) {
    if (!q->initialized) return;
    unsigned before = row_destroyed;
    rows_drop(GuardedRows_value(&q->rows));
    q->initialized = false;
    GuardedRows_check(&q->rows);
    REQUIRE(row_alive == q->initial_alive && row_destroyed == before + 1, "native result caller alone destroys returned row and pointer buffer");
}
#define NATIVE_QUERY(name, context, sequence)                            \
    NativeQueryCall name __attribute__((cleanup(native_query_cleanup))); \
    native_query_init(&name, context, sequence)
struct QueryUnwind {
    NativeQueryCall *query;
    bool returned;
};
static void query_unwind_cleanup(struct QueryUnwind *u) {
    if (!u->returned) native_query_cleanup(u->query);
}
static void query_generic_original(azce_call *frame) {
    NativeQueryCall *q = (void *) frame->args[7];
    ++q->adapters;
    REQUIRE(frame->args[0] == (uintptr_t) &q->adapter && frame->args[1] == q->first && frame->args[2] == q->count && frame->args[3] == (uintptr_t) q->actual_mount && frame->args[8] == (uintptr_t) q->rows.bytes, "Generic native receiver/window/real Mount ref and hidden result remain unchanged");
    cb_forward_rows((void *) frame->args[8], cb_native_query, &q->adapter, q->first, q->count, q->actual_mount);
    q->initialized = true;
    GuardedRows_value(&q->rows)->begin[0]->pad[0] = 0x23;
    struct QueryUnwind guard __attribute__((cleanup(query_unwind_cleanup))) = {q, false};
    action_run(q->inside_adapter);
    guard.returned = true;
    frame->result[0] = frame->args[8];
}
static void query_wrapper_original(azce_call *frame) {
    NativeQueryCall *q = (void *) frame->args[7];
    ++q->wrappers;
    REQUIRE(frame->args[0] == (uintptr_t) &q->context->list && frame->args[1] == q->context->bank && frame->args[2] == q->selector && frame->args[3] == q->first && frame->args[4] == q->count && frame->args[8] == (uintptr_t) q->rows.bytes, "ListServer native client/selector/window and hidden result remain unchanged");
    azce_call adapter = {0};
    adapter.kind = AZCE_KIND_GENERIC;
    adapter.args[0] = (uintptr_t) &q->adapter;
    adapter.args[1] = q->first;
    adapter.args[2] = q->count;
    adapter.args[3] = (uintptr_t) q->actual_mount;
    adapter.args[7] = (uintptr_t) q;
    adapter.args[8] = frame->args[8];
    adapter.original = (uintptr_t) query_generic_original;
    azsdk_cache_dispatch_call(&adapter);
    REQUIRE(adapter.result[0] == adapter.args[8], "Generic native result address unchanged");
    struct QueryUnwind guard __attribute__((cleanup(query_unwind_cleanup))) = {q, false};
    action_run(q->after_adapter);
    guard.returned = true;
    frame->result[0] = frame->args[8];
}
static void native_query_dispatch(void *v) {
    NativeQueryCall *q = v;
    azce_call frame = {0};
    frame.kind = AZCE_KIND_QUERY;
    frame.args[0] = (uintptr_t) &q->context->list;
    frame.args[1] = q->context->bank;
    frame.args[2] = q->selector;
    frame.args[3] = q->first;
    frame.args[4] = q->count;
    frame.args[7] = (uintptr_t) q;
    frame.args[8] = (uintptr_t) q->rows.bytes;
    frame.original = (uintptr_t) query_wrapper_original;
    azsdk_cache_dispatch_call(&frame);
    GuardedRows_check(&q->rows);
    NativeRows *rows = GuardedRows_value(&q->rows);
    REQUIRE(frame.result[0] == frame.args[8] && q->initialized && q->wrappers == 1 && q->adapters == 1 && row_alive == q->initial_alive + 1 && rows->end == rows->begin + 1 && rows->begin[0]->pad[0] == 0x23, "actual native owning rows returned before observer assertion");
    REQUIRE(q->adapter.result_destination == (uintptr_t) q->rows.bytes, "native row destination and caller ownership unchanged without result moves");
    printf("NATIVE_QUERY_ROWS_OK build=%u client=%u row=%u sequence=%u\n", build_id, q->context->bank, rows->begin[0]->value, q->sequence);
    fflush(stdout);
}
struct QueryMessage {
    NativeQueryCall *query;
    Action action;
};
static void query_message_library(void *v) {
    struct QueryMessage *m = v;
    void (*forward)(unsigned, void *, void (*)(void *), void *) = azsdk_cache_test_forward_library;
    if (forward) forward(build_id, &m->query->context->server, thunk, &m->action);
    else action_run(m->action);
}
static void native_query_message(NativeQueryCall *q, Action action, unsigned kind, bool wrong_position) {
    _Alignas(8) unsigned char bytes[0x40] = {0}, arguments[4 * 48] = {0};
    uint16_t native_kind = (uint16_t) kind;
    uint32_t magic = 0x872349ae, type = 1;
    uint32_t fields[] = {((q->context->bank ^ (wrong_position ? 1u : 0u)) << 24) | (q->selector << 16), q->first, q->count, 0};
    uintptr_t begin = (uintptr_t) arguments, end = begin + sizeof arguments;
    memcpy(bytes, &native_kind, 2);
    memcpy(bytes + 4, &magic, 4);
    memcpy(bytes + 8, &q->sequence, 4);
    memcpy(bytes + 0x28, &begin, 8);
    memcpy(bytes + 0x30, &end, 8);
    memcpy(bytes + 0x38, &end, 8);
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(arguments + 48 * i, &type, 4);
        memcpy(arguments + 48 * i + 8, &fields[i], 4);
    }
    struct azcm_borrow borrow = {build_id, &q->context->server, bytes, q->parameter};
    struct QueryMessage m = {q, action};
    azcm_forward_owned(&borrow, (struct azcb_forward) {query_message_library, &m});
}
static void native_query_observed(NativeQueryCall *q) {
    struct azcq_evidence out __attribute__((cleanup(azcq_evidence_drop))) = {0};
    REQUIRE(evidence(&out), "missing actual native query/reply scoped evidence after real rows");
    REQUIRE(out.sequence == q->sequence && out.client == q->context->bank && out.selector == q->selector && out.start == q->first && out.count == q->count && out.cache.birth_epoch && out.cache.source_epoch == q->context->source.owner_epoch && out.cache.row_value == context_database(q->context)->fixture_layout, "query evidence copies exact source epoch, raw row scalar and native request window");
    REQUIRE(same_owner(&out.cache.cache, NATIVE_OWNER(context_selected(q->context))) && same_owner(&out.cache.collector, NATIVE_OWNER(&q->context->list.collector)) && same_owner(&out.cache.mount, NATIVE_OWNER(&q->context->list.mount)) && same_owner(&out.cache.database, NATIVE_OWNER(&context_mount(q->context)->database)), "query evidence retains only genuine weak cache/Collector/Mount/DB control identities");
    struct azcm_envelope copied = {0};
    REQUIRE(azcm_current(&q->context->server, &copied) && copied.sequence == q->sequence && copied.parameter == q->parameter, "copied request envelope/transport context remains in same owned callback");
}
static void born(Context *context) {
    NATIVE_CALL(native, context);
    uint64_t before = api()->counters().next_birth;
    native_call_run(&native, true, false);
    REQUIRE(api()->counters().next_birth == before + 1, "existing actual native birth adapter prerequisite");
}
static void clear_selected(azce_call *frame) {
    Context *c = (void *) frame->args[7];
    REQUIRE(frame->args[0] == (uintptr_t) &c->list, "native clear receiver unchanged");
    fixture_drop(context_selected(c));
}
static void negative_query_body(void *v) {
    NativeQueryCall *q = v;
    native_query_dispatch(q);
    query_evidence_absent();
    native_query_cleanup(q);
}
static void nested_message_body(void *v) {
    query_evidence_absent();
    negative_query_body(v);
}
static void positive_query_body(void *v) {
    NativeQueryCall *q = v;
    query_evidence_absent();
    native_query_dispatch(q);
    native_query_observed(q);
    native_query_cleanup(q);
    native_query_observed(q);
    for (unsigned wrong = 0; wrong < 2; ++wrong) {
        NATIVE_QUERY(nested, q->context, 800 + q->context->bank + wrong);
        native_query_message(&nested, (Action) {nested_message_body, &nested}, wrong ? 0x3000 : 0x7000, wrong);
    }
}
static void positive_and_nested(Context *c) {
    NATIVE_QUERY(q, c, 100 + c->bank);
    query_evidence_absent();
    native_query_message(&q, (Action) {positive_query_body, &q}, 0x3000, false);
    query_evidence_absent();
}
static void foreign_mount(Context *c, Context *foreign) {
    NATIVE_QUERY(q, c, 1200);
    q.actual_mount = &foreign->list.mount;
    native_query_message(&q, (Action) {negative_query_body, &q}, 0x3000, false);
    query_evidence_absent();
}
static void query_selection_change(void *v) {
    Context *c = v;
    azce_call frame = {0};
    frame.kind = AZCE_KIND_SELECTION_CLEAR;
    frame.args[0] = (uintptr_t) &c->list;
    frame.args[7] = (uintptr_t) c;
    frame.original = (uintptr_t) clear_selected;
    azsdk_cache_dispatch_call(&frame);
}
static void selection_change(Context *c, bool inside_adapter) {
    NATIVE_QUERY(q, c, inside_adapter ? 1300 : 1301);
    CacheOwner selected OWNED = fixture_copy(*context_selected(c));
    Action mutation = {query_selection_change, c};
    if (inside_adapter) q.inside_adapter = mutation;
    else q.after_adapter = mutation;
    native_query_message(&q, (Action) {negative_query_body, &q}, 0x3000, false);
    owner_set(context_selected(c), selected);
    query_evidence_absent();
}
static void throwing_query_body(void *v) {
    NativeQueryCall *q = v;
    bool caught = catches(native_query_dispatch, q);
    REQUIRE(caught && q->wrappers == 1 && q->adapters == 1 && !q->initialized && !row_alive, "native query exception forwards exactly once with native caller cleanup");
    query_evidence_absent();
}
static void native_throw(Context *c) {
    NATIVE_QUERY(q, c, 1400);
    q.inside_adapter = (Action) {raise_action, NULL};
    native_query_message(&q, (Action) {throwing_query_body, &q}, 0x3000, false);
    query_evidence_absent();
}
static void nested_query_inside(void *v) {
    Context *c = v;
    NATIVE_QUERY(inner, c, 1500);
    native_query_dispatch(&inner);
    struct azcq_evidence out __attribute__((cleanup(azcq_evidence_drop))) = {0};
    REQUIRE(!evidence(&out), "unfinished outer native query must hide nested reply evidence");
    native_query_cleanup(&inner);
}
static void nested_query_callback(Context *c) {
    NATIVE_QUERY(outer, c, 1500);
    outer.inside_adapter = (Action) {nested_query_inside, c};
    native_query_message(&outer, (Action) {negative_query_body, &outer}, 0x3000, false);
    query_evidence_absent();
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "build arg");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "real source configured");
    factory_entry = dispatch_native_factory;
    reset();
    {
        WORLD(world);
        Context *contexts[] = {&world.a, &world.b, &world.c, &world.d};
        for (size_t i = 0; i < 4; ++i) {
            born(contexts[i]);
            positive_and_nested(contexts[i]);
        }
        foreign_mount(&world.a, &world.b);
        selection_change(&world.a, true);
        selection_change(&world.a, false);
        native_throw(&world.a);
        nested_query_callback(&world.a);
    }
    REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "native query adapters retain no native owners or rows after callbacks");
    printf("PASS native-cache-query build=%u: real dispatcher rows, source owners, reply scope and refusal/cleanup\n", build_id);
    return 0;
}

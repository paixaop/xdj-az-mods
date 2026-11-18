/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual dispatcher/source state; native bodies and register bridge are C fixtures. */
#define SDK_CACHE_DISPATCH_MAIN sdkcachenative_dispatch_baseline
#include "sdkcachedispatch_test.c"
#undef SDK_CACHE_DISPATCH_MAIN
#include "../../src/common/azcachemessage.h"
void azsdk_cache_test_forward_library(unsigned, void *, void (*)(void *), void *) __attribute__((weak));
void azsdk_cache_test_forward_guarded_library(unsigned, void *, void (*)(void *), void *) __attribute__((weak));
typedef struct NativeCall {
    Context *context;
    CreatorOwner creator;
    GuardedOwner result;
    unsigned selections, creations;
    bool discard_holders, result_initialized;
    Action nested;
} NativeCall;
static void native_call_init(NativeCall *n, Context *c) {
    *n = (NativeCall) {.context = c, .creator = creator_new()};
    n->creator->content = 17;
    GuardedOwner_init(&n->result);
}
static void native_call_drop(NativeCall *n) {
    if (n->result_initialized) fixture_drop(GuardedOwner_value(&n->result));
    GuardedOwner_check(&n->result);
    creator_drop(&n->creator);
}
#define NATIVE_CALL(name, context)                              \
    NativeCall name __attribute__((cleanup(native_call_drop))); \
    native_call_init(&name, context)
static void native_create(azce_call *frame) {
    NativeCall *n = (void *) frame->args[7];
    ++n->creations;
    cb_forward_owner((void *) frame->args[8], cb_native_create, (void *) frame->args[0], (void *) frame->args[1]);
    n->result_initialized = true;
    frame->result[0] = frame->args[8];
    action_run(n->nested);
    if (n->discard_holders) {
        creator_drop(&n->creator);
        fixture_drop(&n->context->list.collector);
        n->context->list.collector = collector_new();
    }
}
static void native_store(azce_call *frame) {
    NativeCall *n = (void *) frame->args[7];
    owner_set(context_selected(n->context), *GuardedOwner_value(&n->result));
}
static void native_select(azce_call *frame) {
    NativeCall *n = (void *) frame->args[7];
    ++n->selections;
    owner_set(&n->creator->database, context_mount(n->context)->database);
    azce_call creation = {0};
    creation.kind = AZCE_KIND_CREATE;
    creation.args[0] = (uintptr_t) context_collector(n->context);
    creation.args[1] = (uintptr_t) &n->creator;
    creation.args[7] = (uintptr_t) n;
    creation.args[8] = (uintptr_t) n->result.bytes;
    creation.original = (uintptr_t) native_create;
    azsdk_cache_dispatch_call(&creation);
    REQUIRE(creation.result[0] == creation.args[8], "native Collector hidden result address unchanged");
    azce_call publication = {0};
    publication.kind = AZCE_KIND_SELECTION_STORE;
    publication.args[0] = (uintptr_t) &n->context->list;
    publication.args[1] = 1;
    publication.args[2] = n->context->bank;
    publication.args[3] = (uintptr_t) n->result.bytes;
    publication.args[7] = (uintptr_t) n;
    publication.original = (uintptr_t) native_store;
    azsdk_cache_dispatch_call(&publication);
    fixture_drop(GuardedOwner_value(&n->result));
    n->result_initialized = false;
    GuardedOwner_check(&n->result);
    frame->result[0] = 16;
}
static void native_run_selection(void *v) {
    NativeCall *n = v;
    azce_call frame = {0};
    frame.kind = AZCE_KIND_SELECTION;
    frame.args[0] = (uintptr_t) &n->context->list;
    frame.args[1] = 1;
    frame.args[2] = n->context->bank;
    frame.args[3] = (uintptr_t) &n->creator;
    frame.args[7] = (uintptr_t) n;
    frame.original = (uintptr_t) native_select;
    azsdk_cache_dispatch_call(&frame);
    REQUIRE(frame.result[0] == 16, "native selection item count unchanged");
}
struct NativeLibrary {
    NativeCall *call;
    bool guarded;
};
static void native_run_library(void *v) {
    struct NativeLibrary *l = v;
    NativeCall *n = l->call;
    void (*guarded)(unsigned, void *, void (*)(void *), void *) = azsdk_cache_test_forward_guarded_library;
    void (*ordinary)(unsigned, void *, void (*)(void *), void *) = azsdk_cache_test_forward_library;
    if (l->guarded && guarded) azsdk_cache_test_forward_guarded_library(build_id, &n->context->server, native_run_selection, n);
    else if (ordinary) azsdk_cache_test_forward_library(build_id, &n->context->server, native_run_selection, n);
    else native_run_selection(n);
}
static void native_call_run(NativeCall *n, bool supported, bool guarded) {
    _Alignas(8) unsigned char message[0x40] = {0}, arguments[96] = {0};
    uint16_t kind = supported ? 0x2102 : 0x7000;
    uint32_t magic = 0x872349ae, type = 1, selector = (n->context->bank << 24) | (1u << 16);
    uintptr_t begin = (uintptr_t) arguments, end = begin + sizeof arguments;
    memcpy(message, &kind, 2);
    memcpy(message + 4, &magic, 4);
    memcpy(message + 0x28, &begin, 8);
    memcpy(message + 0x30, &end, 8);
    memcpy(message + 0x38, &end, 8);
    memcpy(arguments, &type, 4);
    memcpy(arguments + 8, &selector, 4);
    memcpy(arguments + 48, &type, 4);
    struct azcm_borrow borrow = {build_id, &n->context->server, message, 0};
    struct NativeLibrary l = {n, guarded};
    azcm_forward_owned(&borrow, (struct azcb_forward) {native_run_library, &l});
    REQUIRE(n->selections == 1 && n->creations == 1 && context_selected(n->context)->object, "real native selection/creation/result ran exactly once before observation checks");
    printf("NATIVE_SELECTION_OWNER_OK build=%u client=%u\n", build_id, n->context->bank);
    fflush(stdout);
}
static void uninstalled_observer(void) {
    reset();
    WORLD(world);
    REQUIRE(!azsdk_cache_capture_ready(), "startup admission begins unavailable");
    NATIVE_CALL(call, &world.a);
    native_call_run(&call, true, true);
    REQUIRE(!api()->counters().next_birth, "production route must not observe before complete startup admission");
}
static void native_join(void) {
    reset();
    WORLD(world);
    Context *contexts[] = {&world.a, &world.b, &world.c, &world.d};
    for (size_t i = 0; i < 4; ++i) {
        Context *c = contexts[i];
        uint64_t before = api()->counters().next_birth;
        NATIVE_CALL(call, c);
        native_call_run(&call, true, false);
        REQUIRE(api()->counters().next_birth == before + 1, "missing actual owned message/selection/Collector source-birth adapter");
        Queried selected QUERIED = query(c);
        REQUIRE(selected.status == AZS_OK && selected.witness.source_epoch == c->source.owner_epoch && selected.witness.cache.object == context_selected(c)->object && !fixture_expired(selected.witness.cache), "native selected-cache publication must preserve its genuine cache birth");
        OwnerVector *caches = &context_collector(c)->caches;
        REQUIRE(caches->end != caches->begin && owners_equal(*context_selected(c), caches->end[-1]), "observer used the actual native shared owner, never a fabricated cache owner");
    }
}
static void released_holders(void) {
    reset();
    WORLD(world);
    NATIVE_CALL(call, &world.a);
    call.discard_holders = true;
    native_call_run(&call, true, false);
    REQUIRE(api()->counters().next_birth == 1, "pre-forward owner snapshots survive native Creator/Collector holder destruction");
}
static void unsupported_message(void) {
    reset();
    WORLD(world);
    NATIVE_CALL(call, &world.a);
    native_call_run(&call, false, false);
    REQUIRE(!api()->counters().next_birth, "unsupported parent message forwards without source admission");
}
#ifndef SDK_CACHE_NATIVE_MAIN
#define SDK_CACHE_NATIVE_MAIN main
#endif
int SDK_CACHE_NATIVE_MAIN(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "build arg");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "real source configured");
    factory_entry = dispatch_native_factory;
    uninstalled_observer();
    native_join();
    released_holders();
    unsupported_message();
    REQUIRE(!cache_alive && !collector_alive && !pool_alive && !row_alive, "entry adapters retain no native owner beyond the invocation");
    printf("PASS native-cache-entry build=%u: actual source/selection/Collector ownership\n", build_id);
    return 0;
}

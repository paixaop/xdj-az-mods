/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azsources.h"
#include "../common/azcachemessage.h"
#include "../common/azcachebirth.h"
#include "../common/azcacheentry.h"
#include "../common/azcachereplynative.h"
#include <string.h>

/* Standalone message/routing fixtures can omit the source implementation.
 * Production links the strong definitions in sdk_native_sources.c. */
void azcb_forward_library_owned(unsigned, void *, struct azcb_forward) __attribute__((weak));
void azcb_observe_entry_owned(struct azce_call *, struct azcb_forward) __attribute__((weak));
void azcr_forward_ticket_owned(struct azce_call *, struct azcb_forward) __attribute__((weak));
void azcr_forward_reception_owned(struct azce_call *, struct azcb_forward) __attribute__((weak));

static void cache_original(void *context) {
    azsdk_cache_invoke_original(((struct azce_call *) (context)));
}
struct FactoryForward {
    struct azce_call *call;
    const void *raw_result;
};
static void factory_original(void *context) {
    __auto_type forward = &(*((struct FactoryForward *) (context)));
    azsdk_cache_invoke_original(forward->call);
    // These two exact factory entries return a new raw pointer in X0, not
    // a shared owner or an X8 result. Copy it only after normal forwarding.
    forward->raw_result = ((const void *) (forward->call->result[0]));
}
static void cache_factory(struct azce_call *call, unsigned kind) {
    struct FactoryForward forward = {call, NULL};
    struct azcb_factory_borrow borrow = {
        ((const void *) (call->args[0])), kind,
        ((const void *) (call->args[1])), &forward.raw_result};
    // The existing helper requires an eligible same-invocation Collector
    // birth scope. Entry identity/registers alone grant no owner or source.
    azcb_observe_factory_owned(&borrow, (struct azcb_forward) {factory_original, &forward});
}
struct WriteForward {
    struct azce_call *call;
    int status;
};
static void write_original(void *context) {
    __auto_type forward = &(*((struct WriteForward *) (context)));
    azsdk_cache_invoke_original(forward->call);
    // Both reviewed entries return int in W0. Preserve the complete result
    // frame and copy only those 32 bits after normal native return.
    const uint32_t bits = ((uint32_t) (forward->call->result[0]));
    _Static_assert(sizeof forward->status == sizeof bits, "native W0 int width");
    memcpy(&forward->status, &bits, sizeof bits);
}
static void cache_write(struct azce_call *call, unsigned kind) {
    struct WriteForward forward = {call, -1};
    struct azcb_write_borrow borrow = {
        ((const void *) (call->args[0])), kind,
        ((unsigned) (((uint32_t) (call->args[1])))),
        kind == AZCB_SETUP_WRITE ? ((const void *) (call->args[2])) : NULL,
        &forward.status};
    // Raw registers supply no shared ownership. The existing helper retires
    // every write and admits construction only in its eligible pinned scope.
    azcb_observe_construction_write_owned(&borrow, (struct azcb_forward) {write_original, &forward});
}

enum CacheDispatchPolicy {
    CACHE_FORWARD,
    CACHE_TICKET,
    CACHE_RECEPTION,
    CACHE_OBSERVE,
    CACHE_FACTORY,
    CACHE_WRITE,
    CACHE_SELECT,
    CACHE_RETIRE
};
static enum CacheDispatchPolicy cache_dispatch_policy(unsigned kind) {
    static const unsigned char policies[AZCE_KIND_COUNT] = {
        [AZCE_KIND_REPLY_TICKET] = CACHE_TICKET,
        [AZCE_KIND_REPLY_RECEPTION] = CACHE_RECEPTION,
        [AZCE_KIND_SELECTION] = CACHE_OBSERVE,
        [AZCE_KIND_CREATE] = CACHE_OBSERVE,
        [AZCE_KIND_QUERY] = CACHE_OBSERVE,
        [AZCE_KIND_GENERIC] = CACHE_OBSERVE,
        [AZCE_KIND_TRACKINFO] = CACHE_OBSERVE,
        [AZCE_KIND_PRIMARY_FACTORY] = CACHE_FACTORY,
        [AZCE_KIND_STANDARD_FACTORY] = CACHE_FACTORY,
        [AZCE_KIND_SETUP] = CACHE_WRITE,
        [AZCE_KIND_LAYOUT] = CACHE_WRITE,
        [AZCE_KIND_SELECTION_STORE] = CACHE_SELECT,
        [AZCE_KIND_SELECTION_CLEAR] = CACHE_SELECT,
        [AZCE_KIND_CLEAR] = CACHE_RETIRE,
        [AZCE_KIND_CACHE_DESTROY] = CACHE_RETIRE,
        [AZCE_KIND_POOL_GROW] = CACHE_RETIRE,
        [AZCE_KIND_COLLECTOR_DISPOSE] = CACHE_RETIRE,
        [AZCE_KIND_SERVER_RESET] = CACHE_RETIRE,
        [AZCE_KIND_SERVER_DESTROY] = CACHE_RETIRE,
        [AZCE_KIND_LOADINFO_DESTROY] = CACHE_RETIRE,
        [AZCE_KIND_COLLECTOR_DESTROY] = CACHE_RETIRE,
        [AZCE_KIND_COLLECTOR_DELETE] = CACHE_RETIRE,
    };
    return kind < AZCE_KIND_COUNT ? policies[kind] : CACHE_FORWARD;
}
static void cache_observer_dispatch(struct azce_call *call, void (*observer)(struct azce_call *, struct azcb_forward)) {
    if (observer) observer(call, (struct azcb_forward) {cache_original, call});
    else azsdk_cache_invoke_original(call);
}
static void cache_factory_dispatch(struct azce_call *call) {
    cache_factory(call, call->kind == AZCE_KIND_PRIMARY_FACTORY ? AZCB_PRIMARY_FACTORY : AZCB_STANDARD_FALLBACK_FACTORY);
}
static void cache_write_dispatch(struct azce_call *call) {
    cache_write(call, call->kind == AZCE_KIND_SETUP ? AZCB_SETUP_WRITE : AZCB_LAYOUT_WRITE);
}
static void cache_dispatch_action(struct azce_call *call, enum CacheDispatchPolicy policy) {
    struct azcb_forward original = {cache_original, call};
    switch (policy) {
    case CACHE_TICKET: cache_observer_dispatch(call, azcr_forward_ticket_owned); return;
    case CACHE_RECEPTION: cache_observer_dispatch(call, azcr_forward_reception_owned); return;
    case CACHE_OBSERVE: cache_observer_dispatch(call, azcb_observe_entry_owned); return;
    case CACHE_FACTORY: cache_factory_dispatch(call); return;
    case CACHE_WRITE: cache_write_dispatch(call); return;
    case CACHE_SELECT: azcb_selection_forward(original); return;
    case CACHE_RETIRE:
        /* Conservative retirement needs no receiver borrow. Its scope must
         * include native mutation and exception unwinding. */
        azcb_retire_forward(original);
        return;
    default: azsdk_cache_invoke_original(call);
    }
}
void azsdk_cache_dispatch_call(struct azce_call *call) {
    if (call) cache_dispatch_action(call, cache_dispatch_policy(call->kind));
}

struct NativeMessage {
    unsigned build;
    void *server, *message;
    unsigned parameter;
    azsdk_source_message_original original;
};
static void message_original(void *opaque) {
    struct NativeMessage *call = opaque;
    call->original(call->server, call->message, call->parameter);
}
static void message_library(void *opaque) {
    struct NativeMessage *call = opaque;
    if (azcb_forward_library_owned) azcb_forward_library_owned(call->build, call->server, (struct azcb_forward) {message_original, call});
    else message_original(call);
}
void azsdk_sources_forward_owned_message(int build, void *server, void *message, unsigned parameter, azsdk_source_message_original original) {
    if (!original) return;
    struct azcm_borrow borrow = {(unsigned) build, server, message, parameter};
    struct NativeMessage call = {(unsigned) build, server, message, parameter, original};
    azcm_forward_owned(&borrow, (struct azcb_forward) {message_library, &call});
}

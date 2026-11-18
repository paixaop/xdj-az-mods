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

void azsdk_cache_dispatch_call(struct azce_call *call) {
    if (!call) return;
    switch (call->kind) {
    case AZCE_KIND_REPLY_TICKET:
        if (azcr_forward_ticket_owned)
            azcr_forward_ticket_owned(call, (struct azcb_forward) {cache_original, call});
        else azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_REPLY_RECEPTION:
        if (azcr_forward_reception_owned)
            azcr_forward_reception_owned(call, (struct azcb_forward) {cache_original, call});
        else azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_SELECTION:
    case AZCE_KIND_CREATE:
    case AZCE_KIND_QUERY:
    case AZCE_KIND_GENERIC:
    case AZCE_KIND_TRACKINFO:
        if (azcb_observe_entry_owned) {
            azcb_observe_entry_owned(call, (struct azcb_forward) {cache_original, call});
            return;
        }
        azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_PRIMARY_FACTORY:
        cache_factory(call, AZCB_PRIMARY_FACTORY);
        return;
    case AZCE_KIND_STANDARD_FACTORY:
        cache_factory(call, AZCB_STANDARD_FALLBACK_FACTORY);
        return;
    case AZCE_KIND_SETUP:
        cache_write(call, AZCB_SETUP_WRITE);
        return;
    case AZCE_KIND_LAYOUT:
        cache_write(call, AZCB_LAYOUT_WRITE);
        return;
    case AZCE_KIND_SELECTION_STORE:
    case AZCE_KIND_SELECTION_CLEAR:
        azcb_selection_forward((struct azcb_forward) {cache_original, call});
        return;
    case AZCE_KIND_CLEAR:
    case AZCE_KIND_CACHE_DESTROY:
    case AZCE_KIND_POOL_GROW:
    case AZCE_KIND_COLLECTOR_DISPOSE:
    case AZCE_KIND_SERVER_RESET:
    case AZCE_KIND_SERVER_DESTROY:
    case AZCE_KIND_LOADINFO_DESTROY:
    case AZCE_KIND_COLLECTOR_DESTROY:
    case AZCE_KIND_COLLECTOR_DELETE:
        // Global conservative retirement needs no native receiver borrow.
        // Forward inside its scope so mutation and exception paths invalidate
        // earlier witnesses before the original can alter native storage.
        azcb_retire_forward((struct azcb_forward) {cache_original, call});
        return;
    default:
        // Factory/query/selection observation still requires qualified owned
        // context; register preservation alone cannot supply it.
        azsdk_cache_invoke_original(call);
    }
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

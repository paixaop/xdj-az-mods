/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azsources.h"
#include "../common/azcachemessage.h"
#include "../common/azcachebirth.h"
#include "../common/azcacheentry.h"
#include "../common/azcachereplynative.h"
#include <cstring>

/* Standalone message/routing fixtures can omit the source implementation.
 * Production links the strong definitions in sdk_native_sources.cpp. */
namespace azsdk_cache_birth {
void forward_library_owned(unsigned, void *, Forward) __attribute__((weak));
void observe_entry_owned(azce_call *, Forward) __attribute__((weak));
}
namespace azsdk_cache_reply {
void forward_ticket_owned(azce_call *, azsdk_cache_birth::Forward) __attribute__((weak));
void forward_reception_owned(azce_call *, azsdk_cache_birth::Forward) __attribute__((weak));
}

namespace {
void cache_original(void *context) {
    azsdk_cache_invoke_original(static_cast<azce_call *>(context));
}
struct FactoryForward {
    azce_call *call;
    const void *raw_result;
};
void factory_original(void *context) {
    auto &forward = *static_cast<FactoryForward *>(context);
    azsdk_cache_invoke_original(forward.call);
    // These two exact factory entries return a new raw pointer in X0, not
    // a shared owner or an X8 result. Copy it only after normal forwarding.
    forward.raw_result = reinterpret_cast<const void *>(forward.call->result[0]);
}
void cache_factory(azce_call *call, unsigned kind) {
    FactoryForward forward {call, nullptr};
    azsdk_cache_birth::FactoryBorrow borrow {
        reinterpret_cast<const void *>(call->args[0]), kind,
        reinterpret_cast<const void *>(call->args[1]), &forward.raw_result
    };
    // The existing helper requires an eligible same-invocation Collector
    // birth scope. Entry identity/registers alone grant no owner or source.
    azsdk_cache_birth::observe_factory_owned(borrow, {factory_original, &forward});
}
struct WriteForward {
    azce_call *call;
    int status;
};
void write_original(void *context) {
    auto &forward = *static_cast<WriteForward *>(context);
    azsdk_cache_invoke_original(forward.call);
    // Both reviewed entries return int in W0. Preserve the complete result
    // frame and copy only those 32 bits after normal native return.
    const std::uint32_t bits = static_cast<std::uint32_t>(forward.call->result[0]);
    static_assert(sizeof forward.status == sizeof bits, "native W0 int width");
    std::memcpy(&forward.status, &bits, sizeof bits);
}
void cache_write(azce_call *call, unsigned kind) {
    WriteForward forward {call, -1};
    azsdk_cache_birth::WriteBorrow borrow {
        reinterpret_cast<const void *>(call->args[0]), kind,
        static_cast<unsigned>(static_cast<std::uint32_t>(call->args[1])),
        kind == azsdk_cache_birth::SetupWrite ? reinterpret_cast<const void *>(call->args[2]) : nullptr,
        &forward.status
    };
    // Raw registers supply no shared ownership. The existing helper retires
    // every write and admits construction only in its eligible pinned scope.
    azsdk_cache_birth::observe_construction_write_owned(borrow, {write_original, &forward});
}
}

extern "C" void azsdk_cache_dispatch_call(azce_call *call) {
    if (!call) return;
    switch (call->kind) {
    case AZCE_KIND_REPLY_TICKET:
        if (azsdk_cache_reply::forward_ticket_owned)
            azsdk_cache_reply::forward_ticket_owned(call, {cache_original, call});
        else azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_REPLY_RECEPTION:
        if (azsdk_cache_reply::forward_reception_owned)
            azsdk_cache_reply::forward_reception_owned(call, {cache_original, call});
        else azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_SELECTION:
    case AZCE_KIND_CREATE:
    case AZCE_KIND_QUERY:
    case AZCE_KIND_GENERIC:
    case AZCE_KIND_TRACKINFO:
        if (azsdk_cache_birth::observe_entry_owned) {
            azsdk_cache_birth::observe_entry_owned(call, {cache_original, call});
            return;
        }
        azsdk_cache_invoke_original(call);
        return;
    case AZCE_KIND_PRIMARY_FACTORY:
        cache_factory(call, azsdk_cache_birth::PrimaryFactory);
        return;
    case AZCE_KIND_STANDARD_FACTORY:
        cache_factory(call, azsdk_cache_birth::StandardFallbackFactory);
        return;
    case AZCE_KIND_SETUP:
        cache_write(call, azsdk_cache_birth::SetupWrite);
        return;
    case AZCE_KIND_LAYOUT:
        cache_write(call, azsdk_cache_birth::LayoutWrite);
        return;
    case AZCE_KIND_SELECTION_STORE:
    case AZCE_KIND_SELECTION_CLEAR:
        azsdk_cache_birth::selection_forward({cache_original, call});
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
        azsdk_cache_birth::retire_forward({cache_original, call});
        return;
    default:
        // Factory/query/selection observation still requires qualified owned
        // context; register preservation alone cannot supply it.
        azsdk_cache_invoke_original(call);
    }
}

extern "C" void azsdk_sources_forward_owned_message(
    int build, void *server, void *message, unsigned parameter,
    azsdk_source_message_original original) {
    if (!original) return;
    azsdk_cache_message::Borrow borrow {static_cast<unsigned>(build), server, message, parameter};
    azsdk_cache_message::forward_owned(borrow, [&]() {
        struct NativeMessage {
            void *server, *message;
            unsigned parameter;
            azsdk_source_message_original original;
            static void forward(void *p) {
                auto &call = *static_cast<NativeMessage *>(p);
                call.original(call.server, call.message, call.parameter);
            }
        } call {server, message, parameter, original};
        if (azsdk_cache_birth::forward_library_owned)
            azsdk_cache_birth::forward_library_owned(static_cast<unsigned>(build), server,
                                                     {NativeMessage::forward, &call});
        else NativeMessage::forward(&call);
    });
}

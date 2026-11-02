/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual dispatcher/source state; only the machine register bridge and native
 * bodies are fixture implementations. No independent source/cache model. */
#define SDK_CACHE_DISPATCH_MAIN sdkcachenative_dispatch_baseline
#include "sdkcachedispatch_test.cpp"
#undef SDK_CACHE_DISPATCH_MAIN

#if defined(__linux__)
#include "../../src/common/azcachemessage.h"
extern "C" void azsdk_cache_test_forward_library(unsigned, void *, void (*)(void *), void *)
    __attribute__((weak));
extern "C" void azsdk_cache_test_forward_guarded_library(unsigned, void *, void (*)(void *), void *)
    __attribute__((weak));

namespace {
struct NativeCall {
    Context &context;
    CreatorOwner creator;
    Guarded<CacheOwner> result;
    unsigned selections = 0, creations = 0;
    bool discard_holders = false;
    std::function<void()> nested;
    explicit NativeCall(Context &value) : context(value), creator(new ListCacheCreator) {
        creator->content = 17;
        // The native selection original, not our observer, supplies this DB.
    }
    static void create(azce_call *frame) {
        auto &self = *reinterpret_cast<NativeCall *>(frame->args[7]);
        ++self.creations;
        cb_forward_owner(reinterpret_cast<void *>(frame->args[8]), cb_native_create,
            reinterpret_cast<ListCacheCollector *>(frame->args[0]),
            reinterpret_cast<CreatorOwner *>(frame->args[1]));
        frame->result[0] = frame->args[8];
        if (self.nested) self.nested();
        // Valid native return: result has its own real owner. Destroying the
        // Creator and replacing its holder must not trigger any observer read
        // of that Creator after forwarding, nor a raw Collector/pool reread.
        if (self.discard_holders) {
            self.creator.reset();
            self.context.list.collector.reset(new ListCacheCollector);
        }
    }
    static void select(azce_call *frame) {
        auto &self = *reinterpret_cast<NativeCall *>(frame->args[7]);
        ++self.selections;
        self.creator->database = self.context.mount().database;
        azce_call creation = {};
        creation.kind = AZCE_KIND_CREATE;
        creation.args[0] = reinterpret_cast<std::uintptr_t>(self.context.list.collector.get());
        creation.args[1] = reinterpret_cast<std::uintptr_t>(&self.creator);
        creation.args[7] = reinterpret_cast<std::uintptr_t>(&self);
        creation.args[8] = reinterpret_cast<std::uintptr_t>(self.result.bytes);
        creation.original = reinterpret_cast<std::uintptr_t>(&create);
        azsdk_cache_dispatch_call(&creation);
        REQUIRE(creation.result[0] == creation.args[8], "native Collector X8 result address unchanged");
        azce_call publication = {};
        publication.kind = AZCE_KIND_SELECTION_STORE;
        publication.args[0] = reinterpret_cast<std::uintptr_t>(&self.context.list);
        publication.args[1] = 1;
        publication.args[2] = self.context.bank;
        publication.args[3] = reinterpret_cast<std::uintptr_t>(self.result.bytes);
        publication.args[7] = reinterpret_cast<std::uintptr_t>(&self);
        publication.original = reinterpret_cast<std::uintptr_t>(&store);
        azsdk_cache_dispatch_call(&publication);
        self.result.value().~CacheOwner(); self.result.check();
        frame->result[0] = 16;
    }
    static void store(azce_call *frame) {
        auto &self = *reinterpret_cast<NativeCall *>(frame->args[7]);
        self.context.selected() = self.result.value();
    }
    static void run_selection(void *value) {
        auto &self = *static_cast<NativeCall *>(value);
        azce_call frame = {};
        frame.kind = AZCE_KIND_SELECTION;
        frame.args[0] = reinterpret_cast<std::uintptr_t>(&self.context.list);
        frame.args[1] = 1;
        frame.args[2] = self.context.bank;
        frame.args[3] = reinterpret_cast<std::uintptr_t>(&self.creator);
        frame.args[7] = reinterpret_cast<std::uintptr_t>(&self);
        frame.original = reinterpret_cast<std::uintptr_t>(&select);
        azsdk_cache_dispatch_call(&frame);
        REQUIRE(frame.result[0] == 16, "native selection item count unchanged");
    }
    void run(bool supported_message = true, bool guarded = false) {
        alignas(8) unsigned char message[0x40] = {}, arguments[96] = {};
        std::uint16_t kind = supported_message ? 0x2102 : 0x7000;
        std::uint32_t magic = 0x872349ae, type = 1;
        std::uint32_t selector = (context.bank << 24) | (1u << 16);
        auto begin = reinterpret_cast<std::uintptr_t>(arguments), end = begin + sizeof arguments;
        std::memcpy(message, &kind, sizeof kind); std::memcpy(message + 4, &magic, sizeof magic);
        std::memcpy(message + 0x28, &begin, 8); std::memcpy(message + 0x30, &end, 8);
        std::memcpy(message + 0x38, &end, 8); std::memcpy(arguments, &type, 4);
        std::memcpy(arguments + 8, &selector, 4); std::memcpy(arguments + 48, &type, 4);
        azsdk_cache_message::Borrow borrow {build_id, &context.server, message, 0};
        azsdk_cache_message::forward_owned(borrow, [&] {
            if (guarded && azsdk_cache_test_forward_guarded_library)
                azsdk_cache_test_forward_guarded_library(build_id, &context.server, run_selection, this);
            else if (azsdk_cache_test_forward_library)
                azsdk_cache_test_forward_library(build_id, &context.server, run_selection, this);
            else run_selection(this);
        });
        REQUIRE(selections == 1 && creations == 1 && context.selected(),
                "real native selection/creation/result ran exactly once before observation checks");
        std::printf("NATIVE_SELECTION_OWNER_OK build=%u client=%u\n", build_id, context.bank);
        std::fflush(stdout);
    }
};
void uninstalled_observer() {
    reset(); World world;
    REQUIRE(!azsdk_cache_capture_ready(), "startup admission begins unavailable");
    NativeCall call(world.a); call.run(true, true);
    REQUIRE(!api().counters().next_birth,
            "production route must not observe before complete startup admission");
}
void native_join() {
    reset(); World world;
    for (auto context : {&world.a, &world.b, &world.c, &world.d}) {
        auto before = api().counters().next_birth;
        NativeCall call(*context); call.run();
        REQUIRE(api().counters().next_birth == before + 1,
                "missing actual owned message/selection/Collector source-birth adapter");
        auto selected = query(*context);
        REQUIRE(selected.status == AZS_OK && selected.witness.source_epoch == context->source.owner_epoch &&
                selected.witness.cache.lock().get() == context->selected().get(),
                "native selected-cache publication must preserve its genuine cache birth");
        REQUIRE(!context->selected().owner_before(context->list.collector->caches.back()) &&
                !context->list.collector->caches.back().owner_before(context->selected()),
                "observer used the actual native shared owner, never a fabricated cache owner");
    }
}
void released_holders() {
    reset(); World world;
    NativeCall call(world.a); call.discard_holders = true; call.run();
    REQUIRE(api().counters().next_birth == 1,
            "pre-forward owner snapshots survive native Creator/Collector holder destruction");
}
void unsupported_message() {
    reset(); World world;
    NativeCall call(world.a); call.run(false);
    REQUIRE(!api().counters().next_birth, "unsupported parent message forwards without source admission");
}
}
#ifndef SDK_CACHE_NATIVE_MAIN
#define SDK_CACHE_NATIVE_MAIN main
#endif
int SDK_CACHE_NATIVE_MAIN(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "build arg");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK, "real source configured");
    cache_factory_fixture::factory_entry = dispatch_native_factory;
    uninstalled_observer(); native_join(); released_holders(); unsupported_message();
    REQUIRE(!ListCache::alive && !ListCacheCollector::alive && !FixturePool::alive && !NativeRow::alive,
            "entry adapters retain no native owner beyond the invocation");
    std::printf("PASS native-cache-entry build=%u: actual source/selection/Collector ownership\n", build_id);
    return 0;
}
#endif

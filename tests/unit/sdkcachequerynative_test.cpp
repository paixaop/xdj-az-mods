/* SPDX-License-Identifier: GPL-3.0-only */
/* Real dispatcher/source/cache helpers. Native bodies and the register bridge
 * are fixtures; raw row tag23 is neither TrackID nor committed-load evidence. */
#define SDK_CACHE_NATIVE_MAIN sdkcachequery_native_baseline
#include "sdkcachenative_test.cpp"
#undef SDK_CACHE_NATIVE_MAIN

#if defined(__linux__)
#include "../../src/common/azcachequery.h"
namespace azsdk_cache_query {
bool current_owned(Evidence &) noexcept __attribute__((weak));
}
namespace {
namespace cq = azsdk_cache_query;
bool evidence(cq::Evidence &out) {
    auto reader = cq::current_owned;
    return reader && reader(out);
}
void absent_query() {
    cq::Evidence out;
    out.sequence = 0x12345678; out.cache.source_epoch = UINT64_C(0xabcdef0123456789);
    REQUIRE(!evidence(out) && out.sequence == 0x12345678 && out.cache.source_epoch == UINT64_C(0xabcdef0123456789),
            "unavailable query evidence leaves output unchanged and masks older observations");
}
struct NativeQueryCall {
    Context &context;
    FixtureAdapter adapter;
    Guarded<NativeRows> rows;
    const std::shared_ptr<MountPoint> *actual_mount;
    std::function<void()> inside_adapter, after_adapter;
    unsigned wrappers = 0, adapters = 0;
    unsigned sequence, parameter, first = 0, count = 1, selector = 1;
    unsigned initial_destroyed = NativeRow::destroyed, initial_moves = NativeRows::moves;
    unsigned initial_alive = NativeRow::alive;
    bool initialized = false;

    explicit NativeQueryCall(Context &source, unsigned serial)
        : context(source), adapter(source.selected().get()), actual_mount(&source.list.mount),
          sequence(serial), parameter(0x80000000u | serial) {
        REQUIRE(offset(&adapter, &adapter.cache) == 8 && adapter.cache, "actual adapter borrows selected cache at+8");
    }
    ~NativeQueryCall() { cleanup(); }
    void cleanup() {
        if (!initialized) return;
        const unsigned before = NativeRow::destroyed;
        rows.value().~NativeRows(); initialized = false; rows.check();
        REQUIRE(NativeRow::alive == initial_alive && NativeRow::destroyed == before + 1,
                "native result caller alone destroys returned row and pointer buffer");
    }
    static void generic_original(azce_call *frame) {
        auto &self = *reinterpret_cast<NativeQueryCall *>(frame->args[7]);
        ++self.adapters;
        REQUIRE(frame->args[0] == reinterpret_cast<std::uintptr_t>(&self.adapter) &&
                frame->args[1] == self.first && frame->args[2] == self.count &&
                frame->args[3] == reinterpret_cast<std::uintptr_t>(self.actual_mount) &&
                frame->args[8] == reinterpret_cast<std::uintptr_t>(self.rows.bytes),
                "Generic native receiver/window/real Mount ref and X8 remain unchanged");
        cb_forward_rows(reinterpret_cast<void *>(frame->args[8]), cb_native_query, &self.adapter,
                        self.first, self.count, self.actual_mount);
        self.initialized = true;
        self.rows.value().begin[0]->pad[0] = 0x23;
        try { if (self.inside_adapter) self.inside_adapter(); }
        catch (...) { self.cleanup(); throw; }
        frame->result[0] = frame->args[8];
    }
    static void wrapper_original(azce_call *frame) {
        auto &self = *reinterpret_cast<NativeQueryCall *>(frame->args[7]);
        ++self.wrappers;
        REQUIRE(frame->args[0] == reinterpret_cast<std::uintptr_t>(&self.context.list) &&
                frame->args[1] == self.context.bank && frame->args[2] == self.selector &&
                frame->args[3] == self.first && frame->args[4] == self.count &&
                frame->args[8] == reinterpret_cast<std::uintptr_t>(self.rows.bytes),
                "ListServer native client/selector/window and X8 remain unchanged");
        azce_call adapter = {};
        adapter.kind = AZCE_KIND_GENERIC;
        adapter.args[0] = reinterpret_cast<std::uintptr_t>(&self.adapter);
        adapter.args[1] = self.first; adapter.args[2] = self.count;
        adapter.args[3] = reinterpret_cast<std::uintptr_t>(self.actual_mount);
        adapter.args[7] = reinterpret_cast<std::uintptr_t>(&self);
        adapter.args[8] = frame->args[8];
        adapter.original = reinterpret_cast<std::uintptr_t>(&generic_original);
        azsdk_cache_dispatch_call(&adapter);
        REQUIRE(adapter.result[0] == adapter.args[8], "Generic native result address unchanged");
        try { if (self.after_adapter) self.after_adapter(); }
        catch (...) { self.cleanup(); throw; }
        frame->result[0] = frame->args[8];
    }
    void dispatch() {
        azce_call frame = {};
        frame.kind = AZCE_KIND_QUERY;
        frame.args[0] = reinterpret_cast<std::uintptr_t>(&context.list);
        frame.args[1] = context.bank; frame.args[2] = selector;
        frame.args[3] = first; frame.args[4] = count;
        frame.args[7] = reinterpret_cast<std::uintptr_t>(this);
        frame.args[8] = reinterpret_cast<std::uintptr_t>(rows.bytes);
        frame.original = reinterpret_cast<std::uintptr_t>(&wrapper_original);
        azsdk_cache_dispatch_call(&frame);
        rows.check();
        REQUIRE(frame.result[0] == frame.args[8] && initialized && wrappers == 1 && adapters == 1 &&
                NativeRow::alive == initial_alive + 1 && rows.value().end == rows.value().begin + 1 &&
                rows.value().begin[0]->pad[0] == 0x23,
                "actual native owning rows returned before observer assertion");
#if defined(__aarch64__)
        REQUIRE(adapter.result_destination == reinterpret_cast<std::uintptr_t>(rows.bytes) &&
                NativeRows::moves == initial_moves, "native row destination and caller ownership unchanged");
#endif
        std::printf("NATIVE_QUERY_ROWS_OK build=%u client=%u row=%u sequence=%u\n", build_id, context.bank,
                    rows.value().begin[0]->value, sequence); std::fflush(stdout);
    }
    void message(std::function<void()> action, unsigned kind = 0x3000, bool wrong_position = false) {
        alignas(8) unsigned char bytes[0x40] = {}, arguments[4 * 48] = {};
        const std::uint16_t native_kind = static_cast<std::uint16_t>(kind);
        const std::uint32_t magic = 0x872349ae, type = 1;
        const std::uint32_t fields[] = {
            ((context.bank ^ (wrong_position ? 1u : 0u)) << 24) | (selector << 16), first, count, 0
        };
        const auto begin = reinterpret_cast<std::uintptr_t>(arguments), end = begin + sizeof arguments;
        std::memcpy(bytes, &native_kind, 2); std::memcpy(bytes + 4, &magic, 4);
        std::memcpy(bytes + 8, &sequence, 4);
        std::memcpy(bytes + 0x28, &begin, 8); std::memcpy(bytes + 0x30, &end, 8); std::memcpy(bytes + 0x38, &end, 8);
        for (unsigned i = 0; i != 4; ++i) {
            std::memcpy(arguments + 48 * i, &type, 4);
            std::memcpy(arguments + 48 * i + 8, &fields[i], 4);
        }
        azsdk_cache_message::Borrow borrow {build_id, &context.server, bytes, parameter};
        azsdk_cache_message::forward_owned(borrow, [&] {
            if (azsdk_cache_test_forward_library)
                azsdk_cache_test_forward_library(build_id, &context.server, thunk, &action);
            else action();
        });
    }
    void observed() {
        cq::Evidence out;
        REQUIRE(evidence(out), "missing actual native query/reply scoped evidence after real rows");
        REQUIRE(out.sequence == sequence && out.client == context.bank && out.selector == selector &&
                out.start == first && out.count == count && out.cache.birth_epoch &&
                out.cache.source_epoch == context.source.owner_epoch &&
                out.cache.row_value == context.mount().database->fixture_layout,
                "query evidence copies exact source epoch, raw row scalar and native request window");
        REQUIRE(same_owner(out.cache.cache, context.selected()) &&
                same_owner(out.cache.collector, context.list.collector) &&
                same_owner(out.cache.mount, context.list.mount) && same_owner(out.cache.database, context.mount().database),
                "query evidence retains only genuine weak cache/Collector/Mount/DB control identities");
        azsdk_cache_message::Envelope copied;
        REQUIRE(azsdk_cache_message::current(&context.server, copied) && copied.sequence == sequence &&
                copied.parameter == parameter, "copied request envelope/transport context remains in same owned callback");
    }
};
void born(Context &context) {
    NativeCall native(context); const auto before = api().counters().next_birth;
    native.run();
    REQUIRE(api().counters().next_birth == before + 1, "existing actual native birth adapter prerequisite");
}
void clear_selected(azce_call *frame) {
    auto &context = *reinterpret_cast<Context *>(frame->args[7]);
    REQUIRE(frame->args[0] == reinterpret_cast<std::uintptr_t>(&context.list), "native clear receiver unchanged");
    context.selected().reset();
}
void positive_and_nested(Context &context) {
    NativeQueryCall query(context, 100 + context.bank);
    absent_query();
    query.message([&] {
        absent_query(); query.dispatch(); query.observed();
        query.cleanup(); query.observed(); // evidence owns scalars/weak controls, never row memory
        for (bool wrong_position : {false, true}) {
            NativeQueryCall nested(context, 800 + context.bank + unsigned(wrong_position));
            nested.message([&] {
                absent_query(); nested.dispatch(); absent_query(); nested.cleanup();
            }, wrong_position ? 0x3000 : 0x7000, wrong_position);
        }
    });
    absent_query();
}
void foreign_mount(Context &context, Context &foreign) {
    NativeQueryCall query(context, 1200); query.actual_mount = &foreign.list.mount;
    query.message([&] { query.dispatch(); absent_query(); query.cleanup(); });
    absent_query();
}
void selection_change(Context &context, bool inside_adapter) {
    NativeQueryCall query(context, inside_adapter ? 1300 : 1301);
    const auto selected = context.selected();
    auto mutation = [&] {
        azce_call frame = {}; frame.kind = AZCE_KIND_SELECTION_CLEAR;
        frame.args[0] = reinterpret_cast<std::uintptr_t>(&context.list);
        frame.args[7] = reinterpret_cast<std::uintptr_t>(&context);
        frame.original = reinterpret_cast<std::uintptr_t>(&clear_selected);
        azsdk_cache_dispatch_call(&frame);
    };
    if (inside_adapter) query.inside_adapter = mutation; else query.after_adapter = mutation;
    query.message([&] { query.dispatch(); absent_query(); query.cleanup(); });
    context.selected() = selected; absent_query();
}
void native_throw(Context &context) {
    NativeQueryCall query(context, 1400);
    query.inside_adapter = [] { throw std::runtime_error("native query exact exception"); };
    query.message([&] {
        bool caught = false;
        try { query.dispatch(); }
        catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native query exact exception") == 0; }
        REQUIRE(caught && query.wrappers == 1 && query.adapters == 1 && !query.initialized && !NativeRow::alive,
                "native query exception forwards exactly once with native caller cleanup");
        absent_query();
    });
    absent_query();
}
void nested_query_callback(Context &context) {
    NativeQueryCall outer(context, 1500);
    outer.inside_adapter = [&] {
        NativeQueryCall inner(context, 1500);
        inner.dispatch();
        cq::Evidence out;
        REQUIRE(!evidence(out), "unfinished outer native query must hide nested reply evidence");
        inner.cleanup();
    };
    outer.message([&] { outer.dispatch(); absent_query(); outer.cleanup(); });
    absent_query();
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "build arg");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK, "real source configured");
    cache_factory_fixture::factory_entry = dispatch_native_factory;
    reset();
    {
        World world;
        for (auto context : {&world.a, &world.b, &world.c, &world.d}) {
            born(*context); positive_and_nested(*context);
        }
        foreign_mount(world.a, world.b);
        selection_change(world.a, true); selection_change(world.a, false); native_throw(world.a);
        nested_query_callback(world.a);
    }
    REQUIRE(!ListCache::alive && !ListCacheCollector::alive && !FixturePool::alive && !NativeRow::alive,
            "native query adapters retain no native owners or rows after callbacks");
    std::printf("PASS native-cache-query build=%u: real dispatcher rows, source owners, reply scope and refusal/cleanup\n", build_id);
    return 0;
}
#endif

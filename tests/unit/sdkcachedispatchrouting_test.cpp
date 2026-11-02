/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable routing test: spies test the call order and register-frame contract
 * only. They provide no cache/source service, native ABI or ownership proof. */
#include "../../src/common/azcachebirth.h"
#include "../../src/common/azcacheentry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>

#define CHECK(x, why) do { if (!(x)) { std::fprintf(stderr, "FAIL cache-dispatch-routing: %s\n", why); std::abort(); } } while (0)
namespace {
unsigned retirement_calls, retirement_depth, factory_calls, factory_depth;
unsigned write_calls, write_depth;
const int *active_status;
bool factory_entry(unsigned kind) {
    return kind == AZCE_KIND_PRIMARY_FACTORY || kind == AZCE_KIND_STANDARD_FACTORY;
}
bool write_entry(unsigned kind) { return kind == AZCE_KIND_SETUP || kind == AZCE_KIND_LAYOUT; }
struct WriteReceipt {
    const void *receiver = nullptr, *pool = nullptr;
    unsigned kind = 0, argument = 0;
    std::uint32_t status = 0;
} last_write;
struct FactoryReceipt {
    const void *receiver = nullptr, *pool = nullptr, *result = nullptr;
    unsigned kind = 0;
} last_factory;
void (*frame_original(unsigned kind))(azce_call *);
struct Frame {
    azce_call call = {}, before = {};
    std::uint64_t output[3] = {UINT64_C(0xcafefeed), 0, UINT64_C(0xfedcba98)};
    unsigned forwards = 0, expected_depth;
    bool null_factory = false;
    std::uint32_t status_bits = UINT32_C(0x56789abc);
    std::function<void()> action;
    Frame(unsigned kind, unsigned depth, std::function<void()> work = [] {})
        : expected_depth(depth), action(work) {
        call.kind = kind; call.reserved = 0x8a734951;
        call.caller_link = UINT64_C(0x0123456789abcdef);
        call.original = reinterpret_cast<std::uintptr_t>(frame_original(kind));
        for (unsigned i = 0; i != 9; ++i) call.args[i] = UINT64_C(0x1302000000) + i;
        call.args[7] = reinterpret_cast<std::uintptr_t>(this);
        call.args[8] = reinterpret_cast<std::uintptr_t>(&output[1]);
        if (factory_entry(kind)) {
            call.args[0] = reinterpret_cast<std::uintptr_t>(this);
            call.args[1] = reinterpret_cast<std::uintptr_t>(&output[0]);
        }
        if (write_entry(kind)) {
            call.args[0] = reinterpret_cast<std::uintptr_t>(this);
            call.args[1] = UINT64_C(0xfedcba9800000010);
            call.args[2] = kind == AZCE_KIND_SETUP ? reinterpret_cast<std::uintptr_t>(&output[0]) :
                                                  UINT64_C(0x987654320000ab23);
            call.args[3] = UINT64_C(0x876543210000cd45);
        }
        for (unsigned i = 0; i != 8; ++i) {
            call.vector_args[i].words[0] = UINT64_C(0x1234567800000000) + i;
            call.vector_args[i].words[1] = UINT64_C(0xabcdef0100000000) + i;
        }
        before = call;
    }
    std::uint64_t result0() const {
        if (factory_entry(call.kind)) return null_factory ? 0 : reinterpret_cast<std::uintptr_t>(&output[2]);
        return write_entry(call.kind) ? UINT64_C(0xabcd123400000000) | status_bits : UINT64_C(0xabcd123456789abc);
    }
    void inputs() const {
        CHECK(std::memcmp(call.args, before.args, sizeof call.args) == 0 &&
              std::memcmp(call.vector_args, before.vector_args, sizeof call.vector_args) == 0 &&
              call.kind == before.kind && call.reserved == before.reserved &&
              call.original == before.original && call.caller_link == before.caller_link,
              "original arguments, X8 and caller frame unchanged");
    }
    static void original(azce_call *call) {
        auto &self = *reinterpret_cast<Frame *>(call->args[7]);
        CHECK(&self.call == call, "original receives exact caller-owned frame");
        self.inputs(); ++self.forwards;
        call->result[0] = self.result0();
        call->result[1] = UINT64_C(0x87364951abcdef01);
        for (unsigned i = 0; i != 4; ++i) {
            call->vector_results[i].words[0] = UINT64_C(0x3736000000) + i;
            call->vector_results[i].words[1] = UINT64_C(0x8273000000) + i;
        }
        *reinterpret_cast<std::uint64_t *>(call->args[8]) = UINT64_C(0xfdecab0198765432);
        std::printf("ORIGINAL_FRAME_FORWARD_OK kind=%u depth=%u\n", call->kind, retirement_depth);
        std::fflush(stdout);
        CHECK(write_entry(call->kind) || retirement_depth == self.expected_depth,
              "missing mutation retirement routing before original");
        self.action();
    }
    void results() const {
        inputs();
        CHECK(forwards == 1 && output[0] == UINT64_C(0xcafefeed) &&
              output[1] == UINT64_C(0xfdecab0198765432) && output[2] == UINT64_C(0xfedcba98) &&
              call.result[0] == result0() &&
              call.result[1] == UINT64_C(0x87364951abcdef01), "exactly one original and unchanged result frame");
        for (unsigned i = 0; i != 4; ++i)
            CHECK(call.vector_results[i].words[0] == UINT64_C(0x3736000000) + i &&
                  call.vector_results[i].words[1] == UINT64_C(0x8273000000) + i,
                  "all SIMD result bits preserved");
    }
};
template<unsigned Kind> void distinct_original(azce_call *call) {
    CHECK(call->kind == Kind, "each entry forwards its own distinct original");
    Frame::original(call);
}
void (*frame_original(unsigned kind))(azce_call *) {
    static void (*const originals[21])(azce_call *) = {
        distinct_original<0>, distinct_original<1>, distinct_original<2>, distinct_original<3>,
        distinct_original<4>, distinct_original<5>, distinct_original<6>, distinct_original<7>,
        distinct_original<8>, distinct_original<9>, distinct_original<10>, distinct_original<11>,
        distinct_original<12>, distinct_original<13>, distinct_original<14>, distinct_original<15>,
        distinct_original<16>, distinct_original<17>, distinct_original<18>, distinct_original<19>,
        distinct_original<20>
    };
    return kind < 21 ? originals[kind] : Frame::original;
}
void run(unsigned kind, bool mutation, bool throwing) {
    const unsigned prior = retirement_calls, prior_factory = factory_calls;
    Frame frame(kind, mutation ? 1 : 0, [=] {
        if (throwing) throw std::runtime_error("original exact exception");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&frame.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "original exact exception") == 0; }
    CHECK(caught == throwing, "original exception propagates unchanged");
    frame.results();
    CHECK(factory_calls == prior_factory + (factory_entry(kind) ? 1 : 0),
          "missing native factory entry adaptation");
    if (factory_entry(kind) && !throwing)
        CHECK(last_factory.receiver == reinterpret_cast<void *>(frame.call.args[0]) &&
              last_factory.pool == reinterpret_cast<void *>(frame.call.args[1]) &&
              last_factory.result == reinterpret_cast<void *>(frame.call.result[0]) &&
              last_factory.kind == (kind == AZCE_KIND_PRIMARY_FACTORY ? azsdk_cache_birth::PrimaryFactory :
                                                                      azsdk_cache_birth::StandardFallbackFactory),
              "factory adapter copies proved receiver/pool/raw-result registers only");
    CHECK(!factory_depth, "factory scope restores on return and native exception");
    CHECK(!retirement_depth && retirement_calls == prior + (mutation ? 1 : 0),
          "only known mutations use one retirement forwarding scope");
}
void entry_table() {
    const unsigned ids[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE,
        AZCE_KIND_QUERY, AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO, AZCE_KIND_SETUP, AZCE_KIND_LAYOUT,
        AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW, AZCE_KIND_PRIMARY_FACTORY,
        AZCE_KIND_STANDARD_FACTORY, AZCE_KIND_COLLECTOR_DISPOSE, AZCE_KIND_SELECTION_STORE,
        AZCE_KIND_SELECTION_CLEAR, AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY,
        AZCE_KIND_LOADINFO_DESTROY, AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE};
    for (unsigned kind = 0; kind != 21; ++kind)
        CHECK(ids[kind] == kind, "private numeric entry IDs preserve all existing values and append in order");
    void *entries[21] = {};
    for (unsigned kind = 0; kind != 11; ++kind) {
        entries[kind] = azsdk_cache_entry(kind);
        CHECK(entries[kind], "existing entry remains available");
        for (unsigned prior = 0; prior != kind; ++prior)
            CHECK(entries[prior] != entries[kind], "existing entries remain distinct");
    }
    std::puts("LEGACY_ENTRY_TABLE_OK entries=11"); std::fflush(stdout);
    // Numeric new indices keep OLD production compilable. Missing appended
    // entries, rather than enum/header/link failures, are the intended RED.
    for (unsigned kind = 11; kind != 21; ++kind) {
        entries[kind] = azsdk_cache_entry(kind);
        CHECK(entries[kind], "missing distinct appended cache entries and original slots");
        for (unsigned prior = 0; prior != kind; ++prior)
            CHECK(entries[prior] != entries[kind], "appended entry aliases an existing native redirect");
    }
    CHECK(AZCE_KIND_COUNT == 21, "one independent original slot per entry");
    // Slot isolation is portable. Actual assembly slot indexing/CFI remains
    // separately covered by the AArch64 entry test, not these abort-only stubs.
    for (unsigned kind = 0; kind != 21; ++kind)
        azsdk_cache_entry_originals[kind] = reinterpret_cast<void *>(frame_original(kind));
    for (unsigned kind = 0; kind != 21; ++kind) {
        CHECK(azsdk_cache_entry_originals[kind] == reinterpret_cast<void *>(frame_original(kind)),
              "distinct originals cannot overwrite each other");
        for (unsigned prior = 0; prior != kind; ++prior)
            CHECK(frame_original(prior) != frame_original(kind), "test originals have distinct identities");
        azsdk_cache_entry_originals[kind] = nullptr;
    }
    CHECK(!azsdk_cache_entry(21) && !azsdk_cache_entry(UINT32_MAX), "unknown entries remain unavailable");
}
void null_factory(unsigned kind) {
    const unsigned prior = factory_calls;
    Frame frame(kind, 0); frame.null_factory = true;
    azsdk_cache_dispatch_call(&frame.call); frame.results();
    CHECK(factory_calls == prior + 1 && !last_factory.result && !factory_depth,
          "null factory result forwards unchanged without a fabricated owner");
}
void nested(bool throwing) {
    const unsigned prior = retirement_calls;
    Frame inner(AZCE_KIND_COLLECTOR_DELETE, 2, [=] {
        if (throwing) throw std::runtime_error("nested exact exception");
    });
    Frame outer(AZCE_KIND_SERVER_RESET, 1, [&] {
        Frame forwarded(AZCE_KIND_QUERY, 1);
        azsdk_cache_dispatch_call(&forwarded.call); forwarded.results();
        try { azsdk_cache_dispatch_call(&inner.call); }
        catch (...) { CHECK(retirement_depth == 1, "inner unwind restores outer scope"); throw; }
        CHECK(retirement_depth == 1, "inner return restores outer scope");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&outer.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "nested exact exception") == 0; }
    CHECK(caught == throwing, "nested exact exception survives dispatcher");
    inner.results(); outer.results();
    CHECK(!retirement_depth && retirement_calls == prior + 2, "nested routing scopes finish exactly once");
}
void nested_factory(bool throwing) {
    const unsigned prior = factory_calls;
    Frame inner(AZCE_KIND_STANDARD_FACTORY, 0, [=] {
        CHECK(factory_depth == 2, "nested factory forwarding owns independent local raw-result slot");
        if (throwing) throw std::runtime_error("nested factory exact exception");
    });
    Frame outer(AZCE_KIND_PRIMARY_FACTORY, 0, [&] {
        CHECK(factory_depth == 1, "outer factory wraps only its genuine original");
        try { azsdk_cache_dispatch_call(&inner.call); }
        catch (...) { CHECK(factory_depth == 1, "nested factory exception restores outer scope"); throw; }
        CHECK(factory_depth == 1, "nested factory return restores outer scope");
    });
    bool caught = false;
    try { azsdk_cache_dispatch_call(&outer.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "nested factory exact exception") == 0; }
    CHECK(caught == throwing, "nested factory exception preserved");
    inner.results(); outer.results();
    CHECK(factory_calls == prior + 2 && !factory_depth, "nested factory calls are exact once and unwind");
    if (!throwing)
        CHECK(last_factory.receiver == reinterpret_cast<void *>(outer.call.args[0]) &&
              last_factory.result == reinterpret_cast<void *>(outer.call.result[0]),
              "inner result cannot replace outer factory raw result");
}
void write(unsigned kind, std::uint32_t status, std::uint32_t argument, bool throwing) {
    const unsigned prior = write_calls, prior_retirement = retirement_calls;
    Frame frame(kind, 1, [=] {
        if (active_status) CHECK(*active_status == -1, "status copied only after normal original return");
        if (throwing) throw std::runtime_error("write exact exception");
    });
    frame.status_bits = status;
    frame.call.args[1] = UINT64_C(0xfedcba9800000000) | argument;
    frame.before = frame.call;
    bool caught = false;
    try { azsdk_cache_dispatch_call(&frame.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "write exact exception") == 0; }
    CHECK(caught == throwing, "write native exception propagates unchanged");
    frame.results();
    std::printf("WRITE_NATIVE_FORWARD_OK kind=%u status=%08x\n", kind, status); std::fflush(stdout);
    CHECK(write_calls == prior + 1, "missing native construction write entry adaptation");
    CHECK(!write_depth && !active_status && !retirement_depth && retirement_calls == prior_retirement + 1,
          "one construction write retirement scope restores on normal and exceptional return");
    if (!throwing)
        CHECK(last_write.receiver == reinterpret_cast<void *>(frame.call.args[0]) &&
              last_write.pool == (kind == AZCE_KIND_SETUP ? reinterpret_cast<void *>(frame.call.args[2]) : nullptr) &&
              last_write.kind == (kind == AZCE_KIND_SETUP ? azsdk_cache_birth::SetupWrite : azsdk_cache_birth::LayoutWrite) &&
              last_write.argument == argument && last_write.status == status,
              "write adapter copies only exact receiver, low W1, setup pool and signed W0 bits");
}
void nested_write(bool throwing) {
    const unsigned prior = write_calls, prior_retirement = retirement_calls;
    Frame inner(AZCE_KIND_LAYOUT, 2, [=] {
        CHECK(write_depth == 2 && retirement_depth == 2 && active_status && *active_status == -1,
              "nested write keeps its separate unpopulated status until return");
        if (throwing) throw std::runtime_error("nested write exact exception");
    });
    inner.status_bits = UINT32_C(0x80000000);
    Frame outer(AZCE_KIND_SETUP, 1, [&] {
        CHECK(write_depth == 1 && retirement_depth == 1 && active_status && *active_status == -1,
              "outer write wraps genuine native execution before copying status");
        const int *outer_status = active_status;
        try { azsdk_cache_dispatch_call(&inner.call); }
        catch (...) {
            CHECK(write_depth == 1 && retirement_depth == 1 && active_status == outer_status && *outer_status == -1,
                  "nested write exception restores original outer local status");
            throw;
        }
        CHECK(write_depth == 1 && retirement_depth == 1 && active_status == outer_status && *outer_status == -1,
              "nested write result cannot replace outer local status");
    });
    outer.status_bits = UINT32_C(0x7fffffff);
    bool caught = false;
    try { azsdk_cache_dispatch_call(&outer.call); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "nested write exact exception") == 0; }
    CHECK(caught == throwing, "nested write native exception remains exact");
    inner.results(); outer.results();
    CHECK(write_calls == prior + 2 && retirement_calls == prior_retirement + 2 &&
          !write_depth && !retirement_depth && !active_status, "nested write scopes finish exactly once");
    if (!throwing) CHECK(last_write.receiver == reinterpret_cast<void *>(outer.call.args[0]) &&
                         last_write.status == outer.status_bits, "outer status remains independent of nested return");
}
}
namespace azsdk_cache_birth {
// Explicit transparent fallbacks keep this Darwin/Linux portable; they do
// not provide or pretend to test the separately owned native source scope.
void forward_library_owned(unsigned, void *, Forward original) { original.call(original.context); }
void observe_entry_owned(azce_call *, Forward original) { original.call(original.context); }
void retire_forward(Forward original) {
    CHECK(original.call, "routing supplies a forwarding closure");
    ++retirement_calls;
    struct Scope { Scope() { ++retirement_depth; } ~Scope() { --retirement_depth; } } scope;
    original.call(original.context);
}
void selection_forward(Forward original) {
    // This portable spy checks ordering only; the real-state fixture checks
    // that selected-storage fencing preserves the independent cache birth.
    retire_forward(original);
}
void observe_factory_owned(const FactoryBorrow &borrow, Forward original) {
    CHECK(original.call && borrow.native_result && !*borrow.native_result,
          "factory raw-result slot is observer-local and initially empty");
    ++factory_calls;
    struct Scope { Scope() { ++factory_depth; } ~Scope() { --factory_depth; } } scope;
    original.call(original.context);
    last_factory.receiver = borrow.receiver_creator;
    last_factory.pool = borrow.borrowed_pool;
    last_factory.result = *borrow.native_result;
    last_factory.kind = borrow.kind;
}
void observe_construction_write_owned(const WriteBorrow &borrow, Forward original) {
    CHECK(original.call && borrow.native_status && *borrow.native_status == -1,
          "write status is a separate observer-local int initialized before native call");
    CHECK(borrow.kind == SetupWrite || borrow.kind == LayoutWrite, "only reviewed construction write kinds");
    auto frame = static_cast<Frame *>(const_cast<void *>(borrow.receiver_cache));
    CHECK(frame && borrow.native_status != reinterpret_cast<const int *>(&frame->call.result[0]),
          "write status never aliases native register result storage");
    ++write_calls;
    struct Scope {
        const int *previous;
        explicit Scope(const int *status) : previous(active_status) { active_status = status; ++write_depth; }
        ~Scope() { active_status = previous; --write_depth; }
    } scope(borrow.native_status);
    try { retire_forward(original); }
    catch (...) {
        CHECK(*borrow.native_status == -1, "throwing original never fabricates a returned status");
        throw;
    }
    last_write.receiver = borrow.receiver_cache;
    last_write.pool = borrow.borrowed_pool;
    last_write.kind = borrow.kind;
    last_write.argument = borrow.argument;
    std::memcpy(&last_write.status, borrow.native_status, sizeof last_write.status);
}
}
extern "C" void azsdk_cache_invoke_original(azce_call *call) {
    CHECK(call && call->original, "explicit frame backend has genuine original");
    reinterpret_cast<void (*)(azce_call *)>(call->original)(call);
}
#define UNUSED_ENTRY(n) extern "C" void azsdk_cache_entry_##n() { std::abort(); }
UNUSED_ENTRY(0) UNUSED_ENTRY(1) UNUSED_ENTRY(2) UNUSED_ENTRY(3) UNUSED_ENTRY(4) UNUSED_ENTRY(5)
UNUSED_ENTRY(6) UNUSED_ENTRY(7) UNUSED_ENTRY(8) UNUSED_ENTRY(9) UNUSED_ENTRY(10)
UNUSED_ENTRY(11) UNUSED_ENTRY(12) UNUSED_ENTRY(13) UNUSED_ENTRY(14) UNUSED_ENTRY(15)
UNUSED_ENTRY(16) UNUSED_ENTRY(17) UNUSED_ENTRY(18) UNUSED_ENTRY(19) UNUSED_ENTRY(20)
#undef UNUSED_ENTRY
int main() {
    const unsigned forwarded[] = {AZCE_KIND_MESSAGE, AZCE_KIND_SELECTION, AZCE_KIND_CREATE,
        AZCE_KIND_QUERY, AZCE_KIND_GENERIC, AZCE_KIND_TRACKINFO,
        AZCE_KIND_COUNT, UINT32_MAX};
    for (auto kind : forwarded) { run(kind, false, false); run(kind, false, true); }
    std::puts("ROUTING_FORWARD_BASELINE_OK"); std::fflush(stdout);
    entry_table();
    for (auto kind : {AZCE_KIND_PRIMARY_FACTORY, AZCE_KIND_STANDARD_FACTORY}) {
        run(kind, false, false); run(kind, false, true); null_factory(kind);
    }
    for (auto kind : {AZCE_KIND_SETUP, AZCE_KIND_LAYOUT})
        for (auto status : {UINT32_C(0), UINT32_C(1), UINT32_C(0x80000000), UINT32_C(0xffffffff)}) {
            write(kind, status, status, false); write(kind, status, status, true);
        }
    nested_write(false); nested_write(true);
    for (auto kind : {AZCE_KIND_CLEAR, AZCE_KIND_CACHE_DESTROY, AZCE_KIND_POOL_GROW,
                     AZCE_KIND_COLLECTOR_DISPOSE, AZCE_KIND_SELECTION_STORE, AZCE_KIND_SELECTION_CLEAR,
                     AZCE_KIND_SERVER_RESET, AZCE_KIND_SERVER_DESTROY, AZCE_KIND_LOADINFO_DESTROY,
                     AZCE_KIND_COLLECTOR_DESTROY, AZCE_KIND_COLLECTOR_DELETE}) {
        run(kind, true, false); run(kind, true, true);
    }
    nested(false); nested(true); nested_factory(false); nested_factory(true);
    std::puts("PASS cache-dispatch-routing: factory/write adapters, retirement, exact frames/status bits, exceptions and reentry");
}

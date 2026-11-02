/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic GNU ABI/schema requirement only. These are not recovered native
 * nominal C++ types, native hooks, or content/TrackID/source/load/SQL claims.
 * Caller ownership and serialization of every result/row are preconditions;
 * span checks and the proposed private copier acquire no native liveness. */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
    "FAIL cache-row: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)
#ifndef AZCACHE_ROW_HEADER
#define AZCACHE_ROW_HEADER "../../src/common/azcacherow.h"
#endif
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(AZCACHE_ROW_HEADER)
#include AZCACHE_ROW_HEADER
#else
namespace azsdk_cache_row {
inline bool copy_owned_tag23(const void *, std::uint32_t &) noexcept { return false; }
}
#endif

#if !defined(__linux__) || !defined(__GLIBCXX__)
int main() { std::puts("SKIP: requires Linux and GNU libstdc++"); return 77; }
#else
namespace {
unsigned profile;
struct Counts { unsigned alive = 0, created = 0, destroyed = 0, forwards = 0; } counts[4];
struct CallerOwner { unsigned context; explicit CallerOwner(unsigned c) : context(c) {} };
struct Spec { std::uint8_t kind; std::uint32_t value; bool null; };
struct NativeRow {
    std::uint32_t value;                         // +0 scalar, not full TrackID
    std::uint8_t kind, padding[3];               // +4 byte tag; padding deliberately nonzero
    std::uint32_t auxiliary;
    std::uint8_t flag, padding2[3];
    std::u16string first;                        // +0x10 genuine GNU strings
    std::uint32_t numeric;
    std::uint8_t flag2, padding3[3];
    std::u16string second;                       // +0x38
    std::uint64_t auxiliary2;
    std::u16string third;                        // +0x60
    std::uint32_t fixture_context;               // +0x80 fixture bookkeeping only
    std::uint8_t tail[20];
    NativeRow(unsigned context, const Spec &s)
        : value(s.value), kind(s.kind), padding{0xa5, 0x5a, 0xff}, auxiliary(0x11223344),
          flag(1), padding2{2, 3, 4}, first(65, u'A'), numeric(0x55667788),
          flag2(5), padding3{6, 7, 8}, second(67, u'B'), auxiliary2(0x12345678),
          third(69, u'C'), fixture_context(context), tail{} {
        REQUIRE(context < 4, "four independent caller contexts");
        ++counts[context].alive; ++counts[context].created;
    }
    ~NativeRow() {
        REQUIRE(counts[fixture_context].alive, "native caller destroys each owned row once");
        --counts[fixture_context].alive; ++counts[fixture_context].destroyed;
        // The three real strings are subsequently destroyed by C++ itself.
    }
};
using NativeRows = std::vector<std::unique_ptr<NativeRow>>;
struct Context {
    unsigned id;
    std::shared_ptr<CallerOwner> owner;
    std::vector<Spec> specs;
    explicit Context(unsigned c) : id(c), owner(new CallerOwner(c)) {}
};
static_assert(sizeof(std::u16string) == 32 && sizeof(NativeRow) == 0x98, "GNU native-shaped row width");
static_assert(offsetof(NativeRow, kind) == 4 && offsetof(NativeRow, first) == 0x10 &&
              offsetof(NativeRow, second) == 0x38 && offsetof(NativeRow, third) == 0x60,
              "row schema offsets, not a native nominal type claim");
static_assert(sizeof(NativeRows) == 24 && sizeof(std::unique_ptr<NativeRow>) == 8 &&
              !std::is_trivially_copyable<NativeRows>::value,
              "nontrivial GNU24-byte carrier with owning8-byte pointer slots");
NativeRows native_query(Context &context, unsigned start, unsigned count) {
    REQUIRE(context.owner, "original has its real complete caller owner");
    ++counts[context.id].forwards;
    NativeRows rows;
    if (start < context.specs.size()) {
        const auto available = context.specs.size() - start;
        const auto length = count < available ? count : available;
        for (std::size_t i = 0; i < length; ++i) {
            const auto &spec = context.specs[start + i];
            rows.emplace_back(spec.null ? nullptr : new NativeRow(context.id, spec));
        }
    }
    return rows;
}
struct GuardedResult {
    std::uint64_t before = UINT64_C(0x123456789abcdef0);
    alignas(NativeRows) unsigned char storage[sizeof(NativeRows)];
    std::uint64_t after = UINT64_C(0xfedcba9876543210);
    bool constructed = false;
    NativeRows &rows() { return *reinterpret_cast<NativeRows *>(storage); }
    void check() const { REQUIRE(before == UINT64_C(0x123456789abcdef0) && after == UINT64_C(0xfedcba9876543210), "original result output guards"); }
    void destroy() { if (constructed) { rows().~NativeRows(); constructed = false; } check(); }
    ~GuardedResult() { destroy(); }
};
struct Snapshot {
    std::array<unsigned char, 24> carrier;
    std::vector<NativeRow *> pointers;
    std::vector<std::array<unsigned char, 0x98>> rows;
    explicit Snapshot(NativeRows &native) {
        std::memcpy(carrier.data(), &native, carrier.size());
        for (const auto &p : native) {
            pointers.push_back(p.get()); rows.emplace_back();
            if (p) std::memcpy(rows.back().data(), p.get(), rows.back().size());
        }
    }
    void check(NativeRows &native) const {
        REQUIRE(!std::memcmp(carrier.data(), &native, carrier.size()) && native.size() == pointers.size(),
                "copier leaves the actual owning carrier unchanged");
        for (std::size_t i = 0; i < pointers.size(); ++i)
            REQUIRE(native[i].get() == pointers[i] && (!native[i] || !std::memcmp(rows[i].data(), native[i].get(), 0x98)),
                    "copier does not mutate, release or replace any native row/string");
    }
};
bool forwarded_copy(Context &context, unsigned start, unsigned count, std::uint32_t &out, bool first = false) {
    const auto forwards = counts[context.id].forwards, prior_alive = counts[context.id].alive;
    const auto prior_created = counts[context.id].created, prior_destroyed = counts[context.id].destroyed;
    const auto owners = context.owner.use_count();
    std::weak_ptr<CallerOwner> weak = context.owner;
    GuardedResult result;
    // Genuine nontrivial GNU return into caller-owned storage. Entry bridge/X8
    // preservation belongs to the existing separate ABI suite, not this parser.
    new (result.storage) NativeRows(native_query(context, start, count)); result.constructed = true;
    result.check();
    REQUIRE(counts[context.id].forwards == forwards + 1 && context.owner.use_count() == owners && !weak.expired(),
            "original forwards once without replacing or retaining its genuine caller owner");
    const auto allocated = counts[context.id].created - prior_created;
    REQUIRE(counts[context.id].alive == prior_alive + allocated && counts[context.id].destroyed == prior_destroyed,
            "native output caller owns every newly returned row before copy");
    Snapshot snapshot(result.rows());
    const bool copied = azsdk_cache_row::copy_owned_tag23(result.storage, out);
    snapshot.check(result.rows()); result.check();
    REQUIRE(counts[context.id].alive == prior_alive + allocated && counts[context.id].destroyed == prior_destroyed &&
            context.owner.use_count() == owners, "scalar copy cannot consume native rows or retain caller ownership");
    result.destroy();
    REQUIRE(counts[context.id].alive == prior_alive && counts[context.id].destroyed == prior_destroyed + allocated &&
            context.owner.use_count() == owners && !weak.expired(), "only native result caller cleans each row and its three strings");
    if (first) {
        std::printf("NATIVE_ROW_FORWARD_OK build=%u context=%u carrier24=%zu slot8=%zu row98=%zu forwarded=1 cleaned=%u\n",
                    profile, context.id, sizeof(NativeRows), sizeof(std::unique_ptr<NativeRow>), sizeof(NativeRow), allocated);
        std::fflush(stdout);
    }
    return copied;
}
void refused(Context &context, unsigned start, unsigned count, const char *reason) {
    std::uint32_t value = UINT32_C(0x5a6b7c8d);
    REQUIRE(!forwarded_copy(context, start, count, value) && value == UINT32_C(0x5a6b7c8d), reason);
}
void valid_contexts() {
    const std::uint32_t payloads[4] = {UINT32_C(0x13579bdf), 0, UINT32_MAX, UINT32_C(0x2468ace0)};
    for (unsigned id = 0; id != 4; ++id) {
        std::weak_ptr<CallerOwner> weak;
        std::uint32_t copied = UINT32_C(0x5a6b7c8d);
        {
            Context context(id); weak = context.owner;
            context.specs = {{4, UINT32_C(0xdeadbeef), false}, {0x23, payloads[id], false}, {11, 77, false}};
            const bool found = forwarded_copy(context, 0, 3, copied, id == 0);
            REQUIRE(found && copied == payloads[id], "missing owned tag0x23 scalar copy");
            REQUIRE(!counts[id].alive, "raw scalar survives native result caller destruction");
        }
        REQUIRE(weak.expired() && copied == payloads[id], "copied scalar outlives complete caller without acquiring liveness");
    }
}
void partial_null_duplicate() {
    Context context(0);
    context.specs = {{4, 999, false}, {0x23, 123, false}, {13, 456, false}};
    refused(context, 0, 1, "partial window lacking the tag declines unchanged output");
    refused(context, 2, 1, "late partial window lacking the tag declines unchanged output");
    refused(context, 3, 0, "empty native window declines unchanged output");
    std::uint32_t selected = 0;
    REQUIRE(forwarded_copy(context, 1, 1, selected) && selected == 123, "partial window containing one tagged row copies its scalar");
    context.specs = {{4, 1, false}, {0x23, 123, false}, {0, 0, true}};
    refused(context, 0, 3, "null row after a candidate declines without provisional scalar output");
    context.specs = {{0, 0, true}, {0x23, 123, false}};
    refused(context, 0, 2, "null row before a candidate declines unchanged output");
    context.specs = {{0x23, 123, false}, {0x23, 123, false}};
    refused(context, 0, 2, "duplicate tagged rows decline even when content scalars agree");
    context.specs[1].value = 456;
    refused(context, 0, 2, "contradictory duplicate tagged rows decline unchanged output");
    context.specs.assign(16, Spec{4, 333, false}); context.specs[15] = {0x23, UINT32_MAX, false};
    selected = 0;
    REQUIRE(forwarded_copy(context, 0, 16, selected) && selected == UINT32_MAX, "observation quota16 for the proved families can locate its last tagged row");
    context.specs.emplace_back(Spec{4, 444, false});
    refused(context, 0, 17, "owned span beyond observation quota16 declines before traversal");
}
struct Span { std::uintptr_t begin, end, capacity; };
void malformed_spans() {
    Context context(1);
    std::array<std::unique_ptr<NativeRow>, 4> slots;
    slots[0].reset(new NativeRow(context.id, Spec{0x23, 123, false}));
    slots[1].reset(new NativeRow(context.id, Spec{4, 456, false}));
    const auto base = reinterpret_cast<std::uintptr_t>(slots.data());
    const auto end = base + sizeof slots;
    const auto alive = counts[context.id].alive, destroyed = counts[context.id].destroyed;
    auto decline = [&](const void *carrier, const char *why) {
        std::uint32_t value = UINT32_C(0x5a6b7c8d);
        REQUIRE(!azsdk_cache_row::copy_owned_tag23(carrier, value) && value == UINT32_C(0x5a6b7c8d), why);
        REQUIRE(counts[context.id].alive == alive && counts[context.id].destroyed == destroyed && slots[0] && slots[1],
                "bounded malformed views do not transfer or destroy their still-owned pointees");
    };
    decline(nullptr, "null carrier declines unchanged output");
    const Span bad[] = {{0, 0, 0}, {base, base, end}, {base + 8, base, end},
                        {base, base + 16, base + 8}, {base, base + 1, end},
                        {base, base + 8, end - 1}, {0, base + 8, end}};
    for (const auto &span : bad) decline(&span, "empty or malformed span within actual owned slot storage declines unchanged output");
    // Addresses are actual owned slot storage, not fabricated dangling pointees.
    // Headers deliberately violate native span shape; they establish no liveness.
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "separate fixture profile process");
    profile = unsigned(argv[1][0] - '0');
    valid_contexts(); partial_null_duplicate(); malformed_spans();
    for (const auto &c : counts) REQUIRE(!c.alive && c.created == c.destroyed, "caller-only cleanup balances all four contexts");
    std::printf("PASS cache-row build=%u: raw scalar schema, GNU owning carrier and caller-only cleanup\n", profile);
    return 0;
}
#endif

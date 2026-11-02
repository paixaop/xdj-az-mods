/* SPDX-License-Identifier: GPL-3.0-only */
/* Entry transparency is separate from native ownership/provenance proof. */
#include "azcacheentry.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#if !defined(__linux__) || !defined(__aarch64__)
int main() { std::puts("SKIP: exact source-cache entry ABI needs AArch64 Linux"); return 77; }
#else
extern "C" void *azsdk_cache_entry(unsigned) __attribute__((weak));
extern "C" void *azsdk_cache_entry_originals[AZCE_KIND_COUNT] __attribute__((weak));
extern "C" void azsdk_cache_invoke_original(azce_call *) __attribute__((weak));
static unsigned entered, completed, active, originals, expected_kind;
static std::uintptr_t expected_original;
static bool throw_original;
static std::uint64_t expected[8];
struct DispatchScope { DispatchScope() { ++active; } ~DispatchScope() { --active; } };
static void clobber() {
    asm volatile("mov x0, #99\nmov x1, #98\nmov x2, #97\nmov x3, #96\n"
                 "mov x4, #95\nmov x5, #94\nmov x6, #93\nmov x7, #92\nmov x8, #91\n"
                 ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "memory");
}
extern "C" void azsdk_cache_dispatch_call(azce_call *call) {
    ++entered; DispatchScope scope;
    assert(call && call->kind == expected_kind && !call->reserved && call->original == expected_original);
    for (unsigned i = 0; i < 8; ++i) assert(call->args[i] == expected[i]);
    clobber();
    assert(azsdk_cache_invoke_original);
    azsdk_cache_invoke_original(call);
    ++completed;
    clobber();
}
struct Pair { std::uint64_t a, b; };
using Scalar = Pair (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                       std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
static Pair scalar(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                   std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    ++originals;
    std::uint64_t args[] = {a,b,c,d,e,f,g,h};
    for (unsigned i = 0; i < 8; ++i) assert(args[i] == expected[i]);
    if (throw_original) throw std::runtime_error("native exact exception");
    return {UINT64_C(0xfedcba9876543210), UINT64_C(0x123456789abcdef0)};
}
template<unsigned Kind>
static Pair kind_scalar(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                        std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    assert(expected_kind == Kind && "entry used another target's original");
    return scalar(a,b,c,d,e,f,g,h);
}
static const Scalar kind_originals[] = {
    kind_scalar<0>, kind_scalar<1>, kind_scalar<2>, kind_scalar<3>, kind_scalar<4>,
    kind_scalar<5>, kind_scalar<6>, kind_scalar<7>, kind_scalar<8>, kind_scalar<9>,
    kind_scalar<10>, kind_scalar<11>, kind_scalar<12>, kind_scalar<13>, kind_scalar<14>,
    kind_scalar<15>, kind_scalar<16>, kind_scalar<17>, kind_scalar<18>, kind_scalar<19>, kind_scalar<20>,
    kind_scalar<21>, kind_scalar<22>
};
using Owner = std::shared_ptr<std::uint64_t>;
using Create = Owner (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" {
std::uintptr_t cache_entry_owner_destination = 0;
std::uintptr_t cache_entry_rows_destination = 0;
}
static unsigned rows_alive, rows_destroyed, rows_moves;
struct Rows {
    std::uint64_t a, b, c;
    explicit Rows(std::uint64_t value) : a(value), b(value ^ 123u), c(~value) { ++rows_alive; }
    Rows(const Rows &) = delete;
    Rows(Rows &&other) : a(other.a), b(other.b), c(other.c) { ++rows_moves; ++rows_alive; }
    ~Rows() { --rows_alive; ++rows_destroyed; }
};
static_assert(sizeof(Rows) == 24, "native row-carrier return extent");
using Query = Rows (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" Owner cache_entry_create_impl(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                         std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    assert(cache_entry_owner_destination >= 0x10000 && !(cache_entry_owner_destination & 7u) && "native owner result destination is invalid");
    (void)scalar(a,b,c,d,e,f,g,h);
    return std::make_shared<std::uint64_t>(a + h);
}
extern "C" Owner cache_entry_create(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" Rows cache_entry_query_impl(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                       std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    assert(cache_entry_rows_destination >= 0x10000 && !(cache_entry_rows_destination & 7u) && "native row result destination is invalid");
    (void)scalar(a,b,c,d,e,f,g,h);
    return Rows(a + h);
}
extern "C" Rows cache_entry_query(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
asm(".text\n.global cache_entry_create\n.type cache_entry_create,%function\n"
    "cache_entry_create:\n.cfi_startproc\n"
    "adrp x9,cache_entry_owner_destination\nadd x9,x9,:lo12:cache_entry_owner_destination\n"
    "str x8,[x9]\nb cache_entry_create_impl\n.cfi_endproc\n"
    ".size cache_entry_create,.-cache_entry_create\n"
    ".global cache_entry_query\n.type cache_entry_query,%function\n"
    "cache_entry_query:\n.cfi_startproc\n"
    "adrp x9,cache_entry_rows_destination\nadd x9,x9,:lo12:cache_entry_rows_destination\n"
    "str x8,[x9]\nb cache_entry_query_impl\n.cfi_endproc\n"
    ".size cache_entry_query,.-cache_entry_query\n");
template<class Fn> static Fn install(unsigned kind, Fn original) {
    expected_kind = kind;
    expected_original = reinterpret_cast<std::uintptr_t>(original);
    if (!azsdk_cache_entry) return original; /* old production: no observer entry */
    assert(azsdk_cache_entry_originals);
    auto entry = azsdk_cache_entry(kind);
    assert(entry && "missing distinct native entry/original route");
    azsdk_cache_entry_originals[kind] = reinterpret_cast<void *>(original);
    return reinterpret_cast<Fn>(entry);
}
static void clear_counts() { entered = completed = originals = active = 0; }
int main() {
    for (unsigned i = 0; i < 8; ++i) expected[i] = UINT64_C(0xaabbccdd00000000) + i;
    auto fn = install(AZCE_KIND_SELECTION, scalar); clear_counts();
    auto result = fn(expected[0],expected[1],expected[2],expected[3],expected[4],expected[5],expected[6],expected[7]);
    assert(originals == 1 && result.a == UINT64_C(0xfedcba9876543210) && result.b == UINT64_C(0x123456789abcdef0));
    std::puts("NATIVE_ENTRY_FORWARD_OK: all8 arguments and both return registers"); std::fflush(stdout);
    assert(entered == 1 && completed == 1 && !active && "missing actual native entry observer dispatch");
    assert(AZCE_KIND_COUNT == sizeof kind_originals / sizeof *kind_originals);
    for (unsigned kind = 0; kind < AZCE_KIND_COUNT; ++kind) {
        for (unsigned prior = 0; prior < kind; ++prior) assert(kind_originals[kind] != kind_originals[prior]);
        fn = install(kind, kind_originals[kind]); clear_counts();
        result = fn(expected[0],expected[1],expected[2],expected[3],expected[4],expected[5],expected[6],expected[7]);
        assert(originals == 1 && entered == 1 && completed == 1 && !active);
        assert(result.a == UINT64_C(0xfedcba9876543210) && result.b == UINT64_C(0x123456789abcdef0));
        throw_original = true; clear_counts(); bool caught = false;
        try { fn(expected[0],expected[1],expected[2],expected[3],expected[4],expected[5],expected[6],expected[7]); }
        catch (const std::runtime_error &e) { caught = !std::strcmp(e.what(), "native exact exception"); }
        throw_original = false;
        assert(caught && originals == 1 && entered == 1 && !completed && !active);
    }
    auto create = install(AZCE_KIND_CREATE, cache_entry_create); clear_counts();
    Owner owner = create(expected[0],expected[1],expected[2],expected[3],expected[4],expected[5],expected[6],expected[7]);
    assert(cache_entry_owner_destination == reinterpret_cast<std::uintptr_t>(&owner));
    assert(owner && *owner == expected[0] + expected[7] && owner.use_count() == 1);
    assert(originals == 1 && entered == 1 && completed == 1 && !active);
    auto query = install(AZCE_KIND_QUERY, cache_entry_query); clear_counts();
    {
        Rows rows = query(expected[0],expected[1],expected[2],expected[3],expected[4],expected[5],expected[6],expected[7]);
        assert(cache_entry_rows_destination == reinterpret_cast<std::uintptr_t>(&rows));
        assert(rows.a == expected[0] + expected[7] && rows.b == (rows.a ^ 123u) && rows.c == ~rows.a);
        assert(rows_alive == 1 && !rows_moves && !rows_destroyed);
        assert(originals == 1 && entered == 1 && completed == 1 && !active);
    }
    assert(!rows_alive && rows_destroyed == 1 && !rows_moves);
    assert(!azsdk_cache_entry(AZCE_KIND_COUNT) && !azsdk_cache_entry(UINT32_MAX));
    std::puts("PASS cache entry: integer registers, hidden16/24-byte results, caller-only cleanup, exact-once forwarding and exception CFI");
}
#endif

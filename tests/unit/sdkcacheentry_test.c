/* SPDX-License-Identifier: GPL-3.0-only */
/* Entry transparency is separate from native ownership/provenance proof.
 * Independent C native owners and explicit native hidden-result ABI thunks. */
#include "azcacheentry.h"
#include <assert.h>
#include <stdio.h>
#include "native_abi_fixture.h"
#if !defined(__linux__) || !defined(__aarch64__)
int main(void) {
    puts("SKIP: exact source-cache entry ABI needs AArch64 Linux");
    return 77;
}
#else
void *azsdk_cache_entry(unsigned) __attribute__((weak));
extern void *azsdk_cache_entry_originals[AZCE_KIND_COUNT] __attribute__((weak));
void azsdk_cache_invoke_original(struct azce_call *) __attribute__((weak));
static unsigned entered, completed, active, originals, expected_kind;
static uintptr_t expected_original;
static bool throw_original;
static uint64_t expected[8];
static void scope_end(unsigned *p) {
    (void) p;
    --active;
}
static void clobber(void) {
    __asm__ volatile("mov x0,#99\nmov x1,#98\nmov x2,#97\nmov x3,#96\n"
                     "mov x4,#95\nmov x5,#94\nmov x6,#93\nmov x7,#92\nmov x8,#91\n" ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "memory");
}
void azsdk_cache_dispatch_call(struct azce_call *call) {
    ++entered;
    unsigned scope __attribute__((cleanup(scope_end))) = ++active;
    assert(call && call->kind == expected_kind && !call->reserved && call->original == expected_original);
    for (unsigned i = 0; i < 8; ++i) assert(call->args[i] == expected[i]);
    clobber();
    assert(azsdk_cache_invoke_original);
    azsdk_cache_invoke_original(call);
    ++completed;
    clobber();
}
struct Pair {
    uint64_t a, b;
};
#define ARGS uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h
#define VALUES expected[0], expected[1], expected[2], expected[3], expected[4], expected[5], expected[6], expected[7]
typedef struct Pair (*Scalar)(ARGS);
static struct Pair scalar(ARGS) {
    ++originals;
    uint64_t args[] = {a, b, c, d, e, f, g, h};
    for (unsigned i = 0; i < 8; ++i) assert(args[i] == expected[i]);
    if (throw_original) fixture_raise();
    return (struct Pair) {UINT64_C(0xfedcba9876543210), UINT64_C(0x123456789abcdef0)};
}
#define KIND(n)                                                               \
    static struct Pair kind_scalar_##n(ARGS) {                                \
        assert(expected_kind == n && "entry used another target's original"); \
        return scalar(a, b, c, d, e, f, g, h);                                \
    }
KIND(0)
KIND(1)
KIND(2)
KIND(3)
KIND(4)
KIND(5)
KIND(6)
KIND(7)
KIND(8)
KIND(9)
KIND(10)
KIND(11)
KIND(12)
KIND(13)
KIND(14)
KIND(15)
KIND(16)
KIND(17)
KIND(18)
KIND(19)
KIND(20)
KIND(21)
KIND(22)
#undef KIND
static const Scalar kind_originals[] = {kind_scalar_0, kind_scalar_1, kind_scalar_2, kind_scalar_3, kind_scalar_4, kind_scalar_5, kind_scalar_6, kind_scalar_7, kind_scalar_8, kind_scalar_9, kind_scalar_10, kind_scalar_11, kind_scalar_12, kind_scalar_13, kind_scalar_14, kind_scalar_15, kind_scalar_16, kind_scalar_17, kind_scalar_18, kind_scalar_19, kind_scalar_20, kind_scalar_21, kind_scalar_22};
uintptr_t cache_entry_owner_destination, cache_entry_rows_destination;
static unsigned rows_alive, rows_destroyed, owner_destroyed;
struct Rows {
    uint64_t a, b, c;
};
_Static_assert(sizeof(struct Rows) == 24 && sizeof(struct fixture_owner) == 16, "native owned result extents");
static void owner_dispose(void *p) {
    (void) p;
    ++owner_destroyed;
}
FIXTURE_VTABLE(owner_vtable, owner_dispose);
void cache_entry_create_impl(ARGS, struct fixture_owner *out) {
    assert(cache_entry_owner_destination == (uintptr_t) out && (uintptr_t) out >= 0x10000 && !((uintptr_t) out & 7u));
    (void) scalar(a, b, c, d, e, f, g, h);
    *out = fixture_make(sizeof(uint64_t), owner_vtable);
    *(uint64_t *) out->object = a + h;
}
void cache_entry_query_impl(ARGS, struct Rows *out) {
    assert(cache_entry_rows_destination == (uintptr_t) out && (uintptr_t) out >= 0x10000 && !((uintptr_t) out & 7u));
    (void) scalar(a, b, c, d, e, f, g, h);
    *out = (struct Rows) {a + h, (a + h) ^ 123u, ~(a + h)};
    ++rows_alive;
}
void cache_entry_create(void);
void cache_entry_query(void);
void cache_call_indirect(void *, void *, const uint64_t *);
static void rows_drop(struct Rows *p) {
    (void) p;
    --rows_alive;
    ++rows_destroyed;
}
static void *install(unsigned kind, void *original) {
    expected_kind = kind;
    expected_original = (uintptr_t) original;
    if (!azsdk_cache_entry) return original;
    assert(azsdk_cache_entry_originals);
    void *entry = azsdk_cache_entry(kind);
    assert(entry && "missing distinct native entry/original route");
    azsdk_cache_entry_originals[kind] = original;
    return entry;
}
static void clear_counts(void) { entered = completed = originals = active = 0; }
static void scalar_unwind(void *value) {
    Scalar fn = value;
    (void) fn(VALUES);
}
int main(void) {
    for (unsigned i = 0; i < 8; ++i) expected[i] = UINT64_C(0xaabbccdd00000000) + i;
    Scalar fn = install(AZCE_KIND_SELECTION, scalar);
    clear_counts();
    struct Pair result = fn(VALUES);
    assert(originals == 1 && result.a == UINT64_C(0xfedcba9876543210) && result.b == UINT64_C(0x123456789abcdef0));
    puts("NATIVE_ENTRY_FORWARD_OK: all8 arguments and both return registers");
    fflush(stdout);
    assert(entered == 1 && completed == 1 && !active && "missing actual native entry observer dispatch");
    assert(AZCE_KIND_COUNT == sizeof kind_originals / sizeof *kind_originals);
    for (unsigned kind = 0; kind < AZCE_KIND_COUNT; ++kind) {
        for (unsigned prior = 0; prior < kind; ++prior) assert(kind_originals[kind] != kind_originals[prior]);
        fn = install(kind, kind_originals[kind]);
        clear_counts();
        result = fn(VALUES);
        assert(originals == 1 && entered == 1 && completed == 1 && !active);
        assert(result.a == UINT64_C(0xfedcba9876543210) && result.b == UINT64_C(0x123456789abcdef0));
        throw_original = true;
        clear_counts();
        unsigned deletions = fixture_exception_deletions;
        bool caught = !fixture_native_try(scalar_unwind, fn);
        throw_original = false;
        assert(caught && fixture_exception_deletions == deletions + 1 && originals == 1 && entered == 1 && !completed && !active);
    }
    void *create = install(AZCE_KIND_CREATE, cache_entry_create);
    clear_counts();
    struct fixture_owner owner = {0};
    cache_call_indirect(create, &owner, expected);
    assert(cache_entry_owner_destination == (uintptr_t) &owner);
    assert(owner.object && *(uint64_t *) owner.object == expected[0] + expected[7] && owner.control->strong == 1 && !owner_destroyed);
    assert(originals == 1 && entered == 1 && completed == 1 && !active);
    void *query = install(AZCE_KIND_QUERY, cache_entry_query);
    clear_counts();
    {
        struct Rows rows __attribute__((cleanup(rows_drop))) = {0};
        cache_call_indirect(query, &rows, expected);
        assert(cache_entry_rows_destination == (uintptr_t) &rows);
        assert(rows.a == expected[0] + expected[7] && rows.b == (rows.a ^ 123u) && rows.c == ~rows.a);
        assert(rows_alive == 1 && !rows_destroyed);
        assert(originals == 1 && entered == 1 && completed == 1 && !active);
    }
    fixture_drop(&owner);
    assert(!rows_alive && rows_destroyed == 1 && owner_destroyed == 1);
    assert(!azsdk_cache_entry(AZCE_KIND_COUNT) && !azsdk_cache_entry(UINT32_MAX));
    puts("PASS cache entry: integer registers, hidden16/24-byte results, caller-only cleanup, exact-once forwarding and native unwind CFI");
}
#endif

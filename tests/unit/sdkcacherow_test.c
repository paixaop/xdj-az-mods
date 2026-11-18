/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C native layout and owning result. The parser borrows bytes;
 * only this caller allocates and frees its rows and string buffers. */
#include "native_abi_fixture.h"
#include <stdio.h>
#include <stddef.h>
#define REQUIRE(x, why)                                                                       \
    do {                                                                                      \
        if (!(x)) {                                                                           \
            fprintf(stderr, "FAIL cache-row: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                          \
        }                                                                                     \
    } while (0)
#ifndef AZCACHE_ROW_HEADER
#define AZCACHE_ROW_HEADER "../../src/common/azcacherow.h"
#endif
#if __has_include(AZCACHE_ROW_HEADER)
#include AZCACHE_ROW_HEADER
#else
static bool azcrow_copy_owned_tag23(const void *p, uint32_t *v) {
    (void) p;
    (void) v;
    return false;
}
#endif
static unsigned profile;
struct Counts {
    unsigned alive, created, destroyed, forwards;
};
static struct Counts counts[4];
struct Spec {
    uint8_t kind;
    uint32_t value;
    bool null;
};
struct NativeString {
    uint16_t *data;
    uint64_t size, capacity, reserved;
};
struct NativeRow {
    uint32_t value;
    uint8_t kind, padding[3];
    uint32_t auxiliary;
    uint8_t flag, padding2[3];
    struct NativeString first;
    uint32_t numeric;
    uint8_t flag2, padding3[3];
    struct NativeString second;
    uint64_t auxiliary2;
    struct NativeString third;
    uint32_t fixture_context;
    uint8_t tail[20];
};
struct NativeRows {
    struct NativeRow **begin, **end, **capacity;
};
struct Context {
    unsigned id;
    struct fixture_owner owner;
    struct Spec specs[17];
    size_t size;
};
FIXTURE_VTABLE(caller_vtable, fixture_noop);
static void context_init(struct Context *c, unsigned id) {
    *c = (struct Context) {.id = id, .owner = fixture_make(sizeof(unsigned), caller_vtable)};
    *(unsigned *) c->owner.object = id;
}
static void context_drop(struct Context *c) { fixture_drop(&c->owner); }
#define CONTEXT(name, id)                                       \
    struct Context name __attribute__((cleanup(context_drop))); \
    context_init(&name, id)
static void string_init(struct NativeString *s, size_t n, uint16_t value) {
    s->data = malloc((n + 1) * 2);
    REQUIRE(s->data, "native string allocation");
    s->size = s->capacity = n;
    s->reserved = 0;
    for (size_t i = 0; i < n; i++) s->data[i] = value;
    s->data[n] = 0;
}
static struct NativeRow *row_new(unsigned context, struct Spec s) {
    REQUIRE(context < 4, "four independent caller contexts");
    struct NativeRow *r = calloc(1, sizeof *r);
    REQUIRE(r, "native row allocation");
    *r = (struct NativeRow) {.value = s.value, .kind = s.kind, .padding = {0xa5, 0x5a, 0xff}, .auxiliary = 0x11223344, .flag = 1, .padding2 = {2, 3, 4}, .numeric = 0x55667788, .flag2 = 5, .padding3 = {6, 7, 8}, .auxiliary2 = 0x12345678, .fixture_context = context};
    string_init(&r->first, 65, 'A');
    string_init(&r->second, 67, 'B');
    string_init(&r->third, 69, 'C');
    ++counts[context].alive;
    ++counts[context].created;
    return r;
}
static void row_drop(struct NativeRow *r) {
    if (!r) return;
    REQUIRE(counts[r->fixture_context].alive, "native caller destroys each owned row once");
    --counts[r->fixture_context].alive;
    ++counts[r->fixture_context].destroyed;
    free(r->third.data);
    free(r->second.data);
    free(r->first.data);
    free(r);
}
static void rows_drop(struct NativeRows *r) {
    for (struct NativeRow **p = r->begin; p != r->end; ++p) row_drop(*p);
    free(r->begin);
    *r = (struct NativeRows) {0};
}
_Static_assert(sizeof(struct NativeString) == 32 && sizeof(struct NativeRow) == 0x98, "native row width");
_Static_assert(offsetof(struct NativeRow, kind) == 4 && offsetof(struct NativeRow, first) == 0x10 && offsetof(struct NativeRow, second) == 0x38 && offsetof(struct NativeRow, third) == 0x60, "native row field offsets");
_Static_assert(sizeof(struct NativeRows) == 24 && sizeof(struct NativeRow *) == 8, "owning24-byte result with8-byte slots");
static struct NativeRows native_query(struct Context *c, unsigned start, unsigned count) {
    REQUIRE(c->owner.object, "original has its real complete caller owner");
    ++counts[c->id].forwards;
    struct NativeRows r = {0};
    if (start < c->size) {
        size_t available = c->size - start, n = count < available ? count : available;
        if (n) {
            r.begin = calloc(n, sizeof *r.begin);
            REQUIRE(r.begin, "native slot allocation");
            r.end = r.capacity = r.begin + n;
            for (size_t i = 0; i < n; i++) {
                struct Spec s = c->specs[start + i];
                r.begin[i] = s.null ? NULL : row_new(c->id, s);
            }
        }
    }
    return r;
}
struct GuardedResult {
    uint64_t before;
    struct NativeRows rows;
    uint64_t after;
    bool constructed;
};
static void result_check(const struct GuardedResult *r) { REQUIRE(r->before == UINT64_C(0x123456789abcdef0) && r->after == UINT64_C(0xfedcba9876543210), "original result output guards"); }
static void result_drop(struct GuardedResult *r) {
    if (r->constructed) {
        rows_drop(&r->rows);
        r->constructed = false;
    }
    result_check(r);
}
struct Snapshot {
    unsigned char carrier[24];
    struct NativeRow *pointers[17];
    unsigned char rows[17][0x98];
    size_t count;
};
static void snapshot_init(struct Snapshot *s, const struct NativeRows *r) {
    memcpy(s->carrier, r, sizeof s->carrier);
    s->count = r->begin ? (size_t) (r->end - r->begin) : 0;
    REQUIRE(s->count <= 17, "fixture snapshot capacity");
    for (size_t i = 0; i < s->count; i++) {
        s->pointers[i] = r->begin[i];
        if (r->begin[i]) memcpy(s->rows[i], r->begin[i], sizeof s->rows[i]);
    }
}
static void snapshot_check(const struct Snapshot *s, const struct NativeRows *r) {
    size_t n = r->begin ? (size_t) (r->end - r->begin) : 0;
    REQUIRE(!memcmp(s->carrier, r, 24) && n == s->count, "copier leaves the actual owning carrier unchanged");
    for (size_t i = 0; i < n; i++) REQUIRE(r->begin[i] == s->pointers[i] && (!r->begin[i] || !memcmp(s->rows[i], r->begin[i], 0x98)), "copier does not mutate, release or replace any native row/string");
}
static bool forwarded_copy(struct Context *c, unsigned start, unsigned count, uint32_t *out, bool first) {
    unsigned forwards = counts[c->id].forwards, alive = counts[c->id].alive, created = counts[c->id].created, destroyed = counts[c->id].destroyed;
    int owners = c->owner.control->strong;
    struct fixture_owner weak __attribute__((cleanup(fixture_weak_drop))) = fixture_weak(c->owner);
    struct GuardedResult r __attribute__((cleanup(result_drop))) = {.before = UINT64_C(0x123456789abcdef0), .after = UINT64_C(0xfedcba9876543210)};
    r.rows = native_query(c, start, count);
    r.constructed = true;
    result_check(&r);
    REQUIRE(counts[c->id].forwards == forwards + 1 && c->owner.control->strong == owners && !fixture_expired(weak), "original forwards once without replacing or retaining its genuine caller owner");
    unsigned allocated = counts[c->id].created - created;
    REQUIRE(counts[c->id].alive == alive + allocated && counts[c->id].destroyed == destroyed, "native output caller owns every newly returned row before copy");
    struct Snapshot snapshot = {0};
    snapshot_init(&snapshot, &r.rows);
    bool copied = azcrow_copy_owned_tag23(&r.rows, out);
    snapshot_check(&snapshot, &r.rows);
    result_check(&r);
    REQUIRE(counts[c->id].alive == alive + allocated && counts[c->id].destroyed == destroyed && c->owner.control->strong == owners, "scalar copy cannot consume native rows or retain caller ownership");
    result_drop(&r);
    REQUIRE(counts[c->id].alive == alive && counts[c->id].destroyed == destroyed + allocated && c->owner.control->strong == owners && !fixture_expired(weak), "only native result caller cleans each row and its three strings");
    if (first) {
        printf("NATIVE_ROW_FORWARD_OK build=%u context=%u carrier24=%zu slot8=%zu row98=%zu forwarded=1 cleaned=%u\n", profile, c->id, sizeof(struct NativeRows), sizeof(struct NativeRow *), sizeof(struct NativeRow), allocated);
        fflush(stdout);
    }
    return copied;
}
static void refused(struct Context *c, unsigned start, unsigned count, const char *reason) {
    uint32_t value = UINT32_C(0x5a6b7c8d);
    REQUIRE(!forwarded_copy(c, start, count, &value, false) && value == UINT32_C(0x5a6b7c8d), reason);
}
#define SPECS(c, ...)                                  \
    do {                                               \
        struct Spec values_[] = {__VA_ARGS__};         \
        (c).size = sizeof values_ / sizeof values_[0]; \
        memcpy((c).specs, values_, sizeof values_);    \
    } while (0)
static void valid_contexts(void) {
    const uint32_t payloads[4] = {UINT32_C(0x13579bdf), 0, UINT32_MAX, UINT32_C(0x2468ace0)};
    for (unsigned id = 0; id < 4; id++) {
        struct fixture_owner weak __attribute__((cleanup(fixture_weak_drop))) = {0};
        uint32_t copied = UINT32_C(0x5a6b7c8d);
        {
            CONTEXT(c, id);
            weak = fixture_weak(c.owner);
            SPECS(c, {4, UINT32_C(0xdeadbeef), false}, {0x23, payloads[id], false}, {11, 77, false});
            bool found = forwarded_copy(&c, 0, 3, &copied, id == 0);
            REQUIRE(found && copied == payloads[id], "missing owned tag0x23 scalar copy");
            REQUIRE(!counts[id].alive, "raw scalar survives native result caller destruction");
        }
        REQUIRE(fixture_expired(weak) && copied == payloads[id], "copied scalar outlives complete caller without acquiring liveness");
    }
}
static void partial_null_duplicate(void) {
    CONTEXT(c, 0);
    SPECS(c, {4, 999, false}, {0x23, 123, false}, {13, 456, false});
    refused(&c, 0, 1, "partial window lacking the tag declines unchanged output");
    refused(&c, 2, 1, "late partial window lacking the tag declines unchanged output");
    refused(&c, 3, 0, "empty native window declines unchanged output");
    uint32_t selected = 0;
    REQUIRE(forwarded_copy(&c, 1, 1, &selected, false) && selected == 123, "partial window containing one tagged row copies its scalar");
    SPECS(c, {4, 1, false}, {0x23, 123, false}, {0, 0, true});
    refused(&c, 0, 3, "null row after a candidate declines without provisional scalar output");
    SPECS(c, {0, 0, true}, {0x23, 123, false});
    refused(&c, 0, 2, "null row before a candidate declines unchanged output");
    SPECS(c, {0x23, 123, false}, {0x23, 123, false});
    refused(&c, 0, 2, "duplicate tagged rows decline even when content scalars agree");
    c.specs[1].value = 456;
    refused(&c, 0, 2, "contradictory duplicate tagged rows decline unchanged output");
    c.size = 16;
    for (unsigned i = 0; i < 16; i++) c.specs[i] = (struct Spec) {4, 333, false};
    c.specs[15] = (struct Spec) {0x23, UINT32_MAX, false};
    selected = 0;
    REQUIRE(forwarded_copy(&c, 0, 16, &selected, false) && selected == UINT32_MAX, "observation quota16 for the proved families can locate its last tagged row");
    c.specs[c.size++] = (struct Spec) {4, 444, false};
    refused(&c, 0, 17, "owned span beyond observation quota16 declines before traversal");
}
struct Span {
    uintptr_t begin, end, capacity;
};
static void decline(const void *carrier, const char *why, const struct Context *c, struct NativeRow **slots, unsigned alive, unsigned destroyed) {
    uint32_t value = UINT32_C(0x5a6b7c8d);
    REQUIRE(!azcrow_copy_owned_tag23(carrier, &value) && value == UINT32_C(0x5a6b7c8d), why);
    REQUIRE(counts[c->id].alive == alive && counts[c->id].destroyed == destroyed && slots[0] && slots[1], "bounded malformed views do not transfer or destroy their still-owned pointees");
}
static void malformed_spans(void) {
    CONTEXT(c, 1);
    struct NativeRow *slots[4] = {row_new(c.id, (struct Spec) {0x23, 123, false}), row_new(c.id, (struct Spec) {4, 456, false}), NULL, NULL};
    uintptr_t base = (uintptr_t) slots, end = base + sizeof slots;
    unsigned alive = counts[c.id].alive, destroyed = counts[c.id].destroyed;
    decline(NULL, "null carrier declines unchanged output", &c, slots, alive, destroyed);
    const struct Span bad[] = {{0, 0, 0}, {base, base, end}, {base + 8, base, end}, {base, base + 16, base + 8}, {base, base + 1, end}, {base, base + 8, end - 1}, {0, base + 8, end}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) decline(&bad[i], "empty or malformed span within actual owned slot storage declines unchanged output", &c, slots, alive, destroyed);
    for (unsigned i = 0; i < 4; i++) row_drop(slots[i]);
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate fixture profile process");
    profile = (unsigned) (argv[1][0] - '0');
    valid_contexts();
    partial_null_duplicate();
    malformed_spans();
    for (unsigned i = 0; i < 4; i++) REQUIRE(!counts[i].alive && counts[i].created == counts[i].destroyed, "caller-only cleanup balances all four contexts");
    printf("PASS cache-row build=%u: raw scalar schema, GNU owning carrier and caller-only cleanup\n", profile);
    return 0;
}

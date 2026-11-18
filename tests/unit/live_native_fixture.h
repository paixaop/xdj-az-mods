/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LIVE_NATIVE_FIXTURE_H
#define LIVE_NATIVE_FIXTURE_H
#include "native_abi_fixture.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stddef.h>
#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "FAIL live: %s:%d: %s\n", __FILE__, __LINE__, #c); \
            abort();                                                           \
        }                                                                      \
    } while (0)
typedef struct fixture_owner Lease;
static _Atomic(int) allocation_failure = -1;
void *az_listener_test_allocate(size_t bytes) {
    int count = atomic_load(&allocation_failure);
    while (count >= 0) {
        if (!count) return NULL;
        if (atomic_compare_exchange_weak(&allocation_failure, &count, count - 1)) break;
    }
    return calloc(1, bytes ? bytes : 1);
}
static void allocator_contract(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        size_t size = mode ? 0 : 17;
        atomic_store(&allocation_failure, 0);
        void *failed = az_listener_test_allocate(size);
        CHECK(!failed && atomic_load(&allocation_failure) == 0);
        atomic_store(&allocation_failure, 1);
        void *first = az_listener_test_allocate(size);
        CHECK(first && atomic_load(&allocation_failure) == 0);
        memset(first, 0xa5, size);
        void *next = az_listener_test_allocate(size);
        CHECK(!next && atomic_load(&allocation_failure) == 0);
        free(first);
        atomic_store(&allocation_failure, -1);
        void *zero = az_listener_test_allocate(0);
        CHECK(zero);
        free(zero);
    }
}
typedef struct {
    unsigned char bytes[3];
} Play;
struct Server {
    const void *const *vtable;
    Lease listeners[5][64];
    unsigned count[5];
    int fail_stream;
    bool fail_after;
};
struct Publisher {
    const void *const *vtable;
    unsigned char padding[0x1f20];
    struct Server *server;
};
typedef struct {
    _Alignas(uintptr_t) unsigned char bytes[0x1a8];
} Unit;
static Unit unit_create(struct Publisher *publisher) {
    Unit unit = {0};
    memcpy(unit.bytes + 0x1a0, &publisher, sizeof publisher);
    return unit;
}
typedef struct {
    struct Server server;
    struct Publisher publisher;
    Unit unit;
} Fixture;
static void subscribe(struct Server *server, unsigned stream, const Lease *value) {
    if (server->fail_stream == (int) stream && !server->fail_after) fixture_raise();
    CHECK(server->count[stream] < 64);
    server->listeners[stream][server->count[stream]++] = fixture_weak(*value);
    if (server->fail_stream == (int) stream && server->fail_after) fixture_raise();
}
static void native_play(struct Server *server, const Lease *value) { subscribe(server, 0, value); }
static void native_quantize(struct Server *server, const Lease *value) { subscribe(server, 1, value); }
static void native_key_lock(struct Server *server, const Lease *value) { subscribe(server, 2, value); }
static void native_sync(struct Server *server, const Lease *value) { subscribe(server, 3, value); }
static void native_tempo_lock(struct Server *server, const Lease *value) { subscribe(server, 4, value); }
static void wrong_sync(struct Server *server, const Lease *value) {
    (void) server;
    (void) value;
    fputs("FAIL live: native Sync registration slot 3\n", stderr);
    fflush(stderr);
    abort();
}
static void wrong_play(struct Server *server, const Lease *value) {
    (void) server;
    (void) value;
    fputs("FAIL live: native registration slot 14\n", stderr);
    fflush(stderr);
    abort();
}
static const char short_type_name[] = "A";
static const struct {
    uintptr_t vptr;
    const char *name;
} server_type = {0, "N9dj_player23StateNotificationServerE"},
  publisher_type = {0, "N9dj_player15UnitStateServerE"}, wrong_type = {0, short_type_name};
static const struct {
    uintptr_t top;
    const void *type;
    const void *slots[26];
} server_table = {
    0, &server_type, {[2] = (void *) native_sync, [3] = (void *) wrong_sync, [5] = (void *) native_tempo_lock, [14] = (void *) wrong_play, [15] = (void *) native_play, [19] = (void *) native_quantize, [21] = (void *) native_key_lock}},
  wrong_table = {0, &wrong_type, {0}};
static const struct {
    uintptr_t top;
    const void *type;
    const void *slots[2];
} publisher_table = {0, &publisher_type, {0}};
static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->server.vtable = server_table.slots;
    f->server.fail_stream = -1;
    f->publisher.vtable = publisher_table.slots;
    f->publisher.server = &f->server;
    f->unit = unit_create(&f->publisher);
    CHECK(offsetof(struct Publisher, server) == 0x1f28);
}
static void fixture_finish(Fixture *f) {
    for (unsigned s = 0; s < 5; ++s)
        for (unsigned i = 0; i < f->server.count[s]; ++i) fixture_weak_drop(&f->server.listeners[s][i]);
}
static void fixtures_drop(Fixture (*f)[4]) {
    for (unsigned i = 0; i < 4; ++i) fixture_finish(&(*f)[i]);
}
static void leases_drop(Lease (*leases)[4]) {
    for (unsigned i = 0; i < 4; ++i) fixture_drop(&(*leases)[i]);
}
static Lease get_lease(Fixture *f, unsigned stream) {
    CHECK(f->server.count[stream]);
    Lease lease = fixture_lock(f->server.listeners[stream][f->server.count[stream] - 1]);
    CHECK(lease.object);
    return lease;
}
static uintptr_t vtable(const void *p) {
    uintptr_t t;
    memcpy(&t, p, sizeof t);
    return t;
}
static void dispatch(Lease p, const void *bytes, unsigned deck, bool full) {
    const void *const *t = (const void *const *) vtable(p.object);
    ((void (*)(void *, const void *)) t[2])(p.object, bytes);
    if (full) ((void (*)(void *, const void *, unsigned)) t[3])(p.object, bytes, deck);
}
static void emit_play(Lease p, unsigned a, unsigned b, unsigned c, unsigned d) {
    /* Byte four remains a heap redzone for the payload-overread control. */
    Play *n = malloc(sizeof *n);
    CHECK(n);
    *n = (Play) {{(unsigned char) a, (unsigned char) b, (unsigned char) c}};
    dispatch(p, n, d, true);
    free(n);
}
static void emit_scalar(Lease p, uint32_t value, unsigned d) {
    uint32_t *n = malloc(sizeof *n);
    CHECK(n);
    *n = value;
    dispatch(p, n, d, true);
    free(n);
}
static void emit_fixture_play(Fixture *f, unsigned stream, unsigned a, unsigned b, unsigned c, unsigned d) {
    Lease p __attribute__((cleanup(fixture_drop))) = get_lease(f, stream);
    emit_play(p, a, b, c, d);
}
static void emit_fixture_scalar(Fixture *f, unsigned stream, uint32_t value, unsigned d) {
    Lease p __attribute__((cleanup(fixture_drop))) = get_lease(f, stream);
    emit_scalar(p, value, d);
}
static void dispatch_fixture(Fixture *f, unsigned stream, const void *bytes, unsigned d, bool full) {
    Lease p __attribute__((cleanup(fixture_drop))) = get_lease(f, stream);
    dispatch(p, bytes, d, full);
}
#endif

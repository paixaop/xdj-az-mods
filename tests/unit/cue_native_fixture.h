/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CUE_NATIVE_FIXTURE_H
#define CUE_NATIVE_FIXTURE_H
#include "native_abi_fixture.h"
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#define CHECK(x, why)                               \
    do {                                            \
        if (!(x)) {                                 \
            fprintf(stderr, "FAIL cue: %s\n", why); \
            abort();                                \
        }                                           \
    } while (0)
typedef struct {
    _Alignas(8) unsigned char bytes[0x100];
} CueRecord;
typedef struct fixture_owner Lease;
static CueRecord record(unsigned index, uint32_t revision) {
    CueRecord value = {0};
    memcpy(value.bytes, &index, 4);
    uint32_t padding = 0x98765432;
    memcpy(value.bytes + 4, &padding, 4);
    memcpy(value.bytes + 0x10, &revision, 4);
    return value;
}
static void emit(Lease lease, CueRecord value, unsigned player, bool routed) {
    const void *const *table;
    memcpy(&table, lease.object, sizeof table);
    ((void (*)(void *, const CueRecord *)) table[2])(lease.object, &value);
    if (routed) ((void (*)(void *, const CueRecord *, unsigned)) table[3])(lease.object, &value, player);
}
struct Server {
    const void *const *vtable;
    unsigned char native_prefix[0x1550 - 8];
    uint32_t native_routing_tag;
    Lease listeners[256];
    unsigned count;
    void (*on_register)(void *);
    void *context;
    bool fail_after;
};
struct Publisher {
    const void *const *vtable;
    unsigned char padding[0x1f28 - 8];
    struct Server *server;
};
struct Unit {
    _Alignas(8) unsigned char bytes[0x1a8];
};
typedef struct {
    struct Server server;
    struct Publisher publisher;
    struct Unit unit;
} Fixture;
static void server_add(struct Server *server, const Lease *weak) {
    CHECK(server->count < 256, "bounded fixture weak storage");
    server->listeners[server->count++] = fixture_weak(*weak);
    if (server->on_register) server->on_register(server->context);
    if (server->fail_after) fixture_raise();
}
/* Independent, explicit ELF RTTI and vtable representation. Registration is
 * slot8, with a nontrivial by-value weak parameter passed as a pointer. */
static const struct {
    uintptr_t vptr;
    const char *name;
} server_type = {0, "N9dj_player23StateNotificationServerE"},
  publisher_type = {0, "N9dj_player15UnitStateServerE"};
static const struct {
    uintptr_t top;
    const void *type;
    const void *slots[26];
} server_table = {
    0, &server_type, {[8] = (void *) server_add}};
static const struct {
    uintptr_t top;
    const void *type;
    const void *slots[2];
} publisher_table = {
    0, &publisher_type, {0}};
static void fixture_init(Fixture *f) {
    static _Atomic(uint32_t) next = 0x93510000;
    memset(f, 0, sizeof *f);
    f->server.vtable = server_table.slots;
    f->publisher.vtable = publisher_table.slots;
    f->publisher.server = &f->server;
    struct Publisher *publisher = &f->publisher;
    memcpy(f->unit.bytes + 0x1a0, &publisher, sizeof publisher);
    f->server.native_routing_tag = atomic_fetch_add(&next, 1);
    CHECK(offsetof(struct Server, native_routing_tag) == 0x1550,
          "opaque routing scalar occupies actual channel+48/server+1550 prefix");
}
static void fixture_finish(Fixture *f) {
    for (unsigned i = 0; i < f->server.count; ++i) fixture_weak_drop(&f->server.listeners[i]);
}
static void fixtures_drop(Fixture (*f)[4]) {
    for (unsigned i = 0; i < 4; ++i) fixture_finish(&(*f)[i]);
}
static void leases_drop(Lease (*leases)[4]) {
    for (unsigned i = 0; i < 4; ++i) fixture_drop(&(*leases)[i]);
}
static Lease get_lease(Fixture *f) {
    CHECK(f->server.count, "native weak registration exists");
    return fixture_lock(f->server.listeners[f->server.count - 1]);
}
struct Counter {
    const void *const *vtable;
    unsigned plain, routed;
    bool baseline;
};
static void counter_plain(void *opaque, const CueRecord *value) {
    struct Counter *counter = opaque;
    if (counter->baseline) {
        unsigned index;
        memcpy(&index, value->bytes, 4);
        CHECK(index == 8, "typed native index prefix");
    }
    ++counter->plain;
}
static void counter_routed(void *opaque, const CueRecord *value, unsigned route) {
    (void) value;
    struct Counter *counter = opaque;
    if (counter->baseline) CHECK(route == 3, "typed native routed scalar");
    ++counter->routed;
}
FIXTURE_VTABLE(counter_control, fixture_noop);
static Lease counter_create(bool baseline) {
    static const void *const vtable[] = {(void *) fixture_noop, (void *) fixture_noop, (void *) counter_plain, (void *) counter_routed};
    Lease owner = fixture_make(sizeof(struct Counter), counter_control);
    struct Counter *counter = owner.object;
    counter->vtable = vtable;
    counter->baseline = baseline;
    return owner;
}
#endif

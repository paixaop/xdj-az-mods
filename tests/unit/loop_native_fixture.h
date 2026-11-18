/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LOOP_NATIVE_FIXTURE_H
#define LOOP_NATIVE_FIXTURE_H
#include "native_abi_fixture.h"
#include <stddef.h>
#include <stdio.h>
typedef struct fixture_owner Lease;
struct Server {
    const void *const *vtable;
    Lease listeners[64];
    unsigned count;
    bool fail, fail_after;
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
static void native_add(struct Server *server, const Lease *weak) {
    if (server->fail) fixture_raise();
    assert(server->count < 64);
    server->listeners[server->count++] = fixture_weak(*weak);
    if (server->fail_after) fixture_raise();
}
struct fixture_type {
    uintptr_t vptr;
    const char *name;
};
struct fixture_table {
    uintptr_t top;
    const struct fixture_type *type;
    const void *slots[26];
};
static const char short_name[] = "A";
#define TYPE_TABLE(id, text)                                \
    static const struct fixture_type id##_type = {0, text}; \
    static const struct fixture_table id##_table = {0, &id##_type, {0}}
TYPE_TABLE(cue, "N9dj_player11CuePropertyE");
TYPE_TABLE(point, "N9dj_player13PointPropertyE");
TYPE_TABLE(position, "N12player_basis22PositionWithSourceInfoE");
TYPE_TABLE(short, short_name);
TYPE_TABLE(publisher, "N9dj_player15UnitStateServerE");
static const struct fixture_type server_type = {0, "N9dj_player23StateNotificationServerE"};
static const struct fixture_table server_table = {0, &server_type, {[11] = (void *) native_add}};
static void server_init(struct Server *server) {
    memset(server, 0, sizeof *server);
    server->vtable = server_table.slots;
}
static void server_clear(struct Server *server) {
    for (unsigned i = 0; i < server->count; ++i) fixture_weak_drop(&server->listeners[i]);
    server->count = 0;
}
static void servers_clear(struct Server (*servers)[4]) {
    for (unsigned i = 0; i < 4; ++i) server_clear(&(*servers)[i]);
}
static void publisher_init(struct Publisher *publisher) {
    memset(publisher, 0, sizeof *publisher);
    publisher->vtable = publisher_table.slots;
}
static Lease get_lease(struct Server *server) {
    assert(server->count);
    return fixture_lock(server->listeners[server->count - 1]);
}
static bool last_expired(struct Server *server) {
    assert(server->count);
    return fixture_expired(server->listeners[server->count - 1]);
}
static void leases_drop(Lease (*leases)[4]) {
    for (unsigned i = 0; i < 4; ++i) fixture_drop(&(*leases)[i]);
}
#endif

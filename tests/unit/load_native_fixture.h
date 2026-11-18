/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LOAD_NATIVE_FIXTURE_H
#define LOAD_NATIVE_FIXTURE_H
#include "azloadobserver.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL load: %s:%d: %s\n", __FILE__, __LINE__, #x); \
            abort();                                                           \
        }                                                                      \
    } while (0)
static uint64_t monotonic_ns(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t) t.tv_sec * UINT64_C(1000000000) + (uint64_t) t.tv_nsec;
}
static void until(_Atomic(bool) *flag) {
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        CHECK(monotonic_ns() < deadline);
        sched_yield();
    }
}
typedef struct {
    const void *const *vtable;
    _Atomic(uint32_t) value;
} LoadRequestID;
typedef struct {
    LoadRequestID request;
} TrackLoadOption;
typedef struct {
    const void *const *primary, *const *secondary;
    _Atomic(unsigned) loads, unloads, automatic, result, expect_inline_sample;
    const void *track_expected, *where_expected, *speed_expected;
} TrackLoadFacade;
struct Bundle {
    TrackLoadFacade *facade;
    unsigned char remaining[0x70];
};
typedef struct {
    const void *const *vtable;
    unsigned char pad[0x188];
    struct Bundle *bundle;
} DjPlayerUnit;
typedef struct {
    const void *const *primary;
    unsigned char pad[0x118];
    const void *const *stock;
    unsigned char pad40[0x28];
    const void *const *listener;
    _Atomic(unsigned) callbacks;
    _Atomic(uint32_t) last_id, last_result;
    const void *where_expected;
} LoadController;
static void *listener_interface(LoadController *c) { return &c->listener; }
static void *stock_interface(LoadController *c) { return &c->stock; }
static void *unload_interface(TrackLoadFacade *f) { return &f->secondary; }
struct native_type {
    uintptr_t vptr;
    const char *name;
};
struct native_table {
    intptr_t top;
    const struct native_type *type;
    const void *slots[4];
};
static const struct native_type id_type = {0, "N9dj_player13LoadRequestIDE"}, unit_type = {0, "N9dj_player12DjPlayerUnitE"},
                                facade_type = {0, "N9dj_player15TrackLoadFacadeE"}, controller_type = {0, "N7usecase4load14LoadControllerE"},
                                short_type = {0, "A"}, other_type = {0, "OtherRequest"};
static struct native_table id_table = {0, &id_type, {0}}, unit_table = {0, &unit_type, {0}},
                           short_table = {0, &short_type, {0}}, other_table = {0, &other_type, {0}};
static TrackLoadOption option_create(uint32_t id) { return (TrackLoadOption) {{id_table.slots, id}}; }
static void native_complete(void *self, const LoadRequestID *request, uint32_t result, const void *where) {
    LoadController *c = (LoadController *) ((char *) self - 0x150);
    CHECK(!c->where_expected || where == c->where_expected);
    atomic_store_explicit(&c->last_id, atomic_load_explicit(&request->value, memory_order_acquire), memory_order_relaxed);
    atomic_store_explicit(&c->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->callbacks, 1, memory_order_relaxed);
}
static void native_load(void *self, const void *track, const TrackLoadOption *option, const void *where, const void *speed, void *listener) {
    TrackLoadFacade *f = self;
    CHECK(track == f->track_expected && where == f->where_expected && speed == f->speed_expected);
    atomic_fetch_add_explicit(&f->loads, 1, memory_order_relaxed);
    if (atomic_load_explicit(&f->automatic, memory_order_acquire) && listener) {
        LoadRequestID copy = {id_table.slots, atomic_load_explicit(&option->request.value, memory_order_acquire)};
        const void *const *table = *(const void *const **) listener;
        ((void (*)(void *, const LoadRequestID *, uint32_t, const void *)) table[2])(
            listener, &copy, atomic_load_explicit(&f->result, memory_order_relaxed), where);
        struct azsdk_load_sample sample;
        if (atomic_load_explicit(&f->expect_inline_sample, memory_order_relaxed)) CHECK(azsdk_load_read(0, &sample));
    }
}
static void native_unload(void *self, const TrackLoadOption *option, void *listener) {
    (void) option;
    TrackLoadFacade *f = self;
    CHECK(listener == f->speed_expected);
    atomic_fetch_add_explicit(&f->unloads, 1, memory_order_relaxed);
}
static void native_secondary_unload(void *self, const TrackLoadOption *option, void *listener) { native_unload((char *) self - 8, option, listener); }
static struct native_table facade_table = {0, &facade_type, {[2] = (void *) native_load, [3] = (void *) native_unload}},
                           secondary_table = {-8, &facade_type, {[2] = (void *) native_secondary_unload}},
                           controller_table = {-0x150, &controller_type, {[2] = (void *) native_complete}},
                           stock_table = {-0x120, &controller_type, {0}};
static void controller_init(LoadController *c) {
    memset(c, 0, sizeof *c);
    c->primary = controller_table.slots;
    c->listener = controller_table.slots;
    c->stock = stock_table.slots;
}
static void load(TrackLoadFacade *f, TrackLoadOption option, const void *track, const void *where, const void *speed, void *listener) {
    ((void (*)(void *, const void *, const TrackLoadOption *, const void *, const void *, void *)) f->primary[2])(f, track, &option, where, speed, listener);
}
static void unload(TrackLoadFacade *f, TrackLoadOption option, void *listener) {
    ((void (*)(void *, const TrackLoadOption *, void *)) f->secondary[2])(unload_interface(f), &option, listener);
}
static void primary_unload(TrackLoadFacade *f, TrackLoadOption option, void *listener) {
    ((void (*)(void *, const TrackLoadOption *, void *)) f->primary[3])(f, &option, listener);
}
static void emit(LoadController *c, uint32_t id, uint32_t result, const void *where) {
    LoadRequestID request = {id_table.slots, id};
    ((void (*)(void *, const LoadRequestID *, uint32_t, const void *)) c->listener[2])(listener_interface(c), &request, result, where);
}
typedef struct {
    TrackLoadFacade facade;
    struct Bundle bundle;
    DjPlayerUnit unit;
    LoadController controller;
    int track, where, speed;
} Fixture;
static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->facade.primary = facade_table.slots;
    f->facade.secondary = secondary_table.slots;
    f->bundle.facade = &f->facade;
    f->unit.vtable = unit_table.slots;
    f->unit.bundle = &f->bundle;
    controller_init(&f->controller);
    f->track = 17;
    f->where = 31;
    f->speed = 47;
    f->facade.track_expected = &f->track;
    f->facade.where_expected = &f->where;
    f->facade.speed_expected = &f->speed;
    f->controller.where_expected = &f->where;
    CHECK(offsetof(DjPlayerUnit, bundle) == 0x190);
    CHECK(offsetof(TrackLoadFacade, secondary) == 8);
    CHECK(offsetof(LoadController, listener) == 0x150);
    CHECK(offsetof(LoadController, stock) == 0x120);
}
static void start(Fixture *f, uint32_t id, void *listener) { load(&f->facade, option_create(id), &f->track, &f->where, &f->speed, listener ? listener : listener_interface(&f->controller)); }
static void finish(Fixture *f, uint32_t id, uint32_t result) { emit(&f->controller, id, result, &f->where); }
static void eject(Fixture *f, uint32_t id, bool primary) {
    TrackLoadOption option = option_create(id);
    if (primary) primary_unload(&f->facade, option, &f->speed);
    else unload(&f->facade, option, &f->speed);
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azslipobserver.h"
#include "native_abi_fixture.h"
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
struct Receiver {
    const void *const *vtable;
    unsigned calls, last;
    bool throwing;
    void (*reenter)(void);
};
struct Source {
    unsigned char prefix[0x134];
    unsigned deck;
    pthread_mutex_t mutex;
    struct fixture_owner listeners[128];
    unsigned count;
};
struct Parent {
    unsigned char prefix[0x1120];
    struct fixture_owner source;
};
_Static_assert(offsetof(struct Parent, source) == 0x1120 && offsetof(struct Source, deck) == 0x134, "reviewed offsets");
static struct az_slip_profile profile;
static void (*inside)(struct azce_call *);
static bool fail_registration;
static void source_dispose(void *control) {
    struct Source *s = (void *) ((struct fixture_control *) control + 1);
    for (unsigned i = 0; i < s->count; ++i) fixture_weak_drop(&s->listeners[i]);
    assert(!pthread_mutex_destroy(&s->mutex));
}
static void parent_dispose(void *control) {
    struct Parent *p = (void *) ((struct fixture_control *) control + 1);
    fixture_drop(&p->source);
}
FIXTURE_VTABLE(source_vtable, source_dispose);
FIXTURE_VTABLE(parent_vtable, parent_dispose);
FIXTURE_VTABLE(receiver_control_vtable, fixture_noop);
static void changed(void *object, unsigned raw) {
    struct Receiver *r = object;
    r->calls++;
    r->last = raw;
    if (r->reenter) r->reenter();
    if (r->throwing) {
        errno = EDOM;
        fixture_raise();
    }
}
static const void *const receiver_vtable[] = {(void *) fixture_noop, (void *) fixture_noop, (void *) changed};
static void original(struct azce_call *c) {
    assert(errno == EINTR);
    ++*(unsigned *) (uintptr_t) c->args[7];
    if (inside) inside(c);
    c->result[0] = 0x130200;
    c->result[1] = 0x87654321;
    errno = ERANGE;
}
static void invoke(unsigned kind, void *object, uintptr_t caller, unsigned arg) {
    unsigned forwarded = 0;
    struct azce_call c = {0};
    c.kind = kind;
    c.args[0] = (uintptr_t) object;
    c.args[2] = c.args[5] = arg;
    c.args[7] = (uintptr_t) &forwarded;
    c.caller_link = caller;
    errno = EINTR;
    az_slip_forward(&c, original);
    assert(forwarded == 1 && errno == ERANGE && c.result[0] == 0x130200 && c.result[1] == 0x87654321);
}
static void registration(void *object, const struct az_native_owner *receiver) {
    struct Source *s = object;
    assert(!pthread_mutex_lock(&s->mutex));
    if (fail_registration) {
        fail_registration = false;
        assert(!pthread_mutex_unlock(&s->mutex));
        fixture_raise();
    }
    struct fixture_owner value;
    memcpy(&value, receiver, sizeof value);
    assert(s->count < 128);
    s->listeners[s->count++] = fixture_weak(value);
    assert(!pthread_mutex_unlock(&s->mutex));
}
static void absent(unsigned deck) {
    struct az_slip_receipt out;
    memset(&out, 0xa5, sizeof out);
    struct az_slip_receipt before = out;
    assert(!az_slip_read(deck, &out) && !memcmp(&out, &before, sizeof out));
}
struct Model {
    unsigned deck;
    struct fixture_owner parent, native;
};
static struct Parent *parent(struct Model *m) { return m->parent.object; }
static struct Source *source(struct Model *m) { return parent(m)->source.object; }
static struct Receiver *receiver(struct Model *m) { return m->native.object; }
static struct Model model(unsigned deck, bool publish, bool native_view, bool fail) {
    struct Model m = {.deck = deck, .parent = fixture_make(sizeof(struct Parent), parent_vtable)};
    parent(&m)->source = fixture_make(sizeof(struct Source), source_vtable);
    source(&m)->deck = deck;
    assert(!pthread_mutex_init(&source(&m)->mutex, NULL));
    if (native_view) {
        m.native = fixture_make(sizeof(struct Receiver), receiver_control_vtable);
        receiver(&m)->vtable = receiver_vtable;
        registration(source(&m), (const struct az_native_owner *) &m.native);
    }
    invoke(AZ_SLIP_CONSTRUCT, source(&m), profile.construct_return, deck);
    if (publish) {
        fail_registration = fail;
        invoke(AZ_SLIP_PUBLISH_PARENT, parent(&m), profile.parent_returns[deck], deck);
    }
    return m;
}
static void unlock(pthread_mutex_t **p) { assert(!pthread_mutex_unlock(*p)); }
static void dispatch_source(struct Source *s, unsigned raw) {
    assert(!pthread_mutex_lock(&s->mutex));
    pthread_mutex_t *lock __attribute__((cleanup(unlock))) = &s->mutex;
    (void) lock;
    for (unsigned i = 0; i < s->count; ++i) {
        struct fixture_owner held __attribute__((cleanup(fixture_drop))) = fixture_lock(s->listeners[i]);
        if (held.object) {
            const void *const *table = *(const void *const **) held.object;
            ((void (*)(void *, unsigned)) table[2])(held.object, raw);
        }
    }
}
static void dispatch(struct Model *m, unsigned raw) { dispatch_source(source(m), raw); }
static void destroy(struct Model *m) {
    fixture_drop(&m->native);
    fixture_drop(&m->parent);
}
static struct Model *exception_model, *nested_model;
static void throw_dispatch(void *unused) {
    (void) unused;
    dispatch(exception_model, 9);
}
static void reenter(void) { dispatch(nested_model, 23); }
static void retiring(struct azce_call *unused) {
    (void) unused;
    inside = NULL;
    absent(0);
}
int main(void) {
    profile = (struct az_slip_profile) {0x130200, {0x130204, 0x130208, 0x13020c, 0x130210}, (uintptr_t) source_vtable, (uintptr_t) parent_vtable, registration};
    assert(az_slip_configure(&profile));
    struct Model unbound = model(0, false, true, false);
    dispatch(&unbound, 1);
    assert(receiver(&unbound)->calls == 1);
    absent(0);
    puts("SLIP_NATIVE_FORWARD_BASELINE_OK native weak receiver delivered");
    fflush(stdout);
    struct Model old = model(0, true, true, false);
    dispatch(&old, 255);
    struct az_slip_receipt r = {0};
    assert(az_slip_read(0, &r));
    assert(r.raw == 255 && r.generation && r.revision == 1 && r.observed_ns);
    azsdk_slip_collect_v1();
    const struct azsdk_slip_debug_v1 *debug = &azsdk_slip_observation_v1[0];
    assert(!(debug->sequence & 1) && debug->registered == 1 && debug->valid == 1 && debug->raw == 255 && debug->callbacks == 1 && debug->generation == r.generation && debug->revision == r.revision);
    assert(receiver(&old)->calls == 1 && receiver(&old)->last == 255 && source(&old)->count == 2);
    struct Model failed = model(3, true, true, true);
    dispatch(&failed, 91);
    assert(receiver(&failed)->calls == 1 && source(&failed)->count == 1);
    absent(3);
    destroy(&failed);
    uint64_t generation = r.generation;
    struct Model replacement = model(0, true, true, false);
    absent(0);
    dispatch(&old, 1);
    absent(0);
    dispatch(&replacement, 0);
    assert(az_slip_read(0, &r) && r.generation != generation && r.raw == 0);
    generation = r.generation;
    dispatch(&old, 255);
    assert(receiver(&old)->calls == 3 && az_slip_read(0, &r) && r.generation == generation && r.raw == 0);
    invoke(AZ_SLIP_PUBLISH_PARENT, parent(&old), profile.parent_returns[0], 0);
    assert(az_slip_read(0, &r) && r.generation == generation);
    struct Model no_view[3] = {model(1, true, false, false), model(2, true, false, false), model(3, true, false, false)};
    for (unsigned d = 1; d < 4; d++) {
        dispatch(&no_view[d - 1], 100 + d);
        assert(az_slip_read(d, &r) && r.raw == 100 + d);
    }
    puts("SLIP_GETTER_NATIVE_FORWARD_BASELINE_OK");
    fflush(stdout);
    struct Model *getters[4] = {&replacement, &no_view[0], &no_view[1], &no_view[2]};
    for (unsigned d = 0; d < 4; d++)
        for (unsigned site = 0; site < 3; site++) {
            errno = EINTR;
            azsdk_slip_capture_getter(site, (uintptr_t) source(getters[d]), 0x900000 + d * 0x100 + site * 4);
            assert(errno == EINTR);
        }
    azsdk_slip_collect_v1();
    for (unsigned d = 0; d < 4; d++) {
        const struct azsdk_slip_getter_debug_v1 *g = &azsdk_slip_getter_observation_v1[d];
        assert(g->seen_mask == 7 && !(g->sequence & 1) && g->generation == azsdk_slip_observation_v1[d].generation && g->observed_ns && !g->conflict_mask);
        assert(g->target_slot4 == 0x900000 + d * 0x100 && g->target_slot13 == g->target_slot4 + 4 && g->target_slot14 == g->target_slot4 + 8);
    }
    struct azsdk_slip_getter_debug_v1 *g0 = &azsdk_slip_getter_observation_v1[0];
    azsdk_slip_capture_getter(0, (uintptr_t) source(&old), 0xb00000);
    azsdk_slip_capture_getter(3, (uintptr_t) source(&replacement), 0xb00000);
    azsdk_slip_capture_getter(0, (uintptr_t) source(&replacement), 1);
    azsdk_slip_collect_v1();
    assert(g0->target_slot4 == 0x900000 && !g0->conflict_mask);
    azsdk_slip_capture_getter(0, (uintptr_t) source(&replacement), 0xb00000);
    azsdk_slip_collect_v1();
    assert(g0->target_slot4 == 0x900000 && g0->conflict_mask == 1);
    struct fixture_owner expired = fixture_weak(replacement.native);
    fixture_drop(&replacement.native);
    assert(fixture_expired(expired));
    fixture_weak_drop(&expired);
    dispatch(&replacement, 17);
    assert(az_slip_read(0, &r) && r.raw == 17);
    struct Model exceptions = model(0, true, true, false);
    dispatch(&exceptions, 3);
    receiver(&exceptions)->throwing = true;
    azsdk_slip_collect_v1();
    assert(!g0->seen_mask && !g0->conflict_mask && !g0->target_slot4);
    exception_model = &exceptions;
    assert(!az_native_try(throw_dispatch, NULL));
    assert(errno == EDOM && receiver(&exceptions)->calls == 2 && az_slip_read(0, &r) && r.raw == 3);
    receiver(&exceptions)->throwing = false;
    dispatch(&exceptions, 11);
    assert(az_slip_read(0, &r) && r.raw == 11);
    nested_model = &no_view[1];
    receiver(&exceptions)->reenter = reenter;
    dispatch(&exceptions, 19);
    assert(az_slip_read(0, &r) && r.raw == 19 && az_slip_read(2, &r) && r.raw == 23);
    struct Source *es = source(&exceptions);
    struct fixture_owner late = fixture_lock(es->listeners[es->count - 1]);
    assert(late.object);
    inside = retiring;
    invoke(AZ_SLIP_DISPOSE_SOURCE, parent(&exceptions)->source.control, 0, 0);
    ((void (*)(void *, unsigned))(*(const void *const **) late.object)[2])(late.object, 88);
    absent(0);
    dispatch(&exceptions, 99);
    absent(0);
    assert(receiver(&exceptions)->last == 99);
    fixture_drop(&late);
    struct Model latest = model(0, true, true, false);
    dispatch(&latest, 41);
    invoke(AZ_SLIP_DISPOSE_SOURCE, parent(&old)->source.control, 0, 0);
    assert(az_slip_read(0, &r) && r.raw == 41);
    struct fixture_owner widget_source = fixture_copy(parent(&latest)->source);
    struct fixture_control *control_address = widget_source.control;
    void *address = widget_source.object;
    inside = retiring;
    invoke(AZ_SLIP_DISPOSE_PARENT, latest.parent.control, 0, 0);
    fixture_drop(&latest.parent);
    dispatch_source(widget_source.object, 77);
    absent(0);
    azsdk_slip_collect_v1();
    assert(!debug->registered && !debug->valid && !debug->raw && !debug->observed_ns);
    assert(!g0->generation && !g0->seen_mask && !g0->target_slot4 && !g0->observed_ns);
    invoke(AZ_SLIP_DISPOSE_SOURCE, control_address, 0, 0);
    fixture_drop(&widget_source);
    struct fixture_owner fresh = fixture_make(sizeof(struct Source), source_vtable);
    assert(!pthread_mutex_init(&((struct Source *) fresh.object)->mutex, NULL));
    assert(fresh.object != address && fresh.control != control_address);
    invoke(AZ_SLIP_CONSTRUCT, address, profile.construct_return, 0);
    absent(0);
    absent(4);
    fixture_drop(&fresh);
    destroy(&unbound);
    destroy(&old);
    destroy(&replacement);
    destroy(&exceptions);
    destroy(&latest);
    for (unsigned i = 0; i < 3; ++i) destroy(&no_view[i]);
    assert(fixture_exception_deletions == 2);
    puts("PASS C weak subscription, native coexistence/expiry/unwind, all4 decks and retirement");
}

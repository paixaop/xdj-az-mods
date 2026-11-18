/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C model of the native owned Ticket/reception ABI. The native
 * entry witness is the original promoted control and exact caller PC. */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include "native_abi_fixture.h"
#include "../../src/common/aznative_c.h"
#ifndef REPLY_HEADER
#define REPLY_HEADER "../../src/common/azcachereply.h"
#endif
#if __has_include(REPLY_HEADER)
#include REPLY_HEADER
#define HAVE_REPLY 1
#endif
#ifndef HAVE_REPLY
#include "../../src/common/azcachebirth.h"
struct azcr_request {
    uint64_t request_id[2];
    uint32_t sequence;
    uint16_t reply_kind;
};
struct azcr_promotion {
    uintptr_t control, caller_link;
};
struct azcr_receipt {
    struct azcr_request request;
    struct az_native_owner reception;
};
struct azcr_scope {
    int unused;
};
static void azcr_receipt_drop(struct azcr_receipt *r) { az_native_weak_reset(&r->reception); }
static void azcr_receipt_copy(struct azcr_receipt *d, const struct azcr_receipt *s) {
    az_native_weak_copy(&d->reception, &s->reception);
    d->request = s->request;
}
static void azcr_scope_init(struct azcr_scope *s, const struct az_native_owner *r, const struct azcr_request *q, uintptr_t pc) {
    (void) s;
    (void) r;
    (void) q;
    (void) pc;
}
static void azcr_scope_drop(struct azcr_scope *s) { (void) s; }
static int azcr_forward_result(struct azcr_scope *s, int (*f)(void *), void *p) {
    (void) s;
    return f(p);
}
static void azcr_forward_owned(struct azcr_scope *s, struct azcb_forward f) {
    (void) s;
    f.call(f.context);
}
static bool azcr_receive_owned(struct azcr_scope *s, const struct azcr_promotion *p, const void *r, struct azcr_receipt *o) {
    (void) s;
    (void) p;
    (void) r;
    (void) o;
    return false;
}
static bool azcr_receive_current_owned(const struct azcr_promotion *p, const void *r, struct azcr_receipt *o) {
    (void) p;
    (void) r;
    (void) o;
    return false;
}
#endif
#define CHECK(x, why)                                       \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL cache-reply: %s\n", why); \
            abort();                                        \
        }                                                   \
    } while (0)
#define OWNER __attribute__((cleanup(fixture_drop)))
#define RECEIPT __attribute__((cleanup(azcr_receipt_drop)))
#define SCOPE __attribute__((cleanup(azcr_scope_drop)))
static unsigned packets, receptions, native_calls, callback_calls;
static uintptr_t callback_return;
struct Packet {
    uint16_t kind;
    uint32_t sequence;
};
struct Receiver {
    void (*callback)(struct Receiver *, void *);
    void *context;
};
typedef void (*Entry)(const struct azcr_promotion *, struct Receiver *, void *);
struct Ticket {
    unsigned char prefix[0x80];
    uint32_t tag, padding;
    struct fixture_owner receiver;
    struct Packet *reply;
    struct azcr_request request;
    unsigned state;
};
_Static_assert(offsetof(struct Ticket, receiver) == 0x88, "native tag2 weak receiver offset");
_Static_assert(sizeof(struct fixture_owner) == 16, "native owner pair");
static struct azcr_promotion promotion(uintptr_t control, uintptr_t pc) { return (struct azcr_promotion) {control, pc}; }
static void receiver_dispose(void *p) {
    (void) p;
    --receptions;
}
FIXTURE_VTABLE(receiver_vtable, receiver_dispose);
static struct fixture_owner receiver_new(void) {
    ++receptions;
    return fixture_make(sizeof(struct Receiver), receiver_vtable);
}
static void packet_drop(struct Packet **p) {
    if (*p) {
        --packets;
        free(*p);
        *p = NULL;
    }
}
static void ticket_packet(struct Ticket *t) {
    packet_drop(&t->reply);
    t->reply = calloc(1, sizeof *t->reply);
    CHECK(t->reply, "allocate packet");
    *t->reply = (struct Packet) {0x4001, t->request.sequence};
    ++packets;
}
static struct Ticket ticket_new(struct fixture_owner owner, unsigned context) {
    return (struct Ticket) {.tag = 2, .receiver = fixture_weak(owner), .request = {{0x12340000000000ULL + context, 0xfedcba9880000000ULL + context}, 0xfffffff0U + context, 0x4001}, .state = 1};
}
static void ticket_drop(struct Ticket *t) {
    packet_drop(&t->reply);
    fixture_weak_drop(&t->receiver);
}
#define TICKET __attribute__((cleanup(ticket_drop)))
static void ticket_receiver(struct Ticket *t, struct fixture_owner owner) {
    fixture_weak_drop(&t->receiver);
    t->receiver = fixture_weak(owner);
}
static void scope_init(struct azcr_scope *s, struct Ticket *t) {
    struct az_native_owner w;
    memcpy(&w, &t->receiver, sizeof w);
    azcr_scope_init(s, &w, &t->request, callback_return);
}
static int ticket_process(struct Ticket *t, Entry entry, void *context) {
    ++native_calls;
    struct Packet *local __attribute__((cleanup(packet_drop))) = t->reply;
    t->reply = NULL;
    if (t->state == 3 || !local) return 7;
    t->state = 2;
    struct fixture_owner lease OWNER = fixture_lock(t->receiver);
    if (lease.object) {
        struct azcr_promotion p = promotion((uintptr_t) lease.control, callback_return);
        entry(&p, lease.object, context);
        struct Receiver *receiver = lease.object;
        ++callback_calls;
        if (receiver->callback) receiver->callback(receiver, receiver->context);
    }
    t->state = 3;
    return 7;
}
struct Call {
    struct Ticket *ticket;
    Entry entry;
    void *context;
};
static int process(void *p) {
    struct Call *c = p;
    return ticket_process(c->ticket, c->entry, c->context);
}
static int forward(struct azcr_scope *s, struct Ticket *t, Entry entry, void *context) {
    struct Call c = {t, entry, context};
    return azcr_forward_result(s, process, &c);
}
static bool same_request(const struct azcr_request *a, const struct azcr_request *b) { return a->request_id[0] == b->request_id[0] && a->request_id[1] == b->request_id[1] && a->sequence == b->sequence && a->reply_kind == b->reply_kind; }
static bool unchanged(const struct azcr_receipt *a, const struct azcr_receipt *b) {
    return same_request(&a->request, &b->request) && a->reception.control == b->reception.control && a->reception.object == b->reception.object;
}
static void sentinel(struct azcr_receipt *out, struct fixture_owner owner) {
    out->request = (struct azcr_request) {{0xfeed1234, 0x12345678aabbccddULL}, 0x778899aa, 0x7777};
    struct az_native_owner w;
    memcpy(&w, &owner, sizeof w);
    az_native_weak_copy(&out->reception, &w);
}
struct Baseline {
    struct fixture_owner *handler;
    int callbacks;
};
static void baseline_callback(struct Receiver *self, void *p) {
    struct Baseline *b = p;
    CHECK(self == b->handler->object, "typed native receiver");
    ++b->callbacks;
}
static void baseline_entry(const struct azcr_promotion *p, struct Receiver *self, void *v) {
    struct Baseline *b = v;
    CHECK(p->control == (uintptr_t) b->handler->control && p->caller_link == callback_return && self == b->handler->object && b->handler->control->strong == 2, "native alone promotes real weak receiver");
}
static void native_baseline(void) {
    struct fixture_owner handler OWNER = receiver_new();
    struct Ticket ticket TICKET = ticket_new(handler, 0);
    ticket_packet(&ticket);
    unsigned before = native_calls;
    struct Baseline b = {&handler, 0};
    *(struct Receiver *) handler.object = (struct Receiver) {baseline_callback, &b};
    CHECK(ticket_process(&ticket, baseline_entry, &b) == 7 && native_calls == before + 1 && b.callbacks == 1 && !packets && handler.control->strong == 1, "native owning Ticket forwards once with caller-only packet/receiver cleanup");
    puts("NATIVE_GNU_REPLY_BASELINE_OK");
    fflush(stdout);
}
struct Observation {
    struct azcr_scope *scope;
    struct fixture_owner *handler;
    struct azcr_receipt *out;
    bool observed;
};
struct Foreign {
    const struct azcr_promotion *native;
    struct Receiver *self;
    struct azcr_receipt *out;
    bool result;
};
static void *foreign_receive(void *v) {
    struct Foreign *f = v;
    f->result = azcr_receive_current_owned(f->native, f->self, f->out);
    return NULL;
}
static void observation_entry(const struct azcr_promotion *native, struct Receiver *self, void *v) {
    struct Observation *o = v;
    struct azcr_receipt before RECEIPT = {0};
    azcr_receipt_copy(&before, o->out);
    struct azcr_promotion wrong = promotion(native->control, callback_return + 4), empty = promotion(0, callback_return);
    CHECK(!azcr_receive_owned(o->scope, &wrong, self, o->out) && unchanged(o->out, &before), "wrong native caller PC refuses unchanged output despite actual receiver owner");
    CHECK(!azcr_receive_owned(o->scope, &empty, self, o->out) && unchanged(o->out, &before), "null native promotion refuses unchanged output");
    CHECK(!azcr_receive_owned(o->scope, native, NULL, o->out) && unchanged(o->out, &before), "null native callback alias refuses unchanged output");
    struct Foreign f = {native, self, o->out, true};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, foreign_receive, &f) && !pthread_join(thread, NULL), "foreign thread joins");
    CHECK(!f.result && unchanged(o->out, &before), "thread context alone cannot acquire another native invocation");
    o->observed = azcr_receive_current_owned(native, self, o->out);
    CHECK(o->handler->control->strong == 2, "temporary SDK promotion released before callback original");
}
static void renewed_entry(const struct azcr_promotion *native, struct Receiver *self, void *v) {
    struct Observation *o = v;
    o->observed = azcr_receive_owned(o->scope, native, self, o->out);
}
static void first_observation(unsigned context) {
    struct fixture_owner handler OWNER = receiver_new();
    struct Ticket ticket TICKET = ticket_new(handler, context);
    struct azcr_scope scope SCOPE;
    scope_init(&scope, &ticket);
    CHECK(handler.control->strong == 1, "scope copies weak owner without pre-promoting Reception");
    struct azcr_receipt out RECEIPT = {0};
    struct Observation o = {&scope, &handler, &out, false};
    ticket_packet(&ticket);
    CHECK(forward(&scope, &ticket, observation_entry, &o) == 7, "reply wrapper preserves native return");
    CHECK(o.observed && same_request(&out.request, &ticket.request) && out.reception.control == (void *) handler.control && out.reception.object == handler.object, "missing selected Ticket to native-owned Reception receipt");
    CHECK(!packets && handler.control->strong == 1, "receipt retains no packet or strong receiver");
    struct azcr_receipt saved RECEIPT = {0};
    azcr_receipt_copy(&saved, &out);
    struct azcr_promotion p = promotion((uintptr_t) handler.control, callback_return);
    CHECK(!azcr_receive_owned(&scope, &p, handler.object, &out) && unchanged(&saved, &out), "late callback outside original invocation is refused without changing output");
    CHECK(!azcr_receive_current_owned(&p, handler.object, &out) && unchanged(&saved, &out), "current callback API declines outside native forwarding scope");
    ticket.state = 1;
    ticket_packet(&ticket);
    o.observed = true;
    CHECK(forward(&scope, &ticket, renewed_entry, &o) == 7 && !o.observed && unchanged(&saved, &out), "reusing completed scope cannot renew receipt even for identical native owner/request scalars");
    fixture_drop(&handler);
    CHECK(!az_native_alive(out.reception.control), "copied receipt cannot keep a retired Reception alive");
}
struct Refusal {
    struct azcr_scope *scope;
    struct azcr_receipt *out, *before;
    struct fixture_owner *first;
};
static void refusal_entry(const struct azcr_promotion *p, struct Receiver *self, void *v) {
    struct Refusal *r = v;
    struct azcr_promotion empty = promotion(0, callback_return);
    CHECK(!azcr_receive_owned(r->scope, p, self, r->out) && unchanged(r->out, r->before), "replacement owner cannot inherit old Ticket receipt");
    CHECK(!azcr_receive_owned(r->scope, &empty, self, r->out), "missing native promoted control witness declines");
    CHECK(!azcr_receive_owned(r->scope, p, r->first->object, r->out), "foreign callback alias declines");
}
static void called_entry(const struct azcr_promotion *p, struct Receiver *r, void *v) {
    (void) p;
    (void) r;
    *(bool *) v = true;
}
static void refusal_and_cancel(void) {
    struct fixture_owner first OWNER = receiver_new(), replacement OWNER = receiver_new();
    struct Ticket ticket TICKET = ticket_new(first, 1);
    struct azcr_scope scope SCOPE;
    scope_init(&scope, &ticket);
    struct azcr_receipt out RECEIPT = {0}, before RECEIPT = {0};
    sentinel(&out, first);
    azcr_receipt_copy(&before, &out);
    ticket_packet(&ticket);
    ticket_receiver(&ticket, replacement);
    struct Refusal r = {&scope, &out, &before, &first};
    CHECK(forward(&scope, &ticket, refusal_entry, &r) == 7 && !packets, "refusal still forwards and cleans native result once");
    struct Ticket cancelled TICKET = ticket_new(first, 2);
    struct azcr_scope cancelled_scope SCOPE;
    scope_init(&cancelled_scope, &cancelled);
    ticket_packet(&cancelled);
    bool called = false;
    fixture_drop(&first);
    CHECK(forward(&cancelled_scope, &cancelled, called_entry, &called) == 7 && !called, "SDK weak snapshot must not revive native-cancelled receiver");
    struct Ticket explicit_cancel TICKET = ticket_new(replacement, 3);
    struct azcr_scope cancel_scope SCOPE;
    scope_init(&cancel_scope, &explicit_cancel);
    ticket_packet(&explicit_cancel);
    explicit_cancel.state = 3;
    called = false;
    CHECK(forward(&cancel_scope, &explicit_cancel, called_entry, &called) == 7 && !called && !packets, "native cancellation suppresses callback without changing original cleanup");
}
static void alias_entry(const struct azcr_promotion *p, struct Receiver *self, void *v) {
    struct Refusal *r = v;
    CHECK(self == r->first->object && !azcr_receive_owned(r->scope, p, self, r->out) && unchanged(r->out, r->before), "same receiver address and request cannot substitute a different GNU owner control");
}
static void same_alias_distinct_owner(void) {
    struct Receiver receiver = {0};
    struct fixture_owner a OWNER = receiver_new(), b OWNER = receiver_new();
    a.object = b.object = &receiver;
    struct Ticket ticket TICKET = ticket_new(a, 0);
    struct azcr_scope scope SCOPE;
    scope_init(&scope, &ticket);
    ticket_receiver(&ticket, b);
    ticket_packet(&ticket);
    struct azcr_receipt out RECEIPT = {0}, before RECEIPT = {0};
    sentinel(&out, a);
    azcr_receipt_copy(&before, &out);
    struct Refusal r = {&scope, &out, &before, &a};
    forward(&scope, &ticket, alias_entry, &r);
}
struct Nested {
    struct azcr_scope *outer_scope, *inner_scope;
    struct Ticket *inner;
    struct azcr_receipt *out, *before;
    const struct azcr_promotion *outer_native;
    struct Receiver *outer_self;
    bool nested, restored;
};
static void inner_entry(const struct azcr_promotion *p, struct Receiver *self, void *v) {
    struct Nested *n = v;
    CHECK(!azcr_receive_owned(n->outer_scope, n->outer_native, n->outer_self, n->out) && unchanged(n->out, n->before), "nested invocation masks older scope even for same owner and wire scalars");
    n->nested = azcr_receive_current_owned(p, self, n->out);
    CHECK(same_request(&n->out->request, &n->inner->request), "current callback API selects actual nested receipt");
}
static void unknown_body(void *v) {
    struct Nested *n = v;
    CHECK(!azcr_receive_current_owned(n->outer_native, n->outer_self, n->out) && unchanged(n->out, n->before) && !azcr_receive_owned(n->outer_scope, n->outer_native, n->outer_self, n->out), "unqualified nested invocation masks older valid receiver scope");
}
static void inner_body(void *v) {
    struct Nested *n = v;
    ticket_process(n->inner, inner_entry, n);
    struct azcr_scope unknown SCOPE;
    struct az_native_owner empty = {0};
    azcr_scope_init(&unknown, &empty, &n->inner->request, callback_return);
    struct azcr_receipt prior RECEIPT = {0};
    azcr_receipt_copy(&prior, n->out);
    struct azcr_receipt *saved = n->before;
    n->before = &prior;
    azcr_forward_owned(&unknown, (struct azcb_forward) {unknown_body, n});
    n->before = saved;
    fixture_raise();
}
static void nested_guard(void *v) {
    struct Nested *n = v;
    azcr_forward_owned(n->inner_scope, (struct azcb_forward) {inner_body, n});
}
static void outer_entry(const struct azcr_promotion *p, struct Receiver *self, void *v) {
    struct Nested *n = v;
    n->outer_native = p;
    n->outer_self = self;
    CHECK(!az_native_try(nested_guard, n), "native nested unwind reaches outer handler");
    n->restored = azcr_receive_current_owned(p, self, n->out);
}
static void throw_callback(struct Receiver *r, void *v) {
    (void) r;
    (void) v;
    fixture_raise();
}
static void noop_entry(const struct azcr_promotion *p, struct Receiver *r, void *v) {
    (void) p;
    (void) r;
    (void) v;
}
struct Throwing {
    struct azcr_scope *scope;
    struct Ticket *ticket;
};
static void throwing_body(void *v) {
    struct Throwing *t = v;
    forward(t->scope, t->ticket, noop_entry, NULL);
}
static void nesting_and_throw(void) {
    struct fixture_owner owner OWNER = receiver_new();
    struct Ticket outer TICKET = ticket_new(owner, 0), inner TICKET = ticket_new(owner, 0);
    struct azcr_scope outer_scope SCOPE, inner_scope SCOPE;
    scope_init(&outer_scope, &outer);
    scope_init(&inner_scope, &inner);
    ticket_packet(&outer);
    ticket_packet(&inner);
    struct azcr_receipt out RECEIPT = {0}, before RECEIPT = {0};
    sentinel(&out, owner);
    azcr_receipt_copy(&before, &out);
    struct Nested n = {&outer_scope, &inner_scope, &inner, &out, &before, NULL, NULL, false, false};
    CHECK(forward(&outer_scope, &outer, outer_entry, &n) == 7 && n.nested && n.restored && !packets, "nested native result/unwind restores only actual outer invocation");
    struct Ticket throwing TICKET = ticket_new(owner, 1);
    struct azcr_scope throwing_scope SCOPE;
    scope_init(&throwing_scope, &throwing);
    ticket_packet(&throwing);
    ((struct Receiver *) owner.object)->callback = throw_callback;
    unsigned calls = native_calls, deletions = fixture_exception_deletions;
    struct Throwing t = {&throwing_scope, &throwing};
    bool caught = !az_native_try(throwing_body, &t);
    azcr_receipt_copy(&before, &out);
    struct azcr_promotion p = promotion((uintptr_t) owner.control, callback_return);
    CHECK(caught && fixture_exception_deletions == deletions + 1 && native_calls == calls + 1 && !packets && owner.control->strong == 1 && !azcr_receive_owned(&throwing_scope, &p, owner.object, &out) && unchanged(&out, &before), "native callback unwind propagates once and retires scoped admission with native cleanup");
}
int main(int argc, char **argv) {
    unsigned profile = argc > 1 ? (unsigned) strtoul(argv[1], NULL, 10) : 1;
    CHECK(profile == 1 || profile == 2, "supported fixture profile");
    callback_return = profile == 1 ? 0x10b408c : 0x1144490;
    native_baseline();
    for (unsigned i = 0; i < 4; ++i) first_observation(i);
    refusal_and_cancel();
    same_alias_distinct_owner();
    nesting_and_throw();
    CHECK(!packets && !receptions, "all native fixture owners cleaned by callers");
    printf("CACHE_REPLY_GNU_OWNERS_GREEN profile=%u\n", profile);
}

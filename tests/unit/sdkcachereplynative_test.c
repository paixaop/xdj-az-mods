/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual dispatcher/source helpers with independent C native Ticket/Reception
 * layouts. The synthetic code span proves only this fixture's callback site. */
#define SDK_CACHE_NATIVE_MAIN sdkcachereply_native_baseline
#include "sdkcachenative_test.c"
#undef SDK_CACHE_NATIVE_MAIN
#include "../../src/common/azcachereplynative.h"
bool azcr_current_callback_owned(struct azcr_callback_receipt *) __attribute__((weak));
void azsdk_cache_test_reply_profile(unsigned, uintptr_t, uintptr_t, size_t, const char *) __attribute__((weak));
void azsdk_reply_fixture_hash(const void *, size_t, char *);
static bool reply_capture_ready = true;
static unsigned reply_packets, reply_receivers, reply_tickets;
typedef struct ReplyPacket {
    uint16_t kind;
    unsigned char opaque[6];
    uint32_t sequence, opaque0c;
    unsigned *destroyed;
    unsigned char tail[0x40 - 24];
} ReplyPacket;
typedef struct Reception {
    const void *vtable;
    uint32_t pack_id, opaque;
    void *stocker;
} Reception;
typedef struct Ticket {
    const void *vtable;
    uint64_t request_id[2];
    unsigned char opaque18[8];
    uint32_t expected_sequence, opaque24;
    ReplyPacket *packet;
    unsigned char opaque30[0x41 - 0x30], state, opaque42[0x80 - 0x42];
    uint32_t tag, opaque84;
    CacheOwner receiver;
} Ticket;
_Static_assert(sizeof(ReplyPacket) == 0x40 && offsetof(ReplyPacket, sequence) == 8, "native packet prefix");
_Static_assert(offsetof(Reception, pack_id) == 8 && offsetof(Reception, stocker) == 0x10, "native Reception scalar prefix");
_Static_assert(offsetof(Ticket, request_id) == 8 && offsetof(Ticket, packet) == 0x28 && offsetof(Ticket, state) == 0x41 && offsetof(Ticket, tag) == 0x80 && offsetof(Ticket, receiver) == 0x88, "native Ticket owner/request/packet fields");
static const struct cbf_type reply_reception_type = {NULL, "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE"}, reply_ticket_type = {NULL, "N13music_library21LoadInfoRequestTicketE"};
static const struct {
    uintptr_t offset;
    const struct cbf_type *type;
    const void *slots[14];
} reply_ticket_table = {0, &reply_ticket_type, {0}};
static const struct cbf_table reply_reception_table = {0, &reply_reception_type, {0}};
static void reply_reception_init(Reception *r, uint32_t pack, void *stocker) {
    *r = (Reception) {reply_reception_table.slots, pack, 0xaabbccdd, stocker};
    ++reply_receivers;
}
static void reply_reception_dispose(void *v) {
    (void) v;
    --reply_receivers;
}
FIXTURE_VTABLE(reply_reception_control, reply_reception_dispose);
FIXTURE_VTABLE(reply_borrow_control, fixture_noop);
static CacheOwner reception_new(uint32_t pack, void *stocker) {
    CacheOwner o = fixture_make(sizeof(Reception), reply_reception_control);
    reply_reception_init(o.object, pack, stocker);
    return o;
}
static CacheOwner reception_borrow(Reception *r) {
    CacheOwner o = fixture_make(0, reply_borrow_control);
    o.object = r;
    return o;
}
static void reception_ptr_drop(Reception **r) {
    if (*r) {
        --reply_receivers;
        free(*r);
        *r = NULL;
    }
}
static ReplyPacket *reply_packet_new(uint32_t sequence) {
    ReplyPacket *p = calloc(1, sizeof *p);
    REQUIRE(p, "reply packet allocation");
    p->kind = 0x4001;
    p->sequence = sequence;
    ++reply_packets;
    return p;
}
static void reply_packet_drop(ReplyPacket **p) {
    if (*p) {
        --reply_packets;
        if ((*p)->destroyed) ++*(*p)->destroyed;
        free(*p);
        *p = NULL;
    }
}
static Ticket *ticket_new(CacheOwner owner, unsigned context) {
    Ticket *t = calloc(1, sizeof *t);
    REQUIRE(t, "Ticket allocation");
    t->vtable = reply_ticket_table.slots;
    t->request_id[0] = UINT64_C(0x8070605040302000) + context;
    t->request_id[1] = UINT64_C(0xfedcba9876543200) + context;
    t->expected_sequence = 0xfffffff0U + context;
    uint16_t expected = 0x4001;
    memcpy(t->opaque18 + 2, &expected, 2);
    t->state = 2;
    t->tag = 2;
    t->receiver = fixture_weak(owner);
    t->packet = reply_packet_new(t->expected_sequence);
    ++reply_tickets;
    return t;
}
static void ticket_drop(Ticket **t) {
    if (*t) {
        reply_packet_drop(&(*t)->packet);
        fixture_weak_drop(&(*t)->receiver);
        free(*t);
        *t = NULL;
        --reply_tickets;
    }
}
int azsdk_cache_capture_ready(void) { return reply_capture_ready ? 1 : 0; }
#define CALLBACK_RECEIPT __attribute__((cleanup(azcr_callback_receipt_drop)))
static bool reply_receipt(struct azcr_callback_receipt *out) {
    bool (*getter)(struct azcr_callback_receipt *) = azcr_current_callback_owned;
    return getter && getter(out);
}
static bool equal_request(const struct azcr_request *a, const struct azcr_request *b) { return a->request_id[0] == b->request_id[0] && a->request_id[1] == b->request_id[1] && a->sequence == b->sequence && a->reply_kind == b->reply_kind; }
static bool same_receipt(const struct azcr_callback_receipt *a, const struct azcr_callback_receipt *b) { return equal_request(&a->selected.request, &b->selected.request) && a->selected.reception.control == b->selected.reception.control && a->pack_id == b->pack_id && a->borrowed_stocker == b->borrowed_stocker; }
static void reply_absent(void) {
    struct azcr_callback_receipt out CALLBACK_RECEIPT = {0}, before CALLBACK_RECEIPT = {0};
    out.selected.request.request_id[1] = UINT64_C(0xdead1234abcd5678);
    out.pack_id = 0x98765432;
    out.borrowed_stocker = 0x12345678;
    azcr_callback_receipt_copy(&before, &out);
    REQUIRE(!reply_receipt(&out) && same_receipt(&out, &before), "callback receipt unavailable outside actual native callback scope");
}
typedef struct ReplyCall {
    uint64_t stocker_marker;
    CacheOwner owner;
    Ticket *ticket;
    struct azcr_request expected;
    struct azcr_callback_receipt observed;
    Action before_promotion, inside_callback, after_callback;
    unsigned forwarded, callbacks, packet_destroyed;
    bool got, drop_ticket;
    uintptr_t callback_link, callback_control;
    uint32_t ticket_reserved, reception_reserved;
    uint64_t ticket_caller_reserved, reception_caller_reserved;
} ReplyCall;
static void reply_ticket_original(azce_call *) __attribute__((aligned(16)));
static uintptr_t reply_code_begin(void) { return (uintptr_t) reply_ticket_original; }
static void reply_call_init(ReplyCall *c, unsigned context, CacheOwner actual) {
    *c = (ReplyCall) {0};
    c->owner = actual.object ? fixture_copy(actual) : reception_new(0x88776650U + context, &c->stocker_marker);
    c->ticket = ticket_new(c->owner, context);
    memcpy(c->expected.request_id, c->ticket->request_id, sizeof c->expected.request_id);
    c->expected.sequence = c->ticket->packet->sequence;
    c->expected.reply_kind = c->ticket->packet->kind;
    c->ticket->packet->destroyed = &c->packet_destroyed;
    c->observed.selected.request.request_id[1] = UINT64_C(0xdeadbeef00112233);
    c->observed.pack_id = 0x10293847;
    c->observed.borrowed_stocker = 0x87654321;
    c->callback_link = reply_code_begin() + 4;
}
static void reply_call_drop(ReplyCall *c) {
    ticket_drop(&c->ticket);
    azcr_callback_receipt_drop(&c->observed);
    fixture_drop(&c->owner);
}
#define REPLY_CALL(name, context, actual)                     \
    ReplyCall name __attribute__((cleanup(reply_call_drop))); \
    reply_call_init(&name, context, actual)
static void reply_profile(bool bad_hash, unsigned profile_build) {
    char hash[65];
    REQUIRE(image_contains(&source_state().main_image, reply_code_begin(), 16, kRead | kExecute), "test promotion profile refers to actual owned main-ELF RX bytes");
    azsdk_reply_fixture_hash((void *) reply_code_begin(), 16, hash);
    if (bad_hash) hash[0] = hash[0] == '0' ? '1' : '0';
    void (*profile)(unsigned, uintptr_t, uintptr_t, size_t, const char *) = azsdk_cache_test_reply_profile;
    if (profile) profile(profile_build ? profile_build : build_id, reply_code_begin(), reply_code_begin() + 4, 16, hash);
}
static void reply_receiver_original(azce_call *frame) {
    ReplyCall *c = (void *) frame->args[7];
    ++c->callbacks;
    REQUIRE(frame->args[0] && frame->args[1] && frame->caller_x21 && frame->caller_link == c->callback_link, "native Reception original receives unchanged owned receiver/payload/X21/LR");
    struct azcr_callback_receipt before CALLBACK_RECEIPT = {0};
    azcr_callback_receipt_copy(&before, &c->observed);
    c->got = reply_receipt(&c->observed);
    REQUIRE(c->got || same_receipt(&c->observed, &before), "refused native callback receipt preserves output");
    action_run(c->inside_callback);
    frame->result[0] = UINT64_C(0x1345abcd87659870);
    frame->result[1] = UINT64_C(0xc001d00d88997766);
}
static void reply_ticket_original(azce_call *frame) {
    ReplyCall *c = (void *) frame->args[7];
    ++c->forwarded;
    Ticket *native = c->ticket;
    REQUIRE(frame->args[0] == (uintptr_t) native, "actual selected Ticket receiver unchanged");
    ReplyPacket *packet __attribute__((cleanup(reply_packet_drop))) = native->packet;
    native->packet = NULL;
    action_run(c->before_promotion);
    if (native->state != 3) {
        CacheOwner lease OWNED = fixture_lock(native->receiver);
        if (lease.object) {
            _Alignas(8) unsigned char borrowed_info[0x100] = {0};
            azce_call receiver = {0};
            receiver.kind = AZCE_KIND_REPLY_RECEPTION;
            receiver.reserved = c->reception_reserved;
            receiver.caller_reserved = c->reception_caller_reserved;
            receiver.args[0] = (uintptr_t) lease.object;
            receiver.args[1] = (uintptr_t) borrowed_info;
            receiver.args[7] = (uintptr_t) c;
            receiver.caller_x21 = c->callback_control ? c->callback_control : (uintptr_t) lease.control;
            receiver.caller_link = c->callback_link;
            receiver.original = (uintptr_t) reply_receiver_original;
            azce_call original = receiver;
            azsdk_cache_dispatch_call(&receiver);
            REQUIRE(!memcmp(receiver.args, original.args, sizeof receiver.args) && receiver.caller_x21 == original.caller_x21 && receiver.caller_link == original.caller_link && receiver.result[0] == UINT64_C(0x1345abcd87659870) && receiver.result[1] == UINT64_C(0xc001d00d88997766), "Reception dispatcher preserves native input frame/results");
            reply_absent();
            action_run(c->after_callback);
        }
    }
    native->state = 3;
    reply_packet_drop(&packet);
    if (c->drop_ticket) ticket_drop(&c->ticket);
    frame->result[0] = UINT64_C(0x2345abcd12349876);
    frame->result[1] = UINT64_C(0x9988776655443322);
}
static void reply_call_run(void *v) {
    ReplyCall *c = v;
    struct azcr_callback_receipt enclosing CALLBACK_RECEIPT = {0};
    bool inside = reply_receipt(&enclosing);
    azce_call frame = {0};
    frame.kind = AZCE_KIND_REPLY_TICKET;
    frame.reserved = c->ticket_reserved;
    frame.caller_reserved = c->ticket_caller_reserved;
    frame.args[0] = (uintptr_t) c->ticket;
    frame.args[7] = (uintptr_t) c;
    frame.original = (uintptr_t) reply_ticket_original;
    azce_call input = frame;
    azsdk_cache_dispatch_call(&frame);
    REQUIRE(c->forwarded == 1 && !memcmp(frame.args, input.args, sizeof frame.args) && frame.result[0] == UINT64_C(0x2345abcd12349876) && frame.result[1] == UINT64_C(0x9988776655443322) && c->packet_destroyed == 1, "actual native Ticket original/result/packet cleanup completes once");
    if (inside) {
        struct azcr_callback_receipt restored CALLBACK_RECEIPT = {0};
        REQUIRE(reply_receipt(&restored) && same_receipt(&restored, &enclosing), "nested Ticket forwarding restores the still-active enclosing callback receipt");
    } else reply_absent();
    printf("NATIVE_REPLY_FORWARD_OK build=%u callbacks=%u\n", build_id, c->callbacks);
    fflush(stdout);
}
static void reply_call_check(const ReplyCall *c) {
    Reception *r = c->owner.object;
    REQUIRE(c->got && equal_request(&c->observed.selected.request, &c->expected) && c->observed.selected.reception.control == (void *) c->owner.control && c->observed.selected.reception.object == r && !fixture_expired(c->owner) && c->observed.pack_id == r->pack_id && c->observed.borrowed_stocker == (uintptr_t) r->stocker, "missing actual Ticket dispatch to native-owned callback receipt");
    REQUIRE(owner_count(c->owner) == 1, "actual dispatcher retains no strong Reception after original");
}
static void reply_positive(unsigned context, bool baseline) {
    REPLY_CALL(call, context, (CacheOwner) {0});
    call.drop_ticket = true;
    reply_call_run(&call);
    REQUIRE(call.callbacks == 1 && !call.ticket && !reply_tickets, "native alone cleans Ticket/weak holder before observer returns");
    if (baseline) {
        printf("NATIVE_REPLY_OWNER_BASELINE_OK build=%u\n", build_id);
        fflush(stdout);
    } else reply_call_check(&call);
}
static void reply_refused(void) {
    for (unsigned mode = 0; mode < 11; ++mode) {
        reply_profile(mode == 1, mode == 2 ? (build_id == 1 ? 2u : 1u) : 0);
        REPLY_CALL(call, mode, (CacheOwner) {0});
        CacheOwner foreign_owner OWNED = reception_new(0, NULL);
        if (mode == 0) call.callback_link += 4;
        if (mode == 3) call.callback_control = (uintptr_t) foreign_owner.control;
        if (mode == 4) reply_capture_ready = false;
        if (mode == 5) call.ticket->tag = 1;
        void (*profile)(unsigned, uintptr_t, uintptr_t, size_t, const char *) = azsdk_cache_test_reply_profile;
        if (mode == 6 && profile) profile(0, 0, 0, 0, NULL);
        if (mode == 7) call.ticket_reserved = 1;
        if (mode == 8) call.reception_reserved = 1;
        if (mode == 9) call.ticket_caller_reserved = 1;
        if (mode == 10) call.reception_caller_reserved = 1;
        reply_call_run(&call);
        reply_capture_ready = true;
        REQUIRE(call.callbacks == 1 && !call.got, mode >= 7 ? "incompatible native frame forwards but must not expose an owned reply receipt" : "unqualified code/PC/control/variant/startup still forwards without receipt");
    }
    reply_profile(false, 0);
}
static void cancel_receiver(void *v) {
    ReplyCall *c = v;
    fixture_drop(&c->owner);
}
struct ReceiverReplacement {
    ReplyCall *call;
    CacheOwner replacement;
};
static void replace_receiver(void *v) {
    struct ReceiverReplacement *r = v;
    weak_set(&r->call->ticket->receiver, r->replacement);
}
static void cancellation_and_replacement(void) {
    REPLY_CALL(cancel, 0, (CacheOwner) {0});
    cancel.before_promotion = (Action) {cancel_receiver, &cancel};
    reply_call_run(&cancel);
    REQUIRE(!cancel.callbacks && !cancel.got, "SDK snapshot does not rescue native-expired receiver");
    REPLY_CALL(cancelled, 1, (CacheOwner) {0});
    cancelled.ticket->state = 3;
    reply_call_run(&cancelled);
    REQUIRE(!cancelled.callbacks && !cancelled.got, "native cancellation remains callback-free");
    REPLY_CALL(replace, 2, (CacheOwner) {0});
    Reception *r = replace.owner.object;
    CacheOwner next OWNED = reception_new(r->pack_id, r->stocker);
    struct ReceiverReplacement replacement = {&replace, next};
    replace.before_promotion = (Action) {replace_receiver, &replacement};
    reply_call_run(&replace);
    REQUIRE(replace.callbacks == 1 && !replace.got, "replacement with same scalars has different actual GNU owner");
    Reception *pointee __attribute__((cleanup(reception_ptr_drop))) = malloc(sizeof *pointee);
    REQUIRE(pointee, "alias Reception allocation");
    reply_reception_init(pointee, 0, NULL);
    CacheOwner a OWNED = reception_borrow(pointee), b OWNED = reception_borrow(pointee);
    REPLY_CALL(alias, 3, a);
    struct ReceiverReplacement alias_replacement = {&alias, b};
    alias.before_promotion = (Action) {replace_receiver, &alias_replacement};
    reply_call_run(&alias);
    REQUIRE(alias.callbacks == 1 && !alias.got, "same alias cannot replace actual native promoted control identity");
}
static void nested_reply(void *v) {
    ReplyCall *inner = v;
    struct azcr_callback_receipt prior CALLBACK_RECEIPT = {0}, restored CALLBACK_RECEIPT = {0};
    REQUIRE(reply_receipt(&prior), "outer callback receipt exists before nested native dispatch");
    reply_call_run(inner);
    reply_call_check(inner);
    REQUIRE(reply_receipt(&restored) && same_receipt(&prior, &restored), "nested callback restores exact actual outer receipt");
}
static void nested_and_throw(void) {
    REPLY_CALL(outer, 0, (CacheOwner) {0});
    REPLY_CALL(inner, 1, (CacheOwner) {0});
    outer.inside_callback = (Action) {nested_reply, &inner};
    reply_call_run(&outer);
    reply_call_check(&outer);
    REPLY_CALL(throwing, 2, (CacheOwner) {0});
    throwing.inside_callback = (Action) {raise_action, NULL};
    bool caught = catches(reply_call_run, &throwing);
    REQUIRE(caught && throwing.forwarded == 1 && throwing.callbacks == 1 && !reply_packets && owner_count(throwing.owner) == 1, "native callback exception propagates once with native packet/strong owner cleanup");
    reply_absent();
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "build arg");
    build_id = (unsigned) (argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, (int) build_id, 0) == AZS_OK, "actual SourceState main-ELF guards initialized");
    reply_profile(false, 0);
    reply_absent();
    reply_positive(0, true);
    for (unsigned i = 0; i < 4; ++i) reply_positive(i, false);
    reply_refused();
    cancellation_and_replacement();
    nested_and_throw();
    REQUIRE(!reply_packets && !reply_tickets && !reply_receivers, "native callers alone clean all Ticket/Reception/packet fixture owners");
    printf("PASS native-cache-reply build=%u: actual dispatcher/scoped owner receipt only\n", build_id);
    return 0;
}

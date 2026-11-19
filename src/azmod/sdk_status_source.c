/* SPDX-License-Identifier: GPL-3.0-only */
/* One bounded, fail-closed join for the traced local member-0 UDS route.
 * Included in azmod.c next to the native hook installers. */
#include "../common/azstatus_cue_origin.h"
extern void azfile_native_transport(unsigned, uintptr_t, unsigned) __attribute__((weak));
#include "../common/azstatus_source.h"
#include "sdk_status_source_profile.h"
#include <pthread.h>
#include <stdatomic.h>
#define JS_ROUTES 64u
#define JS_PACKS 32u
#define JS_BIT(x) (1u << (x))
enum {
    JS_SENT,
    JS_WIRE,
    JS_INGRESS,
    JS_SERVER,
    JS_RESPONSE,
    JS_RESPONSE_WIRE,
    JS_CLIENT,
    JS_TICKET
};
struct js_packet {
    uint32_t seq, kind, field0, field1;
};
struct js_route {
    struct js_packet query;
    uint32_t bits;
    uintptr_t sent, wire, received, server, response, reply_wire, client_packet, sender, responder;
    uint64_t source, incarnation;
    int dead;
};
struct js_ticket {
    uintptr_t ticket, receiver, control;
    uint64_t req[2], source, first_incarnation;
    uint32_t track_id, first, next, expected_next;
    uintptr_t next_packet;
};
struct js_pack {
    uintptr_t address, receiver;
    unsigned char track[12];
    uint32_t id;
    uint64_t token, owner;
};
static struct {
    pthread_mutex_t mu;
    _Atomic unsigned ready;
    _Atomic uint64_t lost_routes, lost_packs;
    uint64_t next_token, next_route;
    unsigned build;
    struct js_route routes[JS_ROUTES];
    struct js_ticket tickets[JS_ROUTES];
    struct js_pack packs[JS_PACKS];
    void *original[8];
} js = {.mu = PTHREAD_MUTEX_INITIALIZER};
static __thread struct {
    uint32_t seq;
    uint64_t owner;
    unsigned depth;
} js_serving;
static __thread struct {
    uint32_t seq;
    uintptr_t ticket;
    unsigned depth;
} js_processing;
static int js_lock(uint64_t route, uint64_t pack) {
    if (!atomic_load_explicit(&js.ready, memory_order_acquire)) return 0;
    if (pthread_mutex_trylock(&js.mu)) {
        if (route) atomic_fetch_or(&js.lost_routes, route);
        if (pack) atomic_fetch_or(&js.lost_packs, pack);
        return 0;
    }
    uint64_t lost = atomic_exchange(&js.lost_routes, 0);
    while (lost) {
        unsigned i = (unsigned) __builtin_ctzll(lost);
        js.routes[i].dead = 1;
        lost &= lost - 1;
    }
    lost = atomic_exchange(&js.lost_packs, 0);
    while (lost) {
        unsigned i = (unsigned) __builtin_ctzll(lost);
        memset(&js.packs[i], 0, sizeof js.packs[i]);
        lost &= lost - 1;
    }
    return 1;
}
static uint64_t js_route_mask(uint32_t seq) { return UINT64_C(1) << (seq % JS_ROUTES); }
static uint64_t js_pack_mask(uint32_t id) { return UINT64_C(1) << (id % JS_PACKS); }
static uint64_t js_word(const void *p, unsigned offset, unsigned bytes) {
    uint64_t value = 0;
    if (p) memcpy(&value, (const unsigned char *) p + offset, bytes);
    return value;
}
static int js_query_kind(unsigned kind) { return kind == 0x2102 || kind == 0x3000; }
static int js_response_kind(unsigned kind) { return kind == 0x4000 || kind == 0x4201; }
static int js_arguments(const void *packet, struct js_packet *out) {
    uintptr_t begin = js_word(packet, 40, 8), end = js_word(packet, 48, 8), cap = js_word(packet, 56, 8);
    if (!begin || end < begin || cap < end || end - begin < 96 || end - begin > 4096 * 48u ||
        (end - begin) % 48 || js_word((void *) begin, 0, 4) != 1 || js_word((void *) begin, 48, 4) != 1) return 0;
    out->field0 = (uint32_t) js_word((void *) begin, 8, 4);
    out->field1 = (uint32_t) js_word((void *) begin, 56, 4);
    return 1;
}
static int js_packet(const void *p, struct js_packet *out) {
    if (!p || js_word(p, 4, 4) != 0x872349ae) return 0;
    out->kind = (uint32_t) js_word(p, 0, 2);
    out->seq = (uint32_t) js_word(p, 8, 4);
    out->field0 = out->field1 = 0;
    if (js_query_kind(out->kind)) return js_arguments(p, out);
    return js_response_kind(out->kind);
}
static struct js_route *js_route(uint32_t seq) {
    struct js_route *r = &js.routes[seq % JS_ROUTES];
    return seq && !r->dead && r->query.seq == seq ? r : NULL;
}
static int js_reply(const struct js_route *r, unsigned kind) {
    return (r->query.kind == 0x2102 && kind == 0x4000) || (r->query.kind == 0x3000 && kind == 0x4201);
}
static void js_query_sent(const struct js_packet *packet, uintptr_t message, uintptr_t sender) {
    struct js_route *route = &js.routes[packet->seq % JS_ROUTES];
    if (!packet->seq || route->query.seq == packet->seq || js.next_route == UINT64_MAX) route->dead = 1;
    else *route = (struct js_route) {.query = *packet, .bits = JS_BIT(JS_SENT), .sent = message, .sender = sender, .incarnation = ++js.next_route};
}
static void js_response_sent(const struct js_packet *packet, uintptr_t message, uintptr_t sender) {
    struct js_route *route = js_route(packet->seq);
    if (route && js_reply(route, packet->kind) && route->bits == 15 && js_serving.depth == 1 &&
        js_serving.seq == packet->seq && js_serving.owner == route->source) {
        route->response = message;
        route->responder = sender;
        route->bits |= JS_BIT(JS_RESPONSE);
    }
}
static uint64_t js_data_send(void *self, void *msg, uint32_t peer, void *error) {
    if (azfile_native_transport) azfile_native_transport(AZN_FILE_SEND, (uintptr_t) msg, peer);
    struct js_packet packet;
    if (!peer && js_packet(msg, &packet) && js_lock(js_route_mask(packet.seq), 0)) {
        if (js_query_kind(packet.kind)) js_query_sent(&packet, (uintptr_t) msg, (uintptr_t) self);
        else js_response_sent(&packet, (uintptr_t) msg, (uintptr_t) self);
        pthread_mutex_unlock(&js.mu);
    }
    return ((uint64_t (*)(void *, void *, uint32_t, void *)) js.original[0])(self, msg, peer, error);
}
static void js_wire_record(const struct js_packet *packet, uintptr_t message, uintptr_t wire) {
    struct js_route *route = js_route(packet->seq);
    if (route && route->bits == 1 && route->sent == message && route->query.kind == packet->kind) {
        route->wire = wire;
        route->bits |= JS_BIT(JS_WIRE);
    } else if (route && route->bits == 31 && route->response == message && js_reply(route, packet->kind)) {
        route->reply_wire = wire;
        route->bits |= JS_BIT(JS_RESPONSE_WIRE);
    }
}
static int js_wire_send(void *self, void *msg) {
    if (azfile_native_transport) azfile_native_transport(AZN_FILE_WIRE, (uintptr_t) msg, (unsigned) js_word(self, 8, 4));
    struct js_packet p;
    int watched = !js_word(self, 8, 4) && js_packet(msg, &p);
    if (watched && js_lock(js_route_mask(p.seq), 0)) {
        js_wire_record(&p, (uintptr_t) msg, (uintptr_t) self);
        pthread_mutex_unlock(&js.mu);
    }
    int rc = ((int (*)(void *, void *)) js.original[1])(self, msg);
    if (rc < 0 && watched) atomic_fetch_or(&js.lost_routes, js_route_mask(p.seq));
    return rc;
}
static int js_query_ingress(const struct js_route *route, const struct js_packet *packet) {
    return route->bits == 3 && route->query.kind == packet->kind &&
           route->query.field0 == packet->field0 && route->query.field1 == packet->field1;
}
static int js_response_ingress(const struct js_route *route, const struct js_packet *packet) {
    return route->bits == 63 && js_reply(route, packet->kind);
}
static void js_ingress_record(int client, const struct js_packet *packet, uintptr_t message) {
    struct js_route *route = js_route(packet->seq);
    if (!route) return;
    if (!client && js_query_ingress(route, packet)) {
        route->received = message;
        route->bits |= JS_BIT(JS_INGRESS);
    } else if (client && js_response_ingress(route, packet)) {
        route->client_packet = message;
        route->bits |= JS_BIT(JS_CLIENT);
    } else if (route->query.kind == packet->kind || js_reply(route, packet->kind)) route->dead = 1;
}
static uint64_t js_ingress(int client, void *self, void *msg, uint32_t peer) {
    if (azfile_native_transport) azfile_native_transport(client ? AZN_FILE_CLIENT : AZN_FILE_SERVER, (uintptr_t) msg, peer);
    struct js_packet p;
    int watched = !peer && js_packet(msg, &p) &&
                  (client ? js_response_kind(p.kind) : js_query_kind(p.kind));
    if (watched && js_lock(js_route_mask(p.seq), 0)) {
        js_ingress_record(client, &p, (uintptr_t) msg);
        pthread_mutex_unlock(&js.mu);
    }
    uint64_t accepted = ((uint64_t (*)(void *, void *, uint32_t)) js.original[client ? 3 : 2])(self, msg, peer);
    /* The native receiver decides whether another listener should see the
     * same message. Only subsequent processing of this exact packet in the
     * library or ticket admits the next stage; unrelated rejections are quiet. */
    return accepted;
}
static uint64_t js_server_in(void *self, void *msg, uint32_t peer) { return js_ingress(0, self, msg, peer); }
static uint64_t js_client_in(void *self, void *msg, uint32_t peer) { return js_ingress(1, self, msg, peer); }
static void js_server_record(const struct js_packet *packet, uintptr_t message, uint64_t owner) {
    struct js_route *route = js_route(packet->seq);
    if (route && route->bits == 7 && route->received == message && route->query.kind == packet->kind) {
        route->source = owner;
        route->bits |= JS_BIT(JS_SERVER);
        js_serving.owner = owner;
        js_serving.seq = packet->seq;
    }
}
void azjs_server(uintptr_t msg, uint32_t peer, uint64_t owner, int entering) {
    if (!entering) {
        if (js_serving.depth) --js_serving.depth;
        return;
    }
    if (++js_serving.depth != 1) return;
    js_serving.seq = 0;
    js_serving.owner = 0;
    struct js_packet p;
    if (peer || !owner || !js_packet((void *) msg, &p) || !js_lock(js_route_mask(p.seq), 0)) return;
    js_server_record(&p, msg, owner);
    pthread_mutex_unlock(&js.mu);
}
static int js_ticket_packet(const struct js_route *route, const struct js_packet *packet, uintptr_t message, const void *ticket) {
    return route && route->bits == 127 && route->client_packet == message && js_reply(route, packet->kind) &&
           js_word(ticket, 0x20, 4) == packet->seq && js_word(ticket, 0x80, 1) == 2;
}
static void js_ticket_begin(struct js_ticket *seen, uintptr_t ticket, const struct js_route *route, uint32_t seq) {
    const void *object = (const void *) ticket;
    *seen = (struct js_ticket) {.ticket = ticket, .receiver = js_word(object, 0x88, 8), .control = js_word(object, 0x90, 8), .req = {js_word(object, 8, 8), js_word(object, 16, 8)}, .source = route->source, .first = seq, .first_incarnation = route->incarnation, .track_id = route->query.field1};
    if (!seen->track_id || seen->track_id != js_word(object, 0xb0, 4)) seen->ticket = 0;
}
static int js_ticket_identity(const struct js_ticket *seen, uintptr_t ticket, const struct js_route *route) {
    const void *object = (const void *) ticket;
    return seen->ticket == ticket && !seen->next && seen->source == route->source &&
           seen->receiver == js_word(object, 0x88, 8) && seen->control == js_word(object, 0x90, 8) &&
           seen->req[0] == js_word(object, 8, 8) && seen->req[1] == js_word(object, 16, 8) &&
           seen->track_id == js_word(object, 0xb0, 4);
}
static int js_ticket_continuation(const struct js_ticket *seen, const struct js_route *route, uint32_t seq) {
    const struct js_route *first = js_route(seen->first);
    return first && first->incarnation == seen->first_incarnation && seen->expected_next == seq &&
           seen->next_packet == route->sent && first->sender == route->sender && first->wire == route->wire &&
           first->responder == route->responder && first->reply_wire == route->reply_wire &&
           first->query.field0 == route->query.field0;
}
static void js_ticket_record(uintptr_t ticket, const struct js_packet *packet, struct js_route *route) {
    struct js_ticket *seen = &js.tickets[(ticket >> 4) % JS_ROUTES];
    if (packet->kind == 0x4000) js_ticket_begin(seen, ticket, route, packet->seq);
    else if (js_ticket_identity(seen, ticket, route) && js_ticket_continuation(seen, route, packet->seq)) {
        seen->next = packet->seq;
        js_processing.ticket = ticket;
        js_processing.seq = packet->seq;
    }
    route->bits |= JS_BIT(JS_TICKET);
}
void azjs_ticket(uintptr_t ticket, uintptr_t packet, int entering) {
    if (!entering) {
        if (js_processing.depth) --js_processing.depth;
        return;
    }
    if (++js_processing.depth != 1) return;
    js_processing.ticket = 0;
    js_processing.seq = 0;
    struct js_packet p;
    if (!js_packet((void *) packet, &p) || !js_lock(js_route_mask(p.seq), 0)) return;
    struct js_route *r = js_route(p.seq);
    if (js_ticket_packet(r, &p, packet, (const void *) ticket)) js_ticket_record(ticket, &p, r);
    pthread_mutex_unlock(&js.mu);
}
static void js_ticket_returned(uintptr_t ticket, uintptr_t returned) {
    struct js_ticket *seen = &js.tickets[(ticket >> 4) % JS_ROUTES];
    if (seen->ticket == ticket && seen->first && !seen->next && !seen->expected_next) {
        seen->expected_next = (uint32_t) js_word((void *) returned, 8, 4);
        seen->next_packet = returned;
    }
}
void azjs_ticket_return(uintptr_t ticket, uintptr_t returned) {
    if (!ticket || !returned || js_processing.depth != 1) return;
    /* The ticket constructs arguments 1..7 and assigns its new sequence before
     * returning. The client inserts routing argument zero later, before send. */
    if (js_word((void *) returned, 0, 2) != 0x3000 || js_word((void *) returned, 4, 4) != 0x872349ae || !js_lock(js_route_mask((uint32_t) js_word((void *) returned, 8, 4)), 0)) return;
    js_ticket_returned(ticket, returned);
    pthread_mutex_unlock(&js.mu);
}
static int js_reception_matches(const struct js_ticket *ticket, uintptr_t receiver, const void *info) {
    return js_processing.depth == 1 && js_processing.ticket && ticket->ticket == js_processing.ticket &&
           ticket->receiver == receiver && ticket->next == js_processing.seq && js_word(info, 0, 1) == 1 &&
           js_word(info, 16, 4) == ticket->track_id;
}
void azjs_reception(uintptr_t receiver, uint64_t pack_id, const void *info) {
    if (!pack_id || pack_id > UINT32_MAX || !js_lock(0, js_pack_mask((uint32_t) pack_id))) return;
    struct js_pack *p = &js.packs[pack_id % JS_PACKS];
    struct js_ticket *t = &js.tickets[(js_processing.ticket >> 4) % JS_ROUTES];
    /* Replacing a scope withdraws its old association, including on an
     * unsupported/foreign ticket. Concurrent arrivals for one pack cannot
     * attribute a deferred setData to either observation. */
    int ambiguous = p->id == pack_id && p->receiver && !p->token;
    memset(p, 0, sizeof *p);
    p->id = (uint32_t) pack_id;
    if (!ambiguous && js_reception_matches(t, receiver, info)) {
        p->receiver = receiver;
        p->owner = t->source;
        memcpy(p->track, (const unsigned char *) info + 8, 12);
        t->ticket = 0;
    }
    pthread_mutex_unlock(&js.mu);
}
static int js_pack_matches(const struct js_pack *pack, uint32_t id, uintptr_t address) {
    return pack->id == id && pack->receiver && !pack->token && pack->owner &&
           js_word((void *) address, 0x78, 1) == 1 && !memcmp((const unsigned char *) address + 0x38, pack->track, 12) &&
           !memcmp((const unsigned char *) address + 0x80, pack->track, 12) && js.next_token < UINT64_MAX;
}
static void js_pack_record(uint32_t id, uintptr_t address) {
    struct js_pack *pack = &js.packs[id % JS_PACKS];
    if (js_pack_matches(pack, id, address)) {
        pack->address = address;
        pack->token = ++js.next_token;
    } else if (pack->id == id) memset(pack, 0, sizeof *pack);
}
static void js_pack_commit(const void *where, int from_set_data) {
    const struct js_profile *profile = &js_profiles[js.build == 2];
    if (!from_set_data) return;
    uintptr_t a = js_word(where, 0, 8);
    if (!a || js_word((void *) a, 0, 8) != profile->pack_vptr) return;
    uint32_t id = (uint32_t) js_word((void *) a, 0x30, 4);
    if (js_lock(0, js_pack_mask(id))) {
        /* setData has copied the validated LoadTrackInfo to this actual owned
         * PlayMusicPack. This call precedes any of advance's native listeners,
         * including inline load submission. */
        js_pack_record(id, a);
        pthread_mutex_unlock(&js.mu);
    }
}
static uint64_t js_advance(void *builder, const void *where) {
    js_pack_commit(where, (uintptr_t) __builtin_return_address(0) == js_profiles[js.build == 2].commit_return);
    return ((uint64_t (*)(void *, const void *)) js.original[4])(builder, where);
}
static uint64_t js_destroy(void *self, int slot) {
    if (js_lock(0, js_pack_mask((uint32_t) js_word(self, 0x30, 4)))) {
        for (unsigned i = 0; i < JS_PACKS; i++)
            if (js.packs[i].address == (uintptr_t) self)
                memset(&js.packs[i], 0, sizeof js.packs[i]);
        pthread_mutex_unlock(&js.mu);
    }
    return ((uint64_t (*)(void *)) js.original[slot])(self);
}
static uint64_t js_dtor(void *self) { return js_destroy(self, 5); }
static uint64_t js_delete(void *self) { return js_destroy(self, 6); }
static uint64_t js_pack_token(uintptr_t address, const unsigned char track_id[12]) {
    for (unsigned i = 0; i < JS_PACKS; i++) {
        const struct js_pack *pack = &js.packs[i];
        if (pack->address == address && pack->token && !memcmp(track_id, pack->track, 12) &&
            js_word((void *) address, 0, 8) == js_profiles[js.build == 2].pack_vptr &&
            js_word((void *) address, 0x30, 4) == pack->id) return pack->token;
    }
    return 0;
}
uint64_t azjs_capture(unsigned deck, uint64_t unit, uint64_t epoch, uintptr_t pack, const unsigned char track_id[12]) {
    if (deck >= 4 || !unit || !epoch || !pack || !track_id || !js_lock(0, 0)) return 0;
    /* The facade still owns its Wherefrom. Compare the opaque address and
     * exact native identity copied under that same ownership. */
    uint64_t result = js_pack_token(pack, track_id);
    pthread_mutex_unlock(&js.mu);
    return result;
}
int azjs_lookup(uint64_t token, struct azjs_origin *out) {
    if (!token || !out || !atomic_load(&js.ready)) return 0;
    if (pthread_mutex_trylock(&js.mu)) return -1;
    int found = 0;
    uint64_t lost = atomic_load(&js.lost_packs);
    for (unsigned i = 0; i < JS_PACKS; i++)
        if (!(lost & (UINT64_C(1) << i)) && js.packs[i].token == token) {
            out->owner = js.packs[i].owner;
            memcpy(&out->track_id, js.packs[i].track + 8, 4);
            found = 1;
            break;
        }
    pthread_mutex_unlock(&js.mu);
    return found;
}
static int js_targets_valid(const struct js_profile *profile) {
    for (unsigned i = 0; i < 7; i++)
        if (memcmp((void *) profile->target[i].entry, profile->target[i].bytes, 16) ||
            (profile->target[i].slot && js_word((void *) profile->target[i].slot, 0, 8) != profile->target[i].entry)) return 0;
    return 1;
}
static int js_install_targets(const struct js_profile *profile, azjs_installer install) {
    void *hooks[] = {js_data_send, js_wire_send, js_server_in, js_client_in, js_advance, js_dtor, js_delete};
    for (unsigned i = 0; i < 7; i++) {
        uintptr_t slot = profile->target[i].slot;
        if (!install(slot ? slot : profile->target[i].entry, profile->target[i].entry, hooks[i], &js.original[i], slot != 0) ||
            !js.original[i]) return 0;
    }
    return 1;
}
int azjs_install(int build, azjs_installer install) {
    if (!install || (build != 1 && build != 2)) return 0;
    const struct js_profile *profile = &js_profiles[build == 2];
    if (!js_targets_valid(profile)) return 0;
    js.build = (unsigned) build;
    if (!js_install_targets(profile, install)) return 0;
    atomic_store_explicit(&js.ready, 1, memory_order_release);
    return 1;
}

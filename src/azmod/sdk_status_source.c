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
static int js_packet(const void *p, struct js_packet *out) {
    if (!p || js_word(p, 4, 4) != 0x872349ae) return 0;
    out->kind = (uint32_t) js_word(p, 0, 2);
    out->seq = (uint32_t) js_word(p, 8, 4);
    out->field0 = out->field1 = 0;
    if (out->kind == 0x2102 || out->kind == 0x3000) {
        uintptr_t begin = js_word(p, 40, 8), end = js_word(p, 48, 8), cap = js_word(p, 56, 8);
        if (!begin || end < begin || cap < end || end - begin < 96 || end - begin > 4096 * 48u ||
            (end - begin) % 48 || js_word((void *) begin, 0, 4) != 1 || js_word((void *) begin, 48, 4) != 1) return 0;
        out->field0 = (uint32_t) js_word((void *) begin, 8, 4);
        out->field1 = (uint32_t) js_word((void *) begin, 56, 4);
        return 1;
    }
    return out->kind == 0x4000 || out->kind == 0x4201;
}
static struct js_route *js_route(uint32_t seq) {
    struct js_route *r = &js.routes[seq % JS_ROUTES];
    return seq && !r->dead && r->query.seq == seq ? r : NULL;
}
static int js_reply(const struct js_route *r, unsigned kind) {
    return (r->query.kind == 0x2102 && kind == 0x4000) || (r->query.kind == 0x3000 && kind == 0x4201);
}
static uint64_t js_data_send(void *self, void *msg, uint32_t peer, void *error) {
    if (azfile_native_transport) azfile_native_transport(AZN_FILE_SEND, (uintptr_t) msg, peer);
    struct js_packet p;
    if (!peer && js_packet(msg, &p) && js_lock(js_route_mask(p.seq), 0)) {
        if (p.kind == 0x2102 || p.kind == 0x3000) {
            struct js_route *r = &js.routes[p.seq % JS_ROUTES];
            if (!p.seq || r->query.seq == p.seq || js.next_route == UINT64_MAX) r->dead = 1;
            else *r = (struct js_route) {.query = p, .bits = JS_BIT(JS_SENT), .sent = (uintptr_t) msg, .sender = (uintptr_t) self, .incarnation = ++js.next_route};
        } else {
            struct js_route *r = js_route(p.seq);
            if (r && js_reply(r, p.kind) && r->bits == 15 && js_serving.depth == 1 &&
                js_serving.seq == p.seq && js_serving.owner == r->source) {
                r->response = (uintptr_t) msg;
                r->responder = (uintptr_t) self;
                r->bits |= JS_BIT(JS_RESPONSE);
            }
        }
        pthread_mutex_unlock(&js.mu);
    }
    return ((uint64_t (*)(void *, void *, uint32_t, void *)) js.original[0])(self, msg, peer, error);
}
static int js_wire_send(void *self, void *msg) {
    if (azfile_native_transport) azfile_native_transport(AZN_FILE_WIRE, (uintptr_t) msg, (unsigned) js_word(self, 8, 4));
    struct js_packet p;
    int watched = !js_word(self, 8, 4) && js_packet(msg, &p);
    if (watched && js_lock(js_route_mask(p.seq), 0)) {
        struct js_route *r = js_route(p.seq);
        if (r && r->bits == 1 && r->sent == (uintptr_t) msg && r->query.kind == p.kind) {
            r->wire = (uintptr_t) self;
            r->bits |= JS_BIT(JS_WIRE);
        } else if (r && r->bits == 31 && r->response == (uintptr_t) msg && js_reply(r, p.kind)) {
            r->reply_wire = (uintptr_t) self;
            r->bits |= JS_BIT(JS_RESPONSE_WIRE);
        }
        pthread_mutex_unlock(&js.mu);
    }
    int rc = ((int (*)(void *, void *)) js.original[1])(self, msg);
    if (rc < 0 && watched) atomic_fetch_or(&js.lost_routes, js_route_mask(p.seq));
    return rc;
}
static uint64_t js_ingress(int client, void *self, void *msg, uint32_t peer) {
    if (azfile_native_transport) azfile_native_transport(client ? AZN_FILE_CLIENT : AZN_FILE_SERVER, (uintptr_t) msg, peer);
    struct js_packet p;
    int watched = !peer && js_packet(msg, &p) &&
                  (client ? (p.kind == 0x4000 || p.kind == 0x4201) : (p.kind == 0x2102 || p.kind == 0x3000));
    if (watched && js_lock(js_route_mask(p.seq), 0)) {
        struct js_route *r = js_route(p.seq);
        if (r && !client && r->bits == 3 && r->query.kind == p.kind &&
            r->query.field0 == p.field0 && r->query.field1 == p.field1) {
            r->received = (uintptr_t) msg;
            r->bits |= JS_BIT(JS_INGRESS);
        } else if (r && client && r->bits == 63 && js_reply(r, p.kind)) {
            r->client_packet = (uintptr_t) msg;
            r->bits |= JS_BIT(JS_CLIENT);
        } else if (r && (r->query.kind == p.kind || js_reply(r, p.kind))) r->dead = 1;
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
    struct js_route *r = js_route(p.seq);
    if (r && r->bits == 7 && r->received == msg && r->query.kind == p.kind) {
        r->source = owner;
        r->bits |= JS_BIT(JS_SERVER);
        js_serving.owner = owner;
        js_serving.seq = p.seq;
    }
    pthread_mutex_unlock(&js.mu);
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
    const void *t = (const void *) ticket;
    if (r && r->bits == 127 && r->client_packet == packet && js_reply(r, p.kind) && js_word(t, 0x20, 4) == p.seq &&
        js_word(t, 0x80, 1) == 2) {
        struct js_ticket *seen = &js.tickets[(ticket >> 4) % JS_ROUTES];
        if (p.kind == 0x4000) {
            *seen = (struct js_ticket) {.ticket = ticket, .receiver = js_word(t, 0x88, 8), .control = js_word(t, 0x90, 8), .req = {js_word(t, 8, 8), js_word(t, 16, 8)}, .source = r->source, .first = p.seq, .first_incarnation = r->incarnation, .track_id = r->query.field1};
            if (!seen->track_id || seen->track_id != js_word(t, 0xb0, 4)) seen->ticket = 0;
        } else if (seen->ticket == ticket && !seen->next && seen->source == r->source &&
                   seen->receiver == js_word(t, 0x88, 8) && seen->control == js_word(t, 0x90, 8) &&
                   seen->req[0] == js_word(t, 8, 8) && seen->req[1] == js_word(t, 16, 8) && seen->track_id == js_word(t, 0xb0, 4)) {
            struct js_route *first = js_route(seen->first);
            if (first && first->incarnation == seen->first_incarnation && seen->expected_next == p.seq && seen->next_packet == r->sent && first->sender == r->sender && first->wire == r->wire &&
                first->responder == r->responder && first->reply_wire == r->reply_wire &&
                first->query.field0 == r->query.field0) {
                seen->next = p.seq;
                js_processing.ticket = ticket;
                js_processing.seq = p.seq;
            }
        }
        r->bits |= JS_BIT(JS_TICKET);
    }
    pthread_mutex_unlock(&js.mu);
}
void azjs_ticket_return(uintptr_t ticket, uintptr_t returned) {
    if (!ticket || !returned || js_processing.depth != 1) return;
    /* The ticket constructs arguments 1..7 and assigns its new sequence before
     * returning. The client inserts routing argument zero later, before send. */
    if (js_word((void *) returned, 0, 2) != 0x3000 || js_word((void *) returned, 4, 4) != 0x872349ae || !js_lock(js_route_mask((uint32_t) js_word((void *) returned, 8, 4)), 0)) return;
    struct js_ticket *t = &js.tickets[(ticket >> 4) % JS_ROUTES];
    if (t->ticket == ticket && t->first && !t->next && !t->expected_next) {
        t->expected_next = (uint32_t) js_word((void *) returned, 8, 4);
        t->next_packet = returned;
    }
    pthread_mutex_unlock(&js.mu);
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
    if (!ambiguous && js_processing.depth == 1 && js_processing.ticket && t->ticket == js_processing.ticket &&
        t->receiver == receiver && t->next == js_processing.seq && js_word(info, 0, 1) == 1 &&
        js_word(info, 16, 4) == t->track_id) {
        p->receiver = receiver;
        p->owner = t->source;
        memcpy(p->track, (const unsigned char *) info + 8, 12);
        t->ticket = 0;
    }
    pthread_mutex_unlock(&js.mu);
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
        {
            struct js_pack *p = &js.packs[id % JS_PACKS];
            if (p->id == id && p->receiver && !p->token && p->owner &&
                js_word((void *) a, 0x78, 1) == 1 && !memcmp((const unsigned char *) a + 0x38, p->track, 12) &&
                !memcmp((const unsigned char *) a + 0x80, p->track, 12) && js.next_token < UINT64_MAX) {
                p->address = a;
                p->token = ++js.next_token;
            } else if (p->id == id) memset(p, 0, sizeof *p);
        }
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
uint64_t azjs_capture(unsigned deck, uint64_t unit, uint64_t epoch, uintptr_t pack, const unsigned char track_id[12]) {
    if (deck >= 4 || !unit || !epoch || !pack || !track_id || !js_lock(0, 0)) return 0;
    uint64_t result = 0;
    /* The facade still owns its Wherefrom. We compare the opaque address and
     * the exact native identity copied under that same ownership. */
    for (unsigned i = 0; i < JS_PACKS; i++) {
        const struct js_pack *p = &js.packs[i];
        if (p->address == pack && p->token && !memcmp(track_id, p->track, 12) &&
            js_word((void *) pack, 0, 8) == js_profiles[js.build == 2].pack_vptr &&
            js_word((void *) pack, 0x30, 4) == p->id) {
            result = p->token;
            break;
        }
    }
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
int azjs_install(int build, azjs_installer install) {
    if (!install || (build != 1 && build != 2)) return 0;
    const struct js_profile *p = &js_profiles[build == 2];
    void *hooks[] = {js_data_send, js_wire_send, js_server_in, js_client_in, js_advance, js_dtor, js_delete};
    for (unsigned i = 0; i < 7; i++) {
        if (memcmp((void *) p->target[i].entry, p->target[i].bytes, 16) ||
            (p->target[i].slot && js_word((void *) p->target[i].slot, 0, 8) != p->target[i].entry)) return 0;
    }
    js.build = (unsigned) build;
    for (unsigned i = 0; i < 7; i++) {
        uintptr_t slot = p->target[i].slot;
        if (!install(slot ? slot : p->target[i].entry, p->target[i].entry, hooks[i], &js.original[i], slot != 0) ||
            !js.original[i]) return 0;
    }
    atomic_store_explicit(&js.ready, 1, memory_order_release);
    return 1;
}

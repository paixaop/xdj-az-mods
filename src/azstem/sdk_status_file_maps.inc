/* SPDX-License-Identifier: GPL-3.0-only */
/* Original physical FileData -> local reply -> native typed Content. The grid
 * ticket retains its first reply while requesting PQT2; that continuation is
 * identified by the actual new query sequence, never by a reusable address. */
struct FileMapProfile {
    uintptr_t method[10], content[2];
};
static const FileMapProfile file_map_profiles[] = {
    {{0x181b460, 0x181ff80, 0x1803378, 0x1809398, 0x1802418,
      0x17b12b8, 0x17bfab0, 0x17af930, 0x17b0380, 0x17bea80},
     {0x2798328, 0x2797d30}},
    {{0x1976678, 0x197b198, 0x195efc0, 0x1964dd8, 0x195ead0,
      0x190c6d8, 0x191aed0, 0x190ad58, 0x190b7a8, 0x1919ea0},
     {0x2ae4df8, 0x2ae4800}}};
static const FileMapProfile *file_map_profile;
static void *file_map_original[10];
static _Atomic(const struct azn_file_map_callbacks *) file_map_callbacks = NULL;
static void file_maps_hold(const uint64_t *tokens, size_t n, int acquire) {
    __auto_type cb = atomic_load_explicit(&(file_map_callbacks), memory_order_acquire);
    if (cb) cb->hold(tokens, n, acquire);
}
struct MapBytes {
    uint32_t size;
    char digest[65];
};
static bool map_bytes(uintptr_t data, size_t size, MapBytes *out) {
    if (size > 0x500000 || (size && (data < 0x10000 || data > UINT64_C(0x0000ffffffffffff)))) return false;
    out->size = (uint32_t) (size);
    azsha256_hex(((const void *) (data)), size, out->digest);
    return true;
}
static bool map_block(uintptr_t block, MapBytes *out) {
    return ptr(block) && map_bytes(object_word(block), object_word(block + 8), out);
}
static bool map_same(const MapBytes *a, const MapBytes *b) {
    return a->size == b->size && !memcmp(a->digest, b->digest, sizeof a->digest);
}
struct MapServe {
    uintptr_t server, request, identity, accessor;
    uint32_t seq;
    unsigned kind, getters, atoms;
    bool observed, bad;
    MapBytes wire;
    struct azn_file_tag tag;
    uint64_t tokens[3];
};
static void map_serve_drop(MapServe *self) {
    file_maps_hold(self->tokens, 3, 0);
}

static _Thread_local MapServe *map_serving;
struct MapRoute {
    uint32_t seq;
    unsigned kind, phase;
    uintptr_t query, request, response, delivered;
    MapBytes wire;
    uint64_t tokens[3];
};
static atomic_flag map_guard_busy = ATOMIC_FLAG_INIT;
static _Atomic(uint64_t) map_guard_lost;
struct MapGuard {
    bool held;
};
static MapGuard map_guard_acquire(uint32_t seq) {
    bool held = !atomic_flag_test_and_set_explicit(&map_guard_busy, memory_order_acquire);
    if (!held) atomic_fetch_or_explicit(&map_guard_lost, UINT64_C(1) << (seq % 32), memory_order_release);
    return (MapGuard) {held};
}
static void map_guard_drop(MapGuard *guard) {
    if (guard->held) atomic_flag_clear_explicit(&map_guard_busy, memory_order_release);
}

static MapRoute map_routes[32];
static uint32_t map_highest;
static bool map_wrapped;
static void map_retire(MapRoute *r) {
    file_maps_hold(r->tokens, 3, 0);
    (*r) = (MapRoute) {0};
}
static void map_prune() {
    uint64_t lost = atomic_exchange_explicit(&(map_guard_lost), 0, memory_order_acq_rel);
    while (lost) {
        map_retire(&(map_routes[(unsigned) (__builtin_ctzll(lost))]));
        lost &= lost - 1;
    }
}
static unsigned map_response(unsigned query) { return query == 0x2204 ? 0x4602 : query == 0x2c04 ? 0x4f02
                                                                                                 : 0; }
static void map_route_sent(MapRoute *r, uint32_t seq, unsigned kind, uintptr_t packet) {
    if (map_highest > seq && map_highest - seq > INT32_MAX) map_wrapped = true;
    if (r->seq == seq || map_wrapped || seq <= map_highest) {
        map_retire(r);
        return;
    }
    map_highest = seq;
    map_retire(r);
    r->seq = seq;
    r->kind = kind;
    r->phase = FileSent;
    r->query = packet;
}
static bool map_route_request(MapRoute *r, unsigned stage, uintptr_t packet) {
    if (stage == AZN_FILE_WIRE && r->phase == FileSent && r->query == packet) {
        r->phase = FileWire;
        return true;
    }
    if (stage == AZN_FILE_SERVER && r->phase == FileWire) {
        r->request = packet;
        r->phase = FileReceived;
        return true;
    }
    return false;
}
static bool map_route_client(MapRoute *r, uintptr_t packet) {
    if (r->phase == FileResponseWire) {
        r->delivered = packet;
        r->phase = FileDelivered;
        return true;
    }
    return r->phase == FileDelivered && r->delivered == packet;
}
static bool map_route_response(MapRoute *r, unsigned stage, uintptr_t packet) {
    switch (stage) {
    case AZN_FILE_SEND:
        if (r->phase != FileServed || r->response != packet) return false;
        r->phase = FileResponse;
        return true;
    case AZN_FILE_WIRE:
        if (r->phase != FileResponse || r->response != packet) return false;
        r->phase = FileResponseWire;
        return true;
    case AZN_FILE_CLIENT: return map_route_client(r, packet);
    default: return false;
    }
}
static bool map_route_unexpected(const MapRoute *r, unsigned stage, unsigned kind) {
    return (kind == r->kind && stage != AZN_FILE_CLIENT) || (kind == map_response(r->kind) && stage != AZN_FILE_SERVER);
}
static void map_route_continue(MapRoute *r, uint32_t seq, unsigned stage, unsigned kind, uintptr_t packet) {
    if (r->seq != seq || !r->phase) return;
    if (kind == r->kind && map_route_request(r, stage, packet)) return;
    if (kind == map_response(r->kind) && map_route_response(r, stage, packet)) return;
    if (map_route_unexpected(r, stage, kind)) map_retire(r);
}
static bool map_transport_kind(unsigned kind) { return map_response(kind) || kind == 0x4602 || kind == 0x4f02; }
static void file_map_transport(unsigned stage, uintptr_t packet, unsigned peer) {
    if (peer || !atomic_load_explicit(&(file_map_callbacks), memory_order_acquire)) return;
    unsigned kind = message_kind(packet);
    if (!map_transport_kind(kind)) return;
    uint32_t seq = SCALAR(uint32_t, packet + 8);
    if (!seq) return;
    MapGuard guard __attribute__((cleanup(map_guard_drop))) = map_guard_acquire(seq);
    if (!guard.held) return;
    map_prune();
    MapRoute *r = &map_routes[seq % 32];
    if (map_response(kind) && stage == AZN_FILE_SEND) map_route_sent(r, seq, kind, packet);
    else map_route_continue(r, seq, stage, kind, packet);
}
static void map_serve_begin(MapServe *s, uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity, unsigned kind) {
    if (peer || !atomic_load_explicit(&(file_map_callbacks), memory_order_acquire) || !ptr(request_ref)) return;
    uintptr_t request = object_word(request_ref);
    if (message_kind(request) != kind) return;
    uint32_t seq = SCALAR(uint32_t, request + 8);
    MapGuard guard __attribute__((cleanup(map_guard_drop))) = map_guard_acquire(seq);
    if (!guard.held) return;
    map_prune();
    MapRoute *r = &map_routes[seq % 32];
    if (r->seq != seq || r->phase != FileReceived || r->request != request) return;
    s->server = server;
    s->request = request;
    s->identity = identity;
    s->seq = seq;
    s->kind = r->kind;
}
static void file_map_serve_0(uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity) {
    MapServe s __attribute__((cleanup(map_serve_drop))) = {0};
    map_serve_begin(&s, server, request_ref, peer, identity, 0x2204);
    POINTER_SCOPE(scope, map_serving, s.seq ? &s : NULL);
    ((void (*)(uintptr_t, uintptr_t, unsigned, uintptr_t)) file_map_original[0])(server, request_ref, peer, identity);
}
static void file_map_serve_1(uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity) {
    MapServe s __attribute__((cleanup(map_serve_drop))) = {0};
    map_serve_begin(&s, server, request_ref, peer, identity, 0x2c04);
    POINTER_SCOPE(scope, map_serving, s.seq ? &s : NULL);
    ((void (*)(uintptr_t, uintptr_t, unsigned, uintptr_t)) file_map_original[1])(server, request_ref, peer, identity);
}
static bool map_reply_matches(const MapServe *s, uintptr_t server, uintptr_t packet, unsigned peer) {
    return !peer && s && s->observed && !s->bad && s->server == server && message_kind(packet) == map_response(s->kind) &&
           SCALAR(uint32_t, packet + 8) == s->seq;
}
static void file_map_reply(uintptr_t server, uintptr_t packet, unsigned peer) {
    __auto_type s = map_serving;
    if (!map_reply_matches(s, server, packet, peer)) return;
    MapGuard guard __attribute__((cleanup(map_guard_drop))) = map_guard_acquire(s->seq);
    if (!guard.held) return;
    map_prune();
    __auto_type r = &(map_routes[s->seq % 32]);
    if (r->seq != s->seq || r->phase != FileReceived || r->request != s->request) return;
    r->phase = FileServed;
    r->response = packet;
    r->wire = s->wire;
    memcpy(r->tokens, s->tokens, sizeof r->tokens);
    file_maps_hold(r->tokens, 3, 1);
}
static bool map_pqtz_bytes(const struct azn_file_tag *tag, uintptr_t atom, uintptr_t bytes) {
    return tag->header >= 12 && tag->total >= tag->header && tag->total > 14 && tag->total <= 0x500000 &&
           object_word(atom + 0x28) == tag->total && ptr(bytes);
}
static void map_pqtz_atom(MapServe *s, uintptr_t output) {
    ++s->atoms;
    uintptr_t atom = object_word(output);
    if (s->atoms != 1 || !ptr(atom)) {
        s->bad = true;
        return;
    }
    struct azn_file_tag *tag = &s->tag;
    tag->name = AZN_FILE_PQTZ;
    tag->header = SCALAR(uint32_t, atom + 0x18);
    tag->total = SCALAR(uint32_t, atom + 0x1c);
    tag->first = 14;
    uintptr_t bytes = object_word(atom + 0x30);
    if (!map_pqtz_bytes(tag, atom, bytes)) s->bad = true;
    else azsha256_hex((const void *) bytes, tag->total - 14, tag->digest);
}
static int file_map_pqtz(uintptr_t accessor, uintptr_t source, uintptr_t output) {
    int rc = ((int (*)(uintptr_t, uintptr_t, uintptr_t)) file_map_original[4])(accessor, source, output);
    MapServe *s = map_serving;
    if (!rc && s && s->kind == 0x2204 && s->accessor == accessor && output) map_pqtz_atom(s, output);
    return rc;
}
static uint32_t map_be32(uintptr_t p) { return __builtin_bswap32(SCALAR(uint32_t, p)); }
static bool map_encoded_tag(const struct azn_file_tag *tag, size_t n) {
    return (tag->name == AZN_FILE_PQT2 || tag->name == AZN_FILE_PSSI) && tag->total == n - 4 && tag->header >= 12 && tag->header <= tag->total;
}
static bool map_encoded(uintptr_t block, struct azn_file_tag *tag) {
    if (!ptr(block)) return false;
    uintptr_t data = object_word(block);
    size_t n = object_word(block + 8);
    if (!ptr(data) || n < 16 || n > 0x500000 || SCALAR(uint32_t, data) != n - 4) return false;
    tag->name = map_be32(data + 4);
    tag->header = map_be32(data + 8);
    tag->total = map_be32(data + 12);
    tag->first = 0;
    if (!map_encoded_tag(tag, n)) return false;
    azsha256_hex(((const void *) (data + 4)), tag->total, tag->digest);
    return true;
}
static bool map_get_tag_valid(const MapServe *s, uintptr_t block, bool specified, struct azn_file_tag *tag) {
    return !s->bad && (specified || s->atoms == 1) && (!specified || map_encoded(block, tag));
}
static void map_complete_get(MapServe *s, FileInputUse *inputs) {
    uint64_t read = file_inputs_join(inputs);
    if (!read) {
        s->bad = true;
        return;
    }
    const struct azn_file_map_callbacks *cb = atomic_load_explicit(&(file_map_callbacks), memory_order_acquire);
    uint64_t tokens[3] = {0};
    cb->complete(read, &s->tag, 1, tokens);
    unsigned index = s->tag.name == AZN_FILE_PQTZ ? 0 : s->tag.name == AZN_FILE_PQT2 ? 1
                                                                                     : 2;
    s->tokens[index] = tokens[0];
    file_maps_hold(s->tokens, 3, 1);
    if (!tokens[0]) s->bad = true;
}
static void map_finish_get(MapServe *s, FileInputUse *inputs, int rc, uintptr_t output, bool specified) {
    uintptr_t block = output ? object_word(output) : 0;
    if (block && !map_block(block, &(s->wire))) {
        s->bad = true;
        return;
    }
    if (!block && !map_bytes(0, 0, &(s->wire))) return;
    s->observed = true;
    if (rc || !s->wire.size) return;
    if (!map_get_tag_valid(s, block, specified, &s->tag)) {
        s->bad = true;
        return;
    }
    map_complete_get(s, inputs);
}
static int file_map_get_grid(uintptr_t accessor, uintptr_t identity, uintptr_t output) {
    __auto_type s = map_serving;
    bool active = s && s->kind == 0x2204 && s->identity == identity && ++s->getters == 1;
    FileInputUse inputs __attribute__((cleanup(file_inputs_drop))) = {.api = file_inputs_api()};
    POINTER_SCOPE(input_scope, file_map_inputs, active ? &inputs : NULL);
    if (active) s->accessor = accessor;
    int rc = ((int (*)(uintptr_t, uintptr_t, uintptr_t))(file_map_original[2]))(accessor, identity, output);
    if (active) map_finish_get(s, &(inputs), rc, output, false);
    else if (s) s->bad = true;
    return rc;
}
static int file_map_get_specified(uintptr_t accessor, uintptr_t identity, uintptr_t tags, uintptr_t extension,
                                  uintptr_t output, uintptr_t count) {
    __auto_type s = map_serving;
    bool active = s && s->kind == 0x2c04 && s->identity == identity && ++s->getters == 1;
    FileInputUse inputs __attribute__((cleanup(file_inputs_drop))) = {.api = file_inputs_api()};
    POINTER_SCOPE(input_scope, file_map_inputs, active ? &inputs : NULL);
    if (active) s->accessor = accessor;
    int rc = ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(file_map_original[3]))(
        accessor, identity, tags, extension, output, count);
    if (active) map_finish_get(s, &(inputs), rc, output, true);
    else if (s) s->bad = true;
    return rc;
}
struct MapImport {
    uintptr_t ticket;
    uint32_t seq;
    unsigned kind;
    uint64_t tokens[3];
    MapBytes wire, base, output;
    bool augmented, bad;
};
static void map_import_drop(MapImport *self) {
    file_maps_hold(self->tokens, 3, 0);
}

static _Thread_local MapImport *map_importing;
struct MapPending {
    uintptr_t ticket, query;
    uint32_t seq;
    uint64_t token;
    MapBytes base;
};
static MapPending map_pending[16];
static unsigned map_pending_next;
static atomic_flag map_pending_busy = ATOMIC_FLAG_INIT;
static bool map_pending_ticket(uintptr_t ticket, uint32_t seq, const MapImport *f) {
    return seq > f->seq && SCALAR(uint32_t, ticket + 0x20) == seq && SCALAR(uint16_t, ticket + 0x1a) == 0x4f02 &&
           SCALAR(uint16_t, ticket + 0x1c) == 0x2c04;
}
static void map_pending_put(uintptr_t ticket, uintptr_t query, const MapImport *f) {
    if (!f->tokens[0] || message_kind(query) != 0x2c04) return;
    uint32_t seq = SCALAR(uint32_t, query + 8);
    if (!map_pending_ticket(ticket, seq, f)) return;
    MapBytes base = {0};
    if (!map_block(ticket + 0x80, &(base)) || !map_same(&(base), &(f->wire))) return;
    if (atomic_flag_test_and_set_explicit(&(map_pending_busy), memory_order_acquire)) return;
    __auto_type slot = &(map_pending[map_pending_next++ % 16]);
    file_maps_hold(&slot->token, 1, 0);
    (*slot) = (MapPending) {0};
    slot->ticket = ticket;
    slot->query = query;
    slot->seq = seq;
    slot->token = f->tokens[0];
    slot->base = base;
    file_maps_hold(&slot->token, 1, 1);
    atomic_flag_clear_explicit(&(map_pending_busy), memory_order_release);
}
static void map_pending_take(MapImport *f, uintptr_t query) {
    if (atomic_flag_test_and_set_explicit(&(map_pending_busy), memory_order_acquire)) return;
    for (unsigned p_index = 0; p_index < sizeof(map_pending) / sizeof(map_pending)[0]; ++p_index) {
        MapPending *p = &(map_pending[p_index]);
        if (!p->token || p->seq != f->seq) continue;
        if (p->ticket == f->ticket && p->query == query) {
            f->tokens[0] = p->token;
            f->base = p->base;
            p->token = 0;
        }
        file_maps_hold(&p->token, 1, 0);
        (*p) = (MapPending) {0};
    }
    atomic_flag_clear_explicit(&(map_pending_busy), memory_order_release);
}
struct MapContentOrigin {
    uint64_t pin, tokens[3];
    unsigned kind;
};
static MapContentOrigin map_contents[32];
static unsigned map_content_next;
static atomic_flag map_content_busy = ATOMIC_FLAG_INIT;
static void map_content_put(uintptr_t content, unsigned kind, const uint64_t tokens[3]) {
    if (!typed(content, file_bias + file_map_profile->content[kind])) return;
    uint64_t pin = azn_owned_pin(content);
    if (!pin) return;
    if (atomic_flag_test_and_set_explicit(&(map_content_busy), memory_order_acquire)) {
        azn_pin_drop(pin);
        return;
    }
    __auto_type slot = &(map_contents[map_content_next++ % 32]);
    MapContentOrigin old = (*slot);
    (*slot) = (MapContentOrigin) {0};
    slot->pin = pin;
    slot->kind = kind;
    if (kind) slot->tokens[2] = tokens[2];
    else {
        slot->tokens[0] = tokens[0];
        slot->tokens[1] = tokens[1];
    }
    file_maps_hold(slot->tokens, 3, 1);
    atomic_flag_clear_explicit(&(map_content_busy), memory_order_release);
    file_maps_hold(old.tokens, 3, 0);
    azn_pin_drop(old.pin);
}
static void map_content_origins(const MapContentOrigin *p, uintptr_t grid, uintptr_t phrase, uint64_t tokens[3]) {
    if (!p->pin) return;
    HOLD(ref, p->pin);
    if (!ref.token) return;
    uintptr_t object = pin_for(ref.token)->object;
    if (!p->kind && object == grid) {
        tokens[0] = p->tokens[0];
        tokens[1] = p->tokens[1];
    }
    if (p->kind && object == phrase) tokens[2] = p->tokens[2];
}
static bool map_origin_packet(const struct aznm_packet *packet) {
    return packet && packet->owner && packet->grid.content && packet->phrases.content;
}
static void map_scan_origins(uintptr_t grid, uintptr_t phrase, uint64_t tokens[3]) {
    for (unsigned i = 0; i < sizeof map_contents / sizeof *map_contents; ++i)
        map_content_origins(&map_contents[i], grid, phrase, tokens);
}
static int file_maps_origins(const struct aznm_packet *packet, uint64_t tokens[3]) {
    if (!tokens) return 0;
    memset(tokens, 0, 3 * sizeof *tokens);
    if (!map_origin_packet(packet) ||
        atomic_flag_test_and_set_explicit(&(map_content_busy), memory_order_acquire)) return 0;
    HOLD(grid, packet->grid.content);
    HOLD(phrase, packet->phrases.content);
    if (grid.token && phrase.token) map_scan_origins(pin_for(grid.token)->object, pin_for(phrase.token)->object, tokens);
    bool valid = tokens[0] && tokens[2];
    if (valid) file_maps_hold(tokens, 3, 1);
    else memset(tokens, 0, 3 * sizeof *tokens);
    atomic_flag_clear_explicit(&(map_content_busy), memory_order_release);
    return valid;
}
static bool map_ticket_response(unsigned response, bool phrase) {
    return phrase ? response == 0x4f02 : response == 0x4602 || response == 0x4f02;
}
static bool map_ticket_eligible(uintptr_t ticket, uint32_t seq, unsigned response, bool phrase) {
    return atomic_load_explicit(&(file_map_callbacks), memory_order_acquire) && seq && SCALAR(uint32_t, ticket + 0x20) == seq &&
           SCALAR(uint16_t, ticket + 0x1a) == response && map_ticket_response(response, phrase);
}
static uintptr_t map_ticket_take(MapImport *f, uintptr_t ticket, uintptr_t packet, uint32_t seq, unsigned response) {
    MapGuard guard __attribute__((cleanup(map_guard_drop))) = map_guard_acquire(seq);
    if (!guard.held) return 0;
    map_prune();
    MapRoute *r = &map_routes[seq % 32];
    if (r->seq != seq || r->phase != FileDelivered || r->delivered != packet || SCALAR(uint16_t, ticket + 0x1c) != r->kind) return 0;
    f->ticket = ticket;
    f->seq = seq;
    f->kind = response;
    f->wire = r->wire;
    uintptr_t query = r->query;
    memcpy(f->tokens, r->tokens, sizeof f->tokens);
    memset(r->tokens, 0, sizeof r->tokens);
    map_retire(r);
    return query;
}
static uintptr_t map_ticket_begin(MapImport *f, uintptr_t ticket, bool phrase) {
    uintptr_t packet = object_word(ticket + 0x28);
    unsigned response = message_kind(packet);
    uint32_t seq = packet && response ? SCALAR(uint32_t, packet + 8) : 0;
    return map_ticket_eligible(ticket, seq, response, phrase) ? map_ticket_take(f, ticket, packet, seq, response) : 0;
}
static uintptr_t file_map_ticket_0(uintptr_t ticket) {
    MapImport f __attribute__((cleanup(map_import_drop))) = {0};
    uintptr_t query = map_ticket_begin(&f, ticket, false);
    if (f.kind == 0x4f02) map_pending_take(&f, query);
    POINTER_SCOPE(scope, map_importing, f.ticket ? &f : NULL);
    uintptr_t result = ((uintptr_t (*)(uintptr_t)) file_map_original[5])(ticket);
    if (f.kind == 0x4602 && !f.bad) map_pending_put(ticket, result, &f);
    return result;
}
static uintptr_t file_map_ticket_1(uintptr_t ticket) {
    MapImport f __attribute__((cleanup(map_import_drop))) = {0};
    map_ticket_begin(&f, ticket, true);
    POINTER_SCOPE(scope, map_importing, f.ticket ? &f : NULL);
    return ((uintptr_t (*)(uintptr_t)) file_map_original[6])(ticket);
}
static bool map_augment_eligible(const MapImport *f, uintptr_t ticket) {
    return f && f->ticket == ticket && f->tokens[0] && !f->bad && !f->augmented;
}
static bool map_augment_extra(const MapImport *f, uintptr_t extra, unsigned extra_size) {
    if (extra_size) {
        MapBytes supplement = {0};
        return f->kind == 0x4f02 && f->tokens[1] && map_block(extra, &supplement) && map_same(&supplement, &f->wire);
    }
    return f->kind != 0x4f02 || (!f->wire.size && !f->tokens[1]);
}
static bool map_augment_input(const MapImport *f, uintptr_t base, unsigned size, uintptr_t extra, unsigned extra_size) {
    MapBytes actual = {0};
    const MapBytes *expected = f->kind == 0x4602 ? &f->wire : &f->base;
    return map_bytes(base, size, &actual) && map_same(&actual, expected) && map_augment_extra(f, extra, extra_size);
}
static int file_map_augment(uintptr_t ticket, uintptr_t base, unsigned offset, unsigned size, uintptr_t extra,
                            unsigned extra_size, uintptr_t output, uintptr_t output_size) {
    __auto_type f = map_importing;
    bool active = map_augment_eligible(f, ticket);
    if (active) {
        active = map_augment_input(f, base, size, extra, extra_size);
        if (!active) f->bad = true;
    }
    int rc = ((int (*)(uintptr_t, uintptr_t, unsigned, unsigned, uintptr_t, unsigned, uintptr_t, uintptr_t))(file_map_original[7]))(
        ticket, base, offset, size, extra, extra_size, output, output_size);
    if (active && !rc) {
        f->augmented = map_block(output, &(f->output));
        if (!f->augmented) f->bad = true;
    }
    return rc;
}
#if defined(__aarch64__)
void azn_file_grid_content_entry(), azn_file_phrase_content_entry();
void azn_invoke_map_content(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
__asm__(".text\n.p2align 2\n.global azn_file_grid_content_entry\n.hidden azn_file_grid_content_entry\n.type azn_file_grid_content_entry,%function\n"
        "azn_file_grid_content_entry:\nmov x3,x8\nmov w4,#0\nb azn_file_map_content\n"
        ".size azn_file_grid_content_entry,.-azn_file_grid_content_entry\n"
        ".global azn_file_phrase_content_entry\n.hidden azn_file_phrase_content_entry\n.type azn_file_phrase_content_entry,%function\n"
        "azn_file_phrase_content_entry:\nmov x3,x8\nmov w4,#1\nb azn_file_map_content\n"
        ".size azn_file_phrase_content_entry,.-azn_file_phrase_content_entry\n"
        ".global azn_invoke_map_content\n.hidden azn_invoke_map_content\n.type azn_invoke_map_content,%function\n"
        "azn_invoke_map_content:\nmov x5,x0\nmov x0,x1\nmov x1,x2\nmov x2,x3\nmov x8,x4\nbr x5\n"
        ".size azn_invoke_map_content,.-azn_invoke_map_content\n");
static bool map_content_matches(const MapImport *f, const MapBytes *actual, unsigned kind) {
    if (kind) return f->kind == 0x4f02 && f->tokens[2] && map_same(actual, &f->wire);
    return f->tokens[0] && f->augmented && map_same(actual, &f->output);
}
static bool map_content_eligible(const MapImport *f, uintptr_t ticket, uintptr_t block, uintptr_t status, uintptr_t output, unsigned kind) {
    MapBytes actual = {0};
    return f && !f->bad && f->ticket == ticket && status && output && map_block(block, &actual) && map_content_matches(f, &actual, kind);
}
__attribute__((visibility("hidden"))) void azn_file_map_content(
    uintptr_t ticket, uintptr_t block, uintptr_t status, uintptr_t output, unsigned kind) {
    __auto_type f = map_importing;
    bool active = map_content_eligible(f, ticket, block, status, output, kind);
    azn_invoke_map_content(file_map_original[8 + kind], ticket, block, status, output);
    if (active && !SCALAR(unsigned, status)) map_content_put(object_word(output), kind, f->tokens);
}
#endif
#if defined(__aarch64__)
static bool map_callbacks_valid(const struct azn_file_map_callbacks *cb) {
    return cb && cb->version == 1 && cb->size >= sizeof *cb && cb->complete && cb->hold && cb->inputs;
}
static bool map_install_request(int build, azn_setting_patch_fn patch, const struct azn_file_map_callbacks *cb) {
    return build >= 1 && build <= 2 && patch && map_callbacks_valid(cb) && atomic_load_explicit(&(file_callbacks), memory_order_acquire) &&
           file_profile == &file_profiles[build - 1];
}
static bool map_prologues_valid(void) {
    const uint32_t first[][2] = {{0xa9b57bfd, 0x910003fd}, {0xa9ae7bfd, 0x910003fd}, {0xa9ba7bfd, 0xaa0003e7}, {0xa9b97bfd, 0xaa0003e7}, {0xa9b67bfd, 0xd2800067}, {0xa9af7bfd, 0x910003fd}, {0xa9b57bfd, 0x910003fd}, {0xf100003f, 0x7a401864}, {0xa9b37bfd, 0x910003fd}, {0xa9b37bfd, 0x910003fd}};
    for (unsigned i = 0; i < 10; ++i)
        if (!file_map_original[i] && memcmp((void *) (file_bias + file_map_profile->method[i]), first[i], sizeof first[i])) return false;
    return true;
}
static bool map_install_hooks(azn_setting_patch_fn patch) {
    void *hooks[] = {(void *) file_map_serve_0, (void *) file_map_serve_1, (void *) file_map_get_grid, (void *) file_map_get_specified,
                     (void *) file_map_pqtz, (void *) file_map_ticket_0, (void *) file_map_ticket_1, (void *) file_map_augment,
                     (void *) azn_file_grid_content_entry, (void *) azn_file_phrase_content_entry};
    for (unsigned i = 0; i < 10; ++i)
        if (!file_map_original[i] && (patch(file_bias + file_map_profile->method[i], hooks[i], &file_map_original[i], "status-file-map") != 1 || !file_map_original[i])) return false;
    return true;
}
#endif
static int file_maps_install(int build, azn_setting_patch_fn patch, const struct azn_file_map_callbacks *cb) {
#if defined(__aarch64__)
    if (!map_install_request(build, patch, cb)) return 0;
    __auto_type installed = atomic_load_explicit(&(file_map_callbacks), memory_order_acquire);
    if (installed) return installed == cb;
    file_map_profile = &file_map_profiles[build - 1];
    if (!map_prologues_valid() || !map_install_hooks(patch)) return 0;
    atomic_store_explicit(&(file_map_callbacks), cb, memory_order_release);
    return 1;
#else
    (void) build;
    (void) patch;
    (void) cb;
    return 0;
#endif
}

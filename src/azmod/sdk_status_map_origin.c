/* SPDX-License-Identifier: GPL-3.0-only */
/* Included with the file worker. The native typed constructors establish the
 * other half of this chain. A tag token alone cannot create a map association. */
struct fm_tags {
    struct azn_file_tag value[AZN_FILE_MAP_TAGS];
    unsigned present, ambiguous;
};
static int fm_equal(const struct azn_file_tag *a, const struct azn_file_tag *b) {
    return a->name == b->name && a->header == b->header && a->total == b->total &&
           a->first == b->first && !memcmp(a->digest, b->digest, sizeof a->digest);
}
static int fm_index(uint32_t name) {
    if (name == AZN_FILE_PQTZ) return 0;
    if (name == AZN_FILE_PQT2) return 1;
    if (name == AZN_FILE_PSSI) return 2;
    return -1;
}
static int fm_claim_shape(const struct azn_file_tag *claim, uint32_t name, uint32_t header, uint32_t total) {
    return claim->name == name && claim->header == header && claim->total == total && claim->first < total &&
           (claim->first == 14 || (claim->first <= 12 && !(claim->first & 3)));
}
static void fm_match_claims(const uint8_t *tag, uint32_t name, uint32_t header, uint32_t total,
                            const struct azn_file_tag *claimed, size_t n, unsigned *matched) {
    for (size_t i = 0; i < n; i++) {
        const struct azn_file_tag *c = &claimed[i];
        if (!fm_claim_shape(c, name, header, total)) continue;
        char digest[65];
        azsha256_hex(tag + c->first, total - c->first, digest);
        if (!memcmp(digest, c->digest, sizeof digest)) matched[i]++;
    }
}
static void fm_record_tag(struct fm_tags *out, const uint8_t *tag, uint32_t name, uint32_t header, uint32_t total, int index, unsigned file) {
    unsigned bit = 1u << index;
    /* Public PQTZ is selected from DAT, PSSI from EXT. Reject
     * repeated nominal tags, including equal companion copies. */
    if ((out->present & bit) || file != (index ? 1u : 0u)) out->ambiguous |= bit;
    out->present |= bit;
    out->value[index] = (struct azn_file_tag) {.name = name, .header = header, .total = total};
    azsha256_hex(tag, total, out->value[index].digest);
}
static int fm_scan_file(const uint8_t *data, size_t size, unsigned file, struct fm_tags *out,
                        const struct azn_file_tag *claimed, size_t n, unsigned *matched) {
    if (size > ST_FILE_MAX || !fo_container(data, size)) return 0;
    for (size_t at = hc_be32(data + 4); at < size;) {
        if (size - at < 12) return 0;
        const uint8_t *tag = data + at;
        uint32_t header = hc_be32(tag + 4), total = hc_be32(tag + 8), name = hc_be32(tag);
        if (header < 12 || header > total || total > size - at) return 0;
        int index = fm_index(name);
        if (index >= 0) {
            fm_record_tag(out, tag, name, header, total, index, file);
            fm_match_claims(tag, name, header, total, claimed, n, matched);
        }
        at += total;
    }
    return 1;
}
static int fm_claims_unique(const struct fm_tags *out, const struct azn_file_tag *claimed, size_t n, const unsigned *matched) {
    for (size_t i = 0; i < n; i++) {
        int index = fm_index(claimed[i].name);
        if (index < 0 || matched[i] != 1 || (out->ambiguous & (1u << index))) return 0;
        for (size_t j = 0; j < i; j++)
            if (claimed[i].name == claimed[j].name) return 0;
    }
    return 1;
}
static int fm_scan(const uint8_t *const buf[2], const size_t sizes[2], struct fm_tags *out,
                   const struct azn_file_tag *claimed, size_t n) {
    unsigned matched[AZN_FILE_MAP_TAGS] = {0};
    for (unsigned f = 0; f < 2; f++) {
        if (!buf[f] && !sizes[f]) continue;
        if (!fm_scan_file(buf[f], sizes[f], f, out, claimed, n, matched)) return 0;
    }
    return fm_claims_unique(out, claimed, n, matched);
}
static int fm_read_files(const struct fo_read *read, uint8_t *bytes[2], size_t sizes[2]) {
    for (unsigned f = 0; f < 2; f++) {
        const struct ss_file *file = &read->files[f];
        if (read->inherited && !(read->opened & (1u << f))) continue;
        if (file->rc == AZ_UNSUPPORTED) continue;
        bytes[f] = st_read_file_checked(file->path, &sizes[f], file->device, file->inode, file->signature);
        if (!bytes[f] || !fo_input_bytes(read, f, bytes[f], sizes[f])) return 0;
    }
    return 1;
}
static int fm_claims_opened(const struct fo_read *read, const struct azn_file_tag *tags, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned file = tags[i].name == AZN_FILE_PQTZ ? 1u : 2u;
        if (!(read->opened & file)) return 0;
    }
    return 1;
}
static int fm_validate_read(struct fo_read *read, const struct azn_file_tag *tags, size_t n, struct fm_tags *physical) {
    uint8_t *bytes[2] = {0};
    size_t sizes[2] = {0};
    int valid = fm_read_files(read, bytes, sizes) && fm_claims_opened(read, tags, n);
    const uint8_t *inputs[] = {bytes[0], bytes[1]};
    valid = valid && fm_scan(inputs, sizes, physical, tags, n) && fo_alive(read) && fo_version_current(read);
    free(bytes[0]);
    free(bytes[1]);
    return valid;
}
static void fm_publish_tokens(struct fo_read *read, unsigned slot, const struct azn_file_tag *tags, size_t n,
                              const struct fm_tags *physical, uint64_t tokens[AZN_FILE_MAP_TAGS]) {
    read->kind = 2;
    read->count = (unsigned) n;
    for (unsigned i = 0; i < n; i++) {
        read->tags[i] = physical->value[fm_index(tags[i].name)];
        uint64_t serial = fo_new_id();
        tokens[i] = read->token[i] = serial ? (serial << 6) | slot : 0;
        atomic_store_explicit(&fo_custody[slot].tokens[i], tokens[i], memory_order_release);
    }
    read->complete = 1;
}
static int fm_read_eligible(struct fo_read *read, const struct azn_file_tag *tags, size_t n, const uint64_t *tokens) {
    return tags && tokens && n && n <= AZN_FILE_MAP_TAGS && fo_alive(read) && fo_version_current(read);
}
static void fm_complete(uint64_t id, const struct azn_file_tag *tags, size_t n, uint64_t tokens[AZN_FILE_MAP_TAGS]) {
    if (tokens) memset(tokens, 0, AZN_FILE_MAP_TAGS * sizeof *tokens);
    unsigned slot;
    struct fo_read *read = fo_claim_read(id, &slot);
    if (!read) return;
    struct fm_tags physical = {0};
    int valid = fm_read_eligible(read, tags, n, tokens) && fm_validate_read(read, tags, n, &physical);
    if (read->db) ss_close_database(read->db, read->db_fd);
    read->db = NULL;
    pthread_mutex_lock(&ss_mu);
    if (valid) fm_publish_tokens(read, slot, tags, n, &physical, tokens);
    else {
        fo_reads[slot] = NULL;
        atomic_store_explicit(&fo_custody[slot].state, 0, memory_order_release);
    }
    pthread_mutex_unlock(&ss_mu);
    if (!valid) fo_drop(read);
}
__attribute__((visibility("hidden"))) const struct azn_file_map_callbacks *azfile_map_callbacks(void) {
    static const struct azn_file_map_callbacks callbacks = {sizeof callbacks, 1, fo_begin, fm_complete, fo_hold, fo_opened, fi_api};
    return &callbacks;
}
struct fm_edition {
    uint64_t cut, provider[2], revision[2];
    struct fm_tags tags;
    unsigned ready, beats, phrase_end;
};
struct fm_pair {
    uint64_t epoch, tokens[AZN_FILE_MAP_TAGS], serial;
    uint64_t published[6], publication;
};
static struct fm_edition fm_editions[4];
static struct fm_pair fm_pairs[4];
static int fm_token_read_live(const struct fo_read *r, unsigned i) {
    return r && r->complete == 1 && r->kind == 2 && r->count &&
           !atomic_load_explicit(&fo_custody[i].lost, memory_order_acquire) &&
           atomic_load_explicit(&fo_custody[i].state, memory_order_acquire) >> 16 == r->id;
}
static struct fo_read *fm_token(uint64_t token, unsigned kind, unsigned *position) {
    if (!token) return NULL;
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!fm_token_read_live(r, i)) continue;
        for (unsigned j = 0; j < r->count; j++)
            if (!(r->retired_tags & (1u << j)) && r->token[j] == token && r->tags[j].name == kind) {
                *position = j;
                return r;
            }
    }
    return NULL;
}
static void fm_binding(unsigned deck, uint64_t epoch) {
    struct fm_pair *p = &fm_pairs[deck];
    if (p->epoch && p->epoch != epoch) {
        fo_hold(p->tokens, AZN_FILE_MAP_TAGS, 0);
        memset(p, 0, sizeof *p);
    }
}
static int fm_pair_sources(unsigned deck, const uint64_t tokens[AZN_FILE_MAP_TAGS]) {
    unsigned at = 0;
    struct fo_read *grid = fm_token(tokens[0], AZN_FILE_PQTZ, &at), *phrases = fm_token(tokens[2], AZN_FILE_PSSI, &at);
    struct fo_read *extra = tokens[1] ? fm_token(tokens[1], AZN_FILE_PQT2, &at) : NULL;
    if (!grid || !phrases || !st_ref_equal(&grid->source.ref, &phrases->source.ref)) return 0;
    if (ss_decks[deck].ref.source_id && !st_ref_equal(&grid->source.ref, &ss_decks[deck].ref)) return 0;
    return !tokens[1] || (extra && st_ref_equal(&grid->source.ref, &extra->source.ref));
}
static void fm_replace_pair(struct fm_pair *p, uint64_t epoch, const uint64_t tokens[AZN_FILE_MAP_TAGS]) {
    fo_hold(tokens, AZN_FILE_MAP_TAGS, 1);
    if (p->epoch) fo_hold(p->tokens, AZN_FILE_MAP_TAGS, 0);
    memset(p, 0, sizeof *p);
    p->epoch = epoch;
    p->serial = ss_new_id(&ss_serial);
    memcpy(p->tokens, tokens, sizeof p->tokens);
}
static int fm_pair_request(unsigned deck, uint64_t epoch, const uint64_t *tokens) {
    return deck < 4 && epoch && tokens && tokens[0] && tokens[2];
}
__attribute__((visibility("hidden"))) void azfile_map_pair(unsigned deck, uint64_t epoch, const uint64_t tokens[AZN_FILE_MAP_TAGS]) {
    if (!fm_pair_request(deck, epoch, tokens)) return;
    pthread_mutex_lock(&ss_mu);
    struct fm_pair *p = &fm_pairs[deck];
    if (ss_decks[deck].epoch == epoch && (p->epoch != epoch || memcmp(p->tokens, tokens, sizeof p->tokens))) {
        if (fm_pair_sources(deck, tokens)) fm_replace_pair(p, epoch, tokens);
    }
    pthread_mutex_unlock(&ss_mu);
    track_service_wake();
}
/* Called after the raw FILE collections have reached the reducer. The native
 * paired consumer is the association authority. File tags and epochs prove
 * that its original inputs are exactly these saved collection editions. */
static int fm_edition_matches(const struct ss_binding *b, const struct fm_edition *e) {
    return e->provider[0] == b->provider[1] && e->provider[1] == b->provider[2] &&
           e->revision[0] == b->revision[AZ_STATUS_GRID] && e->revision[1] == b->revision[AZ_STATUS_PHRASES] &&
           e->beats && e->phrase_end < e->beats;
}
static int fm_binding_ready(const struct ss_binding *b, const struct fm_edition *e) {
    return b->ready && !b->retired && !b->writing && e->cut == b->cut && e->ready == 3 &&
           !(b->pending & (SS_TOPIC(GRID) | SS_TOPIC(PHRASES))) && !b->pinned[SS_SAVED_GRID] && fm_edition_matches(b, e);
}
static int fm_token_matches(unsigned deck, const struct ss_binding *b, const struct fm_edition *e, uint64_t token, unsigned i) {
    static const unsigned kinds[] = {AZN_FILE_PQTZ, AZN_FILE_PQT2, AZN_FILE_PSSI};
    unsigned at = 0;
    struct fo_read *r = fm_token(token, kinds[i], &at);
    return r && st_ref_equal(&r->source.ref, &b->ref) && !memcmp(r->files, b->image.file, sizeof r->files) &&
           (e->tags.present & (1u << i)) && !(e->tags.ambiguous & (1u << i)) &&
           fm_equal(&r->tags[at], &e->tags.value[i]) && fo_alive(r) && fo_native_source(deck, b, &r->source);
}
static int fm_pair_ready(unsigned deck, const struct ss_binding *b, const struct fm_pair *p, const struct fm_edition *e) {
    if (!fm_binding_ready(b, e)) return 0;
    for (unsigned i = 0; i < AZN_FILE_MAP_TAGS; i++) {
        if (i == 1 && !p->tokens[i]) continue;
        if (!fm_token_matches(deck, b, e, p->tokens[i], i)) return 0;
    }
    return 1;
}
static void fm_publish_grid(unsigned deck, struct ss_binding *b, struct fm_pair *p, struct fm_edition *e, int ready, const uint64_t key[6]) {
    uint64_t revision = ss_new_id(&ss_serial);
    if (!revision) return;
    az_status_grid_ref_v1 grid = {b->epoch, e->provider[0], e->revision[0], AZ_PROV_FILE, AZ_STATUS_GRID_SAVED};
    struct ds_field field = {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED_GRID, {.info = {ready ? AZ_AVAIL_READY : AZ_AVAIL_STALE, AZ_PROV_DERIVED, revision, st_now()}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = b->epoch, .provider_id = b->provider[2], .count = ready ? 1u : 0u}, ready ? &grid : NULL, NULL};
    if (field.info.provider_id && ds_publish_file(deck, b->epoch, &b->ref, field.info.info.observed_ns, &field, 1, 0, 0) == AZ_OK) {
        memcpy(p->published, key, sizeof p->published);
        p->publication = revision;
    }
}
static void fm_publish(unsigned deck) {
    struct ss_binding *b = &ss_decks[deck];
    struct fm_pair *p = &fm_pairs[deck];
    struct fm_edition *e = &fm_editions[deck];
    if (!p->epoch || p->epoch != b->epoch || !(b->topics & SS_TOPIC(PHRASES)) || !ss_local(&b->ref)) return;
    int ready = fm_pair_ready(deck, b, p, e);
    uint64_t key[] = {b->cut, p->serial, (uint64_t) ready, e->provider[0], e->revision[0], e->revision[1]};
    if (!memcmp(p->published, key, sizeof key)) return;
    fm_publish_grid(deck, b, p, e, ready, key);
}

static int fm_cut_image_valid(const struct ss_image *im, uint64_t known, uint64_t gap, unsigned f, uint64_t components) {
    return !(gap & components) && im->rc == AZ_OK && im->file[f].rc == AZ_OK && known;
}
static int fm_cut_carried(const struct fo_read *r, const struct ss_binding *b, const struct ss_image *im,
                          const uint64_t known[2], const unsigned closes[2], uint64_t gap, unsigned f, uint64_t components) {
#ifdef __linux__
    if (!fm_cut_image_valid(im, known[f], gap, f, components)) return 0;
    return !(r->watch.unsafe_events & (1u << f)) && !(r->watch.open_edits & (1u << f)) &&
           closes[f] && r->watch.close_count[f] == closes[f] && !memcmp(&r->files[f], &b->image.file[f], sizeof r->files[f]);
#else
    (void) r;
    (void) b;
    (void) im;
    (void) known;
    (void) closes;
    (void) gap;
    (void) f;
    (void) components;
    return 0;
#endif
}
static void fm_cut_tags(struct fo_read *r, unsigned f, int carried, uint64_t known) {
    for (unsigned j = 0; j < r->count; j++) {
        unsigned file = r->tags[j].name == AZN_FILE_PQTZ ? 0u : 1u;
        if (file != f) continue;
        if (!carried) r->retired_tags |= 1u << j;
        else if (known & SS_TOPIC(GRID)) r->retimed |= 1u << j;
    }
}
static void fm_file_cut(struct fo_read *r, const struct ss_binding *b, const struct ss_image *im,
                        const uint64_t known[2], const unsigned closes[2], uint64_t gap) {
#ifdef __linux__
    r->watch.pending_events |= ss_drain_events(&r->watch);
#endif
    for (unsigned f = 0; f < 2; f++) {
        int changed = memcmp(&r->files[f], &im->file[f], sizeof r->files[f]) != 0;
        uint64_t components = f ? SS_TOPIC(PHRASES) : SS_TOPIC(GRID);
        if (!changed && !(gap & components)) continue;
        /* A DB commit or unavailable row creates a collection-history gap,
         * but does not establish a write to this original ANLZ input. Its
         * independent live watch and exact physical identity can close that
         * interval, including when the monitor temporarily has no file image.
         * Keep the immutable original witness; never replace it with an empty
         * failed read or rebuild it from matching current bytes. */
#ifdef __linux__
        if (fo_alive_files(r, 1u << f)) continue;
#endif
        int carried = fm_cut_carried(r, b, im, known, closes, gap, f, components);
        fm_cut_tags(r, f, carried, known[f]);
        if (carried) r->watch.uncommitted_file[f] = 0;
        r->files[f] = im->file[f];
    }
    r->watch.pending_events = 0;
    r->watch.unsafe_events = 0;
    memset(r->watch.close_count, 0, sizeof r->watch.close_count);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Observes completed AnalyzedDataAccessor cue and grid writes. The native FileIdentityInfo
 * contains separate UTF-16 audio and ANLZ paths. Its accessor's storage owner
 * provides the UTF-8 root used by the native FilesetInfo constructor. */
struct sw_ticket {
    unsigned deck;
    uint64_t epoch, cut, started;
    az_track_ref ref;
    struct ss_file file[2];
};
struct sw_batch {
    struct sw_ticket item[4];
    unsigned count, lane;
};
static int sw_name(const uint16_t *units, size_t count, const char *text) {
    if ((!units && count) || count > 1023 || !text) return 0;
    const unsigned char *p = (const unsigned char *) text;
    for (size_t i = 0; i < count; i++) {
        uint32_t c = units[i];
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= count || units[i] < 0xdc00 || units[i] > 0xdfff) return 0;
            c = 0x10000 + ((c - 0xd800) << 10) + (units[i] - 0xdc00);
        } else if (c >= 0xdc00 && c <= 0xdfff) return 0;
        if (!c) return 0;
        unsigned char bytes[4];
        unsigned n = 0;
        if (c <= 0x7f) bytes[n++] = (unsigned char) c;
        else {
            if (c < 0x800) bytes[n++] = 0xc0 | (c >> 6);
            else {
                if (c < 0x10000) bytes[n++] = 0xe0 | (c >> 12);
                else {
                    bytes[n++] = 0xf0 | (c >> 18);
                    bytes[n++] = 0x80 | ((c >> 12) & 63);
                }
                bytes[n++] = 0x80 | ((c >> 6) & 63);
            }
            bytes[n++] = 0x80 | (c & 63);
        }
        for (unsigned j = 0; j < n; j++)
            if (*p++ != bytes[j]) return 0;
    }
    return !*p;
}
static void sw_start(struct sw_batch *batch, const char *root, size_t root_n,
                     const uint16_t *audio, size_t audio_n, const uint16_t *analysis, size_t analysis_n) {
    if (!root || root_n > 1023 || !audio_n || !analysis_n) return;
    pthread_mutex_lock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) {
        struct ss_binding *b = &ss_decks[deck];
        if (!b->ready || b->retired || !b->epoch || !(b->topics & (SS_DAT | SS_EXT)) || b->image.rc) continue;
        const char *mount = b->image.src.mount;
        size_t n = strlen(mount);
        if (root_n != n + 8 || memcmp(root, mount, n) || memcmp(root + n, "/PIONEER", 8) ||
            !(b->image.metadata.valid & AZ_META_PATH) || strncmp(b->image.metadata.path, mount, n) ||
            !sw_name(audio, audio_n, b->image.metadata.path + n) ||
            !sw_name(analysis, analysis_n, b->image.file[0].relative)) continue;
        struct sw_ticket *t = &batch->item[batch->count++];
        t->deck = deck;
        t->epoch = b->epoch;
        t->cut = b->cut;
        t->ref = b->ref;
        t->started = ss_new_id(&ss_serial);
        if (b->writing) b->overlapping = 1;
        ++b->writing;
        b->serial = t->started;
        memcpy(t->file, b->image.file, sizeof t->file);
    }
    pthread_mutex_unlock(&ss_mu);
}
static void sw_release(struct sw_batch *batch) {
    if (!batch->count) return;
    pthread_mutex_lock(&ss_mu);
    for (unsigned i = 0; i < batch->count; i++) {
        const struct sw_ticket *t = &batch->item[i];
        struct ss_binding *b = &ss_decks[t->deck];
        if (b->cut == t->cut && b->epoch == t->epoch && b->writing && !--b->writing) {
            b->overlapping = 0;
            azfile_native_refresh(t->deck, t->epoch);
        }
    }
    pthread_mutex_unlock(&ss_mu);
    if (batch->count) {
        ss_signal();
        st_worker_wake();
    }
    batch->count = 0;
}
/* One native save may concern the same track on several units. Share the
 * immutable containers and storage revision, but keep each unit's individual
 * epoch, subscriber interval, native witness and provider incarnation. */
static void sw_finish(struct sw_batch *batch) {
    unsigned handled = 0;
    unsigned lane = batch->lane;
    uint64_t topics = AZ_STATUS_TOPIC(ss_saved_topic[lane]);
    unsigned nfiles = lane == SS_SAVED_GRID ? 1u : 2u;
    uint64_t observed = st_now();
    for (unsigned i = 0; i < batch->count; i++) {
        if (handled & (1u << i)) continue;
        const struct sw_ticket *t = &batch->item[i];
        unsigned members = 0;
        for (unsigned j = i; j < batch->count; j++)
            if (st_ref_equal(&t->ref, &batch->item[j].ref) &&
                !memcmp(t->file, batch->item[j].file, sizeof t->file)) members |= 1u << j;
        handled |= members;
        struct st_source src;
        if (!t->started || ss_source(&t->ref, &src)) continue;
        struct ss_saved_revision *q = calloc(1, sizeof *q);
        struct ss_write_proof proof[2] = {{0}};
        char siblings[2][65] = {{0}};
        for (unsigned f = 0; f < nfiles; f++) {
            struct ss_file file = t->file[f];
            struct stat sb;
            char real[1024];
            if (q) q->rc[f] = file.rc;
            if (!file.path[0] || !st_source_path(src.mount, file.relative, real) || strcmp(real, file.path) ||
                stat(real, &sb) || !S_ISREG(sb.st_mode) ||
                ((uint64_t) sb.st_dev == src.device && (uint64_t) sb.st_ino == src.inode)) continue;
            file.signature = st_stat_sig(&sb);
            file.device = (uint64_t) sb.st_dev;
            file.inode = (uint64_t) sb.st_ino;
            if (!ss_read_digest(&file, proof[f].digest, q ? &q->bytes[f] : NULL, q ? &q->sizes[f] : NULL)) continue;
            proof[f].device = file.device;
            proof[f].inode = file.inode;
            proof[f].signature = file.signature;
            if (q && q->bytes[f]) {
                q->rc[f] = AZ_OK;
                q->total += q->sizes[f];
                ss_sibling_digest(lane, q->bytes[f], q->sizes[f], siblings[f]);
            }
        }
        if (ss_source(&t->ref, &src)) {
            ss_release_saved(q);
            continue;
        }
        pthread_mutex_lock(&ss_mu);
        uint64_t revision = ss_new_id(&ss_serial);
        if (q) {
            q->ref = t->ref;
            q->revision = revision;
            q->observed = observed;
            q->lane = lane;
        }
        for (unsigned j = i; j < batch->count; j++) {
            if (!(members & (1u << j))) continue;
            const struct sw_ticket *target = &batch->item[j];
            unsigned deck = target->deck;
            struct ss_binding *b = &ss_decks[deck];
            if (b->cut != target->cut || b->epoch != target->epoch || b->retired || !st_ref_equal(&b->ref, &target->ref))
                continue;
            int complete = q && q->total && !b->overlapping && revision;
            for (unsigned f = 0; f < nfiles; f++) {
                if (!proof[f].signature) {
                    if (t->file[f].rc != AZ_UNSUPPORTED) complete = 0;
                    continue;
                }
                int same_file = proof[f].device == t->file[f].device && proof[f].inode == t->file[f].inode;
                if (!q || !q->bytes[f] || !same_file) complete = 0;
                if (b->writing > 1) continue;
                struct ss_write_proof done = proof[f];
                done.revision = revision;
                done.ordinal = b->completed[f].ordinal + 1;
                done.only = !b->overlapping && same_file && siblings[f][0] &&
                                    !memcmp(siblings[f], b->siblings[lane][f], sizeof siblings[f])
                                ? topics
                                : 0;
                b->completed[f] = done;
            }
            if (!(b->topics & topics)) continue;
            if (complete && ss_saved_count < SS_SAVE_QUEUE && q->total <= SS_SAVE_PIN_BYTES - ss_saved_bytes) {
                q->targets |= 1u << deck;
                q->target[deck].cut = b->cut;
                q->target[deck].epoch = b->epoch;
                q->target[deck].interest = b->interest[lane];
                q->target[deck].provider = b->provider[ss_saved_provider[lane]];
                q->target[deck].source_pin = ss_pins[deck].epoch == b->epoch ? ss_pins[deck].token : 0;
                ++b->pinned[lane];
                b->pending &= ~topics;
            } else {
                b->interest[lane] = ss_new_id(&ss_serial);
                b->pinned[lane] = 0;
                b->pending |= topics;
                ds_publish_file(deck, b->epoch, &b->ref, observed, NULL, 0, topics, AZ_STATUS_REVISION_LOST);
            }
        }
        if (q && q->targets) {
            ss_saved_queue[ss_saved_count++] = q;
            ss_saved_bytes += q->total;
            q = NULL;
        }
        pthread_mutex_unlock(&ss_mu);
        ss_release_saved(q);
    }
    sw_release(batch);
}
struct sw_target {
    uintptr_t address;
    uint32_t bytes[4];
};
static const struct sw_target sw_targets[2][4] = {
    {{0x180ddd8, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180e7a0, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180eb20, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180b2a8, {0xa9bb7bfd, 0xaa0003e7, 0xaa0103e8, 0x12003c49}}},
    {{0x1969148, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1969b10, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1969e90, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1966618, {0xa9bb7bfd, 0xaa0003e7, 0xaa0103e8, 0x12003c49}}}};
static _Atomic int sw_ready;
static void *sw_original[4];
static void sw_capture(struct sw_batch *b, uintptr_t accessor, uintptr_t identity) {
    if (!atomic_load_explicit(&sw_ready, memory_order_acquire) || !accessor || !identity) return;
    uintptr_t storage = sm_ptr(accessor, 0x18);
    if (storage)
        sw_start(b, (const char *) sm_ptr(storage, 8), sm_ptr(storage, 16),
                 (const uint16_t *) sm_ptr(identity, 0), sm_ptr(identity, 8),
                 (const uint16_t *) sm_ptr(identity, 32), sm_ptr(identity, 40));
}
static uint64_t sw_old(uintptr_t self, uintptr_t identity, uintptr_t a, uintptr_t b) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {0};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)) sw_original[0])(self, identity, a, b);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static uint64_t sw_cue(unsigned index, uintptr_t self, uintptr_t identity, uintptr_t data) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {0};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t)) sw_original[index])(self, identity, data);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static uint64_t sw_new(uintptr_t self, uintptr_t identity, uintptr_t data) { return sw_cue(1, self, identity, data); }
static uint64_t sw_delete(uintptr_t self, uintptr_t identity, uintptr_t data) { return sw_cue(2, self, identity, data); }
static uint64_t sw_offset(uintptr_t self, uintptr_t identity, unsigned offset) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {.lane = SS_SAVED_GRID};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, unsigned)) sw_original[3])(self, identity, offset);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static int sdk_status_save_install(int build, azjs_installer install) {
    void *hooks[] = {sw_old, sw_new, sw_delete, sw_offset};
    if ((build != 1 && build != 2) || !install) return 0;
    const struct sw_target *t = sw_targets[build - 1];
    for (unsigned i = 0; i < 4; i++)
        if (memcmp((void *) t[i].address, t[i].bytes, sizeof t[i].bytes)) return 0;
    for (unsigned i = 0; i < 4; i++)
        if (!install(t[i].address, t[i].address, hooks[i], &sw_original[i], 0) || !sw_original[i]) return 0;
    atomic_store_explicit(&sw_ready, 1, memory_order_release);
    return 1;
}

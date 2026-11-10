/* SPDX-License-Identifier: GPL-3.0-only */
/* Native accessors run on the storage thread. Read tokens hold copied source
 * identity, never a borrowed native object. Public arrays are decoded by st_worker. */
#include "../common/azstatus_cue_origin.h"
#include "../common/azstatus_analysis_origin.h"
__attribute__((visibility("hidden"))) void azfile_native_refresh(unsigned, uint64_t);
enum {
    FO_READS = 64,
    FO_ATOMS = AZN_FILE_CUE_MAX * 4
};
struct fo_atom {
    struct azn_file_atom stamp;
    uint32_t extended, hot, slot, file, offset;
    az_cue_entry selected; /* set by the same assignment as the public array */
};
struct fo_scan {
    struct fo_atom atoms[FO_ATOMS];
    unsigned n, failed;
};
struct fo_edition {
    uint64_t cut, provider, revision;
    struct fo_scan scan;
};
static struct fo_edition fo_editions[4];
struct fo_read {
    uint64_t id, token[AZN_FILE_CUE_MAX], version;
    unsigned count, complete, kind, retired_tags, retimed, opened, input_mask, inherited;
    char input_digest[2][65];
    struct azn_file_cue_row row[AZN_FILE_CUE_MAX];
    struct azn_file_tag tags[AZN_FILE_MAP_TAGS];
    struct st_source source;
    struct ss_file files[2];
    char audio[1024];
    struct ss_monitor watch;
    void *db;
    int db_fd;
};
static uint64_t fo_serial;
static struct fo_read *fo_reads[FO_READS];
static const struct azn_file_input_api *fi_api(void);
/* Stable atomic slots are separate from allocated reads. The audio writer
 * never follows an allocated pointer, locks a mutex or touches the filesystem. */
static struct {
    _Atomic uint64_t state, tokens[AZN_FILE_CUE_MAX];
    _Atomic int lost;
} fo_custody[FO_READS];
#define FO_COUNT_MASK UINT64_C(65535)
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "FILE custody requires lock-free 64-bit atomics");
static uint64_t fo_new_id(void) {
    return fo_serial < (UINT64_MAX >> 16) ? ++fo_serial : 0;
}
static struct {
    uint64_t cut, read;
} fo_held[4];
static int fo_in_use(uint64_t read) {
    for (unsigned deck = 0; deck < 4; deck++)
        if (fo_held[deck].read == read && fo_held[deck].cut == ss_decks[deck].cut &&
            !ss_decks[deck].retired && (ss_decks[deck].topics & SS_TOPIC(CUES))) return 1;
    return 0;
}

/* st_parse_origins already validated framing and the selected list. Examine
 * even the shadowed physical entries for duplicate native atom identities. */
static const struct fo_atom *fo_match(const struct fo_scan *, unsigned, const struct azn_file_atom *);
static void fo_scan_list(struct fo_scan *scan, const uint8_t *const buf[2], const size_t sizes[2],
                         unsigned hot, const uint8_t *const *selected, const struct st_data *d) {
    if (scan->failed) return;
    for (unsigned f = 0; f < 2; f++) {
        if (!buf[f]) continue;
        size_t size = sizes[f];
        if (size < 12 || memcmp(buf[f], "PMAI", 4) || hc_be32(buf[f] + 8) != size ||
            hc_be32(buf[f] + 4) < 12 || hc_be32(buf[f] + 4) > size) {
            scan->failed = 1;
            return;
        }
        for (size_t at = hc_be32(buf[f] + 4); at < size; at += hc_be32(buf[f] + at + 8)) {
            if (size - at < 12) {
                scan->failed = 1;
                return;
            }
            const uint8_t *tag = buf[f] + at;
            size_t pos = hc_be32(tag + 4), len = hc_be32(tag + 8);
            if (pos < 12 || pos > len || len > size - at) {
                scan->failed = 1;
                return;
            }
            int ext = !memcmp(tag, "PCO2", 4);
            if (!ext && memcmp(tag, "PCOB", 4)) continue;
            if (pos < 16 || hc_be32(tag + 12) > 1) {
                scan->failed = 1;
                return;
            }
            if (hc_be32(tag + 12) != hot) continue;
            if (pos < (ext ? 20u : 24u)) {
                scan->failed = 1;
                return;
            }
            unsigned n = hc_be16(tag + (ext ? 16 : 18));
            for (unsigned i = 0; i < n; i++) {
                const uint8_t *e = tag + pos;
                if (len - pos < 12 || memcmp(e, ext ? "PCP2" : "PCPT", 4)) {
                    scan->failed = 1;
                    return;
                }
                unsigned header = hc_be32(e + 4), total = hc_be32(e + 8);
                if (header != (ext ? 16u : 28u) || total < 40 || total > len - pos || scan->n == FO_ATOMS) {
                    scan->failed = 1;
                    return;
                }
                struct fo_atom *atom = &scan->atoms[scan->n++];
                atom->extended = ext;
                atom->hot = hot;
                atom->slot = hc_be32(e + 12);
                atom->file = f;
                atom->offset = (uint32_t) (at + pos);
                atom->stamp.header = header;
                atom->stamp.total = total;
                if (total <= AZN_FILE_CUE_BYTES) azsha256_hex(e + 14, total - 14, atom->stamp.digest);
                for (unsigned j = 0; d && j < d->view.cue_count; j++)
                    if (selected[j] == e) atom->selected = d->cues[j];
                pos += total;
            }
        }
    }
}
static int fo_alive_files(struct fo_read *r, unsigned mask) {
#ifdef __linux__
    if (r->watch.fd < 0) return 0;
    r->watch.pending_events |= ss_drain_events(&r->watch);
    if (r->watch.pending_events & SS_META) return 0;
    for (unsigned f = 0; f < 2; f++)
        if ((mask & (1u << f)) && (r->watch.wd[f] < 0 || r->watch.uncommitted_file[f] ||
                                  (r->watch.unsafe_events & (1u << f)) ||
                                  (r->watch.open_edits & (1u << f)) || r->watch.close_count[f])) return 0;
#endif
    for (unsigned f = 0; f < 2; f++)
        if ((mask & (1u << f)) && !ss_file_current(&r->source, &r->files[f])) return 0;
    struct st_source source;
    return !ss_source(&r->source.ref, &source) && st_resolution_source_equal(&r->source, &source);
}
static int fo_alive(struct fo_read *r) {
    /* Component-specific certified edits reset pending events after proving
     * unchanged siblings. Their historical uncommitted flags are separate. */
#ifdef __linux__
    if (r->watch.fd < 0 || r->watch.wd[0] < 0 || r->watch.wd[1] < 0) return 0;
    r->watch.pending_events |= ss_drain_events(&r->watch);
    if (r->watch.pending_events) return 0;
#endif
    for (unsigned f = 0; f < 2; f++)
        if (!ss_file_current(&r->source, &r->files[f])) return 0;
    struct st_source source;
    return !ss_source(&r->source.ref, &source) && st_resolution_source_equal(&r->source, &source);
}
static int fo_version_current(struct fo_read *r) {
    if (r->inherited) return !r->db;
    uint64_t version = 0;
    return r->db && !ss_database_version(r->db, &version) && version == r->version;
}
static int fo_input_bytes(const struct fo_read *r, unsigned file, const uint8_t *data, size_t size) {
    if (!r->inherited) return 1;
    if (!(r->opened & (1u << file))) return 0;
    char digest[65];
    azsha256_hex(data, size, digest);
    return !memcmp(digest, r->input_digest[file], sizeof digest);
}
static void fo_drop(struct fo_read *r) {
    if (!r) return;
    if (r->db) ss_close_database(r->db, r->db_fd);
    if (r->watch.fd >= 0) close(r->watch.fd);
    free(r);
}
static void fm_file_cut(struct fo_read *, const struct ss_binding *, const struct ss_image *,
                        const uint64_t[2], const unsigned[2], uint64_t);
/* A witnessed grid-only save leaves the exact cue inputs intact. Transfer
 * the raw DAT witness across that one certified edit only when both inotify
 * histories cover the same closures. Replacements, unknown writes and source
 * gaps retire the old read, even if its eventual entry bytes repeat. */
static void fo_file_cut(unsigned deck, const struct ss_binding *b, const struct ss_image *im,
                        const uint64_t known[2], const unsigned closes[2], uint64_t gap) {
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!r || r->complete != 1 || !r->count || !st_ref_equal(&r->source.ref, &b->ref)) continue;
        if (r->kind == 3) {
            /* Original FileData is immutable. Even a certified edit cannot
             * make this old cache object the owner of the replacement input. */
            if (!fo_alive_files(r, r->input_mask)) r->count = 0;
            continue;
        }
        if (r->kind == 2) {
            fm_file_cut(r, b, im, known, closes, gap);
            continue;
        }
        if (gap & SS_TOPIC(CUES)) {
#ifdef __linux__
            /* A DB observation gap can withdraw saved CUES without touching
             * the original native analysis input. Preserve its identity only
             * while the independent file watch and source still prove it.
             * Public resolution below separately requires the current saved
             * edition and exact cue atoms after recovery. */
            if (fo_alive(r)) continue;
#endif
            r->count = 0;
            continue;
        }
        if (!memcmp(r->files, im->file, sizeof r->files)) continue;
        if (memcmp(r->files, b->image.file, sizeof r->files)) {
            r->count = 0;
            continue;
        }
        unsigned clean = 0;
#ifdef __linux__
        r->watch.pending_events |= ss_drain_events(&r->watch);
        clean = known[0] == SS_TOPIC(GRID) && im->rc == AZ_OK && im->file[0].rc == AZ_OK &&
                !memcmp(&r->files[1], &im->file[1], sizeof r->files[1]) &&
                r->watch.pending_events == SS_DAT && !r->watch.unsafe_events && !r->watch.open_edits &&
                closes[0] && r->watch.close_count[0] == closes[0];
#else
        (void) known;
        (void) closes;
#endif
        if (clean) {
            r->files[0] = im->file[0];
            r->watch.pending_events = 0;
            r->watch.close_count[0] = 0;
        } else r->count = 0;
    }
    if (fo_held[deck].cut == b->cut && fo_held[deck].read) azfile_native_refresh(deck, b->epoch);
}
/* The accessor contains both canonical row locators. Require their unique
 * committed row on an actual open DB descriptor of that exact physical port.
 * An unrelated cached getter may run before, during or after another load. */
static int fo_unicode(const uint16_t *p, size_t n) {
    if (!p || !n || n > 1023) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!p[i]) return 0;
        if (p[i] >= 0xd800 && p[i] <= 0xdbff) {
            if (++i == n || p[i] < 0xdc00 || p[i] > 0xdfff) return 0;
        } else if (p[i] >= 0xdc00 && p[i] <= 0xdfff) return 0;
    }
    return 1;
}
static int fo_open(struct fo_read *r, const char *root, size_t length, const uint16_t *audio, size_t n,
                   const uint16_t *analysis, size_t m, const char *analysis_utf8, unsigned required) {
    const int physical_path_only = analysis_utf8 && !audio && !n;
    if (!root || length <= 8 || length >= 1024 || memcmp(root + length - 8, "/PIONEER", 8) ||
        (!physical_path_only && !fo_unicode(audio, n)) || (!analysis_utf8 && !fo_unicode(analysis, m)) || g_sdk_sq.ok != 1) return 0;
    unsigned found = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct st_source s;
        if (!st_source_read(i, &s) && strlen(s.mount) == length - 8 && !memcmp(root, s.mount, length - 8)) {
            r->source = s;
            found++;
        }
    }
    int (*bind16)(void *, int, const void *, int, void (*)(void *)) = dlsym(RTLD_DEFAULT, "sqlite3_bind_text16");
    int (*bind8)(void *, int, const char *, int, void (*)(void *)) = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    Dl_info provider = {0}, binding = {0}, utf8_provider = {0};
    char path[1024];
    if (found != 1 || !bind16 || !dladdr((void *) g_sdk_sq.bind_int64, &provider) ||
        !dladdr((void *) bind16, &binding) || provider.dli_fbase != binding.dli_fbase ||
        (analysis_utf8 && (!bind8 || !dladdr((void *) bind8, &utf8_provider) || provider.dli_fbase != utf8_provider.dli_fbase)) ||
        !st_source_path(r->source.mount, "/PIONEER/rekordbox/exportLibrary.db", path) ||
        !(r->db = ss_open_database(path, &r->source, &r->db_fd)) || ss_database_version(r->db, &r->version)) return 0;
    const char *sql = physical_path_only ? "SELECT content_id FROM content WHERE analysisDataFilePath=?2 COLLATE BINARY LIMIT 2" :
                                          "SELECT content_id FROM content WHERE path=?1 COLLATE BINARY "
                                          "AND analysisDataFilePath=?2 COLLATE BINARY LIMIT 2";
    void *stmt = NULL;
    uint64_t id = 0;
    if (!g_sdk_sq.prepare(r->db, sql, -1, &stmt, NULL) && stmt &&
        (physical_path_only || !bind16(stmt, 1, audio, (int) (2 * n), NULL)) &&
        !(analysis_utf8 ? bind8(stmt, 2, analysis_utf8, -1, NULL) : bind16(stmt, 2, analysis, (int) (2 * m), NULL)) &&
        g_sdk_sq.step(stmt) == 100) {
        long long value = g_sdk_sq.col_int64(stmt, 0);
        if (value > 0 && (unsigned long long) value <= UINT32_MAX && g_sdk_sq.step(stmt) == 101) id = (uint32_t) value;
    }
    if (stmt) g_sdk_sq.finalize(stmt);
    struct st_resolution row = {0};
    if (!id || st_read_row(r->db, (uint32_t) id, &row) || row.analysis_limited ||
        !st_source_path(r->source.mount, row.content, r->audio)) return 0;
    r->source.ref.track_id = (uint32_t) id;
    for (unsigned f = 0; f < 2; f++) {
        ss_file_probe(&r->source, &row, f, &r->files[f]);
        if (!r->files[f].path[0] || (r->files[f].rc && r->files[f].rc != AZ_UNSUPPORTED) ||
            (!r->files[f].rc && r->files[f].device != r->source.device)) return 0;
    }
    if (r->files[0].rc && r->files[1].rc) return 0;
    ss_arm_paths(&r->watch, r->files);
    return required ? fo_alive_files(r, required) : fo_alive(r);
}
static struct fo_read *fo_alloc(void) {
    struct fo_read *r = calloc(1, sizeof *r);
    if (r) r->watch.fd = r->watch.wd[0] = r->watch.wd[1] = r->db_fd = -1;
    return r;
}
/* Takes ownership on every path. Pending reads cannot be evicted. */
static uint64_t fo_insert(struct fo_read *r) {
    pthread_mutex_lock(&ss_mu);
    unsigned slot = FO_READS;
    for (unsigned i = 0; i < FO_READS; i++) {
        if (!fo_reads[i]) {
            slot = i;
            break;
        }
        if (fo_reads[i]->complete == 1 &&
            !(atomic_load_explicit(&fo_custody[i].state, memory_order_acquire) & FO_COUNT_MASK) &&
            !fo_in_use(fo_reads[i]->id) && (slot == FO_READS || fo_reads[i]->id < fo_reads[slot]->id)) slot = i;
    }
    struct fo_read *old = NULL;
    uint64_t id = 0;
    if (slot < FO_READS && (id = fo_new_id())) {
        uint64_t expected = fo_reads[slot] ? fo_reads[slot]->id << 16 : 0;
        if (atomic_compare_exchange_strong_explicit(&fo_custody[slot].state, &expected, 0, memory_order_acq_rel, memory_order_acquire)) {
            r->id = id;
            old = fo_reads[slot];
            fo_reads[slot] = r;
            for (unsigned i = 0; i < AZN_FILE_CUE_MAX; i++) atomic_store_explicit(&fo_custody[slot].tokens[i], 0, memory_order_relaxed);
            atomic_store_explicit(&fo_custody[slot].lost, 0, memory_order_relaxed);
            atomic_store_explicit(&fo_custody[slot].state, id << 16, memory_order_release);
        } else id = 0;
    }
    pthread_mutex_unlock(&ss_mu);
    fo_drop(old);
    if (!id) fo_drop(r);
    return id;
}
static uint64_t fo_begin(const char *root, size_t length, const uint16_t *audio, size_t n,
                         const uint16_t *analysis, size_t m) {
    struct fo_read *r = fo_alloc();
    if (!r) return 0;
    if (!fo_open(r, root, length, audio, n, analysis, m, NULL, 0)) {
        fo_drop(r);
        return 0;
    }
    return fo_insert(r);
}
static unsigned fo_opened(uint64_t id, int descriptor) {
    if (!id || descriptor < 0) return 0;
    struct {
        uint64_t device, inode, signature;
    } expected[2] = {{0}};
    uint64_t mount = 0;
    pthread_mutex_lock(&ss_mu);
    for (unsigned i = 0; i < FO_READS; i++) {
        const struct fo_read *r = fo_reads[i];
        if (!r || r->id != id || r->complete) continue;
        mount = r->source.mount_id;
        for (unsigned f = 0; f < 2; f++) {
            expected[f].device = r->files[f].device;
            expected[f].inode = r->files[f].inode;
            expected[f].signature = r->files[f].signature;
        }
        break;
    }
    pthread_mutex_unlock(&ss_mu);
    struct stat identity;
    if (!mount || fstat(descriptor, &identity) || !S_ISREG(identity.st_mode)) return 0;
    unsigned mask = 0;
    for (unsigned f = 0; f < 2; f++)
        if (expected[f].signature && expected[f].signature == st_stat_sig(&identity) &&
            expected[f].device == (uint64_t) identity.st_dev && expected[f].inode == (uint64_t) identity.st_ino &&
            azsdk_sources_descriptor_identity(descriptor, mount, expected[f].device, expected[f].inode)) mask |= 1u << f;
    if (mask != 1 && mask != 2) return 0;
    unsigned result = 0;
    pthread_mutex_lock(&ss_mu);
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (r && r->id == id && !r->complete && (!r->input_mask || r->input_mask == mask)) {
            r->opened |= mask;
            result = mask;
            break;
        }
    }
    pthread_mutex_unlock(&ss_mu);
    return result;
}
/* Verify the importer on its own storage thread, before it hands the cloned
 * rows to the separately owned native receiver. No ticket or deck is required. */
static struct fo_scan *fo_physical(const struct fo_read *r) {
    struct fo_scan *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (unsigned f = 0; f < 2 && !s->failed; f++) {
        const struct ss_file *file = &r->files[f];
        if (r->inherited && !(r->opened & (1u << f))) continue;
        if (file->rc == AZ_UNSUPPORTED) continue;
        size_t sizes[2] = {0};
        uint8_t *data = st_read_file_checked(file->path, &sizes[f], file->device, file->inode, file->signature);
        const uint8_t *input[2] = {0};
        input[f] = data;
        if (!data || !fo_input_bytes(r, f, data, sizes[f])) s->failed = 1;
        else
            for (unsigned hot = 0; hot < 2; hot++) fo_scan_list(s, input, sizes, hot, NULL, NULL);
        free(data);
    }
    return s;
}
static int fo_identified(const struct fo_scan *s, const struct azn_file_cue_row *row, unsigned opened) {
    const struct fo_atom *a = row->pcpt.total ? fo_match(s, 0, &row->pcpt) : NULL;
    const struct fo_atom *e = row->pcp2.total ? fo_match(s, 1, &row->pcp2) : NULL;
    unsigned needed = (a ? 1u << a->file : 0u) | (e ? 1u << e->file : 0u);
    return needed && (opened & needed) == needed && (!row->pcpt.total || a) && (!row->pcp2.total || e) &&
           (!a || !e || (a->hot == e->hot && a->slot == e->slot));
}
static int fo_stamp_equal(const struct azn_file_atom *a, const struct azn_file_atom *b) {
    return a->header == b->header && a->total == b->total && !memcmp(a->digest, b->digest, sizeof a->digest);
}
static void fo_complete(uint64_t id, const struct azn_file_cue_row *rows, size_t n,
                        uint64_t tokens[AZN_FILE_CUE_MAX]) {
    if (tokens) memset(tokens, 0, AZN_FILE_CUE_MAX * sizeof *tokens);
    pthread_mutex_lock(&ss_mu);
    struct fo_read *r = NULL;
    unsigned slot = 0;
    for (; slot < FO_READS; slot++)
        if (fo_reads[slot] && fo_reads[slot]->id == id && !fo_reads[slot]->complete) {
            r = fo_reads[slot];
            r->complete = 2;
            break;
        }
    pthread_mutex_unlock(&ss_mu);
    if (!r) return;
    int ok = n && n <= AZN_FILE_CUE_MAX && rows && tokens && fo_version_current(r) && fo_alive(r);
    struct fo_scan *physical = ok ? fo_physical(r) : NULL;
    ok = physical && !physical->failed && fo_alive(r) && fo_version_current(r);
    if (r->db) ss_close_database(r->db, r->db_fd);
    r->db = NULL;
    pthread_mutex_lock(&ss_mu);
    if (ok) {
        r->kind = 1;
        r->count = (unsigned) n;
        memcpy(r->row, rows, n * sizeof *rows);
        for (unsigned i = 0; i < n; i++) {
            int unique = fo_identified(physical, &rows[i], r->opened);
            for (unsigned j = 0; j < n; j++)
                if (i != j &&
                    ((rows[i].pcpt.total && fo_stamp_equal(&rows[i].pcpt, &rows[j].pcpt)) ||
                     (rows[i].pcp2.total && fo_stamp_equal(&rows[i].pcp2, &rows[j].pcp2)))) unique = 0;
            if (unique) {
                uint64_t serial = fo_new_id();
                tokens[i] = r->token[i] = serial ? (serial << 6) | slot : 0;
                atomic_store_explicit(&fo_custody[slot].tokens[i], tokens[i], memory_order_release);
            }
        }
        r->complete = 1;
    } else {
        fo_reads[slot] = NULL;
        atomic_store_explicit(&fo_custody[slot].state, 0, memory_order_release);
    }
    pthread_mutex_unlock(&ss_mu);
    free(physical);
    if (!ok) fo_drop(r);
}
static void fo_hold(const uint64_t *tokens, size_t n, int acquire) {
    if (!tokens || !n || n > AZN_FILE_CUE_MAX) return;
    uint64_t visited = 0;
    for (size_t requested = 0; requested < n; requested++) {
        if (!tokens[requested]) continue;
        unsigned slot = (unsigned) (tokens[requested] & 63u);
        uint64_t bit = UINT64_C(1) << slot;
        if (visited & bit) continue;
        visited |= bit;
        uint64_t state = atomic_load_explicit(&fo_custody[slot].state, memory_order_acquire), generation = state >> 16;
        if (!generation) continue;
        int found = 0;
        for (unsigned i = 0; i < AZN_FILE_CUE_MAX && !found; i++) {
            uint64_t token = atomic_load_explicit(&fo_custody[slot].tokens[i], memory_order_acquire);
            for (size_t j = 0; token && j < n; j++)
                if (tokens[j] == token) {
                    found = 1;
                    break;
                }
        }
        if (!found) continue;
        unsigned attempts = 0;
        for (; attempts < 16 && state >> 16 == generation; attempts++) {
            uint64_t count = state & FO_COUNT_MASK;
            if ((acquire && count == FO_COUNT_MASK) || (!acquire && !count)) {
                atomic_store_explicit(&fo_custody[slot].lost, 1, memory_order_release);
                break;
            }
            uint64_t next = acquire ? state + 1 : state - 1;
            if (atomic_compare_exchange_strong_explicit(&fo_custody[slot].state, &state, next, memory_order_acq_rel, memory_order_acquire)) break;
        }
        if (attempts == 16 && state >> 16 == generation)
            atomic_store_explicit(&fo_custody[slot].lost, 1, memory_order_release);
    }
}
__attribute__((visibility("hidden"))) const struct azn_file_cue_callbacks *azfile_status_callbacks(void) {
    /* Version2 requires original physical-read/cache-entry custody. A v1
     * producer must decline instead of rebinding cached atoms by equal bytes. */
    static const struct azn_file_cue_callbacks cb = {sizeof cb, 2, fo_begin, fo_complete, fo_hold, fo_opened, fi_api};
    return &cb;
}
static const struct fo_atom *fo_match(const struct fo_scan *s, unsigned ext, const struct azn_file_atom *key) {
    const struct fo_atom *found = NULL;
    for (unsigned i = 0; i < s->n; i++) {
        const struct fo_atom *a = &s->atoms[i];
        if (a->extended != ext || a->stamp.header != key->header || a->stamp.total != key->total ||
            !key->digest[0] || memcmp(a->stamp.digest, key->digest, sizeof key->digest)) continue;
        if (found) return NULL;
        found = a;
    }
    return found;
}
static int fo_native_source(unsigned deck, const struct ss_binding *binding, const struct st_source *source) {
    if (!ss_pins[deck].token) return 1;
    struct azjs_origin origin = {0};
    struct azsdk_source_sample native = {0};
    return ss_pins[deck].epoch == binding->epoch && !ss_pins[deck].retired &&
           !ss_pin_witness(ss_pins[deck].token, &origin, &native) && origin.track_id == binding->ref.track_id &&
           native.mount_id == source->mount_id && native.device == source->device && native.inode == source->inode;
}
__attribute__((visibility("hidden"))) int azfile_status_ref(unsigned deck, uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *out) {
    if (deck >= 4 || !epoch || !token || !out) return 0;
    int found = 0;
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *b = &ss_decks[deck];
    const struct fo_edition *ed = &fo_editions[deck];
    if (b->epoch == epoch && b->cut == ed->cut && b->ready && !b->writing && !b->retired &&
        (b->topics & SS_TOPIC(CUES)) && !(b->pending & SS_TOPIC(CUES)) && !b->pinned[SS_SAVED_CUES] &&
        ed->revision && b->revision[AZ_STATUS_CUES] == ed->revision && b->provider[3] == ed->provider && !ed->scan.failed) {
        for (unsigned i = 0; i < FO_READS; i++) {
            struct fo_read *r = fo_reads[i];
            if (!r || r->complete != 1 || r->kind != 1 || atomic_load_explicit(&fo_custody[i].lost, memory_order_acquire) ||
                atomic_load_explicit(&fo_custody[i].state, memory_order_acquire) >> 16 != r->id ||
                !st_ref_equal(&r->source.ref, &b->ref) ||
                strcmp(r->audio, b->image.metadata.path) || memcmp(r->files, b->image.file, sizeof r->files)) continue;
            for (unsigned j = 0; j < r->count; j++) {
                if (r->token[j] != token) continue;
                if (!fo_alive(r)) break; /* the monitor resolves a pending certified grid edit */
                if (!fo_native_source(deck, b, &r->source)) break;
                const struct azn_file_cue_row *row = &r->row[j];
                const struct fo_atom *a = row->pcpt.total ? fo_match(&ed->scan, 0, &row->pcpt) : NULL;
                const struct fo_atom *e = row->pcp2.total ? fo_match(&ed->scan, 1, &row->pcp2) : NULL;
                if ((row->pcpt.total && !a) || (row->pcp2.total && !e) ||
                    (a && e && (a->hot != e->hot || a->slot != e->slot)) ||
                    (a && e && a->selected.type && e->selected.type)) break;
                const struct fo_atom *selected = e && e->selected.type ? e : a;
                if (!selected || selected->selected.type != AZ_CUE_LOOP) break;
                *out = (az_status_cue_ref_v1) {.load_epoch = epoch, .provider_id = ed->provider, .collection_revision = ed->revision, .provenance = AZ_PROV_FILE, .cue_field = selected->hot ? AZ_STATUS_CUES_SAVED_HOT : AZ_STATUS_CUES_SAVED_MEMORY, .entry = selected->selected};
                fo_held[deck].cut = b->cut;
                fo_held[deck].read = r->id;
                found = 1;
                break;
            }
            if (found) break;
        }
    }
    pthread_mutex_unlock(&ss_mu);
    return found;
}

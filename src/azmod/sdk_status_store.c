/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after sdk_track.c and the reducer. Only a positively identified LOAD.track can
 * arm storage. Source/commit observation runs independently of the parsing worker. No
 * status selection consumes a mod ticket or changes the explicit immutable SQLite reader. */
#include <poll.h>
#include "libfd.h"
#include "../common/azsha256.h"
#include "../common/azstatus_source.h"
#include "../common/azstatus_identity.h"
#include "sdk_status_proof.h"
#pragma weak azjs_lookup
#pragma weak azsdk_sources_status_read
#ifdef __linux__
#include <sys/inotify.h>
#endif

#define SS_TOPIC(name) AZ_STATUS_TOPIC(AZ_STATUS_##name)
#define SS_META (SS_TOPIC(METADATA) | SS_TOPIC(KEY) | SS_TOPIC(TEMPO))
#define SS_DAT (SS_TOPIC(GRID) | SS_TOPIC(CUES) | SS_TOPIC(OVERVIEW))
#define SS_EXT (SS_TOPIC(PHRASES) | SS_TOPIC(CUES))
#define SS_ALL (SS_META | SS_DAT | SS_EXT)

struct ss_file {
    char path[1024], relative[1024];
    uint64_t signature, device, inode;
    int rc;
};
struct ss_image {
    struct st_source src;
    az_track_metadata metadata;
    int64_t duration_ticks;
    struct ss_file file[2];
    int rc;
};
static uint64_t ss_metadata_changes(const struct ss_image *before, const struct ss_image *after) {
    int duration_changed = !!((before->metadata.valid ^ after->metadata.valid) & AZ_META_DURATION);
    if ((before->metadata.valid & after->metadata.valid) & AZ_META_DURATION) {
        /* Library duration has whole-second resolution. Its exact integer
         * source is authoritative; the public seconds copy is not a clock. */
        int order = compare_timestamps(before->duration_ticks, after->duration_ticks, 1, 1, 1);
        duration_changed = order == AZ_TIME_INVALID || order != AZ_TIME_EQUAL;
    }
    az_track_metadata left = before->metadata, right = after->metadata;
    left.duration_s = right.duration_s = 0;
    uint64_t changed = duration_changed ? SS_META | SS_TOPIC(OVERVIEW) : 0;
    if (memcmp(&left, &right, sizeof left)) changed |= SS_META;
    return changed;
}
static int ss_image_same(const struct ss_image *before, const struct ss_image *after) {
    return before->rc == after->rc && !memcmp(&before->src, &after->src, sizeof before->src) &&
           !memcmp(before->file, after->file, sizeof before->file) && !ss_metadata_changes(before, after);
}
enum {
    SS_SAVED_CUES,
    SS_SAVED_GRID,
    SS_SAVED_LANES
};
static const unsigned ss_saved_topic[SS_SAVED_LANES] = {AZ_STATUS_CUES, AZ_STATUS_GRID};
static const unsigned ss_saved_provider[SS_SAVED_LANES] = {3, 1};
struct ss_write_proof {
    uint64_t revision, ordinal, signature, device, inode;
    char digest[65];
    uint64_t only;
};
struct ss_binding {
    uint64_t cut, epoch, topics, pending, serial, interest[SS_SAVED_LANES];
    az_track_ref ref;
    struct ss_image image;
    struct ss_write_proof completed[2];
    uint64_t consumed[2];
    char siblings[SS_SAVED_LANES][2][65];
    unsigned writing, pinned[SS_SAVED_LANES];
    int overlapping;
    uint64_t provider[4], revision[AZ_STATUS_TOPIC_COUNT];
    int ready, retired;
    uint64_t proof_connection, proof_version;
};
static struct ds_proof_context ss_proof_context(const struct ss_binding *b, unsigned kind, unsigned cause) {
    struct ds_proof_context p = {.kind = kind, .cause = cause, .binding_cut = b->cut,
        .binding_serial = b->serial, .connection = b->proof_connection,
        .db_before = b->proof_version, .db_after = b->proof_version,
        .device = b->image.src.device, .inode = b->image.src.inode, .mount_id = b->image.src.mount_id};
    memcpy(p.providers, b->provider, sizeof p.providers);
    memcpy(p.revisions, b->revision, sizeof p.revisions);
    for (unsigned f = 0; f < 2; f++) {
        p.file_device[f] = b->image.file[f].device;
        p.file_inode[f] = b->image.file[f].inode;
        p.file_signature[f] = b->image.file[f].signature;
    }
    return p;
}
/* Small immutable snapshots of successful native storage commits. The worker
 * drains these before reading the latest files. Overflow has a separate,
 * filtered history loss; it cannot evict an unrelated explicit ticket. */
struct ss_saved_revision {
    az_track_ref ref;
    struct {
        uint64_t cut, epoch, interest, provider, source_pin;
    } target[4];
    uint8_t *bytes[2];
    size_t sizes[2], total;
    int rc[2];
    unsigned targets, lane;
    uint64_t observed, revision;
};
enum {
    SS_SAVE_QUEUE = 16,
    SS_SAVE_PIN_BYTES = 4 * 1024 * 1024
};
static struct ss_saved_revision *ss_saved_queue[SS_SAVE_QUEUE];
static unsigned ss_saved_count;
static size_t ss_saved_bytes;
static void ss_release_saved(struct ss_saved_revision *job) {
    if (!job) return;
    free(job->bytes[0]);
    free(job->bytes[1]);
    free(job);
}
static struct ss_binding ss_decks[4];
static pthread_mutex_t ss_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ss_start_mu = PTHREAD_MUTEX_INITIALIZER;
static int ss_started;
static uint64_t ss_serial, ss_provider;
static uint64_t ss_new_id(uint64_t *counter) {
    return *counter < INT64_MAX ? ++*counter : 0;
}
static uint64_t ss_topics(uint64_t t) {
    if (t & (SS_TOPIC(POSITION) | SS_TOPIC(STEMS))) t |= SS_TOPIC(METADATA);
    if (t & SS_TOPIC(PHRASES)) t |= SS_TOPIC(GRID);
    return t & (SS_ALL | SS_TOPIC(LOAD));
}
static struct {
    uint64_t epoch, token;
    az_track_ref ref;
    int published, retired;
} ss_pins[4];
static int ss_local(const az_track_ref *ref) {
    return ref && ref->version == AZ_DATA_VERSION && ref->size >= sizeof *ref &&
           ref->source_id && ref->source_id <= 2 && ref->source_generation && ref->track_id && ref->library_id;
}
static int ss_source(const az_track_ref *ref, struct st_source *src) {
    int rc = st_source_read((int) ref->source_id - 1, src);
    if (rc) return rc;
    src->ref.track_id = ref->track_id;
    return st_ref_equal(&src->ref, ref) ? AZ_OK : AZ_STALE;
}
static int ss_database_version(void *db, uint64_t *value) {
    void *stmt = NULL;
    int rc = AZ_FAILED;
    if (!g_sdk_sq.prepare(db, "PRAGMA data_version", -1, &stmt, NULL) && stmt &&
        g_sdk_sq.step(stmt) == 100) {
        *value = (uint64_t) g_sdk_sq.col_int64(stmt, 0);
        rc = AZ_OK;
    }
    if (stmt) g_sdk_sq.finalize(stmt);
    return rc;
}
/* Non-URI, READONLY. SQLite owns the read locks and WAL snapshot. The immutable
 * path used by old enumeration/tickets would silently omit a live committed WAL. */
static void *ss_open_database(const char *path, const struct st_source *src, int *fd) {
    void *db = NULL;
    if (g_sdk_sq.open_v2(path, &db, 1, NULL) == 0 && db &&
        !g_sdk_sq.key(db, ONELIBRARY_KEY_SM, (int) sizeof ONELIBRARY_KEY_SM - 1)) {
        *fd = azsdk_sources_reader_fd(db, src->mount_id, src->device, src->inode);
        if (libfd_add(*fd, LIBFD_STATUS)) return db;
    }
    if (db) g_sdk_sq.close(db);
    return NULL;
}
static void ss_close_database(void *db, int fd) {
    libfd_del(fd);
    g_sdk_sq.close(db);
}
/* OneLibrary content.fileType codes. Read this optional column separately so
 * an older export schema cannot take the other metadata fields offline. */
static void ss_read_format(void *db, uint32_t track_id, az_track_metadata *m) {
    static const char *const names[] = {
        [1] = "MP3", [3] = "MP4", [4] = "AAC", [5] = "FLAC", [6] = "ALAC", [11] = "WAV", [12] = "AIFF"};
    void *stmt = NULL;
    const char *sql = "SELECT fileType FROM content WHERE content_id=?1 AND typeof(fileType)='integer'";
    if (!g_sdk_sq.prepare(db, sql, -1, &stmt, NULL) && stmt && !g_sdk_sq.bind_int64(stmt, 1, track_id) &&
        g_sdk_sq.step(stmt) == 100) {
        long long type = g_sdk_sq.col_int64(stmt, 0);
        if (type >= 0 && (uint64_t) type < sizeof names / sizeof names[0] && names[type] && g_sdk_sq.step(stmt) == 101) {
            snprintf(m->format, sizeof m->format, "%s", names[type]);
            m->valid |= AZ_META_FORMAT;
        }
    }
    if (stmt) g_sdk_sq.finalize(stmt);
}
static void ss_file_probe(const struct st_source *src, const struct st_resolution *row,
                          int ext, struct ss_file *file) {
    file->rc = AZ_UNSUPPORTED;
    if (row->analysis_limited) {
        file->rc = AZ_LIMIT;
        return;
    }
    if (!row->analysis[0]) return;
    char relative[sizeof row->analysis];
    memcpy(relative, row->analysis, sizeof relative);
    char *suffix = strrchr(relative, '.');
    if (!suffix || strcmp(suffix, ".DAT")) return;
    if (ext) memcpy(suffix, ".EXT", 5);
    memcpy(file->relative, relative, sizeof file->relative);
    if (!st_source_path(src->mount, relative, file->path)) {
        file->rc = AZ_FAILED;
        return;
    }
    struct stat sb;
    if (stat(file->path, &sb)) {
        if (errno != ENOENT && errno != ENOTDIR) file->rc = AZ_FAILED;
        return;
    }
    file->device = (uint64_t) sb.st_dev;
    file->inode = (uint64_t) sb.st_ino;
    file->signature = st_stat_sig(&sb);
    /* Never open the live SQLite database as an analysis file, even through a
     * hard link. Closing another descriptor for that inode can discard POSIX locks. */
    file->rc = S_ISREG(sb.st_mode) && file->signature &&
                       (file->device != src->device || file->inode != src->inode)
                   ? AZ_OK
                   : AZ_FAILED;
}
struct ss_monitor {
    uint64_t cut, version;
    uint64_t confirmed[2], confirmed_rank[2];
    unsigned close_count[2], unsafe_events, open_edits;
    uint64_t pending_events, pending_db_gap;
    unsigned proof_pending_cause;
    uint64_t proof_pending_before;
    int seeded, fd, db_fd, wd[2], uncommitted_file[2], row_ready;
    struct st_resolution row;
    char paths[2][1024];
    void *db;
};
static struct ss_monitor ss_monitors[4];
static void ss_monitor_reset(struct ss_monitor *m) {
    if (m->db) ss_close_database(m->db, m->db_fd);
    if (m->seeded && m->fd >= 0) close(m->fd);
    memset(m, 0, sizeof *m);
    m->fd = m->wd[0] = m->wd[1] = -1;
}
#ifdef __linux__
static uint64_t ss_drain_events(struct ss_monitor *m) {
    if (m->fd < 0) return 0;
    union {
        struct inotify_event align;
        char bytes[8192];
    } events;
    uint64_t affected = 0;
    for (int batch = 0; batch < 16; batch++) {
        ssize_t n = read(m->fd, events.bytes, sizeof events.bytes);
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? affected : SS_ALL;
        if (!n) return affected | SS_ALL;
        for (size_t p = 0; p < (size_t) n;) {
            const struct inotify_event *e = (const void *) (events.bytes + p);
            p += sizeof *e + e->len;
            if (e->mask & IN_Q_OVERFLOW) {
                affected |= SS_ALL;
                m->uncommitted_file[0] = m->uncommitted_file[1] = 1;
            }
            for (int f = 0; f < 2; f++) {
                if (e->wd != m->wd[f]) continue;
                const char *base = strrchr(m->paths[f], '/');
                if (!e->len || (base && !strcmp(base + 1, e->name))) {
                    /* Linux reports CLOSE_WRITE even for O_RDWR descriptors
                     * that never wrote a byte. Pair closes with MODIFY across
                     * drain calls, including drains interrupted by a stock writer. */
                    if ((e->mask & IN_CLOSE_WRITE) && !(m->open_edits & (1u << f))) continue;
                    affected |= f ? SS_EXT : SS_DAT;
                    if (e->mask & IN_MODIFY) m->open_edits |= 1u << f;
                    if (e->mask & IN_CLOSE_WRITE) {
                        ++m->close_count[f];
                        m->open_edits &= ~(1u << f);
                    }
                    if (e->mask & ~(IN_MODIFY | IN_CLOSE_WRITE)) m->unsafe_events |= 1u << f;
                    if (e->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE)) m->uncommitted_file[f] = 1;
                    if (e->mask & IN_MOVED_TO) m->uncommitted_file[f] = 0;
                }
                if (e->mask & IN_IGNORED) m->wd[f] = -1;
            }
        }
    }
    return affected | SS_ALL;
}
static void ss_arm_paths(struct ss_monitor *m, const struct ss_file files[2]) {
    if (m->fd < 0 || strcmp(m->paths[0], files[0].path) || strcmp(m->paths[1], files[1].path)) {
        if (m->fd >= 0) close(m->fd);
        for (int f = 0; f < 2; f++)
            if (strcmp(m->paths[f], files[f].path)) m->uncommitted_file[f] = 0;
        m->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        m->wd[0] = m->wd[1] = -1;
    }
    for (int f = 0; f < 2; f++) {
        snprintf(m->paths[f], sizeof m->paths[f], "%s", files[f].path);
        if (m->fd < 0 || m->wd[f] >= 0 || !files[f].path[0]) continue;
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", files[f].path);
        char *base = strrchr(parent, '/');
        if (!base) continue;
        *base = 0;
        m->wd[f] = inotify_add_watch(m->fd, parent, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_DELETE_SELF | IN_CLOSE_WRITE);
    }
}
#else
static uint64_t ss_drain_events(struct ss_monitor *m) {
    (void) m;
    return 0;
}
static void ss_arm_paths(struct ss_monitor *m, const struct ss_file f[2]) {
    (void) m;
    (void) f;
}
#endif
static uint64_t ss_file_provider(void) {
    uint64_t id = ss_new_id(&ss_provider);
    return id ? id | AZSP_FILE : 0;
}
static int ss_read_digest(const struct ss_file *file, char digest[65], uint8_t **copy, size_t *count) {
    if (!file->signature || st_disk_sig(file->path) != file->signature) return 0;
    size_t size = 0;
    unsigned char *data = st_read_file_checked(file->path, &size, file->device, file->inode, file->signature);
    if (!data) return 0;
    azsha256_ctx ctx;
    azsha256_init(&ctx);
    azsha256_update(&ctx, data, size);
    azsha256_final_hex(&ctx, digest);
    if (st_disk_sig(file->path) != file->signature) {
        free(data);
        return 0;
    }
    if (copy && count && size <= SS_SAVE_PIN_BYTES) {
        *copy = data;
        *count = size;
    } else free(data);
    return 1;
}
static int ss_digest(const struct ss_file *file, char digest[65]) {
    return ss_read_digest(file, digest, NULL, NULL);
}
static void fo_file_cut(unsigned, const struct ss_binding *, const struct ss_image *,
                        const uint64_t[2], const unsigned[2], uint64_t);
/* m->db remains on this thread: data_version is comparable only on the same
 * connection. A changed token establishes committed activity, never its number or
 * affected rows. An unchanged rollback is quiet. Neither a close nor a WAL append
 * alone is used to claim that an intermediate row or analysis value was observed. */
static void ss_monitor_deck(unsigned deck) {
    pthread_mutex_lock(&ss_mu);
    struct ss_binding before = ss_decks[deck];
    pthread_mutex_unlock(&ss_mu);
    struct ss_monitor *m = &ss_monitors[deck];
    if (!before.topics || !before.ref.track_id || before.retired) {
        if (m->seeded) ss_monitor_reset(m);
        return;
    }
    if (!m->seeded || m->cut != before.cut) {
        ss_monitor_reset(m);
        m->seeded = 1;
        m->cut = before.cut;
    }
    if (before.writing) {
        struct st_source live;
        if (!ss_source(&before.ref, &live)) return;
    }
    m->pending_events |= ss_drain_events(m);
    uint64_t filesystem_events = m->pending_events;
    uint64_t confirmed[2] = {0}, rank[2] = {0};
    uint64_t gap = filesystem_events == SS_ALL ? SS_ALL : m->pending_db_gap;
    unsigned proof_cause = m->proof_pending_cause | (filesystem_events == SS_ALL ? DS_PROOF_FILESYSTEM : 0);
    uint64_t proof_before = m->pending_db_gap ? m->proof_pending_before : m->version;
    uint64_t known[2] = {0};
    int row_retired = 0;
    struct ss_image im = {0};
    int source_rc = ss_source(&before.ref, &im.src);
    uint64_t version = 0;
    char path[1024];
    im.rc = source_rc;
    if (!source_rc) {
        im.rc = AZ_FAILED;
        if (!m->db && st_source_path(im.src.mount, "/PIONEER/rekordbox/exportLibrary.db", path))
            m->db = ss_open_database(path, &im.src, &m->db_fd);
        if (m->db && !ss_database_version(m->db, &version)) {
            if (m->version && m->version != version) { gap |= SS_ALL; proof_cause |= DS_PROOF_DATABASE; }
            if (!m->row_ready || m->version != version) {
                memset(&m->row, 0, sizeof m->row);
                int ambiguous = 0;
                im.rc = st_read_row_checked(m->db, before.ref.track_id, &m->row, &ambiguous);
                row_retired = im.rc == AZ_NOT_FOUND || ambiguous;
                if (!im.rc) ss_read_format(m->db, before.ref.track_id, &m->row.metadata);
                m->row_ready = im.rc == AZ_OK;
            } else im.rc = AZ_OK;
            m->version = version;
            uint64_t after = 0;
            if (ss_database_version(m->db, &after) || after != version) {
                im.rc = AZ_STALE;
                row_retired = 0;
                m->row_ready = 0;
                gap |= SS_ALL;
                proof_cause |= DS_PROOF_READ;
            } else if (!im.rc) {
                im.metadata = m->row.metadata;
                im.duration_ticks = m->row.duration_ticks;
                if (m->row.content[0] && st_source_path(im.src.mount, m->row.content, path) && strlen(path) < sizeof im.metadata.path) {
                    memcpy(im.metadata.path, path, strlen(path) + 1);
                    im.metadata.valid |= AZ_META_PATH;
                }
                for (int f = 0; f < 2; f++) ss_file_probe(&im.src, &m->row, f, &im.file[f]);
                ss_arm_paths(m, im.file);
                for (int f = 0; f < 2; f++) {
                    const struct ss_file *old = &before.image.file[f];
                    if (before.ready && old->signature && old->signature != im.file[f].signature &&
                        old->inode == im.file[f].inode && old->device == im.file[f].device)
                        m->uncommitted_file[f] = 1;
                    const struct ss_write_proof *proof = &before.completed[f];
                    if (proof->revision && m->confirmed[f] != proof->revision) {
                        char digest[65];
                        if (im.file[f].signature == proof->signature && im.file[f].device == proof->device &&
                            im.file[f].inode == proof->inode && ss_digest(&im.file[f], digest) &&
                            !memcmp(digest, proof->digest, sizeof digest)) {
                            /* The native writer closed these exact files. If the
                             * watcher saw another closer, an overflow, different
                             * unrelated bytes or any replacement, retain its gap. */
                            if (proof->only && m->fd >= 0 && m->wd[f] >= 0 &&
                                !(m->unsafe_events & (1u << f)) && proof->ordinal > m->confirmed_rank[f] &&
                                m->close_count[f] == proof->ordinal - m->confirmed_rank[f]) known[f] = proof->only;
                            m->uncommitted_file[f] = 0;
                            confirmed[f] = proof->revision;
                            rank[f] = proof->ordinal;
                        }
                    }
                    if (m->uncommitted_file[f] && im.file[f].rc == AZ_OK)
                        im.file[f].rc = AZ_STALE;
                }
            }
        }
        struct st_source chk;
        source_rc = ss_source(&before.ref, &chk);
        if (source_rc) im.rc = source_rc;
    }
    if (before.ready && source_rc) {
        proof_cause |= DS_PROOF_SOURCE;
        /* A normal disconnection is an invalidation. Only charge a capture gap
         * if the committed activity token cannot close the interval, the observer
         * already found a file gap, or a pinned revision was still owed. */
        uint64_t final_version = 0;
        if (!m->db || ss_database_version(m->db, &final_version) || final_version != m->version) gap |= SS_ALL;
        gap |= before.pending;
        for (unsigned l = 0; l < SS_SAVED_LANES; l++)
            if (before.pinned[l]) gap |= AZ_STATUS_TOPIC(ss_saved_topic[l]);
    }
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *d = &ss_decks[deck];
    /* A writer can begin and finish while this pass is checking SQLite/files.
     * Its notification may already have been read. Keep both the notification
     * and the previous certificate rank for the retry against the new proof. */
    if (d->cut != before.cut || d->retired || !d->topics ||
        (!source_rc && (d->writing || d->serial != before.serial))) {
        if (gap == SS_ALL) {
            m->pending_db_gap = SS_ALL;
            m->proof_pending_cause = proof_cause;
            m->proof_pending_before = proof_before;
        }
        pthread_mutex_unlock(&ss_mu);
        return;
    }
    m->pending_db_gap = 0;
    m->proof_pending_cause = 0;
    m->pending_events = 0;
    m->unsafe_events = 0;
    for (unsigned f = 0; f < 2; f++)
        if (confirmed[f]) {
            m->confirmed[f] = confirmed[f];
            m->confirmed_rank[f] = rank[f];
        }
    uint64_t touched = !d->ready || im.rc != d->image.rc ? SS_ALL : 0;
    touched |= ss_metadata_changes(&d->image, &im);
    int changed[2];
    for (int f = 0; f < 2; f++) {
        if (m->confirmed[f] > d->consumed[f]) d->consumed[f] = m->confirmed[f];
        changed[f] = memcmp(&d->image.file[f], &im.file[f], sizeof im.file[f]) != 0;
        uint64_t components = f ? SS_EXT : SS_DAT;
        int clean = known[f] && im.rc == AZ_OK && d->image.rc == AZ_OK &&
                    im.file[f].rc == AZ_OK && d->image.file[f].rc == AZ_OK;
        if (changed[f]) touched |= clean ? known[f] : components;
        if (!clean && ((filesystem_events & (f ? SS_TOPIC(PHRASES) : SS_TOPIC(GRID))) ||
                       (d->ready && changed[f] && im.rc == AZ_OK && d->image.rc == AZ_OK))) {
            gap |= components;
            proof_cause |= DS_PROOF_FILE_CHANGE;
        }
    }
    if (before.ready && !source_rc && im.rc != AZ_OK && d->image.rc == AZ_OK) {
        gap |= SS_ALL;
        proof_cause |= DS_PROOF_READ;
    }
    if (d->ready && (changed[0] || changed[1] || (gap & (SS_DAT | SS_EXT))))
        fo_file_cut(deck, d, &im, known, m->close_count, gap);
    memset(m->close_count, 0, sizeof m->close_count);
    if (d->ready && (gap & d->topics) && atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        struct ds_proof_context previous = ds_proof_context;
        ds_proof_context = ss_proof_context(d, DS_PROOF_GAP, proof_cause);
        ds_proof_context.connection = (uintptr_t) m->db;
        ds_proof_context.db_before = proof_before;
        ds_proof_context.db_after = version;
        ds_publish_file(deck, d->epoch, &d->ref, st_now(), NULL, 0, gap & d->topics, AZ_STATUS_CAPTURE_GAP);
        ds_proof_context = previous;
    } else if (d->ready && (gap & d->topics))
        ds_publish_file(deck, d->epoch, &d->ref, st_now(), NULL, 0, gap & d->topics, AZ_STATUS_CAPTURE_GAP);
    if (atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        d->proof_connection = (uintptr_t) m->db;
        d->proof_version = version;
    }
    touched |= gap;
    if (touched || !d->ready) {
        uint64_t serial = ss_new_id(&ss_serial);
        if (!serial) {
            pthread_mutex_unlock(&ss_mu);
            return;
        }
        for (unsigned i = 0; i < 3; i++) {
            /* Replacement changes the producer incarnation, even at equal bytes. */
            if (!d->provider[i] || (i && (im.file[i - 1].inode != d->image.file[i - 1].inode ||
                                          im.file[i - 1].device != d->image.file[i - 1].device ||
                                          strcmp(im.file[i - 1].path, d->image.file[i - 1].path)))) d->provider[i] = ss_file_provider();
        }
        /* Hot and memory lists can draw from both files. Neither single container
         * identity denotes this combined cue producer. */
        if (!d->provider[3] || d->provider[1] != before.provider[1] || d->provider[2] != before.provider[2])
            d->provider[3] = ss_file_provider();
        for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
            uint64_t bit = AZ_STATUS_TOPIC(t);
            if (!(touched & bit)) continue;
            uint64_t revision = serial;
            /* Draining the same positive commit through the monitor must not
             * manufacture a second saved collection edition. */
            if (!(gap & bit))
                for (unsigned f = 0; f < 2; f++)
                    if ((known[f] & bit) && confirmed[f]) revision = confirmed[f];
            d->revision[t] = revision;
        }
        d->image = im;
        d->serial = serial;
        d->ready = 1;
        d->pending |= touched & d->topics;
    }
    if (source_rc || row_retired) {
        d->retired = 1;
        d->pending = 0;
        if (row_retired && ss_pins[deck].epoch == before.epoch && ss_pins[deck].token) {
            ss_pins[deck].retired = 1;
            ss_pins[deck].published = 0;
        }
    }
    int pending = d->pending != 0;
    pthread_mutex_unlock(&ss_mu);
    if ((source_rc || row_retired) && atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        struct ds_proof_context previous = ds_proof_context;
        ds_proof_context = ss_proof_context(&before, DS_PROOF_RETIRE, DS_PROOF_SOURCE);
        ds_retire_file_binding(deck, before.epoch, &before.ref);
        ds_proof_context = previous;
    } else if (source_rc || row_retired) ds_retire_file_binding(deck, before.epoch, &before.ref);
    if (pending) st_worker_wake();
}
/* A worker pass reads each relevant container once. Per-component parsing preserves
 * independent failures. A bad PSSI payload cannot poison GRID or PWAV. */
static int ss_file_current(const struct st_source *, const struct ss_file *);
static int ss_pin_witness(uint64_t, struct azjs_origin *, struct azsdk_source_sample *);
#include "sdk_status_cue_origin.c"
#include "sdk_status_file_input.c"
#include "sdk_status_map_origin.c"
struct ss_parse {
    struct fo_scan origin;
    struct fm_tags map_tags;
    struct st_data data;
    int64_t grid_ticks[ST_BEATS];
    int64_t cue_ticks[2 * ST_CUES], hot_ticks[16];
    uint32_t cue_availability[2], hot_count, grid, phrases, overview;
    az_cue_entry hot[8];
    char siblings[SS_SAVED_LANES][2][65];
};
static void fm_update(unsigned deck, const struct ss_binding *b, uint64_t topics, const struct ss_parse *parsed) {
    struct fm_edition *e = &fm_editions[deck];
    if (e->cut != b->cut) {
        memset(e, 0, sizeof *e);
        e->cut = b->cut;
    }
    const unsigned topic[] = {AZ_STATUS_GRID, AZ_STATUS_PHRASES};
    for (unsigned c = 0; c < 2; c++) {
        if (!(topics & AZ_STATUS_TOPIC(topic[c]))) continue;
        unsigned tag = c ? 2u : 0u, bit = 1u << c;
        e->ready &= ~bit;
        e->provider[c] = b->provider[c + 1];
        e->revision[c] = b->revision[topic[c]];
        if (!parsed) continue;
        e->tags.value[tag] = parsed->map_tags.value[tag];
        e->tags.present = (e->tags.present & ~(1u << tag)) | (parsed->map_tags.present & (1u << tag));
        e->tags.ambiguous = (e->tags.ambiguous & ~(1u << tag)) | (parsed->map_tags.ambiguous & (1u << tag));
        unsigned availability = c ? parsed->phrases : parsed->grid;
        if (availability == AZ_AVAIL_READY) e->ready |= bit;
        if (c) {
            e->phrase_end = 0;
            for (unsigned i = 0; i < parsed->data.view.phrase_count; i++)
                if (parsed->data.phrases[i].end_beat > e->phrase_end) e->phrase_end = parsed->data.phrases[i].end_beat;
            e->tags.value[1] = parsed->map_tags.value[1];
            e->tags.present = (e->tags.present & ~2u) | (parsed->map_tags.present & 2u);
            e->tags.ambiguous = (e->tags.ambiguous & ~2u) | (parsed->map_tags.ambiguous & 2u);
        } else {
            e->beats = parsed->data.view.beat_count;
            for (unsigned i = 0; i < FO_READS; i++) {
                struct fo_read *r = fo_reads[i];
                if (!r || r->kind != 2 || r->complete != 1 || !r->retimed ||
                    !st_ref_equal(&r->source.ref, &b->ref) || memcmp(r->files, b->image.file, sizeof r->files) ||
                    !(e->tags.present & 1u) || (e->tags.ambiguous & 1u)) continue;
                for (unsigned j = 0; j < r->count; j++)
                    if (r->retimed & (1u << j)) {
                        r->tags[j] = e->tags.value[0];
                        r->retimed &= ~(1u << j);
                    }
            }
        }
    }
}
static unsigned ss_availability(int rc) {
    if (rc == AZ_UNSUPPORTED || rc == AZ_NOT_FOUND) return AZ_AVAIL_UNSUPPORTED;
    if (rc == AZ_STALE) return AZ_AVAIL_STALE;
    return AZ_AVAIL_FAILED;
}
/* A confirmed writer can exempt other fields only if all PMAI framing and
 * bytes outside the component that writer owns remain identical. */
static int ss_sibling_digest(unsigned lane, const uint8_t *data, size_t size, char digest[65]) {
    if (!data || size < 12 || size > ST_FILE_MAX || memcmp(data, "PMAI", 4) || hc_be32(data + 8) != size) return 0;
    size_t at = hc_be32(data + 4);
    if (at < 12 || at > size) return 0;
    azsha256_ctx hash;
    azsha256_init(&hash);
    azsha256_update(&hash, data, 8);
    azsha256_update(&hash, data + 12, at - 12);
    while (at < size) {
        if (size - at < 12) return 0;
        const uint8_t *tag = data + at;
        size_t h = hc_be32(tag + 4), n = hc_be32(tag + 8);
        if (h < 12 || h > n || n > size - at) return 0;
        if (lane == SS_SAVED_GRID && !memcmp(tag, "PQTZ", 4) && h >= 24) {
            /* The stock offset writer only changes this signed millisecond field. */
            azsha256_update(&hash, tag, 18);
            azsha256_update(&hash, tag + 20, n - 20);
        } else if (lane != SS_SAVED_CUES || (memcmp(tag, "PCOB", 4) && memcmp(tag, "PCO2", 4))) {
            azsha256_update(&hash, tag, n);
        }
        at += n;
    }
    azsha256_final_hex(&hash, digest);
    return 1;
}
static void ss_decode_image(const struct ss_image *im, uint64_t topics, struct ss_parse *out,
                            uint8_t *const buf[2], const size_t sizes[2], const int rc[2]) {
    struct st_data *d = &out->data;
    st_data_init(d);
    d->beat_ticks = out->grid_ticks;
    d->cue_ticks = out->cue_ticks;
    const uint8_t *tag_inputs[] = {buf[0], buf[1]};
    if (!fm_scan(tag_inputs, sizes, &out->map_tags, NULL, 0)) out->map_tags.ambiguous = 7;
    for (unsigned f = 0; f < 2; f++)
        if (rc[f] == AZ_OK)
            for (unsigned l = 0; l < SS_SAVED_LANES; l++)
                ss_sibling_digest(l, buf[f], sizes[f], out->siblings[l][f]);
    const struct {
        uint64_t topic;
        uint32_t mask;
        unsigned containers;
        az_component_info *info;
        uint32_t *availability;
    } components[] = {
        {SS_TOPIC(GRID), AZ_DATA_GRID, 1, &d->view.grid_info, &out->grid},
        {SS_TOPIC(PHRASES), AZ_DATA_PHRASES, 2, &d->view.phrases_info, &out->phrases},
        {SS_TOPIC(CUES), AZ_DATA_CUES, 3, NULL, NULL},
        {SS_TOPIC(OVERVIEW), AZ_DATA_WAVEFORM, 1, &d->view.waveform_info, &out->overview}};
    for (unsigned i = 0; i < 4; i++) {
        if (!(topics & components[i].topic)) continue;
        int result = im->rc;
        for (int f = 0; f < 2; f++) {
            if (!(components[i].containers & (1u << f))) continue;
            if (rc[f] && rc[f] != AZ_UNSUPPORTED) result = rc[f];
        }
        const uint8_t *inputs[2] = {buf[0], buf[1]};
        if (components[i].mask == AZ_DATA_CUES) {
            /* Hot and memory containers have their own declarations and payloads.
             * Keep one malformed list from hiding its independently decodable peer. */
            for (int hot = 1; hot >= 0; hot--) {
                d->view.cue_count = 0;
                d->view.cues_info.availability = AZ_AVAIL_UNSUPPORTED;
                int list_rc = result;
                const uint8_t *selected[ST_CUES] = {0};
                if (!list_rc) list_rc = st_parse_origins(d, inputs, sizes, AZ_DATA_CUES, 1u << hot, selected);
                if (list_rc) out->origin.failed = 1;
                else fo_scan_list(&out->origin, inputs, sizes, (unsigned) hot, selected, d);
                out->cue_availability[hot] = list_rc ? ss_availability(list_rc) : d->view.cues_info.availability;
                if (hot && out->cue_availability[hot] == AZ_AVAIL_READY) {
                    out->hot_count = d->view.cue_count; /* unique slots 1..8 after parser validation */
                    memcpy(out->hot, d->cues, out->hot_count * sizeof out->hot[0]);
                    memcpy(out->hot_ticks, out->cue_ticks, 2 * out->hot_count * sizeof out->hot_ticks[0]);
                }
            }
            continue;
        }
        if (!result) result = st_parse_component(d, inputs, sizes, components[i].mask);
        *components[i].availability = result ? ss_availability(result) : components[i].info->availability;
        if (components[i].mask == AZ_DATA_GRID && out->grid == AZ_AVAIL_READY && d->grid_offset_ms) {
            double offset = d->grid_offset_ms / 1000.0;
            for (unsigned j = 0; j < d->view.beat_count; j++) {
                d->beats[j].time_s += offset;
                out->grid_ticks[j] += d->grid_offset_ms;
            }
        }
    }
    if (out->overview == AZ_AVAIL_READY) {
        if (im->metadata.valid & AZ_META_DURATION)
            d->view.waveform.seconds_per_sample = im->metadata.duration_s / d->view.waveform.sample_count;
        else out->overview = AZ_AVAIL_UNSUPPORTED;
    }
}
static void ss_parse_image(const struct ss_image *im, uint64_t topics, struct ss_parse *out) {
    uint8_t *buf[2] = {NULL, NULL};
    size_t sizes[2] = {0};
    int rc[2] = {AZ_UNSUPPORTED, AZ_UNSUPPORTED};
    for (int f = 0; f < 2; f++) {
        if (!(topics & (f ? SS_EXT : SS_DAT))) continue;
        const struct ss_file *file = &im->file[f];
        rc[f] = im->rc ? im->rc : file->rc;
        if (rc[f] != AZ_OK) continue;
        if (st_disk_sig(file->path) != file->signature) {
            rc[f] = AZ_STALE;
            continue;
        }
        buf[f] = st_read_file_checked(file->path, &sizes[f], file->device, file->inode, file->signature);
        if (!buf[f]) rc[f] = AZ_FAILED;
        if (st_disk_sig(file->path) != file->signature) rc[f] = AZ_STALE;
    }
    ss_decode_image(im, topics, out, buf, sizes, rc);
    free(buf[0]);
    free(buf[1]);
}
struct ss_fields {
    struct ds_field field[16];
    unsigned n;
    uint64_t observed;
    const struct ss_binding *binding;
};
static void ss_field(struct ss_fields *p, unsigned topic, unsigned field, unsigned source,
                     uint32_t availability, const void *value, unsigned count) {
    const struct ss_binding *b = p->binding;
    struct ds_field *f = &p->field[p->n++];
    f->topic = topic;
    f->field = field;
    f->value = availability == AZ_AVAIL_READY ? value : NULL;
    f->info = (az_status_field_info_v1) {
        .info = {availability, AZ_PROV_FILE, b->revision[topic], p->observed},
        .scope = AZ_STATUS_LOAD_SCOPE,
        .scope_id = b->epoch,
        .provider_id = b->provider[source],
        .count = availability == AZ_AVAIL_READY ? count : 0};
}
static unsigned ss_meta_status(const struct ss_binding *b, unsigned bit) {
    if (b->image.rc) return ss_availability(b->image.rc);
    return (b->image.metadata.valid & bit) ? AZ_AVAIL_READY : AZ_AVAIL_UNSUPPORTED;
}
static int ss_publish_image(unsigned deck, const struct ss_binding *b, uint64_t topics,
                            struct ss_parse *parsed, uint64_t observed) {
    struct ss_fields p = {.observed = observed ? observed : st_now(), .binding = b};
    const az_track_metadata *m = &b->image.metadata;
    const double precision = 1.0; /* exportLibrary's integral duration is in seconds */
    if (topics & SS_TOPIC(METADATA)) {
        ss_field(&p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_CONTENT_ID, 0,
                 b->image.rc ? ss_availability(b->image.rc) : AZ_AVAIL_READY, &b->ref.track_id, 1);
        const char *values[] = {m->title, m->artist, m->album, m->path, m->format};
        const unsigned bits[] = {AZ_META_TITLE, AZ_META_ARTIST, AZ_META_ALBUM, AZ_META_PATH, AZ_META_FORMAT};
        for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++)
            ss_field(&p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE + i, 0,
                     ss_meta_status(b, bits[i]), values[i], (unsigned) strlen(values[i]));
        ss_field(&p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S, 0,
                 ss_meta_status(b, AZ_META_DURATION), &m->duration_s, 1);
        ss_field(&p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S, 0,
                 ss_meta_status(b, AZ_META_DURATION), &precision, 1);
    }
    if (topics & SS_TOPIC(KEY))
        ss_field(&p, AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL, 0,
                 ss_meta_status(b, AZ_META_KEY), m->key, (unsigned) strlen(m->key));
    if (topics & SS_TOPIC(TEMPO))
        ss_field(&p, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM, 0,
                 ss_meta_status(b, AZ_META_TEMPO), &m->original_bpm, 1);
    const struct st_data *d = parsed ? &parsed->data : NULL;
    if (topics & SS_TOPIC(GRID))
        ss_field(&p, AZ_STATUS_GRID, AZ_STATUS_GRID_SAVED, 1, d ? parsed->grid : AZ_AVAIL_FAILED,
                 d ? d->beats : NULL, d ? d->view.beat_count : 0);
    if (topics & SS_TOPIC(PHRASES))
        ss_field(&p, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED, 2, d ? parsed->phrases : AZ_AVAIL_FAILED,
                 d ? d->phrases : NULL, d ? d->view.phrase_count : 0);
    if (topics & SS_TOPIC(CUES)) {
        ss_field(&p, AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_HOT, 3,
                 d ? parsed->cue_availability[1] : AZ_AVAIL_FAILED,
                 d ? parsed->hot : NULL, d ? parsed->hot_count : 0);
        ss_field(&p, AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_MEMORY, 3,
                 d ? parsed->cue_availability[0] : AZ_AVAIL_FAILED,
                 d ? d->cues : NULL, d ? d->view.cue_count : 0);
    }
    if (topics & SS_TOPIC(OVERVIEW))
        ss_field(&p, AZ_STATUS_OVERVIEW, AZ_STATUS_OVERVIEW_SAVED, 1, d ? parsed->overview : AZ_AVAIL_FAILED,
                 d ? &d->view.waveform : NULL, 1);
    const int64_t precision_tick = 1;
    const struct ds_time_axis duration_clock = {1, &b->image.duration_ticks, 1, 1, NULL};
    const struct ds_time_axis precision_clock = {1, &precision_tick, 1, 1, NULL};
    const struct ds_time_axis hot_clock = {1000, parsed ? parsed->hot_ticks : NULL, 1, 1000, NULL};
    const struct ds_time_axis memory_clock = {1000, parsed ? parsed->cue_ticks : NULL, 1, 1000, NULL};
    const struct ds_time_axis grid_clock = {1000, parsed ? parsed->grid_ticks : NULL, 1, 1000, NULL};
    /* Preserve the original integral extent. PWAV calibration divides that
     * source duration by the column count only in the public waveform value. */
    for (unsigned i = 0; i < p.n; i++) {
        struct ds_field *field = &p.field[i];
        if (field->info.info.availability != AZ_AVAIL_READY) continue;
        if (field->topic == AZ_STATUS_METADATA &&
            (field->field == AZ_STATUS_METADATA_DURATION_S || field->field == AZ_STATUS_METADATA_DURATION_PRECISION_S))
            field->clock = field->field == AZ_STATUS_METADATA_DURATION_S ? &duration_clock : &precision_clock;
        else if (field->topic == AZ_STATUS_GRID) field->clock = &grid_clock;
        else if (field->topic == AZ_STATUS_CUES)
            field->clock = field->field == AZ_STATUS_CUES_SAVED_HOT ? &hot_clock : &memory_clock;
        else if (field->topic == AZ_STATUS_OVERVIEW) field->clock = &duration_clock;
    }
    return ds_publish_file(deck, b->epoch, &b->ref, p.observed, p.field, p.n, 0, 0);
}
static int ss_publish_baseline(unsigned deck, const struct ss_binding *b, uint64_t topics, struct ss_parse *parsed) {
    if (!atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) return ss_publish_image(deck, b, topics, parsed, 0);
    struct ds_proof_context previous = ds_proof_context;
    ds_proof_context = ss_proof_context(b, DS_PROOF_BASELINE, 0);
    ds_proof_context.topics = topics;
    int rc = ss_publish_image(deck, b, topics, parsed, 0);
    ds_proof_context = previous;
    return rc;
}
static int ss_file_current(const struct st_source *src, const struct ss_file *f) {
    if (!f->relative[0]) return 1;
    char path[1024] = "";
    st_source_path(src->mount, f->relative, path);
    return !strcmp(path, f->path) && (!path[0] || st_disk_sig(path) == f->signature);
}
static int ss_pin_witness(uint64_t token, struct azjs_origin *origin, struct azsdk_source_sample *native);
/* Only st_worker removes jobs. Native writers append under ss_mu. Each
 * edition is read and decoded once for its interested loads. An unavailable
 * native witness fences later editions of that deck without blocking peers. */
static void ss_drain_saved(void) {
    unsigned blocked[SS_SAVED_LANES] = {0}, index = 0;
    for (unsigned n = 0; n < SS_SAVE_QUEUE; n++) {
        pthread_mutex_lock(&ss_mu);
        if (index >= ss_saved_count) {
            pthread_mutex_unlock(&ss_mu);
            break;
        }
        struct ss_saved_revision *q = ss_saved_queue[index];
        unsigned lane = q->lane, topic = ss_saved_topic[lane], provider = ss_saved_provider[lane];
        uint64_t topics = AZ_STATUS_TOPIC(topic);
        unsigned candidates = q->targets & ~blocked[lane];
        pthread_mutex_unlock(&ss_mu);
        if (!candidates) {
            ++index;
            continue;
        }
        struct st_source source;
        int mounted = !ss_source(&q->ref, &source);
        struct ss_parse *parsed = mounted ? calloc(1, sizeof *parsed) : NULL;
        if (parsed) {
            const struct ss_image committed = {0};
            ss_decode_image(&committed, topics, parsed, q->bytes, q->sizes, q->rc);
        }
        pthread_mutex_lock(&ss_mu);
        for (unsigned deck = 0; deck < 4; deck++) {
            unsigned bit = 1u << deck;
            if (!(candidates & bit)) continue;
            struct ss_binding *b = &ss_decks[deck];
            if (b->cut != q->target[deck].cut || b->epoch != q->target[deck].epoch ||
                b->interest[lane] != q->target[deck].interest || !st_ref_equal(&b->ref, &q->ref)) {
                q->targets &= ~bit;
                continue;
            }
            int live = mounted;
            uint64_t pin = q->target[deck].source_pin;
            if (live && pin) {
                struct azjs_origin origin = {0};
                struct azsdk_source_sample native = {0};
                int witness = ss_pin_witness(pin, &origin, &native);
                if (witness == AZ_BUSY) {
                    blocked[lane] |= bit;
                    continue;
                }
                live = witness == AZ_OK && ss_pins[deck].token == pin && !ss_pins[deck].retired &&
                       origin.track_id == q->ref.track_id && native.device == source.device &&
                       native.inode == source.inode && native.mount_id == source.mount_id;
            }
            if (b->pinned[lane]) --b->pinned[lane];
            q->targets &= ~bit;
            if (b->retired || !(b->topics & topics)) continue;
            if (live && parsed) {
                struct ss_binding cut = {.epoch = b->epoch, .ref = q->ref};
                cut.provider[provider] = q->target[deck].provider;
                cut.revision[topic] = q->revision;
                ss_publish_image(deck, &cut, topics, parsed, q->observed);
            } else ds_publish_file(deck, b->epoch, &q->ref, q->observed, NULL, 0, topics, AZ_STATUS_REVISION_LOST);
        }
        int finished = !q->targets;
        if (finished) {
            --ss_saved_count;
            memmove(ss_saved_queue + index, ss_saved_queue + index + 1, (ss_saved_count - index) * sizeof *ss_saved_queue);
            ss_saved_bytes -= q->total;
        } else ++index;
        pthread_mutex_unlock(&ss_mu);
        free(parsed);
        if (finished) ss_release_saved(q);
    }
}
static void ss_work(void) {
    ss_drain_saved();
    struct ss_binding jobs[4];
    pthread_mutex_lock(&ss_mu);
    memcpy(jobs, ss_decks, sizeof jobs);
    pthread_mutex_unlock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) {
        if (!jobs[deck].pending || !jobs[deck].ready || jobs[deck].retired) continue;
        /* Two decks playing the same qualified reference and revision share one parse. */
        unsigned members = 0;
        uint64_t topics = 0;
        for (unsigned i = deck; i < 4; i++)
            if (jobs[i].pending && jobs[i].ready && !jobs[i].retired &&
                st_ref_equal(&jobs[deck].ref, &jobs[i].ref) &&
                ss_image_same(&jobs[deck].image, &jobs[i].image)) {
                members |= 1u << i;
                topics |= jobs[i].pending;
            }
        struct ss_parse *parsed = NULL;
        if (topics & (SS_DAT | SS_EXT)) {
            parsed = calloc(1, sizeof *parsed);
            if (parsed) ss_parse_image(&jobs[deck].image, topics, parsed);
        }
        struct st_source current;
        if (ss_source(&jobs[deck].ref, &current)) {
            free(parsed);
            continue;
        }
        uint64_t stale = 0;
        if ((topics & SS_DAT) && !ss_file_current(&current, &jobs[deck].image.file[0])) stale |= SS_DAT;
        if ((topics & SS_EXT) && !ss_file_current(&current, &jobs[deck].image.file[1])) stale |= SS_EXT;
        /* Let the monitor order the path change and any loss before this
         * component can be published. Other containers and metadata still proceed. */
        for (unsigned i = deck; i < 4; i++) {
            if (!(members & (1u << i))) continue;
            pthread_mutex_lock(&ss_mu);
            struct ss_binding *d = &ss_decks[i];
            uint64_t token = ss_pins[i].epoch == d->epoch ? ss_pins[i].token : 0;
            int source_live = !token || !ss_pins[i].retired;
            if (token && source_live) {
                struct azjs_origin origin = {0};
                struct azsdk_source_sample native = {0};
                source_live = ss_pin_witness(token, &origin, &native) == AZ_OK && origin.track_id == d->ref.track_id &&
                              native.device == current.device && native.inode == current.inode && native.mount_id == current.mount_id;
            }
            uint64_t publish = jobs[i].pending & d->pending & d->topics & ~stale;
            /* A busy source can leave earlier immutable revisions queued.
             * Never publish a current file image ahead of their callbacks. */
            for (unsigned l = 0; l < SS_SAVED_LANES; l++)
                if (d->pinned[l]) publish &= ~AZ_STATUS_TOPIC(ss_saved_topic[l]);
            if (source_live && !d->writing && d->cut == jobs[i].cut && d->serial == jobs[i].serial && !d->retired && publish &&
                ss_publish_baseline(i, d, publish, parsed) == AZ_OK) {
                d->pending &= ~publish;
                fm_update(i, d, publish, parsed);
                if (publish & SS_TOPIC(CUES)) {
                    struct fo_edition *ed = &fo_editions[i];
                    ed->cut = d->cut;
                    ed->provider = d->provider[3];
                    ed->revision = d->revision[AZ_STATUS_CUES];
                    if (parsed) ed->scan = parsed->origin;
                    else ed->scan.failed = 1;
                    azfile_native_refresh(i, d->epoch);
                }
                if (parsed)
                    for (unsigned f = 0; f < 2; f++)
                        for (unsigned l = 0; l < SS_SAVED_LANES; l++)
                            if (parsed->siblings[l][f][0]) memcpy(d->siblings[l][f], parsed->siblings[l][f], sizeof d->siblings[l][f]);
            }
            pthread_mutex_unlock(&ss_mu);
            jobs[i].pending = 0;
        }
        free(parsed);
    }
    pthread_mutex_lock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) fm_publish(deck);
    pthread_mutex_unlock(&ss_mu);
}
static int ss_wake_pipe[2] = {-1, -1};
static void ss_signal(void) {
    if (ss_wake_pipe[1] < 0) return;
    char byte = 0;
    ssize_t n = write(ss_wake_pipe[1], &byte, 1);
    (void) n;
}
/* Match the actual native SQLCipher object's open file to one current USB
 * physical port. No media-number or content-ID fallback. A live WAL is read
 * through SQLite before the new LOAD.track becomes observable. */
static int ss_pin_witness(uint64_t token, struct azjs_origin *origin, struct azsdk_source_sample *native) {
    if (!azjs_lookup || !azsdk_sources_status_read) return AZ_STALE;
    int found = azjs_lookup(token, origin);
    /* A committed pack token and a retired owner epoch are never reused.
     * In contrast, an open connection can still acquire its physical witness. */
    if (found <= 0) return found < 0 ? AZ_BUSY : AZ_CANCELLED;
    int rc = azsdk_sources_status_read(origin->owner, native, sizeof *native);
    if (rc == AZS_BUSY) return AZ_BUSY;
    if (rc == AZS_STALE || rc == AZS_UNKNOWN) return AZ_CANCELLED;
    unsigned need = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_PHYSICAL;
    if (rc != AZS_OK || (native->flags & need) != need || native->physical_status != AZS_PHYSICAL_OK ||
        !origin->track_id || !native->mount_id || !native->inode) return AZ_STALE;
    return AZ_OK;
}
static int ss_pin_identity(uint64_t token, struct st_source *src, az_track_ref *ref) {
    struct azjs_origin origin = {0};
    struct azsdk_source_sample native = {0};
    int rc = ss_pin_witness(token, &origin, &native);
    if (rc) return rc;
    int count = 0;
    for (int port = 0; port < 2; port++) {
        struct st_source local;
        if (!st_source_read(port, &local) && local.mount_id == native.mount_id &&
            local.device == native.device && local.inode == native.inode) {
            *src = local;
            *ref = local.ref;
            ref->track_id = origin.track_id;
            count++;
        }
    }
    return count == 1 ? AZ_OK : AZ_STALE;
}
static int ss_resolve_pin(unsigned deck) {
    pthread_mutex_lock(&ss_mu);
    uint64_t epoch = ss_pins[deck].epoch, token = ss_pins[deck].token;
    int published = ss_pins[deck].published;
    az_track_ref old = ss_pins[deck].ref;
    int interested = ss_decks[deck].topics != 0 && !ss_pins[deck].retired;
    pthread_mutex_unlock(&ss_mu);
    if (!epoch || !token || !interested) return 0;
    struct st_source source;
    az_track_ref ref = {0};
    int rc = ss_pin_identity(token, &source, &ref);
    if (rc == AZ_OK && published && st_ref_equal(&ref, &old)) return 1;
    /* A connection/filesystem observation can be temporarily incomplete. A
     * later proof may recover the original incarnation, never a replacement
     * port/source or row on this pack token. */
    if (old.track_id && rc != AZ_BUSY) {
        struct st_source pinned;
        if (ss_source(&old, &pinned) || (rc == AZ_OK && !st_ref_equal(&old, &ref)))
            rc = AZ_CANCELLED;
    }
    if (rc == AZ_OK && !published) {
        char path[1024];
        rc = AZ_STALE;
        if (st_source_path(source.mount, "/PIONEER/rekordbox/exportLibrary.db", path)) {
            int fd = -1;
            void *db = ss_open_database(path, &source, &fd);
            if (db) {
                struct st_resolution row = {0};
                uint64_t before = 0, after = 0;
                rc = ss_database_version(db, &before);
                if (!rc) rc = st_read_row(db, ref.track_id, &row);
                if (ss_database_version(db, &after) || before != after) rc = AZ_BUSY;
                ss_close_database(db, fd);
                az_track_ref checked = {0};
                if (!rc && (ss_pin_identity(token, &source, &checked) || !st_ref_equal(&ref, &checked))) rc = AZ_STALE;
            }
        }
    }
    if (rc == AZ_BUSY) return 1;
    pthread_mutex_lock(&ss_mu);
    int current = ss_pins[deck].epoch == epoch && ss_pins[deck].token == token && ss_decks[deck].epoch == epoch;
    pthread_mutex_unlock(&ss_mu);
    if (!current) return 0;
    if (published) {
        (void) ds_retire_file_binding(deck, epoch, &old);
    } else if (!rc) {
        int32_t port = (int32_t) ref.source_id;
        uint64_t now = st_now();
        az_status_field_info_v1 info = {{AZ_AVAIL_READY, AZ_PROV_DERIVED, token, now},
                                        AZ_STATUS_LOAD_SCOPE,
                                        1,
                                        epoch,
                                        token};
        struct ds_field fields[] = {{AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, info, &ref, NULL},
                                    {AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, info, &port, NULL}};
        if (ds_publish(deck, epoch, 0, now, fields, 2, NULL, NULL, 0, 0) == AZ_OK) {
            pthread_mutex_lock(&ss_mu);
            if (ss_pins[deck].epoch == epoch && ss_pins[deck].token == token) {
                ss_pins[deck].ref = ref;
                ss_pins[deck].published = 1;
            }
            pthread_mutex_unlock(&ss_mu);
        }
    }
    if (published || rc == AZ_CANCELLED) {
        pthread_mutex_lock(&ss_mu);
        if (ss_pins[deck].epoch == epoch && ss_pins[deck].token == token) {
            ss_pins[deck].retired = rc == AZ_CANCELLED;
            ss_pins[deck].published = 0;
        }
        pthread_mutex_unlock(&ss_mu);
    }
    return 1;
}
static void ss_native_load(unsigned deck, uint64_t epoch, uint64_t token) {
    pthread_mutex_lock(&ss_mu);
    if (ss_pins[deck].epoch != epoch || ss_pins[deck].token != token) {
        memset(&ss_pins[deck], 0, sizeof ss_pins[deck]);
        ss_pins[deck].epoch = epoch;
        ss_pins[deck].token = token;
    }
    pthread_mutex_unlock(&ss_mu);
    if (atomic_load(&g_st_status_jobs)) ss_signal();
}
static void *ss_observe(void *unused) {
    (void) unused;
    for (;;) {
        int binding = 0;
        for (unsigned i = 0; i < 4; i++) {
            ss_monitor_deck(i);
            binding |= ss_resolve_pin(i);
        }
        struct pollfd fds[5] = {{ss_wake_pipe[0], POLLIN, 0}};
        unsigned n = 1;
        int active = binding;
        for (unsigned i = 0; i < 4; i++) {
            if (!ss_monitors[i].seeded) continue;
            active = 1;
            if (ss_monitors[i].fd >= 0) fds[n++] = (struct pollfd) {ss_monitors[i].fd, POLLIN, 0};
        }
        (void) poll(fds, n, active ? 100 : -1);
        if (fds[0].revents & POLLIN) {
            char bytes[64];
            ssize_t received = read(fds[0].fd, bytes, sizeof bytes);
            (void) received;
        }
    }
    return NULL;
}
static int ss_open_wakeup(void) {
    int fd[2];
#ifdef __linux__
    if (pipe2(fd, O_NONBLOCK | O_CLOEXEC)) return 0;
#else
    if (pipe(fd)) return 0; /* cloexec-ok: non-Linux host fixture, both descriptors configured before publication */

    for (int i = 0; i < 2; i++) {
        if (fcntl(fd[i], F_SETFL, O_NONBLOCK) || fcntl(fd[i], F_SETFD, FD_CLOEXEC)) {
            close(fd[0]);
            close(fd[1]);
            return 0;
        }
    }
#endif
    memcpy(ss_wake_pipe, fd, sizeof fd);
    return 1;
}
static int ss_start(void) {
    pthread_mutex_lock(&ss_start_mu);
    if (!ss_started && g_sdk_sq.ok > 0 && !st_start_worker() && ss_open_wakeup()) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        struct sched_param priority = {0};
        pthread_attr_setschedparam(&attr, &priority);
        if (!az_thread_create(&thread, &attr, ss_observe, NULL)) {
            atomic_store(&g_st_status_jobs, ss_work);
            ss_started = 1;
        } else {
            close(ss_wake_pipe[0]);
            close(ss_wake_pipe[1]);
            ss_wake_pipe[0] = ss_wake_pipe[1] = -1;
        }
        pthread_attr_destroy(&attr);
    }
    int started = ss_started;
    pthread_mutex_unlock(&ss_start_mu);
    return started;
}
/* Once a component is no longer watched, a future registration must not replay
 * a READY field left over from an interval with no commit observation. The
 * reducer preserves native siblings when withdrawing our file projection. */
static void ss_withdraw(unsigned deck, struct ss_binding *b, uint64_t topics) {
    if (!topics || !b->ready || b->retired || !ss_local(&b->ref)) return;
    static const uint64_t fields[AZ_STATUS_TOPIC_COUNT] = {
        [AZ_STATUS_METADATA] = 0xffu,
        [AZ_STATUS_KEY] = 1u << AZ_STATUS_KEY_ORIGINAL,
        [AZ_STATUS_TEMPO] = 1u << AZ_STATUS_TEMPO_ORIGINAL_BPM,
        [AZ_STATUS_CUES] = (1u << AZ_STATUS_CUES_SAVED_HOT) | (1u << AZ_STATUS_CUES_SAVED_MEMORY),
        [AZ_STATUS_GRID] = 1u << AZ_STATUS_GRID_SAVED,
        [AZ_STATUS_PHRASES] = 1u << AZ_STATUS_PHRASES_SAVED,
        [AZ_STATUS_OVERVIEW] = 1u << AZ_STATUS_OVERVIEW_SAVED};
    struct ss_fields p = {.binding = b, .observed = st_now()};
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(topics & AZ_STATUS_TOPIC(t))) continue;
        unsigned source = t == AZ_STATUS_CUES ? 3 : t == AZ_STATUS_PHRASES                         ? 2
                                                : (t == AZ_STATUS_GRID || t == AZ_STATUS_OVERVIEW) ? 1
                                                                                                   : 0;
        for (unsigned f = 0; f < 8; f++)
            if (fields[t] & (1u << f)) ss_field(&p, t, f, source, AZ_AVAIL_STALE, NULL, 0);
    }
    if (p.n) ds_publish_file(deck, b->epoch, &b->ref, p.observed, p.field, p.n, 0, 0);
}
static void sdk_status_store_interest(unsigned deck, uint64_t topics) {
    if (deck >= 4) return;
    topics = ss_topics(topics);
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *d = &ss_decks[deck];
    ss_withdraw(deck, d, d->topics & ~topics);
    for (unsigned l = 0; l < SS_SAVED_LANES; l++)
        if ((d->topics ^ topics) & AZ_STATUS_TOPIC(ss_saved_topic[l])) {
            d->interest[l] = ss_new_id(&ss_serial);
            d->pinned[l] = 0;
        }
    d->pending |= topics & ~d->topics;
    d->pending &= topics;
    d->topics = topics;
    pthread_mutex_unlock(&ss_mu);
    if ((topics || atomic_load(&g_st_status_jobs)) && ss_start()) {
        ss_signal();
        st_worker_wake();
    }
}
/* A native-preferred field was withdrawn. Re-offer the qualified file projection
 * only for this load and active interest, through the normal freshness checks. */
static void sdk_status_store_reseed(unsigned deck, uint64_t epoch, uint64_t topics) {
    if (deck >= 4 || !epoch) return;
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *d = &ss_decks[deck];
    int wake = d->epoch == epoch && d->ready && !d->retired && (d->topics & ss_topics(topics));
    if (wake) d->pending |= d->topics & ss_topics(topics);
    pthread_mutex_unlock(&ss_mu);
    if (wake) st_worker_wake();
}
static void sdk_status_store_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t cut) {
    if (deck >= 4) return;
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *d = &ss_decks[deck];
    if (cut > d->cut) {
        fm_binding(deck, epoch);
        uint64_t topics = d->topics;
        memset(d, 0, sizeof *d);
        d->topics = topics;
        d->cut = cut;
        d->epoch = epoch;
        if (epoch && ss_local(ref)) d->ref = *ref;
    }
    pthread_mutex_unlock(&ss_mu);
    if (atomic_load(&g_st_status_jobs)) ss_signal();
}

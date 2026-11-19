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
    struct ds_proof_context p = {.kind = kind, .cause = cause, .binding_cut = b->cut, .binding_serial = b->serial, .connection = b->proof_connection, .db_before = b->proof_version, .db_after = b->proof_version, .device = b->image.src.device, .inode = b->image.src.inode, .mount_id = b->image.src.mount_id};
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
static void ss_format_result(void *stmt, az_track_metadata *m, const char *const *names, size_t count) {
    long long type = g_sdk_sq.col_int64(stmt, 0);
    if (type >= 0 && (uint64_t) type < count && names[type] && g_sdk_sq.step(stmt) == 101) {
        snprintf(m->format, sizeof m->format, "%s", names[type]);
        m->valid |= AZ_META_FORMAT;
    }
}

static void ss_read_format(void *db, uint32_t track_id, az_track_metadata *m) {
    static const char *const names[] = {
        [1] = "MP3", [3] = "MP4", [4] = "AAC", [5] = "FLAC", [6] = "ALAC", [11] = "WAV", [12] = "AIFF"};
    void *stmt = NULL;
    const char *sql = "SELECT fileType FROM content WHERE content_id=?1 AND typeof(fileType)='integer'";
    if (!g_sdk_sq.prepare(db, sql, -1, &stmt, NULL) && stmt && !g_sdk_sq.bind_int64(stmt, 1, track_id) &&
        g_sdk_sq.step(stmt) == 100) {
        ss_format_result(stmt, m, names, sizeof names / sizeof names[0]);
    }
    if (stmt) g_sdk_sq.finalize(stmt);
}
static void ss_file_stat(const struct st_source *src, struct ss_file *file) {
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
    ss_file_stat(src, file);
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
static void ss_track_write_event(struct ss_monitor *m, unsigned f, unsigned mask) {
    if (mask & IN_MODIFY) m->open_edits |= 1u << f;
    if (mask & IN_CLOSE_WRITE) {
        ++m->close_count[f];
        m->open_edits &= ~(1u << f);
    }
    if (mask & ~(IN_MODIFY | IN_CLOSE_WRITE)) m->unsafe_events |= 1u << f;
    if (mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE)) m->uncommitted_file[f] = 1;
    if (mask & IN_MOVED_TO) m->uncommitted_file[f] = 0;
}

static uint64_t ss_file_event(struct ss_monitor *m, unsigned f, const struct inotify_event *e) {
    uint64_t affected = 0;
    const char *base = strrchr(m->paths[f], '/');
    if (!e->len || (base && !strcmp(base + 1, e->name))) {
                    /* Linux reports CLOSE_WRITE even for O_RDWR descriptors
                     * that never wrote a byte. Pair closes with MODIFY across
                     * drain calls, including drains interrupted by a stock writer. */
        if ((e->mask & IN_CLOSE_WRITE) && !(m->open_edits & (1u << f))) return 0;
        ss_track_write_event(m, f, e->mask);
        affected = f ? SS_EXT : SS_DAT;
    }
    if (e->mask & IN_IGNORED) m->wd[f] = -1;
    return affected;
}

static uint64_t ss_inotify_record(struct ss_monitor *m, const struct inotify_event *e) {
    uint64_t affected = 0;
    if (e->mask & IN_Q_OVERFLOW) {
        affected |= SS_ALL;
        m->uncommitted_file[0] = m->uncommitted_file[1] = 1;
    }
    for (int f = 0; f < 2; f++) {
        if (e->wd != m->wd[f]) continue;
        affected |= ss_file_event(m, (unsigned) f, e);
    }
    return affected;
}

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
            affected |= ss_inotify_record(m, e);
        }
    }
    return affected | SS_ALL;
}
static void ss_reset_watches(struct ss_monitor *m, const struct ss_file files[2]) {
    if (m->fd < 0 || strcmp(m->paths[0], files[0].path) || strcmp(m->paths[1], files[1].path)) {
        if (m->fd >= 0) close(m->fd);
        for (int f = 0; f < 2; f++)
            if (strcmp(m->paths[f], files[f].path)) m->uncommitted_file[f] = 0;
        m->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        m->wd[0] = m->wd[1] = -1;
    }
}

static void ss_arm_paths(struct ss_monitor *m, const struct ss_file files[2]) {
    ss_reset_watches(m, files);
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
struct ss_observation {
    unsigned deck, proof_cause;
    struct ss_binding before;
    struct ss_binding *current;
    struct ss_monitor *monitor;
    struct ss_image image;
    uint64_t filesystem_events, confirmed[2], rank[2], gap, proof_before, known[2], version, touched;
    int source_rc, row_retired, changed[2];
};
static int ss_monitor_ready(struct ss_observation *o) {
    if (!o->before.topics || !o->before.ref.track_id || o->before.retired) {
        if (o->monitor->seeded) ss_monitor_reset(o->monitor);
        return 0;
    }
    if (!o->monitor->seeded || o->monitor->cut != o->before.cut) {
        ss_monitor_reset(o->monitor);
        o->monitor->seeded = 1;
        o->monitor->cut = o->before.cut;
    }
    if (o->before.writing) {
        struct st_source live;
        if (!ss_source(&o->before.ref, &live)) return 0;
    }
    return 1;
}

static void ss_begin_observation(struct ss_observation *o) {
    o->monitor->pending_events |= ss_drain_events(o->monitor);
    o->filesystem_events = o->monitor->pending_events;
    o->gap = o->filesystem_events == SS_ALL ? SS_ALL : o->monitor->pending_db_gap;
    o->proof_cause = o->monitor->proof_pending_cause | (o->filesystem_events == SS_ALL ? DS_PROOF_FILESYSTEM : 0);
    o->proof_before = o->monitor->pending_db_gap ? o->monitor->proof_pending_before : o->monitor->version;
    o->source_rc = ss_source(&o->before.ref, &o->image.src);
    o->image.rc = o->source_rc;
}

static void ss_read_observed_row(struct ss_observation *o) {
    if (!o->monitor->row_ready || o->monitor->version != o->version) {
        memset(&o->monitor->row, 0, sizeof o->monitor->row);
        int ambiguous = 0;
        o->image.rc = st_read_row_checked(o->monitor->db, o->before.ref.track_id, &o->monitor->row, &ambiguous);
        o->row_retired = o->image.rc == AZ_NOT_FOUND || ambiguous;
        if (!o->image.rc) ss_read_format(o->monitor->db, o->before.ref.track_id, &o->monitor->row.metadata);
        o->monitor->row_ready = o->image.rc == AZ_OK;
    } else o->image.rc = AZ_OK;
}

static void ss_observed_metadata(struct ss_observation *o) {
    char path[1024];
    o->image.metadata = o->monitor->row.metadata;
    o->image.duration_ticks = o->monitor->row.duration_ticks;
    if (o->monitor->row.content[0] && st_source_path(o->image.src.mount, o->monitor->row.content, path) && strlen(path) < sizeof o->image.metadata.path) {
        memcpy(o->image.metadata.path, path, strlen(path) + 1);
        o->image.metadata.valid |= AZ_META_PATH;
    }
}

static int ss_writer_isolated(struct ss_observation *o, unsigned f, const struct ss_write_proof *proof) {
    return proof->only && o->monitor->fd >= 0 && o->monitor->wd[f] >= 0 &&
           !(o->monitor->unsafe_events & (1u << f)) && proof->ordinal > o->monitor->confirmed_rank[f] &&
           o->monitor->close_count[f] == proof->ordinal - o->monitor->confirmed_rank[f];
}

static int ss_file_proof_matches(struct ss_observation *o, unsigned f, const struct ss_write_proof *proof, char digest[65]) {
    return o->image.file[f].signature == proof->signature && o->image.file[f].device == proof->device &&
           o->image.file[f].inode == proof->inode && ss_digest(&o->image.file[f], digest) &&
           !memcmp(digest, proof->digest, sizeof proof->digest);
}

static void ss_confirm_file(struct ss_observation *o, unsigned f) {
    const struct ss_write_proof *proof = &o->before.completed[f];
    if (proof->revision && o->monitor->confirmed[f] != proof->revision) {
        char digest[65];
        if (ss_file_proof_matches(o, f, proof, digest)) {
                            /* The native writer closed these exact files. If the
                             * watcher saw another closer, an overflow, different
                             * unrelated bytes or any replacement, retain its o->gap. */
            if (ss_writer_isolated(o, f, proof)) o->known[f] = proof->only;
            o->monitor->uncommitted_file[f] = 0;
            o->confirmed[f] = proof->revision;
            o->rank[f] = proof->ordinal;
        }
    }
}

static int ss_observed_inplace_edit(const struct ss_observation *o, const struct ss_file *old, int f) {
    return o->before.ready && old->signature && old->signature != o->image.file[f].signature &&
           old->inode == o->image.file[f].inode && old->device == o->image.file[f].device;
}

static void ss_observed_files(struct ss_observation *o) {
    for (int f = 0; f < 2; f++) ss_file_probe(&o->image.src, &o->monitor->row, f, &o->image.file[f]);
    ss_arm_paths(o->monitor, o->image.file);
    for (int f = 0; f < 2; f++) {
        const struct ss_file *old = &o->before.image.file[f];
        if (ss_observed_inplace_edit(o, old, f))
            o->monitor->uncommitted_file[f] = 1;
        ss_confirm_file(o, (unsigned) f);
        if (o->monitor->uncommitted_file[f] && o->image.file[f].rc == AZ_OK)
            o->image.file[f].rc = AZ_STALE;
    }
}

static void ss_check_observed_row(struct ss_observation *o) {
    o->monitor->version = o->version;
    uint64_t after = 0;
    if (ss_database_version(o->monitor->db, &after) || after != o->version) {
        o->image.rc = AZ_STALE;
        o->row_retired = 0;
        o->monitor->row_ready = 0;
        o->gap |= SS_ALL;
        o->proof_cause |= DS_PROOF_READ;
    } else if (!o->image.rc) {
        ss_observed_metadata(o);
        ss_observed_files(o);
    }
}

static void ss_observe_database(struct ss_observation *o) {
    char path[1024];
    if (!o->source_rc) {
        o->image.rc = AZ_FAILED;
        if (!o->monitor->db && st_source_path(o->image.src.mount, "/PIONEER/rekordbox/exportLibrary.db", path))
            o->monitor->db = ss_open_database(path, &o->image.src, &o->monitor->db_fd);
        if (o->monitor->db && !ss_database_version(o->monitor->db, &o->version)) {
            if (o->monitor->version && o->monitor->version != o->version) {
                o->gap |= SS_ALL;
                o->proof_cause |= DS_PROOF_DATABASE;
            }
            ss_read_observed_row(o);
            ss_check_observed_row(o);
        }
        struct st_source chk;
        o->source_rc = ss_source(&o->before.ref, &chk);
        if (o->source_rc) o->image.rc = o->source_rc;
    }
}

static void ss_observe_source_loss(struct ss_observation *o) {
    if (o->before.ready && o->source_rc) {
        o->proof_cause |= DS_PROOF_SOURCE;
        /* A normal disconnection is an invalidation. Only charge a capture o->gap
         * if the committed activity token cannot close the interval, the observer
         * already found a file o->gap, or a pinned revision was still owed. */
        uint64_t final_version = 0;
        if (!o->monitor->db || ss_database_version(o->monitor->db, &final_version) || final_version != o->monitor->version) o->gap |= SS_ALL;
        o->gap |= o->before.pending;
        for (unsigned l = 0; l < SS_SAVED_LANES; l++)
            if (o->before.pinned[l]) o->gap |= AZ_STATUS_TOPIC(ss_saved_topic[l]);
    }
}

static int ss_observation_current(struct ss_observation *o) {
    /* A writer can begin and finish while this pass is checking SQLite/files.
     * Its notification may already have been read. Keep both the notification
     * and the previous certificate o->rank for the retry against the new proof. */
    if (o->current->cut != o->before.cut || o->current->retired || !o->current->topics ||
        (!o->source_rc && (o->current->writing || o->current->serial != o->before.serial))) {
        if (o->gap == SS_ALL) {
            o->monitor->pending_db_gap = SS_ALL;
            o->monitor->proof_pending_cause = o->proof_cause;
            o->monitor->proof_pending_before = o->proof_before;
        }
        return 0;
    }
    return 1;
}

static void ss_accept_file_proofs(struct ss_observation *o) {
    o->monitor->pending_db_gap = 0;
    o->monitor->proof_pending_cause = 0;
    o->monitor->pending_events = 0;
    o->monitor->unsafe_events = 0;
    for (unsigned f = 0; f < 2; f++)
        if (o->confirmed[f]) {
            o->monitor->confirmed[f] = o->confirmed[f];
            o->monitor->confirmed_rank[f] = o->rank[f];
        }
}

static int ss_clean_file_change(struct ss_observation *o, int f) {
    return o->known[f] && o->image.rc == AZ_OK && o->current->image.rc == AZ_OK &&
           o->image.file[f].rc == AZ_OK && o->current->image.file[f].rc == AZ_OK;
}

static int ss_file_change_gap(struct ss_observation *o, int f, int clean) {
    return !clean && ((o->filesystem_events & (f ? SS_TOPIC(PHRASES) : SS_TOPIC(GRID))) ||
                      (o->current->ready && o->changed[f] && o->image.rc == AZ_OK && o->current->image.rc == AZ_OK));
}

static void ss_observed_changes(struct ss_observation *o) {
    o->touched = !o->current->ready || o->image.rc != o->current->image.rc ? SS_ALL : 0;
    o->touched |= ss_metadata_changes(&o->current->image, &o->image);
    for (int f = 0; f < 2; f++) {
        if (o->monitor->confirmed[f] > o->current->consumed[f]) o->current->consumed[f] = o->monitor->confirmed[f];
        o->changed[f] = memcmp(&o->current->image.file[f], &o->image.file[f], sizeof o->image.file[f]) != 0;
        uint64_t components = f ? SS_EXT : SS_DAT;
        int clean = ss_clean_file_change(o, f);
        if (o->changed[f]) o->touched |= clean ? o->known[f] : components;
        if (ss_file_change_gap(o, f, clean)) {
            o->gap |= components;
            o->proof_cause |= DS_PROOF_FILE_CHANGE;
        }
    }
}

static void ss_close_file_changes(struct ss_observation *o) {
    if (o->before.ready && !o->source_rc && o->image.rc != AZ_OK && o->current->image.rc == AZ_OK) {
        o->gap |= SS_ALL;
        o->proof_cause |= DS_PROOF_READ;
    }
    if (o->current->ready && (o->changed[0] || o->changed[1] || (o->gap & (SS_DAT | SS_EXT))))
        fo_file_cut(o->deck, o->current, &o->image, o->known, o->monitor->close_count, o->gap);
    memset(o->monitor->close_count, 0, sizeof o->monitor->close_count);
}

static void ss_publish_observed_gap(struct ss_observation *o) {
    if (o->current->ready && (o->gap & o->current->topics) && atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        struct ds_proof_context previous = ds_proof_context;
        ds_proof_context = ss_proof_context(o->current, DS_PROOF_GAP, o->proof_cause);
        ds_proof_context.connection = (uintptr_t) o->monitor->db;
        ds_proof_context.db_before = o->proof_before;
        ds_proof_context.db_after = o->version;
        ds_publish_file(o->deck, o->current->epoch, &o->current->ref, st_now(), NULL, 0, o->gap & o->current->topics, AZ_STATUS_CAPTURE_GAP);
        ds_proof_context = previous;
    } else if (o->current->ready && (o->gap & o->current->topics))
        ds_publish_file(o->deck, o->current->epoch, &o->current->ref, st_now(), NULL, 0, o->gap & o->current->topics, AZ_STATUS_CAPTURE_GAP);
    if (atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        o->current->proof_connection = (uintptr_t) o->monitor->db;
        o->current->proof_version = o->version;
    }
}

static int ss_provider_replaced(const struct ss_observation *o, unsigned i) {
    if (!o->current->provider[i]) return 1;
    if (!i) return 0;
    const struct ss_file *next = &o->image.file[i - 1], *old = &o->current->image.file[i - 1];
    return next->inode != old->inode || next->device != old->device || strcmp(next->path, old->path);
}

static void ss_renew_file_providers(struct ss_observation *o) {
    for (unsigned i = 0; i < 3; i++) {
            /* Replacement changes the producer incarnation, even at equal bytes. */
        if (ss_provider_replaced(o, i)) o->current->provider[i] = ss_file_provider();
    }
        /* Hot and memory lists can draw from both files. Neither single container
         * identity denotes this combined cue producer. */
    if (!o->current->provider[3] || o->current->provider[1] != o->before.provider[1] || o->current->provider[2] != o->before.provider[2])
        o->current->provider[3] = ss_file_provider();
}

static void ss_renew_field_revisions(struct ss_observation *o, uint64_t serial) {
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        uint64_t bit = AZ_STATUS_TOPIC(t);
        if (!(o->touched & bit)) continue;
        uint64_t revision = serial;
            /* Draining the same positive commit through the monitor must not
             * manufacture a second saved collection edition. */
        if (!(o->gap & bit))
            for (unsigned f = 0; f < 2; f++)
                if ((o->known[f] & bit) && o->confirmed[f]) revision = o->confirmed[f];
        o->current->revision[t] = revision;
    }
}

static int ss_commit_observation(struct ss_observation *o) {
    o->touched |= o->gap;
    if (o->touched || !o->current->ready) {
        uint64_t serial = ss_new_id(&ss_serial);
        if (!serial) {
            return 0;
        }
        ss_renew_file_providers(o);
        ss_renew_field_revisions(o, serial);
        o->current->image = o->image;
        o->current->serial = serial;
        o->current->ready = 1;
        o->current->pending |= o->touched & o->current->topics;
    }
    return 1;
}

static void ss_retire_observed_binding(struct ss_observation *o) {
    if (o->source_rc || o->row_retired) {
        o->current->retired = 1;
        o->current->pending = 0;
        if (o->row_retired && ss_pins[o->deck].epoch == o->before.epoch && ss_pins[o->deck].token) {
            ss_pins[o->deck].retired = 1;
            ss_pins[o->deck].published = 0;
        }
    }
}

static void ss_publish_observed_retirement(struct ss_observation *o) {
    if ((o->source_rc || o->row_retired) && atomic_load_explicit(&ds_proof_enabled, memory_order_acquire)) {
        struct ds_proof_context previous = ds_proof_context;
        ds_proof_context = ss_proof_context(&o->before, DS_PROOF_RETIRE, DS_PROOF_SOURCE);
        ds_retire_file_binding(o->deck, o->before.epoch, &o->before.ref);
        ds_proof_context = previous;
    } else if (o->source_rc || o->row_retired) ds_retire_file_binding(o->deck, o->before.epoch, &o->before.ref);
}
static void ss_monitor_deck(unsigned deck) {
    struct ss_observation o = {.deck = deck, .monitor = &ss_monitors[deck]};
    pthread_mutex_lock(&ss_mu);
    o.before = ss_decks[deck];
    pthread_mutex_unlock(&ss_mu);
    if (!ss_monitor_ready(&o)) return;
    ss_begin_observation(&o);
    ss_observe_database(&o);
    ss_observe_source_loss(&o);
    pthread_mutex_lock(&ss_mu);
    o.current = &ss_decks[deck];
    if (!ss_observation_current(&o)) {
        pthread_mutex_unlock(&ss_mu);
        return;
    }
    ss_accept_file_proofs(&o);
    ss_observed_changes(&o);
    ss_close_file_changes(&o);
    ss_publish_observed_gap(&o);
    if (!ss_commit_observation(&o)) {
        pthread_mutex_unlock(&ss_mu);
        return;
    }
    ss_retire_observed_binding(&o);
    int pending = o.current->pending != 0;
    pthread_mutex_unlock(&ss_mu);
    ss_publish_observed_retirement(&o);
    if (pending) st_worker_wake();
}
/* A worker pass reads each relevant container once. Per-component parsing preserves
 * independent failures. A bad PSSI payload cannot poison GRID or PWAV. */
static int ss_file_current(const struct st_source *, const struct ss_file *);
static int ss_pin_physical(int rc, const struct azjs_origin *origin, const struct azsdk_source_sample *native) {
    unsigned need = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_PHYSICAL;
    if (rc != AZS_OK || (native->flags & need) != need || native->physical_status != AZS_PHYSICAL_OK ||
        !origin->track_id || !native->mount_id || !native->inode) return AZ_STALE;
    return AZ_OK;
}

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
static int fm_retimed_matches(const struct fo_read *r, const struct fm_edition *e, const struct ss_binding *b) {
    return !(!r || r->kind != 2 || r->complete != 1 || !r->retimed ||
             !st_ref_equal(&r->source.ref, &b->ref) || memcmp(r->files, b->image.file, sizeof r->files) ||
             !(e->tags.present & 1u) || (e->tags.ambiguous & 1u));
}

static void fm_update_phrase(struct fm_edition *e, const struct ss_parse *parsed) {
    e->phrase_end = 0;
    for (unsigned i = 0; i < parsed->data.view.phrase_count; i++)
        if (parsed->data.phrases[i].end_beat > e->phrase_end) e->phrase_end = parsed->data.phrases[i].end_beat;
    e->tags.value[1] = parsed->map_tags.value[1];
    e->tags.present = (e->tags.present & ~2u) | (parsed->map_tags.present & 2u);
    e->tags.ambiguous = (e->tags.ambiguous & ~2u) | (parsed->map_tags.ambiguous & 2u);
}

static void fm_update_grid(struct fm_edition *e, const struct ss_binding *b, const struct ss_parse *parsed) {
    e->beats = parsed->data.view.beat_count;
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!fm_retimed_matches(r, e, b)) continue;
        for (unsigned j = 0; j < r->count; j++)
            if (r->retimed & (1u << j)) {
                r->tags[j] = e->tags.value[0];
                r->retimed &= ~(1u << j);
            }
    }
}

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
        if (c) fm_update_phrase(e, parsed);
        else fm_update_grid(e, b, parsed);
    }
}
static unsigned ss_availability(int rc) {
    if (rc == AZ_UNSUPPORTED || rc == AZ_NOT_FOUND) return AZ_AVAIL_UNSUPPORTED;
    if (rc == AZ_STALE) return AZ_AVAIL_STALE;
    return AZ_AVAIL_FAILED;
}
/* A confirmed writer can exempt other fields only if all PMAI framing and
 * bytes outside the component that writer owns remain identical. */
static void ss_hash_sibling(azsha256_ctx *hash, unsigned lane, const uint8_t *tag, size_t h, size_t n) {
    if (lane == SS_SAVED_GRID && !memcmp(tag, "PQTZ", 4) && h >= 24) {
            /* The stock offset writer only changes this signed millisecond field. */
        azsha256_update(hash, tag, 18);
        azsha256_update(hash, tag + 20, n - 20);
    } else if (lane != SS_SAVED_CUES || (memcmp(tag, "PCOB", 4) && memcmp(tag, "PCO2", 4))) {
        azsha256_update(hash, tag, n);
    }
}

static int ss_pmai_extent(const uint8_t *data, size_t size, size_t *start) {
    if (!data || size < 12 || size > ST_FILE_MAX || memcmp(data, "PMAI", 4) || hc_be32(data + 8) != size) return 0;
    *start = hc_be32(data + 4);
    if (*start < 12 || *start > size) return 0;
    return 1;
}

static int ss_sibling_digest(unsigned lane, const uint8_t *data, size_t size, char digest[65]) {
    size_t at = 0;
    if (!ss_pmai_extent(data, size, &at)) return 0;
    azsha256_ctx hash;
    azsha256_init(&hash);
    azsha256_update(&hash, data, 8);
    azsha256_update(&hash, data + 12, at - 12);
    while (at < size) {
        if (size - at < 12) return 0;
        const uint8_t *tag = data + at;
        size_t h = hc_be32(tag + 4), n = hc_be32(tag + 8);
        if (h < 12 || h > n || n > size - at) return 0;
        ss_hash_sibling(&hash, lane, tag, h, n);
        at += n;
    }
    azsha256_final_hex(&hash, digest);
    return 1;
}
struct ss_component {
    uint64_t topic;
    uint32_t mask;
    unsigned containers;
    az_component_info *info;
    uint32_t *availability;
};

static int ss_component_result(int initial, unsigned containers, const int rc[2]) {
    int result = initial;
    for (int f = 0; f < 2; f++) {
        if (!(containers & (1u << f))) continue;
        if (rc[f] && rc[f] != AZ_UNSUPPORTED) result = rc[f];
    }
    return result;
}

static void ss_decode_cues(struct ss_parse *out, int result, const uint8_t *inputs[2], const size_t sizes[2]) {
    struct st_data *d = &out->data;
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
}

static void ss_apply_grid_offset(struct ss_parse *out) {
    struct st_data *d = &out->data;
    if (out->grid == AZ_AVAIL_READY && d->grid_offset_ms) {
        double offset = d->grid_offset_ms / 1000.0;
        for (unsigned j = 0; j < d->view.beat_count; j++) {
            d->beats[j].time_s += offset;
            out->grid_ticks[j] += d->grid_offset_ms;
        }
    }
}

static void ss_calibrate_overview(const struct ss_image *im, struct ss_parse *out) {
    struct st_data *d = &out->data;
    if (out->overview == AZ_AVAIL_READY) {
        if (im->metadata.valid & AZ_META_DURATION)
            d->view.waveform.seconds_per_sample = im->metadata.duration_s / d->view.waveform.sample_count;
        else out->overview = AZ_AVAIL_UNSUPPORTED;
    }
}

static void ss_decode_file_tags(struct ss_parse *out, uint8_t *const buf[2], const size_t sizes[2], const int rc[2]) {
    const uint8_t *tag_inputs[] = {buf[0], buf[1]};
    if (!fm_scan(tag_inputs, sizes, &out->map_tags, NULL, 0)) out->map_tags.ambiguous = 7;
    for (unsigned f = 0; f < 2; f++)
        if (rc[f] == AZ_OK)
            for (unsigned l = 0; l < SS_SAVED_LANES; l++)
                ss_sibling_digest(l, buf[f], sizes[f], out->siblings[l][f]);
}

static void ss_decode_image(const struct ss_image *im, uint64_t topics, struct ss_parse *out,
                            uint8_t *const buf[2], const size_t sizes[2], const int rc[2]) {
    struct st_data *d = &out->data;
    st_data_init(d);
    d->beat_ticks = out->grid_ticks;
    d->cue_ticks = out->cue_ticks;
    ss_decode_file_tags(out, buf, sizes, rc);
    const struct ss_component components[] = {
        {SS_TOPIC(GRID), AZ_DATA_GRID, 1, &d->view.grid_info, &out->grid},
        {SS_TOPIC(PHRASES), AZ_DATA_PHRASES, 2, &d->view.phrases_info, &out->phrases},
        {SS_TOPIC(CUES), AZ_DATA_CUES, 3, NULL, NULL},
        {SS_TOPIC(OVERVIEW), AZ_DATA_WAVEFORM, 1, &d->view.waveform_info, &out->overview}};
    for (unsigned i = 0; i < 4; i++) {
        if (!(topics & components[i].topic)) continue;
        int result = ss_component_result(im->rc, components[i].containers, rc);
        const uint8_t *inputs[2] = {buf[0], buf[1]};
        if (components[i].mask == AZ_DATA_CUES) {
            ss_decode_cues(out, result, inputs, sizes);
            continue;
        }
        if (!result) result = st_parse_component(d, inputs, sizes, components[i].mask);
        *components[i].availability = result ? ss_availability(result) : components[i].info->availability;
        if (components[i].mask == AZ_DATA_GRID) ss_apply_grid_offset(out);
    }
    ss_calibrate_overview(im, out);
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
static void ss_build_metadata(struct ss_fields *p, uint64_t topics, const double *precision) {
    const struct ss_binding *b = p->binding;
    const az_track_metadata *m = &b->image.metadata;
    if (topics & SS_TOPIC(METADATA)) {
        ss_field(p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_CONTENT_ID, 0,
                 b->image.rc ? ss_availability(b->image.rc) : AZ_AVAIL_READY, &b->ref.track_id, 1);
        const char *values[] = {m->title, m->artist, m->album, m->path, m->format};
        const unsigned bits[] = {AZ_META_TITLE, AZ_META_ARTIST, AZ_META_ALBUM, AZ_META_PATH, AZ_META_FORMAT};
        for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++)
            ss_field(p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE + i, 0,
                     ss_meta_status(b, bits[i]), values[i], (unsigned) strlen(values[i]));
        ss_field(p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S, 0,
                 ss_meta_status(b, AZ_META_DURATION), &m->duration_s, 1);
        ss_field(p, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S, 0,
                 ss_meta_status(b, AZ_META_DURATION), precision, 1);
    }
}

static void ss_build_key_tempo(struct ss_fields *p, uint64_t topics) {
    const struct ss_binding *b = p->binding;
    const az_track_metadata *m = &b->image.metadata;
    if (topics & SS_TOPIC(KEY))
        ss_field(p, AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL, 0,
                 ss_meta_status(b, AZ_META_KEY), m->key, (unsigned) strlen(m->key));
    if (topics & SS_TOPIC(TEMPO))
        ss_field(p, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM, 0,
                 ss_meta_status(b, AZ_META_TEMPO), &m->original_bpm, 1);
}

static void ss_build_phrases(struct ss_fields *p, uint64_t topics, const struct ss_parse *parsed) {
    const struct st_data *d = parsed ? &parsed->data : NULL;
    if (topics & SS_TOPIC(PHRASES))
        ss_field(p, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED, 2, d ? parsed->phrases : AZ_AVAIL_FAILED,
                 d ? d->phrases : NULL, d ? d->view.phrase_count : 0);
}

static void ss_build_maps(struct ss_fields *p, uint64_t topics, const struct ss_parse *parsed) {
    const struct st_data *d = parsed ? &parsed->data : NULL;
    if (topics & SS_TOPIC(GRID))
        ss_field(p, AZ_STATUS_GRID, AZ_STATUS_GRID_SAVED, 1, d ? parsed->grid : AZ_AVAIL_FAILED,
                 d ? d->beats : NULL, d ? d->view.beat_count : 0);
    ss_build_phrases(p, topics, parsed);
}

static void ss_build_cues(struct ss_fields *p, uint64_t topics, const struct ss_parse *parsed) {
    const struct st_data *d = parsed ? &parsed->data : NULL;
    if (topics & SS_TOPIC(CUES)) {
        ss_field(p, AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_HOT, 3,
                 d ? parsed->cue_availability[1] : AZ_AVAIL_FAILED,
                 d ? parsed->hot : NULL, d ? parsed->hot_count : 0);
        ss_field(p, AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_MEMORY, 3,
                 d ? parsed->cue_availability[0] : AZ_AVAIL_FAILED,
                 d ? d->cues : NULL, d ? d->view.cue_count : 0);
    }
}

static void ss_build_overview(struct ss_fields *p, uint64_t topics, const struct ss_parse *parsed) {
    const struct st_data *d = parsed ? &parsed->data : NULL;
    if (topics & SS_TOPIC(OVERVIEW))
        ss_field(p, AZ_STATUS_OVERVIEW, AZ_STATUS_OVERVIEW_SAVED, 1, d ? parsed->overview : AZ_AVAIL_FAILED,
                 d ? &d->view.waveform : NULL, 1);
}

static const struct ds_time_axis *ss_field_clock(const struct ds_field *field, const struct ds_time_axis *const clocks[5]) {
    if (field->topic == AZ_STATUS_METADATA &&
        (field->field == AZ_STATUS_METADATA_DURATION_S || field->field == AZ_STATUS_METADATA_DURATION_PRECISION_S))
        return field->field == AZ_STATUS_METADATA_DURATION_S ? clocks[0] : clocks[1];
    else if (field->topic == AZ_STATUS_GRID) return clocks[4];
    else if (field->topic == AZ_STATUS_CUES)
        return field->field == AZ_STATUS_CUES_SAVED_HOT ? clocks[2] : clocks[3];
    else if (field->topic == AZ_STATUS_OVERVIEW) return clocks[0];
    return NULL;
}

static void ss_attach_clocks(struct ss_fields *p, const struct ds_time_axis *const clocks[5]) {
    /* Preserve the original integral extent. PWAV calibration divides that
     * source duration by the column count only in the public waveform value. */
    for (unsigned i = 0; i < p->n; i++) {
        struct ds_field *field = &p->field[i];
        if (field->info.info.availability != AZ_AVAIL_READY) continue;
        field->clock = ss_field_clock(field, clocks);
    }
}

static int ss_publish_image(unsigned deck, const struct ss_binding *b, uint64_t topics,
                            struct ss_parse *parsed, uint64_t observed) {
    struct ss_fields p = {.observed = observed ? observed : st_now(), .binding = b};
    const double precision = 1.0; /* exportLibrary's integral duration is in seconds */
    ss_build_metadata(&p, topics, &precision);
    ss_build_key_tempo(&p, topics);
    ss_build_maps(&p, topics, parsed);
    ss_build_cues(&p, topics, parsed);
    ss_build_overview(&p, topics, parsed);
    const int64_t precision_tick = 1;
    const struct ds_time_axis duration_clock = {1, &b->image.duration_ticks, 1, 1, NULL};
    const struct ds_time_axis precision_clock = {1, &precision_tick, 1, 1, NULL};
    const struct ds_time_axis hot_clock = {1000, parsed ? parsed->hot_ticks : NULL, 1, 1000, NULL};
    const struct ds_time_axis memory_clock = {1000, parsed ? parsed->cue_ticks : NULL, 1, 1000, NULL};
    const struct ds_time_axis grid_clock = {1000, parsed ? parsed->grid_ticks : NULL, 1, 1000, NULL};
    const struct ds_time_axis *clocks[] = {&duration_clock, &precision_clock, &hot_clock, &memory_clock, &grid_clock};
    ss_attach_clocks(&p, clocks);
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
static struct ss_parse *ss_decode_saved(struct ss_saved_revision *q, uint64_t topics, int mounted) {
    struct ss_parse *parsed = mounted ? calloc(1, sizeof *parsed) : NULL;
    if (parsed) {
        const struct ss_image committed = {0};
        ss_decode_image(&committed, topics, parsed, q->bytes, q->sizes, q->rc);
    }
    return parsed;
}

static int ss_saved_target_current(const struct ss_binding *b, const struct ss_saved_revision *q, unsigned deck) {
    unsigned lane = q->lane;
    return !(b->cut != q->target[deck].cut || b->epoch != q->target[deck].epoch ||
             b->interest[lane] != q->target[deck].interest || !st_ref_equal(&b->ref, &q->ref));
}

static int ss_saved_pin_matches(const struct ss_saved_revision *q, unsigned deck, const struct st_source *source, const struct azjs_origin *origin, const struct azsdk_source_sample *native, uint64_t pin, int witness) {
    return witness == AZ_OK && ss_pins[deck].token == pin && !ss_pins[deck].retired &&
           origin->track_id == q->ref.track_id && native->device == source->device &&
           native->inode == source->inode && native->mount_id == source->mount_id;
}

static int ss_saved_liveness(const struct ss_saved_revision *q, unsigned deck, const struct st_source *source, int *live) {
    uint64_t pin = q->target[deck].source_pin;
    if (*live && pin) {
        struct azjs_origin origin = {0};
        struct azsdk_source_sample native = {0};
        int witness = ss_pin_witness(pin, &origin, &native);
        if (witness == AZ_BUSY) return AZ_BUSY;
        *live = ss_saved_pin_matches(q, deck, source, &origin, &native, pin, witness);
    }
    return AZ_OK;
}

static void ss_deliver_saved(struct ss_saved_revision *q, unsigned deck, const struct st_source *source, int mounted, struct ss_parse *parsed, unsigned *blocked) {
    unsigned bit = 1u << deck, lane = q->lane, topic = ss_saved_topic[lane], provider = ss_saved_provider[lane];
    uint64_t topics = AZ_STATUS_TOPIC(topic);
    struct ss_binding *b = &ss_decks[deck];
    if (!ss_saved_target_current(b, q, deck)) {
        q->targets &= ~bit;
        return;
    }
    int live = mounted;
    if (ss_saved_liveness(q, deck, source, &live) == AZ_BUSY) {
        *blocked |= bit;
        return;
    }
    if (b->pinned[lane]) --b->pinned[lane];
    q->targets &= ~bit;
    if (b->retired || !(b->topics & topics)) return;
    if (live && parsed) {
        struct ss_binding cut = {.epoch = b->epoch, .ref = q->ref};
        cut.provider[provider] = q->target[deck].provider;
        cut.revision[topic] = q->revision;
        ss_publish_image(deck, &cut, topics, parsed, q->observed);
    } else ds_publish_file(deck, b->epoch, &q->ref, q->observed, NULL, 0, topics, AZ_STATUS_REVISION_LOST);
}

static int ss_finish_saved(struct ss_saved_revision *q, unsigned *index) {
    int finished = !q->targets;
    if (finished) {
        --ss_saved_count;
        memmove(ss_saved_queue + (*index), ss_saved_queue + (*index) + 1, (ss_saved_count - (*index)) * sizeof *ss_saved_queue);
        ss_saved_bytes -= q->total;
    } else ++(*index);
    return finished;
}

static void ss_drain_saved(void) {
    unsigned blocked[SS_SAVED_LANES] = {0}, index = 0;
    for (unsigned n = 0; n < SS_SAVE_QUEUE; n++) {
        pthread_mutex_lock(&ss_mu);
        if (index >= ss_saved_count) {
            pthread_mutex_unlock(&ss_mu);
            break;
        }
        struct ss_saved_revision *q = ss_saved_queue[index];
        unsigned lane = q->lane, topic = ss_saved_topic[lane];
        uint64_t topics = AZ_STATUS_TOPIC(topic);
        unsigned candidates = q->targets & ~blocked[lane];
        pthread_mutex_unlock(&ss_mu);
        if (!candidates) {
            ++index;
            continue;
        }
        struct st_source source;
        int mounted = !ss_source(&q->ref, &source);
        struct ss_parse *parsed = ss_decode_saved(q, topics, mounted);
        pthread_mutex_lock(&ss_mu);
        for (unsigned deck = 0; deck < 4; deck++) {
            unsigned bit = 1u << deck;
            if (!(candidates & bit)) continue;
            ss_deliver_saved(q, deck, &source, mounted, parsed, &blocked[lane]);
        }
        int finished = ss_finish_saved(q, &index);
        pthread_mutex_unlock(&ss_mu);
        free(parsed);
        if (finished) ss_release_saved(q);
    }
}
static unsigned ss_group_jobs(const struct ss_binding jobs[4], unsigned deck, uint64_t *topics) {
        /* Two decks playing the same qualified reference and revision share one parse. */
    unsigned members = 0;
    for (unsigned i = deck; i < 4; i++)
        if (jobs[i].pending && jobs[i].ready && !jobs[i].retired &&
            st_ref_equal(&jobs[deck].ref, &jobs[i].ref) &&
            ss_image_same(&jobs[deck].image, &jobs[i].image)) {
            members |= 1u << i;
            *topics |= jobs[i].pending;
        }
    return members;
}

static uint64_t ss_stale_job_files(const struct st_source *current, const struct ss_binding *job, uint64_t topics) {
    uint64_t stale = 0;
    if ((topics & SS_DAT) && !ss_file_current(current, &job->image.file[0])) stale |= SS_DAT;
    if ((topics & SS_EXT) && !ss_file_current(current, &job->image.file[1])) stale |= SS_EXT;
    return stale;
}

static int ss_job_source_live(unsigned deck, const struct ss_binding *d, const struct st_source *current) {
    uint64_t token = ss_pins[deck].epoch == d->epoch ? ss_pins[deck].token : 0;
    int source_live = !token || !ss_pins[deck].retired;
    if (token && source_live) {
        struct azjs_origin origin = {0};
        struct azsdk_source_sample native = {0};
        source_live = ss_pin_witness(token, &origin, &native) == AZ_OK && origin.track_id == d->ref.track_id &&
                      native.device == current->device && native.inode == current->inode && native.mount_id == current->mount_id;
    }
    return source_live;
}

static uint64_t ss_job_publish_topics(const struct ss_binding *d, uint64_t pending, uint64_t stale) {
    uint64_t publish = pending & d->pending & d->topics & ~stale;
            /* A busy source can leave earlier immutable revisions queued.
             * Never publish a current file image ahead of their callbacks. */
    for (unsigned l = 0; l < SS_SAVED_LANES; l++)
        if (d->pinned[l]) publish &= ~AZ_STATUS_TOPIC(ss_saved_topic[l]);
    return publish;
}

static int ss_job_current(const struct ss_binding *current, const struct ss_binding *job) {
    return !current->writing && current->cut == job->cut && current->serial == job->serial && !current->retired;
}

static void ss_update_cue_edition(unsigned i, const struct ss_binding *d, uint64_t publish, const struct ss_parse *parsed) {
    if (!(publish & SS_TOPIC(CUES))) return;
    struct fo_edition *ed = &fo_editions[i];
    ed->cut = d->cut;
    ed->provider = d->provider[3];
    ed->revision = d->revision[AZ_STATUS_CUES];
    if (parsed) ed->scan = parsed->origin;
    else ed->scan.failed = 1;
    azfile_native_refresh(i, d->epoch);
}

static void ss_update_siblings(struct ss_binding *d, const struct ss_parse *parsed) {
    if (!parsed) return;
    for (unsigned f = 0; f < 2; f++)
        for (unsigned l = 0; l < SS_SAVED_LANES; l++)
            if (parsed->siblings[l][f][0]) memcpy(d->siblings[l][f], parsed->siblings[l][f], sizeof d->siblings[l][f]);
}

static void ss_publish_jobs(struct ss_binding jobs[4], unsigned deck, unsigned members, const struct st_source *current, uint64_t stale, struct ss_parse *parsed) {
        /* Let the monitor order the path change and any loss before this
         * component can be published. Other containers and metadata still proceed. */
    for (unsigned i = deck; i < 4; i++) {
        if (!(members & (1u << i))) continue;
        pthread_mutex_lock(&ss_mu);
        struct ss_binding *d = &ss_decks[i];
        int source_live = ss_job_source_live(i, d, current);
        uint64_t publish = ss_job_publish_topics(d, jobs[i].pending, stale);
        if (source_live && ss_job_current(d, &jobs[i]) && publish && ss_publish_baseline(i, d, publish, parsed) == AZ_OK) {
            d->pending &= ~publish;
            fm_update(i, d, publish, parsed);
            ss_update_cue_edition(i, d, publish, parsed);
            ss_update_siblings(d, parsed);
        }
        pthread_mutex_unlock(&ss_mu);
        jobs[i].pending = 0;
    }
}

static void ss_work_group(struct ss_binding jobs[4], unsigned deck) {
    uint64_t topics = 0;
    unsigned members = ss_group_jobs(jobs, deck, &topics);
    struct ss_parse *parsed = NULL;
    if (topics & (SS_DAT | SS_EXT)) {
        parsed = calloc(1, sizeof *parsed);
        if (parsed) ss_parse_image(&jobs[deck].image, topics, parsed);
    }
    struct st_source current;
    if (ss_source(&jobs[deck].ref, &current)) {
        free(parsed);
        return;
    }
    uint64_t stale = ss_stale_job_files(&current, &jobs[deck], topics);
    ss_publish_jobs(jobs, deck, members, &current, stale, parsed);
    free(parsed);
}

static void ss_work(void) {
    ss_drain_saved();
    struct ss_binding jobs[4];
    pthread_mutex_lock(&ss_mu);
    memcpy(jobs, ss_decks, sizeof jobs);
    pthread_mutex_unlock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) {
        if (!jobs[deck].pending || !jobs[deck].ready || jobs[deck].retired) continue;
        ss_work_group(jobs, deck);
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
    return ss_pin_physical(rc, origin, native);
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
static int ss_pin_read_row(uint64_t token, struct st_source *source, const az_track_ref *ref) {
    char path[1024];
    int rc = AZ_STALE;
    if (st_source_path(source->mount, "/PIONEER/rekordbox/exportLibrary.db", path)) {
        int fd = -1;
        void *db = ss_open_database(path, source, &fd);
        if (db) {
            struct st_resolution row = {0};
            uint64_t before = 0, after = 0;
            rc = ss_database_version(db, &before);
            if (!rc) rc = st_read_row(db, ref->track_id, &row);
            if (ss_database_version(db, &after) || before != after) rc = AZ_BUSY;
            ss_close_database(db, fd);
            az_track_ref checked = {0};
            if (!rc && (ss_pin_identity(token, source, &checked) || !st_ref_equal(ref, &checked))) rc = AZ_STALE;
        }
    }
    return rc;
}

static int ss_pin_preserve_incarnation(const az_track_ref *old, const az_track_ref *ref, int rc) {
    if (old->track_id && rc != AZ_BUSY) {
        struct st_source pinned;
        if (ss_source(old, &pinned) || (rc == AZ_OK && !st_ref_equal(old, ref)))
            rc = AZ_CANCELLED;
    }
    return rc;
}

static void ss_publish_pin(unsigned deck, uint64_t epoch, uint64_t token, const az_track_ref *ref) {
    int32_t port = (int32_t) ref->source_id;
    uint64_t now = st_now();
    az_status_field_info_v1 info = {{AZ_AVAIL_READY, AZ_PROV_DERIVED, token, now},
                                    AZ_STATUS_LOAD_SCOPE,
                                    1,
                                    epoch,
                                    token};
    struct ds_field fields[] = {{AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, info, ref, NULL},
                                {AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, info, &port, NULL}};
    if (ds_publish(deck, epoch, 0, now, fields, 2, NULL, NULL, 0, 0) == AZ_OK) {
        pthread_mutex_lock(&ss_mu);
        if (ss_pins[deck].epoch == epoch && ss_pins[deck].token == token) {
            ss_pins[deck].ref = *ref;
            ss_pins[deck].published = 1;
        }
        pthread_mutex_unlock(&ss_mu);
    }
}

static void ss_retire_pin(unsigned deck, uint64_t epoch, uint64_t token, int published, int rc) {
    if (published || rc == AZ_CANCELLED) {
        pthread_mutex_lock(&ss_mu);
        if (ss_pins[deck].epoch == epoch && ss_pins[deck].token == token) {
            ss_pins[deck].retired = rc == AZ_CANCELLED;
            ss_pins[deck].published = 0;
        }
        pthread_mutex_unlock(&ss_mu);
    }
}

struct ss_pin_snapshot {
    uint64_t epoch, token;
    int published;
    az_track_ref ref;
};
static int ss_capture_pin(unsigned deck, struct ss_pin_snapshot *snapshot) {
    pthread_mutex_lock(&ss_mu);
    snapshot->epoch = ss_pins[deck].epoch;
    snapshot->token = ss_pins[deck].token;
    snapshot->published = ss_pins[deck].published;
    snapshot->ref = ss_pins[deck].ref;
    int interested = ss_decks[deck].topics != 0 && !ss_pins[deck].retired;
    pthread_mutex_unlock(&ss_mu);
    if (!snapshot->epoch || !snapshot->token || !interested) return 0;
    return 1;
}

static int ss_pin_current(unsigned deck, uint64_t epoch, uint64_t token) {
    pthread_mutex_lock(&ss_mu);
    int current = ss_pins[deck].epoch == epoch && ss_pins[deck].token == token && ss_decks[deck].epoch == epoch;
    pthread_mutex_unlock(&ss_mu);
    return current;
}

static void ss_apply_pin_resolution(unsigned deck, const struct ss_pin_snapshot *snapshot, const az_track_ref *ref, int rc) {
    if (snapshot->published) (void) ds_retire_file_binding(deck, snapshot->epoch, &snapshot->ref);
    else if (!rc) ss_publish_pin(deck, snapshot->epoch, snapshot->token, ref);
    ss_retire_pin(deck, snapshot->epoch, snapshot->token, snapshot->published, rc);
}

static int ss_resolve_pin(unsigned deck) {
    struct ss_pin_snapshot snapshot;
    if (!ss_capture_pin(deck, &snapshot)) return 0;
    uint64_t epoch = snapshot.epoch, token = snapshot.token;
    int published = snapshot.published;
    az_track_ref old = snapshot.ref;
    struct st_source source;
    az_track_ref ref = {0};
    int rc = ss_pin_identity(token, &source, &ref);
    if (rc == AZ_OK && published && st_ref_equal(&ref, &old)) return 1;
    /* A connection/filesystem observation can be temporarily incomplete. A
     * later proof may recover the original incarnation, never a replacement
     * port/source or row on this pack token. */
    rc = ss_pin_preserve_incarnation(&old, &ref, rc);
    if (rc == AZ_OK && !published) rc = ss_pin_read_row(token, &source, &ref);
    if (rc == AZ_BUSY) return 1;
    int current = ss_pin_current(deck, epoch, token);
    if (!current) return 0;
    ss_apply_pin_resolution(deck, &snapshot, &ref, rc);
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
static unsigned ss_topic_provider(unsigned t) {
    if (t == AZ_STATUS_CUES) return 3;
    if (t == AZ_STATUS_PHRASES) return 2;
    return t == AZ_STATUS_GRID || t == AZ_STATUS_OVERVIEW ? 1 : 0;
}

static int ss_withdraw_ready(const struct ss_binding *b, uint64_t topics) {
    return topics && b->ready && !b->retired && ss_local(&b->ref);
}

static void ss_withdraw(unsigned deck, struct ss_binding *b, uint64_t topics) {
    if (!ss_withdraw_ready(b, topics)) return;
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
        unsigned source = ss_topic_provider(t);
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

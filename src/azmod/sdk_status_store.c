/* SPDX-License-Identifier: GPL-3.0-only */
/* Only a positively identified LOAD.track can
 * arm storage. Source/commit observation runs independently of the parsing worker. No
 * status selection consumes a mod ticket or changes the explicit immutable SQLite reader. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sdk_status_store.h"
#include "sdk_status_engine.h"
#include "sdk_status_host.h"
#include "track_storage.h"
#include "track_service.h"
#include "library_sql.h"
#include "sdk_data_engine.h"
#ifndef az_thread_create
#include "../common/azaltstack.h"
#endif
#include <poll.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <limits.h>
#include "../common/azsha256.h"
#include "../common/azsources.h"
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
    if (!az_storage_sql()->prepare(db, "PRAGMA data_version", -1, &stmt, NULL) && stmt &&
        az_storage_sql()->step(stmt) == 100) {
        *value = (uint64_t) az_storage_sql()->col_int64(stmt, 0);
        rc = AZ_OK;
    }
    if (stmt) az_storage_sql()->finalize(stmt);
    return rc;
}
/* Non-URI, READONLY. SQLite owns the read locks and WAL snapshot. The immutable
 * path used by old enumeration/tickets would silently omit a live committed WAL. */
static void *ss_open_database(const char *path, const struct st_source *src, int *fd) {
    void *db = NULL;
    if (az_storage_sql()->open_v2(path, &db, 1, NULL) == 0 && db &&
        !az_storage_key_database(db)) {
        *fd = azsdk_sources_reader_fd(db, src->mount_id, src->device, src->inode);
        if (az_status_host_reader_register(*fd)) return db;
    }
    if (db) az_storage_sql()->close(db);
    return NULL;
}
static void ss_close_database(void *db, int fd) {
    az_status_host_reader_release(fd);
    az_storage_sql()->close(db);
}
/* OneLibrary content.fileType codes. Read this optional column separately so
 * an older export schema cannot take the other metadata fields offline. */
static void ss_format_result(void *stmt, az_track_metadata *m, const char *const *names, size_t count) {
    long long type = az_storage_sql()->col_int64(stmt, 0);
    if (type >= 0 && (uint64_t) type < count && names[type] && az_storage_sql()->step(stmt) == 101) {
        snprintf(m->format, sizeof m->format, "%s", names[type]);
        m->valid |= AZ_META_FORMAT;
    }
}

static void ss_read_format(void *db, uint32_t track_id, az_track_metadata *m) {
    static const char *const names[] = {
        [1] = "MP3", [3] = "MP4", [4] = "AAC", [5] = "FLAC", [6] = "ALAC", [11] = "WAV", [12] = "AIFF"};
    void *stmt = NULL;
    const char *sql = "SELECT fileType FROM content WHERE content_id=?1 AND typeof(fileType)='integer'";
    if (!az_storage_sql()->prepare(db, sql, -1, &stmt, NULL) && stmt && !az_storage_sql()->bind_int64(stmt, 1, track_id) &&
        az_storage_sql()->step(stmt) == 100) {
        ss_format_result(stmt, m, names, sizeof names / sizeof names[0]);
    }
    if (stmt) az_storage_sql()->finalize(stmt);
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
    if (!st_source_file(src, relative, file->path)) {
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
    if (o->monitor->row.content[0] && st_source_file(&o->image.src, o->monitor->row.content, path) && strlen(path) < sizeof o->image.metadata.path) {
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
        if (!o->monitor->db && st_source_database(&o->image.src, path))
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
    if (o->current->ready && (o->gap & o->current->topics) && ds_proof_is_enabled()) {
        struct ds_proof_context next = ss_proof_context(o->current, DS_PROOF_GAP, o->proof_cause);
        next.connection = (uintptr_t) o->monitor->db;
        next.db_before = o->proof_before;
        next.db_after = o->version;
        struct ds_proof_context previous = ds_proof_enter(&next);
        ds_publish_file(o->deck, o->current->epoch, &o->current->ref, st_now(), NULL, 0, o->gap & o->current->topics, AZ_STATUS_CAPTURE_GAP);
        ds_proof_restore(&previous);
    } else if (o->current->ready && (o->gap & o->current->topics))
        ds_publish_file(o->deck, o->current->epoch, &o->current->ref, st_now(), NULL, 0, o->gap & o->current->topics, AZ_STATUS_CAPTURE_GAP);
    if (ds_proof_is_enabled()) {
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
    if ((o->source_rc || o->row_retired) && ds_proof_is_enabled()) {
        struct ds_proof_context next = ss_proof_context(&o->before, DS_PROOF_RETIRE, DS_PROOF_SOURCE);
        struct ds_proof_context previous = ds_proof_enter(&next);
        ds_retire_file_binding(o->deck, o->before.epoch, &o->before.ref);
        ds_proof_restore(&previous);
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
    if (pending) track_service_wake();
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
/* SPDX-License-Identifier: GPL-3.0-only */
/* Native accessors run on the storage thread. Read tokens hold copied source
 * identity, never a borrowed native object. Public arrays are decoded by st_worker. */
#include "../common/azstatus_cue_origin.h"
#include "../common/azstatus_analysis_origin.h"
struct sqlite3;
struct sqlite3_stmt;
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
    struct sqlite3 *db;
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

static int fo_container(const uint8_t *data, size_t size) {
    return data && size >= 12 && !memcmp(data, "PMAI", 4) && hc_be32(data + 8) == size &&
           hc_be32(data + 4) >= 12 && hc_be32(data + 4) <= size;
}
static struct fo_read *fo_claim_read(uint64_t id, unsigned *slot) {
    struct fo_read *read = NULL;
    pthread_mutex_lock(&ss_mu);
    for (*slot = 0; *slot < FO_READS; (*slot)++)
        if (fo_reads[*slot] && fo_reads[*slot]->id == id && !fo_reads[*slot]->complete) {
            read = fo_reads[*slot];
            read->complete = 2;
            break;
        }
    pthread_mutex_unlock(&ss_mu);
    return read;
}

/* st_parse_origins already validated framing and the selected list. Examine
 * even the shadowed physical entries for duplicate native atom identities. */
static const struct fo_atom *fo_match(const struct fo_scan *, unsigned, const struct azn_file_atom *);
static void fo_selected_atom(struct fo_atom *atom, const uint8_t *entry, const uint8_t *const *selected, const struct st_data *d) {
    for (unsigned j = 0; d && j < d->view.cue_count; j++)
        if (selected[j] == entry) atom->selected = d->cues[j];
}
static int fo_atom_shape(unsigned header, unsigned total, size_t available, unsigned ext, unsigned count) {
    return header == (ext ? 16u : 28u) && total >= 40 && total <= available && count < FO_ATOMS;
}
static int fo_scan_atom(struct fo_scan *scan, const uint8_t *entry, size_t available, unsigned ext,
                        unsigned hot, unsigned file, size_t offset, const uint8_t *const *selected, const struct st_data *d) {
    if (available < 12 || memcmp(entry, ext ? "PCP2" : "PCPT", 4)) return 0;
    unsigned header = hc_be32(entry + 4), total = hc_be32(entry + 8);
    if (!fo_atom_shape(header, total, available, ext, scan->n)) return 0;
    struct fo_atom *atom = &scan->atoms[scan->n++];
    atom->extended = ext;
    atom->hot = hot;
    atom->slot = hc_be32(entry + 12);
    atom->file = file;
    atom->offset = (uint32_t) offset;
    atom->stamp.header = header;
    atom->stamp.total = total;
    if (total <= AZN_FILE_CUE_BYTES) azsha256_hex(entry + 14, total - 14, atom->stamp.digest);
    fo_selected_atom(atom, entry, selected, d);
    return 1;
}
static int fo_scan_entries(struct fo_scan *scan, const uint8_t *tag, size_t at, unsigned hot, unsigned file,
                           unsigned ext, const uint8_t *const *selected, const struct st_data *d) {
    size_t pos = hc_be32(tag + 4), len = hc_be32(tag + 8);
    unsigned n = hc_be16(tag + (ext ? 16 : 18));
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *entry = tag + pos;
        if (!fo_scan_atom(scan, entry, len - pos, ext, hot, file, at + pos, selected, d)) return 0;
        pos += hc_be32(entry + 8);
    }
    return 1;
}
static int fo_scan_tag(struct fo_scan *scan, const uint8_t *tag, size_t at, unsigned hot, unsigned file,
                       const uint8_t *const *selected, const struct st_data *d) {
    size_t pos = hc_be32(tag + 4);
    int ext = !memcmp(tag, "PCO2", 4);
    if (!ext && memcmp(tag, "PCOB", 4)) return 1;
    if (pos < 16 || hc_be32(tag + 12) > 1) return 0;
    if (hc_be32(tag + 12) != hot) return 1;
    if (pos < (ext ? 20u : 24u)) return 0;
    return fo_scan_entries(scan, tag, at, hot, file, ext, selected, d);
}
static int fo_scan_file(struct fo_scan *scan, const uint8_t *data, size_t size, unsigned file,
                        unsigned hot, const uint8_t *const *selected, const struct st_data *d) {
    if (!fo_container(data, size)) return 0;
    for (size_t at = hc_be32(data + 4); at < size; at += hc_be32(data + at + 8)) {
        if (size - at < 12) return 0;
        const uint8_t *tag = data + at;
        size_t pos = hc_be32(tag + 4), len = hc_be32(tag + 8);
        if (pos < 12 || pos > len || len > size - at) return 0;
        if (!fo_scan_tag(scan, tag, at, hot, file, selected, d)) return 0;
    }
    return 1;
}
static void fo_scan_list(struct fo_scan *scan, const uint8_t *const buf[2], const size_t sizes[2],
                         unsigned hot, const uint8_t *const *selected, const struct st_data *d) {
    if (scan->failed) return;
    for (unsigned f = 0; f < 2; f++) {
        if (!buf[f]) continue;
        if (!fo_scan_file(scan, buf[f], sizes[f], f, hot, selected, d)) {
            scan->failed = 1;
            return;
        }
    }
}
static int fo_file_watch_clean(const struct ss_monitor *watch, unsigned f) {
    return watch->wd[f] >= 0 && !watch->uncommitted_file[f] && !(watch->unsafe_events & (1u << f)) &&
           !(watch->open_edits & (1u << f)) && !watch->close_count[f];
}
static int fo_watched_files(struct fo_read *r, unsigned mask) {
#ifdef __linux__
    if (r->watch.fd < 0) return 0;
    r->watch.pending_events |= ss_drain_events(&r->watch);
    if (r->watch.pending_events & SS_META) return 0;
    for (unsigned f = 0; f < 2; f++)
        if ((mask & (1u << f)) && !fo_file_watch_clean(&r->watch, f)) return 0;
#else
    (void) r;
    (void) mask;
#endif
    return 1;
}
static int fo_alive_files(struct fo_read *r, unsigned mask) {
    if (!fo_watched_files(r, mask)) return 0;
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
static int fo_grid_edit_image(const struct fo_read *r, const struct ss_image *im, uint64_t known) {
    return known == SS_TOPIC(GRID) && im->rc == AZ_OK && im->file[0].rc == AZ_OK &&
           !memcmp(&r->files[1], &im->file[1], sizeof r->files[1]);
}
static int fo_grid_edit_clean(struct fo_read *r, const struct ss_image *im, const uint64_t known[2], const unsigned closes[2]) {
#ifdef __linux__
    r->watch.pending_events |= ss_drain_events(&r->watch);
    return fo_grid_edit_image(r, im, known[0]) && r->watch.pending_events == SS_DAT && !r->watch.unsafe_events &&
           !r->watch.open_edits && closes[0] && r->watch.close_count[0] == closes[0];
#else
    (void) r;
    (void) im;
    (void) known;
    (void) closes;
    return 0;
#endif
}
static void fo_cut_cues(struct fo_read *r, const struct ss_binding *b, const struct ss_image *im,
                        const uint64_t known[2], const unsigned closes[2], uint64_t gap) {
    if (gap & SS_TOPIC(CUES)) {
#ifdef __linux__
        /* A DB observation gap can withdraw saved CUES without touching
         * the original native analysis input. Preserve its identity only
         * while the independent file watch and source still prove it.
         * Public resolution below separately requires the current saved
         * edition and exact cue atoms after recovery. */
        if (fo_alive(r)) return;
#endif
        r->count = 0;
        return;
    }
    if (!memcmp(r->files, im->file, sizeof r->files)) return;
    if (memcmp(r->files, b->image.file, sizeof r->files)) {
        r->count = 0;
        return;
    }
    if (fo_grid_edit_clean(r, im, known, closes)) {
        r->files[0] = im->file[0];
        r->watch.pending_events = 0;
        r->watch.close_count[0] = 0;
    } else r->count = 0;
}
static int fo_cut_read_matches(const struct fo_read *r, const struct ss_binding *b) {
    return r && r->complete == 1 && r->count && st_ref_equal(&r->source.ref, &b->ref);
}
static void fo_file_cut(unsigned deck, const struct ss_binding *b, const struct ss_image *im,
                        const uint64_t known[2], const unsigned closes[2], uint64_t gap) {
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!fo_cut_read_matches(r, b)) continue;
        if (r->kind == 3) {
            /* Original FileData is immutable. Even a certified edit cannot
             * make this old cache object the owner of the replacement input. */
            if (!fo_alive_files(r, r->input_mask)) r->count = 0;
        } else if (r->kind == 2) fm_file_cut(r, b, im, known, closes, gap);
        else fo_cut_cues(r, b, im, known, closes, gap);
    }
    if (fo_held[deck].cut == b->cut && fo_held[deck].read) azfile_native_refresh(deck, b->epoch);
}
/* The accessor contains both canonical row locators. Require their unique
 * committed row on an actual open DB descriptor of that exact physical port.
 * An unrelated cached getter may run before, during or after another load. */
static unsigned fo_unicode_width(const uint16_t *p, size_t n) {
    if (!p[0]) return 0;
    if (p[0] >= 0xd800 && p[0] <= 0xdbff) {
        if (n < 2 || p[1] < 0xdc00 || p[1] > 0xdfff) return 0;
        return 2;
    }
    return p[0] >= 0xdc00 && p[0] <= 0xdfff ? 0 : 1;
}
static int fo_unicode(const uint16_t *p, size_t n) {
    if (!p || !n || n > 1023) return 0;
    for (size_t i = 0; i < n;) {
        unsigned width = fo_unicode_width(p + i, n - i);
        if (!width) return 0;
        i += width;
    }
    return 1;
}
struct fo_locator {
    const char *root, *analysis_utf8;
    const uint16_t *audio, *analysis;
    size_t length, audio_units, analysis_units;
    int physical_path_only;
};
struct fo_sql_bindings {
    int (*utf16)(struct sqlite3_stmt *, int, const void *, int, void (*)(void *));
    int (*utf8)(struct sqlite3_stmt *, int, const char *, int, void (*)(void *));
};
static int ss_export_root(const struct st_source *source, char root[1024]) {
    return snprintf(root, 1024, "%s/%s", source->mount, st_source_export(source));
}
static int ss_export_matches(const struct st_source *source, const char *root, size_t length) {
    size_t mount = strlen(source->mount);
    const char *folder = st_source_export(source);
    size_t bytes = strlen(folder);
    return length == mount + 1 + bytes && root[mount] == '/' &&
           !memcmp(source->mount, root, mount) && !memcmp(folder, root + mount + 1, bytes);
}
static int fo_locator_valid(const struct fo_locator *where) {
    if (!where->root || !where->length || where->length >= 1024) return 0;
    if (!where->physical_path_only && !fo_unicode(where->audio, where->audio_units)) return 0;
    if (!where->analysis_utf8 && !fo_unicode(where->analysis, where->analysis_units)) return 0;
    return az_storage_sql()->ok == 1;
}
static int fo_source_root(struct fo_read *r, const struct fo_locator *where) {
    unsigned found = 0;
    for (int i = 0; i < 2; i++) {
        struct st_source source;
        if (!st_source_read(i, &source) && ss_export_matches(&source, where->root, where->length)) {
            r->source = source;
            found++;
        }
    }
    return found == 1;
}
static int fo_sql_provider(const void *function, const void *expected) {
    Dl_info binding = {0};
    return function && dladdr(function, &binding) && binding.dli_fbase == expected;
}
static int fo_sql_bindings(struct fo_sql_bindings *bindings, int need_utf8) {
    bindings->utf16 = (__typeof__(bindings->utf16)) dlsym(RTLD_DEFAULT, "sqlite3_bind_text16");
    bindings->utf8 = (__typeof__(bindings->utf8)) dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    Dl_info provider = {0};
    if (!dladdr((void *) az_storage_sql()->bind_int64, &provider) || !fo_sql_provider((void *) bindings->utf16, provider.dli_fbase)) return 0;
    return !need_utf8 || fo_sql_provider((void *) bindings->utf8, provider.dli_fbase);
}
static int fo_open_database(struct fo_read *r) {
    char path[1024];
    return st_source_database(&r->source, path) &&
           (r->db = ss_open_database(path, &r->source, &r->db_fd)) && !ss_database_version(r->db, &r->version);
}
static int fo_bind_locator(struct sqlite3_stmt *stmt, const struct fo_locator *where, const struct fo_sql_bindings *bindings) {
    if (!where->physical_path_only && bindings->utf16(stmt, 1, where->audio, (int) (2 * where->audio_units), NULL)) return 0;
    return !(where->analysis_utf8 ? bindings->utf8(stmt, 2, where->analysis_utf8, -1, NULL)
                                  : bindings->utf16(stmt, 2, where->analysis, (int) (2 * where->analysis_units), NULL));
}
static int fo_bind_analysis_alias(struct sqlite3_stmt *stmt, const struct st_source *source,
                                  const char *physical, const struct fo_sql_bindings *bindings) {
    if (!source->hfsplus || !physical || strncmp(physical, "/.PIONEER/", sizeof "/.PIONEER/" - 1))
        return 1;
    char logical[1024];
    int bytes = snprintf(logical, sizeof logical, "/%s", physical + 2);
    return (unsigned) bytes < sizeof logical &&
           !bindings->utf8(stmt, 3, logical, -1, (void (*)(void *)) -1); /* SQLITE_TRANSIENT */
}
static uint32_t fo_unique_content(struct sqlite3_stmt *stmt) {
    if (az_storage_sql()->step(stmt) != 100) return 0;
    long long value = az_storage_sql()->col_int64(stmt, 0);
    if (value > 0 && (unsigned long long) value <= UINT32_MAX && az_storage_sql()->step(stmt) == 101) return (uint32_t) value;
    return 0;
}
static uint32_t fo_find_content(struct fo_read *r, const struct fo_locator *where, const struct fo_sql_bindings *bindings) {
    static const char path_only[] =
        "SELECT content_id FROM content WHERE analysisDataFilePath COLLATE BINARY IN (?2, ?3) LIMIT 2";
    static const char with_audio[] =
        "SELECT content_id FROM content WHERE path=?1 COLLATE BINARY "
        "AND analysisDataFilePath COLLATE BINARY IN (?2, ?3) LIMIT 2";
    const char *sql = where->physical_path_only ? path_only : with_audio;
    void *stmt = NULL;
    uint32_t id = 0;
    if (!az_storage_sql()->prepare(r->db, sql, -1, &stmt, NULL) && stmt &&
        fo_bind_locator(stmt, where, bindings) &&
        fo_bind_analysis_alias(stmt, &r->source, where->analysis_utf8, bindings))
        id = fo_unique_content(stmt);
    if (stmt) az_storage_sql()->finalize(stmt);
    return id;
}
static int fo_probe_files(struct fo_read *r, const struct st_resolution *row) {
    for (unsigned f = 0; f < 2; f++) {
        ss_file_probe(&r->source, row, f != 0, &r->files[f]);
        if (!r->files[f].path[0] || (r->files[f].rc && r->files[f].rc != AZ_UNSUPPORTED) ||
            (!r->files[f].rc && r->files[f].device != r->source.device)) return 0;
    }
    return !r->files[0].rc || !r->files[1].rc;
}
static int fo_open_files(struct fo_read *r, uint32_t id, unsigned required) {
    struct st_resolution row = {0};
    if (!id || st_read_row(r->db, id, &row) || row.analysis_limited || !st_source_file(&r->source, row.content, r->audio)) return 0;
    r->source.ref.track_id = id;
    if (!fo_probe_files(r, &row)) return 0;
    ss_arm_paths(&r->watch, r->files);
    return required ? fo_alive_files(r, required) : fo_alive(r);
}
static int fo_open(struct fo_read *r, const char *root, size_t length, const uint16_t *audio, size_t n,
                   const uint16_t *analysis, size_t m, const char *analysis_utf8, unsigned required) {
    struct fo_locator where = {root, analysis_utf8, audio, analysis, length, n, m, analysis_utf8 && !audio && !n};
    if (!fo_locator_valid(&where) || !fo_source_root(r, &where)) return 0;
    struct fo_sql_bindings bindings;
    if (!fo_sql_bindings(&bindings, analysis_utf8 != NULL) || !fo_open_database(r)) return 0;
    return fo_open_files(r, fo_find_content(r, &where, &bindings), required);
}
static struct fo_read *fo_alloc(void) {
    struct fo_read *r = calloc(1, sizeof *r);
    if (r) r->watch.fd = r->watch.wd[0] = r->watch.wd[1] = r->db_fd = -1;
    return r;
}
/* Takes ownership on every path. Pending reads cannot be evicted. */
static unsigned fo_available_slot(void) {
    unsigned slot = FO_READS;
    for (unsigned i = 0; i < FO_READS; i++) {
        if (!fo_reads[i]) return i;
        if (fo_reads[i]->complete == 1 &&
            !(atomic_load_explicit(&fo_custody[i].state, memory_order_acquire) & FO_COUNT_MASK) &&
            !fo_in_use(fo_reads[i]->id) && (slot == FO_READS || fo_reads[i]->id < fo_reads[slot]->id)) slot = i;
    }
    return slot;
}
static int fo_assign_slot(unsigned slot, struct fo_read *r, uint64_t id, struct fo_read **old) {
    uint64_t expected = fo_reads[slot] ? fo_reads[slot]->id << 16 : 0;
    if (!atomic_compare_exchange_strong_explicit(&fo_custody[slot].state, &expected, 0, memory_order_acq_rel, memory_order_acquire)) return 0;
    r->id = id;
    *old = fo_reads[slot];
    fo_reads[slot] = r;
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; i++) atomic_store_explicit(&fo_custody[slot].tokens[i], 0, memory_order_relaxed);
    atomic_store_explicit(&fo_custody[slot].lost, 0, memory_order_relaxed);
    atomic_store_explicit(&fo_custody[slot].state, id << 16, memory_order_release);
    return 1;
}
static uint64_t fo_insert(struct fo_read *r) {
    pthread_mutex_lock(&ss_mu);
    unsigned slot = fo_available_slot();
    struct fo_read *old = NULL;
    uint64_t id = 0;
    if (slot < FO_READS && (id = fo_new_id())) {
        if (!fo_assign_slot(slot, r, id, &old)) id = 0;
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
struct fo_descriptor_identity {
    uint64_t device, inode, signature;
};
static uint64_t fo_capture_files(uint64_t id, struct fo_descriptor_identity expected[2]) {
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
    return mount;
}
static int fo_descriptor_matches(int descriptor, uint64_t mount, const struct stat *identity, const struct fo_descriptor_identity *expected) {
    return expected->signature && expected->signature == st_stat_sig(identity) &&
           expected->device == (uint64_t) identity->st_dev && expected->inode == (uint64_t) identity->st_ino &&
           azsdk_sources_descriptor_identity(descriptor, mount, expected->device, expected->inode);
}
static unsigned fo_record_opened(uint64_t id, unsigned mask) {
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
static unsigned fo_descriptor_mask(int descriptor, uint64_t mount, const struct fo_descriptor_identity expected[2]) {
    struct stat identity;
    if (!mount || fstat(descriptor, &identity) || !S_ISREG(identity.st_mode)) return 0;
    unsigned mask = 0;
    for (unsigned f = 0; f < 2; f++)
        if (fo_descriptor_matches(descriptor, mount, &identity, &expected[f])) mask |= 1u << f;
    return mask;
}
static unsigned fo_opened(uint64_t id, int descriptor) {
    if (!id || descriptor < 0) return 0;
    struct fo_descriptor_identity expected[2] = {{0}};
    uint64_t mount = fo_capture_files(id, expected);
    unsigned mask = fo_descriptor_mask(descriptor, mount, expected);
    if (mask != 1 && mask != 2) return 0;
    return fo_record_opened(id, mask);
}
/* Verify the importer on its own storage thread, before it hands the cloned
 * rows to the separately owned native receiver. No ticket or deck is required. */
static void fo_physical_file(struct fo_scan *s, const struct fo_read *r, unsigned f) {
    const struct ss_file *file = &r->files[f];
    if (r->inherited && !(r->opened & (1u << f))) return;
    if (file->rc == AZ_UNSUPPORTED) return;
    size_t sizes[2] = {0};
    uint8_t *data = st_read_file_checked(file->path, &sizes[f], file->device, file->inode, file->signature);
    const uint8_t *input[2] = {0};
    input[f] = data;
    if (!data || !fo_input_bytes(r, f, data, sizes[f])) s->failed = 1;
    else
        for (unsigned hot = 0; hot < 2; hot++) fo_scan_list(s, input, sizes, hot, NULL, NULL);
    free(data);
}
static struct fo_scan *fo_physical(const struct fo_read *r) {
    struct fo_scan *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (unsigned f = 0; f < 2 && !s->failed; f++) fo_physical_file(s, r, f);
    return s;
}
static int fo_atoms_compatible(const struct fo_atom *a, const struct fo_atom *e) {
    return !a || !e || (a->hot == e->hot && a->slot == e->slot);
}
static int fo_row_atoms(const struct fo_scan *scan, const struct azn_file_cue_row *row, const struct fo_atom **a, const struct fo_atom **e) {
    *a = row->pcpt.total ? fo_match(scan, 0, &row->pcpt) : NULL;
    *e = row->pcp2.total ? fo_match(scan, 1, &row->pcp2) : NULL;
    if ((row->pcpt.total && !*a) || (row->pcp2.total && !*e)) return 0;
    return fo_atoms_compatible(*a, *e);
}
static int fo_identified(const struct fo_scan *s, const struct azn_file_cue_row *row, unsigned opened) {
    const struct fo_atom *a, *e;
    if (!fo_row_atoms(s, row, &a, &e)) return 0;
    unsigned needed = (a ? 1u << a->file : 0u) | (e ? 1u << e->file : 0u);
    return needed && (opened & needed) == needed;
}
static int fo_stamp_equal(const struct azn_file_atom *a, const struct azn_file_atom *b) {
    return a->header == b->header && a->total == b->total && !memcmp(a->digest, b->digest, sizeof a->digest);
}
static int fo_row_unique(const struct azn_file_cue_row *rows, size_t n, unsigned i) {
    for (unsigned j = 0; j < n; j++)
        if (i != j && ((rows[i].pcpt.total && fo_stamp_equal(&rows[i].pcpt, &rows[j].pcpt)) ||
                       (rows[i].pcp2.total && fo_stamp_equal(&rows[i].pcp2, &rows[j].pcp2)))) return 0;
    return 1;
}
static void fo_publish_rows(struct fo_read *r, unsigned slot, const struct fo_scan *physical,
                            const struct azn_file_cue_row *rows, size_t n, uint64_t *tokens) {
    r->kind = 1;
    r->count = (unsigned) n;
    memcpy(r->row, rows, n * sizeof *rows);
    for (unsigned i = 0; i < n; i++) {
        if (!fo_identified(physical, &rows[i], r->opened) || !fo_row_unique(rows, n, i)) continue;
        uint64_t serial = fo_new_id();
        tokens[i] = r->token[i] = serial ? (serial << 6) | slot : 0;
        atomic_store_explicit(&fo_custody[slot].tokens[i], tokens[i], memory_order_release);
    }
    r->complete = 1;
}
static int fo_rows_eligible(struct fo_read *r, const struct azn_file_cue_row *rows, size_t n, const uint64_t *tokens) {
    return n && n <= AZN_FILE_CUE_MAX && rows && tokens && fo_version_current(r) && fo_alive(r);
}
static int fo_physical_current(struct fo_read *r, const struct fo_scan *physical) {
    return physical && !physical->failed && fo_alive(r) && fo_version_current(r);
}
static void fo_complete(uint64_t id, const struct azn_file_cue_row *rows, size_t n, uint64_t tokens[AZN_FILE_CUE_MAX]) {
    if (tokens) memset(tokens, 0, AZN_FILE_CUE_MAX * sizeof *tokens);
    unsigned slot;
    struct fo_read *r = fo_claim_read(id, &slot);
    if (!r) return;
    struct fo_scan *physical = fo_rows_eligible(r, rows, n, tokens) ? fo_physical(r) : NULL;
    int ok = fo_physical_current(r, physical);
    if (r->db) ss_close_database(r->db, r->db_fd);
    r->db = NULL;
    pthread_mutex_lock(&ss_mu);
    if (ok) fo_publish_rows(r, slot, physical, rows, n, tokens);
    else {
        fo_reads[slot] = NULL;
        atomic_store_explicit(&fo_custody[slot].state, 0, memory_order_release);
    }
    pthread_mutex_unlock(&ss_mu);
    free(physical);
    if (!ok) fo_drop(r);
}
static int fo_custody_has_token(unsigned slot, const uint64_t *tokens, size_t n) {
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; i++) {
        uint64_t token = atomic_load_explicit(&fo_custody[slot].tokens[i], memory_order_acquire);
        for (size_t j = 0; token && j < n; j++)
            if (tokens[j] == token) return 1;
    }
    return 0;
}
static int fo_custody_count_invalid(uint64_t count, int acquire) {
    return (acquire && count == FO_COUNT_MASK) || (!acquire && !count);
}
static void fo_hold_slot(unsigned slot, uint64_t state, uint64_t generation, int acquire) {
    unsigned attempts = 0;
    for (; attempts < 16 && state >> 16 == generation; attempts++) {
        uint64_t count = state & FO_COUNT_MASK;
        if (fo_custody_count_invalid(count, acquire)) {
            atomic_store_explicit(&fo_custody[slot].lost, 1, memory_order_release);
            break;
        }
        uint64_t next = acquire ? state + 1 : state - 1;
        if (atomic_compare_exchange_strong_explicit(&fo_custody[slot].state, &state, next, memory_order_acq_rel, memory_order_acquire)) break;
    }
    if (attempts == 16 && state >> 16 == generation) atomic_store_explicit(&fo_custody[slot].lost, 1, memory_order_release);
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
        if (!generation || !fo_custody_has_token(slot, tokens, n)) continue;
        fo_hold_slot(slot, state, generation, acquire);
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
static int fo_edition_matches(const struct ss_binding *b, const struct fo_edition *ed) {
    return ed->revision && b->revision[AZ_STATUS_CUES] == ed->revision && b->provider[3] == ed->provider && !ed->scan.failed;
}
static int fo_binding_ready(const struct ss_binding *b, const struct fo_edition *ed, uint64_t epoch) {
    return b->epoch == epoch && b->cut == ed->cut && b->ready && !b->writing && !b->retired &&
           (b->topics & SS_TOPIC(CUES)) && !(b->pending & SS_TOPIC(CUES)) && !b->pinned[SS_SAVED_CUES] && fo_edition_matches(b, ed);
}
static int fo_reference_read(const struct fo_read *r, unsigned i, const struct ss_binding *b) {
    return r && r->complete == 1 && r->kind == 1 && !atomic_load_explicit(&fo_custody[i].lost, memory_order_acquire) &&
           atomic_load_explicit(&fo_custody[i].state, memory_order_acquire) >> 16 == r->id &&
           st_ref_equal(&r->source.ref, &b->ref) && !strcmp(r->audio, b->image.metadata.path) && !memcmp(r->files, b->image.file, sizeof r->files);
}
static int fo_both_selected(const struct fo_atom *a, const struct fo_atom *e) {
    return a && e && a->selected.type && e->selected.type;
}
static const struct fo_atom *fo_selected_loop(const struct fo_edition *ed, const struct azn_file_cue_row *row) {
    const struct fo_atom *a, *e;
    if (!fo_row_atoms(&ed->scan, row, &a, &e) || fo_both_selected(a, e)) return NULL;
    const struct fo_atom *selected = e && e->selected.type ? e : a;
    return selected && selected->selected.type == AZ_CUE_LOOP ? selected : NULL;
}
static int fo_resolve_loop(unsigned deck, struct fo_read *r, const struct ss_binding *b,
                           const struct fo_edition *ed, uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *out) {
    for (unsigned j = 0; j < r->count; j++) {
        if (r->token[j] != token) continue;
        if (!fo_alive(r)) return 0; /* the monitor resolves a pending certified grid edit */
        if (!fo_native_source(deck, b, &r->source)) return 0;
        const struct fo_atom *selected = fo_selected_loop(ed, &r->row[j]);
        if (!selected) return 0;
        *out = (az_status_cue_ref_v1) {.load_epoch = epoch, .provider_id = ed->provider, .collection_revision = ed->revision, .provenance = AZ_PROV_FILE, .cue_field = selected->hot ? AZ_STATUS_CUES_SAVED_HOT : AZ_STATUS_CUES_SAVED_MEMORY, .entry = selected->selected};
        fo_held[deck].cut = b->cut;
        fo_held[deck].read = r->id;
        return 1;
    }
    return 0;
}
static int fo_resolve_reference(unsigned deck, const struct ss_binding *b, const struct fo_edition *ed,
                                uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *out) {
    if (!fo_binding_ready(b, ed, epoch)) return 0;
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!fo_reference_read(r, i, b)) continue;
        if (fo_resolve_loop(deck, r, b, ed, epoch, token, out)) return 1;
    }
    return 0;
}
__attribute__((visibility("hidden"))) int azfile_status_ref(unsigned deck, uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *out) {
    if (deck >= 4 || !epoch || !token || !out) return 0;
    pthread_mutex_lock(&ss_mu);
    int found = fo_resolve_reference(deck, &ss_decks[deck], &fo_editions[deck], epoch, token, out);
    pthread_mutex_unlock(&ss_mu);
    return found;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the storage worker. The native side retains these witnesses
 * with the original parsed FileData/control, across all getter families. */
static int fi_path_valid(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    if (!path || !bytes || bytes >= 1024 || memchr(path, 0, bytes)) return 0;
    return (!audio && !units) || fo_unicode(audio, units);
}
static int fi_source_paths(const char *actual, size_t bytes, char root[1024], char analysis[1024]) {
    unsigned found = 0;
    for (int port = 0; port < 2; port++) {
        struct st_source source;
        if (st_source_read(port, &source)) continue;
        size_t mount = strlen(source.mount);
        if (bytes <= mount || memcmp(actual, source.mount, mount) || actual[mount] != '/') continue;
        if ((unsigned) ss_export_root(&source, root) >= 1024) return 0;
        memcpy(analysis, actual + mount, bytes - mount + 1);
        found++;
    }
    return found == 1;
}
static unsigned fi_extension(char *analysis) {
    char *extension = strrchr(analysis, '.');
    if (!extension) return 0;
    unsigned required;
    if (!strcmp(extension, ".DAT")) required = 1;
    else if (!strcmp(extension, ".EXT")) required = 2;
    else return 0;
    memcpy(extension, ".DAT", 5);
    return required;
}
static uint64_t fi_begin(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    if (!fi_path_valid(path, bytes, audio, units)) return 0;
    char actual[1024], root[1024], analysis[1024];
    memcpy(actual, path, bytes);
    actual[bytes] = 0;
    if (!fi_source_paths(actual, bytes, root, analysis)) return 0;
    unsigned required = fi_extension(analysis);
    if (!required) return 0;
    struct fo_read *read = fo_alloc();
    if (!read) return 0;
    read->input_mask = required;
    unsigned file = required == 1 ? 0u : 1u;
    if (!fo_open(read, root, strlen(root), audio, units, NULL, 0, analysis, required) ||
        read->files[file].rc || strcmp(read->files[file].path, actual)) {
        fo_drop(read);
        return 0;
    }
    return fo_insert(read);
}

static int fi_framed(const uint8_t *data, size_t size) {
    if (size > ST_FILE_MAX || !fo_container(data, size)) return 0;
    for (size_t at = hc_be32(data + 4); at < size;) {
        if (size - at < 12) return 0;
        uint32_t header = hc_be32(data + at + 4), total = hc_be32(data + at + 8);
        if (header < 12 || header > total || total > size - at) return 0;
        at += total;
    }
    return 1;
}

static int fi_seal_eligible(struct fo_read *read, int original_success, unsigned mask) {
    return original_success && (mask == 1 || mask == 2) && read->opened == mask && fo_version_current(read) && fo_alive_files(read, mask);
}
static int fi_seal_read(struct fo_read *read, int original_success) {
    unsigned mask = read->input_mask, file = mask == 1 ? 0u : 1u;
    if (!fi_seal_eligible(read, original_success, mask)) return 0;
    const struct ss_file *input = &read->files[file];
    size_t size = 0;
    uint8_t *data = st_read_file_checked(input->path, &size, input->device, input->inode, input->signature);
    int valid = fi_framed(data, size) && fo_version_current(read) && fo_alive_files(read, mask);
    if (valid) azsha256_hex(data, size, read->input_digest[file]);
    free(data);
    return valid;
}
static uint64_t fi_seal_token(struct fo_read *read, unsigned slot, int valid) {
    uint64_t token = 0;
    pthread_mutex_lock(&ss_mu);
    uint64_t serial = valid ? fo_new_id() : 0;
    if (serial) {
        read->kind = 3;
        read->count = read->complete = 1;
        token = read->token[0] = (serial << 6) | slot;
        atomic_store_explicit(&fo_custody[slot].tokens[0], token, memory_order_release);
        /* Initial custody is transferred to the native FileData registration. */
        atomic_store_explicit(&fo_custody[slot].state, (read->id << 16) | 1, memory_order_release);
    } else {
        fo_reads[slot] = NULL;
        atomic_store_explicit(&fo_custody[slot].state, 0, memory_order_release);
    }
    pthread_mutex_unlock(&ss_mu);
    return token;
}
static uint64_t fi_seal(uint64_t id, int original_success) {
    unsigned slot;
    struct fo_read *read = fo_claim_read(id, &slot);
    if (!read) return 0;
    int valid = fi_seal_read(read, original_success);
    if (read->db) ss_close_database(read->db, read->db_fd);
    read->db = NULL;
    uint64_t token = fi_seal_token(read, slot, valid);
    if (!token) fo_drop(read);
    return token;
}

/* Called under ss_mu. The native caller separately proves that this exact
 * token still belongs to the borrowed original FileData/control pair. */
static int fi_token_matches(const struct fo_read *read, uint64_t token, unsigned file, unsigned slot) {
    return read && read->kind == 3 && read->complete == 1 && read->count == 1 &&
           read->token[0] == token && read->input_mask == (1u << file) &&
           !atomic_load_explicit(&fo_custody[slot].lost, memory_order_acquire) &&
           atomic_load_explicit(&fo_custody[slot].state, memory_order_acquire) >> 16 == read->id;
}
static struct fo_read *fi_token(uint64_t token, unsigned file) {
    if (!token) return NULL;
    unsigned slot = (unsigned) (token & 63u);
    struct fo_read *read = fo_reads[slot];
    if (!fi_token_matches(read, token, file, slot)) return NULL;
    if (!fo_alive_files(read, read->input_mask)) {
        read->count = 0;
        return NULL;
    }
    return read;
}

static int fi_inherit_input(struct fo_read *read, const struct fo_read *original, unsigned file) {
    if (!original) return 0;
    if (read->opened && (!st_resolution_source_equal(&read->source, &original->source) || strcmp(read->audio, original->audio))) return 0;
    if (!read->opened) {
        read->source = original->source;
        memcpy(read->audio, original->audio, sizeof read->audio);
        memcpy(read->files, original->files, sizeof read->files);
    }
    read->files[file] = original->files[file];
    memcpy(read->input_digest[file], original->input_digest[file], sizeof read->input_digest[file]);
    read->opened |= 1u << file;
    return 1;
}
static int fi_inherit_sources(struct fo_read *read, const uint64_t tokens[2]) {
    int valid = 1;
    pthread_mutex_lock(&ss_mu);
    for (unsigned file = 0; file < 2 && valid; file++) {
        if (!tokens[file]) continue;
        valid = fi_inherit_input(read, fi_token(tokens[file], file), file);
    }
    if (valid) fo_hold(tokens, 2, 1);
    pthread_mutex_unlock(&ss_mu);
    return valid;
}
static int fi_install_watch(struct fo_read *read, const uint64_t tokens[2]) {
    /* An absent participant supplies no origin. Its current metadata only
     * guards selection against a new overriding companion in the public view. */
    struct st_resolution row = {0};
    memcpy(row.analysis, read->files[0].relative, sizeof row.analysis);
    for (unsigned file = 0; file < 2; file++)
        if (!tokens[file]) ss_file_probe(&read->source, &row, file != 0, &read->files[file]);
    ss_arm_paths(&read->watch, read->files);
    /* Install the receiving watch before checking the original watches again.
     * This closes the handoff interval even if file bytes later repeat. */
    int valid = 1;
    pthread_mutex_lock(&ss_mu);
    for (unsigned file = 0; file < 2 && valid; file++)
        if (tokens[file] && !fi_token(tokens[file], file)) valid = 0;
    pthread_mutex_unlock(&ss_mu);
    valid = valid && fo_alive(read);
    fo_hold(tokens, 2, 0);
    return valid;
}
static uint64_t fi_join(const uint64_t inputs[2]) {
    if (!inputs || (!inputs[0] && !inputs[1])) return 0;
    uint64_t tokens[2] = {inputs[0], inputs[1]};
    struct fo_read *read = fo_alloc();
    if (!read) return 0;
    if (!fi_inherit_sources(read, tokens) || !fi_install_watch(read, tokens)) {
        fo_drop(read);
        return 0;
    }
    read->inherited = 1;
    return fo_insert(read);
}

static const struct azn_file_input_api *fi_api(void) {
    static const struct azn_file_input_api api = {sizeof api, 1, fi_begin, fo_opened, fi_seal, fo_hold, fi_join};
    return &api;
}

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
    if (!ds_proof_is_enabled()) return ss_publish_image(deck, b, topics, parsed, 0);
    struct ds_proof_context next = ss_proof_context(b, DS_PROOF_BASELINE, 0);
    next.topics = topics;
    struct ds_proof_context previous = ds_proof_enter(&next);
    int rc = ss_publish_image(deck, b, topics, parsed, 0);
    ds_proof_restore(&previous);
    return rc;
}
static int ss_file_current(const struct st_source *src, const struct ss_file *f) {
    if (!f->relative[0]) return 1;
    char path[1024] = "";
    st_source_file(src, f->relative, path);
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
        /* Move overlapping pointer slots, preserving the FIFO order of owned jobs. */
        // NOLINTNEXTLINE(bugprone-sizeof-expression): queue elements are pointers, not saved-revision records.
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
    if (st_source_database(source, path)) {
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
void ss_native_load(unsigned deck, uint64_t epoch, uint64_t token) {
    pthread_mutex_lock(&ss_mu);
    if (ss_pins[deck].epoch != epoch || ss_pins[deck].token != token) {
        memset(&ss_pins[deck], 0, sizeof ss_pins[deck]);
        ss_pins[deck].epoch = epoch;
        ss_pins[deck].token = token;
    }
    pthread_mutex_unlock(&ss_mu);
    if (track_service_status_started()) ss_signal();
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
    if (!ss_started && az_storage_sql()->ok > 0 && !track_service_start_worker() && ss_open_wakeup()) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        struct sched_param priority = {0};
        pthread_attr_setschedparam(&attr, &priority);
        if (!az_thread_create(&thread, &attr, ss_observe, NULL)) {
            track_service_status_register(ss_work);
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
void sdk_status_store_interest(unsigned deck, uint64_t topics) {
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
    if ((topics || track_service_status_started()) && ss_start()) {
        ss_signal();
        track_service_wake();
    }
}
/* A native-preferred field was withdrawn. Re-offer the qualified file projection
 * only for this load and active interest, through the normal freshness checks. */
void sdk_status_store_reseed(unsigned deck, uint64_t epoch, uint64_t topics) {
    if (deck >= 4 || !epoch) return;
    pthread_mutex_lock(&ss_mu);
    struct ss_binding *d = &ss_decks[deck];
    int wake = d->epoch == epoch && d->ready && !d->retired && (d->topics & ss_topics(topics));
    if (wake) d->pending |= d->topics & ss_topics(topics);
    pthread_mutex_unlock(&ss_mu);
    if (wake) track_service_wake();
}
void sdk_status_store_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t cut) {
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
    if (track_service_status_started()) ss_signal();
}

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
static uint32_t sw_codepoint(const uint16_t *units, size_t count, size_t *index) {
    uint32_t codepoint = units[*index];
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
        if (++*index >= count || units[*index] < 0xdc00 || units[*index] > 0xdfff) return 0;
        return 0x10000 + ((codepoint - 0xd800) << 10) + (units[*index] - 0xdc00);
    }
    return codepoint >= 0xdc00 && codepoint <= 0xdfff ? 0 : codepoint;
}
static unsigned sw_utf8(uint32_t codepoint, unsigned char bytes[4]) {
    unsigned count = 0;
    if (codepoint <= 0x7f) bytes[count++] = (unsigned char) codepoint;
    else {
        if (codepoint < 0x800) bytes[count++] = 0xc0 | (codepoint >> 6);
        else {
            if (codepoint < 0x10000) bytes[count++] = 0xe0 | (codepoint >> 12);
            else {
                bytes[count++] = 0xf0 | (codepoint >> 18);
                bytes[count++] = 0x80 | ((codepoint >> 12) & 63);
            }
            bytes[count++] = 0x80 | ((codepoint >> 6) & 63);
        }
        bytes[count++] = 0x80 | (codepoint & 63);
    }
    return count;
}
static int sw_name(const uint16_t *units, size_t count, const char *text) {
    if ((!units && count) || count > 1023 || !text) return 0;
    const unsigned char *cursor = (const unsigned char *) text;
    for (size_t i = 0; i < count; i++) {
        uint32_t codepoint = sw_codepoint(units, count, &i);
        if (!codepoint) return 0;
        unsigned char bytes[4];
        unsigned length = sw_utf8(codepoint, bytes);
        for (unsigned j = 0; j < length; j++)
            if (*cursor++ != bytes[j]) return 0;
    }
    return !*cursor;
}
static int sw_binding_ready(const struct ss_binding *binding) {
    return binding->ready && !binding->retired && binding->epoch &&
           (binding->topics & (SS_DAT | SS_EXT)) && !binding->image.rc;
}
struct sw_locator {
    const char *root;
    size_t root_length;
    const uint16_t *audio, *analysis;
    size_t audio_length, analysis_length;
};
static int sw_binding_paths(const struct ss_binding *binding, const struct sw_locator *locator) {
    const char *mount = binding->image.src.mount;
    size_t length = strlen(mount);
    return ss_export_matches(&binding->image.src, locator->root, locator->root_length) &&
           (binding->image.metadata.valid & AZ_META_PATH) &&
           !strncmp(binding->image.metadata.path, mount, length) &&
           sw_name(locator->audio, locator->audio_length, binding->image.metadata.path + length) &&
           sw_name(locator->analysis, locator->analysis_length, binding->image.file[0].relative);
}
static void sw_ticket_start(struct sw_batch *batch, unsigned deck, struct ss_binding *binding) {
    struct sw_ticket *ticket = &batch->item[batch->count++];
    ticket->deck = deck;
    ticket->epoch = binding->epoch;
    ticket->cut = binding->cut;
    ticket->ref = binding->ref;
    ticket->started = ss_new_id(&ss_serial);
    if (binding->writing) binding->overlapping = 1;
    ++binding->writing;
    binding->serial = ticket->started;
    memcpy(ticket->file, binding->image.file, sizeof ticket->file);
}
static void sw_start(struct sw_batch *batch, const char *root, size_t root_n,
                     const uint16_t *audio, size_t audio_n, const uint16_t *analysis, size_t analysis_n) {
    if (!root || root_n > 1023 || !audio_n || !analysis_n) return;
    struct sw_locator locator = {root, root_n, audio, analysis, audio_n, analysis_n};
    pthread_mutex_lock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) {
        struct ss_binding *binding = &ss_decks[deck];
        if (sw_binding_ready(binding) && sw_binding_paths(binding, &locator)) sw_ticket_start(batch, deck, binding);
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
        track_service_wake();
    }
    batch->count = 0;
}
/* One native save may concern the same track on several units. Share the
 * immutable containers and storage revision, but keep each unit's individual
 * epoch, subscriber interval, native witness and provider incarnation. */
struct sw_observation {
    const struct sw_batch *batch;
    const struct sw_ticket *first;
    unsigned index, members, lane, nfiles;
    uint64_t topics, observed, revision;
    struct st_source source;
    struct ss_saved_revision *saved;
    struct ss_write_proof proof[2];
    char siblings[2][65];
};
static unsigned sw_group_members(const struct sw_batch *batch, unsigned index) {
    const struct sw_ticket *first = &batch->item[index];
    unsigned members = 0;
    for (unsigned i = index; i < batch->count; i++)
        if (st_ref_equal(&first->ref, &batch->item[i].ref) &&
            !memcmp(first->file, batch->item[i].file, sizeof first->file)) members |= 1u << i;
    return members;
}
static int sw_saved_file_path(const struct ss_file *file, const struct st_source *source, struct stat *status) {
    char path[1024];
    return file->path[0] && st_source_file(source, file->relative, path) && !strcmp(path, file->path) &&
           !stat(path, status) && S_ISREG(status->st_mode) &&
           !((uint64_t) status->st_dev == source->device && (uint64_t) status->st_ino == source->inode);
}
static void sw_read_saved_file(struct sw_observation *observation, unsigned index) {
    struct ss_file file = observation->first->file[index];
    struct ss_saved_revision *saved = observation->saved;
    struct ss_write_proof *proof = &observation->proof[index];
    struct stat status;
    if (saved) saved->rc[index] = file.rc;
    if (!sw_saved_file_path(&file, &observation->source, &status)) return;
    file.signature = st_stat_sig(&status);
    file.device = (uint64_t) status.st_dev;
    file.inode = (uint64_t) status.st_ino;
    if (!ss_read_digest(&file, proof->digest, saved ? &saved->bytes[index] : NULL, saved ? &saved->sizes[index] : NULL)) return;
    proof->device = file.device;
    proof->inode = file.inode;
    proof->signature = file.signature;
    if (saved && saved->bytes[index]) {
        saved->rc[index] = AZ_OK;
        saved->total += saved->sizes[index];
        ss_sibling_digest(observation->lane, saved->bytes[index], saved->sizes[index], observation->siblings[index]);
    }
}
static void sw_revision_start(struct sw_observation *observation) {
    observation->revision = ss_new_id(&ss_serial);
    struct ss_saved_revision *saved = observation->saved;
    if (!saved) return;
    saved->ref = observation->first->ref;
    saved->revision = observation->revision;
    saved->observed = observation->observed;
    saved->lane = observation->lane;
}
static int sw_target_current(const struct ss_binding *binding, const struct sw_ticket *ticket) {
    return binding->cut == ticket->cut && binding->epoch == ticket->epoch && !binding->retired &&
           st_ref_equal(&binding->ref, &ticket->ref);
}
static uint64_t sw_single_lane_proof(const struct sw_observation *observation, const struct ss_binding *binding,
                                     unsigned index, int same_file) {
    return !binding->overlapping && same_file && observation->siblings[index][0] &&
                   !memcmp(observation->siblings[index], binding->siblings[observation->lane][index], sizeof observation->siblings[index])
               ? observation->topics
               : 0;
}
static int sw_target_file_proof(const struct sw_observation *observation, struct ss_binding *binding, unsigned index) {
    const struct ss_write_proof *proof = &observation->proof[index];
    const struct ss_file *file = &observation->first->file[index];
    if (!proof->signature) return file->rc == AZ_UNSUPPORTED;
    int same_file = proof->device == file->device && proof->inode == file->inode;
    int complete = observation->saved && observation->saved->bytes[index] && same_file;
    if (binding->writing > 1) return complete;
    struct ss_write_proof done = *proof;
    done.revision = observation->revision;
    done.ordinal = binding->completed[index].ordinal + 1;
    done.only = sw_single_lane_proof(observation, binding, index, same_file);
    binding->completed[index] = done;
    return complete;
}
static int sw_target_complete(const struct sw_observation *observation, struct ss_binding *binding) {
    int complete = observation->saved && observation->saved->total && !binding->overlapping && observation->revision;
    for (unsigned index = 0; index < observation->nfiles; index++) {
        int file_complete = sw_target_file_proof(observation, binding, index);
        if (!file_complete) complete = 0;
    }
    return complete;
}
static void sw_pin_target(const struct sw_observation *observation, unsigned deck, struct ss_binding *binding) {
    struct ss_saved_revision *saved = observation->saved;
    unsigned lane = observation->lane;
    saved->targets |= 1u << deck;
    saved->target[deck].cut = binding->cut;
    saved->target[deck].epoch = binding->epoch;
    saved->target[deck].interest = binding->interest[lane];
    saved->target[deck].provider = binding->provider[ss_saved_provider[lane]];
    saved->target[deck].source_pin = ss_pins[deck].epoch == binding->epoch ? ss_pins[deck].token : 0;
    ++binding->pinned[lane];
    binding->pending &= ~observation->topics;
}
static void sw_queue_target(const struct sw_observation *observation, unsigned deck, struct ss_binding *binding, int complete) {
    if (!(binding->topics & observation->topics)) return;
    if (complete && ss_saved_count < SS_SAVE_QUEUE && observation->saved->total <= SS_SAVE_PIN_BYTES - ss_saved_bytes)
        sw_pin_target(observation, deck, binding);
    else {
        binding->interest[observation->lane] = ss_new_id(&ss_serial);
        binding->pinned[observation->lane] = 0;
        binding->pending |= observation->topics;
        ds_publish_file(deck, binding->epoch, &binding->ref, observation->observed, NULL, 0, observation->topics, AZ_STATUS_REVISION_LOST);
    }
}
static void sw_publish_group(struct sw_observation *observation) {
    pthread_mutex_lock(&ss_mu);
    sw_revision_start(observation);
    for (unsigned i = observation->index; i < observation->batch->count; i++) {
        if (!(observation->members & (1u << i))) continue;
        const struct sw_ticket *ticket = &observation->batch->item[i];
        struct ss_binding *binding = &ss_decks[ticket->deck];
        if (!sw_target_current(binding, ticket)) continue;
        int complete = sw_target_complete(observation, binding);
        sw_queue_target(observation, ticket->deck, binding, complete);
    }
    if (observation->saved && observation->saved->targets) {
        ss_saved_queue[ss_saved_count++] = observation->saved;
        ss_saved_bytes += observation->saved->total;
        observation->saved = NULL;
    }
    pthread_mutex_unlock(&ss_mu);
}
static void sw_finish_group(struct sw_observation *observation) {
    if (!observation->first->started || ss_source(&observation->first->ref, &observation->source)) return;
    observation->saved = calloc(1, sizeof *observation->saved);
    for (unsigned i = 0; i < observation->nfiles; i++) sw_read_saved_file(observation, i);
    if (!ss_source(&observation->first->ref, &observation->source)) sw_publish_group(observation);
    ss_release_saved(observation->saved);
}
static void sw_finish(struct sw_batch *batch) {
    unsigned handled = 0;
    uint64_t observed = st_now();
    for (unsigned i = 0; i < batch->count; i++) {
        if (handled & (1u << i)) continue;
        struct sw_observation observation = {.batch = batch, .first = &batch->item[i], .index = i, .members = sw_group_members(batch, i), .lane = batch->lane, .nfiles = batch->lane == SS_SAVED_GRID ? 1u : 2u, .topics = AZ_STATUS_TOPIC(ss_saved_topic[batch->lane]), .observed = observed};
        handled |= observation.members;
        sw_finish_group(&observation);
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
static uintptr_t sw_ptr(uintptr_t object, unsigned offset) {
    return __atomic_load_n((const uintptr_t *) (object + offset), __ATOMIC_ACQUIRE);
}
static void sw_capture(struct sw_batch *b, uintptr_t accessor, uintptr_t identity) {
    if (!atomic_load_explicit(&sw_ready, memory_order_acquire) || !accessor || !identity) return;
    uintptr_t storage = sw_ptr(accessor, 0x18);
    if (storage)
        sw_start(b, (const char *) sw_ptr(storage, 8), sw_ptr(storage, 16),
                 (const uint16_t *) sw_ptr(identity, 0), sw_ptr(identity, 8),
                 (const uint16_t *) sw_ptr(identity, 32), sw_ptr(identity, 40));
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
int sdk_status_save_install(int build, azjs_installer install) {
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

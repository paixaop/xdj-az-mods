/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared stored-track provider. Included by community.c after sdk_lib.c. This provider
 * uses only existing library services and files, so it adds no EP147 address mappings. */
#include "sdk_data_engine.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include "utf8_cut.h"
#include "sdk_wave_provider.h"
#include "../common/azsources.h"
static struct st_engine g_st = ST_ENGINE_INIT;
static pthread_mutex_t g_st_source_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_st_started;
static int g_st_enabled;
static uint64_t g_st_generation;
struct st_source {
    az_track_ref ref;
    uint64_t mount_id, device, inode;
    char mount[1024];
};
static struct st_source g_st_sources[2];
/* Worker-only, one positive locator per physical source. This holds copied
 * values, never a SQLite handle, statement, native pointer or waveform window. */
struct st_resolution {
    struct st_source source;
    uint64_t signature;
    az_track_metadata metadata;
    char database[1024], content[2048], analysis[1024];
    int valid, analysis_limited;
};
static struct st_resolution g_st_resolutions[2];
static int st_source_path(const char *mount, const char *relative, char out[1024]);
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out);
static uint32_t sd_live_fields(void);
static uint64_t sdk_command_capabilities(void);
static void sdk_command_retire_owner(int owner);
/* A stat fingerprint detects file replacement and edits, including nanosecond timestamps.
 * It is a freshness hint, not a content hash or a permanent library identifier. */
static uint64_t st_disk_sig(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    uint64_t v = (uint64_t) st.st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) st.st_ino;
    v = v * 1099511628211ULL ^ (uint64_t) st.st_size;
    v = v * 1099511628211ULL ^ (uint64_t) st.st_mtim.tv_sec;
    v = v * 1099511628211ULL ^ (uint64_t) st.st_mtim.tv_nsec;
    return v ? v : 1;
}
#ifndef ST_MOUNTINFO
#define ST_MOUNTINFO "/proc/self/mountinfo"
#endif
static uint64_t st_mount_id(const char *mount) {
    FILE *f = fopen(ST_MOUNTINFO, "re");
    if (!f) return 0;
    char line[4096], mp[2048];
    unsigned long long id = 0, found = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%llu %*u %*s %*s %2047s", &id, mp) == 2 && !strcmp(mp, mount)) found = id;
    fclose(f);
    return found;
}
/* Worker only. Resolve by physical USB port and mount lifecycle; never by title/content ID. */
static int st_source_read(int stick, struct st_source *out) {
    if (stick < 0 || stick > 1) return AZ_INVALID;
    char mount[1024], db[1100];
    struct stat sb;
    int ok = sdk_stick_mount(stick, mount, sizeof mount);
    uint64_t mid = ok ? st_mount_id(mount) : 0;
    if (ok) ok = st_source_path(mount, "/PIONEER/rekordbox/exportLibrary.db", db);
    ok = ok && mid && !stat(db, &sb) && S_ISREG(sb.st_mode);
    pthread_mutex_lock(&g_st_source_mu);
    struct st_source *s = &g_st_sources[stick];
    if (!ok) {
        memset(s, 0, sizeof *s);
        pthread_mutex_unlock(&g_st_source_mu);
        return AZ_STALE;
    }
    uint64_t lib = (uint64_t) sb.st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) sb.st_ino;
    if (!lib) lib = 1;
    if (s->mount_id != mid || s->device != (uint64_t) sb.st_dev || s->inode != (uint64_t) sb.st_ino || strcmp(s->mount, mount)) {
        memset(s, 0, sizeof *s);
        if (g_st_generation == UINT64_MAX) {
            pthread_mutex_unlock(&g_st_source_mu);
            return AZ_LIMIT;
        }
        s->mount_id = mid;
        s->device = (uint64_t) sb.st_dev;
        s->inode = (uint64_t) sb.st_ino;
        s->ref = (az_track_ref) {sizeof(az_track_ref), AZ_DATA_VERSION, (uint64_t) stick + 1, ++g_st_generation, lib, 0, 0};
        snprintf(s->mount, sizeof s->mount, "%s", mount);
    }
    *out = *s;
    pthread_mutex_unlock(&g_st_source_mu);
    return AZ_OK;
}
/* Resolve every path inside its source. A database row cannot escape into another stick or
 * arbitrary core storage, even through symlinks or a ../ component. */
static int st_source_path(const char *mount, const char *relative, char out[1024]) {
    if (!relative || relative[0] != '/') return 0;
    char joined[2048], real[PATH_MAX], root[PATH_MAX];
    if (snprintf(joined, sizeof joined, "%s%s", mount, relative) >= (int) sizeof joined ||
        !realpath(mount, root)) return 0;
    if (!realpath(joined, real)) {
        if (errno != ENOENT) return 0;
        /* Keep missing descendants in the freshness set, including missing parent
         * directories. Resolve the existing prefix and reject unresolved traversal. */
        char prefix[sizeof joined];
        memcpy(prefix, joined, strlen(joined) + 1);
        char parent[PATH_MAX];
        for (;;) {
            char *slash = strrchr(prefix, '/');
            if (!slash || slash == prefix) return 0;
            *slash = 0;
            if (realpath(prefix, parent)) break;
            if (errno != ENOENT) return 0;
        }
        const char *tail = joined + strlen(prefix);
        for (const char *p = tail; *p;) {
            if (*p == '/') {
                p++;
                continue;
            }
            const char *end = strchr(p, '/');
            size_t len = end ? (size_t) (end - p) : strlen(p);
            if (len == 2 && !memcmp(p, "..", 2)) return 0;
            p += len;
        }
        if (snprintf(real, sizeof real, "%s%s", parent, tail) >= (int) sizeof real) return 0;
    }
    size_t n = strlen(root);
    if (strncmp(real, root, n) || real[n] != '/' || strlen(real) >= 1024) return 0;
    snprintf(out, 1024, "%s", real);
    return 1;
}
/* immutable SQLite reads cannot safely ignore a live WAL or rollback journal. */
static int st_database_quiet(const char *db) {
    const char *suffix[] = {"-wal", "-journal"};
    for (unsigned i = 0; i < 2; i++) {
        char path[1100];
        if (snprintf(path, sizeof path, "%s%s", db, suffix[i]) >= (int) sizeof path) return 0;
        struct stat sb;
        if (!stat(path, &sb)) {
            if (!S_ISREG(sb.st_mode) || sb.st_size > 0) return 0;
        } else if (errno != ENOENT) return 0;
    }
    return 1;
}
static uint8_t *st_read_file(const char *path, size_t *size) {
    /* A path can change after the fingerprint check; never wait for a FIFO writer
     * before fstat establishes that the opened object is a regular file. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat sb;
    uint8_t *b = NULL;
    if (!fstat(fd, &sb) && S_ISREG(sb.st_mode) && sb.st_size >= 12 && (uint64_t) sb.st_size <= ST_FILE_MAX) {
        *size = (size_t) sb.st_size;
        b = malloc(*size);
        if (b) {
            size_t n = 0;
            while (n < *size) {
                ssize_t k = read(fd, b + n, *size - n);
                if (k <= 0) break;
                n += (size_t) k;
            }
            if (n != *size) {
                free(b);
                b = NULL;
            }
        }
    }
    close(fd);
    return b;
}
static void st_resolution_retire(const az_track_ref *ref, const char *database) {
    if (ref->source_id < 1 || ref->source_id > 2) return;
    struct st_resolution *cached = &g_st_resolutions[ref->source_id - 1];
    az_track_ref source = cached->source.ref;
    source.track_id = ref->track_id;
    /* An old snapshot must not retire a newer mount/DB incarnation. All tracks
     * on the matching source/database share its quiet/fingerprint condition. */
    if (cached->valid && st_ref_equal(ref, &source) &&
        (!database || !strcmp(database, cached->database))) cached->valid = 0;
}
static int st_disk_fresh(const az_track_ref *ref, const struct st_data *data, void *ctx) {
    (void) ctx;
    struct st_source src;
    if (ref->source_id < 1 || ref->source_id > 2 || st_source_read((int) ref->source_id - 1, &src)) {
        st_resolution_retire(ref, NULL);
        return 0;
    }
    src.ref.track_id = ref->track_id;
    if (!st_ref_equal(ref, &src.ref)) {
        st_resolution_retire(ref, NULL);
        return 0;
    }
    char database[1024];
    if (!st_source_path(src.mount, "/PIONEER/rekordbox/exportLibrary.db", database) ||
        strcmp(database, data->files[0]) || !st_database_quiet(data->files[0]) ||
        !data->signatures[0] || st_disk_sig(data->files[0]) != data->signatures[0]) {
        st_resolution_retire(ref, data->files[0]);
        return 0;
    }
    for (int i = 1; i < 3; i++) {
        if (data->analysis_paths[i - 1][0]) {
            char current[1024] = "";
            st_source_path(src.mount, data->analysis_paths[i - 1], current);
            if (strcmp(current, data->files[i])) return 0;
        }
        if (data->files[i][0] && st_disk_sig(data->files[i]) != data->signatures[i]) return 0;
    }
    return 1;
}
static int st_resolution_source_equal(const struct st_source *a, const struct st_source *b) {
    return st_ref_equal(&a->ref, &b->ref) && a->mount_id == b->mount_id &&
           a->device == b->device && a->inode == b->inode && !strcmp(a->mount, b->mount);
}
/* Recheck after SQL and before publishing a positive locator. A stat signature
 * has the same freshness-hint limits as stored snapshots; it is not a DB hash. */
static int st_resolution_current(const struct st_source *source, const char *database, uint64_t signature) {
    struct st_source current;
    char path[1024];
    if (!signature || st_source_read((int) source->ref.source_id - 1, &current)) return 0;
    current.ref.track_id = source->ref.track_id;
    return st_resolution_source_equal(source, &current) &&
           st_source_path(current.mount, "/PIONEER/rekordbox/exportLibrary.db", path) &&
           !strcmp(database, path) && st_database_quiet(path) && st_disk_sig(path) == signature;
}
static int st_disk_load(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    (void) ctx;
    struct st_source src;
    if (ref->source_id < 1 || ref->source_id > 2) return AZ_STALE;
    struct st_resolution *cached = &g_st_resolutions[ref->source_id - 1];
    if (st_source_read((int) ref->source_id - 1, &src)) {
        cached->valid = 0;
        return AZ_STALE;
    }
    src.ref.track_id = ref->track_id;
    if (!st_ref_equal(ref, &src.ref)) {
        cached->valid = 0;
        return AZ_STALE;
    }
    /* SQLite resolves an in-source symlink to its target; sidecars and freshness must
     * use that same physical filename rather than the alias used by the export. */
    if (!st_source_path(src.mount, "/PIONEER/rekordbox/exportLibrary.db", data->files[0])) return AZ_STALE;
    if (!st_database_quiet(data->files[0])) {
        cached->valid = 0;
        return AZ_STALE;
    }
    data->signatures[0] = st_disk_sig(data->files[0]);
    if (!data->signatures[0]) {
        cached->valid = 0;
        return AZ_STALE;
    }
    static const char query[] = "SELECT c.title,a.name,al.name,k.name,c.path,c.bpmx100,c.length,c.analysisDataFilePath "
                                "FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist "
                                "LEFT JOIN album al ON al.album_id=c.album_id LEFT JOIN key k ON k.key_id=c.key_id "
                                "WHERE c.content_id=?1";
    int rc = AZ_OK;
    struct st_resolution row;
    int reuse = cached->valid && st_resolution_source_equal(&cached->source, &src) &&
                cached->signature == data->signatures[0] && !strcmp(cached->database, data->files[0]);
    if (reuse) row = *cached;
    else {
        cached->valid = 0;
        memset(&row, 0, sizeof row);
        row.source = src;
        row.signature = data->signatures[0];
        snprintf(row.database, sizeof row.database, "%s", data->files[0]);
        row.metadata.size = sizeof row.metadata;
        row.metadata.version = AZ_DATA_VERSION;
        void *db = sdk_db_open(data->files[0]), *stmt = NULL;
        if (!db) return AZ_FAILED;
        rc = AZ_FAILED;
        if (!g_sdk_sq.prepare(db, query, -1, &stmt, NULL) && stmt &&
            !g_sdk_sq.bind_int64(stmt, 1, ref->track_id)) {
            int step = g_sdk_sq.step(stmt);
            if (step == 100) {
                az_track_metadata *m = &row.metadata;
                char *texts[] = {m->title, m->artist, m->album, m->key};
                size_t caps[] = {sizeof m->title, sizeof m->artist, sizeof m->album, sizeof m->key};
                for (int i = 0; i < 4; i++) {
                    const unsigned char *v = g_sdk_sq.col_text(stmt, i);
                    if (v) {
                        snprintf(texts[i], caps[i], "%s", v);
                        utf8_cut(texts[i]);
                        m->valid |= 1u << i;
                    }
                }
                const unsigned char *path = g_sdk_sq.col_text(stmt, 4), *ap = g_sdk_sq.col_text(stmt, 7);
                /* Oversized raw paths could not fit st_source_path's joined buffer
                 * either. Do not retain a truncated prefix as an audio locator. */
                if (path && snprintf(row.content, sizeof row.content, "%s", path) >= (int) sizeof row.content)
                    row.content[0] = 0;
                m->original_bpm = g_sdk_sq.col_int64(stmt, 5) / 100.0;
                m->duration_s = (double) g_sdk_sq.col_int64(stmt, 6);
                if (m->original_bpm > 0) m->valid |= AZ_META_TEMPO;
                if (m->duration_s > 0) m->valid |= AZ_META_DURATION;
                rc = AZ_OK;
                if (ap && snprintf(row.analysis, sizeof row.analysis, "%s", ap) >= (int) sizeof row.analysis) {
                    row.analysis_limited = 1;
                    row.analysis[0] = 0;
                }
                /* A duplicate content id is ambiguous inside this exact library as well. */
                if (g_sdk_sq.step(stmt) != 101) rc = AZ_FAILED;
            } else if (step == 101) rc = AZ_NOT_FOUND;
        }
        if (stmt) g_sdk_sq.finalize(stmt);
        g_sdk_sq.close(db);
        if (rc) return rc;
    }
    if (row.analysis_limited && (mask & ~AZ_DATA_METADATA)) rc = AZ_LIMIT;
    if (rc) return rc;
    if (!reuse && !st_resolution_current(&src, data->files[0], data->signatures[0])) {
        cached->valid = 0;
        return AZ_STALE;
    }
    if (!reuse && !row.analysis_limited) {
        row.valid = 1;
        *cached = row;
    }
    data->view.metadata = row.metadata;
    char real[1024];
    if (row.content[0] && st_source_path(src.mount, row.content, real) && strlen(real) < sizeof data->view.metadata.path) {
        snprintf(data->view.metadata.path, sizeof data->view.metadata.path, "%s", real);
        data->view.metadata.valid |= AZ_META_PATH;
    }
    const char *analysis = row.analysis;
    if (mask & AZ_DATA_METADATA) data->view.metadata_info.availability = AZ_AVAIL_READY;
    if (mask == ST_DETAIL) {
        char relative[1024];
        if (!analysis[0]) return AZ_UNSUPPORTED;
        snprintf(relative, sizeof relative, "%s", analysis);
        char *dot = strrchr(relative, '.');
        if (!dot || strcmp(dot, ".DAT")) return AZ_UNSUPPORTED;
        uint32_t file_encoding = st_wave_file_encoding(data->wave_query.encoding);
        unsigned analysis_slot = sww_is_mono_preview(file_encoding) ? 0u : 1u;
        if (analysis_slot)
            memcpy(dot, (file_encoding == AZ_WAVE_PWV7_BANDS || file_encoding == AZ_WAVE_PWV6_PREVIEW) ? ".2EX" : ".EXT", 5);
        snprintf(data->analysis_paths[analysis_slot], sizeof data->analysis_paths[analysis_slot], "%s", relative);
        unsigned file_slot = analysis_slot + 1u;
        if (!st_source_path(src.mount, relative, data->files[file_slot])) return AZ_UNSUPPORTED;
        data->signatures[file_slot] = st_disk_sig(data->files[file_slot]);
        if (!data->signatures[file_slot]) return AZ_UNSUPPORTED;
        rc = st_wave_read(data->files[file_slot], data);
        return rc ? rc : st_disk_fresh(ref, data, NULL) ? AZ_OK
                                                        : AZ_STALE;
    }
    uint8_t *buf[2] = {NULL, NULL};
    size_t sizes[2] = {0, 0};
    if ((mask & ~AZ_DATA_METADATA) && analysis[0]) {
        char relative[1024];
        for (int i = 0; i < 2; i++) {
            snprintf(relative, sizeof relative, "%s", analysis);
            char *dot = strrchr(relative, '.');
            if (i && dot && !strcmp(dot, ".DAT")) memcpy(dot, ".EXT", 5);
            else if (i) continue;
            snprintf(data->analysis_paths[i], sizeof data->analysis_paths[i], "%s", relative);
            if (st_source_path(src.mount, relative, data->files[i + 1])) {
                data->signatures[i + 1] = st_disk_sig(data->files[i + 1]);
                if (!data->signatures[i + 1]) continue;
                buf[i] = st_read_file(data->files[i + 1], &sizes[i]);
                if (!buf[i]) {
                    rc = AZ_FAILED;
                    break;
                }
            }
        }
        if (!rc && (buf[0] || buf[1])) rc = st_parse_pair(data, (const uint8_t *const *) buf, sizes, mask);
    }
    free(buf[0]);
    free(buf[1]);
    if (rc) return rc;
    if (data->view.waveform.sample_count) {
        if (data->view.metadata.valid & AZ_META_DURATION)
            data->view.waveform.seconds_per_sample = data->view.metadata.duration_s / data->view.waveform.sample_count;
        else {
            data->view.waveform_info.availability = AZ_AVAIL_UNSUPPORTED;
            data->view.waveform.sample_count = 0;
        }
    }
    if (!(mask & AZ_DATA_METADATA)) {
        memset(&data->view.metadata, 0, sizeof data->view.metadata);
        data->view.metadata.size = sizeof data->view.metadata;
        data->view.metadata.version = AZ_DATA_VERSION;
    }
    return st_disk_fresh(ref, data, NULL) ? AZ_OK : AZ_STALE;
}
static void *st_worker(void *arg) {
    (void) arg;
    pthread_setname_np(pthread_self(), "az-track-data");
    for (;;) {
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        /* Weak native control blocks are reclaimed off the library/audio/UI hooks. */
        (void) azsdk_sources_sweep();
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL); /* core worker only; no native or playback callbacks */
    }
    return NULL;
}
/* Called only by track_resolve on a mod worker, serialized by the source mutex. */
static int st_start_worker(void) {
    pthread_mutex_lock(&g_st_source_mu);
    if (!g_st_started) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        struct sched_param sp = {0};
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        g_st_started = az_thread_create(&thread, &attr, st_worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    int rc = g_st_started ? AZ_OK : AZ_FAILED;
    pthread_mutex_unlock(&g_st_source_mu);
    return rc;
}
static void sdk_data_retire_owner(int owner) {
#ifdef CM_MAX
    _Static_assert(CM_MAX <= ST_OWNER_CAP, "data owner retirement covers every core mod index");
#endif
    st_retire_owner(&g_st, owner);
    sdk_command_retire_owner(owner);
}
static int st_mod_owner(const char *id, uintptr_t caller, const char *service) {
    cm_mod *c = cm_self(id, caller, service);
    return c && !g_cm_in_frame ? (int) (c - g_cm) : -1;
}
static int sdk_data_capabilities(az_data_capabilities *out) {
    if (!out || st_record_size(out) < sizeof *out) return AZ_INVALID;
    az_data_capabilities v = {out->size, AZ_DATA_VERSION, g_st_enabled && g_sdk_sq.ok > 0 ? 31u : 0u, g_st_enabled ? sd_live_fields() : 0u, 0, ST_PER_OWNER, ST_SLOTS, ST_WAVE, 0};
    if (g_st_enabled) v.commands = sdk_command_capabilities();
    memcpy(out, &v, sizeof v);
    return AZ_OK;
}
static int sdk_track_resolve(const char *id, int stick, uint32_t cid, az_track_ref *out) {
    if (st_mod_owner(id, CM_RA(), "track_resolve") < 0 || g_cm_on_ui || !out || st_record_size(out) < sizeof *out || !cid) return AZ_INVALID;
    if (!g_st_enabled || g_sdk_sq.ok <= 0) return AZ_UNSUPPORTED;
    struct st_source source;
    int rc = st_source_read(stick, &source);
    if (rc) return rc;
    if (st_start_worker()) return AZ_FAILED;
    uint32_t size = out->size;
    *out = source.ref;
    out->size = size;
    out->track_id = cid;
    return AZ_OK;
}
static int sdk_track_request(const char *id, const az_track_ref *ref, uint32_t mask, uint64_t *ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_request");
    if (owner < 0) return AZ_INVALID;
    if (!g_st_enabled || g_sdk_sq.ok <= 0) return AZ_UNSUPPORTED;
    if (!atomic_load_explicit(&g_st_started, memory_order_acquire)) return AZ_STALE;
    return st_request(&g_st, owner, ref, mask, ticket);
}
static int sdk_track_acquire(const char *id, uint64_t ticket, const az_track_snapshot **out) {
    int owner = st_mod_owner(id, CM_RA(), "track_acquire");
    return owner < 0 ? AZ_INVALID : st_acquire(&g_st, owner, ticket, out);
}
static int sdk_waveform_request(const char *id, const az_track_ref *ref, const az_wave_query *query, uint64_t *ticket) {
    int owner = st_mod_owner(id, CM_RA(), "waveform_request");
    if (owner < 0 || !query) return AZ_INVALID;
    if (!g_st_enabled || g_sdk_sq.ok <= 0) return AZ_UNSUPPORTED;
    if (!atomic_load_explicit(&g_st_started, memory_order_acquire)) return AZ_STALE;
    return st_request_with_wave(&g_st, owner, ref, ST_DETAIL, query, ticket);
}
static int sdk_waveform_acquire(const char *id, uint64_t ticket, const az_wave_snapshot **out) {
    int owner = st_mod_owner(id, CM_RA(), "waveform_acquire");
    return owner < 0 ? AZ_INVALID : st_wave_acquire(&g_st, owner, ticket, out);
}
static int sdk_track_release(const char *id, uint64_t ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_release");
    return owner < 0 ? AZ_INVALID : st_release(&g_st, owner, ticket, 0);
}
static int sdk_track_cancel(const char *id, uint64_t ticket) {
    int owner = st_mod_owner(id, CM_RA(), "track_cancel");
    return owner < 0 ? AZ_INVALID : st_release(&g_st, owner, ticket, 1);
}
static int sdk_data_event(const char *id, uint64_t after, az_data_change *out) {
    int owner = st_mod_owner(id, CM_RA(), "data_event");
    return owner < 0 ? AZ_INVALID : st_event_read(&g_st, owner, after, out);
}
/* Only the opt-in native transient commands are admitted. Refusal performs no action. */
#include "sdk_command.c"
static int sdk_command_submit(const char *id, const az_command_request *r, az_command_result *out) {
    if (st_mod_owner(id, CM_RA(), "command_submit") < 0 || !scmd_output_ok(out) || !scmd_request_valid(r)) return AZ_INVALID;
    if (r->operation >= AZ_CMD_LOOP_IN && r->operation <= AZ_CMD_CUE_COLOR && (r->flags & AZ_COMMAND_NATIVE_LOAD))
        return sdk_exit_submit(st_mod_owner(id, CM_RA(), "command_submit"), r, out);
    return scmd_refuse(out, r, AZ_UNSUPPORTED, scmd_now(&g_sdk_commands));
}

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
#include "../common/azsha256.h"
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
    int64_t duration_ticks; /* original OneLibrary integral seconds */
    char database[1024], content[2048], analysis[1024];
    int valid, analysis_limited, analysis_generated;
};
static struct st_resolution g_st_resolutions[2];
static int st_source_path(const char *mount, const char *relative, char out[1024]);
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out);
static uint32_t sd_live_fields(void);
static uint64_t sdk_command_capabilities(void);
static void sdk_command_retire_owner(int owner);
/* A stat fingerprint detects file replacement and edits, including nanosecond timestamps.
 * It is a freshness hint, not a content hash or a permanent library identifier. */
static uint64_t st_stat_sig(const struct stat *st) {
    if (!S_ISREG(st->st_mode)) return 0;
    uint64_t v = (uint64_t) st->st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) st->st_ino;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_size;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_mtim.tv_sec;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_mtim.tv_nsec;
    return v ? v : 1;
}
static uint64_t st_disk_sig(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : st_stat_sig(&st);
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
static int st_source_probe(int stick, char mount[1024], uint64_t *mount_id, struct stat *sb) {
    char database[1100];
    if (!sdk_stick_mount(stick, mount, 1024)) return 0;
    *mount_id = st_mount_id(mount);
    return st_source_path(mount, "/PIONEER/rekordbox/exportLibrary.db", database) &&
           *mount_id && !stat(database, sb) && S_ISREG(sb->st_mode);
}
static int st_source_update(struct st_source *s, int stick, const char *mount, uint64_t mid, struct stat sb) {
    uint64_t lib = (uint64_t) sb.st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) sb.st_ino;
    if (!lib) lib = 1;
    if (s->mount_id != mid || s->device != (uint64_t) sb.st_dev || s->inode != (uint64_t) sb.st_ino || strcmp(s->mount, mount)) {
        memset(s, 0, sizeof *s);
        if (g_st_generation == UINT64_MAX) return AZ_LIMIT;
        s->mount_id = mid;
        s->device = (uint64_t) sb.st_dev;
        s->inode = (uint64_t) sb.st_ino;
        s->ref = (az_track_ref) {sizeof(az_track_ref), AZ_DATA_VERSION, (uint64_t) stick + 1, ++g_st_generation, lib, 0, 0};
        snprintf(s->mount, sizeof s->mount, "%s", mount);
    }
    return AZ_OK;
}
/* Worker only. Resolve by physical USB port and mount lifecycle; never by title/content ID. */
static int st_source_read(int stick, struct st_source *out) {
    if (stick < 0 || stick > 1) return AZ_INVALID;
    char mount[1024];
    struct stat sb;
    uint64_t mid = 0;
    int ok = st_source_probe(stick, mount, &mid, &sb);
    pthread_mutex_lock(&g_st_source_mu);
    struct st_source *source = &g_st_sources[stick];
    int rc = AZ_STALE;
    if (!ok) memset(source, 0, sizeof *source);
    else {
        rc = st_source_update(source, stick, mount, mid, sb);
        if (rc == AZ_OK) *out = *source;
    }
    pthread_mutex_unlock(&g_st_source_mu);
    return rc;
}
static int st_unresolved_tail_safe(const char *tail) {
    for (const char *p = tail; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t) (end - p) : strlen(p);
        if (length == 2 && !memcmp(p, "..", 2)) return 0;
        p += length;
    }
    return 1;
}
static int st_missing_descendant(const char *joined, char real[PATH_MAX]) {
    /* Keep missing descendants in freshness checks, using an existing canonical
     * prefix and rejecting unresolved traversal. */
    char prefix[2048], parent[PATH_MAX];
    memcpy(prefix, joined, strlen(joined) + 1);
    for (;;) {
        char *slash = strrchr(prefix, '/');
        if (!slash || slash == prefix) return 0;
        *slash = 0;
        if (realpath(prefix, parent)) break;
        if (errno != ENOENT) return 0;
    }
    const char *tail = joined + strlen(prefix);
    if (!st_unresolved_tail_safe(tail)) return 0;
    return snprintf(real, PATH_MAX, "%s%s", parent, tail) < PATH_MAX;
}
static int st_path_inside(const char *real, const char *root) {
    size_t n = strlen(root);
    return !strncmp(real, root, n) && real[n] == '/' && strlen(real) < 1024;
}
/* Resolve every path inside its source, including symlinks and absent descendants. */
static int st_source_path(const char *mount, const char *relative, char out[1024]) {
    if (!relative || relative[0] != '/') return 0;
    char joined[2048], real[PATH_MAX], root[PATH_MAX];
    if (snprintf(joined, sizeof joined, "%s%s", mount, relative) >= (int) sizeof joined || !realpath(mount, root)) return 0;
    if (!realpath(joined, real)) {
        if (errno != ENOENT || !st_missing_descendant(joined, real)) return 0;
    }
    if (!st_path_inside(real, root)) return 0;
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
static int st_read_matches(const struct stat *sb, uint64_t device, uint64_t inode, uint64_t signature) {
    return !signature || ((uint64_t) sb->st_dev == device && (uint64_t) sb->st_ino == inode &&
                          st_stat_sig(sb) == signature);
}
static int st_read_regular(int fd, struct stat *sb, uint64_t device, uint64_t inode, uint64_t signature) {
    return !fstat(fd, sb) && S_ISREG(sb->st_mode) && sb->st_size >= 12 &&
           (uint64_t) sb->st_size <= ST_FILE_MAX && st_read_matches(sb, device, inode, signature);
}
static int st_read_complete(int fd, uint8_t *buffer, size_t size) {
    size_t read_bytes = 0;
    while (read_bytes < size) {
        ssize_t count = read(fd, buffer + read_bytes, size - read_bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        read_bytes += (size_t) count;
    }
    return 1;
}
static int st_read_recheck(int fd, uint64_t device, uint64_t inode, uint64_t signature) {
    if (!signature) return 1;
    struct stat sb;
    return !fstat(fd, &sb) && st_read_matches(&sb, device, inode, signature);
}
static uint8_t *st_read_file_checked(const char *path, size_t *size,
                                     uint64_t device, uint64_t inode, uint64_t signature) {
    /* Establish regular-file ownership before any read can wait for a FIFO writer. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat sb;
    uint8_t *buffer = NULL;
    if (st_read_regular(fd, &sb, device, inode, signature)) {
        *size = (size_t) sb.st_size;
        buffer = malloc(*size);
        if (buffer && (!st_read_complete(fd, buffer, *size) || !st_read_recheck(fd, device, inode, signature))) {
            free(buffer);
            buffer = NULL;
        }
    }
    close(fd);
    return buffer;
}
static uint8_t *st_read_file(const char *path, size_t *size) {
    return st_read_file_checked(path, size, 0, 0, 0);
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
static int st_ref_source_current(const az_track_ref *ref, struct st_source *source) {
    if (ref->source_id < 1 || ref->source_id > 2 || st_source_read((int) ref->source_id - 1, source)) return 0;
    source->ref.track_id = ref->track_id;
    return st_ref_equal(ref, &source->ref);
}
static int st_database_fresh(const struct st_source *source, const struct st_data *data) {
    char database[1024];
    return st_source_path(source->mount, "/PIONEER/rekordbox/exportLibrary.db", database) &&
           !strcmp(database, data->files[0]) && st_database_quiet(data->files[0]) &&
           data->signatures[0] && st_disk_sig(data->files[0]) == data->signatures[0];
}
static int st_analysis_files_fresh(const struct st_source *source, const struct st_data *data) {
    for (int i = 1; i < 3; i++) {
        if (data->analysis_paths[i - 1][0]) {
            char current[1024] = "";
            st_source_path(source->mount, data->analysis_paths[i - 1], current);
            if (strcmp(current, data->files[i])) return 0;
        }
        if (data->files[i][0] && st_disk_sig(data->files[i]) != data->signatures[i]) return 0;
    }
    return 1;
}
static int st_disk_fresh(const az_track_ref *ref, const struct st_data *data, void *ctx) {
    (void) ctx;
    struct st_source source;
    if (!st_ref_source_current(ref, &source)) {
        st_resolution_retire(ref, NULL);
        return 0;
    }
    if (!st_database_fresh(&source, data)) {
        st_resolution_retire(ref, data->files[0]);
        return 0;
    }
    return st_analysis_files_fresh(&source, data);
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
static void st_row_text(void *stmt, az_track_metadata *metadata) {
    char *texts[] = {metadata->title, metadata->artist, metadata->album, metadata->key};
    size_t capacities[] = {sizeof metadata->title, sizeof metadata->artist, sizeof metadata->album, sizeof metadata->key};
    for (int i = 0; i < 4; i++) {
        const unsigned char *value = g_sdk_sq.col_text(stmt, i);
        if (!value) continue;
        snprintf(texts[i], capacities[i], "%s", value);
        utf8_cut(texts[i]);
        metadata->valid |= 1u << i;
    }
}
static void st_row_timing(void *stmt, struct st_resolution *row) {
    az_track_metadata *metadata = &row->metadata;
    metadata->original_bpm = g_sdk_sq.col_int64(stmt, 5) / 100.0;
    row->duration_ticks = g_sdk_sq.col_int64(stmt, 6);
    metadata->duration_s = (double) row->duration_ticks;
    if (metadata->original_bpm > 0) metadata->valid |= AZ_META_TEMPO;
    int order = compare_timestamps(row->duration_ticks, 0, 1, 1, 1);
    if (order != AZ_TIME_INVALID && order == AZ_TIME_AFTER) metadata->valid |= AZ_META_DURATION;
}
static void st_row_analysis(struct st_resolution *row, const unsigned char *analysis, uint32_t track_id) {
    if (analysis && snprintf(row->analysis, sizeof row->analysis, "%s", analysis) >= (int) sizeof row->analysis) {
        row->analysis_limited = 1;
        row->analysis[0] = 0;
    }
    /* Reserve a source-local path while an unanalysed export awaits native spool. */
    if (!row->analysis[0] && !row->analysis_limited && row->content[0]) {
        char digest[65];
        azsha256_hex(row->content, strlen(row->content), digest);
        snprintf(row->analysis, sizeof row->analysis,
                 "/PIONEER/USBANLZ/AZMODS/%08x-%.32s/ANLZ0000.DAT", track_id, digest);
        row->analysis_generated = 1;
    }
}
static int st_decode_row(void *stmt, uint32_t track_id, struct st_resolution *row, int *ambiguous) {
    st_row_text(stmt, &row->metadata);
    const unsigned char *path = g_sdk_sq.col_text(stmt, 4), *analysis = g_sdk_sq.col_text(stmt, 7);
    /* Truncated paths must never become audio locators. */
    if (path && snprintf(row->content, sizeof row->content, "%s", path) >= (int) sizeof row->content) row->content[0] = 0;
    st_row_timing(stmt, row);
    st_row_analysis(row, analysis, track_id);
    int next = g_sdk_sq.step(stmt);
    if (ambiguous) *ambiguous = next == 100;
    return next == 101 ? AZ_OK : AZ_FAILED;
}
/* Both immutable tickets and transaction-aware status reads decode the same unique row. */
static int st_read_row_checked(void *db, uint32_t track_id, struct st_resolution *row, int *ambiguous) {
    static const char query[] = "SELECT c.title,a.name,al.name,k.name,c.path,c.bpmx100,c.length,c.analysisDataFilePath "
                                "FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist "
                                "LEFT JOIN album al ON al.album_id=c.album_id LEFT JOIN key k ON k.key_id=c.key_id "
                                "WHERE c.content_id=?1";
    void *stmt = NULL;
    int rc = AZ_FAILED;
    if (!g_sdk_sq.prepare(db, query, -1, &stmt, NULL) && stmt && !g_sdk_sq.bind_int64(stmt, 1, track_id)) {
        int step = g_sdk_sq.step(stmt);
        if (step == 100) rc = st_decode_row(stmt, track_id, row, ambiguous);
        else if (step == 101) rc = AZ_NOT_FOUND;
    }
    if (stmt) g_sdk_sq.finalize(stmt);
    return rc;
}
static int st_read_row(void *db, uint32_t track_id, struct st_resolution *row) {
    return st_read_row_checked(db, track_id, row, NULL);
}
static int st_disk_source(const az_track_ref *ref, struct st_data *data, struct st_source *source,
                          struct st_resolution *cached) {
    struct st_source src;
    if (!st_ref_source_current(ref, &src)) {
        cached->valid = 0;
        return AZ_STALE;
    }
    /* SQLite and sidecars must use the same canonical physical database path. */
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
    *source = src;
    return AZ_OK;
}
static int st_resolution_reusable(const struct st_resolution *cached, const struct st_source *source, const struct st_data *data) {
    return cached->valid && st_resolution_source_equal(&cached->source, source) &&
           cached->signature == data->signatures[0] && !strcmp(cached->database, data->files[0]);
}
static int st_query_resolution(const struct st_source *source, const struct st_data *data, uint32_t track_id,
                               struct st_resolution *row) {
    memset(row, 0, sizeof *row);
    row->source = *source;
    row->signature = data->signatures[0];
    snprintf(row->database, sizeof row->database, "%s", data->files[0]);
    row->metadata.size = sizeof row->metadata;
    row->metadata.version = AZ_DATA_VERSION;
    void *db = sdk_db_open(data->files[0]);
    if (!db) return AZ_FAILED;
    int rc = st_read_row(db, track_id, row);
    g_sdk_sq.close(db);
    return rc;
}
static int st_resolve_row(const az_track_ref *ref, uint32_t mask, const struct st_source *source,
                          const struct st_data *data, struct st_resolution *cached, struct st_resolution *row) {
    int reuse = st_resolution_reusable(cached, source, data);
    int rc = AZ_OK;
    if (reuse) *row = *cached;
    else {
        cached->valid = 0;
        rc = st_query_resolution(source, data, ref->track_id, row);
        if (rc) return rc;
    }
    if (row->analysis_limited && (mask & ~AZ_DATA_METADATA)) rc = AZ_LIMIT;
    if (rc) return rc;
    if (reuse) return AZ_OK;
    if (!st_resolution_current(source, data->files[0], data->signatures[0])) {
        cached->valid = 0;
        return AZ_STALE;
    }
    if (!row->analysis_limited) {
        row->valid = 1;
        *cached = *row;
    }
    return AZ_OK;
}
static void st_disk_metadata(const struct st_source *source, const struct st_resolution *row, uint32_t mask, struct st_data *data) {
    data->view.metadata = row->metadata;
    char real[1024];
    if (row->content[0] && st_source_path(source->mount, row->content, real) && strlen(real) < sizeof data->view.metadata.path) {
        snprintf(data->view.metadata.path, sizeof data->view.metadata.path, "%s", real);
        data->view.metadata.valid |= AZ_META_PATH;
    }
    if (mask & AZ_DATA_METADATA) data->view.metadata_info.availability = AZ_AVAIL_READY;
}
static int st_wave_analysis_path(const char *analysis, uint32_t file_encoding, char relative[1024]) {
    if (!analysis[0]) return -1;
    snprintf(relative, 1024, "%s", analysis);
    char *dot = strrchr(relative, '.');
    if (!dot || strcmp(dot, ".DAT")) return -1;
    unsigned slot = sww_is_mono_preview(file_encoding) ? 0u : 1u;
    if (slot) memcpy(dot, (file_encoding == AZ_WAVE_PWV7_BANDS || file_encoding == AZ_WAVE_PWV6_PREVIEW) ? ".2EX" : ".EXT", 5);
    return (int) slot;
}
static int st_disk_detail(const az_track_ref *ref, const struct st_source *source, const char *analysis, struct st_data *data) {
    char relative[1024];
    uint32_t encoding = st_wave_file_encoding(data->wave_query.encoding);
    int slot = st_wave_analysis_path(analysis, encoding, relative);
    if (slot < 0) return AZ_UNSUPPORTED;
    snprintf(data->analysis_paths[slot], sizeof data->analysis_paths[slot], "%s", relative);
    unsigned file_slot = (unsigned) slot + 1;
    if (!st_source_path(source->mount, relative, data->files[file_slot])) return AZ_UNSUPPORTED;
    data->signatures[file_slot] = st_disk_sig(data->files[file_slot]);
    if (!data->signatures[file_slot]) return AZ_UNSUPPORTED;
    int rc = st_wave_read(data->files[file_slot], data);
    return rc ? rc : st_disk_fresh(ref, data, NULL) ? AZ_OK
                                                    : AZ_STALE;
}
static int st_analysis_relative(const char *analysis, unsigned file, char relative[1024]) {
    snprintf(relative, 1024, "%s", analysis);
    if (!file) return 1;
    char *dot = strrchr(relative, '.');
    if (!dot || strcmp(dot, ".DAT")) return 0;
    memcpy(dot, ".EXT", 5);
    return 1;
}
static int st_read_analysis(const struct st_source *source, const char *analysis, unsigned file,
                            struct st_data *data, uint8_t **buffer, size_t *size) {
    char relative[1024];
    if (!st_analysis_relative(analysis, file, relative)) return AZ_OK;
    snprintf(data->analysis_paths[file], sizeof data->analysis_paths[file], "%s", relative);
    if (!st_source_path(source->mount, relative, data->files[file + 1])) return AZ_OK;
    data->signatures[file + 1] = st_disk_sig(data->files[file + 1]);
    if (!data->signatures[file + 1]) return AZ_OK;
    *buffer = st_read_file(data->files[file + 1], size);
    return *buffer ? AZ_OK : AZ_FAILED;
}
static int st_disk_analysis(const struct st_source *source, const char *analysis, uint32_t mask, struct st_data *data) {
    if (!(mask & ~AZ_DATA_METADATA) || !analysis[0]) return AZ_OK;
    uint8_t *buffer[2] = {NULL, NULL};
    size_t size[2] = {0, 0};
    int rc = AZ_OK;
    for (unsigned file = 0; file < 2; file++) {
        rc = st_read_analysis(source, analysis, file, data, &buffer[file], &size[file]);
        if (rc) break;
    }
    if (!rc && (buffer[0] || buffer[1])) rc = st_parse_pair(data, (const uint8_t *const *) buffer, size, mask);
    free(buffer[0]);
    free(buffer[1]);
    return rc;
}
static void st_disk_preview_clock(struct st_data *data) {
    if (!data->view.waveform.sample_count) return;
    if (data->view.metadata.valid & AZ_META_DURATION)
        data->view.waveform.seconds_per_sample = data->view.metadata.duration_s / data->view.waveform.sample_count;
    else {
        data->view.waveform_info.availability = AZ_AVAIL_UNSUPPORTED;
        data->view.waveform.sample_count = 0;
    }
}
static void st_disk_select_metadata(struct st_data *data, uint32_t mask) {
    if (mask & AZ_DATA_METADATA) return;
    memset(&data->view.metadata, 0, sizeof data->view.metadata);
    data->view.metadata.size = sizeof data->view.metadata;
    data->view.metadata.version = AZ_DATA_VERSION;
}
static int st_disk_load(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    (void) ctx;
    if (ref->source_id < 1 || ref->source_id > 2) return AZ_STALE;
    struct st_resolution *cached = &g_st_resolutions[ref->source_id - 1];
    struct st_source source;
    int rc = st_disk_source(ref, data, &source, cached);
    if (rc) return rc;
    struct st_resolution row;
    rc = st_resolve_row(ref, mask, &source, data, cached, &row);
    if (rc) return rc;
    st_disk_metadata(&source, &row, mask, data);
    if (mask == ST_DETAIL) return st_disk_detail(ref, &source, row.analysis, data);
    rc = st_disk_analysis(&source, row.analysis, mask, data);
    if (rc) return rc;
    st_disk_preview_clock(data);
    st_disk_select_metadata(data, mask);
    return st_disk_fresh(ref, data, NULL) ? AZ_OK : AZ_STALE;
}
/* Only the core worker runs the optional status jobs. Starting or changing
 * interest wakes it independently of the explicit-ticket freshness deadline. */
static _Atomic(void (*)(void)) g_st_status_jobs;
static pthread_mutex_t g_st_wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_st_wake_cv = PTHREAD_COND_INITIALIZER;
static int g_st_wake_pending;
static void st_worker_wake(void) {
    pthread_mutex_lock(&g_st_wake_mu);
    g_st_wake_pending = 1;
    pthread_cond_signal(&g_st_wake_cv);
    pthread_mutex_unlock(&g_st_wake_mu);
}
static void *st_worker(void *arg) {
    (void) arg;
    pthread_setname_np(pthread_self(), "az-track-data");
    for (;;) {
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        /* Weak native control blocks are reclaimed off the library/audio/UI hooks. */
        (void) azsdk_sources_sweep();
        void (*status_jobs)(void) = atomic_load(&g_st_status_jobs);
        if (status_jobs) status_jobs();
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint64_t nanos = (uint64_t) deadline.tv_nsec + UINT64_C(100000000);
        deadline.tv_sec += (time_t) (nanos / UINT64_C(1000000000));
        deadline.tv_nsec = (long) (nanos % UINT64_C(1000000000));
        pthread_mutex_lock(&g_st_wake_mu);
        while (!g_st_wake_pending && pthread_cond_timedwait(&g_st_wake_cv, &g_st_wake_mu, &deadline) == 0) {}
        g_st_wake_pending = 0;
        pthread_mutex_unlock(&g_st_wake_mu);
    }
    return NULL;
}
/* Start once, independently of whether explicit tickets or loaded-track watches came first. */
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
static void sdk_data_owner_init(int owner) {
    st_reopen_owner(&g_st, owner);
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
static int st_resolve_reference(int stick, uint32_t cid, az_track_ref *out) {
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
static int sdk_track_resolve(const char *id, int stick, uint32_t cid, az_track_ref *out) {
    if (st_mod_owner(id, CM_RA(), "track_resolve") < 0 || g_cm_on_ui || !out || st_record_size(out) < sizeof *out || !cid) return AZ_INVALID;
    if (!g_st_enabled || g_sdk_sq.ok <= 0) return AZ_UNSUPPORTED;
    return st_resolve_reference(stick, cid, out);
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
    int owner = st_mod_owner(id, CM_RA(), "command_submit");
    if (owner < 0 || !scmd_output_ok(out) || !scmd_request_valid(r)) return AZ_INVALID;
    return sdk_exit_submit(owner, r, out);
}

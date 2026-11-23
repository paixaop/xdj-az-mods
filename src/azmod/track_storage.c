/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "track_storage.h"
#include "library_sql.h"
#include "sdk_analysis.h"
#include "utf8_cut.h"
#include "../common/azsha256.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif
static pthread_mutex_t g_st_source_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_st_generation;
static struct st_source g_st_sources[2];
static int st_ref_equal(const az_track_ref *a, const az_track_ref *b) {
    return a->source_id == b->source_id && a->source_generation == b->source_generation &&
           a->library_id == b->library_id && a->track_id == b->track_id;
}
/* A stat fingerprint detects file replacement and edits, including nanosecond timestamps.
 * It is a freshness hint, not a content hash or a permanent library identifier. */
uint64_t st_stat_sig(const struct stat *st) {
    if (!S_ISREG(st->st_mode)) return 0;
    uint64_t v = (uint64_t) st->st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) st->st_ino;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_size;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_mtim.tv_sec;
    v = v * 1099511628211ULL ^ (uint64_t) st->st_mtim.tv_nsec;
    return v ? v : 1;
}
uint64_t st_disk_sig(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : st_stat_sig(&st);
}
static uint64_t st_mount_id(const char *mount) {
    FILE *f = fopen(az_storage_mountinfo(), "re");
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
    if (!az_storage_stick_mount(stick, mount, 1024)) return 0;
    *mount_id = st_mount_id(mount);
    return st_source_path(mount, "/PIONEER/rekordbox/exportLibrary.db", database) &&
           *mount_id && !stat(database, sb) && S_ISREG(sb->st_mode);
}
int st_source_assign(struct st_source *s, uint64_t *generation, int stick, const char *mount, uint64_t mid, struct stat sb) {
    uint64_t lib = (uint64_t) sb.st_dev * 0x9e3779b185ebca87ULL ^ (uint64_t) sb.st_ino;
    if (!lib) lib = 1;
    if (s->mount_id != mid || s->device != (uint64_t) sb.st_dev || s->inode != (uint64_t) sb.st_ino || strcmp(s->mount, mount)) {
        memset(s, 0, sizeof *s);
        if (*generation == UINT64_MAX) return AZ_LIMIT;
        s->mount_id = mid;
        s->device = (uint64_t) sb.st_dev;
        s->inode = (uint64_t) sb.st_ino;
        s->ref = (az_track_ref) {sizeof(az_track_ref), AZ_DATA_VERSION, (uint64_t) stick + 1, ++*generation, lib, 0, 0};
        snprintf(s->mount, sizeof s->mount, "%s", mount);
    }
    return AZ_OK;
}
/* Worker only. Resolve by physical USB port and mount lifecycle; never by title/content ID. */
int st_source_read(int stick, struct st_source *out) {
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
        rc = st_source_assign(source, &g_st_generation, stick, mount, mid, sb);
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
int st_source_path(const char *mount, const char *relative, char out[1024]) {
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
int st_database_quiet(const char *db) {
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
uint8_t *st_read_file_checked(const char *path, size_t *size,
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
uint8_t *st_read_file(const char *path, size_t *size) {
    return st_read_file_checked(path, size, 0, 0, 0);
}
static int st_hash_bytes(int fd, off_t bytes, char digest[65]) {
    unsigned char buffer[16384];
    azsha256_ctx hash;
    azsha256_init(&hash);
    while (bytes > 0) {
        size_t chunk = bytes < (off_t) sizeof buffer ? (size_t) bytes : sizeof buffer;
        if (!st_read_complete(fd, buffer, chunk)) return 0;
        azsha256_update(&hash, buffer, chunk);
        bytes -= (off_t) chunk;
    }
    azsha256_final_hex(&hash, digest);
    return 1;
}
static int st_hash_input(int fd, struct stat *status, uint64_t signature) {
    /* The Beatport reader accepts AAC containers up to 128 MiB. */
    return !fstat(fd, status) && status->st_size > 0 && status->st_size <= 128 * 1024 * 1024 &&
           st_stat_sig(status) == signature;
}
int st_hash_file(const char *path, uint64_t signature, char digest[65]) {
    if (!signature) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat status;
    int ok = st_hash_input(fd, &status, signature);
    if (ok) ok = st_hash_bytes(fd, status.st_size, digest) &&
                 st_read_recheck(fd, status.st_dev, status.st_ino, signature);
    close(fd);
    return ok;
}
int st_resolution_source_equal(const struct st_source *a, const struct st_source *b) {
    return st_ref_equal(&a->ref, &b->ref) && a->mount_id == b->mount_id &&
           a->device == b->device && a->inode == b->inode && !strcmp(a->mount, b->mount);
}
static void st_row_text(void *stmt, az_track_metadata *metadata) {
    char *texts[] = {metadata->title, metadata->artist, metadata->album, metadata->key};
    size_t capacities[] = {sizeof metadata->title, sizeof metadata->artist, sizeof metadata->album, sizeof metadata->key};
    for (int i = 0; i < 4; i++) {
        const unsigned char *value = az_storage_sql()->col_text(stmt, i);
        if (!value) continue;
        snprintf(texts[i], capacities[i], "%s", value);
        utf8_cut(texts[i]);
        metadata->valid |= 1u << i;
    }
}
static void st_row_timing(void *stmt, struct st_resolution *row) {
    az_track_metadata *metadata = &row->metadata;
    metadata->original_bpm = (double) az_storage_sql()->col_int64(stmt, 5) / 100.0;
    row->duration_ticks = az_storage_sql()->col_int64(stmt, 6);
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
    const unsigned char *path = az_storage_sql()->col_text(stmt, 4), *analysis = az_storage_sql()->col_text(stmt, 7);
    /* Truncated paths must never become audio locators. */
    if (path && snprintf(row->content, sizeof row->content, "%s", path) >= (int) sizeof row->content) row->content[0] = 0;
    st_row_timing(stmt, row);
    st_row_analysis(row, analysis, track_id);
    int next = az_storage_sql()->step(stmt);
    if (ambiguous) *ambiguous = next == 100;
    return next == 101 ? AZ_OK : AZ_FAILED;
}
/* Both immutable tickets and transaction-aware status reads decode the same unique row. */
int st_read_row_checked(void *db, uint32_t track_id, struct st_resolution *row, int *ambiguous) {
    static const char query[] = "SELECT c.title,a.name,al.name,k.name,c.path,c.bpmx100,c.length,c.analysisDataFilePath "
                                "FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist "
                                "LEFT JOIN album al ON al.album_id=c.album_id LEFT JOIN key k ON k.key_id=c.key_id "
                                "WHERE c.content_id=?1";
    void *stmt = NULL;
    int rc = AZ_FAILED;
    if (!az_storage_sql()->prepare(db, query, -1, &stmt, NULL) && stmt && !az_storage_sql()->bind_int64(stmt, 1, track_id)) {
        int step = az_storage_sql()->step(stmt);
        if (step == 100) rc = st_decode_row(stmt, track_id, row, ambiguous);
        else if (step == 101) rc = AZ_NOT_FOUND;
    }
    if (stmt) az_storage_sql()->finalize(stmt);
    return rc;
}
int st_read_row(void *db, uint32_t track_id, struct st_resolution *row) {
    return st_read_row_checked(db, track_id, row, NULL);
}

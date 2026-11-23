/* SPDX-License-Identifier: GPL-3.0-only */
/* Core-owned acquisition, publication, retirement and durable-link progression. */
#define _GNU_SOURCE
#include "analysis_native_adapter.h"
#include "analysis_encoding.h"
#include "analysis_vocals.h"
#include "track_storage.h"
#include "library_sql.h"
#include "sdk_analysis.h"
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
static int st_ref_equal(const az_track_ref *a, const az_track_ref *b) {
    return a->source_id == b->source_id && a->source_generation == b->source_generation &&
           a->library_id == b->library_id && a->track_id == b->track_id;
}
#include "sdk_analysis_write.h"
#include "analysis_source.h"
#include "sdk_vocals.h"
#include "../common/azstatus_maps.h"
#include "../common/azstatus_native.h"
#include "../common/azanalysis_native.h"
struct sa_open_result {
    uint32_t needed;
    uint64_t job;
    az_track_ref track;
    double original_bpm;
    char audio_path[1024];
};
struct sa_job {
    uint64_t id;
    int owner, needs_link, link_queued;
    az_analysis_owner lifetime;
    char reader_path[1024];
    struct st_source source;
    char database[1024], audio[1024], dat[1024], ext[1024], ex2[1024];
    char stream_identity[1024], stream_digest[65];
    char relative_audio[2048], relative_dat[1024];
    uint64_t signatures[5];
    uint32_t needed;
    int vocals, reader_fd;
    int loaded, deck, streaming;
    int display_read; /* synchronous, read-only native-load lease; no producer owner */
    unsigned vocal_length_s;
    uint64_t load, native_epoch, audio_token;
    const struct azn_analysis_source_api *audio_api;
    const struct azn_maps_api *maps;
    struct aznm_packet native_map;
    uint8_t *native_grid;
    size_t native_grid_bytes;
    /* An admitted publication may have durable files while native refresh is busy.
     * Keep its exact original encoding so only that publication can resume. */
    uint8_t *published;
    struct sa_tags published_tags;
};
static struct sa_job sa_jobs[8];
static uint64_t sa_serial;
static pthread_mutex_t sa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int sa_source_owner_index(az_analysis_owner owner);
static int sa_link_analysis(struct sa_job *job);
static int sa_link_enqueue(struct sa_job *job, const char *path);
static void sa_link_release(uint64_t id);
static void sa_link_wake(void);
static int sa_cleaner_start(void);
static int sa_owner_live(const struct sa_job *job) {
    return job->display_read || analysis_owner_current(job->lifetime);
}
static int sa_loaded_current(const struct sa_job *job);
static void sa_job_drop(struct sa_job *job);
static int sa_grid_same(const struct sa_job *, const uint8_t *, uint32_t);
static const uint8_t *sa_find_tag(const uint8_t *, size_t, const char[4], uint32_t *);
static int sa_disk_files_match(const struct sa_job *j) {
    return st_disk_sig(j->audio) == j->signatures[1] && st_disk_sig(j->dat) == j->signatures[2] &&
           st_disk_sig(j->ext) == j->signatures[3];
}
static int sa_resolved_path_matches(const char *mount, const char *relative, const char *expected) {
    char path[1024];
    return st_source_path(mount, relative, path) && !strcmp(path, expected);
}
static int sa_source_paths_match(const struct sa_job *j, const struct st_source *source) {
    char relative[1024];
    snprintf(relative, sizeof relative, "%s", j->relative_dat);
    size_t len = strlen(relative);
    if (len < 4) return 0;
    memcpy(relative + len - 4, ".EXT", 5);
    if (!sa_resolved_path_matches(source->mount, j->relative_audio, j->audio) ||
        !sa_resolved_path_matches(source->mount, j->relative_dat, j->dat) ||
        !sa_resolved_path_matches(source->mount, relative, j->ext)) return 0;
    if (!j->vocals) return 1;
    memcpy(relative + len - 4, ".2EX", 5);
    return sa_resolved_path_matches(source->mount, relative, j->ex2);
}
static int sa_source_signatures_match(const struct sa_job *j, const struct st_source *source) {
    return st_resolution_source_equal(source, &j->source) && st_database_quiet(j->database) &&
           st_disk_sig(j->database) == j->signatures[0] && sa_disk_files_match(j);
}
static int sa_source_current(const struct sa_job *j) {
    if (j->streaming)
        return st_disk_sig(j->stream_identity) == j->signatures[0] && sa_disk_files_match(j);
    struct st_source source;
    if (st_source_read((int) j->source.ref.source_id - 1, &source)) return 0;
    source.ref.track_id = j->source.ref.track_id;
    return sa_source_paths_match(j, &source) && sa_source_signatures_match(j, &source);
}
static int sa_current(const struct sa_job *j) {
    if (!sa_owner_live(j)) return 0;
    if (j->vocals && st_disk_sig(j->ex2) != j->signatures[4]) return 0;
    if (j->loaded && !sa_loaded_current(j)) return 0;
    return sa_source_current(j) && sa_owner_live(j);
}
static uint8_t *sa_read(const char *path, size_t *size, int *failed) {
    struct stat sb;
    *size = 0;
    if (lstat(path, &sb)) {
        if (errno != ENOENT) *failed = 1;
        return NULL;
    }
    if (S_ISREG(sb.st_mode) && sb.st_size == 0) return NULL; /* empty export placeholder */
    uint8_t *bytes = st_read_file(path, size);
    if (!bytes || !sa_framed(bytes, *size)) {
        free(bytes);
        *failed = 1;
        return NULL;
    }
    return bytes;
}
static int sa_open_request_valid(int owner, const az_track_ref *track, const struct sa_open_result *out) {
    return owner >= 0 && track && out && st_record_size(track) >= sizeof *track && track->version == AZ_DATA_VERSION &&
           track->track_id && track->source_id >= 1 && track->source_id <= 2;
}
static int sa_open_slot(int owner, unsigned *slot) {
    unsigned owned = 0;
    *slot = 0;
    for (unsigned i = 0; i < 8; i++) owned += sa_jobs[i].id && sa_jobs[i].owner == owner;
    while (*slot < 8 && sa_jobs[*slot].id) ++*slot;
    return *slot < 8 && owned < 4 && sa_serial < UINT64_MAX;
}
static int sa_open_row(const az_track_ref *track, struct sa_job *j, struct st_resolution *row) {
    if (st_source_read((int) track->source_id - 1, &j->source)) return AZ_STALE;
    j->source.ref.track_id = track->track_id;
    if (!st_ref_equal(track, &j->source.ref) ||
        !st_source_path(j->source.mount, "/PIONEER/rekordbox/exportLibrary.db", j->database) ||
        !st_database_quiet(j->database)) return AZ_STALE;
    j->signatures[0] = st_disk_sig(j->database);
    void *db = sdk_db_open(j->database);
    if (!db) return AZ_FAILED;
    int rc = st_read_row(db, track->track_id, row);
    az_storage_sql()->close(db);
    return rc;
}
static int sa_dat_path_valid(const char *path) {
    size_t n = strlen(path);
    return n >= 4 && !strcmp(path + n - 4, ".DAT");
}
static int sa_open_paths(struct sa_job *j, const struct st_resolution *row) {
    if (!row->content[0] || !sa_dat_path_valid(row->analysis) || row->analysis_limited ||
        !st_source_path(j->source.mount, row->content, j->audio) ||
        !st_source_path(j->source.mount, row->analysis, j->dat)) return 0;
    if (!sa_dat_path_valid(j->dat)) return 0;
    snprintf(j->relative_audio, sizeof j->relative_audio, "%s", row->content);
    snprintf(j->relative_dat, sizeof j->relative_dat, "%s", row->analysis);
    j->needs_link = row->analysis_generated;
    char relative[1024];
    snprintf(relative, sizeof relative, "%s", row->analysis);
    memcpy(relative + strlen(relative) - 4, ".EXT", 5);
    if (!st_source_path(j->source.mount, relative, j->ext)) return 0;
    memcpy(relative + strlen(relative) - 4, ".2EX", 5);
    if (!st_source_path(j->source.mount, relative, j->ex2)) return 0;
    j->signatures[4] = st_disk_sig(j->ex2);
    j->signatures[1] = st_disk_sig(j->audio);
    j->signatures[2] = st_disk_sig(j->dat);
    j->signatures[3] = st_disk_sig(j->ext);
    return 1;
}
static int sa_tag_counts_valid(int failed, int beats, int phrases, int dat_phrases, int vocal_count) {
    return !failed && beats >= 0 && phrases >= 0 && dat_phrases >= 0 && vocal_count >= 0 && !(phrases && dat_phrases);
}
static int sa_open_needed(struct sa_job *j) {
    size_t sizes[2] = {0, 0};
    int failed = 0;
    uint8_t *dat = sa_read(j->dat, &sizes[0], &failed), *ext = sa_read(j->ext, &sizes[1], &failed);
    int beats = sa_tag_count(dat, sizes[0], "PQTZ"), phrases = sa_tag_count(ext, sizes[1], "PSSI");
    int dat_phrases = sa_tag_count(dat, sizes[0], "PSSI");
    size_t vn = 0;
    uint8_t *vocal = j->vocals ? sa_read(j->ex2, &vn, &failed) : NULL;
    int vocal_count = j->vocals ? sa_tag_count(vocal, vn, "PVDI") : 1;
    free(vocal);
    free(dat);
    free(ext);
    if (!sa_tag_counts_valid(failed, beats, phrases, dat_phrases, vocal_count)) return AZ_FAILED;
    j->needed = (phrases || dat_phrases ? 0 : AZ_DATA_PHRASES) | (vocal_count ? 0 : AZ_ANALYSIS_VOCALS);
    if ((j->needed & AZ_DATA_PHRASES) && !beats) j->needed |= AZ_DATA_GRID;
    return AZ_OK;
}
static int sa_open_link_existing(struct sa_job *j, const az_track_ref *track, struct sa_open_result *out) {
    if (j->needs_link && (!sa_current(j) || !sa_link_analysis(j))) return AZ_FAILED;
    struct sa_open_result linked = {.job = j->id, .track = *track};
    memcpy(out, &linked, sizeof linked);
    return j->needs_link ? AZ_PENDING : ANALYSIS_SOURCE_COMPLETE;
}
static int sa_reader_open(struct sa_job *j) {
    j->reader_fd = open(j->audio, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (j->reader_fd < 0) return AZ_STALE;
    snprintf(j->reader_path, sizeof j->reader_path, "/proc/%ld/fd/%d", (long) getpid(), j->reader_fd);
    return AZ_OK;
}
static void sa_open_publish(struct sa_job *j, const az_track_ref *track, struct sa_open_result *out,
                            unsigned slot, double bpm) {
    j->id = ++sa_serial;
    sa_jobs[slot] = *j;
    struct sa_open_result result = {.needed = j->needed, .job = j->id, .track = *track, .original_bpm = bpm};
    memcpy(result.audio_path, j->reader_path, sizeof result.audio_path);
    memcpy(out, &result, sizeof result);
}
static int sa_open_prepare(const az_track_ref *track, struct sa_job *job, struct st_resolution *row) {
    int rc = sa_open_row(track, job, row);
    if (rc != AZ_OK) return rc;
    if (!sa_open_paths(job, row)) return AZ_UNSUPPORTED;
    if (!job->signatures[0] || !job->signatures[1]) return AZ_STALE;
    return sa_open_needed(job);
}
static int sa_open_reserved(az_analysis_owner lifetime, const az_track_ref *track, struct sa_open_result *out,
                            int loaded, int vocals, unsigned slot) {
    struct sa_job j = {.owner = sa_source_owner_index(lifetime), .lifetime = lifetime, .reader_fd = -1, .vocals = vocals};
    struct st_resolution row = {0};
    int rc = sa_open_prepare(track, &j, &row);
    if (rc != AZ_OK) return rc;
    if (!sa_current(&j)) return AZ_STALE;
    if (!loaded && !j.needed) return sa_open_link_existing(&j, track, out);
    rc = sa_reader_open(&j);
    if (rc) return rc;
    sa_open_publish(&j, track, out, slot, row.metadata.original_bpm);
    return AZ_OK;
}
static int sa_open(az_analysis_owner lifetime, const az_track_ref *track, struct sa_open_result *out, int loaded, int vocals) {
    int owner = sa_source_owner_index(lifetime);
    if (!sa_open_request_valid(owner, track, out)) return AZ_INVALID;
    if (!analysis_owner_current(lifetime)) return AZ_STALE;
    if (sa_cleaner_start()) return AZ_FAILED;
    if (!az_storage_sql_ready()) return AZ_UNSUPPORTED;
    if (!analysis_owner_current(lifetime)) return AZ_STALE;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    unsigned slot;
    int rc = AZ_LIMIT;
    if (sa_open_slot(owner, &slot)) rc = sa_open_reserved(lifetime, track, out, loaded, vocals, slot);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
/* Directory descriptors bind staging/publication to the validated USB tree.
 * Create missing ANLZ directories without following any link component. */
struct sa_staged {
    int dir;
    char temp[80], name[256];
};
static int sa_stage_path(const struct sa_job *job, const char *path, char root[PATH_MAX], char tail[1024]) {
    if (!realpath(job->source.mount, root)) return 0;
    size_t prefix = strlen(root);
    if (strncmp(root, path, prefix) || path[prefix] != '/' || strlen(path + prefix + 1) >= 1024) return 0;
    snprintf(tail, 1024, "%s", path + prefix + 1);
    return 1;
}
static int sa_stage_name(char *tail, struct sa_staged *stage, char **last) {
    *last = strrchr(tail, '/');
    const char *name = *last ? *last + 1 : tail;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= sizeof stage->name) return 0;
    snprintf(stage->name, sizeof stage->name, "%s", name);
    return 1;
}
static int sa_stage_directories(char *tail, struct sa_staged *stage) {
    char *save = NULL;
    for (char *part = strtok_r(tail, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) return 0;
        if (mkdirat(stage->dir, part, 0777) && errno != EEXIST) return 0;
        int next = openat(stage->dir, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) return 0;
        close(stage->dir);
        stage->dir = next;
    }
    return 1;
}
static int sa_stage_file(uint64_t job, struct sa_staged *stage) {
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        snprintf(stage->temp, sizeof stage->temp, ".az-analysis-%llu-%u.tmp", (unsigned long long) job, attempt);
        fd = openat(stage->dir, stage->temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) stage->temp[0] = 0;
    return fd;
}
static int sa_stage_write(int fd, const uint8_t *bytes, size_t size) {
    int ok = 1;
    for (size_t at = 0; at < size;) {
        ssize_t wrote = write(fd, bytes + at, size - at);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            ok = 0;
            break;
        }
        at += (size_t) wrote;
    }
    if (ok && fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    return ok;
}
static int sa_stage(const struct sa_job *job, const char *path, const uint8_t *bytes, size_t size,
                    struct sa_staged *stage) {
    char root[PATH_MAX], tail[1024], *last;
    if (!sa_stage_path(job, path, root, tail) || !sa_stage_name(tail, stage, &last)) return 0;
    stage->dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stage->dir < 0) return 0;
    if (last) {
        *last = 0;
        if (!sa_stage_directories(tail, stage)) return 0;
    }
    int fd = sa_stage_file(job->id, stage);
    if (fd < 0) return 0;
    return sa_stage_write(fd, bytes, size);
}
static void sa_stage_drop(struct sa_staged *stage) {
    if (stage->dir < 0) return;
    if (stage->temp[0]) unlinkat(stage->dir, stage->temp, 0);
    close(stage->dir);
    stage->dir = -1;
}
static int sa_publish(struct sa_staged *stage) {
    if (renameat(stage->dir, stage->temp, stage->dir, stage->name)) return 0;
    stage->temp[0] = 0;
    return !fsync(stage->dir);
}
static int sa_sql_hex(const char *input, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) input; *p; p++) {
        if (n + 2 >= cap) return 0;
        out[n++] = hex[*p >> 4];
        out[n++] = hex[*p & 15];
    }
    out[n] = 0;
    return 1;
}
/* Only the core's existing native transaction spool writes library metadata.
 * Its playback gate defers application while a source is in use. */
static int sa_link_sql(const struct sa_job *job, char sql[13000]) {
    char audio[4096], analysis[2048];
    if (!sa_sql_hex(job->relative_audio, audio, sizeof audio) ||
        !sa_sql_hex(job->relative_dat, analysis, sizeof analysis)) return 0;
    int n = snprintf(sql, 13000,
                     "-- AZMOD analysis-link %u %s %s\n"
                     "UPDATE content SET analysisDataFilePath=CAST(X'%s' AS TEXT) WHERE content_id=%u AND path=CAST(X'%s' AS TEXT) "
                     "AND (analysisDataFilePath IS NULL OR analysisDataFilePath='');\n",
                     job->source.ref.track_id, audio, analysis, analysis, job->source.ref.track_id, audio);
    return n > 0 && n < 13000 ? n : 0;
}
static int sa_link_analysis(struct sa_job *job) {
    if (job->link_queued) return 1;
    char sql[13000], relative[128], path[1024];
    int n = sa_link_sql(job, sql);
    if (!n) return 0;
    if (!job->id) job->id = ++sa_serial;
    snprintf(relative, sizeof relative, "/PIONEER/azspool/track-analysis-%08x-%llu.sql", job->source.ref.track_id, (unsigned long long) job->id);
    if (!st_source_path(job->source.mount, relative, path)) return 0;
    struct sa_staged spool = {.dir = -1};
    int ok = sa_stage(job, path, (const uint8_t *) sql, (size_t) n, &spool) && sa_publish(&spool);
    sa_stage_drop(&spool);
    if (ok && !sa_link_enqueue(job, path)) {
        unlink(path);
        ok = 0;
    }
    if (ok) job->link_queued = 1;
    return ok;
}
/* Encoded native persistence tags, borrowed during one publication. */
static struct sa_job *sa_find_job(int owner, uint64_t job) {
    struct sa_job *found = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == job && sa_jobs[i].owner == owner) found = &sa_jobs[i];
    return found;
}
struct sa_publication {
    struct sa_tags packet;
    struct sa_job *job;
    uint8_t *dat, *ext, *ex2, *newdat, *newext, *newex2;
    size_t dn, en, vfn, ndn, nen, nvn;
    struct sa_staged td, te, tv;
    int files_saved;
};
static int sa_publication_matches(const struct sa_publication *s) {
    const struct sa_tags *p = &s->packet;
    return !!p->pn == !!(s->job->needed & AZ_DATA_PHRASES) &&
           !!p->vn == !!(s->job->needed & AZ_ANALYSIS_VOCALS) && (!p->gn || (s->job->needed & AZ_DATA_GRID));
}
static int sa_publication_read(struct sa_publication *s) {
    int failed = 0;
    s->dat = sa_read(s->job->dat, &s->dn, &failed);
    s->ext = sa_read(s->job->ext, &s->en, &failed);
    if (s->packet.vn) s->ex2 = sa_read(s->job->ex2, &s->vfn, &failed);
    return failed ? AZ_FAILED : AZ_OK;
}
static int sa_publication_loaded_grid(struct sa_publication *s, int existing) {
    struct sa_tags *p = &s->packet;
    const struct sa_job *j = s->job;
    if (p->gn || !j->native_grid || !j->native_grid_bytes) return AZ_INVALID;
    if (!existing) {
        p->grid = j->native_grid;
        p->gn = (uint32_t) j->native_grid_bytes;
        return AZ_OK;
    }
    uint32_t saved_size = 0;
    const uint8_t *saved_grid = sa_find_tag(s->dat, s->dn, "PQTZ", &saved_size);
    return sa_grid_same(j, saved_grid, saved_size) ? AZ_OK : AZ_STALE;
}
static int sa_publication_grid(struct sa_publication *s) {
    int existing = sa_tag_count(s->dat, s->dn, "PQTZ");
    if (s->job->loaded) {
        int rc = sa_publication_loaded_grid(s, existing);
        if (rc != AZ_OK) return rc;
    }
    if (existing < 0 || sa_tag_count(s->ext, s->en, "PSSI") != 0 || sa_tag_count(s->dat, s->dn, "PSSI") != 0 ||
        (!!s->packet.gn == !!existing)) return AZ_STALE;
    return AZ_OK;
}
static int sa_publication_parse_grid(const struct sa_publication *s, struct st_data *parsed) {
    if (s->packet.gn) return st_section(parsed, s->packet.grid, 24, s->packet.gn, AZ_DATA_GRID);
    uint32_t saved_size = 0;
    const uint8_t *saved = sa_find_tag(s->dat, s->dn, "PQTZ", &saved_size);
    return saved ? st_section(parsed, saved, sa_u32(saved + 4), saved_size, AZ_DATA_GRID) : AZ_FAILED;
}
static int sa_publication_dimensions_valid(const struct sa_tags *p, const struct st_data *parsed) {
    return parsed->view.beat_count && parsed->view.phrase_count &&
           sa_u16(p->phrase + 26) <= parsed->view.beat_count && p->pn == 32 + parsed->view.phrase_count * 24 &&
           (!p->gn || p->gn == 24 + parsed->view.beat_count * 8);
}
static int sa_publication_beats_valid(const struct st_data *parsed) {
    for (unsigned i = 0; i < parsed->view.beat_count; i++)
        if (parsed->beats[i].beat_in_bar < 1 || parsed->beats[i].beat_in_bar > 4 ||
            !isfinite(parsed->beats[i].bpm) || parsed->beats[i].bpm <= 0) return 0;
    return 1;
}
static int sa_publication_parse_phrases(const struct sa_publication *s) {
    struct st_data *parsed = calloc(1, sizeof *parsed);
    if (!parsed) return AZ_FAILED;
    int rc = sa_publication_parse_grid(s, parsed);
    if (!rc) rc = st_section(parsed, s->packet.phrase, 32, s->packet.pn, AZ_DATA_PHRASES);
    if (!rc && !sa_publication_dimensions_valid(&s->packet, parsed)) rc = AZ_INVALID;
    if (!rc && !sa_publication_beats_valid(parsed)) rc = AZ_INVALID;
    free(parsed);
    return rc;
}
static int sa_publication_phrases(struct sa_publication *s) {
    struct sa_tags *p = &s->packet;
    if (!p->pn) return p->gn ? AZ_INVALID : AZ_OK;
    int rc = sa_publication_grid(s);
    if (rc != AZ_OK) return rc;
    rc = sa_publication_parse_phrases(s);
    if (rc != AZ_OK) return rc;
    s->newext = sa_merge(s->ext, s->en, p->phrase, p->pn, &s->nen);
    if (p->gn) s->newdat = sa_merge(s->dat, s->dn, p->grid, p->gn, &s->ndn);
    return s->newext && (!p->gn || s->newdat) ? AZ_OK : AZ_FAILED;
}
static int sa_publication_vocals(struct sa_publication *s) {
    const struct sa_tags *p = &s->packet;
    if (!p->vn) return AZ_OK;
    if (sa_tag_count(s->ex2, s->vfn, "PVDI") != 0) return AZ_STALE;
    s->newex2 = sa_merge(s->ex2, s->vfn, p->vocal, p->vn, &s->nvn);
    uint8_t marker[32] = {0};
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result): native FourCC stores exactly four bytes.
    memcpy(marker, "AZVD", 4);
    sa_put32(marker + 4, 32);
    sa_put32(marker + 8, 32);
    sa_put32(marker + 12, 1);
    sa_put32(marker + 16, p->method);
    sa_put32(marker + 20, sv_hash(p->vocal, p->vn));
    sa_put32(marker + 24, p->frames);
    sa_put32(marker + 28, p->rate);
    size_t marked_n = 0;
    uint8_t *marked = s->newex2 ? sa_merge(s->newex2, s->nvn, marker, sizeof marker, &marked_n) : NULL;
    free(s->newex2);
    s->newex2 = marked;
    s->nvn = marked_n;
    return s->newex2 ? AZ_OK : AZ_FAILED;
}
static int sa_publication_stage(struct sa_publication *s) {
    return (!s->packet.gn || sa_stage(s->job, s->job->dat, s->newdat, s->ndn, &s->td)) &&
           (!s->packet.pn || sa_stage(s->job, s->job->ext, s->newext, s->nen, &s->te)) &&
           (!s->packet.vn || sa_stage(s->job, s->job->ex2, s->newex2, s->nvn, &s->tv));
}
static int sa_publication_publish(struct sa_publication *s) {
    /* Each successful component remains reusable after a partial failure. */
    return (!s->packet.gn || sa_publish(&s->td)) && (!s->packet.pn || sa_publish(&s->te)) &&
           (!s->packet.vn || sa_publish(&s->tv));
}
static int sa_publication_notify(struct sa_publication *s) {
    struct sa_job *j = s->job;
    if (!sa_owner_live(j)) return AZ_STALE;
    if (j->loaded && s->packet.pn) {
        int rc = j->maps->publish_phrases((unsigned) j->deck, j->load, &j->native_map, s->packet.phrase, s->packet.pn);
        if (rc) return rc;
    }
    if (j->needs_link && !sa_link_analysis(j)) return AZ_FAILED;
    int rc = j->needs_link ? AZ_PENDING : AZ_OK;
    sa_job_drop(j);
    return rc;
}
static void sa_publication_record(const struct sa_publication *s) {
    /* Preserve the qualified identities of every component we did not write. */
    if (s->packet.gn) s->job->signatures[2] = st_disk_sig(s->job->dat);
    if (s->packet.pn) s->job->signatures[3] = st_disk_sig(s->job->ext);
    if (s->packet.vn) s->job->signatures[4] = st_disk_sig(s->job->ex2);
}
static int sa_publication_run(struct sa_publication *s) {
    if (!sa_publication_matches(s)) return AZ_INVALID;
    int rc = sa_publication_read(s);
    if (rc != AZ_OK) return rc;
    rc = sa_publication_phrases(s);
    if (rc != AZ_OK) return rc;
    rc = sa_publication_vocals(s);
    if (rc != AZ_OK) return rc;
    if (!sa_publication_stage(s)) return AZ_FAILED;
    if (!sa_current(s->job)) return AZ_STALE;
    if (!sa_publication_publish(s)) return AZ_FAILED;
    /* Only our durable writes advance this baseline. Native-only retries must
     * not adopt another writer's edits as the saved publication's identity. */
    sa_publication_record(s);
    s->files_saved = 1;
    return sa_publication_notify(s);
}
static int sa_publication_shape_same(const struct sa_tags *a, const struct sa_tags *b) {
    return a->gn == b->gn && a->pn == b->pn && a->vn == b->vn &&
           a->rate == b->rate && a->frames == b->frames && a->method == b->method;
}
static int sa_publication_bytes_same(const struct sa_tags *a, const struct sa_tags *b) {
    return !memcmp(a->grid, b->grid, a->gn) && !memcmp(a->phrase, b->phrase, a->pn) &&
           !memcmp(a->vocal, b->vocal, a->vn);
}
static int sa_publication_resume(struct sa_publication *s) {
    const struct sa_tags *saved = &s->job->published_tags;
    if (!sa_publication_shape_same(&s->packet, saved) || !sa_publication_bytes_same(&s->packet, saved)) return AZ_STALE;
    return sa_publication_notify(s);
}
static int sa_publication_commit(int owner, uint64_t job, struct sa_publication *s, uint8_t **storage) {
    s->job = sa_find_job(owner, job);
    if (!s->job || !sa_current(s->job)) return AZ_STALE;
    if (s->job->published) return sa_publication_resume(s);
    /* Loaded grid insertion may substitute the captured grid into packet during
     * persistence. Retain the caller's original encoding for retry comparison. */
    struct sa_tags original = s->packet;
    int rc = sa_publication_run(s);
    if (rc == AZ_BUSY && s->files_saved) {
        s->job->published = *storage;
        s->job->published_tags = original;
        *storage = NULL;
    }
    return rc;
}
static void sa_publication_drop(struct sa_publication *s) {
    sa_stage_drop(&s->td);
    sa_stage_drop(&s->te);
    sa_stage_drop(&s->tv);
    free(s->dat);
    free(s->ext);
    free(s->ex2);
    free(s->newdat);
    free(s->newext);
    free(s->newex2);
}
/* SPDX-License-Identifier: GPL-3.0-only */
/* Copied native grid retained for the owned computation input. */
struct sa_grid_view {
    const uint8_t *bytes;
    uint8_t *file;
    uint32_t size, header, count;
};
static int sa_grid_header_valid(struct sa_grid_view *grid) {
    if (grid->size < 24 || memcmp(grid->bytes, "PQTZ", 4)) return 0;
    grid->header = sa_u32(grid->bytes + 4);
    grid->count = sa_u32(grid->bytes + 20);
    return grid->header >= 24 && grid->header <= grid->size && grid->count && grid->count <= 32768 &&
           grid->count <= (grid->size - grid->header) / 8;
}
static int sa_grid_rows_valid(const struct sa_grid_view *grid) {
    for (uint32_t i = 0; i < grid->count; i++) {
        const uint8_t *row = grid->bytes + grid->header + i * 8;
        if (sa_u16(row) < 1 || sa_u16(row) > 4 || !sa_u16(row + 2)) return 0;
    }
    return 1;
}
static int sa_grid_read(const struct sa_job *job, struct sa_grid_view *grid) {
    grid->bytes = job->native_grid;
    grid->size = (uint32_t) job->native_grid_bytes;
    if (!grid->bytes) {
        size_t file_size = 0;
        int failed = 0;
        grid->file = sa_read(job->dat, &file_size, &failed);
        if (failed) return AZ_FAILED;
        grid->bytes = sa_find_tag(grid->file, file_size, "PQTZ", &grid->size);
    }
    if (!grid->bytes) return AZ_NOT_FOUND;
    if (!sa_grid_header_valid(grid) || !sa_grid_rows_valid(grid)) return AZ_FAILED;
    return AZ_OK;
}
static void sa_grid_copy(const struct sa_grid_view *grid, az_beat *beats) {
    for (uint32_t i = 0; i < grid->count; i++) {
        const uint8_t *row = grid->bytes + grid->header + i * 8;
        beats[i] = (az_beat) {.time_s = (double) sa_u32(row + 4) * 0.001,
                              .bpm = (double) sa_u16(row + 2) * 0.01,
                              .beat_in_bar = sa_u16(row)};
    }
}
static int sa_commit_typed(int owner, uint64_t job, const az_analysis_result_v1 *result) {
    if (owner < 0 || !job || !sa_result_valid(result)) return AZ_INVALID;
    struct sa_publication state = {.td = {.dir = -1}, .te = {.dir = -1}, .tv = {.dir = -1}};
    uint8_t *storage = sa_encode_native(result, &state.packet);
    if (!storage) return AZ_LIMIT;
    int rc = AZ_BUSY;
    if (!pthread_mutex_trylock(&sa_mutex)) {
        rc = sa_publication_commit(owner, job, &state, &storage);
        sa_publication_drop(&state);
        pthread_mutex_unlock(&sa_mutex);
    }
    free(storage);
    return rc;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Worker-side loaded leases. Native objects remain behind the private service. */
static const struct azstatus_native_v1 *sa_native(void) {
    azstatus_native_fn get = (azstatus_native_fn) dlsym(RTLD_DEFAULT, "azstem_status_service_v1");
    const struct azstatus_native_v1 *api = get ? get() : NULL;
    return api && api->version == 1 && api->size >= offsetof(struct azstatus_native_v1, analysis_source) + sizeof api->analysis_source ? api : NULL;
}
static int sa_loaded_current(const struct sa_job *j) {
    return j->maps && j->maps->current((unsigned) j->deck, j->load, &j->native_map) == AZ_OK &&
           (!j->audio_token || (j->audio_api && j->audio_api->current(j->audio_token) == AZ_OK));
}
static void sa_job_drop(struct sa_job *j) {
    if (j->reader_fd >= 0 && j->reader_path[0]) close(j->reader_fd);
    if (j->maps) j->maps->release(&j->native_map);
    if (j->audio_token && j->audio_api) j->audio_api->close(j->audio_token);
    free(j->native_grid);
    free(j->published);
    memset(j, 0, sizeof *j);
    j->reader_fd = -1;
}
static void sa_put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
struct sa_grid_capture {
    uint32_t count, rate;
    az_beat *rows;
    int64_t *ticks;
    uint8_t *file;
    size_t tag_size;
};
static void sa_grid_capture_drop(struct sa_grid_capture *capture) {
    free(capture->file);
    free(capture->rows);
    free(capture->ticks);
}
static int sa_grid_capture_allocate(struct sa_grid_capture *capture) {
    capture->tag_size = 24u + capture->count * 8u;
    capture->rows = calloc(capture->count, sizeof *capture->rows);
    capture->ticks = calloc(capture->count, sizeof *capture->ticks);
    capture->file = calloc(1, capture->tag_size);
    return capture->rows && capture->ticks && capture->file ? AZ_OK : AZ_FAILED;
}
static int sa_grid_capture_read(struct sa_job *job, struct sa_grid_capture *capture) {
    uint32_t count = 0;
    int rc = job->maps->read(&job->native_map.grid, AZNM_GRID, capture->rows, capture->count, &count);
    if (rc) return rc;
    if (count != capture->count) return AZ_STALE;
    rc = job->maps->read_samples(&job->native_map.grid, capture->ticks, capture->count, &count, &capture->rate);
    if (rc) return rc;
    return count != capture->count || !capture->rate ? AZ_STALE : AZ_OK;
}
static unsigned sa_grid_first_bar(const struct sa_grid_capture *capture) {
    for (unsigned i = 0; i < capture->count; i++)
        if (capture->rows[i].beat_in_bar == 1) return i;
    return capture->count;
}
static int sa_grid_encode_row(const struct sa_grid_capture *capture, unsigned first, unsigned i, uint8_t *out) {
    unsigned beat = capture->rows[i].beat_in_bar ? capture->rows[i].beat_in_bar : (i + 4u - first % 4u) % 4u + 1u;
    double ms = (double) capture->ticks[i] * 1000.0 / capture->rate, bpm = capture->rows[i].bpm * 100.0;
    if (beat > 4 || !isfinite(ms) || ms < 0 || ms > UINT32_MAX || !isfinite(bpm) || bpm < 1 || bpm > UINT16_MAX) return AZ_UNSUPPORTED;
    sa_put16(out, beat);
    sa_put16(out + 2, (unsigned) llround(bpm));
    sa_put32(out + 4, (uint32_t) llround(ms));
    return AZ_OK;
}
static int sa_grid_encode(struct sa_grid_capture *capture) {
    unsigned first = sa_grid_first_bar(capture);
    if (first == capture->count) return AZ_UNSUPPORTED;
    uint8_t *grid = capture->file;
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result): native FourCC stores exactly four bytes.
    memcpy(grid, "PQTZ", 4);
    sa_put32(grid + 4, 24);
    sa_put32(grid + 8, (uint32_t) capture->tag_size);
    sa_put32(grid + 16, 0x80000);
    sa_put32(grid + 20, capture->count);
    for (unsigned i = 0; i < capture->count; i++) {
        int rc = sa_grid_encode_row(capture, first, i, grid + 24 + i * 8);
        if (rc) return rc;
    }
    return AZ_OK;
}
/* Preserve the native rows and clock in a read-only PQTZ snapshot for PSSI. */
static int sa_capture_grid(struct sa_job *j) {
    struct sa_grid_capture capture = {0};
    int rc = j->maps->read(&j->native_map.grid, AZNM_GRID, NULL, 0, &capture.count);
    if (rc) return rc;
    if (capture.count < 2) return AZ_BUSY;
    if (capture.count > 32768) return AZ_UNSUPPORTED;
    rc = sa_grid_capture_allocate(&capture);
    if (!rc) rc = sa_grid_capture_read(j, &capture);
    if (!rc) rc = sa_grid_encode(&capture);
    if (!rc) {
        j->native_grid = capture.file;
        j->native_grid_bytes = capture.tag_size;
        capture.file = NULL;
    }
    sa_grid_capture_drop(&capture);
    return rc;
}
static int sa_stream_create_dir(const char *path, int read_only) {
    return read_only || !mkdir(path, 0700) || errno == EEXIST;
}
static int sa_stream_directory(struct sa_job *j, int read_only) {
    char base[1024];
    if (snprintf(base, sizeof base, "%s/analysis", az_analysis_state_dir()) >= (int) sizeof base ||
        !sa_stream_create_dir(base, read_only)) return AZ_FAILED;
    /* 32 bounded slots shared by all decks. Verify the entire digest on a hit. */
    if (snprintf(j->source.mount, sizeof j->source.mount, "%s/beatport-v2-%c%c", base,
                 j->stream_digest[0], j->stream_digest[1] > '7' ? '1' : '0') >= (int) sizeof j->source.mount ||
        !sa_stream_create_dir(j->source.mount, read_only)) return AZ_FAILED;
    return AZ_OK;
}
static int sa_stream_read_directory(const struct sa_job *j) {
    struct stat directory;
    if (lstat(j->source.mount, &directory)) return errno == ENOENT ? AZ_NOT_FOUND : AZ_FAILED;
    return S_ISDIR(directory.st_mode) ? AZ_OK : AZ_FAILED;
}
static int sa_stream_file_paths(struct sa_job *j) {
    return st_source_path(j->source.mount, "/analysis.DAT", j->dat) &&
           st_source_path(j->source.mount, "/analysis.EXT", j->ext) &&
           st_source_path(j->source.mount, "/analysis.2EX", j->ex2) &&
           st_source_path(j->source.mount, "/identity", j->stream_identity);
}
static int sa_stream_key_matches(const struct sa_job *j) {
    char old[65];
    int fd = open(j->stream_identity, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    ssize_t bytes = read(fd, old, sizeof old);
    close(fd);
    return bytes == 64 && !memcmp(old, j->stream_digest, 64);
}
static int sa_stream_unlink(const char *path) {
    return !unlink(path) || errno == ENOENT;
}
static int sa_stream_clear_phrases(struct sa_job *j) {
    if (!sa_stream_unlink(j->ext) || !sa_stream_unlink(j->dat)) return AZ_FAILED;
    j->signatures[2] = j->signatures[3] = 0;
    return AZ_OK;
}
static int sa_stream_replace(struct sa_job *j) {
    if (sa_stream_clear_phrases(j) || !sa_stream_unlink(j->ex2)) return AZ_FAILED;
    struct sa_staged staged = {.dir = -1};
    int ok = sa_stage(j, j->stream_identity, (const uint8_t *) j->stream_digest, 64, &staged) && sa_publish(&staged);
    sa_stage_drop(&staged);
    return ok ? AZ_OK : AZ_FAILED;
}
/* sa_mutex serializes writers and protects leases in other decks sharing a slot. */
static int sa_stream_slot_busy(const struct sa_job *j) {
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id && sa_jobs[i].streaming &&
            !strcmp(j->source.mount, sa_jobs[i].source.mount)) return 1;
    return 0;
}
static int sa_stream_bind_key(struct sa_job *j, int read_only) {
    j->signatures[0] = st_disk_sig(j->stream_identity);
    if (sa_stream_key_matches(j)) return AZ_OK;
    if (read_only) return AZ_NOT_FOUND;
    int rc = sa_stream_replace(j);
    j->signatures[0] = st_disk_sig(j->stream_identity);
    return rc;
}
static int sa_stream_bind_files(struct sa_job *j, int read_only) {
    int rc = sa_stream_bind_key(j, read_only);
    if (rc) return rc;
    j->signatures[2] = st_disk_sig(j->dat);
    j->signatures[3] = st_disk_sig(j->ext);
    j->signatures[4] = st_disk_sig(j->ex2);
    return j->signatures[0] && st_disk_sig(j->stream_identity) == j->signatures[0] ? AZ_OK : AZ_STALE;
}
static int sa_stream_paths(struct sa_job *j, int read_only) {
    int rc = sa_stream_directory(j, read_only);
    if (!rc) rc = sa_stream_read_directory(j);
    if (rc) return rc;
    if (!read_only && sa_stream_slot_busy(j)) return AZ_BUSY;
    return sa_stream_file_paths(j) ? sa_stream_bind_files(j, read_only) : AZ_FAILED;
}
static const uint8_t *sa_find_tag(const uint8_t *data, size_t n, const char tag[4], uint32_t *length) {
    if (!data || !sa_framed(data, n)) return NULL;
    for (size_t at = sa_u32(data + 4); at < n; at += sa_u32(data + at + 8))
        if (!memcmp(data + at, tag, 4)) {
            *length = sa_u32(data + at + 8);
            return data + at;
        }
    return NULL;
}
static int sa_grid_row_same(const uint8_t *file, const uint8_t *native, uint32_t rate) {
    if (sa_u16(file) != sa_u16(native) || sa_u16(file + 2) != sa_u16(native + 2)) return 0;
    /* PQTZ rounds an exact native sample coordinate to whole milliseconds. */
    int order = compare_timestamps(sa_u32(file + 4), sa_u32(native + 4), 1000, rate, (rate + 999u) / 1000u + 1u);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static int sa_grid_same(const struct sa_job *j, const uint8_t *grid, uint32_t size) {
    if (!grid || size < 24 || sa_u32(grid + 4) != 24 || size != j->native_grid_bytes ||
        sa_u32(grid + 20) != sa_u32(j->native_grid + 20)) return 0;
    uint32_t rate = j->native_map.grid.sample_rate;
    if (!rate) return 0;
    for (uint32_t at = 24; at < size; at += 8)
        if (!sa_grid_row_same(grid + at, j->native_grid + at, rate)) return 0;
    return 1;
}
/* The locator transaction changes the DB signature. Keep it deferred while
 * this same request still needs vocals, so its own commit cannot stale its input. */
static int sa_saved_link(struct sa_job *job) {
    if (!job->needs_link || (job->needed & AZ_ANALYSIS_VOCALS)) return AZ_OK;
    return sa_link_analysis(job) ? AZ_OK : AZ_FAILED;
}
static int sa_saved_phrases_publish(struct sa_job *j, const uint8_t *grid, uint32_t grid_size,
                                    const uint8_t *phrase, uint32_t phrase_size) {
    if (!phrase || !sa_u16(phrase + 16)) return AZ_OK;
    if (!sa_grid_same(j, grid, grid_size) || !sa_loaded_current(j) || !sa_owner_live(j)) return AZ_STALE;
    int rc = sa_saved_link(j);
    if (rc) return rc;
    rc = j->maps->publish_phrases((unsigned) j->deck, j->load, &j->native_map, phrase, phrase_size);
    return rc ? rc : AZ_NOT_FOUND;
}
static int sa_saved_stream_grid(struct sa_job *j, const uint8_t *grid, uint32_t size) {
    if (!j->streaming || !grid || sa_grid_same(j, grid, size)) return AZ_NOT_FOUND;
    return sa_current(j) ? sa_stream_clear_phrases(j) : AZ_STALE;
}
static int sa_saved_phrases(struct sa_job *j, const uint8_t *dat, size_t dat_size, const uint8_t *ext, size_t ext_size) {
    int grid_count = sa_tag_count(dat, dat_size, "PQTZ");
    int dat_phrases = sa_tag_count(dat, dat_size, "PSSI"), ext_phrases = sa_tag_count(ext, ext_size, "PSSI");
    if (grid_count < 0 || dat_phrases < 0 || ext_phrases < 0 || (dat_phrases && ext_phrases)) return AZ_FAILED;
    uint32_t grid_size = 0, phrase_size = 0;
    const uint8_t *grid = sa_find_tag(dat, dat_size, "PQTZ", &grid_size);
    int rc = sa_saved_stream_grid(j, grid, grid_size);
    if (rc != AZ_NOT_FOUND) return rc;
    const uint8_t *phrase = ext_phrases ? sa_find_tag(ext, ext_size, "PSSI", &phrase_size)
                                       : sa_find_tag(dat, dat_size, "PSSI", &phrase_size);
    return sa_saved_phrases_publish(j, grid, grid_size, phrase, phrase_size);
}
/* Saved phrase indexes are usable only against the captured native grid. */
static int sa_refresh_saved(struct sa_job *j) {
    size_t dat_size = 0, ext_size = 0;
    int failed = 0;
    uint8_t *dat = sa_read(j->dat, &dat_size, &failed), *ext = sa_read(j->ext, &ext_size, &failed);
    int rc = failed ? AZ_FAILED : sa_saved_phrases(j, dat, dat_size, ext, ext_size);
    free(dat);
    free(ext);
    return rc;
}
static int sa_recapture_loaded(struct sa_job *j) {
    j->maps->release(&j->native_map);
    memset(&j->native_map, 0, sizeof j->native_map);
    int rc = j->maps->capture((unsigned) j->deck, j->load, &j->native_map);
    if (rc) j->maps = NULL;
    return rc;
}
static int sa_loaded_phrase_need(struct sa_job *j, uint32_t native_phrases) {
    j->needed = (j->needed & AZ_ANALYSIS_VOCALS) | (native_phrases ? 0 : AZ_DATA_PHRASES);
    if (native_phrases) return AZ_OK;
    int rc = sa_refresh_saved(j);
    if (rc != AZ_NOT_FOUND) return rc;
    j->needed &= ~AZ_DATA_PHRASES;
    return j->vocals ? sa_recapture_loaded(j) : AZ_OK;
}
static int sa_loaded_vocal_need(struct sa_job *j) {
    j->needed &= ~AZ_ANALYSIS_VOCALS;
    if (!j->vocals) return AZ_OK;
    int failed = 0;
    size_t n = 0;
    uint8_t *file = sa_read(j->ex2, &n, &failed);
    int count = sa_tag_count(file, n, "PVDI");
    free(file);
    if (failed || count < 0) return AZ_FAILED;
    if (!count) j->needed |= AZ_ANALYSIS_VOCALS;
    return AZ_OK;
}
/* Publishing saved phrases retires the old packet; a vocal lease recaptures it. */
static int sa_loaded_needs(struct sa_job *j, uint32_t native_phrases) {
    int rc = sa_loaded_vocal_need(j);
    if (rc) return rc;
    rc = sa_loaded_phrase_need(j, native_phrases);
    return rc ? rc : j->needed ? AZ_OK
                               : ANALYSIS_SOURCE_COMPLETE;
}
static const struct azn_maps_api *sa_loaded_maps(const struct azstatus_native_v1 *api) {
    const struct azn_maps_api *maps = api && api->maps ? api->maps() : NULL;
    return maps && maps->size >= offsetof(struct azn_maps_api, publish_phrases) + sizeof maps->publish_phrases &&
                   maps->capture && maps->current && maps->publish_phrases
               ? maps
               : NULL;
}
static int sa_capture_loaded(struct sa_job *j, uint32_t *phrases) {
    int rc = j->maps->capture((unsigned) j->deck, j->load, &j->native_map);
    if (rc) {
        j->maps = NULL;
        return rc;
    }
    return j->maps->read(&j->native_map.phrases, AZNM_PHRASES, NULL, 0, phrases);
}
static void sa_usb_attach(struct sa_job *found, const struct sa_job *native) {
    struct sa_job saved = *found;
    saved.loaded = 1;
    saved.deck = native->deck;
    saved.load = native->load;
    saved.native_epoch = native->native_epoch;
    saved.maps = native->maps;
    saved.native_map = native->native_map;
    saved.native_grid = native->native_grid;
    saved.native_grid_bytes = native->native_grid_bytes;
    *found = saved;
}
static struct sa_job *sa_find_owned_job(uint64_t id, int owner) {
    struct sa_job *found = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == id && sa_jobs[i].owner == owner) found = &sa_jobs[i];
    return found;
}
/* Native-only reuse does not authorize a locator to absent persistent maps.
 * Complete saved files may still need their existing conditional transaction. */
static int sa_usb_reuse_link(struct sa_job *job, uint32_t stored_needed, int rc) {
    if (rc != ANALYSIS_SOURCE_COMPLETE || !job->needs_link) return rc;
    if (job->link_queued) return AZ_PENDING;
    if (stored_needed) return rc;
    if (!sa_current(job)) return AZ_STALE;
    return sa_link_analysis(job) ? AZ_PENDING : AZ_FAILED;
}
static int sa_usb_bind(struct sa_job *j, uint32_t phrases, struct sa_open_result *result) {
    struct sa_job *found = sa_find_owned_job(result->job, j->owner);
    if (!found || found->lifetime.generation != j->lifetime.generation) {
        sa_job_drop(j);
        return AZ_STALE;
    }
    sa_usb_attach(found, j);
    uint32_t stored_needed = found->needed;
    int rc = sa_loaded_current(found) ? sa_loaded_needs(found, phrases) : AZ_STALE;
    result->needed = found->needed;
    rc = sa_usb_reuse_link(found, stored_needed, rc);
    if (rc) {
        if (rc != AZ_PENDING) sa_link_release(found->id);
        sa_job_drop(found);
    }
    return rc;
}
static int sa_usb_register(struct sa_job *j, uint32_t phrases, struct sa_open_result *result) {
    if (pthread_mutex_lock(&sa_mutex)) {
        sa_job_drop(j);
        return AZ_FAILED;
    }
    int rc = sa_usb_bind(j, phrases, result);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_usb_open(struct sa_job *j, uint32_t phrases, struct sa_open_result *result) {
    az_track_ref track = {0};
    int rc = az_status_loaded_reference((unsigned) j->deck, j->load, &track);
    if (!rc) rc = sa_open(j->lifetime, &track, result, 1, j->vocals);
    if (rc) {
        sa_job_drop(j);
        return rc;
    }
    return sa_usb_register(j, phrases, result);
}
static pthread_mutex_t sa_digest_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct sa_digest_entry {
    uint64_t signature;
    char digest[65];
} sa_digests[4];

static int sa_digest_cached(struct sa_job *j) {
    pthread_mutex_lock(&sa_digest_mutex);
    const struct sa_digest_entry *entry = &sa_digests[j->deck];
    int hit = entry->signature == j->signatures[1];
    if (hit) memcpy(j->stream_digest, entry->digest, sizeof entry->digest);
    pthread_mutex_unlock(&sa_digest_mutex);
    return hit;
}
static void sa_digest_remember(const struct sa_job *j) {
    pthread_mutex_lock(&sa_digest_mutex);
    struct sa_digest_entry *entry = &sa_digests[j->deck];
    entry->signature = j->signatures[1];
    memcpy(entry->digest, j->stream_digest, sizeof entry->digest);
    pthread_mutex_unlock(&sa_digest_mutex);
}
static int sa_stream_fingerprint(struct sa_job *j) {
    j->signatures[1] = st_disk_sig(j->audio);
    if (!j->signatures[1]) return AZ_STALE;
    if (sa_digest_cached(j)) return AZ_OK;
    if (!st_hash_file(j->audio, j->signatures[1], j->stream_digest)) return AZ_STALE;
    sa_digest_remember(j);
    return AZ_OK;
}
static const struct azn_analysis_source_api *sa_stream_api(const struct azstatus_native_v1 *api) {
    const struct azn_analysis_source_api *source = api->analysis_source ? api->analysis_source() : NULL;
    return source && source->size >= sizeof *source && source->open && source->current && source->close ? source : NULL;
}
static int sa_stream_capture(struct sa_job *j, const struct azstatus_native_v1 *api) {
    j->streaming = 1;
    j->audio_api = sa_stream_api(api);
    if (!j->audio_api) return AZ_UNSUPPORTED;
    struct azn_analysis_source source = {.size = sizeof source, .version = 1};
    int rc = j->audio_api->open((unsigned) j->deck, j->native_epoch, j->load, &source);
    if (rc) return rc;
    j->audio_token = source.token;
    memcpy(j->audio, source.audio_path, sizeof j->audio);
    return sa_stream_fingerprint(j);
}
static int sa_stream_deck_busy(int deck) {
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id && sa_jobs[i].loaded && sa_jobs[i].deck == deck) return 1;
    return 0;
}
static int sa_stream_capacity(unsigned slot, unsigned owned) {
    return slot < 8 && owned < 4 && sa_serial != UINT64_MAX;
}
static int sa_stream_slot(const struct sa_job *job, unsigned *slot) {
    if (sa_stream_deck_busy(job->deck)) return AZ_BUSY;
    unsigned owned = 0;
    *slot = 8;
    for (unsigned i = 0; i < 8; i++) {
        if (sa_jobs[i].id && sa_jobs[i].owner == job->owner) owned++;
        if (!sa_jobs[i].id && *slot == 8) *slot = i;
    }
    if (!sa_owner_live(job)) return AZ_STALE;
    return sa_stream_capacity(*slot, owned) ? AZ_OK : AZ_LIMIT;
}
static int sa_stream_prepare(struct sa_job *j, uint32_t phrases, unsigned *slot) {
    int rc = sa_stream_slot(j, slot);
    if (rc) return rc;
    if (!sa_loaded_current(j)) return AZ_STALE;
    j->id = ++sa_serial;
    rc = sa_stream_paths(j, 0);
    return rc ? rc : sa_loaded_needs(j, phrases);
}
static void sa_stream_retain(const struct sa_job *j, unsigned slot, struct sa_open_result *result) {
    sa_jobs[slot] = *j;
    result->job = j->id;
    memcpy(result->audio_path, j->reader_path, sizeof result->audio_path);
}
static int sa_stream_register(struct sa_job *j, uint32_t phrases, struct sa_open_result *result) {
    if (pthread_mutex_trylock(&sa_mutex)) {
        sa_job_drop(j);
        return AZ_BUSY;
    }
    unsigned slot = 8;
    int rc = sa_stream_prepare(j, phrases, &slot);
    result->needed = j->needed;
    if (!rc && !sa_current(j)) rc = AZ_STALE;
    if (!rc) rc = sa_reader_open(j);
    if (!rc) sa_stream_retain(j, slot, result);
    else sa_job_drop(j);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_stream_open(struct sa_job *j, const struct azstatus_native_v1 *api, uint32_t phrases, struct sa_open_result *result) {
    int rc = sa_stream_capture(j, api);
    if (rc) {
        sa_job_drop(j);
        return rc == AZ_NOT_FOUND ? AZ_BUSY : rc; /* Wait for the native download. */
    }
    return sa_stream_register(j, phrases, result);
}
static int sa_loaded_seed(const struct azstatus_native_v1 *api, int deck, uint64_t load, struct azstatus_native_receipt *receipt) {
    return api->load_seed && api->load_seed((unsigned) deck, receipt) && receipt->load_generation == load ? AZ_OK : AZ_STALE;
}
static void sa_loaded_result(struct sa_open_result *result, struct sa_open_result *out, int rc) {
    if (rc == AZ_OK || rc == AZ_PENDING) {
        memcpy(out, result, sizeof *result);
    } else if (rc == ANALYSIS_SOURCE_COMPLETE) {
        result->job = 0;
        memcpy(out, result, sizeof *result);
    }
}
static int sa_loaded_open_source(struct sa_job *j, const struct azstatus_native_v1 *api,
                                 const struct azstatus_native_receipt *receipt, uint32_t phrases, struct sa_open_result *out) {
    struct sa_open_result result = {.needed = AZ_DATA_PHRASES};
    if (receipt->original_bpm_state == AZN_BPM_READY) result.original_bpm = receipt->original_bpm;
    int rc;
    if (receipt->source_kind == AZ_STATUS_SOURCE_USB1 || receipt->source_kind == AZ_STATUS_SOURCE_USB2)
        rc = sa_usb_open(j, phrases, &result);
    else rc = sa_stream_open(j, api, phrases, &result);
    sa_loaded_result(&result, out, rc);
    return rc;
}
static int sa_loaded_lease(struct sa_job *j, const struct azstatus_native_v1 *api,
                           const struct azstatus_native_receipt *receipt, struct sa_open_result *out) {
    uint32_t phrases = 0;
    int rc = sa_capture_loaded(j, &phrases);
    if (!sa_owner_live(j)) rc = AZ_STALE;
    if (rc) {
        sa_job_drop(j);
        return rc;
    }
    if (phrases && !j->vocals) {
        struct sa_open_result existing = {0};
        memcpy(out, &existing, sizeof existing);
        sa_job_drop(j);
        return ANALYSIS_SOURCE_COMPLETE;
    }
    if (!phrases && (rc = sa_capture_grid(j))) {
        sa_job_drop(j);
        return rc;
    }
    return sa_loaded_open_source(j, api, receipt, phrases, out);
}
static int sa_loaded_request_valid(int owner, int deck, uint64_t load, const struct sa_open_result *out) {
    return owner >= 0 && deck >= 0 && deck < 4 && load && out;
}
static int sa_open_loaded(az_analysis_owner lifetime, int deck, uint64_t load, struct sa_open_result *out, int vocals) {
    int owner = sa_source_owner_index(lifetime);
    if (!sa_loaded_request_valid(owner, deck, load, out)) return AZ_INVALID;
    if (!analysis_owner_current(lifetime)) return AZ_STALE;
    if (sa_cleaner_start()) return AZ_FAILED;
    const struct azstatus_native_v1 *api = sa_native();
    const struct azn_maps_api *maps = sa_loaded_maps(api);
    if (!maps) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    int rc = sa_loaded_seed(api, deck, load, &receipt);
    if (rc) return rc;
    struct sa_job job = {.owner = owner, .lifetime = lifetime, .reader_fd = -1, .loaded = 1, .deck = deck, .load = load, .native_epoch = receipt.epoch, .maps = maps, .vocals = vocals};
    return sa_loaded_lease(&job, api, &receipt, out);
}


/* SPDX-License-Identifier: GPL-3.0-only */
/* Copied worker reads share the core's source resolution and freshness checks. */
static int sv_track_valid(const az_track_ref *track) {
    return track && st_record_size(track) >= sizeof *track && track->version == AZ_DATA_VERSION &&
           track->source_id >= 1 && track->source_id <= 2 && track->track_id;
}
static int sv_source(const az_track_ref *track, struct sa_job *j) {
    if (st_source_read((int) track->source_id - 1, &j->source)) return AZ_STALE;
    j->source.ref.track_id = track->track_id;
    if (!st_ref_equal(track, &j->source.ref) ||
        !st_source_path(j->source.mount, "/PIONEER/rekordbox/exportLibrary.db", j->database) ||
        !st_database_quiet(j->database)) return AZ_STALE;
    j->signatures[0] = st_disk_sig(j->database);
    return AZ_OK;
}
static int sv_query(const az_track_ref *track, struct sa_job *j, struct st_resolution *row) {
    void *db = sdk_db_open(j->database);
    if (!db) return AZ_FAILED;
    int rc = st_read_row(db, track->track_id, row);
    az_storage_sql()->close(db);
    return rc;
}
static unsigned sv_duration(const struct st_resolution *row) {
    return row->duration_ticks > 0 && row->duration_ticks < 86400 ? (unsigned) row->duration_ticks : 0;
}
static int sv_analysis_paths(struct sa_job *j, const struct st_resolution *row) {
    size_t n = strlen(row->analysis);
    if (row->analysis_limited || n < 4 || strcmp(row->analysis + n - 4, ".DAT")) return AZ_UNSUPPORTED;
    snprintf(j->relative_audio, sizeof j->relative_audio, "%s", row->content);
    snprintf(j->relative_dat, sizeof j->relative_dat, "%s", row->analysis);
    char relative[1024];
    snprintf(relative, sizeof relative, "%s", row->analysis);
    if (!st_source_path(j->source.mount, row->content, j->audio) ||
        !st_source_path(j->source.mount, relative, j->dat)) return AZ_UNSUPPORTED;
    memcpy(relative + n - 4, ".EXT", 5);
    if (!st_source_path(j->source.mount, relative, j->ext)) return AZ_UNSUPPORTED;
    memcpy(relative + n - 4, ".2EX", 5);
    return st_source_path(j->source.mount, relative, j->ex2) ? AZ_OK : AZ_UNSUPPORTED;
}
static void sv_signatures(struct sa_job *j) {
    j->signatures[1] = st_disk_sig(j->audio);
    j->signatures[2] = st_disk_sig(j->dat);
    j->signatures[3] = st_disk_sig(j->ext);
    j->signatures[4] = st_disk_sig(j->ex2);
    j->vocals = 1;
}
static int sv_location(const az_track_ref *track, struct sa_job *j) {
    if (!sv_track_valid(track)) return AZ_INVALID;
    if (!az_storage_sql_ready()) return AZ_UNSUPPORTED;
    int rc = sv_source(track, j);
    if (rc) return rc;
    struct st_resolution row = {0};
    rc = sv_query(track, j, &row);
    if (rc) return rc;
    j->vocal_length_s = sv_duration(&row);
    rc = sv_analysis_paths(j, &row);
    if (rc) return rc;
    sv_signatures(j);
    return sa_current(j) ? AZ_OK : AZ_STALE;
}
static int sv_read(struct sa_job *j, az_vocal_snapshot_v1 *out, az_vocal_interval *intervals, uint32_t capacity) {
    int failed = 0;
    size_t n = 0;
    uint8_t *file = sa_read(j->ex2, &n, &failed);
    az_vocal_snapshot_v1 result = {.size = out->size, .version = AZ_DATA_VERSION};
    int rc = failed ? AZ_FAILED : sv_file(file, n, &result, NULL, 0);
    if (rc == AZ_OK || rc == AZ_LIMIT) {
        if (!sa_current(j)) rc = AZ_STALE;
        else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            result.info.observed_ns = (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
            result.info.revision = j->signatures[4];
            if (result.interval_count <= capacity) rc = sv_file(file, n, &result, intervals, capacity);
            if (rc == AZ_OK || rc == AZ_LIMIT) memcpy(out, &result, sizeof result);
        }
    }
    free(file);
    return rc;
}
int analysis_vocal_intervals(az_analysis_owner lifetime, const az_track_ref *track, az_vocal_snapshot_v1 *out,
                             az_vocal_interval *intervals, uint32_t capacity) {
    int owner = sa_source_owner_index(lifetime);
    if (!analysis_owner_current(lifetime)) return AZ_STALE;
    if (owner < 0 || !out || st_record_size(out) < sizeof *out ||
        (!intervals && capacity)) return AZ_INVALID;
    struct sa_job j = {.owner = owner, .lifetime = lifetime, .reader_fd = -1};
    int rc = sv_location(track, &j);
    return rc ? rc : sv_read(&j, out, intervals, capacity);
}
static int sv_output_valid(const az_vocal_snapshot_v1 *out, const az_vocal_interval *intervals, uint32_t capacity) {
    return out && (intervals || !capacity);
}
static const struct azn_maps_api *sv_loaded_maps(const struct azstatus_native_v1 *api) {
    const struct azn_maps_api *maps = api && api->maps ? api->maps() : NULL;
    return maps && maps->capture && maps->current && maps->release && api->load_seed ? maps : NULL;
}
static int sv_stream_open(struct sa_job *j, const struct azstatus_native_v1 *api) {
    /* Reads never evict another load's cache or create settings files. */
    int rc = sa_stream_capture(j, api);
    if (!rc) rc = sa_stream_paths(j, 1);
    if (rc) sa_job_drop(j);
    return rc;
}
static int sv_loaded_location(struct sa_job *j, const struct azstatus_native_v1 *api, unsigned source_kind) {
    if (source_kind != AZ_STATUS_SOURCE_USB1 && source_kind != AZ_STATUS_SOURCE_USB2) return sv_stream_open(j, api);
    az_track_ref ref = {0};
    int rc = az_status_loaded_reference((unsigned) j->deck, j->load, &ref);
    return rc ? rc : sv_location(&ref, j);
}
static int sv_loaded_read(struct sa_job *j, const struct azn_maps_api *maps, az_vocal_snapshot_v1 *out,
                          az_vocal_interval *intervals, uint32_t capacity) {
    int rc = maps->capture((unsigned) j->deck, j->load, &j->native_map);
    if (!rc) {
        j->maps = maps;
        j->loaded = 1;
        rc = sv_read(j, out, intervals, capacity);
    }
    sa_job_drop(j);
    return rc;
}
int analysis_vocal_intervals_loaded(az_analysis_owner lifetime, int deck, uint64_t load, az_vocal_snapshot_v1 *out,
                                    az_vocal_interval *intervals, uint32_t capacity) {
    int owner = sa_source_owner_index(lifetime);
    if (!analysis_owner_current(lifetime)) return AZ_STALE;
    if (deck < 0 || deck >= 4 || !load ||
        !sv_output_valid(out, intervals, capacity)) return AZ_INVALID;
    const struct azstatus_native_v1 *api = sa_native();
    const struct azn_maps_api *maps = sv_loaded_maps(api);
    if (!maps) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    if (sa_loaded_seed(api, deck, load, &receipt)) return AZ_STALE;
    struct sa_job j = {.owner = owner, .lifetime = lifetime, .reader_fd = -1, .deck = deck, .load = load, .native_epoch = receipt.epoch, .vocals = 1};
    int rc = sv_loaded_location(&j, api, receipt.source_kind);
    return rc ? rc : sv_loaded_read(&j, maps, out, intervals, capacity);
}

struct va_bytes {
    const uint8_t *data;
    size_t size;
};
static int va_read_bytes(void *context, uint8_t *out, size_t count, uint64_t offset) {
    const struct va_bytes *bytes = context;
    if (offset > bytes->size || count > bytes->size - offset) return 0;
    memcpy(out, bytes->data + offset, count);
    return 1;
}
static int va_tag_count(const uint8_t *file, size_t size) {
    int count = 0;
    for (size_t at = sa_u32(file + 4); at < size; at += sa_u32(file + at + 8))
        count += !memcmp(file + at, "PVDI", 4);
    return count;
}
static int va_parse(const uint8_t *file, size_t size, struct vd_track *out) {
    if (!file) return AZ_NOT_FOUND;
    int count = va_tag_count(file, size);
    if (!count) return AZ_NOT_FOUND;
    if (count != 1) return AZ_FAILED;
    struct va_bytes bytes = {file, size};
    int rc = vd_anlz_read(va_read_bytes, &bytes, size, SA_FILE_MAX, out);
    if (rc < 0) return AZ_FAILED;
    return out->n ? AZ_OK : AZ_NOT_FOUND;
}
static int va_read(struct sa_job *job, struct vd_track *out, unsigned *length) {
    size_t size = 0;
    int failed = 0;
    uint8_t *file = sa_read(job->ex2, &size, &failed);
    struct vd_track track = {0};
    int rc = failed ? AZ_FAILED : va_parse(file, size, &track);
    free(file);
    if (!sa_current(job)) rc = AZ_STALE;
    if (rc != AZ_OK) {
        free(track.v);
        return rc;
    }
    unsigned seconds = job->vocal_length_s;
    if (!seconds) seconds = (unsigned) (((uint64_t) track.n * track.hop + track.rate / 2) / track.rate);
    *length = seconds ? seconds : 1;
    *out = track;
    return AZ_OK;
}
static int va_capture(struct sa_job *job, const struct azn_maps_api *maps,
                      struct vd_track *out, unsigned *length) {
    int rc = maps->capture((unsigned) job->deck, job->load, &job->native_map);
    if (rc) return rc;
    job->maps = maps;
    job->loaded = 1;
    return va_read(job, out, length);
}
static int va_loaded(const struct azstatus_native_v1 *api, unsigned deck, uint64_t load,
                     struct vd_track *out, unsigned *length) {
    const struct azn_maps_api *maps = sv_loaded_maps(api);
    if (!maps) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    if (sa_loaded_seed(api, (int) deck, load, &receipt)) return AZ_STALE;
    struct sa_job job = {.display_read = 1, .reader_fd = -1, .deck = (int) deck, .load = load, .native_epoch = receipt.epoch, .vocals = 1};
    int rc = sv_loaded_location(&job, api, receipt.source_kind);
    if (!rc) rc = va_capture(&job, maps, out, length);
    sa_job_drop(&job);
    return rc;
}
int analysis_vocal_track_loaded(unsigned deck, uint64_t load, struct vd_track *out,
                                unsigned *length_s) {
    if (deck >= 4 || !load || !out || !length_s) return AZ_INVALID;
    return va_loaded(sa_native(), deck, load, out, length_s);
}


/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded analysis publication progression, owned by the core service worker. */
struct sa_link {
    uint64_t id;
    az_analysis_owner owner;
    struct st_source source;
    char database[1024], audio[2048], analysis[1024], spool[1024];
    int admitted, outcome;
};
static struct sa_link sa_links[8];
static _Atomic(void (*)(void)) sa_completion_wake;
void analysis_source_set_wake(void (*wake)(void)) {
    atomic_store_explicit(&sa_completion_wake, wake, memory_order_release);
}
static void sa_link_notify(void) {
    void (*wake)(void) = atomic_load_explicit(&sa_completion_wake, memory_order_acquire);
    if (wake) wake();
}
static int sa_link_enqueue(struct sa_job *job, const char *path) {
    struct sa_link *entry = NULL;
    for (unsigned i = 0; i < 8 && !entry; ++i)
        if (!sa_links[i].id || sa_links[i].id == job->id) entry = &sa_links[i];
    if (!entry) return 0;
    *entry = (struct sa_link) {.id = job->id, .owner = job->lifetime, .source = job->source, .outcome = AZ_PENDING};
    memcpy(entry->database, job->database, sizeof entry->database);
    memcpy(entry->audio, job->relative_audio, sizeof entry->audio);
    memcpy(entry->analysis, job->relative_dat, sizeof entry->analysis);
    snprintf(entry->spool, sizeof entry->spool, "%s", path);
    sa_link_wake();
    return 1;
}
static int sa_link_row(const struct sa_link *link) {
    if (!st_database_quiet(link->database)) return AZ_PENDING;
    void *db = sdk_db_open(link->database);
    if (!db) return AZ_FAILED;
    struct st_resolution row = {0};
    int rc = st_read_row(db, link->source.ref.track_id, &row);
    az_storage_sql()->close(db);
    if (rc) return rc;
    if (strcmp(row.content, link->audio)) return AZ_STALE;
    if (!row.analysis_generated) return strcmp(row.analysis, link->analysis) ? AZ_STALE : AZ_OK;
    return strcmp(row.analysis, link->analysis) ? AZ_STALE : AZ_PENDING;
}
static int sa_link_current(const struct sa_link *link) {
    struct st_source source;
    if (st_source_read((int) link->source.ref.source_id - 1, &source)) return AZ_STALE;
    source.ref.track_id = link->source.ref.track_id;
    if (!st_resolution_source_equal(&source, &link->source)) return AZ_STALE;
    return sa_link_row(link);
}
static int sa_link_file_state(const struct sa_link *link) {
    char marker[1100];
    snprintf(marker, sizeof marker, "%s.failed", link->spool);
    if (!access(marker, F_OK)) return AZ_FAILED;
    snprintf(marker, sizeof marker, "%s.rejected", link->spool);
    if (!access(marker, F_OK)) return AZ_FAILED;
    return access(link->spool, F_OK) ? AZ_FAILED : AZ_PENDING;
}
static int sa_link_progress(struct sa_link *link) {
    if (!analysis_owner_current(link->owner) && !link->admitted) {
        unlink(link->spool);
        return AZ_STALE;
    }
    int rc = sa_link_current(link);
    if (rc != AZ_PENDING) return rc;
    rc = sa_link_file_state(link);
    if (rc != AZ_PENDING || link->admitted) return rc;
    rc = az_analysis_spool_admit((int) link->source.ref.source_id - 1);
    if (rc == AZ_BUSY) return AZ_PENDING;
    if (rc != AZ_OK) return rc;
    link->admitted = 1;
    return AZ_PENDING;
}
static int sa_links_service(void) {
    int pending = 0, changed = 0;
    pthread_mutex_lock(&sa_mutex);
    for (unsigned i = 0; i < 8; ++i) {
        struct sa_link *link = &sa_links[i];
        if (!link->id) continue;
        if (!analysis_owner_current(link->owner)) {
            sa_link_release(link->id);
            continue;
        }
        if (link->outcome != AZ_PENDING) continue;
        link->outcome = sa_link_progress(link);
        pending |= link->outcome == AZ_PENDING;
        changed |= link->outcome != AZ_PENDING;
    }
    pthread_mutex_unlock(&sa_mutex);
    if (changed) sa_link_notify();
    return pending;
}
static void sa_link_release(uint64_t id) {
    for (unsigned i = 0; i < 8; ++i) {
        struct sa_link *link = &sa_links[i];
        if (!link->id || link->id != id) continue;
        if (!link->admitted) unlink(link->spool);
        memset(link, 0, sizeof *link);
    }
}
int analysis_source_outcome(az_analysis_owner owner, const analysis_source_input *input) {
    if (!input || !input->lease) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    int rc = AZ_STALE;
    for (unsigned i = 0; i < 8; ++i) {
        const struct sa_link *link = &sa_links[i];
        if (link->id == input->lease && link->owner.generation == owner.generation) rc = link->outcome;
    }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Deferred reclamation and private computation input boundary. */
#include <semaphore.h>
static pthread_mutex_t sa_cleaner_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t sa_cleaner_wake;
static _Atomic int sa_cleaner_started;
static _Atomic int sa_cleanup_pending;
static int sa_take_retired(struct sa_job *out) {
    pthread_mutex_lock(&sa_mutex);
    int found = 0;
    for (unsigned i = 0; i < 8 && !found; ++i) {
        if (!sa_jobs[i].id || sa_owner_live(&sa_jobs[i])) continue;
        *out = sa_jobs[i];
        memset(&sa_jobs[i], 0, sizeof sa_jobs[i]);
        sa_jobs[i].reader_fd = -1;
        found = 1;
    }
    pthread_mutex_unlock(&sa_mutex);
    return found;
}
static void *sa_cleaner(void *unused) {
    (void) unused;
    int pending = 0;
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        if (pending) {
            while (sem_timedwait(&sa_cleaner_wake, &deadline) && errno == EINTR) {}
        } else {
            while (sem_wait(&sa_cleaner_wake) && errno == EINTR) {}
        }
        atomic_store_explicit(&sa_cleanup_pending, 0, memory_order_release);
        struct sa_job job;
        while (sa_take_retired(&job)) sa_job_drop(&job);
        pending = sa_links_service();
    }
    return NULL;
}
static int sa_cleaner_launch(void) {
    if (sem_init(&sa_cleaner_wake, 0, 0)) return AZ_FAILED;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attributes, SCHED_OTHER);
    struct sched_param priority = {0};
    pthread_attr_setschedparam(&attributes, &priority);
    pthread_t thread;
    int rc = az_analysis_thread_create(&thread, &attributes, sa_cleaner, NULL);
    pthread_attr_destroy(&attributes);
    if (rc) sem_destroy(&sa_cleaner_wake);
    return rc ? AZ_FAILED : AZ_OK;
}
static int sa_cleaner_start(void) {
    pthread_mutex_lock(&sa_cleaner_mutex);
    if (!sa_cleaner_started) sa_cleaner_started = sa_cleaner_launch() == AZ_OK;
    int rc = sa_cleaner_started ? AZ_OK : AZ_FAILED;
    pthread_mutex_unlock(&sa_cleaner_mutex);
    return rc;
}
static void sa_link_wake(void) {
    if (!atomic_load_explicit(&sa_cleaner_started, memory_order_acquire)) return;
    if (!atomic_exchange_explicit(&sa_cleanup_pending, 1, memory_order_acq_rel))
        sem_post(&sa_cleaner_wake);
}
void analysis_source_retire(az_analysis_owner owner) {
    if (owner.generation) sa_link_wake();
}
static int sa_source_owner_index(az_analysis_owner owner) {
    return owner.kind == AZ_ANALYSIS_OWNER_CORE ? 32 : (int) owner.slot;
}
static int sa_input_grid(const struct sa_job *job, analysis_source_input *input) {
    if (!(input->needed & AZ_DATA_PHRASES)) return AZ_OK;
    struct sa_grid_view grid = {0};
    int rc = sa_grid_read(job, &grid);
    if (rc == AZ_OK) {
        input->beats = calloc(grid.count, sizeof *input->beats);
        if (!input->beats) rc = AZ_FAILED;
        else {
            sa_grid_copy(&grid, input->beats);
            input->beat_count = grid.count;
        }
    }
    free(grid.file);
    /* File-only callers can provide a generated grid with their typed result. */
    return rc == AZ_NOT_FOUND && !job->loaded ? AZ_OK : rc;
}
static int sa_input_copy(az_analysis_owner owner, const analysis_source_request *request,
                         const struct sa_open_result *lease, analysis_source_input *input) {
    pthread_mutex_lock(&sa_mutex);
    struct sa_job *job = sa_find_job(sa_source_owner_index(owner), lease->job);
    int rc = AZ_STALE;
    if (job && job->lifetime.generation == owner.generation && sa_current(job)) {
        job->needed &= request->components | AZ_DATA_GRID;
        if (!(job->needed & AZ_DATA_PHRASES)) job->needed &= ~AZ_DATA_GRID;
        input->needed = job->needed;
        input->load_epoch = job->native_epoch;
        rc = input->needed ? sa_input_grid(job, input) : ANALYSIS_SOURCE_COMPLETE;
        if (rc == AZ_OK) {
            input->reader_fd = job->reader_fd;
            job->reader_fd = -1;
        }
    }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_input_request_valid(const analysis_source_request *request, const analysis_source_input *input) {
    return request && input && request->components &&
           !(request->components & ~(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS));
}
int analysis_source_acquire(az_analysis_owner owner, const analysis_source_request *request,
                            analysis_source_input *input) {
    if (!sa_input_request_valid(request, input)) return AZ_INVALID;
    if (!analysis_owner_current(owner)) return AZ_STALE;
    struct sa_open_result lease = {0};
    int vocals = !!(request->components & AZ_ANALYSIS_VOCALS);
    int rc = request->loaded ? sa_open_loaded(owner, request->deck, request->load, &lease, vocals) : sa_open(owner, &request->track, &lease, 0, vocals);
    if (rc != AZ_OK && rc != AZ_PENDING) return rc;
    *input = (analysis_source_input) {.lease = lease.job, .owner = owner, .reader_fd = -1, .bpm = lease.original_bpm, .track = lease.track, .deck = (unsigned) request->deck};
    memcpy(input->audio_path, lease.audio_path, sizeof input->audio_path);
    if (rc == AZ_PENDING) return rc;
    rc = sa_input_copy(owner, request, &lease, input);
    if (rc) analysis_source_release(input);
    return rc;
}
int analysis_source_publish(az_analysis_owner owner, const analysis_source_input *input,
                            const az_analysis_result_v1 *result) {
    if (!input || !input->lease) return AZ_INVALID;
    if (!analysis_owner_current(owner) || owner.generation != input->owner.generation) return AZ_STALE;
    return sa_commit_typed(sa_source_owner_index(owner), input->lease, result);
}
void analysis_source_release(analysis_source_input *input) {
    if (!input) return;
    pthread_mutex_lock(&sa_mutex);
    struct sa_job *job = sa_find_job(sa_source_owner_index(input->owner), input->lease);
    struct sa_job retired = {.reader_fd = -1};
    if (job) {
        retired = *job;
        memset(job, 0, sizeof *job);
        job->reader_fd = -1;
    }
    sa_link_release(input->lease);
    pthread_mutex_unlock(&sa_mutex);
    sa_job_drop(&retired);
    if (input->audio_path[0] && input->reader_fd >= 0) close(input->reader_fd);
    free(input->beats);
    memset(input, 0, sizeof *input);
}


/* SPDX-License-Identifier: GPL-3.0-only */
/* Public worker-only source leases and ANLZ publication. Included by community.c. */
#include "sdk_analysis_write.h"
#include "analysis_source.h"
#include "sdk_vocals.h"
#include "../common/azstatus_maps.h"
#include "../common/azstatus_native.h"
#include "../common/azanalysis_native.h"
struct sa_job {
    uint64_t id;
    int owner, needs_link;
    az_analysis_owner lifetime;
    int reader_fd;
    char reader_path[1024];
    struct st_source source;
    char database[1024], audio[1024], dat[1024], ext[1024], ex2[1024];
    char relative_audio[2048], relative_dat[1024];
    uint64_t signatures[5];
    uint32_t needed;
    int vocals;
    int loaded, deck, streaming;
    uint64_t load, native_epoch, audio_token;
    const struct azn_analysis_source_api *audio_api;
    const struct azn_maps_api *maps;
    struct aznm_packet native_map;
    char grid_snapshot[1024];
    uint8_t *native_grid;
    size_t native_grid_bytes;
};
static struct sa_job sa_jobs[8];
static uint64_t sa_serial;
static pthread_mutex_t sa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int sa_link_analysis(struct sa_job *job);
static int sa_link_enqueue(struct sa_job *job, const char *path);
static void sa_link_wake(void);
static int sa_cleaner_start(void);
static az_analysis_owner sa_lifetime(int owner) {
    return owner == 32 ? analysis_owner_core() : analysis_owner_get((unsigned) owner);
}
static int sa_owner_live(const struct sa_job *job) {
    return analysis_owner_current(job->lifetime);
}
static int sa_loaded_current(const struct sa_job *job);
static void sa_job_drop(struct sa_job *job);
static int sa_grid_same(const struct sa_job *, const uint8_t *, uint32_t);
static const uint8_t *sa_find_tag(const uint8_t *, size_t, const char[4], uint32_t *);
static int sa_owner(const char *id, const char *name, uintptr_t caller) {
    if (g_cm_on_ui || g_sdk_on_tap_thread) return -1;
    int owner = st_mod_owner(id, caller, name);
    return analysis_owner_current(analysis_owner_for_caller((unsigned) owner)) ? owner : -1;
}
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
static int sa_current(const struct sa_job *j) {
    if (!sa_owner_live(j)) return 0;
    if (j->vocals && st_disk_sig(j->ex2) != j->signatures[4]) return 0;
    if (j->loaded && !sa_loaded_current(j)) return 0;
    if (j->streaming) return sa_disk_files_match(j);
    struct st_source source;
    if (st_source_read((int) j->source.ref.source_id - 1, &source)) return 0;
    source.ref.track_id = j->source.ref.track_id;
    return sa_source_paths_match(j, &source) && sa_source_signatures_match(j, &source);
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
static int sa_open_request_valid(int owner, const az_track_ref *track, const analysis_source_lease *out) {
    return owner >= 0 && track && out && st_record_size(out) >= sizeof *out &&
           st_record_size(track) >= sizeof *track && track->version == AZ_DATA_VERSION &&
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
    g_sdk_sq.close(db);
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
static int sa_open_link_existing(struct sa_job *j, const az_track_ref *track, analysis_source_lease *out) {
    if (j->needs_link && (!sa_current(j) || !sa_link_analysis(j))) return AZ_FAILED;
    analysis_source_lease linked = {.size = out->size, .version = AZ_DATA_VERSION, .flags = j->needs_link ? AZ_ANALYSIS_NEEDS_LINK : 0, .job = j->id, .track = *track};
    memcpy(out, &linked, sizeof linked);
    return j->needs_link ? AZ_PENDING : AZ_NOT_FOUND;
}
static int sa_reader_open(struct sa_job *j) {
    j->reader_fd = open(j->audio, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (j->reader_fd < 0) return AZ_STALE;
    snprintf(j->reader_path, sizeof j->reader_path, "/proc/%ld/fd/%d", (long) getpid(), j->reader_fd);
    return AZ_OK;
}
static void sa_open_publish(struct sa_job *j, const az_track_ref *track, analysis_source_lease *out,
                            unsigned slot, double bpm) {
    j->id = ++sa_serial;
    sa_jobs[slot] = *j;
    analysis_source_lease result = {.size = out->size, .version = AZ_DATA_VERSION, .needed = j->needed, .job = j->id, .flags = j->needs_link ? AZ_ANALYSIS_NEEDS_LINK : 0, .track = *track, .original_bpm = bpm};
    memcpy(result.audio_path, j->reader_path, sizeof result.audio_path);
    memcpy(result.analysis_path, j->dat, sizeof result.analysis_path);
    memcpy(out, &result, sizeof result);
}
static int sa_open_prepare(const az_track_ref *track, struct sa_job *job, struct st_resolution *row) {
    int rc = sa_open_row(track, job, row);
    if (rc != AZ_OK) return rc;
    if (!sa_open_paths(job, row)) return AZ_UNSUPPORTED;
    if (!job->signatures[0] || !job->signatures[1]) return AZ_STALE;
    return sa_open_needed(job);
}
static int sa_open_reserved(int owner, const az_track_ref *track, analysis_source_lease *out,
                            int loaded, int vocals, unsigned slot) {
    struct sa_job j = {.owner = owner, .lifetime = sa_lifetime(owner), .reader_fd = -1, .vocals = vocals};
    struct st_resolution row = {0};
    int rc = sa_open_prepare(track, &j, &row);
    if (rc != AZ_OK) return rc;
    if (!loaded && !j.needed) return sa_open_link_existing(&j, track, out);
    if (!sa_current(&j)) return AZ_STALE;
    rc = sa_reader_open(&j);
    if (rc) return rc;
    sa_open_publish(&j, track, out, slot, row.metadata.original_bpm);
    return AZ_OK;
}
static int sa_open(int owner, const az_track_ref *track, analysis_source_lease *out, int loaded, int vocals) {
    if (!sa_open_request_valid(owner, track, out)) return AZ_INVALID;
    if (!analysis_owner_current(sa_lifetime(owner))) return AZ_STALE;
    if (sa_cleaner_start()) return AZ_FAILED;
    if (!sdk_sq_resolve()) return AZ_UNSUPPORTED;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    unsigned slot;
    int rc = AZ_LIMIT;
    if (sa_open_slot(owner, &slot)) rc = sa_open_reserved(owner, track, out, loaded, vocals, slot);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sdk_analysis_open(const char *id, const az_track_ref *track, analysis_source_lease *out) {
    return sa_open(sa_owner(id, "analysis_open", CM_RA()), track, out, 0, 0);
}
static int sdk_analysis_open_v2(const char *id, const az_track_ref *track, analysis_source_lease *out) {
    return sa_open(sa_owner(id, "analysis_open_v2", CM_RA()), track, out, 0, 1);
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
    char sql[13000], relative[128], path[1024];
    int n = sa_link_sql(job, sql);
    if (!n) return 0;
    if (!job->id) job->id = ++sa_serial;
    snprintf(relative, sizeof relative, "/PIONEER/azspool/track-analysis-%08x-%llu.sql", job->source.ref.track_id, (unsigned long long) job->id);
    if (!st_source_path(job->source.mount, relative, path)) return 0;
    struct sa_staged spool = {.dir = -1};
    int ok = sa_stage(job, path, (const uint8_t *) sql, (size_t) n, &spool) && sa_publish(&spool);
    sa_stage_drop(&spool);
    if (ok && !sa_link_enqueue(job, path)) { unlink(path); ok = 0; }
    return ok;
}
/* Encoded native persistence tags, borrowed during one publication. */
struct sa_tags {
    uint32_t gn, pn, vn, rate, frames, method;
    const uint8_t *grid, *phrase, *vocal;
};
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
    j->signatures[2] = st_disk_sig(j->dat);
    j->signatures[3] = st_disk_sig(j->ext);
    j->signatures[4] = st_disk_sig(j->ex2);
    if (j->loaded && s->packet.pn) {
        int rc = j->maps->publish_phrases((unsigned) j->deck, j->load, &j->native_map, s->packet.phrase, s->packet.pn);
        if (rc) return rc;
    }
    if (j->needs_link && !sa_link_analysis(j)) return AZ_FAILED;
    int rc = j->needs_link ? AZ_PENDING : AZ_OK;
    sa_job_drop(j);
    return rc;
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
    return sa_publication_notify(s);
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
#include "sdk_analysis_typed.c"
static int sdk_analysis_close(const char *id, uint64_t job) {
    if (g_cm_on_ui || g_sdk_on_tap_thread) return AZ_INVALID;
    int owner = st_mod_owner(id, CM_RA(), "analysis_close");
    if (owner < 0 || !job) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    int rc = AZ_STALE;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == job && sa_jobs[i].owner == owner) {
            sa_job_drop(&sa_jobs[i]);
            rc = AZ_OK;
            break;
        }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}

#include "sdk_analysis_loaded.c"
#include "sdk_vocal_service.c"

#include "analysis_link.c"
#include "analysis_source_adapter.c"

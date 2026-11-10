/* SPDX-License-Identifier: GPL-3.0-only */
/* Public worker-only source leases and ANLZ publication. Included by community.c. */
#include "sdk_analysis_write.h"
struct sa_job {
    uint64_t id;
    int owner, needs_link;
    struct st_source source;
    char database[1024], audio[1024], dat[1024], ext[1024];
    char relative_audio[2048], relative_dat[1024];
    uint64_t signatures[4];
};
static struct sa_job sa_jobs[8];
static uint64_t sa_serial;
static pthread_mutex_t sa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int sa_link_analysis(const struct sa_job *job);
static int sa_owner(const char *id, const char *name, uintptr_t caller) {
    return (g_cm_on_ui || g_sdk_on_tap_thread) ? -1 : st_mod_owner(id, caller, name);
}
static int sa_current(const struct sa_job *j) {
    struct st_source source;
    if (st_source_read((int) j->source.ref.source_id - 1, &source)) return 0;
    source.ref.track_id = j->source.ref.track_id;
    char audio[1024], dat[1024], ext[1024], relative[1024];
    snprintf(relative, sizeof relative, "%s", j->relative_dat);
    size_t len = strlen(relative);
    if (len < 4) return 0;
    memcpy(relative + len - 4, ".EXT", 5);
    if (!st_source_path(source.mount, j->relative_audio, audio) || strcmp(audio, j->audio) ||
        !st_source_path(source.mount, j->relative_dat, dat) || strcmp(dat, j->dat) ||
        !st_source_path(source.mount, relative, ext) || strcmp(ext, j->ext)) return 0;
    return st_resolution_source_equal(&source, &j->source) && st_database_quiet(j->database) &&
           st_disk_sig(j->database) == j->signatures[0] && st_disk_sig(j->audio) == j->signatures[1] &&
           st_disk_sig(j->dat) == j->signatures[2] && st_disk_sig(j->ext) == j->signatures[3];
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
static int sdk_analysis_open(const char *id, const az_track_ref *track, az_analysis_job_v1 *out) {
    int owner = sa_owner(id, "analysis_open", CM_RA());
    if (owner < 0 || !track || !out || st_record_size(out) < sizeof *out ||
        st_record_size(track) < sizeof *track || track->version != AZ_DATA_VERSION ||
        !track->track_id || track->source_id < 1 || track->source_id > 2) return AZ_INVALID;
    if (!sdk_sq_resolve()) return AZ_UNSUPPORTED;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    unsigned slot = 0, owned = 0;
    for (unsigned i = 0; i < 8; i++) owned += sa_jobs[i].id && sa_jobs[i].owner == owner;
    while (slot < 8 && sa_jobs[slot].id) slot++;
    int rc = AZ_LIMIT;
    if (slot == 8 || owned >= 4 || sa_serial == UINT64_MAX) goto done;
    struct sa_job j = {.owner = owner};
    struct st_resolution row = {0};
    rc = AZ_STALE;
    if (st_source_read((int) track->source_id - 1, &j.source)) goto done;
    j.source.ref.track_id = track->track_id;
    if (!st_ref_equal(track, &j.source.ref) ||
        !st_source_path(j.source.mount, "/PIONEER/rekordbox/exportLibrary.db", j.database) ||
        !st_database_quiet(j.database)) goto done;
    j.signatures[0] = st_disk_sig(j.database);
    void *db = sdk_db_open(j.database);
    if (!db) {
        rc = AZ_FAILED;
        goto done;
    }
    rc = st_read_row(db, track->track_id, &row);
    g_sdk_sq.close(db);
    if (rc) goto done;
    rc = AZ_UNSUPPORTED;
    if (!row.content[0] || strlen(row.analysis) < 4 || strcmp(row.analysis + strlen(row.analysis) - 4, ".DAT") || row.analysis_limited ||
        !st_source_path(j.source.mount, row.content, j.audio) ||
        !st_source_path(j.source.mount, row.analysis, j.dat)) goto done;
    size_t len = strlen(j.dat);
    if (len < 4 || strcmp(j.dat + len - 4, ".DAT")) goto done;
    snprintf(j.relative_audio, sizeof j.relative_audio, "%s", row.content);
    snprintf(j.relative_dat, sizeof j.relative_dat, "%s", row.analysis);
    j.needs_link = row.analysis_generated;
    char relative[1024];
    snprintf(relative, sizeof relative, "%s", row.analysis);
    memcpy(relative + strlen(relative) - 4, ".EXT", 5);
    if (!st_source_path(j.source.mount, relative, j.ext)) goto done;
    j.signatures[1] = st_disk_sig(j.audio);
    j.signatures[2] = st_disk_sig(j.dat);
    j.signatures[3] = st_disk_sig(j.ext);
    if (!j.signatures[0] || !j.signatures[1]) {
        rc = AZ_STALE;
        goto done;
    }
    size_t sizes[2] = {0, 0};
    int failed = 0;
    uint8_t *dat = sa_read(j.dat, &sizes[0], &failed), *ext = sa_read(j.ext, &sizes[1], &failed);
    int beats = sa_tag_count(dat, sizes[0], "PQTZ"), phrases = sa_tag_count(ext, sizes[1], "PSSI");
    int dat_phrases = sa_tag_count(dat, sizes[0], "PSSI");
    free(dat);
    free(ext);
    rc = AZ_FAILED;
    if (failed || beats < 0 || phrases < 0 || dat_phrases < 0) goto done;
    if (phrases || dat_phrases) {
        if (j.needs_link) {
            if (!sa_current(&j) || !sa_link_analysis(&j)) goto done;
        }
        az_analysis_job_v1 linked = {.size = out->size, .version = AZ_DATA_VERSION, .flags = j.needs_link ? AZ_ANALYSIS_NEEDS_LINK : 0, .track = *track};
        memcpy(out, &linked, sizeof linked);
        rc = AZ_NOT_FOUND;
        goto done;
    }
    if (!sa_current(&j)) {
        rc = AZ_STALE;
        goto done;
    }
    j.id = ++sa_serial;
    sa_jobs[slot] = j;
    az_analysis_job_v1 result = {.size = out->size, .version = AZ_DATA_VERSION, .needed = AZ_DATA_PHRASES | (beats ? 0 : AZ_DATA_GRID), .job = j.id, .flags = j.needs_link ? AZ_ANALYSIS_NEEDS_LINK : 0, .track = *track, .original_bpm = row.metadata.original_bpm};
    memcpy(result.audio_path, j.audio, sizeof result.audio_path);
    memcpy(result.analysis_path, j.dat, sizeof result.analysis_path);
    memcpy(out, &result, sizeof result);
    rc = AZ_OK;
done:
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
/* Directory descriptors bind staging/publication to the validated USB tree.
 * Create missing ANLZ directories without following any link component. */
struct sa_staged {
    int dir;
    char temp[80], name[256];
};
static int sa_stage(const struct sa_job *job, const char *path, const uint8_t *bytes, size_t size,
                    struct sa_staged *stage) {
    char root[PATH_MAX], tail[1024];
    if (!realpath(job->source.mount, root)) return 0;
    size_t prefix = strlen(root);
    if (strncmp(root, path, prefix) || path[prefix] != '/' || strlen(path + prefix + 1) >= sizeof tail) return 0;
    snprintf(tail, sizeof tail, "%s", path + prefix + 1);
    char *last = strrchr(tail, '/'), *name = last ? last + 1 : tail;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= sizeof stage->name) return 0;
    snprintf(stage->name, sizeof stage->name, "%s", name);
    stage->dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stage->dir < 0) return 0;
    if (last) {
        *last = 0;
        char *save = NULL;
        for (char *part = strtok_r(tail, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
            if (!strcmp(part, ".") || !strcmp(part, "..")) return 0;
            if (mkdirat(stage->dir, part, 0777) && errno != EEXIST) return 0;
            int next = openat(stage->dir, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) return 0;
            close(stage->dir);
            stage->dir = next;
        }
    }
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        snprintf(stage->temp, sizeof stage->temp, ".az-analysis-%llu-%u.tmp", (unsigned long long) job->id, attempt);
        fd = openat(stage->dir, stage->temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        stage->temp[0] = 0;
        return 0;
    }
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
static int sa_link_analysis(const struct sa_job *job) {
    char audio[4096], analysis[2048], sql[6500], relative[128], path[1024];
    if (!sa_sql_hex(job->relative_audio, audio, sizeof audio) ||
        !sa_sql_hex(job->relative_dat, analysis, sizeof analysis)) return 0;
    int n = snprintf(sql, sizeof sql,
                     "UPDATE content SET analysisDataFilePath=CAST(X'%s' AS TEXT) WHERE content_id=%u AND path=CAST(X'%s' AS TEXT) "
                     "AND (analysisDataFilePath IS NULL OR analysisDataFilePath='');\n",
                     analysis, job->source.ref.track_id, audio);
    if (n < 0 || n >= (int) sizeof sql) return 0;
    snprintf(relative, sizeof relative, "/PIONEER/azspool/track-analysis-%08x.sql", job->source.ref.track_id);
    if (!st_source_path(job->source.mount, relative, path)) return 0;
    struct sa_staged spool = {.dir = -1};
    int ok = sa_stage(job, path, (const uint8_t *) sql, (size_t) n, &spool) && sa_publish(&spool);
    sa_stage_drop(&spool);
    return ok;
}
static int sdk_analysis_commit(const char *id, uint64_t job, const void *data, uint32_t size) {
    int owner = sa_owner(id, "analysis_commit", CM_RA());
    if (owner < 0 || !job || !data || size < 16 || size > 16 + 24 + 32768 * 8 + 32 + 1024 * 24) return AZ_INVALID;
    const uint8_t *bytes = data;
    uint32_t gn = sa_u32(bytes + 4), pn = sa_u32(bytes + 8), rate = sa_u32(bytes + 12);
    if (memcmp(bytes, "TA01", 4) || gn > size - 16 || pn != size - 16 - gn ||
        pn < 32 || (rate != 44100 && rate != 48000)) return AZ_INVALID;
    const uint8_t *grid = bytes + 16, *phrase = grid + gn;
    if (memcmp(phrase, "PSSI", 4) || sa_u32(phrase + 4) != 32 || sa_u32(phrase + 8) != pn ||
        sa_u16(phrase + 12) != 0 || sa_u16(phrase + 14) != 24 || !sa_u16(phrase + 16) ||
        (gn && (gn < 24 || memcmp(grid, "PQTZ", 4) || sa_u32(grid + 4) != 24 || sa_u32(grid + 8) != gn))) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    struct sa_job *j = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == job && sa_jobs[i].owner == owner) j = &sa_jobs[i];
    int rc = AZ_STALE;
    uint8_t *dat = NULL, *ext = NULL, *newdat = NULL, *newext = NULL;
    struct sa_staged td = {.dir = -1}, te = {.dir = -1};
    if (!j || !sa_current(j)) goto done;
    size_t dn = 0, en = 0, ndn = 0, nen = 0;
    int failed = 0;
    dat = sa_read(j->dat, &dn, &failed);
    ext = sa_read(j->ext, &en, &failed);
    if (failed) {
        rc = AZ_FAILED;
        goto done;
    }
    int existing = sa_tag_count(dat, dn, "PQTZ");
    if (existing < 0 || sa_tag_count(ext, en, "PSSI") != 0 || (gn && existing) || (!gn && !existing)) goto done;
    struct st_data *parsed = calloc(1, sizeof *parsed);
    if (!parsed) {
        rc = AZ_FAILED;
        goto done;
    }
    if (gn) rc = st_section(parsed, grid, 24, gn, AZ_DATA_GRID);
    else {
        rc = AZ_FAILED;
        for (size_t at = sa_u32(dat + 4); at < dn; at += sa_u32(dat + at + 8))
            if (!memcmp(dat + at, "PQTZ", 4)) rc = st_section(parsed, dat + at, sa_u32(dat + at + 4), sa_u32(dat + at + 8), AZ_DATA_GRID);
    }
    if (!rc) rc = st_section(parsed, phrase, 32, pn, AZ_DATA_PHRASES);
    if (!rc && (!parsed->view.beat_count || !parsed->view.phrase_count ||
                sa_u16(phrase + 26) > parsed->view.beat_count ||
                pn != 32 + parsed->view.phrase_count * 24 || (gn && gn != 24 + parsed->view.beat_count * 8))) rc = AZ_INVALID;
    /* Validate grid positions and tempos as well as increasing timestamp rows. */
    for (unsigned i = 0; !rc && i < parsed->view.beat_count; i++)
        if (parsed->beats[i].beat_in_bar < 1 || parsed->beats[i].beat_in_bar > 4 ||
            !isfinite(parsed->beats[i].bpm) || parsed->beats[i].bpm <= 0) rc = AZ_INVALID;
    free(parsed);
    if (rc) goto done;
    newext = sa_merge(ext, en, phrase, pn, &nen);
    if (gn) newdat = sa_merge(dat, dn, grid, gn, &ndn);
    if (!newext || (gn && !newdat)) {
        rc = AZ_FAILED;
        goto done;
    }
    if ((gn && !sa_stage(j, j->dat, newdat, ndn, &td)) || !sa_stage(j, j->ext, newext, nen, &te)) {
        rc = AZ_FAILED;
        goto done;
    }
    if (!sa_current(j)) {
        rc = AZ_STALE;
        goto done;
    }
    /* Grid is durable before any phrases reference it. If the second rename fails,
     * the valid generated grid remains and a subsequent job can fill PSSI only. */
    if (gn && !sa_publish(&td)) {
        rc = AZ_FAILED;
        goto done;
    }
    if (!sa_publish(&te)) {
        rc = AZ_FAILED;
        goto done;
    }
    if (j->needs_link && !sa_link_analysis(j)) {
        rc = AZ_FAILED;
        goto done;
    }
    memset(j, 0, sizeof *j);
    rc = AZ_OK;
done:
    sa_stage_drop(&td);
    sa_stage_drop(&te);
    free(dat);
    free(ext);
    free(newdat);
    free(newext);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sdk_analysis_close(const char *id, uint64_t job) {
    int owner = sa_owner(id, "analysis_close", CM_RA());
    if (owner < 0 || !job) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    int rc = AZ_STALE;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == job && sa_jobs[i].owner == owner) {
            memset(&sa_jobs[i], 0, sizeof sa_jobs[i]);
            rc = AZ_OK;
            break;
        }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}

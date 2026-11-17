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
    if (j->maps) j->maps->release(&j->native_map);
    if (j->audio_token && j->audio_api) j->audio_api->close(j->audio_token);
    if (j->grid_snapshot[0]) unlink(j->grid_snapshot);
    free(j->native_grid);
    memset(j, 0, sizeof *j);
}
static void sa_put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
/* The worker consumes a read-only PQTZ snapshot. PSSI uses grid indexes; this
 * snapshot preserves every native row and its current clock/offset. */
static int sa_grid_snapshot(struct sa_job *j) {
    uint32_t n = 0, ticks_n = 0, rate = 0;
    int rc = j->maps->read(&j->native_map.grid, AZNM_GRID, NULL, 0, &n);
    if (rc != AZ_OK || n < 2 || n > 32768) return rc == AZ_OK ? (n < 2 ? AZ_BUSY : AZ_UNSUPPORTED) : rc;
    az_beat *rows = calloc(n, sizeof *rows);
    int64_t *ticks = calloc(n, sizeof *ticks);
    size_t tag_size = 24u + n * 8u, total = 28u + tag_size;
    uint8_t *dat = calloc(1, total);
    if (!rows || !ticks || !dat) { free(rows); free(ticks); free(dat); return AZ_FAILED; }
    rc = j->maps->read(&j->native_map.grid, AZNM_GRID, rows, n, &ticks_n);
    if (!rc && ticks_n != n) rc = AZ_STALE;
    if (!rc) rc = j->maps->read_samples(&j->native_map.grid, ticks, n, &ticks_n, &rate);
    if (!rc && (ticks_n != n || !rate)) rc = AZ_STALE;
    unsigned first = n;
    for (unsigned i = 0; !rc && i < n; i++) if (rows[i].beat_in_bar == 1) { first = i; break; }
    if (!rc && first == n) rc = AZ_UNSUPPORTED;
    memcpy(dat, "PMAI", 4); sa_put32(dat + 4, 28); sa_put32(dat + 8, (uint32_t) total);
    uint8_t *grid = dat + 28;
    memcpy(grid, "PQTZ", 4); sa_put32(grid + 4, 24); sa_put32(grid + 8, (uint32_t) tag_size);
    sa_put32(grid + 16, 0x80000); sa_put32(grid + 20, n);
    for (unsigned i = 0; !rc && i < n; i++) {
        unsigned beat = rows[i].beat_in_bar ? rows[i].beat_in_bar : (i + 4u - first % 4u) % 4u + 1u;
        double ms = (double) ticks[i] * 1000.0 / rate, bpm = rows[i].bpm * 100.0;
        if (beat > 4 || !isfinite(ms) || ms < 0 || ms > UINT32_MAX || !isfinite(bpm) || bpm < 1 || bpm > UINT16_MAX) { rc = AZ_UNSUPPORTED; break; }
        sa_put16(grid + 24 + i * 8, beat); sa_put16(grid + 26 + i * 8, (unsigned) llround(bpm));
        sa_put32(grid + 28 + i * 8, (uint32_t) llround(ms));
    }
    if (!rc) {
        snprintf(j->grid_snapshot, sizeof j->grid_snapshot, "/tmp/az-analysis-grid-XXXXXX");
        int fd = mkostemp(j->grid_snapshot, O_CLOEXEC);
        if (fd < 0) { j->grid_snapshot[0] = 0; rc = AZ_FAILED; }
        else {
            size_t at = 0;
            while (at < total) {
                ssize_t wrote = write(fd, dat + at, total - at);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) break;
                at += (size_t) wrote;
            }
            if (at != total || fchmod(fd, 0400)) rc = AZ_FAILED;
            if (close(fd)) rc = AZ_FAILED;
        }
    }
    if (!rc) {
        j->native_grid = malloc(tag_size);
        if (!j->native_grid) rc = AZ_FAILED;
        else { memcpy(j->native_grid, grid, tag_size); j->native_grid_bytes = tag_size; }
    }
    free(dat); free(rows); free(ticks);
    return rc;
}
/* Four bounded per-deck caches. A new native audio fingerprint replaces that
 * deck's old cache; neither audio nor credentials are copied into settings. */
static int sa_stream_paths(struct sa_job *j, const unsigned char track[12], int read_only) {
    char base[1024], key[96], old[96] = {0}, identity[1024];
    if (snprintf(base, sizeof base, "%s/analysis", CM_STATE) >= (int) sizeof base ||
        (!read_only && mkdir(base, 0700) && errno != EEXIST)) return AZ_FAILED;
    if (snprintf(j->source.mount, sizeof j->source.mount, "%s/beatport-%d", base, j->deck) >= (int) sizeof j->source.mount ||
        (!read_only && mkdir(j->source.mount, 0700) && errno != EEXIST)) return AZ_FAILED;
    if (read_only) {
        struct stat directory;
        if (lstat(j->source.mount, &directory)) return errno == ENOENT ? AZ_NOT_FOUND : AZ_FAILED;
        if (!S_ISDIR(directory.st_mode)) return AZ_FAILED;
    }
    if (!st_source_path(j->source.mount, "/analysis.DAT", j->dat) ||
        !st_source_path(j->source.mount, "/analysis.EXT", j->ext) ||
        !st_source_path(j->source.mount, "/analysis.2EX", j->ex2) ||
        !st_source_path(j->source.mount, "/identity", identity)) return AZ_FAILED;
    for (unsigned i = 0; i < 12; i++) snprintf(key + i * 2, 3, "%02x", track[i]);
    snprintf(key + 24, sizeof key - 24, "-%016llx", (unsigned long long) j->signatures[1]);
    int fd = open(identity, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) { ssize_t n = read(fd, old, sizeof old - 1); if (n < 0) old[0] = 0; close(fd); }
    if (strcmp(key, old)) {
        if (read_only) return AZ_NOT_FOUND;
        if ((unlink(j->dat) && errno != ENOENT) || (unlink(j->ext) && errno != ENOENT) || (unlink(j->ex2) && errno != ENOENT)) return AZ_FAILED;
        struct sa_staged staged = {.dir = -1};
        int ok = sa_stage(j, identity, (const uint8_t *) key, strlen(key), &staged) && sa_publish(&staged);
        sa_stage_drop(&staged);
        if (!ok) return AZ_FAILED;
    }
    j->signatures[2] = st_disk_sig(j->dat); j->signatures[3] = st_disk_sig(j->ext);
    j->signatures[4] = st_disk_sig(j->ex2);
    return AZ_OK;
}
static const uint8_t *sa_find_tag(const uint8_t *data, size_t n, const char tag[4], uint32_t *length) {
    if (!data || !sa_framed(data, n)) return NULL;
    for (size_t at = sa_u32(data + 4); at < n; at += sa_u32(data + at + 8))
        if (!memcmp(data + at, tag, 4)) { *length = sa_u32(data + at + 8); return data + at; }
    return NULL;
}
static int sa_grid_same(const struct sa_job *j, const uint8_t *grid, uint32_t size) {
    if (!grid || size < 24 || sa_u32(grid + 4) != 24 || size != j->native_grid_bytes ||
        sa_u32(grid + 20) != sa_u32(j->native_grid + 20)) return 0;
    uint32_t rate = j->native_map.grid.sample_rate;
    if (!rate) return 0;
    for (uint32_t at = 24; at < size; at += 8) {
        if (sa_u16(grid + at) != sa_u16(j->native_grid + at) ||
            sa_u16(grid + at + 2) != sa_u16(j->native_grid + at + 2)) return 0;
        /* PQTZ rounds the exact native sample coordinate to whole ms. */
        int order = compare_timestamps(sa_u32(grid + at + 4), sa_u32(j->native_grid + at + 4),
                                       1000, rate, (rate + 999u) / 1000u + 1u);
        if (order == AZ_TIME_INVALID || order != AZ_TIME_EQUAL) return 0;
    }
    return 1;
}
/* Existing file phrases can be applied immediately, without another decoder.
 * Require the file's beat indexes and native grid snapshot to agree. */
static int sa_refresh_saved(struct sa_job *j) {
    size_t dn = 0, en = 0; int failed = 0;
    uint8_t *dat = sa_read(j->dat, &dn, &failed), *ext = sa_read(j->ext, &en, &failed);
    int grid_count = sa_tag_count(dat, dn, "PQTZ");
    int dat_phrases = sa_tag_count(dat, dn, "PSSI"), ext_phrases = sa_tag_count(ext, en, "PSSI");
    if (failed || grid_count < 0 || dat_phrases < 0 || ext_phrases < 0 || (dat_phrases && ext_phrases)) {
        free(dat); free(ext); return AZ_FAILED;
    }
    uint32_t gn = 0, pn = 0;
    const uint8_t *grid = sa_find_tag(dat, dn, "PQTZ", &gn);
    const uint8_t *phrase = ext_phrases ? sa_find_tag(ext, en, "PSSI", &pn) :
        dat_phrases ? sa_find_tag(dat, dn, "PSSI", &pn) : NULL;
    int rc = AZ_OK;
    if (failed) rc = AZ_FAILED;
    else if (phrase && sa_u16(phrase + 16)) {
        if (!sa_grid_same(j, grid, gn)) rc = AZ_STALE;
        else if (!sa_loaded_current(j)) rc = AZ_STALE;
        else if (j->needs_link && !sa_link_analysis(j)) rc = AZ_FAILED;
        else rc = j->maps->publish_phrases((unsigned) j->deck, j->load, &j->native_map, phrase, pn);
        if (!rc) rc = AZ_NOT_FOUND;
    }
    free(dat); free(ext);
    return rc;
}
/* Phrase and vocal presence are independent. Refreshing saved phrases retires
 * the old packet; capture the new edition before retaining a vocal-only lease. */
static int sa_loaded_needs(struct sa_job *j, uint32_t native_phrases) {
    j->needed = native_phrases ? 0 : AZ_DATA_PHRASES;
    if (!native_phrases) {
        int rc = sa_refresh_saved(j);
        if (rc == AZ_NOT_FOUND) {
            j->needed = 0;
            if (j->vocals) {
                j->maps->release(&j->native_map);
                memset(&j->native_map, 0, sizeof j->native_map);
                rc = j->maps->capture((unsigned) j->deck, j->load, &j->native_map);
                if (rc) { j->maps = NULL; return rc; }
            }
        } else if (rc) return rc;
    }
    if (j->vocals) {
        int failed = 0; size_t n = 0;
        uint8_t *file = sa_read(j->ex2, &n, &failed);
        int count = sa_tag_count(file, n, "PVDI"); free(file);
        if (failed || count < 0) return AZ_FAILED;
        if (!count) j->needed |= AZ_ANALYSIS_VOCALS;
    }
    return j->needed ? AZ_OK : AZ_NOT_FOUND;
}
static int sa_open_loaded(int owner, int deck, uint64_t load, az_analysis_job_v1 *out, int vocals) {
    if (owner < 0 || deck < 0 || deck >= 4 || !load || !out || st_record_size(out) < sizeof *out) return AZ_INVALID;
    const struct azstatus_native_v1 *api = sa_native();
    const struct azn_maps_api *maps = api && api->maps ? api->maps() : NULL;
    if (!maps || maps->size < offsetof(struct azn_maps_api, publish_phrases) + sizeof maps->publish_phrases ||
        !maps->capture || !maps->current || !maps->publish_phrases) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    if (!api->load_seed || !api->load_seed((unsigned) deck, &receipt) || receipt.load_generation != load) return AZ_STALE;
    struct sa_job j = {.owner = owner, .loaded = 1, .deck = deck, .load = load, .native_epoch = receipt.epoch, .maps = maps, .vocals = vocals};
    int rc = maps->capture((unsigned) deck, load, &j.native_map);
    if (rc) { j.maps = NULL; return rc; }
    uint32_t phrases = 0;
    rc = maps->read(&j.native_map.phrases, AZNM_PHRASES, NULL, 0, &phrases);
    if (rc) { sa_job_drop(&j); return rc; }
    if (phrases && !vocals) {
        az_analysis_job_v1 existing = {.size = out->size, .version = AZ_DATA_VERSION, .flags = AZ_ANALYSIS_LOADED};
        memcpy(out, &existing, sizeof existing); sa_job_drop(&j); return AZ_NOT_FOUND;
    }
    if (!phrases && (rc = sa_grid_snapshot(&j))) { sa_job_drop(&j); return rc; }
    az_analysis_job_v1 result = {.size = out->size, .version = AZ_DATA_VERSION, .needed = AZ_DATA_PHRASES, .flags = AZ_ANALYSIS_LOADED};
    if (receipt.original_bpm_state == AZN_BPM_READY) result.original_bpm = receipt.original_bpm;
    if (receipt.source_kind == AZ_STATUS_SOURCE_USB1 || receipt.source_kind == AZ_STATUS_SOURCE_USB2) {
        az_track_ref track = {0};
        rc = sa_loaded_ref((unsigned) deck, load, &track);
        if (!rc) rc = sa_open(owner, &track, &result, 1, vocals);
        if (rc) { sa_job_drop(&j); return rc; }
        if (pthread_mutex_lock(&sa_mutex)) { sa_job_drop(&j); return AZ_FAILED; }
        struct sa_job *found = NULL;
        for (unsigned i = 0; i < 8; i++) if (sa_jobs[i].id == result.job && sa_jobs[i].owner == owner) found = &sa_jobs[i];
        if (!found) { pthread_mutex_unlock(&sa_mutex); sa_job_drop(&j); return AZ_STALE; }
        struct sa_job saved = *found;
        saved.loaded = 1; saved.deck = deck; saved.load = load; saved.native_epoch = receipt.epoch;
        saved.maps = maps; saved.native_map = j.native_map;
        saved.native_grid = j.native_grid; saved.native_grid_bytes = j.native_grid_bytes;
        memcpy(saved.grid_snapshot, j.grid_snapshot, sizeof saved.grid_snapshot);
        *found = saved;
        rc = sa_loaded_current(found) ? sa_loaded_needs(found, phrases) : AZ_STALE;
        result.needed = found->needed;
        if (rc) sa_job_drop(found);
        else memcpy(result.analysis_path, found->grid_snapshot, sizeof result.analysis_path);
        pthread_mutex_unlock(&sa_mutex);
    } else {
        j.streaming = 1;
        j.audio_api = api->analysis_source ? api->analysis_source() : NULL;
        if (!j.audio_api || j.audio_api->size < sizeof *j.audio_api || !j.audio_api->open) { sa_job_drop(&j); return AZ_UNSUPPORTED; }
        struct azn_analysis_source source = {.size = sizeof source, .version = 1};
        rc = j.audio_api->open((unsigned) deck, receipt.epoch, load, &source);
        if (rc == AZ_NOT_FOUND) rc = AZ_BUSY; /* completion capture has not arrived yet */
        if (rc) { sa_job_drop(&j); return rc; }
        j.audio_token = source.token;
        memcpy(j.audio, source.audio_path, sizeof j.audio);
        j.signatures[1] = st_disk_sig(j.audio);
        if (!j.signatures[1]) { sa_job_drop(&j); return AZ_STALE; }
        if (pthread_mutex_trylock(&sa_mutex)) { sa_job_drop(&j); return AZ_BUSY; }
        unsigned slot = 8, owned = 0;
        for (unsigned i = 0; i < 8; i++) {
            if (sa_jobs[i].id && sa_jobs[i].loaded && sa_jobs[i].deck == deck) { rc = AZ_BUSY; break; }
            if (sa_jobs[i].id && sa_jobs[i].owner == owner) owned++;
            if (!sa_jobs[i].id && slot == 8) slot = i;
        }
        if (!rc && (slot == 8 || owned >= 4 || sa_serial == UINT64_MAX)) rc = AZ_LIMIT;
        if (!rc) { j.id = ++sa_serial; rc = sa_stream_paths(&j, source.track_id, 0); }
        if (!rc) rc = sa_loaded_needs(&j, phrases);
        result.needed = j.needed;
        if (!rc && !sa_current(&j)) rc = AZ_STALE;
        if (!rc) {
            sa_jobs[slot] = j; result.job = j.id; result.flags |= AZ_ANALYSIS_STREAMING;
            memcpy(result.audio_path, j.audio, sizeof result.audio_path);
            memcpy(result.analysis_path, j.grid_snapshot, sizeof result.analysis_path);
        } else sa_job_drop(&j);
        pthread_mutex_unlock(&sa_mutex);
    }
    if (!rc) { result.flags |= AZ_ANALYSIS_LOADED; memcpy(out, &result, sizeof result); }
    else if (rc == AZ_NOT_FOUND) { result.job = 0; memcpy(out, &result, sizeof result); }
    return rc;
}

static int sdk_analysis_open_loaded(const char *id, int deck, uint64_t load, az_analysis_job_v1 *out) {
    return sa_open_loaded(sa_owner(id, "analysis_open_loaded", CM_RA()), deck, load, out, 0);
}
static int sdk_analysis_open_loaded_v2(const char *id, int deck, uint64_t load, az_analysis_job_v1 *out) {
    return sa_open_loaded(sa_owner(id, "analysis_open_loaded_v2", CM_RA()), deck, load, out, 1);
}

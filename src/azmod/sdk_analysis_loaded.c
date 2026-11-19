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
static void sa_put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
struct sa_grid_capture {
    uint32_t count, rate;
    az_beat *rows;
    int64_t *ticks;
    uint8_t *file;
    size_t tag_size, file_size;
};
static void sa_grid_capture_drop(struct sa_grid_capture *capture) {
    free(capture->file);
    free(capture->rows);
    free(capture->ticks);
}
static int sa_grid_capture_allocate(struct sa_grid_capture *capture) {
    capture->tag_size = 24u + capture->count * 8u;
    capture->file_size = 28u + capture->tag_size;
    capture->rows = calloc(capture->count, sizeof *capture->rows);
    capture->ticks = calloc(capture->count, sizeof *capture->ticks);
    capture->file = calloc(1, capture->file_size);
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
    uint8_t *file = capture->file, *grid = file + 28;
    memcpy(file, "PMAI", 4);
    sa_put32(file + 4, 28);
    sa_put32(file + 8, (uint32_t) capture->file_size);
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
static int sa_snapshot_write(int fd, const uint8_t *data, size_t size) {
    size_t at = 0;
    while (at < size) {
        ssize_t wrote = write(fd, data + at, size - at);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return AZ_FAILED;
        at += (size_t) wrote;
    }
    return fchmod(fd, 0400) ? AZ_FAILED : AZ_OK;
}
static int sa_snapshot_file(struct sa_job *job, const struct sa_grid_capture *capture) {
    snprintf(job->grid_snapshot, sizeof job->grid_snapshot, "/tmp/az-analysis-grid-XXXXXX");
    int fd = mkostemp(job->grid_snapshot, O_CLOEXEC);
    if (fd < 0) {
        job->grid_snapshot[0] = 0;
        return AZ_FAILED;
    }
    int rc = sa_snapshot_write(fd, capture->file, capture->file_size);
    if (close(fd)) rc = AZ_FAILED;
    return rc;
}
static int sa_grid_retain(struct sa_job *job, const struct sa_grid_capture *capture) {
    job->native_grid = malloc(capture->tag_size);
    if (!job->native_grid) return AZ_FAILED;
    memcpy(job->native_grid, capture->file + 28, capture->tag_size);
    job->native_grid_bytes = capture->tag_size;
    return AZ_OK;
}
/* Preserve the native rows and clock in a read-only PQTZ snapshot for PSSI. */
static int sa_grid_snapshot(struct sa_job *j) {
    struct sa_grid_capture capture = {0};
    int rc = j->maps->read(&j->native_map.grid, AZNM_GRID, NULL, 0, &capture.count);
    if (rc) return rc;
    if (capture.count < 2) return AZ_BUSY;
    if (capture.count > 32768) return AZ_UNSUPPORTED;
    rc = sa_grid_capture_allocate(&capture);
    if (!rc) rc = sa_grid_capture_read(j, &capture);
    if (!rc) rc = sa_grid_encode(&capture);
    if (!rc) rc = sa_snapshot_file(j, &capture);
    if (!rc) rc = sa_grid_retain(j, &capture);
    sa_grid_capture_drop(&capture);
    return rc;
}
static int sa_stream_directory(struct sa_job *j, int read_only) {
    char base[1024];
    if (snprintf(base, sizeof base, "%s/analysis", CM_STATE) >= (int) sizeof base ||
        (!read_only && mkdir(base, 0700) && errno != EEXIST)) return AZ_FAILED;
    if (snprintf(j->source.mount, sizeof j->source.mount, "%s/beatport-%d", base, j->deck) >= (int) sizeof j->source.mount ||
        (!read_only && mkdir(j->source.mount, 0700) && errno != EEXIST)) return AZ_FAILED;
    return AZ_OK;
}
static int sa_stream_read_directory(const struct sa_job *j) {
    struct stat directory;
    if (lstat(j->source.mount, &directory)) return errno == ENOENT ? AZ_NOT_FOUND : AZ_FAILED;
    return S_ISDIR(directory.st_mode) ? AZ_OK : AZ_FAILED;
}
static int sa_stream_file_paths(struct sa_job *j, char identity[1024]) {
    return st_source_path(j->source.mount, "/analysis.DAT", j->dat) &&
           st_source_path(j->source.mount, "/analysis.EXT", j->ext) &&
           st_source_path(j->source.mount, "/analysis.2EX", j->ex2) &&
           st_source_path(j->source.mount, "/identity", identity);
}
static void sa_stream_key(const struct sa_job *j, const unsigned char track[12], char key[96]) {
    for (unsigned i = 0; i < 12; i++) snprintf(key + i * 2, 3, "%02x", track[i]);
    snprintf(key + 24, 72, "-%016llx", (unsigned long long) j->signatures[1]);
}
static void sa_stream_read_key(const char *identity, char old[96]) {
    int fd = open(identity, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    if (read(fd, old, 95) < 0) old[0] = 0;
    close(fd);
}
static int sa_stream_replace(struct sa_job *j, const char *identity, const char *key) {
    if ((unlink(j->dat) && errno != ENOENT) || (unlink(j->ext) && errno != ENOENT) || (unlink(j->ex2) && errno != ENOENT)) return AZ_FAILED;
    struct sa_staged staged = {.dir = -1};
    int ok = sa_stage(j, identity, (const uint8_t *) key, strlen(key), &staged) && sa_publish(&staged);
    sa_stage_drop(&staged);
    return ok ? AZ_OK : AZ_FAILED;
}
/* Four per-deck caches; a new audio fingerprint replaces only its own deck. */
static int sa_stream_paths(struct sa_job *j, const unsigned char track[12], int read_only) {
    int rc = sa_stream_directory(j, read_only);
    if (rc) return rc;
    if (read_only && (rc = sa_stream_read_directory(j))) return rc;
    char key[96], old[96] = {0}, identity[1024];
    if (!sa_stream_file_paths(j, identity)) return AZ_FAILED;
    sa_stream_key(j, track, key);
    sa_stream_read_key(identity, old);
    if (strcmp(key, old)) {
        if (read_only) return AZ_NOT_FOUND;
        rc = sa_stream_replace(j, identity, key);
        if (rc) return rc;
    }
    j->signatures[2] = st_disk_sig(j->dat);
    j->signatures[3] = st_disk_sig(j->ext);
    j->signatures[4] = st_disk_sig(j->ex2);
    return AZ_OK;
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
static int sa_saved_phrases_publish(struct sa_job *j, const uint8_t *grid, uint32_t grid_size,
                                    const uint8_t *phrase, uint32_t phrase_size) {
    if (!phrase || !sa_u16(phrase + 16)) return AZ_OK;
    if (!sa_grid_same(j, grid, grid_size) || !sa_loaded_current(j)) return AZ_STALE;
    if (j->needs_link && !sa_link_analysis(j)) return AZ_FAILED;
    int rc = j->maps->publish_phrases((unsigned) j->deck, j->load, &j->native_map, phrase, phrase_size);
    return rc ? rc : AZ_NOT_FOUND;
}
static int sa_saved_phrases(struct sa_job *j, const uint8_t *dat, size_t dat_size, const uint8_t *ext, size_t ext_size) {
    int grid_count = sa_tag_count(dat, dat_size, "PQTZ");
    int dat_phrases = sa_tag_count(dat, dat_size, "PSSI"), ext_phrases = sa_tag_count(ext, ext_size, "PSSI");
    if (grid_count < 0 || dat_phrases < 0 || ext_phrases < 0 || (dat_phrases && ext_phrases)) return AZ_FAILED;
    uint32_t grid_size = 0, phrase_size = 0;
    const uint8_t *grid = sa_find_tag(dat, dat_size, "PQTZ", &grid_size);
    const uint8_t *phrase = ext_phrases ? sa_find_tag(ext, ext_size, "PSSI", &phrase_size) : dat_phrases ? sa_find_tag(dat, dat_size, "PSSI", &phrase_size)
                                                                                                         : NULL;
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
    j->needed = native_phrases ? 0 : AZ_DATA_PHRASES;
    if (native_phrases) return AZ_OK;
    int rc = sa_refresh_saved(j);
    if (rc != AZ_NOT_FOUND) return rc;
    j->needed = 0;
    return j->vocals ? sa_recapture_loaded(j) : AZ_OK;
}
static int sa_loaded_vocal_need(struct sa_job *j) {
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
    int rc = sa_loaded_phrase_need(j, native_phrases);
    if (rc) return rc;
    rc = sa_loaded_vocal_need(j);
    return rc ? rc : j->needed ? AZ_OK
                               : AZ_NOT_FOUND;
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
    memcpy(saved.grid_snapshot, native->grid_snapshot, sizeof saved.grid_snapshot);
    *found = saved;
}
static struct sa_job *sa_find_owned_job(uint64_t id, int owner) {
    struct sa_job *found = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == id && sa_jobs[i].owner == owner) found = &sa_jobs[i];
    return found;
}
static int sa_usb_register(struct sa_job *j, uint32_t phrases, az_analysis_job_v1 *result) {
    if (pthread_mutex_lock(&sa_mutex)) {
        sa_job_drop(j);
        return AZ_FAILED;
    }
    struct sa_job *found = sa_find_owned_job(result->job, j->owner);
    if (!found) {
        pthread_mutex_unlock(&sa_mutex);
        sa_job_drop(j);
        return AZ_STALE;
    }
    sa_usb_attach(found, j);
    int rc = sa_loaded_current(found) ? sa_loaded_needs(found, phrases) : AZ_STALE;
    result->needed = found->needed;
    if (rc) sa_job_drop(found);
    else memcpy(result->analysis_path, found->grid_snapshot, sizeof result->analysis_path);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_usb_open(struct sa_job *j, uint32_t phrases, az_analysis_job_v1 *result) {
    az_track_ref track = {0};
    int rc = sa_loaded_ref((unsigned) j->deck, j->load, &track);
    if (!rc) rc = sa_open(j->owner, &track, result, 1, j->vocals);
    if (rc) {
        sa_job_drop(j);
        return rc;
    }
    return sa_usb_register(j, phrases, result);
}
static int sa_stream_capture(struct sa_job *j, const struct azstatus_native_v1 *api, struct azn_analysis_source *source) {
    j->streaming = 1;
    j->audio_api = api->analysis_source ? api->analysis_source() : NULL;
    if (!j->audio_api || j->audio_api->size < sizeof *j->audio_api || !j->audio_api->open) return AZ_UNSUPPORTED;
    int rc = j->audio_api->open((unsigned) j->deck, j->native_epoch, j->load, source);
    if (rc == AZ_NOT_FOUND) rc = AZ_BUSY; /* Completion capture has not arrived yet. */
    if (rc) return rc;
    j->audio_token = source->token;
    memcpy(j->audio, source->audio_path, sizeof j->audio);
    j->signatures[1] = st_disk_sig(j->audio);
    return j->signatures[1] ? AZ_OK : AZ_STALE;
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
    return sa_stream_capacity(*slot, owned) ? AZ_OK : AZ_LIMIT;
}
static int sa_stream_prepare(struct sa_job *j, uint32_t phrases, const unsigned char track[12], unsigned *slot) {
    int rc = sa_stream_slot(j, slot);
    if (rc) return rc;
    j->id = ++sa_serial;
    rc = sa_stream_paths(j, track, 0);
    return rc ? rc : sa_loaded_needs(j, phrases);
}
static void sa_stream_retain(const struct sa_job *j, unsigned slot, az_analysis_job_v1 *result) {
    sa_jobs[slot] = *j;
    result->job = j->id;
    result->flags |= AZ_ANALYSIS_STREAMING;
    memcpy(result->audio_path, j->audio, sizeof result->audio_path);
    memcpy(result->analysis_path, j->grid_snapshot, sizeof result->analysis_path);
}
static int sa_stream_register(struct sa_job *j, uint32_t phrases, const unsigned char track[12], az_analysis_job_v1 *result) {
    if (pthread_mutex_trylock(&sa_mutex)) {
        sa_job_drop(j);
        return AZ_BUSY;
    }
    unsigned slot = 8;
    int rc = sa_stream_prepare(j, phrases, track, &slot);
    result->needed = j->needed;
    if (!rc && !sa_current(j)) rc = AZ_STALE;
    if (!rc) sa_stream_retain(j, slot, result);
    else sa_job_drop(j);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_stream_open(struct sa_job *j, const struct azstatus_native_v1 *api, uint32_t phrases, az_analysis_job_v1 *result) {
    struct azn_analysis_source source = {.size = sizeof source, .version = 1};
    int rc = sa_stream_capture(j, api, &source);
    if (rc) {
        sa_job_drop(j);
        return rc;
    }
    return sa_stream_register(j, phrases, source.track_id, result);
}
static int sa_loaded_seed(const struct azstatus_native_v1 *api, int deck, uint64_t load, struct azstatus_native_receipt *receipt) {
    return api->load_seed && api->load_seed((unsigned) deck, receipt) && receipt->load_generation == load ? AZ_OK : AZ_STALE;
}
static void sa_loaded_result(az_analysis_job_v1 *result, az_analysis_job_v1 *out, int rc) {
    if (!rc) {
        result->flags |= AZ_ANALYSIS_LOADED;
        memcpy(out, result, sizeof *result);
    } else if (rc == AZ_NOT_FOUND) {
        result->job = 0;
        memcpy(out, result, sizeof *result);
    }
}
static int sa_loaded_open_source(struct sa_job *j, const struct azstatus_native_v1 *api,
                                 const struct azstatus_native_receipt *receipt, uint32_t phrases, az_analysis_job_v1 *out) {
    az_analysis_job_v1 result = {.size = out->size, .version = AZ_DATA_VERSION, .needed = AZ_DATA_PHRASES, .flags = AZ_ANALYSIS_LOADED};
    if (receipt->original_bpm_state == AZN_BPM_READY) result.original_bpm = receipt->original_bpm;
    int rc;
    if (receipt->source_kind == AZ_STATUS_SOURCE_USB1 || receipt->source_kind == AZ_STATUS_SOURCE_USB2)
        rc = sa_usb_open(j, phrases, &result);
    else rc = sa_stream_open(j, api, phrases, &result);
    sa_loaded_result(&result, out, rc);
    return rc;
}
static int sa_loaded_lease(struct sa_job *j, const struct azstatus_native_v1 *api,
                           const struct azstatus_native_receipt *receipt, az_analysis_job_v1 *out) {
    uint32_t phrases = 0;
    int rc = sa_capture_loaded(j, &phrases);
    if (rc) {
        sa_job_drop(j);
        return rc;
    }
    if (phrases && !j->vocals) {
        az_analysis_job_v1 existing = {.size = out->size, .version = AZ_DATA_VERSION, .flags = AZ_ANALYSIS_LOADED};
        memcpy(out, &existing, sizeof existing);
        sa_job_drop(j);
        return AZ_NOT_FOUND;
    }
    if (!phrases && (rc = sa_grid_snapshot(j))) {
        sa_job_drop(j);
        return rc;
    }
    return sa_loaded_open_source(j, api, receipt, phrases, out);
}
static int sa_open_loaded(int owner, int deck, uint64_t load, az_analysis_job_v1 *out, int vocals) {
    if (owner < 0 || deck < 0 || deck >= 4 || !load || !out || st_record_size(out) < sizeof *out) return AZ_INVALID;
    const struct azstatus_native_v1 *api = sa_native();
    const struct azn_maps_api *maps = sa_loaded_maps(api);
    if (!maps) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    int rc = sa_loaded_seed(api, deck, load, &receipt);
    if (rc) return rc;
    struct sa_job job = {.owner = owner, .loaded = 1, .deck = deck, .load = load, .native_epoch = receipt.epoch, .maps = maps, .vocals = vocals};
    return sa_loaded_lease(&job, api, &receipt, out);
}

static int sdk_analysis_open_loaded(const char *id, int deck, uint64_t load, az_analysis_job_v1 *out) {
    return sa_open_loaded(sa_owner(id, "analysis_open_loaded", CM_RA()), deck, load, out, 0);
}
static int sdk_analysis_open_loaded_v2(const char *id, int deck, uint64_t load, az_analysis_job_v1 *out) {
    return sa_open_loaded(sa_owner(id, "analysis_open_loaded_v2", CM_RA()), deck, load, out, 1);
}

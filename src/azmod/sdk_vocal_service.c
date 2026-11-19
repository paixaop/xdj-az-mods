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
    g_sdk_sq.close(db);
    return rc;
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
    if (!sdk_sq_resolve()) return AZ_UNSUPPORTED;
    int rc = sv_source(track, j);
    if (rc) return rc;
    struct st_resolution row = {0};
    rc = sv_query(track, j, &row);
    if (rc) return rc;
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
static int sdk_vocal_intervals(const char *id, const az_track_ref *track, az_vocal_snapshot_v1 *out,
                               az_vocal_interval *intervals, uint32_t capacity) {
    if (sa_owner(id, "vocal_intervals", CM_RA()) < 0 || !out || st_record_size(out) < sizeof *out ||
        (!intervals && capacity)) return AZ_INVALID;
    struct sa_job j = {0};
    int rc = sv_location(track, &j);
    return rc ? rc : sv_read(&j, out, intervals, capacity);
}
static int sv_output_valid(const az_vocal_snapshot_v1 *out, const az_vocal_interval *intervals, uint32_t capacity) {
    return out && st_record_size(out) >= sizeof *out && (intervals || !capacity);
}
static const struct azn_maps_api *sv_loaded_maps(const struct azstatus_native_v1 *api) {
    const struct azn_maps_api *maps = api && api->maps ? api->maps() : NULL;
    return maps && maps->capture && maps->current && maps->release && api->load_seed ? maps : NULL;
}
static int sv_stream_open(struct sa_job *j, const struct azstatus_native_v1 *api) {
    j->streaming = 1;
    j->audio_api = api->analysis_source ? api->analysis_source() : NULL;
    if (!j->audio_api || j->audio_api->size < sizeof *j->audio_api || !j->audio_api->open ||
        !j->audio_api->current || !j->audio_api->close) return AZ_UNSUPPORTED;
    struct azn_analysis_source source = {.size = sizeof source, .version = 1};
    int rc = j->audio_api->open((unsigned) j->deck, j->native_epoch, j->load, &source);
    if (rc) return rc;
    j->audio_token = source.token;
    memcpy(j->audio, source.audio_path, sizeof j->audio);
    j->signatures[1] = st_disk_sig(j->audio);
    /* Reads never evict another load's cache or create settings files. */
    rc = sa_stream_paths(j, source.track_id, 1);
    if (rc) sa_job_drop(j);
    return rc;
}
static int sv_loaded_location(struct sa_job *j, const struct azstatus_native_v1 *api, unsigned source_kind) {
    if (source_kind != AZ_STATUS_SOURCE_USB1 && source_kind != AZ_STATUS_SOURCE_USB2) return sv_stream_open(j, api);
    az_track_ref ref = {0};
    int rc = sa_loaded_ref((unsigned) j->deck, j->load, &ref);
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
static int sdk_vocal_intervals_loaded(const char *id, int deck, uint64_t load, az_vocal_snapshot_v1 *out,
                                      az_vocal_interval *intervals, uint32_t capacity) {
    if (sa_owner(id, "vocal_intervals_loaded", CM_RA()) < 0 || deck < 0 || deck >= 4 || !load ||
        !sv_output_valid(out, intervals, capacity)) return AZ_INVALID;
    const struct azstatus_native_v1 *api = sa_native();
    const struct azn_maps_api *maps = sv_loaded_maps(api);
    if (!maps) return AZ_UNSUPPORTED;
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    if (sa_loaded_seed(api, deck, load, &receipt)) return AZ_STALE;
    struct sa_job j = {.deck = deck, .load = load, .native_epoch = receipt.epoch, .vocals = 1};
    int rc = sv_loaded_location(&j, api, receipt.source_kind);
    return rc ? rc : sv_loaded_read(&j, maps, out, intervals, capacity);
}

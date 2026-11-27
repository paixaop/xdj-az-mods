/* SPDX-License-Identifier: GPL-3.0-only */
/* One owner for stored-data tickets, immutable snapshots, disk cache and worker. */
#define _GNU_SOURCE
#include "track_service.h"
#include "track_storage.h"
#include "library_sql.h"
#include "sdk_data_engine.h"
#include "../common/azsources.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "sdk_wave_provider.h"
static struct st_engine g_st = ST_ENGINE_INIT;
static pthread_mutex_t g_st_start_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_st_started, g_st_enabled;
static struct st_resolution g_st_resolutions[2];
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
    return st_source_database(source, database) &&
           !strcmp(database, data->files[0]) && st_database_quiet(data->files[0]) &&
           data->signatures[0] && st_disk_sig(data->files[0]) == data->signatures[0];
}
static int st_analysis_files_fresh(const struct st_source *source, const struct st_data *data) {
    for (int i = 1; i < 3; i++) {
        if (data->analysis_paths[i - 1][0]) {
            char current[1024] = "";
            st_source_file(source, data->analysis_paths[i - 1], current);
            if (strcmp(current, data->files[i])) return 0;
        }
        if (data->files[i][0] && st_disk_sig(data->files[i]) != data->signatures[i]) return 0;
    }
    return 1;
}
int track_service_disk_fresh(const az_track_ref *ref, const struct st_data *data, void *ctx) {
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
/* Recheck after SQL and before publishing a positive locator. A stat signature
 * has the same freshness-hint limits as stored snapshots; it is not a DB hash. */
static int st_resolution_current(const struct st_source *source, const char *database, uint64_t signature) {
    struct st_source current;
    char path[1024];
    if (!signature || st_source_read((int) source->ref.source_id - 1, &current)) return 0;
    current.ref.track_id = source->ref.track_id;
    return st_resolution_source_equal(source, &current) &&
           st_source_database(&current, path) &&
           !strcmp(database, path) && st_database_quiet(path) && st_disk_sig(path) == signature;
}
static int st_disk_source(const az_track_ref *ref, struct st_data *data, struct st_source *source,
                          struct st_resolution *cached) {
    struct st_source src;
    if (!st_ref_source_current(ref, &src)) {
        cached->valid = 0;
        return AZ_STALE;
    }
    /* SQLite and sidecars must use the same canonical physical database path. */
    if (!st_source_database(&src, data->files[0])) return AZ_STALE;
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
    az_storage_sql()->close(db);
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
    if (row->content[0] && st_source_file(source, row->content, real) && strlen(real) < sizeof data->view.metadata.path) {
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
    if (!st_source_file(source, relative, data->files[file_slot])) return AZ_UNSUPPORTED;
    data->signatures[file_slot] = st_disk_sig(data->files[file_slot]);
    if (!data->signatures[file_slot]) return AZ_UNSUPPORTED;
    int rc = st_wave_read(data->files[file_slot], data);
    return rc ? rc : track_service_disk_fresh(ref, data, NULL) ? AZ_OK
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
    if (!st_source_file(source, relative, data->files[file + 1])) return AZ_OK;
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
int track_service_disk_load(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
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
    return track_service_disk_fresh(ref, data, NULL) ? AZ_OK : AZ_STALE;
}
/* Only the core worker runs the optional status jobs. Starting or changing
 * interest wakes it independently of the explicit-ticket freshness deadline. */
static _Atomic(void (*)(void)) g_st_status_jobs;
static pthread_mutex_t g_st_wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_st_wake_cv = PTHREAD_COND_INITIALIZER;
static int g_st_wake_pending;
void track_service_wake(void) {
    pthread_mutex_lock(&g_st_wake_mu);
    g_st_wake_pending = 1;
    pthread_cond_signal(&g_st_wake_cv);
    pthread_mutex_unlock(&g_st_wake_mu);
}
static void *st_worker(void *arg) {
    (void) arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "az-track-data");
#else
    pthread_setname_np("az-track-data");
#endif
    for (;;) {
        track_service_work();
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
int track_service_start_worker(void) {
    pthread_mutex_lock(&g_st_start_mu);
    if (!g_st_started) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        struct sched_param sp = {0};
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        g_st_started = az_storage_thread_create(&thread, &attr, st_worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    int rc = g_st_started ? AZ_OK : AZ_FAILED;
    pthread_mutex_unlock(&g_st_start_mu);
    return rc;
}

void track_service_work(void) {
    st_work(&g_st, track_service_disk_load, track_service_disk_fresh, NULL);
}
void track_service_configure(int enabled) { atomic_store(&g_st_enabled, !!enabled); }
int track_service_enabled(void) { return atomic_load(&g_st_enabled); }
void track_service_owner_init(int owner) { st_reopen_owner(&g_st, owner); }
void track_service_owner_retire(int owner) { st_retire_owner(&g_st, owner); }
void track_service_status_register(void (*jobs)(void)) { atomic_store(&g_st_status_jobs, jobs); }
int track_service_status_started(void) { return atomic_load(&g_st_status_jobs) != NULL; }
int track_service_capabilities(az_data_capabilities *out) {
    if (!out || st_record_size(out) < sizeof *out) return AZ_INVALID;
    az_data_capabilities value = {.size = out->size, .version = AZ_DATA_VERSION, .stored_components = track_service_enabled() && az_storage_sql()->ok > 0 ? 31u : 0u, .max_requests_per_mod = ST_PER_OWNER, .max_snapshots = ST_SLOTS, .max_wave_samples = ST_WAVE};
    *out = value;
    return AZ_OK;
}
static int track_service_admit(void) {
    if (!track_service_enabled() || az_storage_sql()->ok <= 0) return AZ_UNSUPPORTED;
    return atomic_load_explicit(&g_st_started, memory_order_acquire) ? AZ_OK : AZ_STALE;
}
int track_service_resolve(int stick, uint32_t cid, az_track_ref *out) {
    if (!out || st_record_size(out) < sizeof *out || !cid) return AZ_INVALID;
    if (!track_service_enabled() || az_storage_sql()->ok <= 0) return AZ_UNSUPPORTED;
    struct st_source source;
    int rc = st_source_read(stick, &source);
    if (rc) return rc;
    if (track_service_start_worker()) return AZ_FAILED;
    uint32_t size = out->size;
    *out = source.ref;
    out->size = size;
    out->track_id = cid;
    return AZ_OK;
}
int track_service_request(int owner, const az_track_ref *ref, uint32_t mask, uint64_t *ticket) {
    int rc = track_service_admit();
    return rc ? rc : st_request(&g_st, owner, ref, mask, ticket);
}
int track_service_acquire(int owner, uint64_t ticket, const az_track_snapshot **out) {
    return st_acquire(&g_st, owner, ticket, out);
}
int track_service_wave_request(int owner, const az_track_ref *ref, const az_wave_query *query, uint64_t *ticket) {
    if (!query) return AZ_INVALID;
    int rc = track_service_admit();
    return rc ? rc : st_request_with_wave(&g_st, owner, ref, ST_DETAIL, query, ticket);
}
int track_service_wave_acquire(int owner, uint64_t ticket, const az_wave_snapshot **out) {
    return st_wave_acquire(&g_st, owner, ticket, out);
}
int track_service_release(int owner, uint64_t ticket, int cancel) {
    return st_release(&g_st, owner, ticket, cancel);
}
int track_service_event(int owner, uint64_t after, az_data_change *out) {
    return st_event_read(&g_st, owner, after, out);
}

int track_service_owner_retired(int owner) { return st_owner_retired(&g_st, owner); }
int track_service_event_head(uint64_t *sequence) {
    if (!sequence) return AZ_INVALID;
    if (pthread_mutex_trylock(&g_st.mu)) return AZ_BUSY;
    int rc = g_st.events_exhausted ? AZ_LIMIT : AZ_OK;
    if (!rc) *sequence = g_st.sequence;
    pthread_mutex_unlock(&g_st.mu);
    return rc;
}

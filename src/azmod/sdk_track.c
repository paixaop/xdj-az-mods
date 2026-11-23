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
static pthread_mutex_t g_st_start_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_st_started;
static int g_st_enabled;
#include "track_storage.h"
#ifndef ST_MOUNTINFO
#define ST_MOUNTINFO "/proc/self/mountinfo"
#endif
const char *az_storage_mountinfo(void) { return ST_MOUNTINFO; }
static struct st_resolution g_st_resolutions[2];
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out);
static uint32_t sd_live_fields(void);
static uint64_t sdk_command_capabilities(void);
static void sdk_command_retire_owner(int owner);
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
        g_st_started = az_thread_create(&thread, &attr, st_worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    int rc = g_st_started ? AZ_OK : AZ_FAILED;
    pthread_mutex_unlock(&g_st_start_mu);
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

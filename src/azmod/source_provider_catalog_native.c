/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by azmod.c after the other owning-library worker operations. */
#include "source_provider_carrier.h"
#include "source_provider_catalog.h"
#include "source_provider_lifetime.h"
#include "source_import_analyzer.h"

enum catalog_job_phase {
    CATALOG_QUEUED,
    CATALOG_OWNED,
    CATALOG_DONE
};

struct catalog_job {
    enum catalog_job_phase phase;
    int status;
    uint32_t lookup;
    char *owner;
    az_source_track_v1 *track;
    const char *provider;
    const struct azsp_request *request;
    const struct azsp_result *result;
    uint32_t *ids;
    enum azsp_catalog_stream_operation stream_operation;
    const struct azsp_catalog_stream_request *stream;
};

static pthread_mutex_t catalog_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t catalog_job_changed = PTHREAD_COND_INITIALIZER;
static _Atomic(struct catalog_job *) catalog_job_pending;
static _Atomic int catalog_wake;

int azsp_catalog_take_wake(void) {
    return atomic_exchange_explicit(&catalog_wake, 0, memory_order_acq_rel);
}

static int catalog_wait_for_claim(struct catalog_job *job) {
    struct timespec timeout;
    if (clock_gettime(CLOCK_REALTIME, &timeout))
        return AZ_FAILED;
    timeout.tv_sec += 4;
    while (job->phase == CATALOG_QUEUED) {
        int rc = pthread_cond_timedwait(&catalog_job_changed, &catalog_job_lock, &timeout);
        if (rc && job->phase == CATALOG_QUEUED)
            return AZ_BUSY;
    }
    return AZ_OK;
}

static int catalog_submit_locked(struct catalog_job *job) {
    if (atomic_load_explicit(&catalog_job_pending, memory_order_acquire))
        return AZ_BUSY;
    atomic_store_explicit(&catalog_job_pending, job, memory_order_release);
    atomic_store_explicit(&catalog_wake, 1, memory_order_release);
    atomic_store_explicit(&g_mv_kick, 1, memory_order_release);
    int status = catalog_wait_for_claim(job);
    if (status == AZ_OK) {
        while (job->phase != CATALOG_DONE)
            pthread_cond_wait(&catalog_job_changed, &catalog_job_lock);
        status = job->status;
    }
    atomic_store_explicit(&catalog_job_pending, NULL, memory_order_release);
    return status;
}

static int catalog_submit(struct catalog_job *job) {
    if (!azsp_carrier_media() || !__atomic_load_n(&g_orig_mlserver_onmsg, __ATOMIC_ACQUIRE))
        return AZ_UNSUPPORTED;
    pthread_mutex_lock(&catalog_job_lock);
    int status = catalog_submit_locked(job);
    pthread_mutex_unlock(&catalog_job_lock);
    return status;
}

int azsp_catalog_apply(const char *provider, const struct azsp_request *request,
                       const struct azsp_result *result, uint32_t ids[AZ_SOURCE_PAGE_MAX]) {
    struct catalog_job job = {.provider = provider, .request = request, .result = result, .ids = ids};
    return catalog_submit(&job);
}

int azsp_catalog_track(uint32_t id, char provider[33], az_source_track_v1 *track) {
    if (!id)
        return AZ_INVALID;
    struct catalog_job job = {.lookup = id, .owner = provider, .track = track};
    return catalog_submit(&job);
}

int azsp_catalog_stream(enum azsp_catalog_stream_operation operation, const struct azsp_catalog_stream_request *request) {
    struct catalog_job job = {.stream_operation = operation, .stream = request};
    return catalog_submit(&job);
}

static struct catalog_job *catalog_claim(void) {
    /* The submitter drops this lock when it waits. Claiming under the same
     * lock prevents an early wake from missing a newly submitted request. */
    pthread_mutex_lock(&catalog_job_lock);
    struct catalog_job *job = atomic_load_explicit(&catalog_job_pending, memory_order_acquire);
    if (job && job->phase == CATALOG_QUEUED) {
        job->phase = CATALOG_OWNED;
        pthread_cond_signal(&catalog_job_changed);
    } else
        job = NULL;
    pthread_mutex_unlock(&catalog_job_lock);
    return job;
}

static int catalog_analysis_watch(const uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES],
                                  const uint8_t requester[AZSPI_NATIVE_REQUESTER_BYTES], void *output) {
    return azspd_analysis_watch(raw_id, requester, output);
}

static int catalog_stream_analyze(const struct import_server *server, const struct azspi_sql *sql,
                                  const struct azsp_catalog_stream_request *request) {
    if (!request->analysis_ticket)
        return AZ_INVALID;
    int status = azsp_catalog_stream_idle(sql, server->connection, request->id);
    if (status != AZ_OK)
        return status;
    uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES];
    status = azspi_analyzer_submit_watched(g_ep147_build, server->database, (uint16_t) server->media,
                                           request->source, request->audio, raw_id,
                                           catalog_analysis_watch, request->analysis_ticket);
    return status == AZ_PENDING ? AZ_OK : status;
}

static int catalog_stream_update(const struct import_server *server, const struct azspi_sql *sql,
                                 const struct catalog_job *job) {
    const struct azsp_catalog_stream_request *request = job->stream;
    if (job->stream_operation == AZSP_CATALOG_STREAM_BIND)
        return azsp_catalog_stream_bind(sql, server->connection, request->id, request->record);
    if (job->stream_operation == AZSP_CATALOG_STREAM_CLEAR)
        return azsp_catalog_stream_clear(sql, server->connection, request->id, request->audio, request->dat);
    return AZ_INVALID;
}

static int catalog_execute_stream(const struct import_server *server, const struct azspi_sql *sql,
                                  const struct catalog_job *job) {
    const struct azsp_catalog_stream_request *request = job->stream;
    if (request->media != azsp_carrier_media() || !playlist_database_current(server->connection, request->source))
        return AZ_STALE;
    if (sql->autocommit(server->connection) != 1)
        return AZ_BUSY;
    if (job->stream_operation == AZSP_CATALOG_STREAM_ANALYZE)
        return catalog_stream_analyze(server, sql, request);
    int status = catalog_stream_update(server, sql, job);
    if (status == AZ_OK)
        lib_invalidate(server->cache, server->connection, NULL);
    return status;
}

static int catalog_execute(const struct import_server *server, struct catalog_job *job) {
    struct azspi_sql sql;
    if (!source_database_sql(&sql))
        return AZ_UNSUPPORTED;
    if (job->stream)
        return catalog_execute_stream(server, &sql, job);
    if (job->lookup)
        return azsp_catalog_lookup(&sql, server->connection, job->lookup, job->owner, job->track);
    if (sql.autocommit(server->connection) != 1)
        return AZ_BUSY;
    int status = azsp_catalog_publish(&sql, server->connection, job->provider, job->request, job->result, job->ids);
    if (status == AZ_OK)
        lib_invalidate(server->cache, server->connection, NULL);
    return status;
}

static void catalog_finished(struct catalog_job *job, int status) {
    pthread_mutex_lock(&catalog_job_lock);
    job->status = status;
    job->phase = CATALOG_DONE;
    pthread_cond_broadcast(&catalog_job_changed);
    pthread_mutex_unlock(&catalog_job_lock);
}

static void source_catalog_native(void *server) {
    if (!atomic_load_explicit(&catalog_job_pending, memory_order_acquire))
        return;
    uint16_t media = azsp_carrier_library_media(server);
    if (!media)
        return;
    struct import_server owner;
    if (!import_server(server, &owner) || owner.media != media)
        return;
    struct catalog_job *job = catalog_claim();
    if (job)
        catalog_finished(job, catalog_execute(&owner, job));
}

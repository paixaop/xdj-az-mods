/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after sdk_playlist_move.c, on the same native library boundary. */
#include "source_import.h"
#include "source_import_analyzer.h"
#include "source_provider_lifetime.h"

enum import_phase {
    IMPORT_FREE,
    IMPORT_QUEUED,
    IMPORT_RUNNING,
    IMPORT_DONE
};
enum import_operation {
    IMPORT_WRITE,
    IMPORT_ANALYZE,
    IMPORT_DESTINATION
};

struct import_job {
    uint64_t ticket;
    long long deadline;
    unsigned media;
    enum import_phase phase;
    enum import_operation operation;
    struct azspi_record record;
    uint32_t track_id;
    unsigned destination;
    uint64_t analysis_ticket;
    int status;
};

static pthread_mutex_t import_lock = PTHREAD_MUTEX_INITIALIZER;
static struct import_job import_jobs[4]; /* one request per physical deck */
static unsigned import_pending;
static uint64_t import_sequence;

static struct import_job *import_find(uint64_t ticket) {
    if (!ticket)
        return NULL;
    for (size_t i = 0; i < sizeof(import_jobs) / sizeof(import_jobs[0]); ++i) {
        if (import_jobs[i].ticket == ticket)
            return &import_jobs[i];
    }
    return NULL;
}

static struct import_job *import_free_slot(void) {
    for (size_t i = 0; i < sizeof(import_jobs) / sizeof(import_jobs[0]); ++i) {
        if (import_jobs[i].phase == IMPORT_FREE)
            return &import_jobs[i];
    }
    return NULL;
}

static int import_enqueue(const struct azspi_record *record, unsigned media,
                          enum import_operation operation, uint64_t *ticket) {
    struct import_job *job = import_free_slot();
    if (!job)
        return AZ_BUSY;
    if (import_sequence == UINT64_MAX)
        return AZ_LIMIT;
    *job = (struct import_job) {.ticket = ++import_sequence, .deadline = drag_now_ms() + MV_DEADLINE_MS, .media = media, .phase = IMPORT_QUEUED, .operation = operation, .record = *record};
    *ticket = job->ticket;
    __atomic_add_fetch(&import_pending, 1, __ATOMIC_RELEASE);
    atomic_store_explicit(&g_mv_kick, 1, memory_order_release);
    return AZ_PENDING;
}

static unsigned import_storage_byte(const struct st_source *source) {
    uint64_t port = source->ref.source_id;
    if (!port || port > 2 || !source->mount_id)
        return 0;
    return (unsigned) (port - 1) * 32u + 2u;
}

static int import_media_valid(const struct st_source *source, unsigned media, enum import_operation operation) {
    unsigned storage = import_storage_byte(source);
    if (!storage || (media & 0xff) != storage)
        return 0;
    return operation == IMPORT_DESTINATION || (media > 0xff && media <= 0xffff);
}

static int import_submit(const struct azspi_record *record, unsigned media,
                         enum import_operation operation, uint64_t *ticket) {
    if (!ticket || !import_media_valid(&record->source, media, operation))
        return AZ_INVALID;
    if (g_ep147_build != EPB_V200 || !__atomic_load_n(&g_orig_mlserver_onmsg, __ATOMIC_ACQUIRE))
        return AZ_UNSUPPORTED;
    pthread_mutex_lock(&import_lock);
    int status = import_enqueue(record, media, operation, ticket);
    pthread_mutex_unlock(&import_lock);
    return status;
}

int azspi_submit(const struct azspi_record *record, unsigned media, uint64_t *ticket) {
    return import_submit(record, media, IMPORT_WRITE, ticket);
}

int azspi_submit_destination(const struct st_source *source, uint64_t *ticket) {
    struct azspi_record record = {.source = *source};
    return import_submit(&record, import_storage_byte(source), IMPORT_DESTINATION, ticket);
}

int azspi_submit_analysis(const struct st_source *source, unsigned media, const char *audio,
                          uint64_t *ticket) {
    char resolved[1024];
    if (!st_source_path(source->mount, audio, resolved))
        return AZ_INVALID;
    uint64_t signature = st_disk_sig(resolved);
    if (!signature)
        return AZ_STALE;
    struct azspi_record record = {.source = *source, .audio.signature = signature};
    if ((unsigned) snprintf(record.audio.relative, sizeof(record.audio.relative), "%s", audio) >= sizeof(record.audio.relative))
        return AZ_LIMIT;
    return import_submit(&record, media, IMPORT_ANALYZE, ticket);
}

static void import_reject(struct import_job *job) {
    __atomic_sub_fetch(&import_pending, 1, __ATOMIC_RELEASE);
    job->status = AZ_CANCELLED;
    job->phase = IMPORT_DONE;
}

static void import_expire(struct import_job *job, long long now) {
    if (job->phase == IMPORT_QUEUED && now >= job->deadline)
        import_reject(job);
}

static void import_copy_result(const struct import_job *job, void *out) {
    switch (job->operation) {
    case IMPORT_WRITE: memcpy(out, &job->track_id, sizeof(job->track_id)); break;
    case IMPORT_ANALYZE: memcpy(out, &job->analysis_ticket, sizeof(job->analysis_ticket)); break;
    case IMPORT_DESTINATION: memcpy(out, &job->destination, sizeof(job->destination)); break;
    }
}

static int import_result(struct import_job *job, enum import_operation operation, void *out) {
    if (!job)
        return AZ_STALE;
    if (job->operation != operation)
        return AZ_INVALID;
    import_expire(job, drag_now_ms());
    if (job->phase != IMPORT_DONE)
        return AZ_PENDING;
    int status = job->status;
    if (status == AZ_OK)
        import_copy_result(job, out);
    job->phase = IMPORT_FREE;
    job->ticket = 0;
    return status;
}

static int import_poll(uint64_t ticket, enum import_operation operation, void *out) {
    if (!out)
        return AZ_INVALID;
    pthread_mutex_lock(&import_lock);
    int status = import_result(import_find(ticket), operation, out);
    pthread_mutex_unlock(&import_lock);
    return status;
}

int azspi_poll(uint64_t ticket, uint32_t *track_id) {
    return import_poll(ticket, IMPORT_WRITE, track_id);
}

int azspi_poll_analysis(uint64_t ticket, uint64_t *analysis_ticket) {
    return import_poll(ticket, IMPORT_ANALYZE, analysis_ticket);
}

int azspi_poll_destination(uint64_t ticket, unsigned *media) {
    return import_poll(ticket, IMPORT_DESTINATION, media);
}

int azspi_cancel(uint64_t ticket) {
    pthread_mutex_lock(&import_lock);
    struct import_job *job = import_find(ticket);
    int status = AZ_STALE;
    if (job && job->phase == IMPORT_QUEUED) {
        import_reject(job);
        status = AZ_OK;
    } else if (job) status = AZ_BUSY;
    pthread_mutex_unlock(&import_lock);
    return status;
}

static int import_job_earlier(const struct import_job *job, const struct import_job *other, unsigned media) {
    unsigned match = job->operation == IMPORT_DESTINATION ? media & 0xffu : media;
    return job->phase == IMPORT_QUEUED && job->media == match && (!other || job->ticket < other->ticket);
}

static struct import_job *import_claim_locked(unsigned media) {
    struct import_job *selected = NULL;
    long long now = drag_now_ms();
    for (size_t i = 0; i < sizeof(import_jobs) / sizeof(import_jobs[0]); ++i) {
        struct import_job *job = &import_jobs[i];
        import_expire(job, now);
        if (import_job_earlier(job, selected, media))
            selected = job;
    }
    if (selected) {
        __atomic_sub_fetch(&import_pending, 1, __ATOMIC_RELEASE);
        selected->phase = IMPORT_RUNNING;
    }
    return selected;
}

static int import_usb_current(void *conn, const struct st_source *source) {
    struct st_source current;
    int stick = (int) source->ref.source_id - 1;
    return st_source_read(stick, &current) == AZ_OK && st_resolution_source_equal(&current, source) &&
           playlist_database_current(conn, source);
}

static int source_database_sql(struct azspi_sql *sql) {
    *sql = (struct azspi_sql) {.api = az_storage_sql(), .autocommit = g_sq.get_autocommit};
    *(void **) &sql->bind_text = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    return sql->bind_text && az_storage_sql_ready();
}

static int import_backend_write(void *conn, uintptr_t cache, struct import_job *job, uint32_t *id) {
    if (!import_usb_current(conn, &job->record.source))
        return AZ_STALE;
    struct azspi_sql sql;
    if (!source_database_sql(&sql))
        return AZ_UNSUPPORTED;
    if (sql.autocommit(conn) != 1)
        return AZ_BUSY;
    if (lib_backup(conn, NULL))
        return AZ_FAILED;
    int status = azspi_commit(&sql, conn, &job->record, id);
    if (status == AZ_OK)
        lib_invalidate(cache, conn, NULL);
    return status;
}

struct import_server {
    void *connection, *database;
    uintptr_t cache;
    unsigned media;
};

static void *import_connection(uintptr_t database) {
    uintptr_t handle = 0, magic = 0;
    if (!own_q(database + SQLDB_CONN, &handle) || !handle)
        return NULL;
    if (!own_q(handle + SQLITE_MAGIC_Q, &magic) || (unsigned) magic != SQLITE_MAGIC_OPEN)
        return NULL;
    return (void *) handle;
}

static int import_server(void *server, struct import_server *out) {
    uintptr_t svc = 0, lib = 0, db = 0;
    if (mlserver_chain(server, &svc, &lib, &db, &out->media))
        return 0;
    void *connection = import_connection(db);
    if (!connection || !sq_resolve())
        return 0;
    if (!own_q(svc + INFOUPD_CACHE, &out->cache) || !out->cache)
        return 0;
    out->connection = connection;
    out->database = (void *) db;
    return 1;
}

static struct import_job *import_claim(unsigned media) {
    if (pthread_mutex_trylock(&import_lock)) {
        atomic_store_explicit(&g_mv_kick, 1, memory_order_release);
        return NULL;
    }
    struct import_job *job = import_claim_locked(media);
    pthread_mutex_unlock(&import_lock);
    return job;
}

static void import_completed(struct import_job *job, int status) {
    pthread_mutex_lock(&import_lock);
    job->status = status;
    job->phase = IMPORT_DONE;
    pthread_mutex_unlock(&import_lock);
}

static int import_file_current(const struct azspi_record *record) {
    char resolved[1024];
    return st_source_path(record->source.mount, record->audio.relative, resolved) &&
           record->audio.signature == st_disk_sig(resolved);
}

static int import_arm_analysis(const uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES],
                               const uint8_t requester[AZSPI_NATIVE_REQUESTER_BYTES], void *output) {
    return azspd_analysis_watch(raw_id, requester, output);
}

static int import_backend_analyze(const struct import_server *server, struct import_job *job) {
    const struct azspi_record *record = &job->record;
    if (!import_usb_current(server->connection, &record->source) || !import_file_current(record))
        return AZ_STALE;
    uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES];
    int status = azspi_analyzer_submit_watched(g_ep147_build, server->database, (uint16_t) server->media,
                                               &record->source, record->audio.relative, raw_id,
                                               import_arm_analysis, &job->analysis_ticket);
    return status == AZ_PENDING ? AZ_OK : status;
}

static int import_backend_destination(const struct import_server *server, struct import_job *job) {
    if (server->media <= 0xff || server->media > 0xffff)
        return AZ_STALE;
    if (!import_usb_current(server->connection, &job->record.source))
        return AZ_STALE;
    job->destination = server->media;
    return AZ_OK;
}

static int import_execute(const struct import_server *server, struct import_job *job) {
    switch (job->operation) {
    case IMPORT_WRITE: return import_backend_write(server->connection, server->cache, job, &job->track_id);
    case IMPORT_ANALYZE: return import_backend_analyze(server, job);
    case IMPORT_DESTINATION: return import_backend_destination(server, job);
    }
    return AZ_INVALID;
}

static void source_import_native(void *server) {
    if (!__atomic_load_n(&import_pending, __ATOMIC_ACQUIRE))
        return;
    struct import_server owner;
    if (!import_server(server, &owner))
        return;
    struct import_job *job = import_claim(owner.media);
    if (!job)
        return;
    import_completed(job, import_execute(&owner, job));
    if (__atomic_load_n(&import_pending, __ATOMIC_ACQUIRE))
        atomic_store_explicit(&g_mv_kick, 1, memory_order_release);
}

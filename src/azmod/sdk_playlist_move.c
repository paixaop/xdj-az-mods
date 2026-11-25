/* SPDX-License-Identifier: GPL-3.0-only */
/* The JUCE message thread reserves the existing editor slot. EP147's USB
 * library thread performs the edit using the same writer, journal and backup
 * as a knob drop. SDK requests never enter the gesture's UI completion path. */
enum playlist_move_phase { PLAYLIST_QUEUED, PLAYLIST_ARMED, PLAYLIST_FINISHED };

struct playlist_job {
    az_playlist_move_v1 request;
    struct st_source source;
    uint64_t id;
    long long deadline;
    char owner[33];
    enum playlist_move_phase phase;
    az_playlist_move_result_v1 result;
};

static pthread_mutex_t g_playlist_mu = PTHREAD_MUTEX_INITIALIZER;
static struct playlist_job g_playlist;

static bool playlist_editor_ready(void)
{
    return atomic_load_explicit(&g_reorder_enabled, memory_order_acquire) != 0;
}

static bool playlist_positions_valid(const az_playlist_move_v1 *q)
{
    return q->from && q->to && q->from != q->to;
}

static bool playlist_move_valid(const az_playlist_move_v1 *q)
{
    return q && q->size >= sizeof *q && q->version == AZ_DATA_VERSION &&
        q->stick < 2 && q->playlist_id && playlist_positions_valid(q) && q->revision && q->entry_id;
}

static bool playlist_source_matches(const az_playlist_move_v1 *q, const struct st_source *source)
{
    return q->source_generation == source->ref.source_generation &&
           q->library_id == source->ref.library_id;
}

static int playlist_queue_locked(const char *owner, const az_playlist_move_v1 *request,
                                 const struct st_source *source, uint64_t *receipt)
{
    if (g_playlist.id && g_playlist.phase != PLAYLIST_FINISHED)
        return AZ_BUSY;
    if (g_playlist.id == UINT64_MAX)
        return AZ_LIMIT;
    uint64_t id = g_playlist.id + 1;
    g_playlist = (struct playlist_job){ .request = *request, .source = *source, .id = id,
        .deadline = drag_now_ms() + MV_DEADLINE_MS, .phase = PLAYLIST_QUEUED };
    snprintf(g_playlist.owner, sizeof g_playlist.owner, "%s", owner);
    *receipt = id;
    return AZ_PENDING;
}

static int playlist_queue(const char *owner, const az_playlist_move_v1 *request, uint64_t *receipt)
{
    if (!receipt || !playlist_move_valid(request))
        return AZ_INVALID;
    if (!playlist_editor_ready())
        return AZ_UNSUPPORTED;
    struct st_source source;
    if (st_source_read((int)request->stick, &source) != AZ_OK || !playlist_source_matches(request, &source))
        return AZ_STALE;
    pthread_mutex_lock(&g_playlist_mu);
    int rc = playlist_queue_locked(owner, request, &source, receipt);
    pthread_mutex_unlock(&g_playlist_mu);
    return rc;
}

static int playlist_poll_locked(const char *owner, uint64_t id, az_playlist_move_result_v1 *out)
{
    if (id != g_playlist.id || strcmp(owner, g_playlist.owner))
        return AZ_STALE;
    if (g_playlist.phase != PLAYLIST_FINISHED)
        return AZ_PENDING;
    *out = g_playlist.result;
    return AZ_OK;
}

static int playlist_poll(const char *owner, uint64_t id, az_playlist_move_result_v1 *out)
{
    if (!id || !out || out->size < sizeof *out || out->version != AZ_DATA_VERSION)
        return AZ_INVALID;
    pthread_mutex_lock(&g_playlist_mu);
    int rc = playlist_poll_locked(owner, id, out);
    pthread_mutex_unlock(&g_playlist_mu);
    return rc;
}

static void playlist_complete(int status, unsigned outcome, unsigned pdb)
{
    g_playlist.result = (az_playlist_move_result_v1){ .size = sizeof g_playlist.result,
        .version = AZ_DATA_VERSION, .status = status, .outcome = outcome, .device_library = pdb };
    g_playlist.phase = PLAYLIST_FINISHED;
}

static void playlist_expire(void)
{
    if (g_playlist.phase == PLAYLIST_ARMED) {
        int armed = MV_SDK_ARMED;
        if (!atomic_compare_exchange_strong(&g_mv_state, &armed, MV_IDLE))
            return;
    }
    playlist_complete(AZ_CANCELLED, AZ_PLAYLIST_NOT_MOVED, AZ_PDB_NOT_ATTEMPTED);
}

static void playlist_dispatch(void)
{
    if (!g_playlist.id || g_playlist.phase == PLAYLIST_FINISHED)
        return;
    if (drag_now_ms() >= g_playlist.deadline || !playlist_editor_ready()) {
        playlist_expire();
        return;
    }
    if (g_playlist.phase == PLAYLIST_QUEUED && atomic_load(&g_mv_state) == MV_IDLE &&
        !atomic_load_explicit(&g_drag_on, memory_order_acquire)) {
        g_playlist.phase = PLAYLIST_ARMED;
        atomic_store(&g_mv_state, MV_SDK_ARMED);
        atomic_store(&g_mv_kick, 1);
    }
}

static void playlist_sdk_tick(void)
{
    if (pthread_mutex_trylock(&g_playlist_mu) != 0)
        return;
    playlist_dispatch();
    pthread_mutex_unlock(&g_playlist_mu);
}

static void *playlist_connection(void *server, unsigned *media)
{
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0, magic = 0;
    if (mlserver_chain(server, &svc, &lib, &db, media) ||
        !own_q(db + SQLDB_CONN, &conn) || !conn ||
        !own_q(conn + SQLITE_MAGIC_Q, &magic) || (unsigned)magic != SQLITE_MAGIC_OPEN)
        return NULL;
    return (void *)conn;
}

static bool playlist_claim_locked(void *conn, struct playlist_job *job)
{
    if (atomic_load(&g_mv_state) != MV_SDK_ARMED || drag_now_ms() >= g_playlist.deadline)
        return false;
    char mount[1024];
    if (!cp_mount_of(conn, mount, sizeof mount) || strcmp(mount, g_playlist.source.mount))
        return false;
    *job = g_playlist;
    atomic_store(&g_mv_state, MV_SDK_TAKEN);
    return true;
}

static bool playlist_claim(void *conn, struct playlist_job *job)
{
    if (!conn || pthread_mutex_trylock(&g_playlist_mu) != 0)
        return false;
    bool claimed = playlist_claim_locked(conn, job);
    pthread_mutex_unlock(&g_playlist_mu);
    return claimed;
}

static bool playlist_database_current(void *conn, const struct st_source *source)
{
    const char *(*filename)(void *, const char *);
    *(void **)&filename = dlsym(RTLD_DEFAULT, "sqlite3_db_filename");
    const char *path = filename ? filename(conn, "main") : NULL;
    struct stat disk;
    return path && stat(path, &disk) == 0 && (uint64_t)disk.st_dev == source->device &&
           (uint64_t)disk.st_ino == source->inode;
}

static bool playlist_current(void *conn, const struct playlist_job *job)
{
    struct st_source current;
    return st_source_read((int)job->request.stick, &current) == AZ_OK &&
           st_resolution_source_equal(&current, &job->source) &&
           playlist_database_current(conn, &current);
}

static int playlist_verify_order(void *conn, const az_playlist_move_v1 *q, az_playlist_entry_v1 *entry)
{
    az_playlist_order_v1 order = {0};
    int rc = playlist_order_query(conn, q->playlist_id, q->from - 1, entry, 1, &order);
    if (rc != AZ_OK)
        return rc;
    if (order.revision != q->revision || !order.returned || entry->entry_id != q->entry_id)
        return AZ_STALE;
    return q->to <= order.total ? AZ_OK : AZ_INVALID;
}

static int playlist_preflight(void *conn, const struct playlist_job *job, az_playlist_entry_v1 *entry)
{
    if (!playlist_editor_ready() || !playlist_current(conn, job))
        return AZ_STALE;
    if (atomic_load_explicit(&g_drag_on, memory_order_acquire) ||
        !az_storage_sql_ready() || !sq_resolve() || g_sq.get_autocommit(conn) != 1)
        return AZ_BUSY;
    return playlist_verify_order(conn, &job->request, entry);
}
static bool playlist_moved(void *conn, const az_playlist_move_v1 *q)
{
    az_playlist_order_v1 order = {0};
    az_playlist_entry_v1 entry = {0};
    return playlist_order_query(conn, q->playlist_id, q->to - 1, &entry, 1, &order) == AZ_OK &&
           order.returned == 1 && entry.entry_id == q->entry_id;
}

static int playlist_run_native(void *server, unsigned media, const az_playlist_move_v1 *q, uint32_t track)
{
    g_mv = (typeof(g_mv)){ .op = MV_OP_MOVE, .pl = q->playlist_id, .cid = track,
        .from = q->from, .to = q->to, .media = media };
    t_pst_own++;
    int rc = mv_execute(server, NULL);
    t_pst_own--;
    return rc;
}

static void playlist_execute(void *server, void *conn, unsigned media, const struct playlist_job *job)
{
    az_playlist_entry_v1 entry = {0};
    int status = playlist_preflight(conn, job, &entry);
    unsigned outcome = AZ_PLAYLIST_NOT_MOVED, pdb = AZ_PDB_NOT_ATTEMPTED;
    if (status == AZ_OK) {
        outcome = AZ_PLAYLIST_UNCERTAIN;
        status = AZ_FAILED;
        if (playlist_run_native(server, media, &job->request, entry.track_id) == 0) {
            pdb = (unsigned)mv_pdb_sync_status(server);
            if (playlist_moved(conn, &job->request)) {
                outcome = AZ_PLAYLIST_MOVED;
                status = AZ_OK;
            }
        }
    }
    pthread_mutex_lock(&g_playlist_mu);
    playlist_complete(status, outcome, pdb);
    atomic_store(&g_mv_state, MV_IDLE);
    pthread_mutex_unlock(&g_playlist_mu);
}

static void playlist_sdk_native(void *server)
{
    if (atomic_load(&g_mv_state) != MV_SDK_ARMED)
        return;
    unsigned media = 0;
    void *conn = playlist_connection(server, &media);
    struct playlist_job job;
    if (playlist_claim(conn, &job))
        playlist_execute(server, conn, media, &job);
}

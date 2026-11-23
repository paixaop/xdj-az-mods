/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded analysis publication progression, owned by the core service worker. */
struct sa_link {
    uint64_t id;
    az_analysis_owner owner;
    struct st_source source;
    char database[1024], audio[2048], analysis[1024], spool[1024];
    int admitted, outcome;
};
static struct sa_link sa_links[8];
static _Atomic(void (*)(void)) sa_completion_wake;
void analysis_source_set_wake(void (*wake)(void)) {
    atomic_store_explicit(&sa_completion_wake, wake, memory_order_release);
}
static void sa_link_notify(void) {
    void (*wake)(void) = atomic_load_explicit(&sa_completion_wake, memory_order_acquire);
    if (wake) wake();
}
static int sa_link_enqueue(struct sa_job *job, const char *path) {
    struct sa_link *entry = NULL;
    for (unsigned i = 0; i < 8 && !entry; ++i)
        if (!sa_links[i].id || sa_links[i].id == job->id) entry = &sa_links[i];
    if (!entry) return 0;
    *entry = (struct sa_link) {.id = job->id, .owner = job->lifetime, .source = job->source, .outcome = AZ_PENDING};
    memcpy(entry->database, job->database, sizeof entry->database);
    memcpy(entry->audio, job->relative_audio, sizeof entry->audio);
    memcpy(entry->analysis, job->relative_dat, sizeof entry->analysis);
    snprintf(entry->spool, sizeof entry->spool, "%s", path);
    sa_link_wake();
    return 1;
}
static int sa_link_row(const struct sa_link *link) {
    if (!st_database_quiet(link->database)) return AZ_PENDING;
    void *db = sdk_db_open(link->database);
    if (!db) return AZ_FAILED;
    struct st_resolution row = {0};
    int rc = st_read_row(db, link->source.ref.track_id, &row);
    g_sdk_sq.close(db);
    if (rc) return rc;
    if (strcmp(row.content, link->audio)) return AZ_STALE;
    if (!row.analysis_generated) return strcmp(row.analysis, link->analysis) ? AZ_STALE : AZ_OK;
    return strcmp(row.analysis, link->analysis) ? AZ_STALE : AZ_PENDING;
}
static int sa_link_current(const struct sa_link *link) {
    struct st_source source;
    if (st_source_read((int) link->source.ref.source_id - 1, &source)) return AZ_STALE;
    source.ref.track_id = link->source.ref.track_id;
    if (!st_resolution_source_equal(&source, &link->source)) return AZ_STALE;
    return sa_link_row(link);
}
static int sa_link_file_state(const struct sa_link *link) {
    char marker[1100];
    snprintf(marker, sizeof marker, "%s.failed", link->spool);
    if (!access(marker, F_OK)) return AZ_FAILED;
    snprintf(marker, sizeof marker, "%s.rejected", link->spool);
    if (!access(marker, F_OK)) return AZ_FAILED;
    return access(link->spool, F_OK) ? AZ_FAILED : AZ_PENDING;
}
static int sa_link_progress(struct sa_link *link) {
    if (!analysis_owner_current(link->owner) && !link->admitted) {
        unlink(link->spool);
        return AZ_STALE;
    }
    int rc = sa_link_current(link);
    if (rc != AZ_PENDING) return rc;
    rc = sa_link_file_state(link);
    if (rc != AZ_PENDING || link->admitted) return rc;
    rc = sdk_lib_spool_admit((int) link->source.ref.source_id - 1);
    if (rc == AZ_BUSY) return AZ_PENDING;
    if (rc != AZ_OK) return rc;
    link->admitted = 1;
    return AZ_PENDING;
}
static void sa_link_release(uint64_t id);
static int sa_links_service(void) {
    int pending = 0, changed = 0;
    pthread_mutex_lock(&sa_mutex);
    for (unsigned i = 0; i < 8; ++i) {
        struct sa_link *link = &sa_links[i];
        if (!link->id) continue;
        if (!analysis_owner_current(link->owner)) { sa_link_release(link->id); continue; }
        if (link->outcome != AZ_PENDING) continue;
        link->outcome = sa_link_progress(link);
        pending |= link->outcome == AZ_PENDING;
        changed |= link->outcome != AZ_PENDING;
    }
    pthread_mutex_unlock(&sa_mutex);
    if (changed) sa_link_notify();
    return pending;
}
static void sa_link_release(uint64_t id) {
    for (unsigned i = 0; i < 8; ++i) {
        struct sa_link *link = &sa_links[i];
        if (!link->id || link->id != id) continue;
        if (!link->admitted) unlink(link->spool);
        memset(link, 0, sizeof *link);
    }
}
int analysis_source_outcome(az_analysis_owner owner, const analysis_source_input *input) {
    if (!input || !input->lease) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    int rc = AZ_STALE;
    for (unsigned i = 0; i < 8; ++i) {
        const struct sa_link *link = &sa_links[i];
        if (link->id == input->lease && link->owner.generation == owner.generation) rc = link->outcome;
    }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}

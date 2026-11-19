/* SPDX-License-Identifier: GPL-3.0-only */
/* Private source adapter. Included after the source/publication implementation. */
#include <semaphore.h>
static pthread_mutex_t sa_cleaner_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t sa_cleaner_wake;
static _Atomic int sa_cleaner_started;
static _Atomic int sa_cleanup_pending;
static int sa_take_retired(struct sa_job *out) {
    pthread_mutex_lock(&sa_mutex);
    int found = 0;
    for (unsigned i = 0; i < 8 && !found; ++i) {
        if (!sa_jobs[i].id || sa_owner_live(&sa_jobs[i])) continue;
        *out = sa_jobs[i];
        memset(&sa_jobs[i], 0, sizeof sa_jobs[i]);
        sa_jobs[i].reader_fd = -1;
        found = 1;
    }
    pthread_mutex_unlock(&sa_mutex);
    return found;
}
static void *sa_cleaner(void *unused) {
    (void) unused;
    int pending = 0;
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        if (pending) {
            while (sem_timedwait(&sa_cleaner_wake, &deadline) && errno == EINTR) {}
        } else {
            while (sem_wait(&sa_cleaner_wake) && errno == EINTR) {}
        }
        atomic_store_explicit(&sa_cleanup_pending, 0, memory_order_release);
        struct sa_job job;
        while (sa_take_retired(&job)) sa_job_drop(&job);
        pending = sa_links_service();
    }
    return NULL;
}
static int sa_cleaner_launch(void) {
    if (sem_init(&sa_cleaner_wake, 0, 0)) return AZ_FAILED;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attributes, SCHED_OTHER);
    struct sched_param priority = {0};
    pthread_attr_setschedparam(&attributes, &priority);
    pthread_t thread;
    int rc = az_thread_create(&thread, &attributes, sa_cleaner, NULL);
    pthread_attr_destroy(&attributes);
    if (rc) sem_destroy(&sa_cleaner_wake);
    return rc ? AZ_FAILED : AZ_OK;
}
static int sa_cleaner_start(void) {
    pthread_mutex_lock(&sa_cleaner_mutex);
    if (!sa_cleaner_started) sa_cleaner_started = sa_cleaner_launch() == AZ_OK;
    int rc = sa_cleaner_started ? AZ_OK : AZ_FAILED;
    pthread_mutex_unlock(&sa_cleaner_mutex);
    return rc;
}
static void sa_link_wake(void) {
    if (!atomic_load_explicit(&sa_cleaner_started, memory_order_acquire)) return;
    if (!atomic_exchange_explicit(&sa_cleanup_pending, 1, memory_order_acq_rel))
        sem_post(&sa_cleaner_wake);
}
void analysis_source_retire(az_analysis_owner owner) {
    if (owner.generation) sa_link_wake();
}
static int sa_source_owner_index(az_analysis_owner owner) {
    return owner.kind == AZ_ANALYSIS_OWNER_CORE ? 32 : (int) owner.slot;
}
static int sa_input_grid(const struct sa_job *job, analysis_source_input *input) {
    if (!(input->needed & AZ_DATA_PHRASES)) return AZ_OK;
    struct sa_grid_view grid = {0};
    int rc = sa_grid_read(job, &grid);
    if (rc == AZ_OK) {
        input->beats = calloc(grid.count, sizeof *input->beats);
        if (!input->beats) rc = AZ_FAILED;
        else {
            analysis_grid_shape result = {.size = sizeof result};
            rc = sa_grid_copy(&grid, &result, input->beats, grid.count);
            input->beat_count = result.beat_count;
        }
    }
    free(grid.file);
    /* File-only callers can provide a generated grid with their typed result. */
    return rc == AZ_NOT_FOUND && !job->loaded ? AZ_OK : rc;
}
static int sa_input_copy(az_analysis_owner owner, const analysis_source_request *request,
                          const analysis_source_lease *lease, analysis_source_input *input) {
    pthread_mutex_lock(&sa_mutex);
    struct sa_job *job = sa_find_job(sa_source_owner_index(owner), lease->job);
    int rc = AZ_STALE;
    if (job && job->lifetime.generation == owner.generation && sa_current(job)) {
        job->needed &= request->components | AZ_DATA_GRID;
        if (!(job->needed & AZ_DATA_PHRASES)) job->needed &= ~AZ_DATA_GRID;
        input->needed = job->needed;
        input->load_epoch = job->native_epoch;
        rc = input->needed ? sa_input_grid(job, input) : AZ_NOT_FOUND;
    }
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_input_request_valid(const analysis_source_request *request, const analysis_source_input *input) {
    return request && input && request->components &&
           !(request->components & ~(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS));
}
int analysis_source_acquire(az_analysis_owner owner, const analysis_source_request *request,
                             analysis_source_input *input) {
    if (!sa_input_request_valid(request, input)) return AZ_INVALID;
    if (!analysis_owner_current(owner)) return AZ_STALE;
    int index = sa_source_owner_index(owner);
    analysis_source_lease lease = {.size = sizeof lease};
    int vocals = !!(request->components & AZ_ANALYSIS_VOCALS);
    int rc = request->loaded ? sa_open_loaded(index, request->deck, request->load, &lease, vocals) :
                              sa_open(index, &request->track, &lease, 0, vocals);
    if (rc != AZ_OK && rc != AZ_PENDING) return rc;
    *input = (analysis_source_input) {.lease = lease.job, .owner = owner, .bpm = lease.original_bpm,
                                      .track = lease.track, .deck = (unsigned) request->deck};
    memcpy(input->audio_path, lease.audio_path, sizeof input->audio_path);
    if (rc == AZ_PENDING) return rc;
    rc = sa_input_copy(owner, request, &lease, input);
    if (rc) analysis_source_release(input);
    return rc;
}
int analysis_source_publish(az_analysis_owner owner, const analysis_source_input *input,
                             const az_analysis_result_v1 *result) {
    if (!input || !input->lease) return AZ_INVALID;
    if (!analysis_owner_current(owner) || owner.generation != input->owner.generation) return AZ_STALE;
    return sa_commit_typed(sa_source_owner_index(owner), input->lease, result);
}
void analysis_source_release(analysis_source_input *input) {
    if (!input) return;
    pthread_mutex_lock(&sa_mutex);
    struct sa_job *job = sa_find_job(sa_source_owner_index(input->owner), input->lease);
    struct sa_job retired = {.reader_fd = -1};
    if (job) {
        retired = *job;
        memset(job, 0, sizeof *job);
        job->reader_fd = -1;
    }
    sa_link_release(input->lease);
    pthread_mutex_unlock(&sa_mutex);
    sa_job_drop(&retired);
    free(input->beats);
    memset(input, 0, sizeof *input);
}

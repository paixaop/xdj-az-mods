/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "analysis_runtime.h"
#include "../analysis/analysis.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif
#define ANALYSIS_SLOTS 12u
#define ANALYSIS_PUBLIC_SLOTS 8u
struct ar_job {
    az_analysis_owner owner;
    az_analysis_request_v1 request;
    az_analysis_status_v1 status;
    ta_result *result;
    analysis_source_input input;
    uint64_t outcome_checked;
    atomic_int cancelled;
    int released, automatic, publishing;
    struct timespec started;
};
struct ar_load {
    uint64_t epoch, attempted, order;
    int loaded;
    az_track_ref track;
    az_analysis_status_v1 status;
};
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t worker;
    struct ar_job jobs[ANALYSIS_SLOTS];
    struct ar_load decks[4];
    az_analysis_owner automatic_owner;
    uint64_t serial, notification;
    unsigned next;
    int started, stopping, enabled, policy, condition_ready;
    char assets[1024];
} ar = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static int ar_condition_init(void) {
    if (ar.condition_ready) return AZ_OK;
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes)) return AZ_FAILED;
    int rc = 0;
#ifdef __linux__
    rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
    if (!rc) rc = pthread_cond_init(&ar.wake, &attributes);
    pthread_condattr_destroy(&attributes);
    if (!rc) ar.condition_ready = 1;
    return rc ? AZ_FAILED : AZ_OK;
}
static void ar_signal(void) {
    if (ar.condition_ready) pthread_cond_signal(&ar.wake);
}
static int ar_wait_seconds(long seconds) {
#ifdef __APPLE__
    struct timespec delay = {.tv_sec = seconds};
    return pthread_cond_timedwait_relative_np(&ar.wake, &ar.mutex, &delay);
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) return ETIMEDOUT;
    deadline.tv_sec += seconds;
    return pthread_cond_timedwait(&ar.wake, &ar.mutex, &deadline);
#endif
}
static int ar_owner_equal(az_analysis_owner a, az_analysis_owner b) {
    return a.generation == b.generation && a.slot == b.slot && a.kind == b.kind;
}
static int ar_terminal(unsigned state) {
    return state >= AZ_ANALYSIS_COMPLETE;
}
static int ar_retryable(int result) {
    return result == AZ_BUSY || result == AZ_LIMIT || result == AZ_STALE;
}
static int ar_policy(void) {
#ifdef __linux__
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(1, &cpus); CPU_SET(2, &cpus); CPU_SET(3, &cpus);
    return !sched_setaffinity(0, sizeof cpus, &cpus) && !setpriority(PRIO_PROCESS, 0, 19);
#else
    return 1;
#endif
}
static void ar_drop(struct ar_job *job) {
    ta_result_free(job->result);
    memset(job, 0, sizeof *job);
    atomic_init(&job->cancelled, 0);
}
static int ar_execution_valid(const az_analysis_request_v1 *r) {
    if (r->stage_mode > AZ_ANALYSIS_CONCURRENT) return 0;
    if (r->phrase_threads < 1 || r->phrase_threads > 3) return 0;
    if (r->vocal_threads < 1 || r->vocal_threads > 3) return 0;
    if (r->stage_mode == AZ_ANALYSIS_SEQUENTIAL) return 1;
    if (r->components != (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)) return 1;
    return r->phrase_threads + r->vocal_threads <= 3;
}
static int ar_source_valid(const az_analysis_request_v1 *r) {
    if (r->source_kind == AZ_ANALYSIS_DECK) return r->deck < 4 && r->load_epoch;
    if (r->source_kind != AZ_ANALYSIS_TRACK) return 0;
    return r->track.size >= sizeof r->track && r->track.version == AZ_DATA_VERSION &&
           r->track.track_id && r->track.source_id >= 1 && r->track.source_id <= 2;
}
static int ar_request_valid(const az_analysis_request_v1 *r) {
    if (!r || r->size < sizeof *r || r->version != AZ_ANALYSIS_REQUEST_VERSION) return 0;
    if (!r->components || (r->components & ~(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS))) return 0;
    for (unsigned i = 0; i < 4; i++) if (r->reserved[i]) return 0;
    return ar_source_valid(r) && ar_execution_valid(r);
}
static struct ar_job *ar_find(az_analysis_owner owner, uint64_t id) {
    for (unsigned i = 0; i < ANALYSIS_SLOTS; i++) {
        struct ar_job *job = &ar.jobs[i];
        if (job->status.request_id == id && ar_owner_equal(job->owner, owner)) return job;
    }
    return NULL;
}
static int ar_owned_count(az_analysis_owner owner) {
    int count = 0;
    for (unsigned i = 0; i < ANALYSIS_PUBLIC_SLOTS; i++)
        count += ar.jobs[i].status.request_id && ar_owner_equal(ar.jobs[i].owner, owner);
    return count;
}
static struct ar_job *ar_free_slot(void) {
    for (unsigned i = 0; i < ANALYSIS_PUBLIC_SLOTS; i++)
        if (!ar.jobs[i].status.request_id) return &ar.jobs[i];
    return NULL;
}
static void ar_admit(struct ar_job *job, az_analysis_owner owner, const az_analysis_request_v1 *r) {
    ar_drop(job);
    job->owner = owner;
    job->request = *r;
    job->status = (az_analysis_status_v1){.size = sizeof job->status,
        .version = AZ_DATA_VERSION, .state = AZ_ANALYSIS_QUEUED,
        .request_id = ++ar.serial, .load_epoch = r->load_epoch,
        .track = r->track, .deck = r->deck, .result = AZ_PENDING};
}
static int ar_admission(az_analysis_owner owner, struct ar_job **job) {
    if (!analysis_owner_current(owner)) return AZ_STALE;
    if (!ar.started || ar.stopping) return AZ_UNSUPPORTED;
    *job = ar_free_slot();
    if (!*job || ar_owned_count(owner) >= 4 || ar.serial == UINT64_MAX) return AZ_LIMIT;
    return AZ_OK;
}
int analysis_runtime_request(az_analysis_owner owner, const az_analysis_request_v1 *r, uint64_t *id) {
    if (!id || !ar_request_valid(r) || owner.kind != AZ_ANALYSIS_OWNER_MOD) return AZ_INVALID;
    if (pthread_mutex_trylock(&ar.mutex)) return AZ_BUSY;
    struct ar_job *job = NULL;
    int rc = ar_admission(owner, &job);
    if (rc == AZ_OK) {
        ar_admit(job, owner, r);
        *id = job->status.request_id;
        ar.notification++;
    ar_signal();
    }
    pthread_mutex_unlock(&ar.mutex);
    return rc;
}
static int ar_cancelled(void *user) {
    struct ar_job *job = user;
    struct timespec now;
    if (atomic_load(&job->cancelled)) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 1;
    return now.tv_sec - job->started.tv_sec >= 600;
}
static int ar_engine(struct ar_job *job, const analysis_source_input *input, char error[160]) {
    if (ar_cancelled(job)) return AZ_STALE;
    ta_execution execution = {(ta_stage_mode)job->request.stage_mode,
        job->request.phrase_threads, job->request.vocal_threads};
    ta_request request = {.audio_path = input->audio_path, .assets_path = ar.assets,
        .beats = input->beats, .beat_count = input->beat_count, .needed = input->needed,
        .bpm = input->bpm, .execution = &execution, .cancelled = ar_cancelled, .user = job};
    int rc = ta_analyze(&request, &job->result, error, 160);
    if (rc == TA_CANCELLED) return AZ_STALE;
    if (rc == TA_LIMIT) return AZ_LIMIT;
    return rc == TA_OK ? AZ_OK : AZ_FAILED;
}
static int ar_pending(struct ar_job *job) {
    if (ar_cancelled(job)) return AZ_STALE;
    return analysis_source_outcome(job->owner, &job->input);
}
static int ar_compute(struct ar_job *job, char error[160]) {
    if (!ar.policy) return AZ_UNSUPPORTED;
    if (clock_gettime(CLOCK_MONOTONIC, &job->started)) return AZ_FAILED;
    if (ar_cancelled(job)) return AZ_STALE;
    analysis_source_request request = {.loaded = job->request.source_kind == AZ_ANALYSIS_DECK,
        .components = job->request.components, .deck = (int32_t)job->request.deck,
        .load = job->request.load_epoch, .track = job->request.track};
    int rc = analysis_source_acquire(job->owner, &request, &job->input);
    if (rc) return rc;
    rc = ar_engine(job, &job->input, error);
    if (rc == AZ_OK && ar_cancelled(job)) rc = AZ_STALE;
    if (rc == AZ_OK) {
        job->publishing = 1;
        rc = analysis_source_publish(job->owner, &job->input, ta_result_view(job->result));
    }
    return rc;
}
static void ar_finish(struct ar_job *job, int rc) {
    job->publishing = 0;
    job->status.result = rc == ANALYSIS_SOURCE_COMPLETE ? AZ_NOT_FOUND : rc;
    job->status.state = rc == AZ_OK || rc == ANALYSIS_SOURCE_COMPLETE ? AZ_ANALYSIS_COMPLETE : AZ_ANALYSIS_FAILED;
    if (atomic_load(&job->cancelled)) {
        job->status.state = AZ_ANALYSIS_CANCELLED;
        job->status.result = AZ_STALE;
    }
    if (rc == AZ_OK || rc == ANALYSIS_SOURCE_COMPLETE)
        job->status.completed_components = job->request.components;
    if (!job->status.diagnostic[0] && rc)
        snprintf(job->status.diagnostic, sizeof job->status.diagnostic, "analysis result %d", rc);
}
static void ar_publish_status(const struct ar_job *job) {
    if (!job->automatic) return;
    struct ar_load *deck = &ar.decks[job->request.deck];
    if (deck->epoch == job->request.load_epoch) deck->status = job->status;
}
static void ar_finish_automatic(struct ar_job *job) {
    struct ar_load *deck = &ar.decks[job->request.deck];
    if (deck->epoch != job->request.load_epoch) return;
    ar_publish_status(job);
    if (ar_retryable(job->status.result) && !atomic_load(&job->cancelled)) deck->attempted = 0;
}
static void ar_configuration_failure(struct ar_load *load, unsigned deck, const char *error) {
    load->status = (az_analysis_status_v1){.size = sizeof load->status,
        .version = AZ_DATA_VERSION, .request_id = ++ar.serial,
        .state = AZ_ANALYSIS_FAILED, .result = AZ_INVALID,
        .deck = deck, .load_epoch = load->epoch, .track = load->track};
    snprintf(load->status.diagnostic, sizeof load->status.diagnostic, "%s", error);
}
static void ar_collect(struct ar_job *job, int rc) {
    ar_finish(job, rc);
    if (job->automatic) ar_finish_automatic(job);
    if (job->automatic || job->released) ar_drop(job);
}
static void ar_prepare_automatic(unsigned deck) {
    struct ar_load *load = &ar.decks[deck];
    struct ar_job *job = &ar.jobs[ANALYSIS_PUBLIC_SLOTS + deck];
    if (job->status.request_id || !load->loaded || !load->epoch || load->attempted == load->epoch) return;
    if (!ar.enabled || ar.serial == UINT64_MAX) return;
    ta_execution execution;
    char error[160];
    int rc = ta_execution_environment(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, &execution, error, sizeof error);
    load->attempted = load->epoch;
    if (rc) {
        ar_configuration_failure(load, deck, error);
        return;
    }
    az_analysis_request_v1 r = {.size = sizeof r, .version = AZ_ANALYSIS_REQUEST_VERSION,
        .source_kind = AZ_ANALYSIS_DECK, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS,
        .track = load->track, .load_epoch = load->epoch, .deck = deck,
        .stage_mode = (uint32_t)execution.mode, .phrase_threads = execution.phrase_threads,
        .vocal_threads = execution.vocal_threads};
    ar_admit(job, ar.automatic_owner, &r);
    job->automatic = 1;
    load->status = job->status;
}
static struct ar_job *ar_next(void) {
    for (unsigned deck = 0; deck < 4; deck++) ar_prepare_automatic(deck);
    for (unsigned n = 0; n < ANALYSIS_SLOTS; n++) {
        unsigned slot = (ar.next + n) % ANALYSIS_SLOTS;
        struct ar_job *job = &ar.jobs[slot];
        int pending = (job->status.state == AZ_ANALYSIS_SAVED || job->publishing) &&
                      job->outcome_checked != ar.notification;
        if (job->status.state != AZ_ANALYSIS_QUEUED && !pending) continue;
        ar.next = (slot + 1) % ANALYSIS_SLOTS;
        if (!pending) job->status.state = AZ_ANALYSIS_RUNNING;
        ar_publish_status(job);
        job->outcome_checked = ar.notification;
        return job;
    }
    return NULL;
}
static void ar_wait_retry(int retry) {
    if (!retry || ar.stopping) return;
    ar_wait_seconds(1);
    ar.notification++;
}
static int ar_advance(struct ar_job *job, char error[160]) {
    if (job->status.state == AZ_ANALYSIS_SAVED) return ar_pending(job);
    if (!job->publishing) return ar_compute(job, error);
    if (ar_cancelled(job)) return AZ_STALE;
    return analysis_source_publish(job->owner, &job->input, ta_result_view(job->result));
}
static void ar_saved(struct ar_job *job, int busy) {
    job->status.result = AZ_PENDING;
    if (!busy) job->publishing = 0;
    job->status.state = job->publishing ? AZ_ANALYSIS_RUNNING : AZ_ANALYSIS_SAVED;
    if (!job->publishing) job->status.completed_components = job->request.components;
    ar_publish_status(job);
}
static void ar_process_job(struct ar_job *job) {
    pthread_mutex_unlock(&ar.mutex);
    char error[160] = {0};
    int rc = ar_advance(job, error);
    int busy = (job->status.state == AZ_ANALYSIS_SAVED || job->publishing) && rc == AZ_BUSY;
    if (busy) rc = AZ_PENDING;
    if (rc != AZ_PENDING) analysis_source_release(&job->input);
    pthread_mutex_lock(&ar.mutex);
    snprintf(job->status.diagnostic, sizeof job->status.diagnostic, "%s", error);
    int retry = busy || (job->automatic && ar_retryable(rc));
    if (rc == AZ_PENDING) ar_saved(job, busy);
    else ar_collect(job, rc);
    ar_wait_retry(retry);
}
static void ar_shutdown_jobs(void) {
    pthread_mutex_unlock(&ar.mutex);
    for (unsigned i = 0; i < ANALYSIS_SLOTS; i++) {
        analysis_source_release(&ar.jobs[i].input);
        pthread_mutex_lock(&ar.mutex);
        ar_drop(&ar.jobs[i]);
        pthread_mutex_unlock(&ar.mutex);
    }
    pthread_mutex_lock(&ar.mutex);
}
static long ar_saved_deadline(void) {
    long remaining = -1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    for (unsigned i = 0; i < ANALYSIS_SLOTS; i++) {
        const struct ar_job *job = &ar.jobs[i];
        if (job->status.state != AZ_ANALYSIS_SAVED) continue;
        long seconds = 600 - (now.tv_sec - job->started.tv_sec);
        if (seconds <= 0) return 0;
        if (remaining < 0 || seconds < remaining) remaining = seconds;
    }
    return remaining;
}
static void ar_wait_work(void) {
    long remaining = ar_saved_deadline();
    if (remaining < 0) { pthread_cond_wait(&ar.wake, &ar.mutex); return; }
    if (!remaining) { ar.notification++; return; }
    if (ar_wait_seconds(remaining) == ETIMEDOUT) ar.notification++;
}
static void *ar_worker(void *unused) {
    (void)unused;
    ar.policy = ar_policy();
    pthread_mutex_lock(&ar.mutex);
    while (!ar.stopping) {
        struct ar_job *job = ar_next();
        if (job) ar_process_job(job);
        else ar_wait_work();
    }
    ar_shutdown_jobs();
    pthread_mutex_unlock(&ar.mutex);
    return NULL;
}
int analysis_runtime_start(const char *assets) {
    if (!assets || !*assets || strlen(assets) >= sizeof ar.assets) return AZ_INVALID;
    pthread_mutex_lock(&ar.mutex);
    if (ar.started) { pthread_mutex_unlock(&ar.mutex); return AZ_OK; }
    int condition = ar_condition_init();
    if (condition) { pthread_mutex_unlock(&ar.mutex); return condition; }
    snprintf(ar.assets, sizeof ar.assets, "%s", assets);
    ar.stopping = 0;
    analysis_source_set_wake(analysis_runtime_wake);
    int rc = pthread_create(&ar.worker, NULL, ar_worker, NULL);
    if (!rc) ar.started = 1;
    pthread_mutex_unlock(&ar.mutex);
    return rc ? AZ_FAILED : AZ_OK;
}
void analysis_runtime_shutdown(void) {
    pthread_mutex_lock(&ar.mutex);
    int started = ar.started;
    ar.stopping = 1;
    for (unsigned i = 0; i < ANALYSIS_SLOTS; i++) atomic_store(&ar.jobs[i].cancelled, 1);
    if (ar.condition_ready) pthread_cond_broadcast(&ar.wake);
    pthread_mutex_unlock(&ar.mutex);
    if (started && pthread_join(ar.worker, NULL)) abort();
    pthread_mutex_lock(&ar.mutex);
    ar.started = 0;
    pthread_mutex_unlock(&ar.mutex);
}
int analysis_runtime_status(az_analysis_owner owner, uint64_t id, az_analysis_status_v1 *out) {
    if (!id || !out || out->size < sizeof *out) return AZ_INVALID;
    if (pthread_mutex_trylock(&ar.mutex)) return AZ_BUSY;
    struct ar_job *job = ar_find(owner, id);
    int rc = job && !job->released ? AZ_OK : AZ_STALE;
    if (rc == AZ_OK) *out = job->status;
    pthread_mutex_unlock(&ar.mutex);
    return rc;
}
static void ar_revoke(struct ar_job *job, int release) {
    atomic_store(&job->cancelled, 1);
    job->released |= release;
    if (job->status.state == AZ_ANALYSIS_RUNNING || job->status.state == AZ_ANALYSIS_SAVED) return;
    ar_finish(job, AZ_STALE);
    if (job->released) ar_drop(job);
}
static int ar_cancel_or_release(az_analysis_owner owner, uint64_t id, int release) {
    if (!id) return AZ_INVALID;
    if (pthread_mutex_trylock(&ar.mutex)) return AZ_BUSY;
    struct ar_job *job = ar_find(owner, id);
    int rc = job ? AZ_OK : AZ_STALE;
    if (job) ar_revoke(job, release);
    ar.notification++;
    ar_signal();
    pthread_mutex_unlock(&ar.mutex);
    return rc;
}
int analysis_runtime_cancel(az_analysis_owner owner, uint64_t id) {
    return ar_cancel_or_release(owner, id, 0);
}
int analysis_runtime_release(az_analysis_owner owner, uint64_t id) {
    return ar_cancel_or_release(owner, id, 1);
}
void analysis_runtime_retire(az_analysis_owner owner) {
    pthread_mutex_lock(&ar.mutex);
    for (unsigned i = 0; i < ANALYSIS_SLOTS; i++)
        if (ar_owner_equal(ar.jobs[i].owner, owner)) ar_revoke(&ar.jobs[i], 1);
    ar.notification++;
    ar_signal();
    pthread_mutex_unlock(&ar.mutex);
}
static int ar_result_ready(const struct ar_job *job, const az_analysis_result_v1 **result) {
    if (!ar_terminal(job->status.state)) return AZ_PENDING;
    if (job->status.result != AZ_OK) return job->status.result;
    *result = ta_result_view(job->result);
    return *result ? AZ_OK : AZ_NOT_FOUND;
}
static void ar_copy_arrays(const az_analysis_result_v1 *r, az_analysis_result_v1 *copy,
                            az_analysis_phrase_v1 *phrases, az_vocal_frame_v1 *vocals) {
    if (r->phrases.count) memcpy(phrases, r->phrases.entries, r->phrases.count * sizeof *phrases);
    if (r->vocals.count) memcpy(vocals, r->vocals.frames, r->vocals.count * sizeof *vocals);
    copy->phrases.entries = r->phrases.count ? phrases : NULL;
    copy->vocals.frames = r->vocals.count ? vocals : NULL;
}
static int ar_copy_result(const struct ar_job *job, az_analysis_result_v1 *out,
                           az_analysis_phrase_v1 *phrases, uint32_t pc,
                           az_vocal_frame_v1 *vocals, uint32_t vc) {
    const az_analysis_result_v1 *r = NULL;
    int rc = ar_result_ready(job, &r);
    if (rc) return rc;
    az_analysis_result_v1 copy = *r;
    copy.phrases.entries = NULL;
    copy.vocals.frames = NULL;
    if (pc < r->phrases.count || vc < r->vocals.count) { *out = copy; return AZ_LIMIT; }
    ar_copy_arrays(r, &copy, phrases, vocals);
    *out = copy;
    return AZ_OK;
}
static int ar_result_arguments(uint64_t id, const az_analysis_result_v1 *out,
                                const az_analysis_phrase_v1 *phrases, uint32_t pc,
                                const az_vocal_frame_v1 *vocals, uint32_t vc) {
    return id && out && out->size >= sizeof *out && (!pc || phrases) && (!vc || vocals);
}
int analysis_runtime_result(az_analysis_owner owner, uint64_t id, az_analysis_result_v1 *out,
                             az_analysis_phrase_v1 *phrases, uint32_t pc,
                             az_vocal_frame_v1 *vocals, uint32_t vc) {
    if (!ar_result_arguments(id, out, phrases, pc, vocals, vc)) return AZ_INVALID;
    if (pthread_mutex_trylock(&ar.mutex)) return AZ_BUSY;
    struct ar_job *job = ar_find(owner, id);
    int rc = job && !job->released ? ar_copy_result(job, out, phrases, pc, vocals, vc) : AZ_STALE;
    pthread_mutex_unlock(&ar.mutex);
    return rc;
}
int analysis_runtime_capabilities(az_analysis_capabilities_v1 *out) {
    if (!out || out->size < sizeof *out) return AZ_INVALID;
    *out = (az_analysis_capabilities_v1){.size = sizeof *out, .version = AZ_DATA_VERSION,
        .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS,
        .stage_modes = (1u << AZ_ANALYSIS_SEQUENTIAL) | (1u << AZ_ANALYSIS_CONCURRENT),
        .max_workers = 3, .max_requests = 8, .max_owner_requests = 4};
    return AZ_OK;
}
void analysis_runtime_enable(az_analysis_owner owner, int enabled) {
    pthread_mutex_lock(&ar.mutex);
    ar.enabled = !!enabled;
    ar.automatic_owner = owner;
    for (unsigned i = 0; i < 4; i++) {
        ar.decks[i].attempted = 0;
        struct ar_job *job = &ar.jobs[ANALYSIS_PUBLIC_SLOTS + i];
        if (job->status.request_id) ar_revoke(job, 1);
    }
    ar.notification++;
    ar_signal();
    pthread_mutex_unlock(&ar.mutex);
}
void analysis_runtime_load(unsigned deck, uint64_t order, uint64_t epoch, int loaded, const az_track_ref *track) {
    if (deck >= 4) return;
    pthread_mutex_lock(&ar.mutex);
    struct ar_load *load = &ar.decks[deck];
    if (!order || order <= load->order) { pthread_mutex_unlock(&ar.mutex); return; }
    load->order = order;
    if (load->epoch != epoch || !loaded) {
        load->attempted = 0;
        struct ar_job *job = &ar.jobs[ANALYSIS_PUBLIC_SLOTS + deck];
        if (job->status.request_id) ar_revoke(job, 1);
    }
    load->epoch = epoch;
    load->loaded = loaded;
    load->track = track ? *track : (az_track_ref){0};
    ar.notification++;
    ar_signal();
    pthread_mutex_unlock(&ar.mutex);
}
int analysis_runtime_deck_status(unsigned deck, az_analysis_status_v1 *out) {
    if (deck >= 4 || !out || out->size < sizeof *out) return AZ_INVALID;
    if (pthread_mutex_trylock(&ar.mutex)) return AZ_BUSY;
    *out = ar.decks[deck].status;
    out->size = sizeof *out;
    out->version = AZ_DATA_VERSION;
    pthread_mutex_unlock(&ar.mutex);
    return AZ_OK;
}

void analysis_runtime_wake(void) {
    pthread_mutex_lock(&ar.mutex);
    ar.notification++;
    ar_signal();
    pthread_mutex_unlock(&ar.mutex);
}

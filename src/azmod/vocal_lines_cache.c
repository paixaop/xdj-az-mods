/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "vocal_lines.h"
#include "vocal_lines_intervals.h"
#include "vocal_lines_projection.h"
#include "analysis_source.h"
#include "analysis_native_adapter.h"
#include "../common/aztimestamp.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif

#define VL_CAPACITY 65536u
/* Control lock protects only short copied commands. No file read, sorting,
 * interval comparison/copy, or projection holds it. Paint never takes it. */
static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker_thread;
static int started;
static int policy_state; /* control_lock: 0 pending, 1 ready, -1 failed */
static _Atomic int running, enabled;
static unsigned gap_ms = 2000;
static struct {
    uint64_t order, epoch;
} wanted[4];
static _Atomic uint64_t tokens[4], revisions[4], published_owners[4];
struct vl_request {
    unsigned deck, gap;
    uint64_t token, epoch;
    az_analysis_owner owner;
};
struct vl_result {
    uint32_t count, rate;
};
static struct vl_entry {
    struct vl_request request;
    struct vl_result result;
    az_vocal_interval intervals[VL_CAPACITY];
} entries[4];
static az_vocal_interval scratch[VL_CAPACITY]; /* sole worker */

static void invalidate(unsigned deck) {
    atomic_fetch_add(&tokens[deck], 1);
    atomic_fetch_add(&revisions[deck], 1);
}
static void invalidate_all(void) {
    for (unsigned deck = 0; deck < 4; deck++) invalidate(deck);
}
void vocal_lines_load(unsigned deck, uint64_t order, uint64_t epoch, int loaded) {
    if (deck >= 4 || !order) return;
    pthread_mutex_lock(&control_lock);
    if (order > wanted[deck].order) {
        wanted[deck].order = order;
        uint64_t next_epoch = loaded ? epoch : 0;
        if (wanted[deck].epoch != next_epoch) {
            wanted[deck].epoch = next_epoch;
            invalidate(deck);
            pthread_cond_signal(&changed);
        }
    }
    pthread_mutex_unlock(&control_lock);
}
void vocal_lines_config(unsigned gap, int on) {
    pthread_mutex_lock(&control_lock);
    if (gap_ms != gap || atomic_load(&enabled) != !!on) {
        gap_ms = gap;
        atomic_store(&enabled, !!on);
        invalidate_all();
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&control_lock);
}
static int request_current(const struct vl_request *request) {
    return request->epoch && atomic_load(&running) && atomic_load(&enabled) &&
           request->token == atomic_load(&tokens[request->deck]) && analysis_owner_current(request->owner);
}
static struct vl_request request_snapshot(unsigned deck) {
    pthread_mutex_lock(&control_lock);
    struct vl_request request = {.deck = deck, .gap = gap_ms, .epoch = wanted[deck].epoch, .token = atomic_load(&tokens[deck]), .owner = {0}};
    pthread_mutex_unlock(&control_lock);
    request.owner = analysis_owner_core();
    return request;
}
static int snapshot_valid(const az_vocal_snapshot_v1 *snapshot) {
    if (snapshot->size < sizeof *snapshot || snapshot->version != AZ_DATA_VERSION ||
        snapshot->interval_count > VL_CAPACITY || !snapshot->sample_rate) return 0;
    return snapshot->info.availability == (snapshot->interval_count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY);
}
static int interval_bounds(az_vocal_interval *interval, const az_vocal_snapshot_v1 *snapshot) {
    int order = compare_timestamps(interval->stop_s, snapshot->analyzed_s, 1, snapshot->sample_rate, 1);
    if (order == AZ_TIME_INVALID || order == AZ_TIME_AFTER) return 0;
    if (interval->stop_s > snapshot->analyzed_s) interval->stop_s = snapshot->analyzed_s;
    return 1;
}
static int snapshot_bounds(const az_vocal_snapshot_v1 *snapshot) {
    int order = compare_timestamps(snapshot->analyzed_s, 0, 1, snapshot->sample_rate, 1);
    if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return 0;
    for (unsigned i = 0; i < snapshot->interval_count; i++)
        if (!interval_bounds(&scratch[i], snapshot)) return 0;
    return 1;
}
static struct vl_result read_intervals(const struct vl_request *request) {
    az_vocal_snapshot_v1 snapshot = {.size = sizeof snapshot};
    struct vl_result result = {0};
    int rc = analysis_vocal_intervals_loaded(request->owner, (int) request->deck, request->epoch,
                                             &snapshot, scratch, VL_CAPACITY);
    if (rc != AZ_OK || !snapshot_valid(&snapshot) || !snapshot_bounds(&snapshot)) return result;
    if (vocal_lines_merge(scratch, snapshot.interval_count, snapshot.sample_rate,
                          request->gap / 1000.0, &result.count) != AZ_OK) return result;
    result.rate = snapshot.sample_rate;
    return result;
}
static int same_interval(az_vocal_interval a, az_vocal_interval b, uint32_t rate) {
    int first = compare_timestamps(a.start_s, b.start_s, 1, rate, 1);
    int last = compare_timestamps(a.stop_s, b.stop_s, 1, rate, 1);
    return first != AZ_TIME_INVALID && first == AZ_TIME_EQUAL &&
           last != AZ_TIME_INVALID && last == AZ_TIME_EQUAL;
}
static int same_result(const struct vl_entry *entry, const struct vl_request *request,
                       struct vl_result result) {
    if (entry->request.token != request->token || entry->request.owner.generation != request->owner.generation ||
        entry->result.count != result.count || entry->result.rate != result.rate) return 0;
    for (unsigned i = 0; i < result.count; i++)
        if (!same_interval(entry->intervals[i], scratch[i], result.rate)) return 0;
    return 1;
}
static void publish(const struct vl_request *request, struct vl_result result) {
    pthread_mutex_lock(&cache_lock);
    struct vl_entry *entry = &entries[request->deck];
    if (request_current(request) && !same_result(entry, request, result)) {
        memcpy(entry->intervals, scratch, result.count * sizeof *scratch);
        /* A load/configuration may change while copying. Its token invalidates
         * the old entry too; never commit a now-stale completion's metadata. */
        if (request_current(request)) {
            entry->request = *request;
            entry->result = result;
            atomic_store(&published_owners[request->deck], request->owner.generation);
            atomic_fetch_add(&revisions[request->deck], 1);
        }
    }
    pthread_mutex_unlock(&cache_lock);
}
static void poll_deck(unsigned deck) {
    struct vl_request request = request_snapshot(deck);
    if (!request_current(&request)) return;
    /* Provider revisions are stat fingerprints, not content identities. Compare
     * the actual consolidated answer before deciding whether to repaint. */
    struct vl_result result = read_intervals(&request);
    publish(&request, result);
}
static void pause_worker(void) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return;
    deadline.tv_sec++;
    pthread_mutex_lock(&control_lock);
    if (atomic_load(&running)) pthread_cond_timedwait(&changed, &control_lock, &deadline);
    pthread_mutex_unlock(&control_lock);
}
static int worker_policy(void) {
#ifdef __linux__
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(1, &cpus);
    CPU_SET(2, &cpus);
    CPU_SET(3, &cpus);
    return !sched_setaffinity(0, sizeof cpus, &cpus) && !setpriority(PRIO_PROCESS, 0, 19);
#else
    return 1;
#endif
}
static int publish_policy(int ready) {
    pthread_mutex_lock(&control_lock);
    policy_state = ready ? 1 : -1;
    if (!ready) atomic_store(&running, 0);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control_lock);
    return ready;
}
static int await_policy(void) {
    pthread_mutex_lock(&control_lock);
    while (!policy_state) pthread_cond_wait(&changed, &control_lock);
    int ready = policy_state > 0;
    pthread_mutex_unlock(&control_lock);
    return ready;
}
static void *worker(void *unused) {
    (void) unused;
    if (!publish_policy(worker_policy())) return NULL;
    while (atomic_load(&running)) {
        for (unsigned deck = 0; deck < 4 && atomic_load(&running); deck++) poll_deck(deck);
        pause_worker();
    }
    return NULL;
}
int vocal_lines_start(void) {
    if (started) return AZ_OK;
    pthread_mutex_lock(&control_lock);
    invalidate_all();
    policy_state = 0;
    atomic_store(&running, 1);
    pthread_mutex_unlock(&control_lock);
    if (az_analysis_thread_create(&worker_thread, NULL, worker, NULL)) {
        atomic_store(&running, 0);
        return AZ_FAILED;
    }
    started = 1;
    if (!await_policy()) {
        vocal_lines_stop();
        return AZ_FAILED;
    }
    return AZ_OK;
}
void vocal_lines_stop(void) {
    if (!started) return;
    pthread_mutex_lock(&control_lock);
    atomic_store(&running, 0);
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&control_lock);
    if (pthread_join(worker_thread, NULL)) abort();
    started = 0;
}
static int entry_current(unsigned deck, uint64_t epoch) {
    return epoch && entries[deck].request.epoch == epoch && request_current(&entries[deck].request);
}
int vocal_lines_pixels(unsigned deck, uint64_t epoch, double start, double stop,
                       int width, uint8_t out[VOCAL_LINES_WIDTH]) {
    if (!out) return 0;
    memset(out, 0, VOCAL_LINES_WIDTH);
    if (deck >= 4 || pthread_mutex_trylock(&cache_lock)) return 0;
    int visible = 0;
    if (entry_current(deck, epoch))
        visible = vocal_lines_project(entries[deck].intervals, entries[deck].result.count,
                                      entries[deck].result.rate, start, stop, width, out);
    if (!entry_current(deck, epoch)) {
        memset(out, 0, VOCAL_LINES_WIDTH);
        visible = 0;
    }
    pthread_mutex_unlock(&cache_lock);
    return visible;
}
uint32_t vocal_lines_sample_rate(unsigned deck, uint64_t epoch) {
    if (deck >= 4 || pthread_mutex_trylock(&cache_lock)) return 0;
    uint32_t rate = entry_current(deck, epoch) ? entries[deck].result.rate : 0;
    pthread_mutex_unlock(&cache_lock);
    return rate;
}
uint64_t vocal_lines_revision(unsigned deck) {
    if (deck >= 4 || !atomic_load(&running) || !atomic_load(&enabled)) return 0;
    az_analysis_owner owner = analysis_owner_core();
    if (!analysis_owner_current(owner) || atomic_load(&published_owners[deck]) != owner.generation) return 0;
    return atomic_load(&revisions[deck]);
}

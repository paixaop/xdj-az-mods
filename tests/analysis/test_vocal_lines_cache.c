/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#ifdef __linux__
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/resource.h>
static _Atomic int fail_affinity, fail_priority;
/* Fail the real production policy at its system-call boundaries. Success still
 * runs the target's actual affinity/priority calls on the worker thread. */
static int fixture_affinity(pid_t pid, size_t size, const cpu_set_t *cpus) {
    if (atomic_load(&fail_affinity)) {
        errno = EPERM;
        return -1;
    }
    return sched_setaffinity(pid, size, cpus);
}
static int fixture_priority(int which, id_t who, int value) {
    if (atomic_load(&fail_priority)) {
        errno = EPERM;
        return -1;
    }
    return setpriority(which, who, value);
}
#define sched_setaffinity fixture_affinity
#define setpriority fixture_priority
#endif
#include "../../src/azmod/vocal_lines_cache.c"
#ifdef __linux__
#undef sched_setaffinity
#undef setpriority
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
void analysis_fixture_fail_thread_start(void);
static az_vocal_snapshot_v1 reply;
static az_vocal_interval rows[4];
static unsigned row_count;
static int result_code, action;
static _Atomic unsigned calls;
static _Atomic int blocked, entered, stopped;
static uint64_t next_order;
static void pause_ms(void) {
    const struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}
int analysis_vocal_intervals_loaded(az_analysis_owner owner, int deck, uint64_t epoch,
                                    az_vocal_snapshot_v1 *out, az_vocal_interval *intervals, uint32_t capacity) {
    assert(owner.kind == AZ_ANALYSIS_OWNER_CORE && epoch && deck >= 0 && deck < 4);
    assert(out->size == sizeof *out && capacity == VL_CAPACITY);
    atomic_fetch_add(&calls, 1);
    *out = reply;
    memcpy(intervals, rows, row_count * sizeof *rows);
    atomic_store(&entered, 1);
    while (atomic_load(&blocked)) pause_ms();
    if (action == 1) vocal_lines_load((unsigned) deck, ++next_order, epoch + 1, 1);
    if (action == 2) vocal_lines_config(5000, 1);
    if (action == 3) analysis_owner_core_retire();
    if (action == 4) vocal_lines_load((unsigned) deck, ++next_order, epoch, 1);
    return result_code;
}
static void reset(void) {
    vocal_lines_stop();
    analysis_owner_core_retire();
    assert(analysis_owner_core_begin().generation);
    for (unsigned d = 0; d < 4; d++) vocal_lines_load(d, ++next_order, 0, 0);
    vocal_lines_config(1000, 1);
    atomic_store(&running, 1); /* Deterministic turns of the production worker. */
    reply = (az_vocal_snapshot_v1) {.size = sizeof reply, .version = AZ_DATA_VERSION, .info = {.availability = AZ_AVAIL_READY}, .sample_rate = 22050, .interval_count = 2, .analyzed_s = 10};
    rows[0] = (az_vocal_interval) {1, 2};
    rows[1] = (az_vocal_interval) {3, 4};
    row_count = 2;
    result_code = action = 0;
    atomic_store(&blocked, 0);
    atomic_store(&entered, 0);
    atomic_store(&calls, 0);
}
static int pixels(unsigned deck, uint64_t epoch, uint8_t *out) {
    return vocal_lines_pixels(deck, epoch, 0, 20, 100, out);
}
static void load(unsigned deck, uint64_t epoch) { vocal_lines_load(deck, ++next_order, epoch, 1); }
static void publication_and_order(void) {
    reset();
    load(0, 10);
    poll_deck(0);
    uint8_t out[VOCAL_LINES_WIDTH];
    assert(pixels(0, 10, out) && vocal_lines_sample_rate(0, 10) == 22050);
    for (unsigned x = 0; x < VOCAL_LINES_WIDTH; x++)
        assert(out[x] == ((x >= 5 && x < 10) || (x >= 15 && x < 20)));
    uint64_t revision = vocal_lines_revision(0);
    assert(revision);
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
    vocal_lines_config(1000, 1);
    assert(vocal_lines_revision(0) == revision);
    vocal_lines_load(0, next_order, 99, 1);
    vocal_lines_load(0, next_order - 1, 99, 1);
    vocal_lines_load(0, 0, 99, 1);
    vocal_lines_load(4, 1, 99, 1);
    assert(pixels(0, 10, out));
    load(0, 10); /* Newer receipt for the same native load preserves its answer. */
    assert(pixels(0, 10, out) && vocal_lines_revision(0) == revision);
    rows[0].start_s = 0;
    poll_deck(0);
    assert(vocal_lines_revision(0) > revision);
    vocal_lines_config(2000, 1);
    assert(!pixels(0, 10, out));
    assert(!vocal_lines_sample_rate(0, 10));
    poll_deck(0);
    assert(pixels(0, 10, out));
    for (unsigned x = 0; x < VOCAL_LINES_WIDTH; x++) assert(out[x] == (x < 20));
    pthread_mutex_lock(&cache_lock);
    assert(!pixels(0, 10, out) && !vocal_lines_sample_rate(0, 10));
    vocal_lines_load(0, ++next_order, 0, 0); /* Never waits for the paint/data lock. */
    pthread_mutex_unlock(&cache_lock);
    assert(!pixels(0, 10, out));
    poll_deck(0);
    assert(!pixels(4, 10, out));
    assert(!vocal_lines_sample_rate(4, 10));
    assert(!vocal_lines_revision(4));
    assert(!vocal_lines_pixels(0, 10, 0, 20, 100, NULL));
}
static void rejects_completion(void) {
    uint8_t out[VOCAL_LINES_WIDTH];
    for (int effect = 1; effect <= 3; effect++) {
        reset();
        load(1, 10);
        action = effect;
        poll_deck(1);
        assert(!pixels(1, 10, out) && !vocal_lines_sample_rate(1, 10));
        action = 0;
        if (effect == 3) assert(analysis_owner_core_begin().generation);
        poll_deck(1);
        assert(pixels(1, effect == 1 ? 11 : 10, out));
    }
    vocal_lines_config(1000, 0);
    assert(!vocal_lines_revision(1));
    assert(!pixels(1, 10, out));
    poll_deck(1);
}
static void same_epoch_during_read(void) {
    uint8_t out[VOCAL_LINES_WIDTH];
    reset();
    load(0, 10);
    action = 4;
    poll_deck(0); /* A newer LOAD receipt during IO must not discard this read. */
    assert(pixels(0, 10, out));
    uint64_t revision = vocal_lines_revision(0);
    poll_deck(0);
    assert(pixels(0, 10, out) && vocal_lines_revision(0) == revision);
    assert(wanted[0].order == next_order);
}
static void hide_bad_reply(void) {
    load(0, 10);
    poll_deck(0);
    uint8_t out[VOCAL_LINES_WIDTH];
    assert(!pixels(0, 10, out) && !memchr(out, 1, sizeof out));
}
static void invalid_snapshots(void) {
    const int errors[] = {AZ_BUSY, AZ_PENDING, AZ_STALE, AZ_NOT_FOUND, AZ_LIMIT, AZ_FAILED};
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; i++) {
        reset();
        result_code = errors[i];
        hide_bad_reply();
    }
    reset();
    reply.size--;
    hide_bad_reply();
    reset();
    reply.version++;
    hide_bad_reply();
    reset();
    reply.interval_count = VL_CAPACITY + 1;
    hide_bad_reply();
    reset();
    reply.sample_rate = 0;
    hide_bad_reply();
    reset();
    reply.info.availability = AZ_AVAIL_EMPTY;
    hide_bad_reply();
    reset();
    reply.info.availability = AZ_AVAIL_FAILED;
    hide_bad_reply();
    reset();
    reply.analyzed_s = NAN;
    hide_bad_reply();
    reset();
    reply.analyzed_s = 0;
    hide_bad_reply();
    reset();
    rows[1].stop_s = 11;
    hide_bad_reply();
    reset();
    rows[1].stop_s = NAN;
    hide_bad_reply();
    reset();
    rows[1].start_s = NAN;
    hide_bad_reply();
    reset();
    rows[1].start_s = 4;
    hide_bad_reply();
    reset();
    rows[1].start_s = -1;
    hide_bad_reply();
    reset();
    row_count = reply.interval_count = 0;
    reply.info.availability = AZ_AVAIL_EMPTY;
    hide_bad_reply();
    uint64_t revision = vocal_lines_revision(0);
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
}
static void source_clocks(void) {
    const uint32_t rates[] = {22050, 44100, 48000, 96000};
    uint8_t out[VOCAL_LINES_WIDTH];
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++) {
        reset();
        reply.sample_rate = rates[i];
        rows[1].stop_s = 10 + 0.5 / rates[i];
        load(0, 10);
        poll_deck(0);
        assert(pixels(0, 10, out) && vocal_lines_sample_rate(0, 10) == rates[i]);
        assert(entries[0].intervals[1].stop_s <= reply.analyzed_s);
        assert(!out[50]); /* Metadata duration20s, analysis10s must not stretch. */
        assert(vocal_lines_pixels(0, 10, -5, 15, 100, out));
        assert(!out[0] && out[30]);
    }
}
static void provider_revision_cache(void) {
    reset();
    reply.info.revision = 123;
    load(0, 10);
    poll_deck(0);
    uint64_t revision = vocal_lines_revision(0);
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
    /* The file's stat tuple can stay unchanged across an in-place rewrite. */
    rows[0].start_s = 0;
    poll_deck(0);
    assert(vocal_lines_revision(0) > revision);
    uint8_t out[VOCAL_LINES_WIDTH];
    assert(pixels(0, 10, out) && out[0]);
    revision = vocal_lines_revision(0);
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
    reply.info.revision++;
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
    poll_deck(0);
    assert(vocal_lines_revision(0) == revision);
    vocal_lines_config(2000, 1);
    poll_deck(0);
    assert(pixels(0, 10, out));
    for (unsigned x = 5; x < 20; x++) assert(out[x]);
    analysis_owner_core_retire();
    assert(analysis_owner_core_begin().generation);
    assert(!pixels(0, 10, out));
    poll_deck(0);
    assert(pixels(0, 10, out));
}
static void wait_visible(unsigned deck, uint64_t epoch) {
    uint8_t out[VOCAL_LINES_WIDTH];
    for (unsigned i = 0; i < 4000; i++) {
        if (pixels(deck, epoch, out)) return;
        pause_ms();
    }
    assert(!"vocal worker failed to publish");
}
static void wait_entered(void) {
    for (unsigned i = 0; i < 4000; i++) {
        if (atomic_load(&entered)) return;
        pause_ms();
    }
    assert(!"vocal worker did not start source read");
}
static void *stop_thread(void *unused) {
    (void) unused;
    vocal_lines_stop();
    atomic_store(&stopped, 1);
    return NULL;
}
static void lifecycle(void) {
    reset();
    atomic_store(&running, 0);
    analysis_fixture_fail_thread_start();
    assert(vocal_lines_start() == AZ_FAILED);
    assert(!vocal_lines_revision(0));
    vocal_lines_stop();
    for (unsigned deck = 0; deck < 4; deck++) load(deck, 10 + deck);
    atomic_store(&blocked, 1);
    atomic_store(&entered, 0);
    assert(vocal_lines_start() == AZ_OK && vocal_lines_start() == AZ_OK);
    wait_entered();
    load(0, 20);
    vocal_lines_config(2000, 1);
    analysis_owner_core_retire();
    assert(!vocal_lines_revision(0));
    assert(analysis_owner_core_begin().generation); /* No new load event. */
    atomic_store(&blocked, 0);
    wait_visible(0, 20);
    for (unsigned deck = 1; deck < 4; deck++) wait_visible(deck, 10 + deck);
    atomic_store(&blocked, 1);
    atomic_store(&entered, 0);
    vocal_lines_config(3000, 1);
    wait_entered();
    pthread_t stop;
    atomic_store(&stopped, 0);
    assert(!pthread_create(&stop, NULL, stop_thread, NULL));
    while (atomic_load(&running)) pause_ms();
    assert(!atomic_load(&stopped));
    uint8_t out[VOCAL_LINES_WIDTH];
    assert(!pixels(0, 20, out));
    atomic_store(&blocked, 0);
    assert(!pthread_join(stop, NULL));
    assert(atomic_load(&stopped));
    vocal_lines_stop();
    assert(vocal_lines_start() == AZ_OK);
    wait_visible(0, 20);
    vocal_lines_stop();
}
static void scheduling_failures(void) {
#ifdef __linux__
    reset();
    atomic_store(&running, 0);
    load(0, 10);
    atomic_store(&fail_affinity, 1);
    assert(vocal_lines_start() == AZ_FAILED);
    assert(!started && !atomic_load(&running) && !atomic_load(&calls));
    atomic_store(&fail_affinity, 0);
    atomic_store(&fail_priority, 1);
    assert(vocal_lines_start() == AZ_FAILED);
    assert(!started && !atomic_load(&running) && !atomic_load(&calls));
    atomic_store(&fail_priority, 0);
    assert(vocal_lines_start() == AZ_OK);
    wait_visible(0, 10);
    vocal_lines_stop();
#endif
}
int main(void) {
    publication_and_order();
    rejects_completion();
    same_epoch_during_read();
    invalid_snapshots();
    source_clocks();
    provider_revision_cache();
    lifecycle();
    scheduling_failures();
    puts("PASS core vocal cache ordered loads, stale owners/config, clocks and worker lifecycle");
    return 0;
}

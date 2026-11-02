/* SPDX-License-Identifier: GPL-3.0-only */
/* Scalar diagnostic regression against a synthetic OWN helper, never EP147. */
#define _GNU_SOURCE
#include <time.h>
static int probe_clock_gettime(clockid_t, struct timespec *);
static int probe_clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
#define clock_gettime probe_clock_gettime
#define clock_nanosleep probe_clock_nanosleep
#ifndef PROBE_INC
#define PROBE_INC "../integration/sdk-source-probe.c"
#endif
#define AZSDK_SOURCE_PROBE_TEST
#include PROBE_INC
#undef clock_gettime
#undef clock_nanosleep
#include <assert.h>
#include <limits.h>
static int fake_clock;
static uint64_t fake_ns, query_cost, startup_cost;
static unsigned query_count, expected_media, clock_reads, fail_clock_at, backward_query;
static unsigned sleep_calls, interrupt_sleeps;
static int overflow_clock, invalid_candidate;
static int probe_clock_gettime(clockid_t id, struct timespec *out) {
    if (!fake_clock) return clock_gettime(id, out);
    assert(id == CLOCK_MONOTONIC);
    if (++clock_reads == fail_clock_at) { errno = EIO; return -1; }
    out->tv_sec = (time_t)(overflow_clock ? UINT64_MAX / UINT64_C(1000000000) + 1 :
                           fake_ns / UINT64_C(1000000000));
    out->tv_nsec = (long)(fake_ns % UINT64_C(1000000000));
    return 0;
}
static int probe_clock_nanosleep(clockid_t id, int flags, const struct timespec *until,
                                 struct timespec *remaining) {
    if (!fake_clock) return clock_nanosleep(id, flags, until, remaining);
    assert(id == CLOCK_MONOTONIC && flags == TIMER_ABSTIME && !remaining);
    ++sleep_calls;
    if (interrupt_sleeps) { --interrupt_sleeps; return EINTR; }
    uint64_t ns = (uint64_t)until->tv_sec * UINT64_C(1000000000) + (uint64_t)until->tv_nsec;
    if (ns > fake_ns) fake_ns = ns;
    return 0;
}
static int timed_candidate(uint16_t media, void *out, size_t size) {
    (void)out;
    assert(size == sizeof(struct azsdk_source_sample));
    assert(media && media == expected_media);
    if (++expected_media > UINT16_MAX) expected_media = 1;
    ++query_count;
    if (backward_query && query_count == backward_query) --fake_ns;
    else { assert(query_cost <= UINT64_MAX - fake_ns); fake_ns += query_cost; }
    if (invalid_candidate && media == 1) {
        struct azsdk_source_sample *sample = out;
        if (invalid_candidate == 2) {
            sample->reserved = 1; /* BUSY must not modify caller storage. */
            return AZS_BUSY;
        }
        memset(sample, 0, sizeof *sample);
        sample->struct_size = sizeof *sample;
        sample->flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE;
        sample->owner_epoch = 9; sample->revision = 2;
        sample->observed_ns = fake_ns + 1; /* One-nanosecond future record. */
        sample->native_media_id = media;
        sample->physical_status = AZS_PHYSICAL_DISABLED;
        return AZS_OK;
    }
    return AZS_UNKNOWN; /* Failure/status paths must leave the scalar untouched. */
}
static void reset_fake_scan(void) {
    fake_clock = 1;
    fake_ns = UINT64_C(1000000000);
    query_cost = startup_cost = 0;
    query_count = clock_reads = sleep_calls = interrupt_sleeps = 0;
    fail_clock_at = backward_query = 0;
    expected_media = 1;
    overflow_clock = invalid_candidate = 0;
}
static char *run_fake_scan(void) {
    FILE *file = tmpfile();
    assert(file);
    struct source_log log = {file, 0, 0};
    struct source_budget budget;
    (void)source_budget_start(&budget);
    assert(startup_cost <= UINT64_MAX - fake_ns);
    fake_ns += startup_cost;
    source_scan(timed_candidate, &log, &budget);
    assert(!fflush(file));
    long bytes = ftell(file);
    assert(bytes > 0 && (unsigned long)bytes <= SOURCE_LOG_LIMIT);
    char *text = calloc((size_t)bytes + 1, 1);
    assert(text);
    rewind(file);
    assert(fread(text, 1, (size_t)bytes, file) == (size_t)bytes);
    fclose(file);
    return text;
}
static void test_invalid_record_evidence(void) {
    for (int kind = 1; kind <= 2; ++kind) {
        reset_fake_scan(); invalid_candidate = kind;
        char *text = run_fake_scan();
        unsigned records = 0;
        for (char *at = text; (at = strstr(at, "source_invalid ")) != NULL; ++at) ++records;
        assert(records == 8 && "bounded invalid candidate evidence must be retained");
        assert(strstr(text, "media=0x0001") && strstr(text, "now_ns="));
        assert(strstr(text, kind == 1 ? "result=1" : "result=-3"));
        assert(strstr(text, kind == 1 ? "error_output_changed=0" : "error_output_changed=1"));
        assert(strstr(text, "invalid=1") && strstr(text, "complete status=time_limit"));
        free(text);
    }
    fake_clock = 0;
}
static void test_scan_walltime(void) {
    reset_fake_scan();
    query_cost = UINT64_C(1000000); /* One ms of actual work per scalar query. */
    uint64_t start = fake_ns;
    char *text = run_fake_scan();
    assert(fake_ns - start <= UINT64_C(300000000000) + query_cost);
    assert(query_count && query_count < 3000u * SOURCE_BATCH);
    assert(strstr(text, "complete status=time_limit"));
    free(text);
    reset_fake_scan();
    startup_cost = UINT64_C(200000000000); /* Resolution/logging are inside the same budget. */
    query_cost = UINT64_C(1000000);
    start = fake_ns;
    text = run_fake_scan();
    assert(fake_ns - start <= UINT64_C(300000000000) + query_cost);
    assert(query_count && query_count < UINT16_MAX * 2u);
    assert(strstr(text, "complete status=time_limit"));
    free(text);
    reset_fake_scan();
    query_cost = UINT64_C(100000); /* Deadline falls inside the final cadence sleep. */
    start = fake_ns;
    text = run_fake_scan();
    assert(fake_ns - start == UINT64_C(300000000000));
    assert(strstr(text, "complete status=time_limit"));
    free(text);
    reset_fake_scan();
    text = run_fake_scan();
    assert(query_count >= UINT16_MAX && expected_media == query_count % UINT16_MAX + 1);
    assert(strstr(text, "scan pass=1 queried=65535"));
    assert(strstr(text, "complete status=time_limit"));
    free(text);
    reset_fake_scan();
    fail_clock_at = 1;
    text = run_fake_scan();
    assert(!query_count && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    fail_clock_at = 3; /* Initial, pre-query, then failed post-query timestamp. */
    text = run_fake_scan();
    assert(query_count == 1 && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    fake_ns = 0;
    text = run_fake_scan();
    assert(!query_count && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    fake_ns = UINT64_MAX - UINT64_C(150000000000);
    text = run_fake_scan();
    assert(!query_count && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    overflow_clock = 1;
    text = run_fake_scan();
    assert(!query_count && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    backward_query = 2;
    text = run_fake_scan();
    assert(query_count == 2 && strstr(text, "complete status=clock_refused"));
    free(text);
    reset_fake_scan();
    interrupt_sleeps = 4;
    text = run_fake_scan();
    assert(sleep_calls == 4 && strstr(text, "complete status=cadence_refused"));
    free(text);
    fake_clock = 0;
}
int main(int argc, char **argv) {
    assert(argc == 6);
    void *good = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    void *bad_name = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    assert(good && bad_name);
    void *anchor = dlsym(good, "azmod_dm_key");
    void *wrong_anchor = dlsym(good, "source_fake_calls");
    void *renamed_anchor = dlsym(bad_name, "azmod_dm_key");
    unsigned (*calls)(void), (*anchors)(void), (*renamed_calls)(void);
    *(void **) &calls = dlsym(good, "source_fake_calls");
    *(void **) &anchors = dlsym(good, "source_fake_anchors");
    *(void **) &renamed_calls = dlsym(bad_name, "source_fake_calls");
    assert(anchor && wrong_anchor && renamed_anchor && calls && anchors && renamed_calls);
    int64_t delta = strtoll(argv[4], NULL, 10), data_delta = strtoll(argv[5], NULL, 10);
    uintptr_t answer = 0;
    assert(diagnostic_delta(100, 1, &answer) && answer == 101);
    assert(diagnostic_delta(100, -1, &answer) && answer == 99);
    assert(!diagnostic_delta(2, -3, &answer));
    assert(!diagnostic_delta(UINTPTR_MAX - 1, 2, &answer));
    assert(!diagnostic_delta(0, INT64_MIN, &answer));
    assert(diagnostic_delta(UINTPTR_MAX, INT64_MIN, &answer) && answer == INT64_MAX);
    char wrong_id[129];
    assert(strlen(argv[3]) < sizeof wrong_id);
    strcpy(wrong_id, argv[3]);
    wrong_id[0] = wrong_id[0] == '0' ? '1' : '0';
    assert(!diagnostic_resolve(anchor, wrong_id, delta));
    assert(!diagnostic_resolve(anchor, "", delta));
    assert(!diagnostic_resolve(anchor, "f", delta));
    assert(!diagnostic_resolve(anchor, "xx", delta));
    assert(!diagnostic_resolve(renamed_anchor, argv[3], delta));
    assert(!diagnostic_resolve(wrong_anchor, argv[3], delta));
    assert(!diagnostic_resolve(anchor, argv[3], 0));
    assert(!diagnostic_resolve(anchor, argv[3], INT64_MIN));
    assert(!diagnostic_resolve(anchor, argv[3], INT64_MAX));
    assert(!diagnostic_resolve(anchor, argv[3], data_delta));
#if defined(__aarch64__)
    assert(!diagnostic_resolve(anchor, argv[3], delta + 1));
#endif
    assert(!calls() && !anchors() && !renamed_calls());
    source_candidate_fn read = diagnostic_resolve(anchor, argv[3], delta);
    assert(read && !calls() && !anchors());
    struct azsdk_source_sample value;
    assert(read(0x123, &value, sizeof value) == AZS_OK && calls() == 1);
    assert(source_record_valid(AZS_OK, 0x123, &value, 200));
    assert(!source_record_valid(AZS_OK, 0x124, &value, 200));
    assert(!source_record_valid(AZS_OK, 0x123, &value, 99));
    assert(!source_record_valid(AZS_OK, 0x123, &value, 0));
    struct azsdk_source_sample invalid = value;
    invalid.reserved = 1;
    assert(!source_record_valid(AZS_OK, 0x123, &invalid, 200));
    invalid = value;
    invalid.flags |= AZS_PHYSICAL;
    assert(!source_record_valid(AZS_OK, 0x123, &invalid, 200));
    invalid.physical_status = AZS_PHYSICAL_OK;
    invalid.inode = 7;
    invalid.mount_id = 4;
    invalid.physical_observed_ns = 150;
    assert(source_record_valid(AZS_OK, 0x123, &invalid, 200));
    invalid.physical_observed_ns = 201;
    assert(!source_record_valid(AZS_OK, 0x123, &invalid, 200));
    FILE *file = tmpfile();
    assert(file);
    struct source_log log = {file, 0, 0};
    struct source_history history[SOURCE_HISTORY] = {{0}};
    assert(source_changed(&log, history, 0x123, AZS_OK, &value));
    size_t first = log.bytes;
    assert(first && source_changed(&log, history, 0x123, AZS_OK, &value) && log.bytes == first);
    ++value.revision;
    assert(source_changed(&log, history, 0x123, AZS_OK, &value) && log.bytes > first);
    assert(source_changed(&log, history, 0x123, AZS_UNKNOWN, NULL));
    first = log.bytes;
    assert(source_changed(&log, history, 0x123, AZS_UNKNOWN, NULL) && log.bytes == first);
    memset(&value, 0, sizeof value);
    value.struct_size = sizeof value;
    value.native_media_id = 0x123;
    value.flags = AZS_HISTORY_INCOMPLETE | AZS_MEDIA_AMBIGUOUS;
    assert(source_record_valid(AZS_AMBIGUOUS, 0x123, &value, 200));
    assert(source_changed(&log, history, 0x123, AZS_AMBIGUOUS, &value));
    for (unsigned i = 0; i < SOURCE_HISTORY; ++i) history[i].media = (uint16_t) (i + 1);
    first = log.bytes;
    assert(!source_changed(&log, history, 0x123, AZS_AMBIGUOUS, &value) && first == log.bytes);
    log.bytes = SOURCE_LOG_LIMIT - SOURCE_LOG_RESERVE - 3;
    long position = ftell(file);
    assert(!source_line(&log, "longer-than-the-remaining-budget\n") && log.stopped && ftell(file) == position);
    assert(!source_line(&log, "x") && ftell(file) == position);
    rewind(file);
    char text[4096] = {0};
    assert(fread(text, 1, sizeof text - 1, file));
    assert(strstr(text, "status=current media=0x0123 epoch=9"));
    assert(strstr(text, "status=unknown media=0x0123 previous_epoch=9"));
    assert(strstr(text, "status=ambiguous media=0x0123 epoch=0"));
    assert(strstr(text, "history_incomplete=1 loaded_source_valid=0"));
    fclose(file);
    test_invalid_record_evidence();
    test_scan_walltime();
    struct source_budget real_budget;
    assert(source_now() && source_budget_start(&real_budget) && source_delay(&real_budget) == 1);
    assert(!anchors() && !renamed_calls());
    dlclose(bad_name);
    dlclose(good);
    puts("source probe: ID/name/RX/delta refusals, scalar guards/caps, full media scan, walltime, clock/overflow and interrupted cadence passed");
}

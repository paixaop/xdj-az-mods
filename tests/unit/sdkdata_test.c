/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef ST_ENGINE_HEADER
#define ST_ENGINE_HEADER "sdk_data_engine.h"
#endif
#include ST_ENGINE_HEADER
static int file_seconds_equal(double actual, double expected, uint64_t clock_rate) {
    int order = compare_timestamps(actual, expected, 1, clock_rate, 1);
    assert(order != AZ_TIME_INVALID);
    return order == AZ_TIME_EQUAL;
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static void put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
static size_t section(uint8_t *p, const char *tag, size_t header, size_t n) {
    memset(p, 0, n);
    memcpy(p, tag, 4);
    put32(p + 4, (uint32_t) header);
    put32(p + 8, (uint32_t) n);
    return n;
}
static uint8_t fixture[1024];
static size_t make_fixture(void) {
    size_t n = section(fixture, "PMAI", 28, 28);
    uint8_t *p = fixture + n;
    n += section(p, "PQTZ", 24, 24 + 3 * 8);
    put32(p + 20, 3);
    for (int i = 0; i < 3; i++) {
        put16(p + 24 + i * 8, i + 1);
        put16(p + 26 + i * 8, i == 2 ? 6000 : 12000);
        put32(p + 28 + i * 8, i == 2 ? 1500 : i * 500);
    }
    p = fixture + n;
    n += section(p, "PCO2", 20, 20 + 52);
    put32(p + 12, 1);
    put16(p + 16, 1);
    uint8_t *e = p + 20;
    section(e, "PCP2", 16, 52);
    put32(e + 12, 1);
    e[16] = 2;
    put32(e + 20, 0);
    put32(e + 24, 1000);
    put32(e + 40, 4);
    put16(e + 44, 'A');
    put16(e + 46, 0);
    e[48] = 1;
    e[49] = 0x12;
    e[50] = 0x34;
    e[51] = 0x56;
    p = fixture + n;
    n += section(p, "PCOB", 24, 24 + 56);
    put32(p + 12, 0);
    put16(p + 18, 1);
    e = p + 24;
    section(e, "PCPT", 28, 56);
    e[28] = 1;
    put32(e + 32, 0);
    put32(e + 36, UINT32_MAX);
    p = fixture + n;
    n += section(p, "PSSI", 32, 32 + 24);
    put32(p + 12, 24);
    put16(p + 16, 1);
    put16(p + 18, 2);
    put16(p + 26, 3);
    put16(p + 34, 1);
    put16(p + 36, 7);
    p = fixture + n;
    n += section(p, "PWAV", 20, 24);
    put32(p + 12, 4);
    p[20] = 1;
    p[21] = 2;
    p[22] = 3;
    p[23] = 4;
    put32(fixture + 8, (uint32_t) n);
    return n;
}
static void parser(void) {
    size_t n = make_fixture();
    const uint8_t *b[2] = {fixture, NULL};
    size_t sizes[2] = {n, 0};
    struct st_data *d = malloc(sizeof *d);
    assert(d);
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, 31) == AZ_OK);
    assert(d->view.cue_count == 2 && file_seconds_equal(d->cues[0].start_s, 0, 1000) && file_seconds_equal(d->cues[1].start_s, 0, 1000));
    assert(d->cues[0].type == AZ_CUE_LOOP && file_seconds_equal(d->cues[0].end_s, 1, 1000));
    assert(d->cues[0].rgb == 0x123456 && !strcmp(d->cues[0].comment, "A"));
    assert(!(d->cues[0].valid & AZ_CUE_AUTO_ACTIVE) && d->cues[0].auto_active == -1);
    assert(d->cues[1].classification == AZ_CUE_MEMORY && d->cues[1].type == AZ_CUE_POINT);
    assert(d->view.phrase_count == 1 && d->phrases[0].raw_kind == 7 && d->phrases[0].raw_mood == 2);
    assert(d->phrases[0].start_beat == 0 && d->phrases[0].end_beat == 2);
    assert(d->view.waveform.sample_count == 4 && d->wave[3] == 4);
    double beat, time;
    assert(st_time_to_beat(&d->view, 1, &beat) == AZ_OK && beat == 1.5);
    assert(st_beat_to_time(&d->view, beat, &time) == AZ_OK && file_seconds_equal(time, 1, 1000));
    assert(st_time_to_beat(&d->view, 0, &beat) == AZ_OK && beat == 0);
    assert(st_time_to_beat(&d->view, 1.6, &beat) == AZ_NOT_FOUND);
    assert(st_beat_to_time(&d->view, NAN, &time) == AZ_INVALID);
    az_time_interval v = {.size = sizeof v};
    assert(st_interval(&d->view, .25, 1, 2, &v) == AZ_OK && v.beats == 1 && file_seconds_equal(v.source_s, .75, 1000) && file_seconds_equal(v.playback_s, .375, 2000));
    assert(!(v.valid & AZ_INTERVAL_BARS));
    assert(st_interval(&d->view, 0, 1, 1e-320, &v) == AZ_OK && !(v.valid & AZ_INTERVAL_PLAYBACK));
    assert(st_interval(&d->view, 0, 1, 0, &v) == AZ_OK && !(v.valid & AZ_INTERVAL_PLAYBACK));
    for (size_t i = 0; i < n; i++) {
        sizes[0] = i;
        st_data_init(d);
        assert(st_parse_pair(d, b, sizes, 31) < 0);
    }
    sizes[0] = n;
    /* A claimed count that exceeds its section and a trailing partial tag both fail. */
    put32(fixture + 28 + 20, UINT32_MAX);
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, 31) < 0);
    n = make_fixture();
    put32(fixture + 8, (uint32_t) n + 1);
    sizes[0] = n + 1;
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, 31) < 0);
    /* An explicit empty list is empty, a file without that tag is unsupported. */
    section(fixture, "PMAI", 28, 48);
    section(fixture + 28, "PCO2", 20, 20);
    put32(fixture + 40, 1);
    sizes[0] = 48;
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, AZ_DATA_CUES) == AZ_OK);
    assert(d->view.cues_info.availability == AZ_AVAIL_EMPTY);
    section(fixture, "PMAI", 28, 28);
    sizes[0] = 28;
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, AZ_DATA_CUES) == AZ_OK && d->view.cues_info.availability == AZ_AVAIL_UNSUPPORTED);
    section(fixture, "PMAI", 28, 60);
    section(fixture + 28, "PSSI", 32, 32);
    put32(fixture + 40, 24);
    sizes[0] = 60;
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, AZ_DATA_PHRASES) == AZ_OK);
    assert(d->view.phrases_info.availability == AZ_AVAIL_EMPTY);
    /* Legacy hot slots can be split across the pair; the second file is not a
     * duplicate classification to discard. Memory entries keep source precedence. */
    uint8_t ext[108];
    section(fixture, "PMAI", 28, 108);
    section(fixture + 28, "PCOB", 24, 80);
    put32(fixture + 40, 1);
    put16(fixture + 46, 1);
    section(fixture + 52, "PCPT", 28, 56);
    put32(fixture + 64, 1);
    fixture[80] = 1;
    memcpy(ext, fixture, sizeof ext);
    put32(ext + 64, 8);
    put32(ext + 84, 500);
    b[1] = ext;
    sizes[0] = sizes[1] = 108;
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, AZ_DATA_CUES) == AZ_OK && d->view.cue_count == 2);
    assert(d->cues[0].slot == 1 && d->cues[1].slot == 8);
    put32(fixture + 40, 0);
    put32(ext + 40, 0);
    st_data_init(d);
    assert(st_parse_pair(d, b, sizes, AZ_DATA_CUES) == AZ_OK && d->view.cue_count == 1);
    free(d);
}
static void timing_envelopes(void) {
    /* Real short allocations catch a read beyond the caller's versioned prefix under
     * ASan. Reject before looking at grid availability or any array pointer. */
    double out = 123;
    az_time_interval interval = {.size = sizeof interval};
    uint32_t *prefix = malloc(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    assert(st_time_to_beat((const az_track_snapshot *) prefix, 0, &out) == AZ_INVALID && out == 123);
    assert(st_beat_to_time((const az_track_snapshot *) prefix, 0, &out) == AZ_INVALID && out == 123);
    assert(st_interval((const az_track_snapshot *) prefix, 0, 1, 1, &interval) == AZ_INVALID);
    free(prefix);
    for (unsigned words = 1; words <= 2; words++) {
        uint32_t *short_snapshot = malloc(words * sizeof *short_snapshot);
        assert(short_snapshot);
        short_snapshot[0] = words * sizeof *short_snapshot;
        if (words == 2) short_snapshot[1] = AZ_DATA_VERSION;
        assert(st_time_to_beat((const az_track_snapshot *) short_snapshot, 0, &out) == AZ_INVALID && out == 123);
        assert(st_beat_to_time((const az_track_snapshot *) short_snapshot, 0, &out) == AZ_INVALID && out == 123);
        assert(st_interval((const az_track_snapshot *) short_snapshot, 0, 1, 1, &interval) == AZ_INVALID);
        free(short_snapshot);
    }
    az_beat beats[] = {{0, 120, 1, 0}, {.5, 120, 2, 0}, {1.5, 60, 3, 0}};
    az_track_snapshot s = {.size = sizeof s, .version = AZ_DATA_VERSION + 1, .grid_info = {.availability = AZ_AVAIL_READY}, .beat_count = 3, .beats = beats};
    assert(st_time_to_beat(&s, 1, &out) == AZ_INVALID && out == 123);
    assert(st_beat_to_time(&s, 1, &out) == AZ_INVALID && out == 123);
    assert(st_interval(&s, 0, 1, 1, &interval) == AZ_INVALID);
    s.version = AZ_DATA_VERSION;
    s.size += 32; /* A newer, compatible record prefix remains usable. */
    assert(st_time_to_beat(&s, 1, &out) == AZ_OK && out == 1.5);
    assert(st_beat_to_time(&s, 1.5, &out) == AZ_OK && file_seconds_equal(out, 1, 1000));
    assert(st_interval(NULL, 0, 1, 2, &interval) == AZ_OK && file_seconds_equal(interval.playback_s, .5, 2000) && !(interval.valid & AZ_INTERVAL_BEATS));
}
/* Cross-DLL callers provide an opaque allocation. Keep the deliberately short
 * heap object real for ASan without GCC7's inlined fortify false positives. */
__attribute__((noinline)) static void *request_prefix_buffer(size_t size) { return malloc(size); }
static void request_envelopes(void) {
    struct st_engine e = ST_ENGINE_INIT;
    uint32_t *prefix = request_prefix_buffer(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    az_track_ref ref = {sizeof ref, 1, 1, 1, 9, 42, 0};
    az_wave_query query = {sizeof query, 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    uint64_t ticket = UINT64_MAX;
    assert(st_request(&e, 0, (const az_track_ref *) prefix, 1, &ticket) == AZ_INVALID);
    assert(st_request_with_wave(&e, 0, (const az_track_ref *) prefix, ST_DETAIL, &query, &ticket) == AZ_INVALID);
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, (const az_wave_query *) prefix, &ticket) == AZ_INVALID);
    assert(st_event_read(&e, 0, 0, (az_data_change *) prefix) == AZ_INVALID);
    assert(st_interval(NULL, 0, 1, 1, (az_time_interval *) prefix) == AZ_INVALID);
    assert(ticket == UINT64_MAX && *prefix == sizeof *prefix);
    free(prefix);
    st_destroy(&e);
}
static _Atomic int fresh = 1, loads;
static int provider(const az_track_ref *ref, uint32_t mask, struct st_data *d, void *ctx) {
    (void) mask;
    (void) ctx;
    atomic_fetch_add(&loads, 1);
    snprintf(d->view.metadata.title, sizeof d->view.metadata.title, "source %llu", (unsigned long long) ref->source_id);
    d->view.metadata_info.availability = AZ_AVAIL_READY;
    return AZ_OK;
}
static int freshness(const az_track_ref *ref, const struct st_data *d, void *ctx) {
    (void) ref;
    (void) d;
    (void) ctx;
    return atomic_load(&fresh);
}
static void ticket_exhaustion(void) {
    for (int owner = 0; owner < 4; ++owner) {
        struct st_engine e = ST_ENGINE_INIT;
        az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, (uint64_t) owner + 1, 1, 9, 42, 0};
        e.next = UINT64_MAX - 3;
        uint64_t last = 0;
        assert(st_request(&e, owner, &ref, AZ_DATA_METADATA, &last) == AZ_OK && last == UINT64_MAX - 1);
        st_work(&e, provider, NULL, NULL);
        const az_track_snapshot *held = NULL;
        assert(st_acquire(&e, owner, last, &held) == AZ_OK && held && held->track.source_id == ref.source_id);
        puts("TICKET_EXHAUSTION_BASELINE_OK: final complete identity and immutable acquisition work");
        fflush(stdout);
        for (unsigned mode = 0; mode < 2; ++mode) {
            struct st_slot slots[ST_SLOTS];
            struct st_ticket tickets[ST_TICKETS];
            memcpy(slots, e.slots, sizeof slots);
            memcpy(tickets, e.tickets, sizeof tickets);
            const uint64_t sequence = e.sequence;
            uint64_t rejected = UINT64_C(0xcafef00d12345678);
            az_wave_query wave = {sizeof wave, AZ_DATA_VERSION, AZ_WAVE_PWV3_MONO, 0, 1, 0};
            int rc = mode ? st_request_with_wave(&e, owner, &ref, ST_DETAIL, &wave, &rejected)
                          : st_request(&e, owner, &ref, AZ_DATA_METADATA, &rejected);
            assert(rc == AZ_LIMIT && "exhausted track and waveform ticket identities must not wrap");
            assert(rejected == UINT64_C(0xcafef00d12345678) && e.next == UINT64_MAX - 1 && e.sequence == sequence);
            assert(!memcmp(slots, e.slots, sizeof slots) && !memcmp(tickets, e.tickets, sizeof tickets));
            const az_track_snapshot *still_held = NULL;
            assert(st_acquire(&e, owner, last, &still_held) == AZ_OK && still_held == held);
        }
        assert(st_release(&e, owner, last, 0) == AZ_OK);
        st_destroy(&e);
    }
    puts("PASS data ticket exhaustion: four owners, track/wave refusal, unchanged live snapshots");
}
static void event_exhaustion(void) {
    struct st_engine e = ST_ENGINE_INIT;
    az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 1, 9, 42, 0};
    uint64_t ticket = 0;
    assert(st_request(&e, 0, &ref, AZ_DATA_METADATA, &ticket) == AZ_OK);
    e.sequence = UINT64_MAX - 2;
    st_work(&e, provider, NULL, NULL);
    az_data_change event = {.size = sizeof event};
    assert(st_event_read(&e, 0, UINT64_MAX - 2, &event) == AZ_OK && event.sequence == UINT64_MAX - 1);
    puts("EVENT_EXHAUSTION_BASELINE_OK: last usable journal cursor is readable");
    fflush(stdout);
    assert(st_release(&e, 0, ticket, 1) == AZ_OK);
    assert(e.sequence == UINT64_MAX - 1 && "event journal must not publish its invalid cursor or wrap");
    const az_data_change before = event;
    assert(st_event_read(&e, 0, UINT64_MAX - 1, &event) == AZ_LIMIT);
    assert(!memcmp(&event, &before, sizeof event));
    assert(st_release(&e, 0, ticket, 0) == AZ_OK);
    st_destroy(&e);
    puts("PASS event exhaustion: bounded terminal poll, unchanged output and no journal cursor reuse");
}
static void cache(void) {
    struct st_engine e = ST_ENGINE_INIT;
    az_track_ref a = {sizeof a, 1, 1, 1, 9, 42, 0}, b = a;
    b.source_id = 2;
    a.size += 32; /* a newer input prefix must not inflate the returned nested record */
    uint64_t t1, t2, t3;
    const az_track_snapshot *v1 = NULL, *v2 = NULL;
    assert(st_request(&e, 0, &a, 1, &t1) == AZ_OK);
    assert(st_acquire(&e, 0, t1, &v1) == AZ_PENDING && !v1);
    assert(st_request(&e, 1, &a, 1, &t2) == AZ_OK);
    assert(st_request(&e, 1, &b, 1, &t3) == AZ_OK);
    st_work(&e, provider, freshness, NULL);
    assert(atomic_load(&loads) == 2);
    assert(st_acquire(&e, 0, t1, &v1) == AZ_OK && st_acquire(&e, 1, t2, &v2) == AZ_OK && v1 == v2);
    assert(v1->track.size == sizeof(az_track_ref));
    const az_track_snapshot *other;
    assert(st_acquire(&e, 1, t3, &other) == AZ_OK && other != v1 && other->track.source_id == 2);
    assert(st_acquire(&e, 1, t1, &other) == AZ_STALE);
    az_data_change ev = {.size = sizeof ev};
    assert(st_event_read(&e, 0, 0, &ev) == AZ_OK && ev.ticket == t1 && ev.revision == v1->revision);
    assert(st_release(&e, 1, t2, 1) == AZ_OK && st_acquire(&e, 1, t2, &other) == AZ_CANCELLED);
    assert(st_acquire(&e, 0, t1, &other) == AZ_OK);
    atomic_store(&fresh, 0);
    st_work(&e, provider, freshness, NULL);
    assert(st_acquire(&e, 0, t1, &other) == AZ_STALE && !other);
    assert(!strcmp(v1->metadata.title, "source 1")); /* held memory survives invalidation */
    assert(st_release(&e, 0, t1, 0) == AZ_OK && st_acquire(&e, 0, t1, &other) == AZ_STALE);
    assert(st_release(&e, 1, t2, 0) == AZ_OK && st_release(&e, 1, t3, 0) == AZ_OK);
    atomic_store(&fresh, 1);
    for (int i = 0; i < ST_EVENTS + 3; i++) {
        assert(st_request(&e, 0, &a, 1, &t1) == AZ_OK);
        st_work(&e, provider, freshness, NULL);
        assert(st_release(&e, 0, t1, 0) == AZ_OK);
    }
    ev.size = sizeof ev;
    assert(st_event_read(&e, 0, 0, &ev) == AZ_OK && ev.missed);
    pthread_mutex_lock(&e.mu);
    assert(st_request(&e, 0, &a, 1, &t1) == AZ_BUSY);
    pthread_mutex_unlock(&e.mu);
    uint64_t ids[ST_PER_OWNER];
    for (int i = 0; i < ST_PER_OWNER; i++) assert(st_request(&e, 0, &a, 1, &ids[i]) == AZ_OK);
    assert(st_request(&e, 0, &a, 1, &t1) == AZ_LIMIT);
    for (int i = 0; i < ST_PER_OWNER; i++) assert(st_release(&e, 0, ids[i], 0) == AZ_OK);
    st_destroy(&e);
}
static struct st_engine concurrent = ST_ENGINE_INIT;
static _Atomic int stop;
static void *worker(void *arg) {
    (void) arg;
    while (!atomic_load(&stop)) st_work(&concurrent, provider, freshness, NULL);
    return NULL;
}
static void *consumer(void *arg) {
    int owner = (int) (intptr_t) arg;
    az_track_ref ref = {sizeof ref, 1, 1, 1, 9, 42, 0};
    for (int i = 0; i < 2000; i++) {
        uint64_t id;
        int rc;
        do { rc = st_request(&concurrent, owner, &ref, 1, &id); } while (rc == AZ_BUSY);
        assert(rc == AZ_OK);
        const az_track_snapshot *v;
        do { rc = st_acquire(&concurrent, owner, id, &v); } while (rc == AZ_BUSY || rc == AZ_PENDING);
        assert(rc == AZ_OK && v->track.track_id == 42 && !strcmp(v->metadata.title, "source 1"));
        do { rc = st_release(&concurrent, owner, id, 0); } while (rc == AZ_BUSY);
        assert(rc == AZ_OK);
    }
    return NULL;
}
static int wave_provider(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *ctx) {
    (void) ref;
    (void) ctx;
    assert(mask == ST_DETAIL);
    data->wave_view.encoding = data->wave_query.encoding;
    data->wave_view.first_column = data->wave_query.first_column;
    data->wave_view.column_count = data->wave_query.column_count;
    data->wave_view.info.availability = AZ_AVAIL_READY;
    data->detail[0] = (uint8_t) data->wave_query.first_column;
    return AZ_OK;
}
static void wave_cache(void) {
    struct st_engine e = ST_ENGINE_INIT;
    az_track_ref ref = {sizeof ref, 1, 1, 1, 9, 42, 0};
    az_wave_query q = {sizeof q, 1, AZ_WAVE_PWV3_MONO, 75, 16, 0};
    uint64_t first, shared, other;
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, &q, &first) == AZ_OK);
    assert(st_request_with_wave(&e, 1, &ref, ST_DETAIL, &q, &shared) == AZ_OK);
    q.first_column = 150;
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, &q, &other) == AZ_OK);
    uint64_t shorter;
    q.column_count = 8;
    assert(st_request_with_wave(&e, 2, &ref, ST_DETAIL, &q, &shorter) == AZ_OK);
    st_work(&e, wave_provider, NULL, NULL);
    const az_wave_snapshot *a, *b, *c;
    assert(st_wave_acquire(&e, 0, first, &a) == AZ_OK);
    assert(st_wave_acquire(&e, 1, shared, &b) == AZ_OK && a == b);
    assert(st_wave_acquire(&e, 0, other, &c) == AZ_OK && c != a);
    assert(a->first_column == 75 && a->columns[0] == 75 && c->first_column == 150);
    assert(a->info.provenance == AZ_PROV_FILE && a->revision && a->observed_ns);
    const az_wave_snapshot *short_view;
    assert(st_wave_acquire(&e, 2, shorter, &short_view) == AZ_OK);
    assert(short_view != c && short_view->column_count == 8 && c->column_count == 16);
    const az_track_snapshot *wrong;
    assert(st_acquire(&e, 0, first, &wrong) == AZ_INVALID && !wrong);
    assert(st_wave_acquire(&e, 2, first, &c) == AZ_STALE && !c);
    assert(st_release(&e, 0, first, 1) == AZ_OK);
    assert(st_wave_acquire(&e, 0, first, &c) == AZ_CANCELLED);
    assert(st_wave_acquire(&e, 1, shared, &c) == AZ_OK && c == b);
    q.encoding = 999;
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, &q, &first) == AZ_UNSUPPORTED);
    q.encoding = 1;
    q.column_count = AZ_WAVE_WINDOW_MAX + 1;
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, &q, &first) == AZ_LIMIT);
    q.column_count = 0;
    assert(st_request_with_wave(&e, 0, &ref, ST_DETAIL, &q, &first) == AZ_INVALID);
    st_destroy(&e);
}
static void revision_exhaustion(void) {
    for (unsigned wave = 0; wave < 2; ++wave) {
        struct st_engine e = ST_ENGINE_INIT;
        az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 1, 9, 42, 0};
        az_wave_query query = {sizeof query, AZ_DATA_VERSION, AZ_WAVE_PWV3_MONO, 0, 1, 0};
        uint64_t first = 0, next = 0;
        e.revision = UINT64_MAX - 1;
        assert((wave ? st_request_with_wave(&e, 0, &ref, ST_DETAIL, &query, &first)
                     : st_request(&e, 0, &ref, AZ_DATA_METADATA, &first)) == AZ_OK);
        st_work(&e, wave ? wave_provider : provider, NULL, NULL);
        const az_track_snapshot *track = NULL;
        const az_wave_snapshot *columns = NULL;
        assert((wave ? st_wave_acquire(&e, 0, first, &columns) : st_acquire(&e, 0, first, &track)) == AZ_OK);
        assert((wave ? columns->revision : track->revision) == UINT64_MAX);
        puts("REVISION_EXHAUSTION_BASELINE_OK: final immutable revision is acquirable");
        fflush(stdout);
        ++ref.track_id;
        assert((wave ? st_request_with_wave(&e, 1, &ref, ST_DETAIL, &query, &next)
                     : st_request(&e, 1, &ref, AZ_DATA_METADATA, &next)) == AZ_OK);
        st_work(&e, wave ? wave_provider : provider, NULL, NULL);
        const az_track_snapshot *unpublished = NULL;
        const az_wave_snapshot *unpublished_wave = NULL;
        int rc = wave ? st_wave_acquire(&e, 1, next, &unpublished_wave) : st_acquire(&e, 1, next, &unpublished);
        assert(rc == AZ_LIMIT && "snapshot publication must refuse exhausted revision identity");
        assert(!unpublished && !unpublished_wave && e.revision == UINT64_MAX);
        assert((wave ? columns->revision : track->revision) == UINT64_MAX);
        assert(st_release(&e, 0, first, 0) == AZ_OK && st_release(&e, 1, next, 0) == AZ_OK);
        st_destroy(&e);
    }
    puts("PASS revision exhaustion: track/wave failure publication preserves earlier immutable views");
}
static void retention_and_event_edges(void) {
    struct st_engine e = ST_ENGINE_INIT;
    az_track_ref ref = {sizeof ref, 1, 1, 1, 9, 42, 0};
    uint64_t tickets[ST_SLOTS], extra = UINT64_MAX;
    const az_track_snapshot *held[ST_SLOTS];
    for (int i = 0; i < ST_SLOTS; i++) {
        ref.track_id = 42u + (uint32_t) i;
        assert(st_request(&e, i, &ref, AZ_DATA_METADATA, &tickets[i]) == AZ_OK);
    }
    st_work(&e, provider, freshness, NULL);
    for (int i = 0; i < ST_SLOTS; i++)
        assert(st_acquire(&e, i, tickets[i], &held[i]) == AZ_OK && held[i]->track.track_id == 42u + (uint32_t) i);
    ref.track_id = 999;
    assert(st_request(&e, ST_SLOTS, &ref, AZ_DATA_METADATA, &extra) == AZ_LIMIT && extra == UINT64_MAX);
    assert(st_release(&e, 0, tickets[0], 0) == AZ_OK);
    assert(st_request(&e, ST_SLOTS, &ref, AZ_DATA_METADATA, &extra) == AZ_OK);
    st_work(&e, provider, freshness, NULL);
    const az_track_snapshot *replacement;
    assert(st_acquire(&e, ST_SLOTS, extra, &replacement) == AZ_OK && replacement->track.track_id == 999);
    assert(st_acquire(&e, 0, tickets[0], &replacement) == AZ_STALE && !replacement);
    for (int i = 1; i < ST_SLOTS; i++) {
        assert(held[i]->track.track_id == 42u + (uint32_t) i && !strcmp(held[i]->metadata.title, "source 1"));
        assert(st_release(&e, i, tickets[i], 0) == AZ_OK);
    }
    assert(st_release(&e, ST_SLOTS, extra, 0) == AZ_OK);
    for (int i = 0; i < ST_EVENTS + 1; i++) {
        assert(st_request(&e, 0, &ref, AZ_DATA_METADATA, &extra) == AZ_OK);
        assert(st_release(&e, 0, extra, 0) == AZ_OK);
    }
    struct {
        az_data_change value;
        uint64_t guard;
    } event = {.value = {.size = sizeof event}, .guard = UINT64_C(0xaabbccddeeff0123)};
    assert(st_event_read(&e, 0, 0, &event.value) == AZ_OK && event.value.missed && event.value.ticket);
    assert(event.value.size == sizeof event && event.guard == UINT64_C(0xaabbccddeeff0123));
    assert(st_event_read(&e, 99, 0, &event.value) == AZ_OK && event.value.missed && !event.value.ticket && !event.value.kind);
    assert(event.value.sequence == e.sequence); /* Recovery works even without a retained owner event. */
    az_data_change before = event.value;
    assert(st_event_read(&e, 99, e.sequence, &event.value) == AZ_EMPTY);
    assert(st_event_read(&e, 99, e.sequence + 1, &event.value) == AZ_INVALID);
    assert(st_event_read(&e, 99, UINT64_MAX, &event.value) == AZ_INVALID);
    assert(!memcmp(&before, &event.value, sizeof before));
    uint32_t *short_event = malloc(sizeof *short_event);
    assert(short_event);
    *short_event = sizeof *short_event;
    assert(st_event_read(&e, 0, 0, (az_data_change *) short_event) == AZ_INVALID && *short_event == sizeof *short_event);
    free(short_event);
    st_destroy(&e);
}
struct epoch_source {
    uint64_t generation[4];
};
static int epoch_load(const az_track_ref *ref, uint32_t mask, struct st_data *data, void *context) {
    (void) mask;
    (void) context;
    snprintf(data->view.metadata.title, sizeof data->view.metadata.title, "source %llu epoch %llu",
             (unsigned long long) ref->source_id, (unsigned long long) ref->source_generation);
    data->view.metadata_info.availability = AZ_AVAIL_READY;
    return AZ_OK;
}
static int epoch_fresh(const az_track_ref *ref, const struct st_data *data, void *context) {
    (void) data;
    struct epoch_source *source = context;
    assert(ref->source_id >= 1 && ref->source_id <= 4);
    return source->generation[ref->source_id - 1] == ref->source_generation;
}
static void reconnect_matrix(void) {
    struct st_engine engine = ST_ENGINE_INIT;
    struct epoch_source source = {{1, 1, 1, 1}};
    uint64_t tickets[4];
    const az_track_snapshot *snapshots[4];
    az_track_ref refs[4];
    for (unsigned i = 0; i < 4; ++i) {
        refs[i] = (az_track_ref) {sizeof(az_track_ref), 1, i + 1, 1, 9, 42, 0};
        assert(st_request(&engine, i, &refs[i], AZ_DATA_METADATA, &tickets[i]) == AZ_OK);
    }
    st_work(&engine, epoch_load, epoch_fresh, &source);
    for (unsigned i = 0; i < 4; ++i) assert(st_acquire(&engine, i, tickets[i], &snapshots[i]) == AZ_OK);
    for (unsigned round = 0; round < 32; ++round)
        for (unsigned i = 0; i < 4; ++i) {
            const az_track_snapshot *old = snapshots[i], *out = NULL;
            uint64_t old_ticket = tickets[i], old_revision = old->revision;
            char old_title[sizeof old->metadata.title];
            memcpy(old_title, old->metadata.title, sizeof old_title);
            source.generation[i]++;
            st_work(&engine, epoch_load, epoch_fresh, &source);
            assert(st_acquire(&engine, i, old_ticket, &out) == AZ_STALE && !out);
            assert(!memcmp(old->metadata.title, old_title, sizeof old_title));
            refs[i].source_generation = source.generation[i];
            assert(st_request(&engine, i, &refs[i], AZ_DATA_METADATA, &tickets[i]) == AZ_OK);
            st_work(&engine, epoch_load, epoch_fresh, &source);
            assert(st_acquire(&engine, i, tickets[i], &out) == AZ_OK && out != old);
            assert(out->revision > old_revision && out->track.track_id == 42 && out->track.source_id == i + 1);
            assert(out->track.source_generation == source.generation[i]);
            assert(!memcmp(old->metadata.title, old_title, sizeof old_title));
            snapshots[i] = out;
            for (unsigned other = 0; other < 4; ++other)
                if (other != i) {
                    const az_track_snapshot *peer;
                    assert(st_acquire(&engine, other, tickets[other], &peer) == AZ_OK && peer == snapshots[other]);
                }
            assert(st_release(&engine, i, old_ticket, 0) == AZ_OK);
            assert(st_acquire(&engine, i, old_ticket, &out) == AZ_STALE && !out);
            assert(st_acquire(&engine, (i + 1) % 4, tickets[i], &out) == AZ_STALE && !out);
        }
    for (unsigned i = 0; i < 4; ++i) {
        az_data_change change = {.size = sizeof change};
        assert(st_event_read(&engine, i, 0, &change) == AZ_OK && change.missed);
        assert(st_release(&engine, i, tickets[i], 0) == AZ_OK);
    }
    st_destroy(&engine);
}
int main(void) {
#ifdef SDK_TICKET_EXHAUSTION_ONLY
    ticket_exhaustion();
    return 0;
#endif
    reconnect_matrix();
    timing_envelopes();
    request_envelopes();
    parser();
    cache();
    wave_cache();
    ticket_exhaustion();
    event_exhaustion();
    revision_exhaustion();
    retention_and_event_edges();
    pthread_t w, c[4];
    assert(!pthread_create(&w, NULL, worker, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_create(&c[i], NULL, consumer, (void *) (intptr_t) i));
    for (int i = 0; i < 4; i++) pthread_join(c[i], NULL);
    atomic_store(&stop, 1);
    pthread_join(w, NULL);
    st_destroy(&concurrent);
    puts("sdk data: parser, timing, identity, lifetime, cancellation, events and concurrency passed");
    return 0;
}

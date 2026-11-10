/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "sdk_status_engine.c"
static void ds_gui_interest(uint64_t topics) { (void) topics; }
static unsigned wakes;
static uint64_t due;
static void ds_wake(void) { wakes++; }
static void ds_deadline(uint64_t ns) { due = ns; }
static void ds_native_interest(unsigned d, uint64_t mask) {
    (void) d;
    (void) mask;
}
static void sdk_status_store_interest(unsigned d, uint64_t mask) {
    (void) d;
    (void) mask;
}
static void sdk_status_store_binding(unsigned d, uint64_t e, const az_track_ref *r, uint64_t c) {
    (void) d;
    (void) e;
    (void) r;
    (void) c;
}
static int ds_live(int owner) { return owner >= 0; }
struct trace {
    uint64_t sequence, lost;
    unsigned initial, changes, recoveries, actions;
};
static void capture(const az_deck_status_event_v1 *e, void *u) {
    struct trace *trace = u;
    assert(e->sequence > trace->sequence);
    trace->sequence = e->sequence;
    if (e->kind == AZ_STATUS_INITIAL) {
        trace->initial++;
        assert(e->changed_topics == e->topics);
    } else if (e->kind == AZ_STATUS_RESYNC) {
        trace->recoveries++;
        assert(e->lost_topics && e->loss_reason && !e->occurrence && !e->phrase_transition);
        trace->lost = e->lost_topics;
    } else {
        trace->changes++;
        if (e->occurrence) trace->actions++;
    }
    if (!(e->topics & AZ_STATUS_TOPIC(AZ_STATUS_LOOP))) assert(!e->view->loop);
}
static az_deck_subscription watch(unsigned owner, uint64_t topics, struct trace *trace) {
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1, 30, topics};
    az_deck_subscription handle = 0;
    assert(ds_register(owner, &w, capture, trace, &handle) == AZ_OK && handle);
    return handle;
}
static struct ds_field ready(unsigned t, unsigned f, const void *v, unsigned count) {
    return (struct ds_field) {t, f, {.info = {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, ds_now()}, .scope = AZ_STATUS_LOAD_SCOPE, .count = count, .scope_id = 1, .provider_id = 10}, v, NULL};
}
static int set(unsigned t, unsigned f, const void *v) {
    struct ds_field field = ready(t, f, v, 1);
    return ds_publish(0, 1, 0, ds_now(), &field, 1, NULL, NULL, 0, 0);
}
static void fill_tempo(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        double value = 90 + (i % 2);
        assert(set(AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_CURRENT_BPM, &value) == AZ_OK);
    }
}
static void keys(void) {
    const struct ds_image *im = &ds.deck[0].current;
    int32_t on = 1, shift = 1;
    struct ds_field fields[] = {
        ready(AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL, "Am", 2),
        ready(AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES, &shift, 1),
        ready(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_MASTER_TEMPO, &on, 1),
    };
    fields[0].info.info.provenance = AZ_PROV_FILE;
    assert(ds_publish(0, 1, 0, ds_now(), fields, 3, NULL, NULL, 0, 0) == AZ_OK);
    assert(!strcmp(ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL), "Am"));
    assert(!strcmp(ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE), "3A"));
    assert(im->field[AZ_STATUS_KEY][AZ_STATUS_KEY_EFFECTIVE]->info.count == 2);
    shift = -1;
    assert(set(AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES, &shift) == AZ_OK);
    assert(!strcmp(ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE), "1A"));
    shift = 0;
    assert(set(AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES, &shift) == AZ_OK);
    assert(!strcmp(ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE), "8A"));
    on = 0;
    assert(set(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_MASTER_TEMPO, &on) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE));
    const char *names[] = {"12B", "E", "F♭ major", "Dbm", "unknown"};
    const char *canonical[] = {"12B", "12B", "12B", "12A", NULL};
    on = 1;
    for (unsigned n = 0; n < sizeof names / sizeof *names; ++n) {
        fields[0].value = names[n];
        fields[0].info.count = (unsigned) strlen(names[n]);
        assert(ds_publish(0, 1, 0, ds_now(), fields, 3, NULL, NULL, 0, 0) == AZ_OK);
        const char *effective = ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE);
        assert(canonical[n] ? effective && !strcmp(canonical[n], effective) : !effective);
    }
}
static void pressure(void) {
    struct trace loops = {0}, both = {0}, transport = {0};
    uint64_t loop = AZ_STATUS_TOPIC(AZ_STATUS_LOOP);
    az_deck_subscription h = watch(0, loop, &loops);
    watch(1, loop | DS_ACTION_ROUTES, &both);
    watch(2, DS_ACTION_ROUTES, &transport);
    ds_drain(8);
    assert(loops.initial == 1 && both.initial == 1 && transport.initial == 1);
    fill_tempo(DS_HISTORY + 5);
    ds_drain(8);
    assert(!loops.changes && !loops.recoveries && !both.changes && !both.recoveries);
    int engaged = 0;
    assert(set(AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, &engaged) == AZ_OK);
    fill_tempo(DS_HISTORY);
    az_status_occurrence_v1 jump = {.size = sizeof jump, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_JUMP, .ordinal = 1};
    assert(ds_publish(0, 1, 0, ds_now(), NULL, 0, &jump, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(loops.recoveries == 1 && loops.lost == loop && !loops.changes);
    assert(both.recoveries == 1 && both.lost == loop && both.actions == 1);
    assert(!transport.recoveries && transport.actions == 1);
    assert(ds_release(1, h) == AZ_INVALID && ds_release(0, h) == AZ_OK && ds_release(0, h) == AZ_STALE);
    ds_retire(1);
    ds_retire(2);
}
static void assert_seconds(double first, double second, unsigned rate, unsigned window) {
    int order = compare_timestamps(first, second, 1, rate, window);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
}
static void source_clock_boundaries(void) {
    int order = ds_compare_source_times(-1, 1000, -48, 48000, 48000, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
    order = ds_compare_source_times(INT64_MAX - 1, UINT32_MAX, INT64_MAX, UINT32_MAX, UINT32_MAX, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE);
    order = ds_compare_source_times(INT64_MAX - 1, UINT32_MAX, INT64_MAX, UINT32_MAX, UINT32_MAX, 2);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
    assert(ds_compare_source_times(1, 0, 1, 10, 10, 1) == AZ_TIME_INVALID);
    assert(ds_compare_source_times(1, 10, 1, 10, 10, 0) == AZ_TIME_INVALID);

    /* The declared 64-sample observation covers one sample past LOOP OUT.
     * A setter's atomic one-sample observation distinguishes that difference. */
    az_status_coord_v1 out = {.samples = 149852, .rate_hz = 44100};
    az_status_coord_v1 landed = {.samples = 149853, .rate_hz = 44100};
    struct ds_time_axis clock = {44100, NULL, 64, 44100, NULL};
    assert(ds_payload_same(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &out, &landed, sizeof out, &clock, &clock));
    clock.window_samples = 1;
    assert(!ds_payload_same(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &out, &landed, sizeof out, &clock, &clock));
    clock.window_samples = 64;
    landed.samples = out.samples + 64;
    assert(!ds_payload_same(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &out, &landed, sizeof out, &clock, &clock));

    int64_t ticks[] = {-10, 10, 20};
    struct ds_time_axis grid_clock = {10, ticks, 1, 10, NULL};
    az_phrase phrases[] = {{0, 1, 1, 1, 0, 0}, {1, 2, 2, 1, 0, 0}};
    struct ds_map map = {.rows = phrases, .clock = &grid_clock, .count = 2, .provider = 1, .revision = 1};
    az_status_coord_v1 at = {.seconds = .9, .samples = 9, .rate_hz = 10};
    az_status_phrase_endpoint_v1 endpoint = ds_at(&map, &at, 0, 0, 10, 1);
    assert(endpoint.kind == AZ_PHRASE_ITEM && endpoint.index == 0);
    endpoint = ds_at(&map, &at, 0, 0, 10, 2);
    assert(endpoint.kind == AZ_PHRASE_ITEM && endpoint.index == 1);
    assert(!ds_at(&map, &at, 0, 0, 10, 0).kind);

    const unsigned deck = 1;
    az_beat beats[] = {{-1, 60, 1, 0}, {1, 60, 2, 0}, {2, 60, 3, 0}};
    at = (az_status_coord_v1) {.samples = 0, .rate_hz = 10};
    struct ds_time_axis head_clock = {10, NULL, 1, 10, NULL};
    struct ds_field fields[] = {ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 3),
                                ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &at, 1)};
    fields[0].clock = &grid_clock;
    fields[1].clock = &head_clock;
    assert(ds_publish(deck, 1, DS_BOUNDARY, ds_now(), fields, 2, NULL, NULL, 0, 0) == AZ_OK);
    const double *phase = ds_value(&ds.deck[deck].current, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT);
    assert(phase && *phase == .5);
    const struct ds_cell *copy = ds_ready(&ds.deck[deck].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    order = ds_compare_source_times(copy->clock.ticks[0], copy->clock.units_per_second, -10, 10, 10, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);

    ticks[0] = INT64_C(1) << 54;
    ticks[1] = ticks[0] + 10;
    ticks[2] = ticks[1] + 10;
    for (unsigned i = 0; i < 3; i++) beats[i].time_s = (double) ticks[i] / 10;
    at.samples = ticks[0] + 5;
    at.seconds = (double) at.samples / 10;
    fields[0].info.info.revision++;
    assert(ds_publish(deck, 1, 0, ds_now(), fields, 2, NULL, NULL, 0, 0) == AZ_OK);
    phase = ds_value(&ds.deck[deck].current, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT);
    assert(phase && *phase == .5);
}
static void phase_alignment(void) {
    az_beat rows[8] = {0};
    int64_t ticks[8];
    struct ds_time_axis grid_clock = {100, ticks, 1, 100, NULL}, head_clock = {100, NULL, 1, 100, NULL};
    const unsigned markers[] = {0, 1, 2, 3, 1, 2, 1, 0};
    for (unsigned i = 0; i < 8; i++) {
        rows[i].time_s = i;
        ticks[i] = i * 100;
        rows[i].bpm = 60;
        rows[i].beat_in_bar = markers[i];
    }
    struct ds_field grid = ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, rows, 8);
    grid.clock = &grid_clock;
    az_status_coord_v1 at = {.seconds = 2.5, .samples = 250, .rate_hz = 100};
    struct ds_field head = ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &at, 1);
    head.clock = &head_clock;
    assert(ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_publish(0, 1, 0, ds_now(), &head, 1, NULL, NULL, 0, 0) == AZ_OK);
    const struct ds_image *im = &ds.deck[0].current;
    assert(*(const double *) ds_value(im, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT) == 0.5);
    assert(*(const double *) ds_value(im, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR) == 0.5);
    rows[4].beat_in_bar = 4;
    rows[5].beat_in_bar = 1;
    rows[6].beat_in_bar = 1;
    grid.info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(*(const double *) ds_value(im, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR) == 0.375);
    at.seconds = 0.5;
    at.samples = 50;
    head.info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), &head, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR));
    at.seconds = 6.5;
    at.samples = 650;
    head.info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), &head, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR));
}
static void time_and_pins(void) {
    struct trace motion = {0};
    watch(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE), &motion);
    ds_drain(8);
    az_status_coord_v1 head = {10, 441000, 44100, 0};
    double duration = 120, precision = 1, rate = 2;
    int64_t duration_tick = 120, precision_tick = 1;
    struct ds_time_axis head_clock = {44100, NULL, 64, 44100, NULL};
    struct ds_time_axis duration_clock = {1, &duration_tick, 1, 1, NULL}, precision_clock = {1, &precision_tick, 1, 1, NULL};
    int forward = 1, playing = AZ_PLAYBACK_PLAYING, off = 0;
    struct ds_field fields[] = {
        ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &head, 1),
        ready(AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S, &duration, 1),
        ready(AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S, &precision, 1),
        ready(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, &playing, 1),
        ready(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, &forward, 1),
        ready(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE, &rate, 1),
        ready(AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, &off, 1),
        ready(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_SLIP, &off, 1),
    };
    fields[0].clock = &head_clock;
    fields[1].clock = &duration_clock;
    fields[2].clock = &precision_clock;
    assert(ds_publish(0, 1, 0, ds_now(), fields, 8, NULL, NULL, 0, 0) == AZ_OK);
    const struct ds_image *im = &ds.deck[0].current;
    assert_seconds(*(const double *) ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S), 110, 44100, 64);
    assert_seconds(*(const double *) ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S), 55, 88200, 64);
    ds_drain(8);
    assert(motion.changes == 1);
    head.seconds = 11;
    head.samples = 485100;
    assert(ds_publish(0, 1, DS_MOTION, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(due && motion.changes == 1);
    struct timespec pause = {0, 40000000};
    nanosleep(&pause, NULL);
    ds_drain(8);
    assert(!due && motion.changes == 2);
    const az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 2, 3, 4, 0};
    assert(set(AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, &ref) == AZ_OK);
    uint8_t samples[] = {3, 4};
    az_waveform wave = {sizeof wave, AZ_DATA_VERSION, AZ_WAVE_PREVIEW, 2, 0, 60, samples};
    struct ds_field picture = ready(AZ_STATUS_OVERVIEW, AZ_STATUS_OVERVIEW_SAVED, &wave, 1);
    picture.info.info.provenance = AZ_PROV_FILE;
    picture.clock = &duration_clock;
    assert(ds_publish_file(0, 1, &ref, ds_now(), &picture, 1, 0, 0) == AZ_OK);
    samples[0] = 99;
    const az_waveform *copy = ds_value(im, AZ_STATUS_OVERVIEW, AZ_STATUS_OVERVIEW_SAVED);
    assert(copy && copy->samples[0] == 3 && copy->samples[1] == 4);
    int native_port = 2, derived_port = 1;
    struct ds_field source = ready(AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, &native_port, 1);
    assert(ds_publish(0, 1, 0, ds_now(), &source, 1, NULL, NULL, 0, 0) == AZ_OK);
    source.value = &derived_port;
    source.info.info.provenance = AZ_PROV_DERIVED;
    assert(ds_publish(0, 1, 0, ds_now(), &source, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(*(const int *) ds_value(im, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE) == native_port);
    assert(ds_retire_file_binding(0, 1, &ref) == AZ_OK);
    assert(*(const int *) ds_value(im, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE) == native_port);
    assert(ds_publish_file(0, 1, &ref, ds_now(), &picture, 1, 0, 0) == AZ_STALE);
    assert(ds_publish(0, 0, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), fields, 8, NULL, NULL, 0, 0) == AZ_STALE);
    ds_retire(0);
}
struct phrase_trace {
    unsigned count, baselines, actions;
    int pairs[16][2];
    double measured[16];
};
static int endpoint_number(const az_status_phrase_endpoint_v1 *e) {
    return e->kind == AZ_PHRASE_ITEM ? (int) e->index : -(int) e->kind;
}
static void phrases(const az_deck_status_event_v1 *e, void *u) {
    struct phrase_trace *t = u;
    if (e->occurrence) t->actions++;
    if (!e->phrase_transition) {
        t->baselines++;
        return;
    }
    unsigned i = t->count++;
    assert(i < 16 && !memcmp(e->view->phrase_playback->active, &e->phrase_transition->to, sizeof e->phrase_transition->to));
    t->pairs[i][0] = endpoint_number(&e->phrase_transition->from);
    t->pairs[i][1] = endpoint_number(&e->phrase_transition->to);
    t->measured[i] = e->view->position->head.seconds;
}
static void phrase_motion(void) {
    struct phrase_trace t = {0};
    az_deck_subscription h;
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1, 1, DS_ACTION_ROUTES | DS_PHRASE_ROUTE};
    assert(ds_register(2, &watch, phrases, &t, &h) == AZ_OK);
    const unsigned deck = 0;
    uint64_t now = ds_now();
    az_beat beats[] = {{0, 60, 1, 0}, {10, 60, 2, 0}, {20, 60, 3, 0}, {30, 60, 4, 0}};
    int64_t ticks[] = {0, 480000, 960000, 1440000};
    struct ds_time_axis grid_clock = {48000, ticks, 1, 48000, NULL}, head_clock = {48000, NULL, 64, 48000, NULL};
    az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {1, 2, 2, 1, 0, 0}, {2, 3, 3, 1, 0, 0}};
    az_status_grid_ref_v1 ref = {2, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    az_status_coord_v1 head = {0, 0, 48000, 0};
    struct ds_field data[] = {ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 4),
                              ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, rows, 3),
                              ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, &ref, 1),
                              ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &head, 1)};
    data[0].clock = &grid_clock;
    data[3].clock = &head_clock;
    for (unsigned i = 0; i < 4; i++) data[i].info.scope_id = 2;
    struct ds_path path = {.order = 1, .generation = 1, .phrase_provider = 10, .phrase_revision = 1, .grid = ref, .kind = DS_PATH_BASELINE, .endpoints = 3, .opening = 1, .from = head, .to = head, .observed_window_samples = 64, .observed_window_rate = 48000, .from_window_samples = 64, .from_window_rate = 48000};
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_BOUNDARY, now, data, 4, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    path.order++;
    path.kind = DS_PATH_TRAVEL;
    path.opening = 0;
    path.from = head;
    path.to = (az_status_coord_v1) {25, 1200000, 48000, 0};
    head = path.to;
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_MOTION, now + 1, &data[3], 1, NULL, NULL, 0, 0) == AZ_OK);
    path.order++;
    path.from = head;
    path.to = (az_status_coord_v1) {5, 240000, 48000, 0};
    head = path.to;
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_MOTION, now + 2, &data[3], 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    const int excursion[5][2] = {{-AZ_PHRASE_START, 0}, {0, 1}, {1, 2}, {2, 1}, {1, 0}};
    assert(t.count == 5 && !memcmp(t.pairs, excursion, sizeof excursion));
    assert_seconds(t.measured[0], 0, 48000, 64);
    assert_seconds(t.measured[2], 0, 48000, 64);
    assert_seconds(t.measured[3], 25, 48000, 64);
    path.order++;
    path.generation++;
    path.kind = DS_PATH_ACTION;
    path.from = head;
    path.to = (az_status_coord_v1) {25, 1200000, 48000, 0};
    head = path.to;
    az_status_occurrence_v1 jump = {.size = sizeof jump, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_JUMP, .endpoints = 3, .ordinal = 2, .from = path.from, .to = head};
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, 0, now + 3, &data[3], 1, &jump, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(t.count == 6 && t.pairs[5][0] == 0 && t.pairs[5][1] == 2 && t.actions == 1);
    path.order++;
    path.kind = DS_PATH_TRAVEL;
    path.from = head;
    path.to = (az_status_coord_v1) {30, 1440000, 48000, 0};
    head = path.to;
    path.defer_terminal = 1;
    path.terminal = path.to;
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_MOTION, now + 4, &data[3], 1, NULL, NULL, 0, 0) == AZ_OK);
    while (ds_drain(8)) {}
    assert(t.count == 6); /* the final grid boundary cannot manufacture a GAP */
    path.order++;
    path.from = path.to;
    path.defer_terminal = 0;
    path.ended = 1;
    path.confirm_terminal = 1;
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_MOTION, now + 4, &data[3], 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(t.count == 7 && t.pairs[6][0] == 2 && t.pairs[6][1] == -AZ_PHRASE_END);
    path.order++;
    path.generation = 1;
    assert(ds_publish_checked(&path, NULL, 0, deck, 2, DS_MOTION, now, &data[3], 1, NULL, NULL, 0, 0) == AZ_STALE);
    ds_retire(2);
}
static void map_race(int new_first, int jump) {
    unsigned epoch = 3 + new_first + 2 * jump;
    uint64_t now = ds_now();
    az_beat beats[] = {{0, 60, 1, 0}, {10, 60, 2, 0}, {20, 60, 3, 0}};
    int64_t ticks[] = {0, 480000, 960000};
    struct ds_time_axis grid_clock = {48000, ticks, 1, 48000, NULL}, head_clock = {48000, NULL, 64, 48000, NULL};
    az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {1, 2, 2, 1, 0, 0}};
    az_status_grid_ref_v1 ref = {epoch, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    az_status_coord_v1 head = {9, 9 * 48000, 48000, 0};
    struct ds_field data[] = {ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 3),
                              ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, rows, 2),
                              ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, &ref, 1),
                              ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &head, 1)};
    data[0].clock = &grid_clock;
    data[3].clock = &head_clock;
    for (unsigned i = 0; i < 4; i++) data[i].info.scope_id = epoch;
    struct ds_path seed = {.order = 1, .generation = 1, .kind = DS_PATH_BASELINE, .phrase_provider = 10, .phrase_revision = 1, .grid = ref, .endpoints = 3, .from = head, .to = head, .observed_window_samples = 64, .observed_window_rate = 48000, .from_window_samples = 64, .from_window_rate = 48000};
    assert(ds_publish_checked(&seed, NULL, 0, 0, epoch, DS_BOUNDARY, now, data, 4, NULL, NULL, 0, 0) == AZ_OK);
    struct phrase_trace t = {0};
    az_deck_subscription h;
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1, 1, DS_ACTION_ROUTES | DS_PHRASE_ROUTE};
    assert(ds_register(2, &w, phrases, &t, &h) == AZ_OK);
    ds_drain(8);
    struct ds_path change = seed, travel = seed;
    change.order = new_first ? 2 : 3;
    travel.order = new_first ? 3 : 2;
    travel.kind = jump ? DS_PATH_ACTION : DS_PATH_TRAVEL;
    travel.to = (az_status_coord_v1) {11, 11 * 48000, 48000, 0};
    travel.generation = jump ? 2 : 1;
    if (!new_first) {
        change.from = change.to = travel.to;
        change.generation = travel.generation;
    }
    change.grid.revision = 2;
    if (new_first) travel.grid = change.grid;
    az_status_occurrence_v1 action = {.size = sizeof action, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_JUMP, .endpoints = 3, .ordinal = (uint64_t) epoch + 2, .from = travel.from, .to = travel.to};
    data[0].info.info.revision = 2;
    beats[1].time_s = 12;
    ticks[1] = 12 * 48000;
    ref = change.grid;
    data[2].info.info.revision = 2;
    struct ds_field measured = data[3];
    measured.value = &travel.to;
    data[3].value = &change.to;
    /* Deliberately deliver each writer's second-in-order receipt first.
     * Mutate its caller storage after return to verify the history pin. */
    if (new_first) {
        assert(ds_publish_checked(&travel, NULL, 0, 0, epoch, jump ? 0 : DS_MOTION, now + 2, &measured, 1, jump ? &action : NULL, NULL, 0, 0) == AZ_PENDING);
        measured.value = NULL;
        assert(ds_publish_checked(&change, NULL, 0, 0, epoch, 0, now + 1, data, 4, NULL, NULL, 0, 0) == AZ_OK);
    } else {
        assert(ds_publish_checked(&change, NULL, 0, 0, epoch, 0, now + 2, data, 4, NULL, NULL, 0, 0) == AZ_PENDING);
        beats[1].time_s = 14;
        ticks[1] = 14 * 48000;
        assert(ds_publish_checked(&travel, NULL, 0, 0, epoch, jump ? 0 : DS_MOTION, now + 1, &measured, 1, jump ? &action : NULL, NULL, 0, 0) == AZ_OK);
    }
    ds_drain(8);
    assert(t.count == (new_first ? 0u : 1u));
    if (!new_first) assert(t.pairs[0][0] == 0 && t.pairs[0][1] == 1);
    assert(t.actions == (unsigned) jump);
    const struct ds_image *im = &ds.deck[0].current;
    const az_status_phrase_endpoint_v1 *active = ds_value(im, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    const az_beat *owned = ds_value(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    assert(active && active->kind == AZ_PHRASE_ITEM && active->index == 0 && active->grid.revision == 2);
    assert(owned);
    assert_seconds(owned[1].time_s, 12, 48000, 1);
    const struct ds_cell *grid_copy = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    int order = compare_timestamps(grid_copy->clock.ticks[1], 12 * 48000, 48000, 48000, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
    ds_retire(2);
}
struct action_trace {
    uint64_t topics, lost, cut;
    unsigned mask, changes, occurrences, pairs, recoveries;
};
static void record_action(const az_deck_status_event_v1 *event, void *user) {
    struct action_trace *trace = user;
    assert(event->topics == trace->topics && !event->view->phrases);
    if (event->kind == AZ_STATUS_RESYNC) {
        assert(!event->occurrence && !event->phrase_transition);
        trace->recoveries++;
        trace->lost = event->lost_topics;
    } else if (event->kind == AZ_STATUS_CHANGE) {
        trace->changes++;
        if (event->occurrence) {
            assert((trace->topics & DS_ACTION_ROUTES) && event->occurrence->endpoints == trace->mask);
            trace->occurrences++;
        }
        if (event->phrase_transition) {
            const az_status_phrase_transition_v1 *pair = event->phrase_transition;
            assert((trace->topics & DS_PHRASE_ROUTE) && pair->discontinuity == AZ_STATUS_JUMP);
            assert(pair->from.kind == AZ_PHRASE_ITEM && pair->from.index == 0);
            assert(pair->to.kind == AZ_PHRASE_ITEM && pair->to.index == 1);
            const az_status_phrase_endpoint_v1 *active = event->view->phrase_playback->active;
            assert(active && !memcmp(&pair->to, active, sizeof pair->to));
            trace->pairs++;
        }
        if (event->occurrence || event->phrase_transition) trace->cut = event->sequence;
    }
}
static void partial_discontinuities(void) {
    const unsigned deck = 1;
    uint64_t epoch = ds.deck[deck].last_epoch + 1;
    const az_beat beats[] = {{0, 60, 1, 0}, {1, 60, 2, 0}, {2, 60, 3, 0}, {3, 60, 4, 0}, {4, 60, 1, 0}};
    const int64_t ticks[] = {0, 48000, 96000, 144000, 192000};
    const struct ds_time_axis clock = {48000, ticks, 1, 48000, NULL};
    const az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {2, 3, 2, 1, 0, 0}};
    const az_status_grid_ref_v1 grid = {epoch, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    struct ds_field fields[] = {ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 5),
                               ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, rows, 2),
                               ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, &grid, 1)};
    fields[0].clock = &clock;
    for (unsigned i = 0; i < 3; ++i) fields[i].info.scope_id = epoch;
    const az_status_coord_v1 intro = {0.5, 24000, 48000, 0}, verse = {2.5, 120000, 48000, 0};
    const az_status_coord_v1 gaps[] = {{1.5, 72000, 48000, 0}, {3.5, 168000, 48000, 0}};
    struct ds_path path = {.order = 1, .generation = 1, .phrase_provider = 10, .phrase_revision = 1, .grid = grid,
        .kind = DS_PATH_BASELINE, .endpoints = AZ_STATUS_OCCURRENCE_TO, .to = intro,
        .observed_window_samples = 1, .observed_window_rate = 48000, .from_window_samples = 1, .from_window_rate = 48000};
    assert(ds_publish_checked(&path, NULL, 0, deck, epoch, DS_BOUNDARY, ds_now(), fields, 3, NULL, NULL, 0, 0) == AZ_OK);
    struct ds_map map;
    assert(ds_map_at(&ds.deck[deck].current, &path, &map));
    const az_status_phrase_endpoint_v1 from = ds_endpoint(&map, AZ_PHRASE_ITEM, 0);
    const az_status_phrase_endpoint_v1 to = ds_endpoint(&map, AZ_PHRASE_ITEM, 1);
    const uint64_t topics[] = {DS_PHRASE_ROUTE, AZ_STATUS_TOPIC(AZ_STATUS_POSITION),
                               AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT), DS_PHRASE_ROUTE | DS_ACTION_ROUTES};
    struct action_trace traces[4];
    az_deck_subscription handles[4];
    for (unsigned i = 0; i < 4; ++i) {
        traces[i] = (struct action_trace) {.topics = topics[i]};
        const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1u << deck, 0, topics[i]};
        assert(ds_register(0, &watch, record_action, &traces[i], &handles[i]) == AZ_OK);
    }
    struct trace analysis = {0};
    const az_deck_watch_v1 analysis_watch = {sizeof analysis_watch, AZ_DATA_VERSION, 1u << deck, 0, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES)};
    az_deck_subscription analysis_handle;
    assert(ds_register(1, &analysis_watch, capture, &analysis, &analysis_handle) == AZ_OK);
    ds_drain(8);
    const unsigned masks[] = {0, AZ_STATUS_OCCURRENCE_FROM, AZ_STATUS_OCCURRENCE_TO, 3};
    for (unsigned i = 0; i < 8; ++i) {
        int gaps_only = i == 6, same_phrase = i == 7, proven = i < 3;
        path.order++;
        path.kind = DS_PATH_BASELINE;
        path.endpoints = AZ_STATUS_OCCURRENCE_TO;
        path.to = gaps_only ? gaps[0] : intro;
        path.departure = path.landing = NULL;
        assert(ds_publish_checked(&path, NULL, 0, deck, epoch, 0, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
        ds_drain(8);
        path.order++;
        path.generation++;
        path.kind = DS_PATH_ACTION;
        path.endpoints = masks[i >= 6 ? 3 : i % 3];
        path.from = gaps_only ? gaps[0] : intro;
        path.to = gaps_only ? gaps[1] : verse;
        if (same_phrase) path.to = (az_status_coord_v1) {0.25, 12000, 48000, 0};
        if (proven) {
            if (!(path.endpoints & AZ_STATUS_OCCURRENCE_FROM)) path.departure = &from;
            if (!(path.endpoints & AZ_STATUS_OCCURRENCE_TO)) path.landing = &to;
        }
        az_status_occurrence_v1 action = {.size = sizeof action, .version = AZ_DATA_VERSION,
            .kind = same_phrase ? AZ_STATUS_WRAP : AZ_STATUS_JUMP,
            .endpoints = path.endpoints, .ordinal = i + 1};
        if (path.endpoints & AZ_STATUS_OCCURRENCE_FROM) action.from = path.from;
        if (path.endpoints & AZ_STATUS_OCCURRENCE_TO) action.to = path.to;
        for (unsigned j = 0; j < 4; ++j) traces[j] = (struct action_trace) {.topics = topics[j], .mask = path.endpoints};
        assert(ds_publish_checked(&path, NULL, 0, deck, epoch, 0, ds_now(), NULL, 0, &action, NULL, 0, 0) == AZ_OK);
        ds_drain(8);
        assert(traces[1].occurrences == 1 && traces[2].occurrences == 1 && !traces[1].lost && !traces[2].lost);
        assert(traces[1].cut == traces[2].cut);
        if (proven) {
            assert(traces[0].pairs == 1 && traces[3].pairs == 1 && traces[3].occurrences == 1);
            assert(traces[0].cut == traces[1].cut && traces[3].cut == traces[1].cut);
        } else if (gaps_only || same_phrase) {
            assert(!traces[0].changes && !traces[0].recoveries && !traces[3].pairs && traces[3].occurrences == 1);
        } else {
            assert(traces[0].recoveries == 1 && traces[0].lost == DS_PHRASE_ROUTE);
            assert(traces[3].recoveries == 1 && traces[3].lost == (DS_PHRASE_ROUTE | DS_ACTION_ROUTES));
        }
    }
    for (unsigned i = 0; i < 4; ++i) assert(ds_release(0, handles[i]) == AZ_OK);
    assert(analysis.initial == 1 && !analysis.changes && !analysis.recoveries);
    assert(ds_release(1, analysis_handle) == AZ_OK);
    puts("PASS partial coordinates, independent phrase evidence and filtered action payloads");
}
static void phase_barriers(unsigned hz, uint64_t epoch) {
    const unsigned d = 3;
    az_beat beats[] = {{0, 60, 0, 0}, {1, 60, 0, 0}, {2, 60, 0, 0}, {3, 60, 0, 0}};
    int64_t ticks[] = {0, 100, 200, 300};
    struct ds_time_axis grid_clock = {100, ticks, 1, 100, NULL}, head_clock = {100, NULL, 1, 100, NULL};
    az_status_coord_v1 head = {0, 0, 100, 0};
    struct ds_field fields[] = {ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 4),
                                ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &head, 1)};
    fields[0].clock = &grid_clock;
    fields[1].clock = &head_clock;
    fields[0].info.scope_id = fields[1].info.scope_id = epoch;
    assert(ds_publish(d, epoch, DS_BOUNDARY, ds_now(), fields, 2, NULL, NULL, 0, 0) == AZ_OK);
    struct trace t = {0};
    az_deck_subscription h;
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1u << d, hz, AZ_STATUS_TOPIC(AZ_STATUS_PHASE)};
    assert(ds_register(5, &w, capture, &t, &h) == AZ_OK);
    ds_drain(8);
    struct ds_subscription *s = NULL;
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle == h) s = &ds.sub[i];
    assert(s && t.initial == 1);
    /* Keep the cadence closed deterministically; only a barrier may flush it. */
    s->cursor[d].last_motion_ns = ds_now() + UINT64_C(2000000000);
    head = (az_status_coord_v1) {0.25, 25, 100, 0};
    assert(ds_publish(d, epoch, DS_MOTION, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(!t.changes && due);
    az_status_occurrence_v1 jump = {.size = sizeof jump, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_JUMP, .ordinal = epoch * 10, .endpoints = 3, .from = head, .to = {1.25, 125, 100, 0}};
    head = jump.to;
    assert(ds_publish(d, epoch, 0, ds_now(), fields + 1, 1, &jump, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(t.changes == 1 && t.sequence == ds.deck[d].current.sequence && !t.actions && !due);
    jump.ordinal++;
    assert(ds_publish(d, epoch, 0, ds_now(), fields + 1, 1, &jump, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(t.changes == 1 && !due); /* same phase, already delivered */

    s->cursor[d].last_motion_ns = ds_now() + UINT64_C(2000000000);
    head = (az_status_coord_v1) {1.5, 150, 100, 0};
    assert(ds_publish(d, epoch, DS_MOTION, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    int direction = -1;
    struct ds_field change = ready(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, &direction, 1);
    change.info.scope_id = epoch;
    assert(ds_publish(d, epoch, 0, ds_now(), &change, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(t.changes == 2 && t.sequence == ds.deck[d].current.sequence && !due);

    s->cursor[d].last_motion_ns = ds_now() + UINT64_C(2000000000);
    head = (az_status_coord_v1) {1.75, 175, 100, 0};
    assert(ds_publish(d, epoch, DS_MOTION, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    jump.ordinal++;
    jump.from = head;
    jump.to = (az_status_coord_v1) {2.75, 275, 100, 0};
    head = jump.to;
    assert(ds_publish(d, epoch, 0, ds_now(), fields + 1, 1, &jump, NULL, 0, 0) == AZ_OK);
    for (unsigned i = 0; i <= DS_HISTORY; i++) {
        double bpm = 90 + (i % 2);
        change = ready(AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_CURRENT_BPM, &bpm, 1);
        change.info.scope_id = epoch;
        assert(ds_publish(d, epoch, 0, ds_now(), &change, 1, NULL, NULL, 0, 0) == AZ_OK);
    }
    ds_drain(8);
    assert(t.recoveries == 1 && t.lost == AZ_STATUS_TOPIC(AZ_STATUS_PHASE) && !due);
    assert(ds_release(5, h) == AZ_OK);
}
static void motion_generation_order(unsigned kind, unsigned hz) {
    const unsigned deck = 1;
    const uint64_t epoch = ds.deck[deck].last_epoch + 1;
    const struct ds_image *image = &ds.deck[deck].current;
    const az_beat beats[] = {{0, 60, 0, 0}, {1, 60, 0, 0}, {2, 60, 0, 0}, {3, 60, 0, 0}};
    const int64_t ticks[] = {0, 48000, 96000, 144000};
    const struct ds_time_axis clock = {48000, ticks, 1, 48000, NULL};
    const struct ds_time_axis head_clock = {48000, NULL, 1, 48000, NULL};
    const double duration = 3, precision = 1.0 / 48000;
    const int64_t duration_ticks = 144000, precision_ticks = 1;
    const struct ds_time_axis duration_clock = {48000, &duration_ticks, 1, 48000, NULL};
    const struct ds_time_axis precision_clock = {48000, &precision_ticks, 1, 48000, NULL};
    const az_status_coord_v1 start = {0.25, 12000, 48000, 0}, landing = {1.5, 72000, 48000, 0};
    const az_status_coord_v1 old_head = {0.5, 24000, 48000, 0}, new_head = {1.75, 84000, 48000, 0};
    struct ds_field fields[] = {
        ready(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, &start, 1),
        ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 4),
        ready(AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S, &duration, 1),
        ready(AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S, &precision, 1),
    };
    fields[0].clock = &head_clock;
    fields[1].clock = &clock;
    fields[2].clock = &duration_clock;
    fields[3].clock = &precision_clock;
    for (unsigned i = 0; i < 4; ++i) fields[i].info.scope_id = epoch;
    struct ds_path path = {.order = 1, .generation = 1, .kind = DS_PATH_BASELINE, .endpoints = 3,
        .from = start, .to = start, .observed_window_samples = 1, .observed_window_rate = 48000,
        .from_window_samples = 1, .from_window_rate = 48000};
    assert(ds_publish_checked(&path, NULL, 0, deck, epoch, DS_BOUNDARY, ds_now(), fields, 4, NULL, NULL, 0, 0) == AZ_OK);
    struct trace trace = {0};
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1u << deck, hz,
        AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE)};
    az_deck_subscription handle;
    assert(ds_register(0, &watch, capture, &trace, &handle) == AZ_OK);
    ds_drain(8);
    struct ds_cursor *cursor = NULL;
    for (unsigned i = 0; i < DS_SUBS; ++i)
        if (ds.sub[i].handle == handle) cursor = &ds.sub[i].cursor[deck];
    assert(cursor && trace.initial == 1);

    struct ds_path delayed = path, future = path;
    delayed.order = 4;
    delayed.kind = DS_PATH_TRAVEL;
    delayed.to = old_head;
    future.order = 3;
    future.generation = 2;
    future.kind = DS_PATH_TRAVEL;
    future.from = landing;
    future.to = new_head;
    fields[0].value = &new_head;
    assert(ds_publish_checked(&future, NULL, 0, deck, epoch, DS_MOTION, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_PENDING);
    ds_drain(8);
    assert(!trace.changes);
    path.order = 2;
    path.generation = 2;
    path.kind = DS_PATH_ACTION;
    path.to = landing;
    const az_status_occurrence_v1 action = {.size = sizeof action, .version = AZ_DATA_VERSION, .kind = kind,
        .ordinal = ds.deck[deck].last_ordinal + 1, .endpoints = 3, .from = path.from, .to = path.to};
    fields[0].value = &landing;
    assert(ds_publish_checked(&path, NULL, 0, deck, epoch, 0, ds_now(), fields, 1, &action, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(trace.actions == 1 && !trace.recoveries);
    fields[0].value = &old_head;
    assert(ds_publish_checked(&delayed, NULL, 0, deck, epoch, DS_MOTION, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_STALE);
    cursor->last_motion_ns = 0;
    ds_drain(8);
    assert(trace.actions == 1 && !trace.recoveries && !due);
    const az_status_coord_v1 *head = ds_value(&cursor->last, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    assert(head && head->rate_hz == 48000);
    const int order = compare_timestamps(head->samples, new_head.samples, head->rate_hz, head->rate_hz, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
    assert_seconds(*(const double *) ds_value(image, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S), 1.25, 48000, 1);
    assert_seconds(*(const double *) ds_value(image, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT), 0.75, 48000, 1);
    assert(ds_release(0, handle) == AZ_OK);
}
static void test_motion_generation_order(void) {
    for (unsigned kind = AZ_STATUS_SEEK; kind <= AZ_STATUS_WRAP; ++kind) {
        motion_generation_order(kind, 1);
        motion_generation_order(kind, 30);
    }
    puts("PASS all motion barriers reject a delayed older generation at 1 and 30 Hz");
}
static void grid_association(void) {
    const unsigned deck = 1;
    const struct ds_image *image = &ds.deck[deck].current;
    uint64_t epoch = ds.deck[deck].last_epoch + 1;
    const az_beat beats[] = {{0, 120, 1, 0}, {0.5, 120, 2, 0}, {1, 120, 3, 0}};
    const int64_t ticks[] = {0, 24000, 48000};
    const struct ds_time_axis clock = {48000, ticks, 1, 48000, NULL};
    const az_beat edited[] = {{0, 120, 1, 0}, {0.625, 120, 2, 0}, {1, 120, 3, 0}};
    const int64_t edited_ticks[] = {0, 30000, 48000};
    const struct ds_time_axis edited_clock = {48000, edited_ticks, 1, 48000, NULL};
    const az_phrase phrase = {0, 2, 1, 1, 0, 0};
    az_status_grid_ref_v1 current = {epoch, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    az_status_grid_ref_v1 saved = {epoch, 20, 1, AZ_PROV_FILE, AZ_STATUS_GRID_SAVED};
    struct ds_field fields[] = {
        ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, 3),
        ready(AZ_STATUS_GRID, AZ_STATUS_GRID_SAVED, beats, 3),
        ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, &phrase, 1),
        ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED, &phrase, 1),
        ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, &current, 1),
        ready(AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED_GRID, &saved, 1),
    };
    fields[0].clock = fields[1].clock = &clock;
    for (unsigned i = 0; i < 6; ++i) fields[i].info.scope_id = epoch;
    for (unsigned i = 1; i < 6; i += 2) {
        fields[i].info.info.provenance = AZ_PROV_FILE;
        fields[i].info.provider_id = saved.provider_id;
    }
    assert(ds_publish(deck, epoch, DS_BOUNDARY, ds_now(), fields, 6, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    const struct ds_cell *saved_cell = ds_ready(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED_GRID);
    assert(saved_cell);
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1u << deck, 0, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES)};
    struct trace phrases_only = {0};
    az_deck_subscription handle;
    assert(ds_register(0, &watch, capture, &phrases_only, &handle) == AZ_OK);
    ds_drain(8);
    fields[0].value = edited;
    fields[0].clock = &edited_clock;
    fields[0].info.info.revision++;
    assert(ds_publish(deck, epoch, 0, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(phrases_only.initial == 1 && phrases_only.changes == 1 && !phrases_only.recoveries);
    assert(!ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    assert(ds_ready(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED_GRID) == saved_cell);
    assert(ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT));
    assert(ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED));

    ++epoch;
    ++current.revision;
    saved.load_epoch = epoch;
    for (unsigned i = 0; i < 6; ++i) fields[i].info.scope_id = epoch;
    assert(ds_publish(deck, epoch, DS_BOUNDARY, ds_now(), fields, 6, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    assert(ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_SAVED_GRID));
    current.load_epoch = epoch;
    assert(ds_publish(deck, epoch, 0, ds_now(), fields + 4, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_value(image, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    assert(ds_release(0, handle) == AZ_OK);
    puts("PASS separate current and saved grid associations reject a prior load's reference");
}
static void selected_pins(void) {
    const unsigned d = 3;
    struct ds_cell *grid = ds.deck[d].current.field[AZ_STATUS_GRID][AZ_STATUS_GRID_CURRENT];
    assert(grid);
    unsigned refs = grid->refs;
    struct trace t = {0};
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1u << d, 1, AZ_STATUS_TOPIC(AZ_STATUS_LOOP)};
    az_deck_subscription handle;
    assert(ds_register(5, &w, capture, &t, &handle) == AZ_OK && grid->refs == refs);
    struct ds_subscription *s = NULL;
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle == handle) s = &ds.sub[i];
    uint64_t deadline = 0;
    struct ds_delivery call = {0};
    assert(s && ds_next(s, d, &call, ds_now(), &deadline));
    assert(call.event.view->loop && !call.event.view->grid && grid->refs == refs);
    assert(!call.pin.field[AZ_STATUS_GRID][AZ_STATUS_GRID_CURRENT]);
    assert(ds_release(5, handle) == AZ_OK && grid->refs == refs);
    ds_image_copy(&call.pin, NULL);
}
static void unsupported_invalidation(void) {
    const unsigned d = 3;
    uint64_t epoch = ds.deck[d].current.epoch + 1;
    struct ds_field f = ready(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE, NULL, 0);
    f.info.info.availability = AZ_AVAIL_UNSUPPORTED;
    f.info.scope_id = epoch;
    assert(ds_publish(d, epoch, DS_BOUNDARY, ds_now(), &f, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_publish(d, epoch, DS_NATIVE_INPUT, ds_now(), NULL, 0, NULL, NULL, AZ_STATUS_TOPIC(AZ_STATUS_LOOP), AZ_STATUS_CAPTURE_GAP) == AZ_OK);
    struct ds_cell *c = ds.deck[d].current.field[f.topic][f.field];
    assert(c && c->info.info.availability == AZ_AVAIL_UNSUPPORTED && c->info.scope_id == epoch);
    assert(ds_publish(d, epoch + 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    c = ds.deck[d].current.field[f.topic][f.field];
    assert(c && c->info.info.availability == AZ_AVAIL_UNSUPPORTED && c->info.scope_id == epoch + 1);
}
static void queued_loss(void) {
    const unsigned d = 2;
    struct ds_path seed = {.kind = DS_PATH_BASELINE, .order = 1, .generation = 1};
    assert(ds_publish_checked(&seed, NULL, 0, d, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    struct trace t = {0};
    az_deck_subscription h;
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1u << d, 1, DS_ACTION_ROUTES};
    assert(ds_register(5, &w, capture, &t, &h) == AZ_OK);
    ds_drain(8);
    struct ds_path jump = {.kind = DS_PATH_ACTION, .order = 3, .generation = 2};
    az_status_occurrence_v1 action = {.size = sizeof action, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_JUMP, .ordinal = 1};
    assert(ds_publish_checked(&jump, NULL, 0, d, 1, DS_NATIVE_INPUT | DS_HEAD_LOSS, ds_now(), NULL, 0,
                              &action, NULL, DS_ACTION_ROUTES, AZ_STATUS_CAPTURE_GAP) == AZ_PENDING);
    seed.order = 2;
    assert(ds_publish_checked(&seed, NULL, 0, d, 1, 0, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(ds.deck[d].path_order == 3 && t.recoveries == 1 && t.lost == DS_ACTION_ROUTES);
    assert(ds_release(5, h) == AZ_OK);
}
static void canonical_loop_reference(void) {
    const unsigned deck = 3;
    const uint64_t epoch = ds.deck[deck].current.epoch + 1;
    az_cue_entry row = {.size = sizeof row, .version = AZ_DATA_VERSION, .classification = AZ_CUE_MEMORY,
        .entry_id = 1, .type = AZ_CUE_LOOP, .valid = AZ_CUE_START | AZ_CUE_END,
        .start_s = 0, .end_s = 2, .saved = 1, .modified = 0, .auto_active = 0};
    int64_t ticks[] = {0, 88200};
    struct ds_time_axis clock = {44100, ticks, 1, 44100, NULL};
    az_status_cue_ref_v1 ref = {.load_epoch = epoch, .provider_id = 42, .collection_revision = 7,
        .provenance = AZ_PROV_NATIVE, .cue_field = AZ_STATUS_CUES_LIVE_MEMORY, .entry = row};
    ref.entry.start_s = -0.0; /* Representation does not establish row identity. */
    ref.entry.end_s = 2.0 + 1.0 / 44100;
    struct ds_field fields[] = {ready(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, &ref, 1),
        ready(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY, &row, 1)};
    fields[0].info.scope_id = fields[1].info.scope_id = epoch;
    fields[1].info.provider_id = 42;
    fields[1].info.info.revision = 7;
    fields[1].clock = &clock;
    /* Reference deliberately precedes its collection in the same commit. */
    assert(ds_publish(deck, epoch, DS_BOUNDARY, ds_now(), fields, 2, NULL, NULL, 0, 0) == AZ_OK);
    const struct ds_image *im = &ds.deck[deck].current;
    const az_status_cue_ref_v1 *published = ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    assert(published && !signbit(published->entry.start_s));
    assert_seconds(published->entry.end_s, 2, 44100, 1);
    /* Matching bounds and ID cannot attach the old reference to a new edition. */
    ++fields[1].info.info.revision;
    assert(ds_publish(deck, epoch, 0, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY));
    ref.collection_revision++;
    ref.entry.end_s = NAN; /* Copy the qualified row, never the redundant seconds. */
    ++fields[0].info.info.revision;
    assert(ds_publish(deck, epoch, 0, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_OK);
    published = ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    assert(published);
    assert_seconds(published->entry.end_s, 2, 44100, 1);
    ref.provider_id++;
    ++fields[0].info.info.revision;
    assert(ds_publish(deck, epoch, 0, ds_now(), fields, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY));
    puts("PASS canonical saved-loop row, exact edition fences and noncanonical timestamp copies");
}
int main(int argc, char **argv) {
    ds.enabled = 1;
    if (argc == 2 && !strcmp(argv[1], "grid-association")) {
        grid_association();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "partial-discontinuities")) {
        partial_discontinuities();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "motion-order")) {
        test_motion_generation_order();
        return 0;
    }
    source_clock_boundaries();
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    pressure();
    keys();
    phase_alignment();
    time_and_pins();
    phrase_motion();
    map_race(0, 0);
    map_race(1, 0);
    map_race(0, 1);
    map_race(1, 1);
    partial_discontinuities();
    phase_barriers(1, 10);
    phase_barriers(30, 12);
    test_motion_generation_order();
    grid_association();
    queued_loss();
    selected_pins();
    unsupported_invalidation();
    canonical_loop_reference();
    assert(wakes && ds.peak <= DS_BYTES);
    puts("status: filtered eviction, mixed occurrence, no-tick motion, source binding, PWAV pin, derived time, bidirectional phrase crossings, direct JUMP and confirmed END passed");
    return 0;
}

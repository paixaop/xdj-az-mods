/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../../src/common/azaltstack.h"
#include "sdk_status_engine.c"
static void ds_gui_interest(uint64_t topics) { (void) topics; }
static void ds_gui_load(unsigned deck) { (void) deck; }
static void ds_wake(void) {}
static void ds_deadline(uint64_t ns) { (void) ns; }
static void sdk_status_store_binding(unsigned d, uint64_t e, const az_track_ref *r, uint64_t cut) {
    (void) d;
    (void) e;
    (void) r;
    (void) cut;
}
static void sdk_status_store_interest(unsigned d, uint64_t topics) {
    (void) d;
    (void) topics;
}
static void sdk_status_store_reseed(unsigned d, uint64_t e, uint64_t t) {
    (void) d;
    (void) e;
    (void) t;
}
static int ds_live(int owner) { return owner >= 0; }
#include "../../src/common/azstatus_source.h"
static uint64_t last_source_pin;
static void ss_native_load(unsigned deck, uint64_t epoch, uint64_t token) {
    (void) deck;
    (void) epoch;
    last_source_pin = token;
}
uint64_t azjs_capture(unsigned deck, uint64_t unit, uint64_t epoch, uintptr_t pack, const unsigned char track[12]) {
    (void) deck;
    (void) unit;
    (void) epoch;
    (void) pack;
    (void) track;
    return 0;
}
static int g_ep147_build;
static int azm_install_island_hook_at(uintptr_t target, void *hook, void **original, const char *name) {
    (void) target;
    (void) hook;
    (void) original;
    (void) name;
    return 0;
}
#include "sdk_status_native.c"
static int file_ref_valid;
static az_status_cue_ref_v1 file_ref_target;
int azfile_status_ref(unsigned deck, uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *out) {
    if (!file_ref_valid || deck || epoch != file_ref_target.load_epoch || token != 55) return 0;
    *out = file_ref_target;
    return 1;
}
static uint32_t caps(unsigned d) {
    (void) d;
    return AZN_CAP_LOAD | AZN_CAP_LOOP | AZN_CAP_LTI | AZN_CAP_MOTION;
}
static struct ds_cell *cell(unsigned topic, unsigned field) { return ds.deck[0].current.field[topic][field]; }
/* These fixture coordinates are individual integer commits in their declared
 * clock, so their comparison window is one native sample. */
static int coord_at(const az_status_coord_v1 *point, int64_t expected) {
    return point && compare_timestamps(point->samples, expected, point->rate_hz, point->rate_hz, 1) == AZ_TIME_EQUAL;
}
static void test_stationary_terminal_reducer(void) {
    ds.enabled=1;
    const unsigned d=0;
    const uint64_t load=ds.deck[d].last_epoch+1,fence=ds_native_fence[d]+1;
    az_status_grid_ref_v1 grid={load,10,1,AZ_PROV_NATIVE,AZ_STATUS_GRID_CURRENT};
    az_beat beats[]={{0,120,1,0},{500.0/44100,120,2,0},{900.0/44100,120,3,0}};
    const int64_t ticks[]={0,500,900};
    const struct ds_time_axis clock={44100, ticks, 1, 44100, NULL};
    az_phrase rows[]={{0,1,1,1,0,0},{1,2,2,1,0,0}};
    az_status_coord_v1 head={.samples=1000,.rate_hz=44100};
    struct ds_field fields[]={
        {AZ_STATUS_GRID,AZ_STATUS_GRID_CURRENT,{.count=3},beats,&clock},
        {AZ_STATUS_PHRASES,AZ_STATUS_PHRASES_CURRENT,{.count=2},rows,NULL},
        {AZ_STATUS_PHRASES,AZ_STATUS_PHRASES_CURRENT_GRID,{.count=1},&grid,NULL},
        {AZ_STATUS_POSITION,AZ_STATUS_POSITION_HEAD,{.count=1},&head,NULL}};
    for(unsigned n=0;n<4;++n){
        fields[n].info.info=(az_component_info){AZ_AVAIL_READY,AZ_PROV_NATIVE,1,1};
        fields[n].info.scope=AZ_STATUS_LOAD_SCOPE;fields[n].info.scope_id=load;
        fields[n].info.provider_id=n==3?4:10;
    }
    assert(ds_publish(d,load,DS_BOUNDARY,1,fields,4,NULL,NULL,0,0)==AZ_OK);
    ds_native_epoch[d]=load;ds_native_fence[d]=fence;
    ds_native_motion_load[d]=load;ds_native_motion_order[d]=0;
    struct azstatus_native_receipt r={.size=sizeof r,.version=1,.kind=AZN_END,.deck=d,
        .unit=4,.epoch=fence,.load_generation=load,.a=1,.valid=1,.motion_order=1,.motion_generation=1,
        .map={.owner=6,.grid={.provider=10,.revision=1},.phrases={.provider=10,.revision=1}},
        .points={{AZN_COORD,44100,1000},{AZN_COORD,44100,1000}},
        .observed_window_samples=1,.observed_window_rate=44100,.from_window_samples=1,.from_window_rate=44100,
        .motion_flags=AZN_TERMINAL_CONFIRMED};
    ds_native_apply(&r);
    const az_status_phrase_endpoint_v1 *active=ds_value(&ds.deck[d].current,AZ_STATUS_PHRASE_TRANSITION,AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    assert(active&&active->kind==AZ_PHRASE_END);
    struct azstatus_native_receipt confirmed = r;
    for (unsigned repeat=0;repeat<4;++repeat) {
        r.kind=AZN_MOTION;r.a=AZN_MEASURE;r.valid=0;++r.motion_order;
        r.observed_window_samples=r.from_window_samples=64;r.motion_flags=0;
        ds_native_apply(&r);
        active=ds_value(&ds.deck[d].current,AZ_STATUS_PHRASE_TRANSITION,AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        assert(active&&active->kind==AZ_PHRASE_END);
    }
    /* The sample-sized equality proof must not accept one sample of travel,
     * a different writer generation, a replaced map, or an invalid clock. */
    for (unsigned mode=0;mode<4;++mode) {
        confirmed.motion_order=r.motion_order+1;
        ds_native_apply(&confirmed);
        r=confirmed;r.kind=AZN_MOTION;r.a=AZN_MEASURE;r.valid=0;++r.motion_order;
        r.observed_window_samples=r.from_window_samples=64;r.motion_flags=0;
        if (mode==0) ++r.points[1].samples;
        if (mode==1) ++r.motion_generation;
        if (mode==2) ++r.map.phrases.revision;
        if (mode==3) r.points[1].rate=0;
        if (mode==1) {
            const uint64_t before=ds.deck[d].current.sequence;
            assert(ds_native_motion(&r,r.motion_order)==AZ_PENDING);
            assert(ds.deck[d].current.sequence==before);
            r.motion_order=ds_native_motion_order[d];
            continue;
        }
        ds_native_apply(&r);
        active=ds_value(&ds.deck[d].current,AZ_STATUS_PHRASE_TRANSITION,AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        assert(!active||active->kind!=AZ_PHRASE_END);
    }
    puts("PASS confirmed END survives repeated stationary renders; travel, generation, map and invalid clock do not inherit it");
}

static unsigned memory_read_count, memory_release_count;
#include "sdkstatus_terminal_window_test.inc"
static int read_memory(uint64_t token, struct azn_memory_rows *values) {
    assert(token == 64);
    ++memory_read_count;
    values->count = 1;
    values->availability = AZ_AVAIL_READY;
    values->rows[0] = (az_cue_entry) {.size = sizeof(az_cue_entry), .version = AZ_DATA_VERSION, .entry_id = 1, .type = AZ_CUE_LOOP, .classification = AZ_CUE_MEMORY, .valid = AZ_CUE_START | AZ_CUE_END | AZ_CUE_COMMENT, .start_s = 0.01, .end_s = 0.02};
    values->clock_version = 1;
    values->points[0][0] = (struct azn_point) {AZN_COORD, 44100, 441};
    values->points[0][1] = (struct azn_point) {AZN_COORD, 44100, 882};
    strcpy(values->rows[0].comment, "held");
    return 1;
}
static void release_memory(uint64_t token) {
    assert(token == 64);
    ++memory_release_count;
}
static int seed_memory(unsigned deck, struct azstatus_native_receipt *out) {
    (void) deck;
    (void) out;
    return 0;
}
static const struct azn_memory_api *memory_api(void) {
    static const struct azn_memory_api api = {sizeof api, 1, read_memory, release_memory, seed_memory, NULL};
    return &api;
}
/* A queue burst that straddles a real boundary may collapse interior samples
 * on either side. It must retain the last genuine head before the crossing. */
static struct azstatus_native_receipt burst_rows[8];
static unsigned burst_next, burst_count, burst_pairs, burst_openings;
static int take_burst(unsigned d, struct azstatus_native_receipt *out) {
    assert(d == 1);
    if (burst_next == burst_count) return 0;
    *out = burst_rows[burst_next++];
    return 1;
}
static void on_burst(const az_deck_status_event_v1 *event, void *unused) {
    (void) unused;
    if (!event->phrase_transition) return;
    if (event->phrase_transition->from.kind == AZ_PHRASE_START) {
        assert(event->phrase_transition->to.kind == AZ_PHRASE_ITEM && event->phrase_transition->to.index == 0);
        assert(coord_at(&event->view->position->head, 0));
        ++burst_openings;
        return;
    }
    assert(event->phrase_transition->from.index == 0 && event->phrase_transition->to.index == 1);
    assert(coord_at(&event->view->position->head, 9));
    ++burst_pairs;
}
static void test_burst(void) {
    ds.enabled = 1;
    atomic_store(&ds_native_api, NULL);
    const unsigned d = 1;
    const uint64_t load = 50, fence = 17;
    az_status_grid_ref_v1 grid = {load, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    az_beat beats[] = {{0, 60, 1, 0}, {1, 60, 2, 0}, {2, 60, 3, 0}};
    const int64_t ticks[] = {0, 10, 20};
    const struct ds_time_axis clock = {10, ticks, 1, 10, NULL};
    az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {1, 2, 2, 1, 0, 0}};
    /* A native load adoption is only an opening candidate. Its first phrase
     * must start there; a saved cue in a later phrase never becomes START. */
    const int64_t nonzero_ticks[] = {13097, 57197, 101297};
    const struct ds_time_axis nonzero_clock = {44100, nonzero_ticks, 1, 44100, NULL};
    const struct ds_map nonzero_map = {rows, &nonzero_clock, 2, 10, 1, grid};
    az_status_coord_v1 opening_point = {.samples = 13097, .rate_hz = 44100};
    az_status_coord_v1 middle_point = {.samples = 57197, .rate_hz = 44100};
    assert(ds_at(&nonzero_map, &opening_point, 1, 0, 44100, 1).kind == AZ_PHRASE_START);
    az_status_phrase_endpoint_v1 middle = ds_at(&nonzero_map, &middle_point, 1, 0, 44100, 1);
    assert(middle.kind == AZ_PHRASE_ITEM && middle.index == 1);
    az_status_coord_v1 head = {.samples = 0, .rate_hz = 10};
    struct ds_field fields[] = {
        {AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, {.count = 3}, beats, &clock},
        {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, {.count = 2}, rows, NULL},
        {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, {.count = 1}, &grid, NULL},
        {AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, {.count = 1}, &head, NULL}};
    for (unsigned n = 0; n < 4; n++) {
        fields[n].info.info = (az_component_info) {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, 1};
        fields[n].info.scope = AZ_STATUS_LOAD_SCOPE;
        fields[n].info.scope_id = load;
        fields[n].info.provider_id = n == 3 ? 4 : 10;
    }
    assert(ds_publish(d, load, DS_BOUNDARY, 1, fields, 4, NULL, NULL, 0, 0) == AZ_OK);
    ds_native_epoch[d] = load;
    ds_native_fence[d] = fence;
    struct azstatus_native_receipt initial = {.size = sizeof initial, .version = 1, .kind = AZN_MOTION, .a = AZN_MOTION_SEED, .deck = d, .unit = 4, .epoch = fence, .load_generation = load, .motion_generation = 1, .motion_order = 1, .map = {.owner = 6, .grid = {.provider = 10, .revision = 1}, .phrases = {.provider = 10, .revision = 1}}, .points = {{AZN_INVALID, 0, 0}, {AZN_COORD, 10, 0}}, .motion_flags = AZN_AT_OPENING};
    initial.observed_window_samples = initial.from_window_samples = 1;
    initial.observed_window_rate = initial.from_window_rate = 10;
    ds_native_apply(&initial);
    const az_status_phrase_endpoint_v1 *active = ds_value(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    assert(active && active->kind == AZ_PHRASE_START);
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1u << d, 1, DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_POSITION)};
    az_deck_subscription handle;
    assert(ds_register(1, &watch, on_burst, NULL, &handle) == AZ_OK);
    ds_drain(8);
    /* A registration or writer recovery may deliver a later stationary cut
     * after the initial map. It must not silently enter ITEM 0 on that cut. */
    initial.a = AZN_MEASURE;
    initial.points[0] = initial.points[1];
    initial.c = 1;
    ++initial.motion_order;
    ds_native_apply(&initial);
    ds_drain(8);
    active = ds_value(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    assert(active && active->kind == AZ_PHRASE_START && !burst_openings);
    initial.c = 0;
    /* A map cut may have missed HEAD ownership. Its next proven stationary
     * measurement explicitly recovers the opening without a reseed bit. */
    initial.motion_flags = AZN_AT_OPENING;
    assert(ds_publish(d, load, DS_NATIVE_INPUT | DS_HEAD_LOSS, 1, NULL, 0, NULL, NULL,
                      DS_PHRASE_ROUTE, AZ_STATUS_CAPTURE_GAP) == AZ_OK);
    ++initial.motion_order;
    ds_native_apply(&initial);
    ds_drain(8);
    assert(!burst_openings);
    active = ds_value(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    assert(active && active->kind == AZ_PHRASE_START);
    initial.motion_flags = AZN_BEGAN_PLAY;
    ++initial.motion_order;
    ds_native_apply(&initial);
    ds_drain(8);
    assert(burst_openings == 1);
    ds_native_apply(&initial);
    ds_drain(8);
    assert(burst_openings == 1);
    const int samples[] = {0, 3, 6, 9, 10, 11, 12};
    for (unsigned i = 0; i < 6; i++) {
        burst_rows[i] = (struct azstatus_native_receipt) {.size = sizeof burst_rows[0], .version = 1, .kind = AZN_MOTION, .deck = d, .epoch = fence, .load_generation = load, .unit = 4, .a = AZN_MEASURE, .motion_generation = 1, .motion_order = i + 5, .observed_ns = 5000000u + i * 2000000u, .points = {{AZN_COORD, 10, samples[i]}, {AZN_COORD, 10, samples[i + 1]}}, .map = {.owner = 6, .grid = {.provider = 10, .revision = 1}, .phrases = {.provider = 10, .revision = 1}}};
        burst_rows[i].observed_window_samples = burst_rows[i].from_window_samples = 1;
        burst_rows[i].observed_window_rate = burst_rows[i].from_window_rate = 10;
    }
    assert(ds_native_interior(burst_rows, 6, d) == 3);
    struct azstatus_native_receipt pristine = burst_rows[1];
    burst_rows[1].points[1].samples = 0;
    assert(ds_native_interior(burst_rows, 2, d) == 1); /* reversal */
    burst_rows[1] = pristine;
    burst_rows[1].kind = AZN_PLAY;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    burst_rows[1].size = offsetof(struct azstatus_native_receipt, history_only);
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    struct ds_path unknown = {0};
    ds_native_windows(&unknown, &burst_rows[1]);
    assert(!unknown.observed_window_samples && !unknown.from_window_samples);
    burst_rows[1] = pristine;
    burst_rows[1].observed_window_samples = 0;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    burst_rows[1].from_window_rate = 0;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    ++burst_rows[1].motion_order;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    ++burst_rows[1].unit;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    ++burst_rows[1].load_generation;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    ++burst_rows[1].map.owner;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    burst_rows[1].observed_ns += 16000000;
    assert(ds_native_interior(burst_rows, 2, d) == 1);
    burst_rows[1] = pristine;
    struct azstatus_native_v1 queue = {.take = take_burst};
    burst_count = 6;
    assert(ds_native_drain(&queue, d) == 6 && ds_native_drain(&queue, d) == 0 && ds_native_motion_order[d] == 10);
    ds_drain(8);
    assert(burst_pairs == 1);
    const az_status_coord_v1 *latest = ds_value(&ds.deck[d].current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    assert(coord_at(latest, 12));
    burst_rows[6] = burst_rows[5];
    ++burst_rows[6].motion_order;
    burst_rows[6].points[0].samples = 12;
    burst_rows[6].points[1].samples = 13;
    ++burst_count;
    assert(ds_native_drain(&queue, d) == 1);
    latest = ds_value(&ds.deck[d].current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    assert(coord_at(latest, 13) && ds_native_motion_order[d] == 11);
}
static void release_map(const struct aznm_packet *packet) { (void) packet; }
static int unused_map_read(const struct aznm_component *part, unsigned kind, void *rows, uint32_t capacity, uint32_t *count) {
    (void) part;
    (void) kind;
    (void) rows;
    (void) capacity;
    (void) count;
    assert(0);
    return AZ_INVALID;
}
static const struct azn_maps_api *order_maps(void) {
    static const struct azn_maps_api api = {.size = sizeof api, .version = 1, .read = unused_map_read, .release = release_map};
    return &api;
}
static void ignore_interest(unsigned d, uint64_t topics) {
    (void) d;
    (void) topics;
}
static int legacy_map_rows(const struct aznm_component *part, unsigned kind, void *rows, uint32_t capacity, uint32_t *count) {
    const az_beat beats[] = {{0, 60, 1, 0}, {1, 60, 2, 0}, {2, 60, 3, 0}};
    const az_phrase phrases[] = {{0, 2, 1, 1, 0, 0}};
    assert(part->content);
    *count = kind == AZNM_GRID ? 3 : 1;
    if (!rows) return AZ_OK;
    if (capacity < *count) return AZ_LIMIT;
    memcpy(rows, kind == AZNM_GRID ? (const void *) beats : (const void *) phrases,
           kind == AZNM_GRID ? sizeof beats : sizeof phrases);
    return AZ_OK;
}
static const struct azn_maps_api *legacy_maps(void) {
    static const struct azn_maps_api api = {.size = offsetof(struct azn_maps_api, read_samples), .version = 1, .read = legacy_map_rows, .release = release_map};
    return &api;
}
static void test_legacy_map_clock(void) {
    const unsigned d = 2;
    struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .maps = legacy_maps, .interest = ignore_interest};
    atomic_store(&ds_native_api, &api);
    ds_native_epoch[d] = 19;
    ds_native_fence[d] = 46;
    assert(ds_publish(d, 19, DS_BOUNDARY, 1, NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    struct azstatus_native_receipt r = {.size = sizeof r, .version = 1, .kind = AZN_MOTION, .a = AZN_MOTION_SEED, .deck = d, .unit = 6, .epoch = 46, .load_generation = 19, .motion_order = 1, .motion_generation = 1, .points = {{AZN_INVALID, 0, 0}, {AZN_COORD, 100, 50}}, .observed_window_samples = 1, .observed_window_rate = 100};
    assert(ds_native_motion(&r, 1) == AZ_OK);
    r.kind = AZN_MAPS;
    r.motion_order = 2;
    r.map = (struct aznm_packet) {.owner = 69, .changed = AZNM_GRID | AZNM_PHRASES | AZNM_PAIR, .grid = {.content = 1, .provider = 70, .revision = 3, .sample_rate = 100, .availability = AZ_AVAIL_READY}, .phrases = {.content = 2, .provider = 71, .revision = 4, .availability = AZ_AVAIL_READY}};
    ds_native_map(&r);
    const struct ds_cell *grid = ds_ready(&ds.deck[d].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    assert(grid && grid->info.count == 3 && !grid->clock.ticks);
    assert(!ds_ready(&ds.deck[d].current, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT));
    assert(!ds_ready(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE));
    r.motion_order = 0; /* unchanged retained map replay without another head cut */
    uint64_t sequence = ds.deck[d].current.sequence;
    ds_native_map(&r);
    assert(ds.deck[d].current.field[AZ_STATUS_GRID][AZ_STATUS_GRID_CURRENT] == grid);
    assert(ds.deck[d].current.sequence == sequence);
    atomic_store(&ds_native_api, NULL);
}
static unsigned ordered_actions;
static void on_ordered_action(const az_deck_status_event_v1 *event, void *unused) {
    (void) unused;
    if (event->occurrence) {
        assert(event->occurrence->ordinal == 1 && event->occurrence->kind == AZ_STATUS_JUMP);
        ++ordered_actions;
    }
}
static void test_native_order(void) {
    const unsigned d = 3;
    struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .maps = order_maps, .interest = ignore_interest};
    atomic_store(&ds_native_api, &api);
    ds_native_epoch[d] = 8;
    ds_native_fence[d] = 33;
    assert(ds_publish(d, 8, DS_BOUNDARY, 1, NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    struct azstatus_native_receipt r = {.kind = AZN_MOTION, .a = AZN_MOTION_SEED, .deck = d, .size = sizeof r, .version = 1, .epoch = 33, .load_generation = 8, .unit = 5, .motion_order = 1, .motion_generation = 1, .points = {{AZN_INVALID, 0, 0}, {AZN_COORD, 100, 0}}};
    r.observed_window_samples = r.from_window_samples = 1;
    r.observed_window_rate = r.from_window_rate = 100;
    assert(ds_native_motion(&r, 1) == AZ_OK && ds_native_motion_order[d] == 1);
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1u << d, 1, AZ_STATUS_TOPIC(AZ_STATUS_POSITION)};
    az_deck_subscription sub;
    assert(ds_register(1, &watch, on_ordered_action, NULL, &sub) == AZ_OK);
    ds_drain(8);
    struct azstatus_native_receipt map = {.kind = AZN_MAPS, .deck = d, .size = sizeof map, .epoch = 33, .load_generation = 8, .motion_order = 3, .motion_generation = 1, .map = {.changed = AZNM_PAIR}};
    ds_native_map(&map);
    ds_native_map(&map);
    assert(ds.deck[d].path_order == 1 && ds_native_motion_order[d] == 1);
    r.a = AZN_MEASURE;
    r.points[0] = r.points[1];
    r.points[1].samples = 25;
    r.motion_order = 2;
    assert(ds_native_motion(&r, 2) == AZ_OK && ds.deck[d].path_order == 3 && ds_native_motion_order[d] == 3);
    uint64_t cut = ds.deck[d].current.sequence;
    ds_native_map(&map);
    assert(ds.deck[d].current.sequence == cut && ds_native_motion_order[d] == 3);
    r.points[0] = r.points[1];
    r.points[1].samples = 50;
    r.motion_order = 4;
    assert(ds_native_motion(&r, 4) == AZ_OK && ds_native_motion_order[d] == 4 && !ds.metrics[d].capture_gaps);
    r.a = AZN_JUMP;
    r.points[0] = r.points[1];
    r.points[1].samples = 100;
    r.motion_order = 5;
    r.motion_generation = 2;
    r.occurrence_ordinal = 1;
    assert(ds_native_motion(&r, 5) == AZ_OK);
    assert(ds_native_motion(&r, 5) == AZ_STALE);
    ds_drain(8);
    assert(ordered_actions == 1 && ds_native_motion_order[d] == 5);
    r.a = AZN_MEASURE;
    r.points[0] = r.points[1];
    r.points[1].samples = 125;
    r.motion_order = 6;
    r.occurrence_ordinal = 0;
    cut = ds.deck[d].current.sequence;
    ds.deck[d].current.sequence = UINT64_MAX;
    assert(ds_native_motion(&r, 6) == AZ_LIMIT && ds_native_motion_order[d] == 5);
    ds.deck[d].current.sequence = cut;
    assert(ds_native_motion(&r, 6) == AZ_OK && ds_native_motion_order[d] == 6);
    assert(ds_release(1, sub) == AZ_OK);
    atomic_store(&ds_native_api, NULL);
}
#include "sdkstatus_duration_reduce_test.inc"
static void test_original_bpm_reducer(void) {
    const unsigned d = 3;
    struct ds_image *im = &ds.deck[d].current;
    struct azstatus_native_receipt r = {.size = sizeof r, .version = 1, .kind = AZN_LOAD,
        .deck = d, .unit = 98, .epoch = 200001, .load_generation = 200002, .valid = 1,
        .a = AZN_LOADED, .original_bpm = 129.25, .original_bpm_state = AZN_BPM_READY};
    ds_native_apply(&r);
    const struct ds_cell *bpm = ds_ready(im, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM);
    assert(bpm && *(const double *)bpm->data == 129.25 && bpm->info.scope == AZ_STATUS_LOAD_SCOPE &&
           bpm->info.scope_id == 200002 && bpm->info.info.provenance == AZ_PROV_NATIVE);
    struct azstatus_native_receipt old = r;
    ++r.epoch;
    r.load_generation = 0;
    r.a = AZN_LOADING;
    ds_native_apply(&r);
    assert(!ds_ready(im, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM));
    ds_native_apply(&old);
    assert(!ds_ready(im, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM));
    r.a = AZN_LOADED;
    r.load_generation = 200003;
    r.original_bpm_state = AZN_BPM_EMPTY;
    ds_native_apply(&r);
    bpm = im->field[AZ_STATUS_TEMPO][AZ_STATUS_TEMPO_ORIGINAL_BPM];
    assert(bpm && bpm->info.info.availability == AZ_AVAIL_EMPTY && bpm->info.scope_id == 200003);
    ++r.epoch;
    ++r.load_generation;
    r.original_bpm_state = AZN_BPM_READY;
    r.original_bpm = INFINITY;
    ds_native_apply(&r);
    bpm = im->field[AZ_STATUS_TEMPO][AZ_STATUS_TEMPO_ORIGINAL_BPM];
    assert(bpm && bpm->info.info.availability == AZ_AVAIL_STALE);
    ++r.epoch;
    ++r.load_generation;
    r.original_bpm = 140;
    r.size = offsetof(struct azstatus_native_receipt, original_bpm);
    ds_native_apply(&r); /* Older provider receipts have no original-BPM tail. */
    assert(!ds_ready(im, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM));
    puts("native original BPM reducer: load scoping, absence, invalid data and old receipts passed");
}

static void test_native_cue_clocks(void) {
    az_cue_entry rows[2] = {{.size = sizeof(az_cue_entry), .version = AZ_DATA_VERSION,
        .classification = AZ_CUE_MEMORY, .entry_id = 1, .type = AZ_CUE_LOOP, .valid = AZ_CUE_START | AZ_CUE_END},
        {.size = sizeof(az_cue_entry), .version = AZ_DATA_VERSION, .classification = AZ_CUE_MEMORY,
         .entry_id = 2, .type = AZ_CUE_LOOP, .valid = AZ_CUE_START | AZ_CUE_END}};
    struct azn_point points[2][2] = {{{AZN_COORD, 44100, 44101}, {AZN_COORD, 44100, 88201}},
                                    {{AZN_COORD, 48000, 48001}, {AZN_COORD, 48000, 96001}}};
    int64_t ticks[4];
    struct ds_time_axis clock = {0};
    assert(ds_native_cue_clock(2, rows, points, ticks, &clock));
    assert(clock.units_per_second == 7056000 && !clock.window_rate && !clock.window_samples);
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            int order = ds_compare_source_times(ticks[2 * i + j], clock.units_per_second,
                points[i][j].samples, points[i][j].rate, points[i][j].rate, 1);
            assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
        }
    assert(!ds_axis_equal(&clock, &clock, 0)); /* no guessed mixed-bank observation window */
    const unsigned deck = 3;
    const uint64_t epoch = ds.deck[deck].current.epoch + 1;
    struct ds_field f = {.topic = AZ_STATUS_CUES, .field = AZ_STATUS_CUES_LIVE_MEMORY,
        .info = {.info = {AZ_AVAIL_READY, AZ_PROV_NATIVE, 9, 1}, .scope = AZ_STATUS_LOAD_SCOPE,
                 .scope_id = epoch, .provider_id = 71, .count = 2}, .value = rows, .clock = &clock};
    assert(ds_publish(deck, epoch, DS_BOUNDARY, 1, &f, 1, NULL, NULL, 0, 0) == AZ_OK);
    struct ds_image pinned = {0};
    ds_image_select(&pinned, &ds.deck[deck].current, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    memset(ticks, 0, sizeof ticks);
    const struct ds_cell *c = ds_ready(&pinned, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY);
    assert(c && c->clock.ticks);
    assert(ds_compare_source_times(c->clock.ticks[3], c->clock.units_per_second, 96001, 48000, 48000, 1) == AZ_TIME_EQUAL);
    assert(ds_publish(deck, epoch + 1, DS_BOUNDARY, 2, NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_compare_source_times(c->clock.ticks[0], c->clock.units_per_second, 44101, 44100, 44100, 1) == AZ_TIME_EQUAL);
    ds_image_copy(&pinned, NULL);
    points[0][0].rate = 0;
    assert(!ds_native_cue_clock(2, rows, points, ticks, &clock));
    points[0][0].rate = 44100;
    points[0][0].samples = -1;
    assert(!ds_native_cue_clock(2, rows, points, ticks, &clock));
    puts("PASS native cue integer clocks, mixed-rate exact conversion, invalid clocks and queued lifetime");
}
static void test_source_authority(void) {
    const struct ds_image *image = &ds.deck[0].current;
    struct azstatus_native_receipt load = {.size = sizeof load, .kind = AZN_LOAD, .a = AZN_LOADED,
        .unit = 4, .epoch = 1, .load_generation = 1, .valid = 1, .source_kind = AZ_STATUS_SOURCE_USB2,
        .source_pin = 11};
    ds_native_apply(&load);
    assert(last_source_pin == 11);
    const az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 2, 3, 4, 0};
    const int usb1 = AZ_STATUS_SOURCE_USB1;
    struct ds_field binding[] = {
        {AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, {.count = 1}, &ref, NULL},
        {AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, {.count = 1}, &usb1, NULL},
    };
    for (unsigned i = 0; i < 2; ++i) {
        binding[i].info.info = (az_component_info) {AZ_AVAIL_READY, AZ_PROV_DERIVED, 1, 1};
        binding[i].info.scope = AZ_STATUS_LOAD_SCOPE;
        binding[i].info.scope_id = 1;
        binding[i].info.provider_id = 11;
    }
    assert(ds_publish(0, 1, 0, 1, binding, 2, NULL, NULL, 0, 0) == AZ_OK);
    assert(*(const int *) ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE) == AZ_STATUS_SOURCE_USB2);
    load.epoch = load.load_generation = 2;
    load.source_kind = AZ_STATUS_SOURCE_STREAMING;
    load.source_pin = 0;
    ds_native_apply(&load);
    assert(!last_source_pin && !ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK));
    assert(*(const int *) ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE) == AZ_STATUS_SOURCE_STREAMING);

    load.epoch = 3;
    load.a = AZN_LOADING;
    ds_native_apply(&load);
    assert(!image->epoch && !ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE));
    load.load_generation = 3;
    load.a = AZN_LOADED;
    load.source_kind = 0;
    load.source_pin = 12;
    ds_native_apply(&load);
    assert(last_source_pin == 12 && !ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE));
    for (unsigned i = 0; i < 2; ++i) {
        binding[i].info.scope_id = 3;
        binding[i].info.provider_id = 12;
    }
    assert(ds_publish(0, 3, 0, 3, binding, 2, NULL, NULL, 0, 0) == AZ_OK);
    const int *resolved = ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE);
    assert(resolved && *resolved == AZ_STATUS_SOURCE_USB1);
    assert(ds_retire_file_binding(0, 3, &ref) == AZ_OK);
    assert(!ds_value(image, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE));
    puts("PASS source authority follows its load and supports an independently qualified FILE fallback");
}
struct loop_seed_trace {
    unsigned count;
    uint64_t sequence;
};
static void loop_seed_callback(const az_deck_status_event_v1 *event, void *user) {
    struct loop_seed_trace *trace = user;
    const uint64_t engaged = az_status_field_bit(AZ_STATUS_LOOP_ENGAGED);
    assert(event->sequence > trace->sequence && !event->deck);
    assert(event->topics == AZ_STATUS_TOPIC(AZ_STATUS_LOOP) && !event->lost_topics);
    assert(!event->occurrence && !event->phrase_transition);
    const az_status_loop_v1 *loop = event->view->loop;
    assert(loop && (loop->header.changed & engaged));
    if (trace->count == 0) {
        assert(event->load_epoch == 1 && event->kind == AZ_STATUS_INITIAL && !(loop->header.valid & engaged));
    } else if (trace->count == 4) {
        assert(!event->load_epoch && event->kind == AZ_STATUS_CHANGE && !(loop->header.valid & engaged));
        assert(loop->engaged_info.info.availability == AZ_AVAIL_STALE);
    } else {
        assert(trace->count <= 3 && event->load_epoch == 1 && event->kind == AZ_STATUS_CHANGE && (loop->header.valid & engaged));
        assert(loop->engaged == (trace->count == 2));
    }
    trace->sequence = event->sequence;
    ++trace->count;
}
static void test_delayed_loop_seed(void) {
    ds.enabled = 1;
    struct azstatus_native_receipt r = {.size = sizeof r, .kind = AZN_LOAD,
        .a = AZN_LOADED, .valid = 1, .unit = 4, .epoch = 1, .load_generation = 1};
    ds_native_apply(&r);
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1, 0, AZ_STATUS_TOPIC(AZ_STATUS_LOOP)};
    struct loop_seed_trace trace = {0};
    az_deck_subscription handle;
    assert(ds_register(0, &watch, loop_seed_callback, &trace, &handle) == AZ_OK);

    r.kind = AZN_LOOP;
    r.points[0] = (struct azn_point) {AZN_COORD, 48000, 0};
    r.points[1] = (struct azn_point) {AZN_COORD, 48000, 48000};
    r.a = 0;
    ds_native_apply(&r);
    r.a = 1;
    r.points[2] = r.points[0];
    r.points[3] = r.points[1];
    ds_native_apply(&r);
    ds_native_apply(&r); /* An equal owned baseline adds no publication. */
    ds_drain(8);
    assert(trace.count == 3);

    r.a = 0;
    r.points[2] = r.points[3] = (struct azn_point) {AZN_ABSENT, 0, 0};
    ds_native_apply(&r);
    ds_native_apply(&r); /* The next delayed read observes the new EXIT. */
    ds_drain(8);
    assert(trace.count == 4);
    r.kind = AZN_LOAD;
    r.a = AZN_LOADING;
    r.epoch = 2;
    r.load_generation = 0;
    ds_native_apply(&r);
    ds_drain(8);
    assert(trace.count == 5 && ds_release(0, handle) == AZ_OK);
    puts("PASS unknown loop INITIAL, delayed seed and preserved EXIT/RELOOP history");
}
struct slip_barrier_trace {
    unsigned kind, calls;
};
static void slip_barrier_callback(const az_deck_status_event_v1 *event, void *user) {
    struct slip_barrier_trace *trace = user;
    const az_status_position_v1 *position = event->view->position;
    if (event->kind == AZ_STATUS_INITIAL) {
        assert(position->header.valid & az_status_field_bit(AZ_STATUS_POSITION_SLIP_HEAD));
        assert(coord_at(&position->slip_head, 125));
        return;
    }
    assert(event->kind == AZ_STATUS_CHANGE && !event->lost_topics);
    if (!trace->calls++) {
        assert(event->occurrence && event->occurrence->kind == trace->kind);
        assert(!(position->header.valid & az_status_field_bit(AZ_STATUS_POSITION_SLIP_HEAD)));
        assert(coord_at(&position->head, 200));
    } else {
        assert(trace->calls == 2 && !event->occurrence);
        assert(position->header.valid & az_status_field_bit(AZ_STATUS_POSITION_SLIP_HEAD));
        assert(coord_at(&position->slip_head, 300));
    }
}
static void test_slip_barrier(void) {
    ds.enabled = 1;
    const unsigned native_kinds[] = {AZN_SEEK, AZN_JUMP, AZN_WRAP};
    const unsigned public_kinds[] = {AZ_STATUS_SEEK, AZ_STATUS_JUMP, AZ_STATUS_WRAP};
    const unsigned rates[] = {1, 30};
    uint64_t ordinal = 0;
    for (unsigned k = 0; k < sizeof native_kinds / sizeof native_kinds[0]; k++) {
        for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
            uint64_t epoch = ds.deck[0].last_epoch + 1;
            struct azstatus_native_receipt head = {.size = sizeof head, .version = 1, .kind = AZN_LOAD,
                .a = AZN_LOADED, .valid = 1, .unit = 4, .epoch = epoch, .load_generation = epoch};
            ds_native_apply(&head);
            head.kind = AZN_MOTION;
            head.a = AZN_MOTION_SEED;
            head.motion_order = head.motion_generation = 1;
            head.points[1] = (struct azn_point) {AZN_COORD, 44100, 100};
            head.observed_window_samples = 1;
            head.observed_window_rate = 44100;
            ds_native_apply(&head);
            struct azstatus_native_receipt slip = head;
            slip.kind = AZN_SLIP_HEAD;
            slip.attempt = 18;
            slip.valid = AZ_AVAIL_READY;
            slip.b = 1;
            slip.points[0] = (struct azn_point) {AZN_COORD, 44100, 125};
            ds_native_apply(&slip);
            az_deck_watch_v1 watch = {.size = sizeof watch, .version = AZ_DATA_VERSION,
                .deck_mask = 1, .topics = AZ_STATUS_TOPIC(AZ_STATUS_POSITION), .position_hz = rates[i]};
            az_deck_subscription handle;
            struct slip_barrier_trace trace = {.kind = public_kinds[k]};
            assert(ds_register(1, &watch, slip_barrier_callback, &trace, &handle) == AZ_OK);
            assert(ds_drain(8) == 1);
            slip.b = 0;
            slip.points[0].samples = 150;
            ds_native_apply(&slip); /* A previous-generation sample is still coalesced. */
            assert(ds_ready(&ds.deck[0].current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD));
            struct azstatus_native_receipt delayed = slip;
            slip.motion_generation = 2;
            slip.points[0].samples = 300;
            slip.b = 1;
            ds_native_apply(&slip); /* The new-generation sample arrived ahead of its barrier. */
            const struct ds_cell *current = ds_ready(&ds.deck[0].current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD);
            assert(current && coord_at((const void *) current->data, 150));
            head.a = native_kinds[k];
            head.motion_order = head.motion_generation = 2;
            head.occurrence_ordinal = ++ordinal;
            head.points[0] = head.points[1];
            head.points[1].samples = 200;
            head.from_window_samples = head.observed_window_samples;
            head.from_window_rate = head.observed_window_rate;
            ds_native_apply(&head);
            delayed.points[0].samples = 175;
            ds_native_apply(&delayed);
            ds_drain(8);
            assert(trace.calls == 2);
            current = ds_ready(&ds.deck[0].current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD);
            assert(current && coord_at((const void *) current->data, 300));
            assert(ds_release(1, handle) == AZ_OK);

            /* A second parked validity cut cannot be coalesced silently. */
            slip.motion_generation = 3;
            ds_native_apply(&slip);
            slip.valid = AZ_AVAIL_EMPTY;
            ds_native_apply(&slip);
            slip.valid = AZ_AVAIL_READY;
            ds_native_apply(&slip);
            head.a = AZN_REPOSITION;
            head.motion_order = head.motion_generation = 3;
            head.occurrence_ordinal = 0;
            ds_native_apply(&head);
            const struct ds_deck *deck = &ds.deck[0];
            const struct ds_record *last = &deck->history[(deck->next + DS_HISTORY - 1) % DS_HISTORY];
            assert(last->lost == AZ_STATUS_TOPIC(AZ_STATUS_POSITION) && last->reason == AZ_STATUS_CAPTURE_GAP);
            current = ds_ready(&deck->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD);
            assert(current && coord_at((const void *) current->data, 300));
        }
    }
    puts("PASS Slip samples hold for SEEK, JUMP and WRAP and reject delayed prior-generation copies at 1 and 30 Hz");
}
static void test_early_native_head(void) {
    struct ds_deck *deck = &ds.deck[0];
    struct azstatus_native_receipt load = {.size = sizeof load, .version = 1, .kind = AZN_LOAD,
        .a = AZN_LOADED, .valid = 1, .unit = 4, .epoch = 5, .load_generation = 7};
    ds_native_apply(&load);
    struct azstatus_native_receipt motion = load;
    motion.kind = AZN_MOTION;
    motion.a = AZN_MOTION_SEED;
    motion.motion_order = motion.motion_generation = 1;
    motion.points[1] = (struct azn_point) {AZN_COORD, 44100, 100};
    motion.observed_window_samples = motion.from_window_samples = 1;
    motion.observed_window_rate = motion.from_window_rate = 44100;
    ds_native_apply(&motion);
    const uint64_t before = deck->current.sequence;

    motion.motion_generation = 2;
    motion.motion_order = 4;
    motion.a = AZN_MEASURE;
    motion.points[0] = (struct azn_point) {AZN_COORD, 44100, 300};
    motion.points[1].samples = 400;
    ds_native_apply(&motion);
    motion.motion_order = 3;
    motion.points[0].samples = 200;
    motion.points[1].samples = 300;
    ds_native_apply(&motion);
    assert(deck->current.sequence == before && deck->path_order == 1);
    assert(coord_at(ds_value(&deck->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD), 100));

    motion.motion_order = 2;
    motion.a = AZN_SEEK;
    motion.occurrence_ordinal = 1;
    motion.points[0].samples = 100;
    motion.points[1].samples = 200;
    ds_native_apply(&motion);
    assert(deck->path_order == 4 && deck->last_ordinal == 1 && !ds.metrics[0].capture_gaps);
    const struct ds_record *barrier = &deck->history[(deck->next + DS_HISTORY - 1) % DS_HISTORY];
    const struct ds_cell *landing = ds_ready(&barrier->image, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    const struct ds_cell *latest = ds_ready(&deck->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    assert(barrier->action.kind == AZ_STATUS_SEEK && barrier->action.ordinal == 1 && !barrier->lost);
    assert(landing && coord_at((const void *) landing->data, 200));
    assert(latest && coord_at((const void *) latest->data, 400));
    assert(latest->info.info.revision > landing->info.info.revision);

    motion.a = AZN_MEASURE;
    motion.occurrence_ordinal = 0;
    motion.motion_generation = 3;
    motion.motion_order = 6;
    motion.points[1].samples = 600;
    ds_native_apply(&motion);
    assert(deck->path_order == 4);
    struct azstatus_native_receipt gap = {.size = sizeof gap, .version = 1, .kind = AZN_GAP,
        .b = AZN_MOTION, .lost_topics = DS_ACTION_ROUTES | DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_PHASE)};
    ds_native_apply(&gap);
    assert(!ds_pending_head[0].count && !ds_ready(&deck->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD));
    motion.motion_order = 7;
    motion.points[1].samples = 700;
    ds_native_apply(&motion);
    assert(deck->path_order == 7 && !ds_pending_head[0].count);
    assert(coord_at(ds_value(&deck->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD), 700));

    motion.motion_generation = 4;
    for (unsigned i = 0; i < DS_PATH_PENDING; ++i) {
        motion.motion_order = 9 + i;
        ds_native_apply(&motion);
    }
    assert(deck->path_order == 7);
    ++motion.motion_order;
    ds_native_apply(&motion);
    const struct ds_record *recovery = &deck->history[(deck->next + DS_HISTORY - 1) % DS_HISTORY];
    assert(!ds_pending_head[0].count && deck->path_order == motion.motion_order);
    assert((recovery->lost & DS_ACTION_ROUTES) == DS_ACTION_ROUTES && recovery->reason == AZ_STATUS_CAPTURE_GAP);
    puts("PASS future native head ordering, declared capture loss and bounded barrier wait");
}
static void test_hot_metadata(void) {
    const unsigned deck = 0;
    const uint64_t load = 70, epoch = 13;
    ds.enabled = 1;
    assert(ds_publish(deck, load, DS_BOUNDARY, 1, NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    ds_native_epoch[deck] = load;
    ds_native_fence[deck] = epoch;
    struct azn_hot_receipt packet = {
        .base = {.size = sizeof packet, .kind = AZN_HOT_CUES, .unit = 4, .epoch = epoch,
                 .load_generation = load, .attempt = 5, .valid = 1, .revision = 1},
        .details = {.comments = {[0] = "live comment"}, .stored_ms = {[0] = {0, 20}}},
    };
    struct azstatus_native_receipt *r = &packet.base;
    r->hot[0] = (struct azn_hot_slot) {{AZN_COORD, 48000, 0}, {AZN_COORD, 48000, 960},
                                      0x101112, AZN_HOT_COMMENT | AZN_HOT_AUTO_ACTIVE};
    ds_native_apply(r);
    const struct ds_cell *live = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    assert(live->info.info.provenance == AZ_PROV_NATIVE && live->info.count == 1);
    const az_cue_entry *entry = (const void *) live->data;
    assert((entry->valid & (AZ_CUE_COMMENT | AZ_CUE_AUTO_ACTIVE)) == (AZ_CUE_COMMENT | AZ_CUE_AUTO_ACTIVE));
    assert(!strcmp(entry->comment, "live comment") && entry->auto_active == 0);
    assert(!(entry->valid & AZ_CUE_SAVED) && entry->saved == -1 && entry->modified == -1);

    r->size = sizeof *r;
    ++r->revision;
    ds_native_apply(r);
    live = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    entry = (const void *) live->data;
    assert(!(entry->valid & (AZ_CUE_COMMENT | AZ_CUE_AUTO_ACTIVE)) && !entry->comment[0] && entry->auto_active == -1);
    r->size = sizeof packet;
    r->epoch = epoch - 1;
    ++r->revision;
    ds_native_apply(r);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT) == live);
    puts("PASS hot metadata uses its native bank edition and ignores truncated and retired receipts");
}

static void test_cue_storage_flags(void) {
    const unsigned deck = 0;
    const uint64_t load = 20;
    const az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 7, 2, 8, 10, 0};
    const struct ds_field binding = {AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK,
        {.info = {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, 1}, .scope = AZ_STATUS_LOAD_SCOPE,
         .count = 1, .scope_id = load, .provider_id = 4}, &ref, NULL};
    ds.enabled = 1;
    assert(ds_publish(deck, load, DS_BOUNDARY, 1, &binding, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_native_epoch[deck] = load;
    ds_native_fence[deck] = 5;
    struct azn_hot_receipt hot = {.base = {.size = sizeof hot, .kind = AZN_HOT_CUES, .unit = 4,
        .epoch = 5, .load_generation = load, .attempt = 9, .valid = 1, .revision = 1},
        .details = {.comments = {[0] = "Before"}, .stored_ms = {[0] = {1000, 0}, [7] = {2000, 0}}}};
    hot.base.hot[0] = (struct azn_hot_slot) {{AZN_COORD, 44100, 44100}, {0}, 0x112233, AZN_HOT_COMMENT};
    hot.base.hot[7] = (struct azn_hot_slot) {{AZN_COORD, 48000, 96000}, {0}, 0x332211, 0};
    ds_native_apply(&hot.base);
    const struct ds_cell *bank = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    uint64_t revision = bank->info.info.revision;
    const az_cue_entry *live = (const void *) bank->data;
    az_cue_entry saved = live[0];
    saved.saved = 1;
    saved.valid |= AZ_CUE_SAVED;
    const int64_t saved_ms[2] = {1000, 0};
    const struct ds_time_axis file_clock = {1000, saved_ms, 1, 1000, NULL};
    struct ds_field file = {AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_HOT,
        {.info = {AZ_AVAIL_READY, AZ_PROV_FILE, 5, 2}, .scope = AZ_STATUS_LOAD_SCOPE,
         .count = 1, .scope_id = load, .provider_id = 8}, &saved, &file_clock};
    assert(ds_publish_file(deck, load, &ref, 2, &file, 1, 0, 0) == AZ_OK);
    bank = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    live = (const void *) bank->data;
    assert(bank->info.info.provenance == AZ_PROV_NATIVE && bank->info.info.revision > revision);
    assert((live[0].valid & AZ_CUE_SAVED) && live[0].saved == 1 && live[0].modified == 0);
    assert((live[1].valid & AZ_CUE_SAVED) && live[1].saved == 0 && live[1].modified == 1);
    ds_native_apply(&hot.base);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT) == bank);

    strcpy(hot.details.comments[0], "Live edit");
    ++hot.base.revision;
    ds_native_apply(&hot.base);
    bank = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    live = (const void *) bank->data;
    assert(live[0].saved == 1 && live[0].modified == 1);
    const az_cue_entry *file_rows = (const void *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_SAVED_HOT)->data;
    assert(!strcmp(file_rows[0].comment, "Before"));
    file.info.info.availability = AZ_AVAIL_STALE;
    file.info.count = 0;
    assert(ds_publish_file(deck, load, &ref, 3, &file, 1, 0, 0) == AZ_OK);
    bank = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    live = (const void *) bank->data;
    assert(!(live[0].valid & AZ_CUE_SAVED) && live[0].saved == -1 && live[0].modified == -1);
    assert(!strcmp(live[0].comment, "Live edit"));

    az_cue_entry memory = saved;
    memory.classification = AZ_CUE_MEMORY;
    memory.entry_id = 1;
    memory.slot = 0;
    memory.color_id = 3;
    memory.valid = AZ_CUE_START | AZ_CUE_COLOR_ID | AZ_CUE_COMMENT;
    memory.saved = memory.modified = -1;
    const int64_t sample_ticks[2] = {48000, 0};
    const struct ds_time_axis native_clock = {48000, sample_ticks, 1, 48000, saved_ms};
    const struct ds_field current = {AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY,
        {.info = {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, 1}, .scope = AZ_STATUS_LOAD_SCOPE,
         .count = 1, .scope_id = load, .provider_id = 11}, &memory, &native_clock};
    assert(ds_publish(deck, load, DS_NATIVE_INPUT, 4, &current, 1, NULL, NULL, 0, 0) == AZ_OK);
    az_cue_entry stored[2] = {memory, memory};
    stored[0].entry_id = 7;
    stored[1].entry_id = 8;
    file.field = AZ_STATUS_CUES_SAVED_MEMORY;
    file.info.info.availability = AZ_AVAIL_READY;
    file.info.count = 1;
    file.value = stored;
    assert(ds_publish_file(deck, load, &ref, 5, &file, 1, 0, 0) == AZ_OK);
    live = (const void *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->data;
    assert(live->entry_id == 1 && live->saved == 1 && live->modified == 0);
    const int64_t duplicate_ms[] = {1000, 0, 1000, 0};
    const struct ds_time_axis duplicate_clock = {1000, duplicate_ms, 1, 1000, NULL};
    file.clock = &duplicate_clock;
    file.info.count = 2;
    ++file.info.info.revision;
    assert(ds_publish_file(deck, load, &ref, 6, &file, 1, 0, 0) == AZ_OK);
    live = (const void *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->data;
    assert(!(live->valid & AZ_CUE_SAVED) && live->saved == -1 && live->modified == -1);
    puts("PASS exact-load FILE proof annotates native cues, detects edits and absence, and withdraws on invalidation or ambiguity");
}

struct load_join_trace {
    uint64_t generation;
    uint32_t track;
    unsigned initial, changed, resync, qualified;
};
static void on_native_load_join(const az_deck_status_event_v1 *event, void *user) {
    struct load_join_trace *trace = user;
    const az_status_load_v1 *load = event->view->load;
    assert(load && event->load_epoch == trace->generation);
    if (event->kind == AZ_STATUS_INITIAL) ++trace->initial;
    else if (event->kind == AZ_STATUS_CHANGE) ++trace->changed;
    else {
        assert(event->kind == AZ_STATUS_RESYNC && event->lost_topics);
        ++trace->resync;
    }
    assert(load->state_info.info.availability == AZ_AVAIL_READY &&
           load->state_info.info.provenance == AZ_PROV_NATIVE);
    if (event->load_epoch) assert(load->state == AZ_STATUS_LOAD_LOADED);
    if (load->track_info.info.availability == AZ_AVAIL_READY) {
        assert(event->load_epoch && load->track_info.scope == AZ_STATUS_LOAD_SCOPE &&
               load->track_info.scope_id == event->load_epoch && load->track.track_id == trace->track);
        ++trace->qualified;
    }
}
static void test_native_load_join(void) {
    ds.enabled = 1;
    struct azstatus_native_receipt native = {.size = sizeof native, .version = 1,
        .kind = AZN_LOAD, .deck = 0, .unit = 4, .epoch = 51, .load_generation = 701,
        .valid = 1, .attempt = 20, .a = AZN_LOADED, .observed_ns = 1};
    ds_native_apply(&native);
    az_track_ref ref = {.size = sizeof ref, .version = AZ_DATA_VERSION, .source_id = 1,
        .source_generation = 11, .library_id = 99, .track_id = 36};
    struct ds_field track = {AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK,
        {.info = {AZ_AVAIL_READY, AZ_PROV_DERIVED, 1, 1}, .scope = AZ_STATUS_LOAD_SCOPE,
         .count = 1, .scope_id = 701, .provider_id = 55}, &ref, NULL};
    assert(ds_publish(0, 701, 0, 1, &track, 1, NULL, NULL, 0, 0) == AZ_OK);
    struct load_join_trace trace = {.generation = 701, .track = 36};
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1, 0, AZ_STATUS_TOPIC(AZ_STATUS_LOAD)};
    az_deck_subscription handle;
    assert(ds_register(1, &watch, on_native_load_join, &trace, &handle) == AZ_OK);
    ds_drain(8);
    assert(trace.initial == 1 && trace.qualified == 1);

    ++native.epoch;
    native.a = AZN_LOADING;
    native.load_generation = 0;
    ++native.observed_ns;
    ds_native_apply(&native);
    trace.generation = 0;
    ds_drain(8);
    assert(!ds_ready(&ds.deck[0].current, AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK));
    uint64_t before = ds.deck[0].current.sequence;
    assert(ds_publish(0, 701, 0, 1, &track, 1, NULL, NULL, 0, 0) == AZ_STALE);
    assert(ds.deck[0].current.sequence == before);

    native.a = AZN_LOADED;
    native.load_generation = 702;
    ++native.observed_ns;
    ds_native_apply(&native);
    trace.generation = 702;
    trace.track = 88;
    ds_drain(8);
    assert(!ds_ready(&ds.deck[0].current, AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK));
    assert(ds_publish(0, 701, 0, 1, &track, 1, NULL, NULL, 0, 0) == AZ_STALE);
    ref.track_id = 88;
    track.info.scope_id = 702;
    ++track.info.info.revision;
    assert(ds_publish(0, 702, 0, 1, &track, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_drain(8);
    assert(trace.changed && trace.qualified == 2);

    /* Evict the retained callback history while leaving the current load and
     * qualified track unchanged. RESYNC must keep that same native identity. */
    for (unsigned i = 0; i <= DS_HISTORY; ++i) {
        ++native.attempt;
        ++native.observed_ns;
        ds_native_apply(&native);
    }
    ds_drain(8);
    assert(trace.resync == 1 && trace.qualified >= 3);
    assert(ds_release(1, handle) == AZ_OK);
    puts("PASS native load generation joins qualified tracks across INITIAL, CHANGE and RESYNC; delayed old track publication is rejected");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "native-load-join")) {
        test_native_load_join();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "hot-metadata")) {
        test_hot_metadata();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "cue-storage-flags")) {
        test_cue_storage_flags();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "early-native-head")) {
        test_early_native_head();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "slip-barrier")) {
        test_slip_barrier();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "source-authority")) {
        test_source_authority();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "delayed-loop-seed")) {
        test_delayed_loop_seed();
        return 0;
    }
    struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .capabilities = caps, .memory = memory_api};
    atomic_store(&ds_native_api, &api);
    ds_native_ready(0, &api);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN)->info.info.availability == AZ_AVAIL_LOADING);
    assert(!cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY));
    struct azstatus_native_receipt setting = {.kind = AZN_QUANTIZE, .unit = 21, .a = 0, .valid = 1};
    ds_native_apply(&setting);
    assert(cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_QUANTIZE)->info.info.availability == AZ_AVAIL_READY);
    setting.kind = AZN_KEY_LOCK;
    setting.unit = 22;
    setting.a = 1;
    ds_native_apply(&setting);
    assert(*(int *) cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_MASTER_TEMPO)->data == 1);
    assert(*(int *) cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_KEY_LOCK)->data == 1);
    setting.kind = AZN_GAP;
    setting.b = AZN_QUANTIZE;
    setting.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS);
    ds_native_apply(&setting);
    assert(cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_QUANTIZE)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_KEY_LOCK)->info.info.availability == AZ_AVAIL_READY);
    struct azstatus_native_receipt sync = {.kind = AZN_SYNC, .unit = 41, .valid = 1, .a = 1, .b = 2};
    ds_native_apply(&sync);
    assert(*(int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_RAW_MODE)->data == 2);
    assert(*(int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_ENABLED)->data == 1);
    sync.a = 0;
    sync.b = -1;
    ds_native_apply(&sync);
    assert(*(int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_ENABLED)->data == 0);
    assert(cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_RAW_MODE)->info.info.availability == AZ_AVAIL_STALE);
    struct azstatus_native_receipt role = {.kind = AZN_SYNC_ROLE, .unit = 41, .valid = 1, .a = 1, .b = 0};
    ds_native_apply(&role);
    assert(*(const int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_FOLLOWING)->data == 1);
    assert(*(const int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_LOCKED)->data == 0);
    role.b = 1;
    ds_native_apply(&role);
    assert(*(const int *) cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_LOCKED)->data == 1);
    const struct azstatus_native_receipt rgap = {.kind = AZN_GAP, .b = AZN_SYNC_ROLE, .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_SYNC)};
    ds_native_apply(&rgap);
    assert(cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_ENABLED)->info.info.availability == AZ_AVAIL_READY);
    assert(cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_FOLLOWING)->info.info.availability == AZ_AVAIL_STALE);
    role.valid = -1;
    ds_native_apply(&role);
    role.valid = 1;
    ds_native_apply(&role);
    assert(cell(AZ_STATUS_SYNC, AZ_STATUS_SYNC_LOCKED)->info.info.availability == AZ_AVAIL_UNSUPPORTED);
    setting.kind = AZN_QUANTIZE;
    setting.unit = 21;
    setting.valid = -1;
    ds_native_apply(&setting);
    setting.valid = 1;
    ds_native_apply(&setting);
    assert(cell(AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_QUANTIZE)->info.info.availability == AZ_AVAIL_UNSUPPORTED);
    struct azstatus_native_receipt r = {.kind = AZN_LOAD, .a = AZN_LOADING, .unit = 4, .epoch = 42, .attempt = 0, .valid = 1};
    ds_native_apply(&r);
    assert(!ds.deck[0].current.epoch);
    assert(cell(AZ_STATUS_LOAD, AZ_STATUS_LOAD_ATTEMPT)->info.scope_id == 4);
    r.a = AZN_LOADED;
    r.load_generation = 9001;
    r.size = sizeof r;
    r.source_kind = AZ_STATUS_SOURCE_USB2;
    ds_native_apply(&r);
    assert(ds.deck[0].current.epoch == 9001);
    assert(*(int *) cell(AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE)->data == AZ_STATUS_SOURCE_USB2);
    assert(cell(AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE)->info.scope_id == 9001);
    r.kind = AZN_LOOP;
    r.a = 1;
    r.c = AZN_MODE_TIME;
    r.points[0] = (struct azn_point) {AZN_COORD, 48000, 0};
    r.points[1] = (struct azn_point) {AZN_COORD, 48000, 48000};
    r.points[2] = r.points[0];
    r.points[3] = r.points[1];
    ds_native_apply(&r);
    struct ds_cell *kept = cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN);
    assert(kept->info.info.availability == AZ_AVAIL_READY && kept->info.scope_id == 9001);
    assert(((const az_cue_entry *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN)->data)->type == AZ_CUE_LOOP);
    assert(*(int *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE)->data == AZ_STATUS_LOOP_MODE_TIME);
    uint64_t rev = kept->info.info.revision, sequence = ds.deck[0].current.sequence;
    ds_native_apply(&r);
    assert(ds.deck[0].current.sequence == sequence);
    r.c = AZN_MODE_BEAT;
    ds_native_apply(&r);
    assert(*(int *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE)->data == AZ_STATUS_LOOP_MODE_BEAT);
    r.a = 0;
    r.c = AZN_MODE_EMPTY;
    r.points[2].state = r.points[3].state = AZN_ABSENT;
    ds_native_apply(&r);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN)->info.info.revision == rev);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_WORKING_IN)->info.info.availability == AZ_AVAIL_EMPTY);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE)->info.info.availability == AZ_AVAIL_EMPTY);
    struct azstatus_native_receipt ready = r;
    ready.kind = AZN_RELOOP;
    ready.reloop = AZN_RELOOP_ALLOWED;
    ds_native_apply(&ready);
    assert(*(int *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE)->data == 1);
    ready.reloop = AZN_RELOOP_DENIED;
    ds_native_apply(&ready);
    assert(!*(int *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE)->data);
    ready.kind = AZN_GAP;
    ready.b = AZN_RELOOP;
    ready.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_LOOP);
    ds_native_apply(&ready);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN)->info.info.availability == AZ_AVAIL_READY);
    struct azstatus_native_receipt head = {.kind = AZN_MOTION, .unit = 4, .epoch = 42, .load_generation = 9001, .a = AZN_MOTION_SEED, .motion_generation = 1, .motion_order = 1, .points = {{AZN_INVALID, 0, 0}, {AZN_COORD, 44100, 441}}};
    head.size = sizeof head;
    head.observed_window_samples = head.from_window_samples = 1;
    head.observed_window_rate = head.from_window_rate = 44100;
    ds_native_apply(&head);
    assert(compare_timestamps(((const az_status_coord_v1 *) cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD)->data)->seconds, 0.01, 1, head.points[1].rate, 1) == AZ_TIME_EQUAL);
    head.a = AZN_REPOSITION;
    head.motion_generation = 2;
    head.motion_order = 2;
    head.points[1].samples = 0;
    ds_native_apply(&head);
    assert(coord_at((const void *) cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD)->data, 0));
    head.motion_order = 1;
    head.points[1].samples = 1234;
    ds_native_apply(&head);
    assert(coord_at((const void *) cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD)->data, 0));
    head.a = AZN_WRAP;
    head.motion_order = 3;
    head.motion_generation = 3;
    head.occurrence_ordinal = 1;
    head.points[0] = (struct azn_point) {AZN_COORD, 44100, 441};
    head.points[1].samples = 0;
    ds_native_apply(&head);
    assert(ds.deck[0].last_ordinal == 1 && ds.deck[0].motion_generation == 3);
    head.a = AZN_JUMP;
    head.motion_order = 5;
    head.motion_generation = 4;
    head.occurrence_ordinal = 2;
    ds_native_apply(&head);
    assert(ds.deck[0].last_ordinal == 2 && ds.deck[0].path_order == 5);
    struct azstatus_native_receipt rate = {.kind = AZN_RATE, .unit = 4, .attempt = 4, .epoch = 42, .load_generation = 9001, .a = 1, .valid = AZ_AVAIL_READY, .b = AZ_AVAIL_READY};
    double speed = 1.0;
    memcpy(&rate.points[0].samples, &speed, sizeof speed);
    ds_native_apply(&rate);
    struct azstatus_native_receipt slip = {.size = sizeof slip, .kind = AZN_SLIP_HEAD, .deck = 0, .epoch = 42,
        .load_generation = 9001, .unit = 4, .attempt = 18, .valid = AZ_AVAIL_EMPTY, .b = 1, .motion_generation = 4};
    ds_native_apply(&slip);
    struct azstatus_native_receipt cut = {.kind = AZN_GAP, .deck = 0, .b = AZN_MOTION, .lost_topics = DS_ACTION_ROUTES | DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_PHASE)};
    ds_native_apply(&cut);
    assert(cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD)->info.info.availability == AZ_AVAIL_EMPTY);
    assert(cell(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE)->info.info.availability == AZ_AVAIL_READY);
    slip.valid = AZ_AVAIL_READY;
    slip.points[0] = (struct azn_point) {AZN_COORD, 44100, 48000};
    ds_native_apply(&slip);
    ds_native_apply(&cut);
    assert(coord_at((const void *) cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD)->data, 48000));
    head.size = sizeof head;
    head.a = AZN_REPOSITION;
    head.history_only = 1;
    head.occurrence_ordinal = 0;
    head.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    head.motion_order = 6;
    head.motion_generation = 5;
    head.points[1].samples = 32768;
    ds_native_apply(&head);
    assert(ds.deck[0].path_order == 6 && ds.deck[0].last_ordinal == 2);
    assert(cell(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE)->info.info.availability == AZ_AVAIL_READY);
    assert(coord_at((const void *) cell(AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD)->data, 32768));
    struct azstatus_native_receipt hot = {.kind = AZN_HOT_CUES, .unit = 4, .epoch = 42, .load_generation = 9001, .attempt = 37, .valid = 1, .revision = 12};
    hot.size = sizeof hot;
    hot.hot[7].in = (struct azn_point) {AZN_COORD, 48000, 0};
    hot.hot[7].rgb = 0x2378ab;
    hot.hot[0].in = (struct azn_point) {AZN_COORD, 44100, 441};
    hot.hot[0].out = (struct azn_point) {AZN_COORD, 44100, 882};
    ds_native_apply(&hot);
    struct ds_cell *bank = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    assert(bank->info.count == 2 && bank->info.provider_id == 37);
    assert(bank->clock.ticks && bank->clock.units_per_second == 7056000 && !bank->clock.window_rate &&
           !bank->clock.window_samples);
    int hot_clock = ds_compare_source_times(bank->clock.ticks[0], bank->clock.units_per_second, 441, 44100, 44100, 1);
    assert(hot_clock != AZ_TIME_INVALID && hot_clock == AZ_TIME_EQUAL);
    uint64_t bank_cut = ds.deck[0].current.sequence;
    ds_native_apply(&hot);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT) == bank && ds.deck[0].current.sequence == bank_cut);
    const az_cue_entry *rows = (const void *) bank->data;
    assert(rows[0].slot == 1 && rows[0].type == AZ_CUE_LOOP && rows[1].slot == 8 &&
           compare_timestamps(rows[1].start_s, 0, 1, hot.hot[7].in.rate, 1) == AZ_TIME_EQUAL && rows[1].rgb == 0x2378ab);
    struct azstatus_native_receipt linked = r;
    linked.a = linked.valid = 1;
    linked.points[2] = hot.hot[0].in;
    linked.points[3] = hot.hot[0].out;
    linked.hot_link = (struct azn_hot_link) {37, 12, 1, AZN_HOT_LINK_READY};
    ds_native_apply(&linked);
    struct ds_cell *ref_cell = cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    assert(ref_cell && ref_cell->info.info.availability == AZ_AVAIL_READY);
    const az_status_cue_ref_v1 *linked_row = (const void *) ref_cell->data;
    assert(linked_row->entry.slot == 1 && linked_row->provider_id == 37 && linked_row->collection_revision == bank->info.info.revision);
    hot.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    hot.hot[7].rgb = 0xaa00bb;
    ++hot.revision;
    ds_native_apply(&hot);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN)->info.info.availability == AZ_AVAIL_READY);
    const struct azstatus_native_receipt hg = {.kind = AZN_GAP, .b = AZN_HOT_CUES, .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES)};
    ds_native_apply(&hg);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN)->info.info.availability == AZ_AVAIL_READY);
    hot.lost_topics = 0;
    ds_native_apply(&hot);
    ds_native_apply(&linked);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->info.info.availability == AZ_AVAIL_STALE);
    assert(((const az_cue_entry *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT)->data)[1].rgb == 0xaa00bb);
    struct azstatus_native_receipt mem = {.kind = AZN_MEMORY, .unit = 4, .epoch = 42, .load_generation = 9001, .attempt = 38, .memory_pin = 64, .memory_serial = 3, .valid = AZ_AVAIL_READY};
    ds_native_apply(&mem);
    struct ds_cell *memory = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY);
    assert(memory_read_count == 1 && memory_release_count == 1 && memory->info.provider_id == 38 &&
           memory->info.count == 1 && !strcmp(((const az_cue_entry *) memory->data)->comment, "held"));
    assert(memory->clock.ticks && memory->clock.units_per_second == 44100 && memory->clock.window_rate == 44100);
    assert(compare_timestamps(memory->clock.ticks[1], 882, 44100, 44100, 1) == AZ_TIME_EQUAL);
    linked.hot_link = (struct azn_hot_link) {38, 3, 1, AZN_MEMORY_LINK_READY};
    ds_native_apply(&linked);
    ref_cell = cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    assert(ref_cell->info.info.availability == AZ_AVAIL_READY);
    linked_row = (const void *) ref_cell->data;
    assert(linked_row->cue_field == AZ_STATUS_CUES_LIVE_MEMORY && linked_row->provider_id == 38 &&
           linked_row->collection_revision == memory->info.info.revision && !strcmp(linked_row->entry.comment, "held"));
    mem.memory_serial = 2;
    ds_native_apply(&mem);
    assert(memory_read_count == 1 && memory_release_count == 2);
    mem.epoch = 41;
    ds_native_apply(&mem);
    assert(memory_read_count == 1 && memory_release_count == 3);
    const struct azstatus_native_receipt mg = {.kind = AZN_GAP, .b = AZN_MEMORY, .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES)};
    ds_native_apply(&mg);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT)->info.info.availability == AZ_AVAIL_READY);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN)->info.info.availability == AZ_AVAIL_READY);
    mem.epoch = 42;
    mem.memory_serial = 5;
    ds_native_apply(&mem);
    az_cue_entry saved = *(const az_cue_entry *) cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->data;
    saved.entry_id = 1; /* Deliberately collide with LIVE identity, bounds and numeric revision. */
    strcpy(saved.comment, "saved origin");
    struct ds_field stored = {.topic = AZ_STATUS_CUES, .field = AZ_STATUS_CUES_SAVED_MEMORY, .info = {.info = {AZ_AVAIL_READY, AZ_PROV_FILE, cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->info.info.revision, 1}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 9001, .provider_id = AZSP_FILE | 9, .count = 1}, .value = &saved};
    assert(ds_publish(0, 9001, 0, 1, &stored, 1, NULL, NULL, 0, 0) == AZ_OK);
    file_ref_target = (az_status_cue_ref_v1) {.load_epoch = 9001, .provider_id = stored.info.provider_id, .collection_revision = stored.info.info.revision, .provenance = AZ_PROV_FILE, .cue_field = AZ_STATUS_CUES_SAVED_MEMORY, .entry = saved};
    file_ref_valid = 1;
    linked.hot_link = (struct azn_hot_link) {38, 5, 1, AZN_MEMORY_LINK_READY};
    linked.file_row_token = 55;
    ds_native_apply(&linked);
    linked_row = (const void *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->data;
    assert(linked_row->provenance == AZ_PROV_FILE && linked_row->cue_field == AZ_STATUS_CUES_SAVED_MEMORY &&
           linked_row->entry.entry_id == 1 && !strcmp(linked_row->entry.comment, "saved origin"));
    ++mem.memory_serial;
    ds_native_apply(&mem);
    ds_native_apply(&linked);
    linked_row = (const void *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->data;
    assert(linked_row->provenance == AZ_PROV_FILE); /* unrelated LIVE edition */
    file_ref_valid = 0;
    atomic_store(&ds_file_refresh[0], 1);
    ds_file_loop_refresh(0);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->info.info.availability == AZ_AVAIL_STALE);
    file_ref_valid = 1;
    linked.hot_link.stamp = mem.memory_serial; /* genuine new copy from this edition */
    ds_native_apply(&linked);
    file_ref_valid = 0;
    atomic_store(&ds_file_refresh[0], 1);
    ds_file_loop_refresh(0);
    /* The latest native edition is independently READY, but cannot replace
     * the physical FILE target selected by this working-copy witness. */
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->info.info.availability == AZ_AVAIL_READY);
    assert(!strcmp(((const az_cue_entry *)cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY)->data)->comment, "held"));
    file_ref_valid = 1;
    atomic_store(&ds_file_refresh[0], 1);
    ds_file_loop_refresh(0);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->info.info.availability == AZ_AVAIL_READY);
    linked_row = (const void *) cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY)->data;
    assert(linked_row->provenance == AZ_PROV_FILE && linked_row->cue_field == AZ_STATUS_CUES_SAVED_MEMORY &&
           linked_row->entry.entry_id == 1 && !strcmp(linked_row->entry.comment, "saved origin"));
    puts("PASS FILE loop identity: equal LIVE ID/bounds/revision cannot replace lost FILE provenance; qualified FILE recovery restores exact row");
    memory = cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY);
    bank_cut = ds.deck[0].current.sequence;
    ds_native_apply(&mem);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY) == memory && ds.deck[0].current.sequence == bank_cut);
    struct azstatus_native_receipt malformed = linked;
    malformed.points[0].samples = -1;
    ds_native_apply(&malformed);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_LOOP, AZ_STATUS_LOOP_DEFINED)->info.info.availability == AZ_AVAIL_STALE);
    assert(cell(AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN)->info.info.availability == AZ_AVAIL_STALE);
    struct azstatus_native_receipt meta = {.kind = AZN_METADATA, .unit = 99, .epoch = 42, .valid = 1, .content_id = 81, .title = "Étude", .artist = "DJ"};
    ds_native_apply(&meta);
    assert(cell(AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE)->info.scope == AZ_STATUS_UNIT);
    assert(!strcmp((const char *) cell(AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE)->data, "Étude"));
    r.kind = AZN_LOAD;
    r.epoch = 43;
    r.a = AZN_LOADING;
    ds_native_apply(&r);
    assert(!ds.deck[0].current.epoch);
    assert(cell(AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE)->info.info.availability == AZ_AVAIL_STALE);
    ds_native_apply(&meta);
    assert(cell(AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE)->info.info.availability == AZ_AVAIL_STALE);
    r.a = AZN_FAILED;
    ds_native_apply(&r);
    r.a = AZN_LOADING;
    ds_native_apply(&r);
    assert(*(const int32_t *) cell(AZ_STATUS_LOAD, AZ_STATUS_LOAD_STATE)->data == AZ_STATUS_LOAD_FAILED);
    test_burst();
    test_legacy_map_clock();
    test_native_order();
    test_duration_reducer();
    test_original_bpm_reducer();
    test_native_cue_clocks();
    test_stationary_terminal_reducer();
    test_terminal_window_refinement();
    puts("PASS native reducer scope, fenced metadata, installed-field readiness, duplicate suppression and independent loop revisions");
}

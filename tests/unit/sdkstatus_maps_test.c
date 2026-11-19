/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/azstem/sdk_status_maps.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>


struct Ref {
    uintptr_t object;
    unsigned count;
};
static struct Ref refs[4096];
static size_t refs_count;
static struct azstatus_native_receipt observed[256];
static size_t observed_count;
azn_pack_observer observer;
uintptr_t playing;
uint64_t epoch = 7;
unsigned gaps;
bool analysis_refs, shared_second;
void (*during_current)();
uintptr_t replacement_holder, replacement_content;
typedef struct {
    _Alignas(16) unsigned char bytes[0x150];
} Buf150;
typedef struct {
    _Alignas(16) unsigned char bytes[0x80];
} Buf80;
#define put(address, value)                                       \
    do {                                                          \
        __auto_type put_value = (value);                          \
        memcpy((void *) (address), &put_value, sizeof put_value); \
    } while (0)
void drain(const struct azn_maps_api *api) {
    for (size_t i = 0; i < observed_count; ++i) api->release(&observed[i].map);
    observed_count = 0;
}

uint64_t azn_owned_pin(uintptr_t p) {
    if (!p) return 0;
    if (analysis_refs) analysis_ops.grant(p);
    assert(refs_count < sizeof refs / sizeof *refs);
    refs[refs_count++] = (struct Ref) {p, 1};
    return refs_count;
}
int azn_pin_hold(uint64_t t, uintptr_t *out) {
    if (!t || t > refs_count || !refs[t - 1].count) return 0;
    refs[t - 1].count++;
    *out = refs[t - 1].object;
    return 1;
}
void azn_pin_drop(uint64_t t) {
    if (t) {
        assert(t <= refs_count && refs[t - 1].count);
        refs[t - 1].count--;
        if (analysis_refs && !refs[t - 1].count) analysis_drop(refs[t - 1].object);
    }
}
int azn_pack_acquire(unsigned d, struct azn_pack_view *v) {
    if ((d && !(d == 1 && shared_second)) || !playing) return 0;
    *v = (struct azn_pack_view) {azn_owned_pin(playing), 5, 13, epoch + d, playing};
    return 1;
}
int azn_pack_current(unsigned d, const struct azn_pack_view *v) {
    if (during_current) {
        __auto_type interleave = during_current;
        during_current = NULL;
        interleave();
    }
    return (!d || (d == 1 && shared_second)) && v->load == epoch + d && v->object == playing;
}
int azn_pack_retains(uintptr_t pack) { return pack && playing == pack; }
int azn_pack_observe(azn_pack_observer o) {
    observer = o;
    return 1;
}
uint64_t azn_interest(unsigned deck __attribute__((unused))) { return AZ_STATUS_ALL_V1; }
void azn_map_gap(unsigned deck __attribute__((unused))) { ++gaps; }
int azn_offer(struct azstatus_native_receipt *r) {
    assert(observed_count < sizeof observed / sizeof *observed);
    observed[observed_count++] = *r;
    return 1;
}
uint64_t azn_map_begin_mask(unsigned mask) { return mask; }
void azn_map_end_mask(uint64_t ticket) { assert(ticket <= (shared_second ? 3u : 1u)); }
int azn_offer_map_cut(struct azstatus_native_receipt *r) { return azn_offer(r); }
static uintptr_t test_retime(uintptr_t dst, int64_t offset) {
    put(dst + 8, offset);
    return 1;
}
static uintptr_t test_assign(uintptr_t dst, uintptr_t src) {
    put(dst, read_ptr(src, 0));
    return 1;
}
static uintptr_t test_broadcast(uintptr_t holder __attribute__((unused)), unsigned reason __attribute__((unused))) { return 37; }
static void current_update(void) { assert(grid_assign(replacement_holder + 0x28, (uintptr_t) &replacement_content) == 1); }
static void current_retire(void) {
    retire_pair(138);
    playing = 0;
    observer(0, 5, 14, 0);
}
#include "sdkstatus_map_analysis_test.inc"
int main(int argc, char **argv) {
    assert(argc == 2 || (argc == 3 && (!strcmp(argv[2], "browse-retain") || !strcmp(argv[2], "analysis"))));
    const int build = argv[1][0] - '0';
    assert(azn_maps_configure(build));
    const struct azn_maps_api *api = azn_maps_service();
    assert(api && observer);
    if (argc == 3 && !strcmp(argv[2], "analysis")) return test_analysis(api);
    Buf150 pack = {0};
    Buf80 gh = {0}, ph = {0}, gc = {0}, pc = {0}, wrong = {0};
    const Profile *pr = &(profiles[build - 1]);
    put(((uintptr_t) pack.bytes), pr->pack);
    put(((uintptr_t) gh.bytes), pr->grid_holder);
    put(((uintptr_t) ph.bytes), pr->phrase_holder);
    put(((uintptr_t) gc.bytes), pr->grid_content);
    put(((uintptr_t) pc.bytes), pr->phrase_content);
    put(((uintptr_t) pack.bytes) + 0xd8, ((uintptr_t) gh.bytes));
    put(((uintptr_t) pack.bytes) + 0x120, ((uintptr_t) ph.bytes));
    put(((uintptr_t) gh.bytes) + 0x28, ((uintptr_t) gc.bytes));
    put(((uintptr_t) gh.bytes) + 0x30, (int64_t) (-441));
    put(((uintptr_t) gh.bytes) + 0x40, (uint32_t) (44100));
    put(((uintptr_t) ph.bytes) + 0x28, ((uintptr_t) pc.bytes));
    struct Cell {
        int64_t sample;
        double bpm;
    } beats[] = {{441, 122}, {22491, 122}, {44541, 122}, {66591, 122}};
    uint32_t sections[][5] = {{1, 1, 2, 0, 0}, {2, 2, 4, 0, 0}};
    int32_t downbeats[] = {0, 3};
    put(((uintptr_t) gc.bytes) + 0x38, ((uintptr_t) (downbeats)));
    put(((uintptr_t) gc.bytes) + 0x40, (int32_t) (2));
    put(((uintptr_t) gc.bytes) + 0x28, ((uintptr_t) (beats)));
    put(((uintptr_t) gc.bytes) + 0x30, (int32_t) (4));
    put(((uintptr_t) pc.bytes) + 0x40, ((uintptr_t) (sections)));
    put(((uintptr_t) pc.bytes) + 0x48, (int32_t) (2));
    put(((uintptr_t) pc.bytes) + 0x3c, (uint32_t) (0));
#define prove(p) api->prove_pair(138, (p), ((uintptr_t) gh.bytes), ((uintptr_t) gc.bytes), ((uintptr_t) ph.bytes), ((uintptr_t) pc.bytes))
    assert(!prove(((uintptr_t) wrong.bytes)));
    assert(prove(((uintptr_t) pack.bytes)) && !observed_count); // authenticated GUI pair before deck completion
    playing = ((uintptr_t) pack.bytes);
    observer(0, 5, 13, epoch);
    assert(observed_count == 1 && observed[0].load_generation == 7);
    __auto_type snap = observed[0].map;
    if (argc == 3) {
        // Browse clears the only GUI owner while the track stays loaded.
        api->retire_pair(138);
        assert(observed_count == 1 && azn_maps_current(0, epoch, &snap));
        // An actual holder edit is still observed while Browse owns the screen.
        map_writer_original[1] = (void *) test_retime;
        assert(grid_retime(((uintptr_t) gh.bytes) + 0x28, -440) == 1 && observed_count == 2);
        assert(observed[observed_count - 1].map.grid.revision == snap.grid.revision + 1);
        assert(!azn_maps_current(0, epoch, &snap) && azn_maps_current(0, epoch, &observed[observed_count - 1].map));
        // A real withdrawal terminates the lease even without a GUI callback.
        playing = 0;
        observer(0, 5, 14, 0);
        assert(observed_count == 2 && !azn_maps_current(0, epoch, &observed[observed_count - 1].map));
        drain(api);
        for (size_t i = 0; i < refs_count; ++i) assert(!refs[i].count);
        assert(!gaps);
        puts("PASS Browse detach retains the loaded map through real edits and releases it at load retirement");
        return 0;
    }
    struct az_beat b[4];
    struct az_phrase row[2];
    uint32_t n;
    assert(api->read(&snap.grid, AZNM_GRID, NULL, 0, &n) == AZ_OK && n == 4);
    assert(api->read(&snap.grid, AZNM_GRID, b, 3, &n) == AZ_LIMIT);
    assert(api->read(&snap.grid, AZNM_GRID, b, 4, &n) == AZ_OK && n == 4 &&
           compare_timestamps(b[0].time_s, 0, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(b[3].time_s, 1.5, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL && b[1].bpm == 122);
    int64_t ticks[4];
    uint32_t clock = 0;
    assert(api->read_samples(&snap.grid, ticks, 3, &n, &clock) == AZ_LIMIT && !clock);
    assert(api->read_samples(&snap.grid, ticks, 4, &n, &clock) == AZ_OK && n == 4 && clock == 44100);
    assert(compare_timestamps(ticks[0], 0, clock, clock, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(ticks[3], 66150, clock, clock, 1) == AZ_TIME_EQUAL);
    __auto_type negative_origin = snap.grid;
    --negative_origin.offset_samples;
    assert(api->read_samples(&negative_origin, ticks, 4, &n, &clock) == AZ_OK &&
           committed_grid_order(ticks[0], -1, clock) == AZ_TIME_EQUAL);
    assert(b[0].beat_in_bar == 1 && b[1].beat_in_bar == 2 && b[2].beat_in_bar == 3 && b[3].beat_in_bar == 1);
    assert(api->read(&snap.phrases, AZNM_PHRASES, row, 2, &n) == AZ_OK && n == 2 &&
           row[0].start_beat == 0 && row[0].end_beat == 1 && row[1].raw_kind == 2 && row[1].end_beat == 3);
    assert(row[0].raw_mood == 1 && row[1].raw_mood == 1);
    const uint32_t moods[][2] = {{1, 2}, {2, 3}, {3, 0}, {UINT32_MAX, 0}};
    for (unsigned i = 0; i < sizeof moods / sizeof *moods; ++i) {
        const uint32_t *mood = moods[i];
        put(((uintptr_t) pc.bytes) + 0x3c, mood[0]);
        assert(api->read(&snap.phrases, AZNM_PHRASES, row, 2, &n) == AZ_OK && n == 2);
        assert(row[0].raw_mood == mood[1] && row[1].raw_mood == mood[1]);
    }
    put(((uintptr_t) pc.bytes) + 0x3c, (uint32_t) (0));
    assert(prove(((uintptr_t) pack.bytes)) && observed_count == 1);
    assert(api->prove_pair(239, ((uintptr_t) pack.bytes), ((uintptr_t) gh.bytes), ((uintptr_t) gc.bytes), ((uintptr_t) ph.bytes), ((uintptr_t) pc.bytes)));
    assert(observed_count == 1 && azn_maps_current(0, epoch, &snap));
    put(((uintptr_t) gh.bytes) + 0x30, (int64_t) (0));
    assert(prove(((uintptr_t) pack.bytes)) && observed_count == 2);
    assert(observed[1].map.grid.provider == snap.grid.provider && observed[1].map.grid.revision == snap.grid.revision + 1);
    assert(observed[1].map.phrases.revision == snap.phrases.revision);
    assert(api->read(&snap.grid, AZNM_GRID, b, 4, &n) == AZ_OK &&
           compare_timestamps(b[0].time_s, 0, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL); // immutable callback cut
    map_writer_original[0] = (void *) test_assign;
    map_writer_original[1] = (void *) test_retime;
    assert(azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    Buf80 new_grid = gc;
    uintptr_t typed = ((uintptr_t) new_grid.bytes);
    assert(grid_assign(((uintptr_t) gh.bytes) + 0x28, ((uintptr_t) (&typed))) == 1 && observed_count == 3);
    typed = ((uintptr_t) gc.bytes);
    grid_assign(((uintptr_t) gh.bytes) + 0x28, ((uintptr_t) (&typed)));
    grid_retime(((uintptr_t) gh.bytes) + 0x28, -441);
    assert(observed_count == 5);
    assert(observed[4].map.grid.revision == snap.grid.revision + 4 && observed[4].map.phrases.revision == snap.phrases.revision);
    assert(!azn_maps_current(0, epoch, &snap) && azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    map_writer_original[3] = (void *) test_broadcast;
    Buf80 new_phrase = pc;
    typed = ((uintptr_t) new_phrase.bytes);
    phrase_argument = ((uintptr_t) (&typed));
    put(((uintptr_t) ph.bytes) + 0x28, typed);
    assert(!azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    assert(phrase_broadcast(((uintptr_t) ph.bytes), 0) == 37 && observed_count == 6);
    typed = ((uintptr_t) pc.bytes);
    put(((uintptr_t) ph.bytes) + 0x28, typed);
    phrase_broadcast(((uintptr_t) ph.bytes), 0);
    phrase_argument = 0;
    assert(observed_count == 7 && observed[observed_count - 1].map.phrases.revision == snap.phrases.revision + 2);
    api->retire_pair(777);
    assert(observed_count == 7);
    __auto_type before_last_owner = observed[observed_count - 1].map;
    api->retire_pair(138);
    assert(observed_count == 7 && azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    api->retire_pair(239);
    assert(observed_count == 7 && azn_maps_current(0, epoch, &before_last_owner));
    drain(api);
    assert(!gaps);
    assert(prove(((uintptr_t) pack.bytes)) && !observed_count);
    api->seed(0);
    assert(observed_count == 1);
    __auto_type before_reader = observed[observed_count - 1].map;
    replacement_holder = ((uintptr_t) gh.bytes);
    replacement_content = ((uintptr_t) new_grid.bytes);
    during_current = current_update;
    /* A genuine native update crosses a validation reader's final pack check.
     * The reader must reject the old edition without making the writer lose
     * its captured update merely because validation was in progress. */
    assert(!azn_maps_current(0, epoch, &before_reader));
    assert(!during_current && !gaps && observed_count == 2);
    assert(observed[observed_count - 1].map.grid.provider == before_reader.grid.provider &&
           observed[observed_count - 1].map.grid.revision == before_reader.grid.revision + 1);
    assert(azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    __auto_type before_retire = observed[observed_count - 1].map;
    during_current = current_retire;
    assert(!azn_maps_current(0, epoch, &before_retire) && !during_current && !gaps);
    assert(observed_count == 2);
    assert(api->read(&before_reader.grid, AZNM_GRID, b, 4, &n) == AZ_OK && n == 4);
    /* Reusing exactly the same native addresses after retirement cannot make
     * either old stamp valid, even with unchanged row bytes. */
    assert(api->prove_pair(138, ((uintptr_t) pack.bytes), ((uintptr_t) gh.bytes), ((uintptr_t) new_grid.bytes), ((uintptr_t) ph.bytes), ((uintptr_t) pc.bytes)));
    playing = ((uintptr_t) pack.bytes);
    ++epoch;
    observer(0, 5, 14, epoch);
    assert(observed_count == 3 && observed[observed_count - 1].load_generation == epoch);
    assert(!azn_maps_current(0, epoch, &before_reader) && !azn_maps_current(0, epoch, &before_retire));
    assert(azn_maps_current(0, epoch, &observed[observed_count - 1].map));
    playing = 0;
    api->retire_pair(138);
    drain(api);
    for (size_t i = 0; i < refs_count; ++i) assert(!refs[i].count);
    puts("PASS typed map owners, load binding, clock/offset, immutable sections, revisions, reader/writer collision, retirement and address reuse");
    return 0;
}

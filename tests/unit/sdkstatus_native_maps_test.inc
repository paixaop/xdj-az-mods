/* SPDX-License-Identifier: GPL-3.0-only */
static unsigned native_map_reads, native_map_releases, native_map_pairs, native_map_holds, native_map_seeds;
static int native_map_clock_result = AZ_OK, native_map_origins_present = 1;
static uint32_t native_map_clock_rate = 100;
static int native_map_empty_rows;
static int native_map_rows(const struct aznm_component *part, unsigned kind, void *rows, uint32_t capacity, uint32_t *count) {
    if (native_map_empty_rows && rows && kind == AZNM_GRID) {
        *count = 0;
        return AZ_OK;
    }
    return legacy_map_rows(part, kind, rows, capacity, count);
}
static int native_map_exact_samples(const struct aznm_component *part, int64_t *ticks, uint32_t capacity, uint32_t *count, uint32_t *rate) {
    assert(part->content == 1 && capacity == 3);
    ++native_map_reads;
    *count = 3;
    *rate = native_map_clock_rate;
    ticks[0] = 0;
    ticks[1] = 100;
    ticks[2] = 200;
    return native_map_clock_result;
}
static void native_map_release(const struct aznm_packet *packet) {
    assert(packet->grid.content == 1 && packet->phrases.content == 2);
    ++native_map_releases;
}
static void native_map_seed(unsigned deck) {
    assert(deck == 0);
    ++native_map_seeds;
}
static const struct azn_maps_api *native_map_exact_api(void) {
    static const struct azn_maps_api api = {.size = sizeof api, .version = 1, .read = native_map_rows, .release = native_map_release, .seed = native_map_seed, .read_samples = native_map_exact_samples};
    return &api;
}
static int native_map_origins(const struct aznm_packet *packet, uint64_t tokens[3]) {
    assert(packet->owner == 69);
    tokens[0] = 101;
    tokens[1] = 0;
    tokens[2] = 103;
    return native_map_origins_present;
}
void azfile_map_pair(unsigned deck, uint64_t epoch, const uint64_t tokens[3]) {
    assert(deck == 0 && epoch == 13 && tokens[0] == 101 && !tokens[1] && tokens[2] == 103);
    ++native_map_pairs;
}
static void native_map_hold(const uint64_t *tokens, size_t count, int acquire) {
    assert(count == 3 && !acquire && tokens[0] == 101 && !tokens[1] && tokens[2] == 103);
    assert(native_map_pairs == native_map_holds + 1); /* publish while custody is held */
    ++native_map_holds;
}
static void test_native_map_clocks(void) {
    ds.enabled = 1;
    const struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .maps = native_map_exact_api};
    const struct azn_file_cue_api file = {.size = sizeof file, .version = 1, .map_origins = native_map_origins, .map_hold = native_map_hold};
    atomic_store(&ds_native_api, &api);
    atomic_store(&ds_file_cues, &file);
    struct azstatus_native_receipt load = {.size = sizeof load, .version = 1, .kind = AZN_LOAD, .unit = 11, .epoch = 12, .load_generation = 13, .a = AZN_LOADED, .valid = 1};
    ds_native_apply(&load);
    struct azstatus_native_receipt r = load;
    r.kind = AZN_MAPS;
    r.map = (struct aznm_packet) {.owner = 69, .changed = AZNM_GRID | AZNM_PHRASES | AZNM_PAIR, .grid = {.content = 1, .provider = 70, .revision = 3, .sample_rate = 100, .availability = AZ_AVAIL_READY}, .phrases = {.content = 2, .provider = 71, .revision = 4, .availability = AZ_AVAIL_READY}};
    ds_native_apply(&r);
    const struct ds_cell *grid = ds_ready(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    assert(grid && grid->clock.ticks && grid->clock.units_per_second == 100);
    assert(compare_timestamps(grid->clock.ticks[1], 100, 100, 100, 1) == AZ_TIME_EQUAL);
    assert(ds_ready(&ds.deck[0].current, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    assert(native_map_reads == 1 && native_map_pairs == 1 && native_map_holds == 1 && native_map_releases == 1);
    native_map_clock_rate = 101;
    ++r.map.grid.revision;
    ds_native_apply(&r);
    assert(!ds_ready(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT));
    assert(!ds_ready(&ds.deck[0].current, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID));
    assert(native_map_pairs == 1 && native_map_holds == 1 && native_map_releases == 2);
    native_map_clock_rate = 100;
    native_map_clock_result = AZ_LIMIT;
    ++r.map.grid.revision;
    ds_native_apply(&r);
    assert(!ds_ready(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT));
    assert(native_map_pairs == 1 && native_map_releases == 3);
    native_map_clock_result = AZ_OK;
    native_map_origins_present = 0;
    ++r.map.grid.revision;
    ds_native_apply(&r);
    assert(ds_ready(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT));
    assert(native_map_pairs == 1 && native_map_holds == 1 && native_map_releases == 4);
    struct azstatus_native_receipt gap = {.kind = AZN_GAP, .b = AZN_MAPS, .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES)};
    ds_native_apply(&gap);
    assert(native_map_seeds == 1);
    ++r.epoch; /* a taken obsolete packet must still release its retained rows */
    ds_native_apply(&r);
    assert(native_map_releases == 5 && native_map_reads == 4);
    --r.epoch;
    native_map_empty_rows = 1;
    ++r.map.grid.revision;
    unsigned reads = native_map_reads;
    ds_native_apply(&r);
    grid = cell(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    assert(grid && grid->info.info.availability == AZ_AVAIL_EMPTY && !grid->info.count);
    assert(native_map_reads == reads && !grid->clock.ticks);
    native_map_empty_rows = 0;
    atomic_store(&ds_native_api, NULL);
    atomic_store(&ds_file_cues, NULL);
    puts("PASS exact native map clocks, empty second reads, rate/limit rejection, paired FILE custody and obsolete packet release");
}
static unsigned native_load_seeds, native_metadata_seeds, native_stems_seeds;
static int native_load_seed_available = 1;
static int native_seed_load(unsigned deck, struct azstatus_native_receipt *out) {
    assert(deck == 0);
    ++native_load_seeds;
    if (!native_load_seed_available) return 0;
    *out = (struct azstatus_native_receipt) {.size = sizeof *out, .version = 1, .kind = AZN_LOAD, .unit = 11, .epoch = 12, .load_generation = 13, .a = AZN_LOADED, .valid = 1};
    return 1;
}
static int native_seed_stems(unsigned deck, struct azstatus_native_receipt *out) {
    assert(deck == 0);
    ++native_stems_seeds;
    *out = (struct azstatus_native_receipt) {.size = sizeof *out, .version = 1, .kind = AZN_STEMS, .unit = 11, .epoch = 12, .load_generation = 13, .attempt = ds.deck[0].stem_binding, .valid = AZ_AVAIL_READY, .c = AZ_AVAIL_READY, .a = 1, .b = 0};
    return 1;
}
static int native_seed_metadata(unsigned deck, struct azstatus_native_receipt *out) {
    assert(deck == 0);
    ++native_metadata_seeds;
    *out = (struct azstatus_native_receipt) {.size = sizeof *out, .version = 1, .kind = AZN_METADATA, .unit = 41, .epoch = 12, .valid = 1, .content_id = 79, .title = "Recovered", .artist = "Source"};
    return 1;
}
static void test_native_gap_seeds(void) {
    ds.enabled = 1;
    struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .load_seed = native_seed_load, .metadata_seed = native_seed_metadata, .stems_seed = native_seed_stems};
    struct azstatus_native_receipt gap = {.size = sizeof gap, .version = 1, .kind = AZN_GAP, .b = AZN_UNIT_END, .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_LOAD) | AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_STEMS)};
    ds_native_consume(&api, 0, &gap);
    assert(native_load_seeds == 1 && native_metadata_seeds == 1 && native_stems_seeds == 1);
    assert(ds.deck[0].current.epoch == 13);
    assert_native_int(AZ_STATUS_STEMS, AZ_STATUS_STEMS_AVAILABLE, 1);
    assert(!strcmp(ds_value(&ds.deck[0].current, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE), "Recovered"));
    native_load_seed_available = 0;
    ds_native_consume(&api, 0, &gap);
    assert(native_load_seeds == 2 && !ds.deck[0].current.epoch && !ds_native_epoch[0]);
    unsigned metadata = native_metadata_seeds, stems = native_stems_seeds;
    api.size = offsetof(struct azstatus_native_v1, metadata_seed);
    gap.lost_topics &= ~AZ_STATUS_TOPIC(AZ_STATUS_LOAD);
    ds_native_consume(&api, 0, &gap);
    assert(native_metadata_seeds == metadata && native_stems_seeds == stems);
    puts("PASS capture-gap reseeding binds the load before dependent fields, clears unproved loads and respects optional API tails");
}

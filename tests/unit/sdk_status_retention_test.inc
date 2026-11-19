/* SPDX-License-Identifier: GPL-3.0-only */
static void payload_identity(void) {
    az_beat beats[] = {{0, 120, 1, 0}, {.5, 120, 2, 0}};
    int64_t ticks[] = {0, 24000};
    struct ds_time_axis clock = {48000, ticks, 1, 48000, NULL};
    struct ds_field grid = ready(AZ_STATUS_GRID, AZ_STATUS_GRID_SAVED, beats, 2);
    grid.info.info.provenance = AZ_PROV_FILE;
    grid.clock = &clock;
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    struct ds_image *im = &ds.deck[0].current;
    uint64_t sequence = im->sequence;
    beats[0].time_s = -0.0;
    assert(ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(im->sequence == sequence);
    for (unsigned change = 0; change < 4; change++) {
        if (change == 0) beats[1].bpm = 121;
        if (change == 1) beats[1].beat_in_bar = 3;
        if (change == 2) beats[1].reserved = 1;
        if (change == 3) {
            ticks[1] = 30000;
            beats[1].time_s = .625;
        }
        assert(ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
        assert(im->sequence > sequence);
        sequence = im->sequence;
    }
    uint8_t samples[] = {3, 9};
    az_waveform wave = {sizeof wave, AZ_DATA_VERSION, AZ_WAVE_PREVIEW, 2, 0, 1, samples};
    int64_t duration = 2;
    struct ds_time_axis duration_clock = {1, &duration, 1, 1, NULL};
    struct ds_field picture = ready(AZ_STATUS_OVERVIEW, AZ_STATUS_OVERVIEW_SAVED, &wave, 1);
    picture.info.info.provenance = AZ_PROV_FILE;
    picture.clock = &duration_clock;
    assert(ds_publish(0, 1, 0, ds_now(), &picture, 1, NULL, NULL, 0, 0) == AZ_OK);
    sequence = im->sequence;
    assert(ds_publish(0, 1, 0, ds_now(), &picture, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(im->sequence == sequence);
    samples[0] = 4;
    assert(ds_publish(0, 1, 0, ds_now(), &picture, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(im->sequence > sequence);
    sequence = im->sequence;
    duration = 4;
    wave.seconds_per_sample = 2;
    assert(ds_publish(0, 1, 0, ds_now(), &picture, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(im->sequence > sequence);
    puts("PASS FILE grid and waveform identity uses canonical source clocks and payload values, suppressing equal copies while preserving edits");
}

static void memory_budget(void) {
    enum {
        ROWS = 32768,
        PINS = 64
    };
    az_beat *beats = calloc(ROWS, sizeof *beats);
    int64_t *ticks = calloc(ROWS, sizeof *ticks);
    struct ds_image *pins = calloc(PINS, sizeof *pins);
    assert(beats && ticks && pins);
    for (unsigned i = 0; i < ROWS; i++) {
        beats[i] = (az_beat) {i * .5, 120, i % 4 + 1, 0};
        ticks[i] = i * 24000;
    }
    struct ds_time_axis clock = {48000, ticks, 1, 48000, NULL};
    struct ds_field grid = ready(AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, beats, ROWS);
    grid.clock = &clock;
    for (unsigned deck = 0; deck < 4; deck++)
        assert(ds_publish(deck, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    struct trace trace = {0};
    az_deck_subscription handle = watch(0, AZ_STATUS_TOPIC(AZ_STATUS_GRID), &trace);
    ds_drain(8);
    for (unsigned i = 0; i < 100; i++) {
        grid.info.info.revision++;
        assert(ds_publish(i % 4, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    }
    assert(ds.metrics[0].evictions && ds.deck[0].length < DS_HISTORY);
    ds_drain(8);
    assert(trace.recoveries && trace.lost == AZ_STATUS_TOPIC(AZ_STATUS_GRID));
    unsigned count = 0;
    int rc = AZ_OK;
    for (; count < PINS && rc == AZ_OK; count++) {
        ds_image_copy(&pins[count], &ds.deck[0].current);
        grid.info.info.revision++;
        rc = ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0);
    }
    assert(rc == AZ_LIMIT && count < PINS && ds.bytes <= DS_BYTES);
    assert(!ds_value(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT));
    ds_drain(8);
    assert(trace.lost & AZ_STATUS_TOPIC(AZ_STATUS_GRID));
    for (unsigned i = 0; i < count; i++) {
        const az_beat *saved = ds_value(&pins[i], AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
        assert(saved && saved[ROWS - 1].beat_in_bar == 4);
        assert_seconds(saved[ROWS - 1].time_s, (ROWS - 1) * .5, 48000, 1);
        ds_image_copy(&pins[i], NULL);
    }
    assert(ds_release(0, handle) == AZ_OK);
    grid.info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), &grid, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_value(&ds.deck[0].current, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT));
    free(pins);
    free(ticks);
    free(beats);
    puts("PASS global byte budget evicts oldest history, reports cursor loss, preserves pinned grids, withdraws on exhaustion and recovers after release");
}

static unsigned stem_calls;
static uint64_t stem_epoch, stem_binding;
static char stem_path[128];
static void capture_stem_binding(unsigned deck, uint64_t binding, uint64_t epoch, const char *path) {
    assert(deck == 0 && binding > stem_binding);
    stem_calls++;
    stem_binding = binding;
    stem_epoch = epoch;
    snprintf(stem_path, sizeof stem_path, "%s", path ? path : "");
}
static void stem_source_binding(void) {
    ds_bind_stems = capture_stem_binding;
    ds.deck[0].watch_stems = 1;
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    assert(stem_calls == 1 && stem_epoch == 1 && !stem_path[0]);
    const az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 2, 3, 4, 0};
    struct ds_field fields[] = {ready(AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, &ref, 1),
                                ready(AZ_STATUS_METADATA, AZ_STATUS_METADATA_PATH, "/music/song.wav", 15)};
    fields[1].info.info.provenance = AZ_PROV_FILE;
    assert(ds_publish(0, 1, 0, ds_now(), fields, 2, NULL, NULL, 0, 0) == AZ_OK);
    assert(!strcmp(stem_path, "/music/song.wav"));
    fields[1].value = "relative.wav";
    fields[1].info.count = 12;
    fields[1].info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!stem_path[0]);
    fields[1].value = "/music/song.wav";
    fields[1].info.count = 15;
    fields[1].info.info.provenance = AZ_PROV_NATIVE;
    fields[1].info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!stem_path[0]);
    fields[1].info.info.provenance = AZ_PROV_FILE;
    fields[1].info.info.revision++;
    assert(ds_publish(0, 1, 0, ds_now(), fields + 1, 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(!strcmp(stem_path, "/music/song.wav"));
    assert(ds_retire_file_binding(0, 1, &ref) == AZ_OK);
    assert(!stem_path[0]);
    assert(ds_publish(0, 2, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    assert(stem_epoch == 2 && !stem_path[0]);
    ds_bind_stems = NULL;
    puts("PASS stems receive only an absolute FILE path qualified by the current ready track; withdrawal and new loads remove it");
}

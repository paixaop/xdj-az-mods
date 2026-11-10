/* SPDX-License-Identifier: GPL-3.0-only */
static void test_duration_reducer(void) {
    const unsigned d = 3;
    const uint64_t load = 100001, fence = 100002;
    struct azstatus_native_receipt r = {.size = sizeof r, .version = 1, .kind = AZN_LOAD, .deck = d,
        .unit = 84, .epoch = fence, .load_generation = load, .valid = 1, .a = AZN_LOADED};
    ds_native_apply(&r);
    az_status_coord_v1 head = {.seconds = 1, .samples = 48000, .rate_hz = 48000};
    struct ds_time_axis head_clock = {.units_per_second = 48000, .window_samples = 1, .window_rate = 48000};
    int playing = AZ_PLAYBACK_PLAYING, forward = AZ_STATUS_DIRECTION_FORWARD, off = 0;
    double rate = 2;
    struct ds_field input[] = {
        ds_native_field(&r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, AZ_AVAIL_READY, &head),
        ds_native_field(&r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, AZ_AVAIL_READY, &playing),
        ds_native_field(&r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, AZ_AVAIL_READY, &forward),
        ds_native_field(&r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE, AZ_AVAIL_READY, &rate),
        ds_native_field(&r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, AZ_AVAIL_READY, &off),
        ds_native_field(&r, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_SLIP, AZ_AVAIL_READY, &off)};
    input[0].clock = &head_clock;
    assert(ds_native_stamp(d, input, 6));
    assert(ds_publish(d, load, 0, 1, input, 6, NULL, NULL, 0, 0) == AZ_OK);
    struct ds_image *im = &ds.deck[d].current;
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    r.kind = AZN_DURATION;
    r.points[0] = (struct azn_point) {AZN_COORD, 48000, 960000};
    ds_native_apply(&r);
    const struct ds_cell *duration = ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S);
    const struct ds_cell *precision = ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S);
    assert(duration && precision && duration->info.scope == AZ_STATUS_LOAD_SCOPE && duration->info.scope_id == load);
    assert(duration->info.info.provenance == AZ_PROV_NATIVE && duration->clock.ticks && precision->clock.ticks);
    assert(compare_timestamps(duration->clock.ticks[0], 960000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(precision->clock.ticks[0], 1, 48000, 48000, 1) == AZ_TIME_EQUAL);
    const double *remaining = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S);
    const double *estimated = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S);
    assert(remaining && estimated && compare_timestamps(*remaining, 19., 1, 48000, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(*estimated, 9.5, 1, 96000, 1) == AZ_TIME_EQUAL);
    assert(*(const int *) ds_value(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE) == AZ_PLAYBACK_PLAYING);
    uint64_t revision = duration->info.info.revision, sequence = im->sequence;
    ds_native_apply(&r);
    assert(im->sequence == sequence);
    ++r.points[0].samples;
    ds_native_apply(&r);
    assert(ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S)->info.info.revision > revision);
    const struct ds_cell *head_cell = ds_ready(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    struct azstatus_native_receipt gap = {.kind = AZN_GAP, .deck = d, .b = AZN_DURATION,
        .lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION)};
    ds_native_apply(&gap);
    assert(!ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S));
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S));
    assert(ds_ready(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD) == head_cell && ds.deck[d].head_window_rate == 48000);
    ds_native_apply(&r);
    assert(ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    r.points[0].rate = 0;
    ds_native_apply(&r);
    assert(!ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S));
    double fallback = 21;
    int64_t file_ticks = 21;
    struct ds_time_axis file_clock = {.units_per_second = 1, .ticks = &file_ticks, .window_samples = 1, .window_rate = 1};
    struct ds_field stored = {.topic = AZ_STATUS_METADATA, .field = AZ_STATUS_METADATA_DURATION_S,
        .info = {.info = {AZ_AVAIL_READY, AZ_PROV_FILE, 1, 1}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = load,
                 .provider_id = AZSP_FILE | 1, .count = 1}, .value = &fallback, .clock = &file_clock};
    assert(ds_publish(d, load, 0, 1, &stored, 1, NULL, NULL, 0, 0) == AZ_OK);
    ds_native_apply(&r);
    assert(ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S)->info.info.provenance == AZ_PROV_FILE);
    r.points[0].rate = 48000;
    r.points[0].samples = 960000;
    ds_native_apply(&r);
    assert(ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S)->info.info.provenance == AZ_PROV_NATIVE);
    /* Loop/slip/pause suppress wall-time estimates while source remaining survives. */
    int on = 1;
    input[4] = ds_native_field(&r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, AZ_AVAIL_READY, &on);
    assert(ds_native_stamp(d, &input[4], 1));
    assert(ds_publish(d, load, 0, 1, &input[4], 1, NULL, NULL, 0, 0) == AZ_OK);
    assert(ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S));
    /* Exact duration neither stamps ENDED nor emits a phrase transition. */
    assert(*(const int *) ds_value(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE) == AZ_PLAYBACK_PLAYING);
    assert(!ds_value(im, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE));
    struct azstatus_native_receipt old = r;
    r.kind = AZN_LOAD; r.a = AZN_LOADING; ++r.epoch;
    ds_native_apply(&r);
    ds_native_apply(&old);
    assert(!ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S));
    assert(!ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S));
    puts("PASS native duration/precision, late remaining, estimate guards, source loss, FILE fallback and load fencing");
}

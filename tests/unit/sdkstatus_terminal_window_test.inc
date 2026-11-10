/* Consecutive source-qualified terminal measurements keep the final phrase
 * until the independent native END decision. Their windows need not match. */
static void test_terminal_window_refinement(void) {
    ds.enabled = 1;
    const unsigned d = 0;
    {
        /* A genuine gap before the final phrase is not its deferred end.
         * A long continuous leg can begin there and reach the native limit. */
        const int64_t ticks[] = {0, 100, 500, 1000};
        const struct ds_time_axis clock = {44100, ticks, 1, 44100, NULL};
        const az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {2, 3, 2, 1, 0, 0}};
        const struct ds_map map = {.rows = rows, .clock = &clock, .count = 2};
        const struct ds_path path = {.kind = DS_PATH_TRAVEL, .endpoints = 3, .defer_terminal = 1,
            .from = {.samples = 200, .rate_hz = 44100}, .to = {.samples = 1002, .rate_hz = 44100},
            .terminal = {.samples = 1000, .rate_hz = 44100},
            .from_window_rate = 44100, .from_window_samples = 64,
            .observed_window_rate = 44100, .observed_window_samples = 64};
        az_status_phrase_endpoint_v1 departure, landing;
        assert(ds_classify(&map, &path, 1, &departure) && departure.kind == AZ_PHRASE_GAP);
        assert(ds_classify(&map, &path, 0, &landing) && landing.kind == AZ_PHRASE_ITEM && landing.index == 1);
    }
    for (unsigned mode = 0; mode < 7; ++mode) {
        const uint64_t load = ds.deck[d].last_epoch + 1, fence = ds_native_fence[d] + 1;
        const int64_t end = 7883316;
        az_status_grid_ref_v1 grid = {load, 10, 1, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
        const int64_t ticks[] = {0, 4410000, mode == 6 ? end - 128 : end};
        az_beat beats[] = {{0, 120, 1, 0}, {100, 120, 2, 0}, {(double) ticks[2] / 44100, 120, 3, 0}};
        const struct ds_time_axis clock = {44100, ticks, 1, 44100, NULL};
        az_phrase rows[] = {{0, 1, 1, 1, 0, 0}, {1, 2, 2, 1, 0, 0}};
        az_status_coord_v1 head = {.samples = mode == 6 ? end - 256 : end - 128, .rate_hz = 44100};
        struct ds_field fields[] = {
            {AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT, {.count = 3}, beats, &clock},
            {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT, {.count = 2}, rows, NULL},
            {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, {.count = 1}, &grid, NULL},
            {AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, {.count = 1}, &head, NULL}};
        for (unsigned n = 0; n < 4; ++n) {
            fields[n].info.info = (az_component_info) {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, 1};
            fields[n].info.scope = AZ_STATUS_LOAD_SCOPE;
            fields[n].info.scope_id = load;
            fields[n].info.provider_id = n == 3 ? 4 : 10;
        }
        assert(ds_publish(d, load, DS_BOUNDARY, 1, fields, 4, NULL, NULL, 0, 0) == AZ_OK);
        ds_native_epoch[d] = load;
        ds_native_fence[d] = fence;
        struct azstatus_native_receipt r = {
            .size = sizeof r, .version = 1, .kind = AZN_MOTION, .a = AZN_MOTION_SEED, .deck = d,
            .unit = 4, .epoch = fence, .load_generation = load, .motion_order = 1, .motion_generation = 1,
            .map = {.owner = 6, .grid = {.provider = 10, .revision = 1}, .phrases = {.provider = 10, .revision = 1}},
            .points = {{AZN_INVALID, 0, 0}, {AZN_COORD, 44100, head.samples}},
            .observed_window_samples = 64, .observed_window_rate = 44100,
            .from_window_samples = 64, .from_window_rate = 44100};
        assert(ds_native_motion(&r, r.motion_order) == AZ_OK);
        const az_status_phrase_endpoint_v1 *active = ds_value(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        assert(active && active->kind == AZ_PHRASE_ITEM && active->index == 1);
        r.a = AZN_MEASURE;
        r.motion_flags = AZN_TERMINAL_ARRIVAL;
        r.points[2] = (struct azn_point) {AZN_COORD, 44100, end};
        r.points[0] = r.points[1];
        r.points[1].samples = end - 63;
        ++r.motion_order;
        assert(ds_native_motion(&r, r.motion_order) == AZ_OK);
        r.points[0] = r.points[1];
        r.points[1].samples = end + 2;
        ++r.motion_order;
        if (mode == 1) r.observed_window_samples = 3;
        if (mode == 2) r.motion_flags = 0;
        if (mode == 3) ++r.map.phrases.revision;
        if (mode == 4) r.from_window_samples = 0;
        if (mode == 5) r.points[0].samples = end + 64;
        const uint64_t before = ds.deck[d].current.sequence;
        int rc = ds_native_motion(&r, r.motion_order);
        if (mode == 4 || mode == 5) {
            assert(rc == AZ_INVALID && ds.deck[d].current.sequence == before);
            continue;
        }
        assert(rc == AZ_OK);
        struct ds_record *last = &ds.deck[d].history[(ds.deck[d].next + DS_HISTORY - 1) % DS_HISTORY];
        if (mode == 2 || mode == 3) {
            assert(last->lost & DS_PHRASE_ROUTE);
            continue;
        }
        assert(!last->lost);
        active = ds_value(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        assert(active && active->kind == (mode == 6 ? AZ_PHRASE_GAP : AZ_PHRASE_ITEM));
        if (mode != 6) assert(active->index == 1);
        r.kind = AZN_END;
        r.a = r.valid = 1;
        r.points[0] = r.points[1] = (struct azn_point) {AZN_COORD, 44100, end};
        r.observed_window_samples = r.from_window_samples = 1;
        r.motion_flags = AZN_TERMINAL_CONFIRMED | AZN_TERMINAL_CONTINUATION;
        ++r.motion_order;
        ds_native_apply(&r);
        last = &ds.deck[d].history[(ds.deck[d].next + DS_HISTORY - 1) % DS_HISTORY];
        assert(!last->lost && last->pair.size && last->pair.from.kind == (mode == 6 ? AZ_PHRASE_GAP : AZ_PHRASE_ITEM) && last->pair.to.kind == AZ_PHRASE_END);
        if (mode != 6) assert(last->pair.from.index == 1);
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            ++r.motion_order;
            ds_native_apply(&r);
        }
        unsigned endings = 0;
        for (unsigned i = 0; i < ds.deck[d].length; ++i) {
            const struct ds_record *record = &ds.deck[d].history[i];
            if (record->image.epoch == load && record->pair.size && record->pair.to.kind == AZ_PHRASE_END) ++endings;
        }
        assert(endings == 1);
    }
    puts("PASS terminal window refinement, single ITEM-to-END, real tail GAP and unavailable-clock/map negatives");
}

/* SPDX-License-Identifier: GPL-3.0-only */
static void test_stationary_measurements(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t core) {
    Buffer_100 readable = {0}, manager = {0}, mode = {0}, engine = {0};
    __auto_type profile = &(rate_profiles[state().build - 1]);
    double advances[64] = {0};
    put(core + 0xa8, ((uintptr_t) readable.bytes));
    put(core + 9, (uint8_t) (1));
    put(core + 0x28, (uint64_t) (41409));
    put(core + 0x68, (uint32_t) (44100));
    put(((uintptr_t) readable.bytes), profile->readable);
    put(((uintptr_t) readable.bytes) + 0x18, ((uintptr_t) manager.bytes));
    put(((uintptr_t) manager.bytes), profile->manager);
    put(((uintptr_t) manager.bytes) + 0x18, ((uintptr_t) mode.bytes));
    put(((uintptr_t) manager.bytes) + 0x38, ((uintptr_t) engine.bytes));
    put(((uintptr_t) mode.bytes), profile->mode);
    put(((uintptr_t) engine.bytes), profile->pitch);
    put(((uintptr_t) engine.bytes) + 0x18, ((uintptr_t) (advances)));
    put(((uintptr_t) engine.bytes) + 0x20, (uint64_t) (64));
    atomic_store_explicit(&(rate_generator_hooked), true, memory_order_seq_cst);
    __auto_type m = &(slot->motion);
    m->load = fixture_load.load_generation;
    m->order = 100;
    m->generation = 7;
    m->known = true;
    m->previous = m->opening_sample = 41409;
    m->rate = m->opening_rate = m->window_rate = 44100;
    m->window_samples = 64;
    atomic_store_explicit(&(m->opening), true, memory_order_seq_cst);
    m->captured_interference = atomic_load_explicit(&(m->interference), memory_order_seq_cst);
    m->seed = azn_seed_id(0);
    memcpy(m->identity, ((const void *) (core + 0x18)), 12);
    end_state[0].load = 0;
    end_state[0].pending_order = 0;
    atomic_store_explicit(&(end_state[0].relevant), 0, memory_order_seq_cst);
    atomic_store_explicit(&(end_state[0].refresh), 0, memory_order_seq_cst);


#define fixture_render_3(frames_argument, generated_argument, generated_count_argument)                                                                                     \
    do {                                                                                                                                                                    \
        unsigned fixture_render_3_frames = (frames_argument);                                                                                                               \
        unsigned fixture_render_3_generated = (generated_argument);                                                                                                         \
        int fixture_render_3_generated_count = (generated_count_argument);                                                                                                  \
        RateRenderScope source __attribute__((cleanup(rate_render_drop)));                                                                                                  \
        rate_render_init(&source, ((void *) (core)));                                                                                                                       \
        StatusMotionWrite writer __attribute__((cleanup(status_motion_write_drop)));                                                                                        \
        status_motion_write_init(&writer, ((void *) (core)), false, fixture_render_3_frames);                                                                               \
        for (unsigned n = 0; n < fixture_render_3_generated; ++n)                                                                                                           \
            rate_render_generated(((uintptr_t) engine.bytes), fixture_render_3_generated_count < 0 ? (int64_t) fixture_render_3_frames : fixture_render_3_generated_count); \
        status_motion_write_finish(&writer, 0);                                                                                                                             \
    } while (0)




#define fixture_empty_4()                \
    do {                                 \
        azstatus_native_receipt r = {0}; \
        r.size = sizeof r;               \
        r.version = 1;                   \
        assert(!api->take(0, &r));       \
    } while (0)




#define fixture_next_5(order_argument) ({                                                                                   \
    uint64_t fixture_next_5_order = (order_argument);                                                                       \
    __auto_type r = take(api);                                                                                              \
    assert(r.kind == AZN_MOTION && r.a == AZN_MEASURE && !r.lost_topics && !r.c && r.motion_order == fixture_next_5_order); \
    r;                                                                                                                      \
})


    /* More than two complete lanes of actual paused render calls retain one
     * successful opening measurement, without gaps or skipped shared orders. */
    for (unsigned n = 0; n < 600; ++n) fixture_render_3(64, 1, -1);
    __auto_type r = fixture_next_5(101);
    assert(r.motion_flags == AZN_AT_OPENING && point_at(r.points[0], 41409, 1) && point_at(r.points[1], 41409, 1));
    fixture_empty_4();
    assert(m->order == 101);
    atomic_store_explicit(&(m->opening), false, memory_order_seq_cst);
    fixture_render_3(64, 1, -1);
    assert(!fixture_next_5(102).motion_flags);
    for (unsigned n = 0; n < 600; ++n) fixture_render_3(64, 1, -1);
    fixture_empty_4();
    assert(m->order == 102);

    /* Changed source windows and map publication remain visible. A map cut
     * occupies the same sequence as a head write, so it breaks the repeat. */
    fixture_render_3(32, 1, -1);
    r = fixture_next_5(103);
    assert(r.from_window_samples == 64 && r.observed_window_samples == 32);
    fixture_render_3(32, 1, -1);
    r = fixture_next_5(104);
    assert(r.from_window_samples == 32 && r.observed_window_samples == 32);
    fixture_render_3(32, 1, -1);
    fixture_empty_4();
    azstatus_native_receipt map = {0};
    map.kind = AZN_MAPS;
    map.load_generation = fixture_load.load_generation;
    map.map.owner = 77;
    map.map.grid.provider = map.map.phrases.provider = 78;
    map.map.grid.revision = map.map.phrases.revision = 5;
    map.map.grid.content = map.map.phrases.content = 1;
    assert(offer_map_cut(&map));
    r = take(api);
    assert(r.kind == AZN_MAPS && r.motion_order == 105);
    fixture_render_3(32, 1, -1);
    assert(fixture_next_5(106).map.grid.revision == 5);
    fixture_render_3(32, 1, -1);
    fixture_empty_4();

    /* A fractional or net-zero excursion still carries a measurement even
     * if the final integer head matches. Unknown/multiple generators do too. */
    for (unsigned negative = 0; negative < 6; ++negative) {
        fill_samples(advances, advances + 64, 0.0);
        if (negative == 0) advances[0] = 0.125;
        if (negative == 1) {
            advances[0] = 1;
            advances[1] = -1;
        }
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            uint64_t before = m->order;
            if (negative == 3) {
                StatusMotionWrite unqualified __attribute__((cleanup(status_motion_write_drop)));
                status_motion_write_init(&unqualified, ((void *) (core)), false, 32);
                status_motion_write_finish(&unqualified, 0);
            } else fixture_render_3(32, negative == 2 ? 2 : 1, negative == 4 ? 31 : negative == 5 ? 0
                                                                                                  : 32);
            fixture_next_5(before + 1);
        }
        fixture_empty_4();
    }
    fill_samples(advances, advances + 64, 0.0);
    fixture_render_3(32, 1, -1);
    fixture_next_5(m->order);
    /* No generator means no source traversal on the independently qualified
     * internal clear branch; a stale nonzero vector must not be read there. */
    advances[0] = 1;
    fixture_render_3(32, 0, -1);
    fixture_empty_4();
    advances[0] = 0;
    atomic_store_explicit(&(end_state[0].relevant), 1, memory_order_seq_cst);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        uint64_t before = m->order;
        fixture_render_3(32, 1, -1);
        fixture_next_5(before + 1);
    }
    atomic_store_explicit(&(end_state[0].relevant), 0, memory_order_seq_cst);
    azn_motion_gap(0, 0);
    fixture_render_3(32, 1, -1);
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == motion_routes);
    fixture_render_3(32, 1, -1);
    r = take(api);
    assert(r.kind == AZN_MOTION && r.c == 1);
    fixture_empty_4();
    atomic_store_explicit(&(rate_generator_hooked), false, memory_order_seq_cst);
    put(core + 0xa8, (uintptr_t) (0));
    puts("PASS stationary measurement dedup, opening/window/map ordering, real excursions, terminal and loss recovery");

#undef fixture_render_3

#undef fixture_empty_4

#undef fixture_next_5
}

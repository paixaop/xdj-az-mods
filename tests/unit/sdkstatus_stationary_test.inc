/* SPDX-License-Identifier: GPL-3.0-only */
static void test_stationary_measurements(const azstatus_native_v1 *api, Slot &slot, uintptr_t core) {
    Buffer<0x100> readable, manager, mode, engine;
    const auto &profile = rate_profiles[state().build - 1];
    double advances[64] = {};
    put(core + 0xa8, readable.addr());
    put(core + 9, uint8_t(1));
    put(core + 0x28, uint64_t(41409));
    put(core + 0x68, uint32_t(44100));
    put(readable.addr(), profile.readable);
    put(readable.addr() + 0x18, manager.addr());
    put(manager.addr(), profile.manager);
    put(manager.addr() + 0x18, mode.addr());
    put(manager.addr() + 0x38, engine.addr());
    put(mode.addr(), profile.mode);
    put(engine.addr(), profile.pitch);
    put(engine.addr() + 0x18, reinterpret_cast<uintptr_t>(advances));
    put(engine.addr() + 0x20, uint64_t(64));
    rate_generator_hooked.store(true);
    auto &m = slot.motion;
    m.load = fixture_load.load_generation;
    m.order = 100;
    m.generation = 7;
    m.known = true;
    m.previous = m.opening_sample = 41409;
    m.rate = m.opening_rate = m.window_rate = 44100;
    m.window_samples = 64;
    m.opening.store(true);
    m.captured_interference = m.interference.load();
    m.seed = azn_seed_id(0);
    memcpy(m.identity, reinterpret_cast<const void *>(core + 0x18), 12);
    end_state[0].load = 0;
    end_state[0].pending_order = 0;
    end_state[0].relevant.store(0);
    end_state[0].refresh.store(0);
    auto render = [&](unsigned frames, unsigned generated = 1, int generated_count = -1) {
        RateRenderScope source(reinterpret_cast<void *>(core));
        StatusMotionWrite writer(reinterpret_cast<void *>(core), false, frames);
        for (unsigned n = 0; n < generated; ++n)
            RateRenderScope::generated(engine.addr(), generated_count < 0 ? frames : generated_count);
        writer.finish();
    };
    auto empty = [&]() {
        azstatus_native_receipt r {};
        r.size = sizeof r;
        r.version = 1;
        assert(!api->take(0, &r));
    };
    auto next = [&](uint64_t order) {
        auto r = take(api);
        assert(r.kind == AZN_MOTION && r.a == AZN_MEASURE && !r.lost_topics && !r.c && r.motion_order == order);
        return r;
    };
    /* More than two complete lanes of actual paused render calls retain one
     * successful opening measurement, without gaps or skipped shared orders. */
    for (unsigned n = 0; n < 600; ++n) render(64);
    auto r = next(101);
    assert(r.motion_flags == AZN_AT_OPENING && point_at(r.points[0], 41409) && point_at(r.points[1], 41409));
    empty();
    assert(m.order == 101);
    m.opening.store(false);
    render(64);
    assert(!next(102).motion_flags);
    for (unsigned n = 0; n < 600; ++n) render(64);
    empty();
    assert(m.order == 102);

    /* Changed source windows and map publication remain visible. A map cut
     * occupies the same sequence as a head write, so it breaks the repeat. */
    render(32);
    r = next(103);
    assert(r.from_window_samples == 64 && r.observed_window_samples == 32);
    render(32);
    r = next(104);
    assert(r.from_window_samples == 32 && r.observed_window_samples == 32);
    render(32);
    empty();
    azstatus_native_receipt map {};
    map.kind = AZN_MAPS;
    map.load_generation = fixture_load.load_generation;
    map.map.owner = 77;
    map.map.grid.provider = map.map.phrases.provider = 78;
    map.map.grid.revision = map.map.phrases.revision = 5;
    map.map.grid.content = map.map.phrases.content = 1;
    assert(offer_map_cut(&map));
    r = take(api);
    assert(r.kind == AZN_MAPS && r.motion_order == 105);
    render(32);
    assert(next(106).map.grid.revision == 5);
    render(32);
    empty();

    /* A fractional or net-zero excursion still carries a measurement even
     * if the final integer head matches. Unknown/multiple generators do too. */
    for (unsigned negative = 0; negative < 6; ++negative) {
        std::fill(advances, advances + 64, 0.0);
        if (negative == 0) advances[0] = 0.125;
        if (negative == 1) { advances[0] = 1; advances[1] = -1; }
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            uint64_t before = m.order;
            if (negative == 3) {
                StatusMotionWrite unqualified(reinterpret_cast<void *>(core), false, 32);
                unqualified.finish();
            } else render(32, negative == 2 ? 2 : 1, negative == 4 ? 31 : negative == 5 ? 0 : 32);
            next(before + 1);
        }
        empty();
    }
    std::fill(advances, advances + 64, 0.0);
    render(32);
    next(m.order);
    /* No generator means no source traversal on the independently qualified
     * internal clear branch; a stale nonzero vector must not be read there. */
    advances[0] = 1;
    render(32, 0);
    empty();
    advances[0] = 0;
    end_state[0].relevant.store(1);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        uint64_t before = m.order;
        render(32);
        next(before + 1);
    }
    end_state[0].relevant.store(0);
    azn_motion_gap(0, 0);
    render(32);
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == motion_routes);
    render(32);
    r = take(api);
    assert(r.kind == AZN_MOTION && r.c == 1);
    empty();
    rate_generator_hooked.store(false);
    put(core + 0xa8, uintptr_t(0));
    puts("PASS stationary measurement dedup, opening/window/map ordering, real excursions, terminal and loss recovery");
}

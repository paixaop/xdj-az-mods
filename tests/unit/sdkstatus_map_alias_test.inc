/* SPDX-License-Identifier: GPL-3.0-only */
extern "C" void gui_alias_run(unsigned, void (*)(void));
static uintptr_t alias_core;
static void alias_render() {
    StatusMotionWrite write(reinterpret_cast<void *>(alias_core), false, 64);
    put(alias_core + 0x28, word(alias_core + 0x28) + UINT64_C(64));
    write.finish();
}
static void test_gui_map_alias(const azstatus_native_v1 *api, Slot &slot, uintptr_t core) {
    alias_core = core;
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    processing = &slot;
    state().motion_hooked = true;
    put(core + 9, uint8_t(1));
    put(core + 0x20, matched_id);
    put(core + 0x68, uint32_t(44100));
    end_state[0].load = 0;
    end_state[0].relevant.store(false); end_state[0].refresh.store(false);
    auto &m = slot.motion;
    unsigned alias_losses = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        put(core + 0x28, uint64_t(1000));
        m.busy.store(Slot::Motion::FREE); m.foreign.store(0);
        m.known = true; m.load = fixture_load.load_generation;
        m.previous = 1000; m.rate = m.window_rate = 44100; m.window_samples = 64;
        m.order = 100; m.generation = 4; m.opening.store(false);
        m.captured_interference = m.interference.load(); m.seed = azn_seed_id(0);
        memcpy(m.identity, reinterpret_cast<void *>(core + 0x18), 12);
        map_mutations[0].active.store(0);
        map_clocks[0].load.store(fixture_load.load_generation); map_clocks[0].serial.store(2);
        map_clocks[0].owner.store(77); map_clocks[0].grid.store(78); map_clocks[0].grid_rev.store(1);
        map_clocks[0].phrase.store(79); map_clocks[0].phrase_rev.store(1);
        const uint64_t generation = map_mutations[0].generation.load();
        gui_alias_run(mode, alias_render);
        auto r = take(api);
        fprintf(stderr, "alias mode=%u kind=%u action=%d lost=%llx from=%lld to=%lld Fs=%u N=%u mapserial=%llu mutation=%llu->%llu active=%u\n",
            mode, r.kind, r.a, (unsigned long long)r.lost_topics, (long long)r.points[0].samples,
            (long long)r.points[1].samples, r.points[1].rate, r.observed_window_samples,
            (unsigned long long)map_clocks[0].serial.load(), (unsigned long long)generation,
            (unsigned long long)map_mutations[0].generation.load(), map_mutations[0].active.load());
        assert(r.kind == AZN_MOTION && r.a == AZN_MEASURE && point_at(r.points[0], 1000) && point_at(r.points[1], 1064));
        assert(r.points[1].rate == 44100 && r.observed_window_samples == 64 && r.from_window_samples == 64);
        assert(r.map.owner == 77 && r.map.grid.revision == 1 && r.map.phrases.revision == 1);
        if (r.lost_topics) ++alias_losses;
    }
    /* Genuine producer brackets, including an A/B/A edit completed inside
     * the writer interval, must retain ambiguity even with matching maps. */
    for (unsigned completed = 0; completed < 2; ++completed) {
        const uint64_t from = word(core + 0x28);
        uint64_t ticket = 0;
        if (!completed) ticket = azn_map_begin_mask(1);
        {
            StatusMotionWrite write(reinterpret_cast<void *>(core), false, 64);
            if (completed) { ticket = azn_map_begin_mask(1); azn_map_end_mask(ticket); }
            put(core + 0x28, from + UINT64_C(64));
            write.finish();
        }
        if (!completed) azn_map_end_mask(ticket);
        auto r = take(api);
        assert(r.a == AZN_MEASURE && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
        assert(point_at(r.points[0], from) && point_at(r.points[1], from + UINT64_C(64)));
    }
    assert(!alias_losses); /* Same shared map; GUI bookkeeping omitted no trajectory. */
    puts("PASS shared-map GUI retain, adoption and clear preserve measured motion; real active and completed mutations retain loss");
}

/* SPDX-License-Identifier: GPL-3.0-only */
void gui_alias_run(unsigned, void (*)(void));
static uintptr_t alias_core;
static void alias_render() {
    StatusMotionWrite write __attribute__((cleanup(status_motion_write_drop)));
    status_motion_write_init(&write, ((void *) (alias_core)), false, 64);
    put(alias_core + 0x28, word(alias_core + 0x28) + UINT64_C(64));
    status_motion_write_finish(&write, 0);
}
static void test_gui_map_alias(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t core) {
    alias_core = core;
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    processing = slot;
    state().motion_hooked = true;
    put(core + 9, (uint8_t) (1));
    put(core + 0x20, matched_id);
    put(core + 0x68, (uint32_t) (44100));
    end_state[0].load = 0;
    atomic_store_explicit(&(end_state[0].relevant), false, memory_order_seq_cst);
    atomic_store_explicit(&(end_state[0].refresh), false, memory_order_seq_cst);
    __auto_type m = &(slot->motion);
    unsigned alias_losses = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        put(core + 0x28, (uint64_t) (1000));
        atomic_store_explicit(&(m->busy), MOTION_FREE, memory_order_seq_cst);
        atomic_store_explicit(&(m->foreign), 0, memory_order_seq_cst);
        m->known = true;
        m->load = fixture_load.load_generation;
        m->previous = 1000;
        m->rate = m->window_rate = 44100;
        m->window_samples = 64;
        m->order = 100;
        m->generation = 4;
        atomic_store_explicit(&(m->opening), false, memory_order_seq_cst);
        m->captured_interference = atomic_load_explicit(&(m->interference), memory_order_seq_cst);
        m->seed = azn_seed_id(0);
        memcpy(m->identity, ((void *) (core + 0x18)), 12);
        atomic_store_explicit(&(map_mutations[0].active), 0, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].load), fixture_load.load_generation, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].serial), 2, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].owner), 77, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].grid), 78, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].grid_rev), 1, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].phrase), 79, memory_order_seq_cst);
        atomic_store_explicit(&(map_clocks[0].phrase_rev), 1, memory_order_seq_cst);
        const uint64_t generation = atomic_load_explicit(&(map_mutations[0].generation), memory_order_seq_cst);
        gui_alias_run(mode, alias_render);
        __auto_type r = take(api);
        fprintf(stderr, "alias mode=%u kind=%u action=%d lost=%llx from=%lld to=%lld Fs=%u N=%u mapserial=%llu mutation=%llu->%llu active=%u\n",
                mode, r.kind, r.a, (unsigned long long) r.lost_topics, (long long) r.points[0].samples,
                (long long) r.points[1].samples, r.points[1].rate, r.observed_window_samples,
                (unsigned long long) atomic_load_explicit(&(map_clocks[0].serial), memory_order_seq_cst), (unsigned long long) generation,
                (unsigned long long) atomic_load_explicit(&(map_mutations[0].generation), memory_order_seq_cst), atomic_load_explicit(&(map_mutations[0].active), memory_order_seq_cst));
        assert(r.kind == AZN_MOTION && r.a == AZN_MEASURE && point_at(r.points[0], 1000, 1) && point_at(r.points[1], 1064, 1));
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
            StatusMotionWrite write __attribute__((cleanup(status_motion_write_drop)));
            status_motion_write_init(&write, ((void *) (core)), false, 64);
            if (completed) {
                ticket = azn_map_begin_mask(1);
                azn_map_end_mask(ticket);
            }
            put(core + 0x28, from + UINT64_C(64));
            status_motion_write_finish(&write, 0);
        }
        if (!completed) azn_map_end_mask(ticket);
        __auto_type r = take(api);
        assert(r.a == AZN_MEASURE && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
        assert(point_at(r.points[0], from, 1) && point_at(r.points[1], from + UINT64_C(64), 1));
    }
    assert(!alias_losses); /* Same shared map; GUI bookkeeping omitted no trajectory. */
    puts("PASS shared-map GUI retain, adoption and clear preserve measured motion; real active and completed mutations retain loss");
}

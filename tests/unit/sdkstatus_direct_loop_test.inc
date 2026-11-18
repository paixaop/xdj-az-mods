/* SPDX-License-Identifier: GPL-3.0-only */
static void test_direct_loop_actions(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t core, uintptr_t property, uintptr_t basis, uintptr_t pause, uintptr_t quantize, uintptr_t controller) {
    unsigned char saved_property[0x100];
    memcpy(saved_property, ((const void *) (property)), sizeof saved_property);
    const unsigned saved_build = state().build;
    Buffer_80 requested = {0};
    api->interest(0, motion_routes | AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    processing = slot;
    put(basis + 0x38, controller);
    put(pause + 0x50, controller);
    put(pause + 0x58, core);
    put(quantize + 0x20, controller);
    put(quantize + 0x40, quantize);
    put(quantize + 0x48, pause);
    /* The preceding settings fixture replaced the facade's Quantize child.
     * Rebind the native setter too, exactly as required for a coherent owner. */
    put(slot->facade + 0x110, quantize);
    assert(coherent(slot) && set_bindings(slot) && table(quantize, state().profile.quantize));
    put(core + 9, (uint8_t) (1));
    put(core + 0x20, matched_id);
    struct Case {
        uint64_t from, to, in, out;
    };
    const struct Case cases[] = {{1169488, 1082700, 1082435, 1125829},
                                 {1104498, 1017753, 1017387, 1060782},
                                 {1109776, 1034187, 1033704, 1071498}};
    for (unsigned build = 1; build <= 2; ++build) {
        state().build = build;
        {
            for (unsigned range_index_3 = 0; range_index_3 < sizeof(cases) / sizeof(cases)[0]; ++range_index_3) {
                __auto_type c = &(cases[range_index_3]);

                point(property + 0x30, c->in);
                point(property + 0x88, c->out);
                memcpy(((void *) (property + 0x30)), ((void *) (core + 0x18)), 12);
                memcpy(((void *) (property + 0x88)), ((void *) (core + 0x18)), 12);
                put(property + 0x60, (uint32_t) (44100));
                put(property + 0xb8, (uint32_t) (44100));
                put(core + 0x68, (uint32_t) (44100));
                memcpy(requested.bytes + 8, ((void *) (core + 0x18)), 12);
                put(((uintptr_t) requested.bytes) + 0x18, c->to);
                LoopView view = {0};
                assert(loop_view(slot, &(view)) && view.points.rate == 44100);
                const uintptr_t caller = noqua_profiles[build - 1].jump_returns[1];
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    __auto_type m = &(slot->motion);
                    put(core + 0x28, c->from);
                    atomic_store_explicit(&(m->busy), MOTION_FREE, memory_order_seq_cst);
                    atomic_store_explicit(&(m->foreign), 0, memory_order_seq_cst);
                    m->known = true;
                    m->load = fixture_load.load_generation;
                    m->previous = c->from;
                    m->rate = m->window_rate = 44100;
                    m->window_samples = 64;
                    atomic_store_explicit(&(m->opening), false, memory_order_seq_cst);
                    m->captured_interference = atomic_load_explicit(&(m->interference), memory_order_seq_cst);
                    m->seed = azn_seed_id(0);
                    memcpy(m->identity, ((void *) (core + 0x18)), 12);
                    const uint64_t ordinal = m->occurrence;
                    {
                        STATUS_ACTION(direct, 0, 0, true);
                        status_direct_request(&(direct), requested.bytes, 0, caller);
                        assert(direct.direct_kind == AZN_JUMP);
                        StatusMotionWrite action __attribute__((cleanup(status_motion_write_drop)));
                        status_motion_write_init(&action, ((void *) (core)), true, 1);
                        put(core + 0x28, c->to);
                        status_motion_write_finish(&action, ((uintptr_t) requested.bytes));
                    }
                    __auto_type r = take(api);
                    assert(r.a == AZN_JUMP && !r.lost_topics && !r.history_only && r.occurrence_ordinal == ordinal + 1);
                    assert(point_at(r.points[0], c->from, 1) && point_at(r.points[1], c->to, 1));
                    assert(r.from_window_samples == 64 && r.observed_window_samples == 1 &&
                           r.from_window_rate == 44100 && r.observed_window_rate == 44100);
                }
                for (unsigned mode = 0; mode < 17; ++mode) {
                    uintptr_t address = 0;
                    uint64_t saved = 0, replacement = 0;
                    unsigned width = 8;
                    int request_mode = 0;
                    uintptr_t request_caller = caller;
                    const uint64_t saved_load = fixture_load.load_generation;
                    switch (mode) {
                    case 0: request_caller += 4; break;
                    case 1: request_mode = 1; break;
                    case 2: address = quantize + 0x20; break;
                    case 3: address = quantize + 0x40; break;
                    case 4: address = quantize + 0x48; break;
                    case 5: address = pause + 0x58; break;
                    case 6:
                        address = ((uintptr_t) requested.bytes) + 0x10;
                        width = 4;
                        replacement = matched_id + 1;
                        break;
                    case 7:
                        address = core + 0x68;
                        width = 4;
                        replacement = 48000;
                        break;
                    case 8:
                        address = ((uintptr_t) requested.bytes) + 0x18;
                        replacement = c->in - 1;
                        break;
                    case 9:
                        address = ((uintptr_t) requested.bytes) + 0x18;
                        replacement = c->out + 1;
                        break;
                    case 10:
                        address = ((uintptr_t) requested.bytes) + 0x18;
                        replacement = UINT64_MAX;
                        break;
                    case 11: address = property + 0x30; break;
                    case 12: fixture_load.load_generation = 0; break;
                    case 13: address = quantize; break;
                    case 14:
                        address = core + 0x68;
                        width = 4;
                        break;
                    case 15:
                        address = property + 0x60;
                        width = 4;
                        break;
                    case 16: break; // unowned controller scope
                    }
                    if (address) {
                        saved = width == 4 ? (uint32_t) (integer(address)) : word(address);
                        if (width == 4) put(address, (uint32_t) (replacement));
                        else put(address, replacement);
                    }
                    STATUS_ACTION(direct, 0, 0, mode != 16);
                    status_direct_request(&(direct), requested.bytes, request_mode, request_caller);
                    assert(!direct.direct_kind);
                    if (address) {
                        if (width == 4) put(address, (uint32_t) (saved));
                        else put(address, saved);
                    }
                    fixture_load.load_generation = saved_load;
                }
                STATUS_ACTION(direct, 0, 0, true);
                status_direct_request(&(direct), requested.bytes, 0, caller);
                assert(direct.direct_kind == AZN_JUMP);
                StatusHead from = status_head(core), to = from;
                from.sample = c->from;
                to.sample = c->to + 1;
                assert(!status_action(slot, from, to)); // a different committed integer point
                to.sample = c->to;
                to.rate = 48000;
                assert(!status_action(slot, from, to)); // captured source cannot change clock domains
            }
        }
    }
    memcpy(((void *) (property)), saved_property, sizeof saved_property);
    state().build = saved_build;
    puts("PASS owned direct loop JUMP interior targets, both caller profiles, exact commits and ownership/source/clock/range negatives");
}

/* SPDX-License-Identifier: GPL-3.0-only */
static void test_transport_retirement() {
    Buffer<0x300> unit, core, facade;
    Buffer<0x100> bundle, transport, pause, cue, tables[5];
    auto &s = state();
    auto &p = s.profile;
    auto &slot = s.decks[0];
    p.unit = tables[0].addr(); p.core = tables[1].addr();
    p.facade = tables[2].addr(); p.box = tables[3].addr();
    p.play_pause_table = tables[4].addr();
    put(unit.addr(), p.unit); put(core.addr(), p.core); put(facade.addr(), p.facade);
    put(unit.addr() + 0x128, core.addr()); put(unit.addr() + 0x190, bundle.addr());
    put(bundle.addr() + 0x40, facade.addr());
    put(facade.addr() + 0x18, core.addr() + 0x110); put(facade.addr() + 0x40, core.addr());
    put(core.addr() + 0x110, p.box);
    put(core.addr() + 0x20, matched_id);
    put(facade.addr() + 0x58, pause.addr()); put(facade.addr() + 0x68, cue.addr());
    put(facade.addr() + 0x78, transport.addr());
    put(transport.addr(), core.addr()); put(transport.addr() + 8, cue.addr());
    put(transport.addr() + 0xa8, pause.addr());
    put(pause.addr(), p.play_pause_table); put(pause.addr() + 0x58, core.addr());
    slot.unit = unit.addr(); slot.facade = facade.addr(); slot.core.store(core.addr());
    transport_installed.store(15);
    const auto *api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    processing = &slot;
    transport_observe(0, slot, TransportSeed);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    processing = nullptr;
    transport_original[2] = reinterpret_cast<void *>(+[](void *self, unsigned value) {
        put(reinterpret_cast<uintptr_t>(self) + 8, uint8_t(value));
    });
    azstatus_native_receipt empty {}; empty.size = sizeof empty; empty.version = 1;
    const auto original = fixture_load;
    /* The load writer already revoked the prior association. Native teardown
     * invokes these same cached children outside the processing context. */
    fixture_load.load_generation = 0;
    ++fixture_load.invalidation_epoch;
    transport_writer<2, unsigned>(cue.bytes, 1);
    transport_writer<2, unsigned>(cue.bytes, 0);
    assert(!api->take(0, &empty));
    fixture_load.load_generation = original.load_generation + 1;
    transport_observe(0, slot, TransportSeed);
    auto r = take(api);
    assert(r.kind == AZN_TRANSPORT_STATE && r.valid == AZ_AVAIL_READY &&
           r.load_generation == fixture_load.load_generation && !r.lost_topics);
    assert(!api->take(0, &empty));
    puts("PASS retired transport children do not poison replacement admission");

    /* A live out-of-context writer still has unobserved history. */
    transport_writer<2, unsigned>(cue.bytes, 1);
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    transport_observe(0, slot, TransportSeed);
    (void)take(api);
    /* Failure to read an unchanged lifecycle can be contention, not retirement. */
    fixture_load_busy = true;
    transport_writer<2, unsigned>(cue.bytes, 0);
    fixture_load_busy = false;
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    transport_observe(0, slot, TransportSeed);
    (void)take(api);
    puts("PASS active and transiently busy transport history still reports loss");
    fixture_load = original;
    processing = nullptr;
}

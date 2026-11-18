/* SPDX-License-Identifier: GPL-3.0-only */
static void native_callback_20(void *self, unsigned value) {
    put(((uintptr_t) (self)) + 8, (uint8_t) (value));
}
static void test_transport_retirement() {
    Buffer_300 unit = {0}, core = {0}, facade = {0};
    Buffer_100 bundle = {0}, transport = {0}, pause = {0}, cue = {0}, tables[5] = {0};
    __auto_type s = &(state());
    __auto_type p = &(s->profile);
    __auto_type slot = &(s->decks[0]);
    p->unit = ((uintptr_t) tables[0].bytes);
    p->core = ((uintptr_t) tables[1].bytes);
    p->facade = ((uintptr_t) tables[2].bytes);
    p->box = ((uintptr_t) tables[3].bytes);
    p->play_pause_table = ((uintptr_t) tables[4].bytes);
    put(((uintptr_t) unit.bytes), p->unit);
    put(((uintptr_t) core.bytes), p->core);
    put(((uintptr_t) facade.bytes), p->facade);
    put(((uintptr_t) unit.bytes) + 0x128, ((uintptr_t) core.bytes));
    put(((uintptr_t) unit.bytes) + 0x190, ((uintptr_t) bundle.bytes));
    put(((uintptr_t) bundle.bytes) + 0x40, ((uintptr_t) facade.bytes));
    put(((uintptr_t) facade.bytes) + 0x18, ((uintptr_t) core.bytes) + 0x110);
    put(((uintptr_t) facade.bytes) + 0x40, ((uintptr_t) core.bytes));
    put(((uintptr_t) core.bytes) + 0x110, p->box);
    put(((uintptr_t) core.bytes) + 0x20, matched_id);
    put(((uintptr_t) facade.bytes) + 0x58, ((uintptr_t) pause.bytes));
    put(((uintptr_t) facade.bytes) + 0x68, ((uintptr_t) cue.bytes));
    put(((uintptr_t) facade.bytes) + 0x78, ((uintptr_t) transport.bytes));
    put(((uintptr_t) transport.bytes), ((uintptr_t) core.bytes));
    put(((uintptr_t) transport.bytes) + 8, ((uintptr_t) cue.bytes));
    put(((uintptr_t) transport.bytes) + 0xa8, ((uintptr_t) pause.bytes));
    put(((uintptr_t) pause.bytes), p->play_pause_table);
    put(((uintptr_t) pause.bytes) + 0x58, ((uintptr_t) core.bytes));
    slot->unit = ((uintptr_t) unit.bytes);
    slot->facade = ((uintptr_t) facade.bytes);
    atomic_store_explicit(&(slot->core), ((uintptr_t) core.bytes), memory_order_seq_cst);
    atomic_store_explicit(&(transport_installed), 15, memory_order_seq_cst);
    __auto_type api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    processing = slot;
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    processing = NULL;
    transport_original[2] = ((void *) (native_callback_20));
    azstatus_native_receipt empty = {0};
    empty.size = sizeof empty;
    empty.version = 1;
    __auto_type original = fixture_load;
    /* The load writer already revoked the prior association. Native teardown
     * invokes these same cached children outside the processing context. */
    fixture_load.load_generation = 0;
    ++fixture_load.invalidation_epoch;
    transport_writer_2(cue.bytes, 1);
    transport_writer_2(cue.bytes, 0);
    assert(!api->take(0, &empty));
    fixture_load.load_generation = original.load_generation + 1;
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    __auto_type r = take(api);
    assert(r.kind == AZN_TRANSPORT_STATE && r.valid == AZ_AVAIL_READY &&
           r.load_generation == fixture_load.load_generation && !r.lost_topics);
    assert(!api->take(0, &empty));
    puts("PASS retired transport children do not poison replacement admission");

    /* A live out-of-context writer still has unobserved history. */
    transport_writer_2(cue.bytes, 1);
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    (void) take(api);
    /* Failure to read an unchanged lifecycle can be contention, not retirement. */
    fixture_load_busy = true;
    transport_writer_2(cue.bytes, 0);
    fixture_load_busy = false;
    r = take(api);
    assert(r.kind == AZN_GAP && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    (void) take(api);
    puts("PASS active and transiently busy transport history still reports loss");
    fixture_load = original;
    processing = NULL;
}

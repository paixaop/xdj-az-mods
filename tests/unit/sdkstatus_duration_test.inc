/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the native fixture: the writer owns each row and the gate owns
 * the independent current core. No FILE metadata participates in these cuts. */
#include "azstatus_memory.h"
constexpr bool duration_capabilities_unique(uint64_t) { return true; }
template<class... Rest>
constexpr bool duration_capabilities_unique(uint64_t seen, unsigned first, Rest... rest) {
    return first && !(first & (first - 1u)) && !(seen & first) &&
           duration_capabilities_unique(seen | first, rest...);
}
static_assert(duration_capabilities_unique(0, AZN_CAP_LOAD, AZN_CAP_LOOP, AZN_CAP_LTI, AZN_CAP_MOTION,
    AZN_CAP_PLAY_RAW, AZN_CAP_STEMS, AZN_CAP_QUANTIZE, AZN_CAP_KEY_LOCK, AZN_CAP_SYNC, AZN_CAP_SLIDER_LOCK,
    AZN_CAP_MAPS, AZN_CAP_HOT, AZN_CAP_LOOP_MODE, AZN_CAP_RELOOP, AZN_CAP_HOT_LINK, AZN_CAP_RATE,
    AZN_CAP_SYNC_ROLE, AZN_CAP_TRANSPORT_STATE, AZN_CAP_SLIP_HEAD, AZN_CAP_KEY_SHIFT, AZN_CAP_END,
    AZN_CAP_DURATION, AZN_CAP_MEMORY, AZN_CAP_MEMORY_LINK, AZN_CAP_ORIGINAL_BPM),
    "native capability bits must be pairwise distinct");
static void test_native_duration(const azstatus_native_v1 *api, Slot &slot, uintptr_t core) {
    Buffer<0x20> adapter, tables[3];
    Buffer<0x200> unit;
    Buffer<0x50> basis;
    Buffer<0x160> buffer;
    Buffer<0x1b0> rows;
    const uintptr_t row = rows.addr();
    const uintptr_t prior_unit = slot.unit;
    slot.unit = unit.addr();
    put(unit.addr() + 0x180, basis.addr());
    put(basis.addr() + 0x30, core);
    put(basis.addr() + 0x28, adapter.addr());
    uint64_t gate = 0;
    assert(slot.gate.publish(&slot, admitted, &gate) == AZ_OK);
    duration_adapter_table = tables[0].addr();
    duration_buffer_table = tables[1].addr();
    duration_track_table = tables[2].addr();
    put(adapter.addr(), duration_adapter_table);
    put(adapter.addr() + 8, buffer.addr());
    put(buffer.addr(), duration_buffer_table);
    put(buffer.addr() + 0x140, row);
    put(buffer.addr() + 0x148, row + 0xd8);
    put(row, duration_track_table);
    put(row + 8, buffer.addr() + 8);
    put(core + 0xa8, adapter.addr());
    put(core + 0x20, matched_id);
    put(core + 0x68, uint32_t(48000));
    memcpy(rows.bytes + 0x20, reinterpret_cast<const void *>(core + 0x18), 12);
    duration_original[0] = reinterpret_cast<void *>(+[](uintptr_t owned, int64_t value) { put(owned + 0x30, value); });
    duration_original[1] = reinterpret_cast<void *>(+[](uintptr_t owned, unsigned) -> uintptr_t {
        memset(reinterpret_cast<void *>(owned + 0x20), 0, 12);
        put(owned + 0x30, INT64_MAX);
        return 1;
    });
    duration_original[2] = reinterpret_cast<void *>(+[](uintptr_t owned, uintptr_t) -> int64_t {
        return static_cast<int64_t>(word(word(owned + 0x140) + 0x30));
    });
    duration_original[3] = reinterpret_cast<void *>(+[](uintptr_t owned) {
        memset(reinterpret_cast<void *>(owned + 0x20), 0, 12);
    });
    duration_enabled.store(true);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
    duration_set(row, 480000);
    auto value = take(api);
    assert(value.kind == AZN_DURATION && value.valid && point_at(value.points[0], 480000));
    assert(value.points[0].rate == 48000 && value.unit == fixture_load.unit_generation);
    DurationRead seed;
    seed.buffer = duration_buffer(slot, core); seed.id = core + 0x18;
    duration_read = &seed;
    (void) duration_get_length(buffer.addr(), seed.id);
    duration_read = nullptr;
    assert(seed.observed && compare_timestamps(seed.samples, 480000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    DurationRead foreign; foreign.buffer = buffer.addr() + 8; foreign.id = seed.id;
    duration_read = &foreign;
    (void) duration_get_length(buffer.addr(), seed.id);
    duration_read = nullptr;
    assert(!foreign.observed);
    azstatus_native_receipt none {}; none.size = sizeof none; none.version = 1;
    duration_set(row, 480000);
    assert(!api->take(0, &none));
    duration_set(row, 480001); /* one source sample is distinct for N=1 */
    value = take(api);
    assert(value.kind == AZN_DURATION && point_at(value.points[0], 480001));
    assert(api->capabilities(0) & AZN_CAP_DURATION);
    /* An audio-owner lease can overlap hundreds of page-filled callbacks.
     * The already witnessed row is unchanged: no capture gap or reseed loop. */
    azsdk_native::OwnerGate::Lease occupied;
    assert(slot.gate.enter(occupied) == AZ_OK);
    std::thread same_writer([&] { for (unsigned i = 0; i < 300; ++i) duration_set(row, 480001); });
    same_writer.join();
    assert(!api->take(0, &none) && !duration_state[0].refresh.load());
    /* Actual page-filled repeats can collide with the duration reader's
     * scalar lock while an independent audio frame owns the Unit. Equal
     * samples from the same committed source are not omitted history. */
    assert(!duration_state[0].busy.test_and_set());
    std::thread contended_repeat([&] { duration_set(row, 480001); });
    contended_repeat.join();
    assert(!api->take(0, &none) && !duration_state[0].refresh.load());
    duration_state[0].busy.clear();
    std::thread changed_writer([&] { for (unsigned i = 0; i < 300; ++i) duration_set(row, 480002); });
    changed_writer.join();
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    assert(!api->take(0, &none));
    assert(occupied.end() == AZ_OK);
    duration_commit(0, fixture_load, 480002, 48000, true, duration_mutation.load(), buffer.addr());
    assert(take(api).valid);
    assert(slot.gate.enter(occupied) == AZ_OK);
    std::thread recovered_writer([&] { for (unsigned i = 0; i < 300; ++i) duration_set(row, 480002); });
    recovered_writer.join();
    assert(!api->take(0, &none));
    assert(occupied.end() == AZ_OK);
    /* A seed reader owns no native mutation. Retry a collision without a
     * false gap, then require the unchanged seed to publish the retry. */
    const uint64_t certificate = duration_state[0].value.serial.load();
    assert(certificate && !(certificate & 1));
    assert(!duration_state[0].busy.test_and_set());
    duration_commit(0, fixture_load, 480002, 48000, true, duration_mutation.load(), buffer.addr());
    assert(!api->take(0, &none) && !duration_state[0].refresh.load());
    duration_state[0].busy.clear();
    duration_commit(0, fixture_load, 480002, 48000, true, duration_mutation.load(), buffer.addr());
    value = take(api);
    assert(value.kind == AZN_DURATION && value.valid && point_at(value.points[0], 480002));
    assert(duration_state[0].value.serial.load() == certificate);
    assert(slot.gate.enter(occupied) == AZ_OK);
    assert(!duration_state[0].busy.test_and_set());
    /* A deferred seed must not suppress loss from an actual changed writer. */
    duration_commit(0, fixture_load, 480002, 48000, true, duration_mutation.load(), buffer.addr());
    assert(!api->take(0, &none));
    std::thread simultaneous_writer([&] { duration_set(row, 480003); });
    simultaneous_writer.join();
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    Buffer<0x160> unrelated_buffer;
    Buffer<0xd8> unrelated_row;
    memcpy(unrelated_row.bytes, rows.bytes, 0xd8);
    put(unrelated_buffer.addr(), duration_buffer_table);
    put(unrelated_buffer.addr() + 0x140, unrelated_row.addr());
    put(unrelated_buffer.addr() + 0x148, unrelated_row.addr() + 0xd8);
    put(unrelated_row.addr() + 8, unrelated_buffer.addr() + 8);
    std::thread unrelated_writer([&] { duration_set(unrelated_row.addr(), 1); });
    unrelated_writer.join();
    assert(!api->take(0, &none));
    duration_state[0].busy.clear();
    assert(occupied.end() == AZ_OK);
    duration_commit(0, fixture_load, 480003, 48000, true, duration_mutation.load(), buffer.addr());
    assert(take(api).valid);
    /* Reset is an actual native lifetime mutation, even when its previous
     * scalar equals the certificate. Busy plus Unit contention must lose. */
    assert(slot.gate.enter(occupied) == AZ_OK);
    assert(!duration_state[0].busy.test_and_set());
    std::thread reset_writer([&] { duration_reset(row, 1); });
    reset_writer.join();
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    duration_state[0].busy.clear();
    assert(occupied.end() == AZ_OK);
    memcpy(rows.bytes + 0x20, reinterpret_cast<const void *>(core + 0x18), 12);
    put(row + 0x30, int64_t(480003));
    duration_commit(0, fixture_load, 480003, 48000, true, duration_mutation.load(), buffer.addr());
    assert(take(api).valid);
    /* An unavailable certificate never qualifies an equal-sample repeat. */
    duration_commit(0, fixture_load, 480003, 0, false, duration_mutation.load(), buffer.addr());
    assert(!take(api).valid);
    assert(slot.gate.enter(occupied) == AZ_OK);
    assert(!duration_state[0].busy.test_and_set());
    std::thread unknown_repeat([&] { duration_set(row, 480003); });
    unknown_repeat.join();
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    duration_state[0].busy.clear();
    assert(occupied.end() == AZ_OK);
    duration_commit(0, fixture_load, 480003, 48000, true, duration_mutation.load(), buffer.addr());
    assert(take(api).valid);
    azn_capability(0, AZN_CAP_MEMORY_LINK, 1);
    azn_capability(0, AZN_CAP_DURATION, 0);
    assert((api->capabilities(0) & AZN_CAP_MEMORY_LINK) && !(api->capabilities(0) & AZN_CAP_DURATION));
    azn_capability(0, AZN_CAP_DURATION, 1);
    azn_capability(0, AZN_CAP_MEMORY_LINK, 0);
    assert((api->capabilities(0) & AZN_CAP_DURATION) && !(api->capabilities(0) & AZN_CAP_MEMORY_LINK));
    /* A row with the same identity outside the owned vector is not the reader. */
    memcpy(rows.bytes + 0xd8, rows.bytes, 0xd8);
    duration_set(row + 0xd8, 1000);
    assert(!api->take(0, &none));
    put(row + 0x28, matched_id + 1);
    duration_set(row, 1000);
    assert(!api->take(0, &none));
    put(row + 0x28, matched_id);
    /* Unknown source rates cannot become READY and cannot leave old duration. */
    put(core + 0x68, uint32_t(0));
    duration_set(row, 1000);
    assert(!take(api).valid);
    put(core + 0x68, uint32_t(44100));
    duration_set(row, 441000);
    value = take(api);
    assert(value.valid && value.points[0].rate == 44100 && point_at(value.points[0], 441000));
    duration_reset(row, 1);
    assert(!take(api).valid);
    memcpy(rows.bytes + 0x20, reinterpret_cast<const void *>(core + 0x18), 12);
    duration_set(row, INT64_MAX);
    assert(!api->take(0, &none)); /* native unknown sentinel remains unavailable */
    duration_set(row, 441000);
    assert(take(api).valid);
    /* A capture reservation failure invalidates only duration's routes. */
    duration_state[0].busy.test_and_set();
    duration_set(row, 441001);
    duration_state[0].busy.clear();
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    duration_commit(0, fixture_load, 441001, 44100, true);
    assert(take(api).valid);
    /* A seed cut cannot overtake a setter/reset or cross a load boundary. */
    const uint64_t old = duration_mutation.load();
    duration_set(row, 441002);
    assert(take(api).valid);
    duration_commit(0, fixture_load, 400000, 44100, true, old);
    assert(!api->take(0, &none));
    azsdk_load_sample retired = fixture_load;
    ++fixture_load.load_generation;
    ++fixture_load.invalidation_epoch;
    duration_commit(0, retired, 400000, 44100, true);
    assert(!api->take(0, &none));
    duration_set(row, 441002);
    value = take(api);
    assert(value.valid && value.load_generation == fixture_load.load_generation);
    assert(duration_repeat(duration_state[0], fixture_load, buffer.addr(), 441002, false));
    assert(!duration_repeat(duration_state[0], retired, buffer.addr(), 441002, false));
    const uint64_t before_destroy = duration_mutation.load();
    duration_destroy(row);
    assert(!take(api).valid);
    duration_commit(0, fixture_load, 441002, 44100, true, before_destroy);
    assert(!api->take(0, &none));
    assert(slot.gate.retire(gate) == AZ_OK);
    duration_retire(0);
    assert(!(api->capabilities(0) & AZN_CAP_DURATION));
    assert(!duration_repeat(duration_state[0], fixture_load, buffer.addr(), 441002, false));
    duration_enabled.store(false);
    put(core + 0xa8, uintptr_t(0));
    slot.unit = prior_unit;
    puts("PASS native exact duration writer, source/rate qualification, reset, loss, stale seed and load retirement");
}

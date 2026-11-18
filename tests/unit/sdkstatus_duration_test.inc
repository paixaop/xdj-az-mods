/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the native fixture: the writer owns each row and the gate owns
 * the independent current core. No FILE metadata participates in these cuts. */
#include "azstatus_memory.h"
_Static_assert(AZN_CAP_LOAD != 0 && !(AZN_CAP_LOAD & (AZN_CAP_LOAD - 1)), "single capability bit");
_Static_assert(AZN_CAP_LOOP != 0 && !(AZN_CAP_LOOP & (AZN_CAP_LOOP - 1)), "single capability bit");
_Static_assert(AZN_CAP_LTI != 0 && !(AZN_CAP_LTI & (AZN_CAP_LTI - 1)), "single capability bit");
_Static_assert(AZN_CAP_MOTION != 0 && !(AZN_CAP_MOTION & (AZN_CAP_MOTION - 1)), "single capability bit");
_Static_assert(AZN_CAP_PLAY_RAW != 0 && !(AZN_CAP_PLAY_RAW & (AZN_CAP_PLAY_RAW - 1)), "single capability bit");
_Static_assert(AZN_CAP_STEMS != 0 && !(AZN_CAP_STEMS & (AZN_CAP_STEMS - 1)), "single capability bit");
_Static_assert(AZN_CAP_QUANTIZE != 0 && !(AZN_CAP_QUANTIZE & (AZN_CAP_QUANTIZE - 1)), "single capability bit");
_Static_assert(AZN_CAP_KEY_LOCK != 0 && !(AZN_CAP_KEY_LOCK & (AZN_CAP_KEY_LOCK - 1)), "single capability bit");
_Static_assert(AZN_CAP_SYNC != 0 && !(AZN_CAP_SYNC & (AZN_CAP_SYNC - 1)), "single capability bit");
_Static_assert(AZN_CAP_SLIDER_LOCK != 0 && !(AZN_CAP_SLIDER_LOCK & (AZN_CAP_SLIDER_LOCK - 1)), "single capability bit");
_Static_assert(AZN_CAP_MAPS != 0 && !(AZN_CAP_MAPS & (AZN_CAP_MAPS - 1)), "single capability bit");
_Static_assert(AZN_CAP_HOT != 0 && !(AZN_CAP_HOT & (AZN_CAP_HOT - 1)), "single capability bit");
_Static_assert(AZN_CAP_LOOP_MODE != 0 && !(AZN_CAP_LOOP_MODE & (AZN_CAP_LOOP_MODE - 1)), "single capability bit");
_Static_assert(AZN_CAP_RELOOP != 0 && !(AZN_CAP_RELOOP & (AZN_CAP_RELOOP - 1)), "single capability bit");
_Static_assert(AZN_CAP_HOT_LINK != 0 && !(AZN_CAP_HOT_LINK & (AZN_CAP_HOT_LINK - 1)), "single capability bit");
_Static_assert(AZN_CAP_RATE != 0 && !(AZN_CAP_RATE & (AZN_CAP_RATE - 1)), "single capability bit");
_Static_assert(AZN_CAP_SYNC_ROLE != 0 && !(AZN_CAP_SYNC_ROLE & (AZN_CAP_SYNC_ROLE - 1)), "single capability bit");
_Static_assert(AZN_CAP_TRANSPORT_STATE != 0 && !(AZN_CAP_TRANSPORT_STATE & (AZN_CAP_TRANSPORT_STATE - 1)), "single capability bit");
_Static_assert(AZN_CAP_SLIP_HEAD != 0 && !(AZN_CAP_SLIP_HEAD & (AZN_CAP_SLIP_HEAD - 1)), "single capability bit");
_Static_assert(AZN_CAP_KEY_SHIFT != 0 && !(AZN_CAP_KEY_SHIFT & (AZN_CAP_KEY_SHIFT - 1)), "single capability bit");
_Static_assert(AZN_CAP_END != 0 && !(AZN_CAP_END & (AZN_CAP_END - 1)), "single capability bit");
_Static_assert(AZN_CAP_DURATION != 0 && !(AZN_CAP_DURATION & (AZN_CAP_DURATION - 1)), "single capability bit");
_Static_assert(AZN_CAP_MEMORY != 0 && !(AZN_CAP_MEMORY & (AZN_CAP_MEMORY - 1)), "single capability bit");
_Static_assert(AZN_CAP_MEMORY_LINK != 0 && !(AZN_CAP_MEMORY_LINK & (AZN_CAP_MEMORY_LINK - 1)), "single capability bit");
_Static_assert(AZN_CAP_ORIGINAL_BPM != 0 && !(AZN_CAP_ORIGINAL_BPM & (AZN_CAP_ORIGINAL_BPM - 1)), "single capability bit");
_Static_assert((AZN_CAP_LOAD | AZN_CAP_LOOP | AZN_CAP_LTI | AZN_CAP_MOTION | AZN_CAP_PLAY_RAW | AZN_CAP_STEMS | AZN_CAP_QUANTIZE | AZN_CAP_KEY_LOCK | AZN_CAP_SYNC | AZN_CAP_SLIDER_LOCK | AZN_CAP_MAPS | AZN_CAP_HOT | AZN_CAP_LOOP_MODE | AZN_CAP_RELOOP | AZN_CAP_HOT_LINK | AZN_CAP_RATE | AZN_CAP_SYNC_ROLE | AZN_CAP_TRANSPORT_STATE | AZN_CAP_SLIP_HEAD | AZN_CAP_KEY_SHIFT | AZN_CAP_END | AZN_CAP_DURATION | AZN_CAP_MEMORY | AZN_CAP_MEMORY_LINK | AZN_CAP_ORIGINAL_BPM) == ((uint64_t) AZN_CAP_LOAD + (uint64_t) AZN_CAP_LOOP + (uint64_t) AZN_CAP_LTI + (uint64_t) AZN_CAP_MOTION + (uint64_t) AZN_CAP_PLAY_RAW + (uint64_t) AZN_CAP_STEMS + (uint64_t) AZN_CAP_QUANTIZE + (uint64_t) AZN_CAP_KEY_LOCK + (uint64_t) AZN_CAP_SYNC + (uint64_t) AZN_CAP_SLIDER_LOCK + (uint64_t) AZN_CAP_MAPS + (uint64_t) AZN_CAP_HOT + (uint64_t) AZN_CAP_LOOP_MODE + (uint64_t) AZN_CAP_RELOOP + (uint64_t) AZN_CAP_HOT_LINK + (uint64_t) AZN_CAP_RATE + (uint64_t) AZN_CAP_SYNC_ROLE + (uint64_t) AZN_CAP_TRANSPORT_STATE + (uint64_t) AZN_CAP_SLIP_HEAD + (uint64_t) AZN_CAP_KEY_SHIFT + (uint64_t) AZN_CAP_END + (uint64_t) AZN_CAP_DURATION + (uint64_t) AZN_CAP_MEMORY + (uint64_t) AZN_CAP_MEMORY_LINK + (uint64_t) AZN_CAP_ORIGINAL_BPM), "distinct capability bits");
struct DurationJob {
    uintptr_t row;
    int64_t samples;
    unsigned repeats;
    bool reset;
};
static void *duration_worker(void *opaque) {
    struct DurationJob *job = opaque;
    for (unsigned i = 0; i < job->repeats; ++i) {
        if (job->reset) duration_reset(job->row, (unsigned) job->samples);
        else duration_set(job->row, job->samples);
    }
    return NULL;
}
static void native_callback_21(uintptr_t owned) {
    memset(((void *) (owned + 0x20)), 0, 12);
}
static int64_t native_callback_22(uintptr_t owned, uintptr_t unused_1) {
    (void) unused_1;
    return ((int64_t) (word(word(owned + 0x140) + 0x30)));
}
static uintptr_t native_callback_23(uintptr_t owned, unsigned unused_1) {
    (void) unused_1;
    memset(((void *) (owned + 0x20)), 0, 12);
    put(owned + 0x30, INT64_MAX);
    return 1;
}
static void native_callback_24(uintptr_t owned, int64_t value) { put(owned + 0x30, value); }
static void test_native_duration(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t core) {
    Buffer_20 adapter = {0}, tables[3] = {0};
    Buffer_200 unit = {0};
    Buffer_50 basis = {0};
    Buffer_160 buffer = {0};
    Buffer_1b0 rows = {0};
    const uintptr_t row = ((uintptr_t) rows.bytes);
    const uintptr_t prior_unit = slot->unit;
    slot->unit = ((uintptr_t) unit.bytes);
    put(((uintptr_t) unit.bytes) + 0x180, ((uintptr_t) basis.bytes));
    put(((uintptr_t) basis.bytes) + 0x30, core);
    put(((uintptr_t) basis.bytes) + 0x28, ((uintptr_t) adapter.bytes));
    uint64_t gate = 0;
    assert(az_owner_publish(&slot->gate, slot, admitted, &gate) == AZ_OK);
    duration_adapter_table = ((uintptr_t) tables[0].bytes);
    duration_buffer_table = ((uintptr_t) tables[1].bytes);
    duration_track_table = ((uintptr_t) tables[2].bytes);
    put(((uintptr_t) adapter.bytes), duration_adapter_table);
    put(((uintptr_t) adapter.bytes) + 8, ((uintptr_t) buffer.bytes));
    put(((uintptr_t) buffer.bytes), duration_buffer_table);
    put(((uintptr_t) buffer.bytes) + 0x140, row);
    put(((uintptr_t) buffer.bytes) + 0x148, row + 0xd8);
    put(row, duration_track_table);
    put(row + 8, ((uintptr_t) buffer.bytes) + 8);
    put(core + 0xa8, ((uintptr_t) adapter.bytes));
    put(core + 0x20, matched_id);
    put(core + 0x68, (uint32_t) (48000));
    memcpy(rows.bytes + 0x20, ((const void *) (core + 0x18)), 12);
    duration_original[0] = ((void *) (native_callback_24));
    duration_original[1] = ((void *) (native_callback_23));
    duration_original[2] = ((void *) (native_callback_22));
    duration_original[3] = ((void *) (native_callback_21));
    atomic_store_explicit(&(duration_enabled), true, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
    duration_set(row, 480000);
    __auto_type value = take(api);
    assert(value.kind == AZN_DURATION && value.valid && point_at(value.points[0], 480000, 1));
    assert(value.points[0].rate == 48000 && value.unit == fixture_load.unit_generation);
    DurationRead seed = {0};
    seed.buffer = duration_buffer(slot, core);
    seed.id = core + 0x18;
    duration_read = &seed;
    (void) duration_get_length(((uintptr_t) buffer.bytes), seed.id);
    duration_read = NULL;
    assert(seed.observed && compare_timestamps(seed.samples, 480000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    DurationRead foreign = {0};
    foreign.buffer = ((uintptr_t) buffer.bytes) + 8;
    foreign.id = seed.id;
    duration_read = &foreign;
    (void) duration_get_length(((uintptr_t) buffer.bytes), seed.id);
    duration_read = NULL;
    assert(!foreign.observed);
    azstatus_native_receipt none = {0};
    none.size = sizeof none;
    none.version = 1;
    duration_set(row, 480000);
    assert(!api->take(0, &none));
    duration_set(row, 480001); /* one source sample is distinct for N=1 */
    value = take(api);
    assert(value.kind == AZN_DURATION && point_at(value.points[0], 480001, 1));
    assert(api->capabilities(0) & AZN_CAP_DURATION);
    /* An audio-owner lease can overlap hundreds of page-filled callbacks.
     * The already witnessed row is unchanged: no capture gap or reseed loop. */
    AZ_OWNER_LEASE(occupied);
    assert(az_owner_enter(&slot->gate, &occupied) == AZ_OK);
    struct DurationJob same_writer_job = {row, 480001, 300, false};
    pthread_t same_writer;
    assert(!pthread_create(&same_writer, NULL, duration_worker, &same_writer_job));
    assert(!pthread_join(same_writer, NULL));
    assert(!api->take(0, &none) && !atomic_load_explicit(&(duration_state[0].refresh), memory_order_seq_cst));
    /* Actual page-filled repeats can collide with the duration reader's
     * scalar lock while an independent audio frame owns the Unit. Equal
     * samples from the same committed source are not omitted history. */
    assert(!atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst));
    struct DurationJob contended_repeat_job = {row, 480001, 1, false};
    pthread_t contended_repeat;
    assert(!pthread_create(&contended_repeat, NULL, duration_worker, &contended_repeat_job));
    assert(!pthread_join(contended_repeat, NULL));
    assert(!api->take(0, &none) && !atomic_load_explicit(&(duration_state[0].refresh), memory_order_seq_cst));
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    struct DurationJob changed_writer_job = {row, 480002, 300, false};
    pthread_t changed_writer;
    assert(!pthread_create(&changed_writer, NULL, duration_worker, &changed_writer_job));
    assert(!pthread_join(changed_writer, NULL));
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    assert(!api->take(0, &none));
    assert(az_owner_lease_end(&occupied) == AZ_OK);
    duration_commit(0, fixture_load, 480002, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(take(api).valid);
    assert(az_owner_enter(&slot->gate, &occupied) == AZ_OK);
    struct DurationJob recovered_writer_job = {row, 480002, 300, false};
    pthread_t recovered_writer;
    assert(!pthread_create(&recovered_writer, NULL, duration_worker, &recovered_writer_job));
    assert(!pthread_join(recovered_writer, NULL));
    assert(!api->take(0, &none));
    assert(az_owner_lease_end(&occupied) == AZ_OK);
    /* A seed reader owns no native mutation. Retry a collision without a
     * false gap, then require the unchanged seed to publish the retry. */
    const uint64_t certificate = atomic_load_explicit(&(duration_state[0].value.serial), memory_order_seq_cst);
    assert(certificate && !(certificate & 1));
    assert(!atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst));
    duration_commit(0, fixture_load, 480002, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(!api->take(0, &none) && !atomic_load_explicit(&(duration_state[0].refresh), memory_order_seq_cst));
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    duration_commit(0, fixture_load, 480002, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    value = take(api);
    assert(value.kind == AZN_DURATION && value.valid && point_at(value.points[0], 480002, 1));
    assert(atomic_load_explicit(&(duration_state[0].value.serial), memory_order_seq_cst) == certificate);
    assert(az_owner_enter(&slot->gate, &occupied) == AZ_OK);
    assert(!atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst));
    /* A deferred seed must not suppress loss from an actual changed writer. */
    duration_commit(0, fixture_load, 480002, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(!api->take(0, &none));
    struct DurationJob simultaneous_writer_job = {row, 480003, 1, false};
    pthread_t simultaneous_writer;
    assert(!pthread_create(&simultaneous_writer, NULL, duration_worker, &simultaneous_writer_job));
    assert(!pthread_join(simultaneous_writer, NULL));
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    Buffer_160 unrelated_buffer = {0};
    Buffer_d8 unrelated_row = {0};
    memcpy(unrelated_row.bytes, rows.bytes, 0xd8);
    put(((uintptr_t) unrelated_buffer.bytes), duration_buffer_table);
    put(((uintptr_t) unrelated_buffer.bytes) + 0x140, ((uintptr_t) unrelated_row.bytes));
    put(((uintptr_t) unrelated_buffer.bytes) + 0x148, ((uintptr_t) unrelated_row.bytes) + 0xd8);
    put(((uintptr_t) unrelated_row.bytes) + 8, ((uintptr_t) unrelated_buffer.bytes) + 8);
    struct DurationJob unrelated_writer_job = {((uintptr_t) unrelated_row.bytes), 1, 1, false};
    pthread_t unrelated_writer;
    assert(!pthread_create(&unrelated_writer, NULL, duration_worker, &unrelated_writer_job));
    assert(!pthread_join(unrelated_writer, NULL));
    assert(!api->take(0, &none));
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    assert(az_owner_lease_end(&occupied) == AZ_OK);
    duration_commit(0, fixture_load, 480003, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(take(api).valid);
    /* Reset is an actual native lifetime mutation, even when its previous
     * scalar equals the certificate. Busy plus Unit contention must lose. */
    assert(az_owner_enter(&slot->gate, &occupied) == AZ_OK);
    assert(!atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst));
    struct DurationJob reset_writer_job = {row, 1, 1, true};
    pthread_t reset_writer;
    assert(!pthread_create(&reset_writer, NULL, duration_worker, &reset_writer_job));
    assert(!pthread_join(reset_writer, NULL));
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    assert(az_owner_lease_end(&occupied) == AZ_OK);
    memcpy(rows.bytes + 0x20, ((const void *) (core + 0x18)), 12);
    put(row + 0x30, (int64_t) (480003));
    duration_commit(0, fixture_load, 480003, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(take(api).valid);
    /* An unavailable certificate never qualifies an equal-sample repeat. */
    duration_commit(0, fixture_load, 480003, 0, false, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
    assert(!take(api).valid);
    assert(az_owner_enter(&slot->gate, &occupied) == AZ_OK);
    assert(!atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst));
    struct DurationJob unknown_repeat_job = {row, 480003, 1, false};
    pthread_t unknown_repeat;
    assert(!pthread_create(&unknown_repeat, NULL, duration_worker, &unknown_repeat_job));
    assert(!pthread_join(unknown_repeat, NULL));
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    assert(az_owner_lease_end(&occupied) == AZ_OK);
    duration_commit(0, fixture_load, 480003, 48000, true, atomic_load_explicit(&(duration_mutation), memory_order_seq_cst), ((uintptr_t) buffer.bytes));
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
    put(core + 0x68, (uint32_t) (0));
    duration_set(row, 1000);
    assert(!take(api).valid);
    put(core + 0x68, (uint32_t) (44100));
    duration_set(row, 441000);
    value = take(api);
    assert(value.valid && value.points[0].rate == 44100 && point_at(value.points[0], 441000, 1));
    duration_reset(row, 1);
    assert(!take(api).valid);
    memcpy(rows.bytes + 0x20, ((const void *) (core + 0x18)), 12);
    duration_set(row, INT64_MAX);
    assert(!api->take(0, &none)); /* native unknown sentinel remains unavailable */
    duration_set(row, 441000);
    assert(take(api).valid);
    /* A capture reservation failure invalidates only duration's routes. */
    atomic_flag_test_and_set_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    duration_set(row, 441001);
    atomic_flag_clear_explicit(&(duration_state[0].busy), memory_order_seq_cst);
    value = take(api);
    assert(value.kind == AZN_GAP && value.b == AZN_DURATION && value.lost_topics == duration_routes);
    duration_commit(0, fixture_load, 441001, 44100, true, UINT64_MAX, 0);
    assert(take(api).valid);
    /* A seed cut cannot overtake a setter/reset or cross a load boundary. */
    const uint64_t old = atomic_load_explicit(&(duration_mutation), memory_order_seq_cst);
    duration_set(row, 441002);
    assert(take(api).valid);
    duration_commit(0, fixture_load, 400000, 44100, true, old, 0);
    assert(!api->take(0, &none));
    azsdk_load_sample retired = fixture_load;
    ++fixture_load.load_generation;
    ++fixture_load.invalidation_epoch;
    duration_commit(0, retired, 400000, 44100, true, UINT64_MAX, 0);
    assert(!api->take(0, &none));
    duration_set(row, 441002);
    value = take(api);
    assert(value.valid && value.load_generation == fixture_load.load_generation);
    assert(duration_repeat(&(duration_state[0]), fixture_load, ((uintptr_t) buffer.bytes), 441002, false));
    assert(!duration_repeat(&(duration_state[0]), retired, ((uintptr_t) buffer.bytes), 441002, false));
    const uint64_t before_destroy = atomic_load_explicit(&(duration_mutation), memory_order_seq_cst);
    duration_destroy(row);
    assert(!take(api).valid);
    duration_commit(0, fixture_load, 441002, 44100, true, before_destroy, 0);
    assert(!api->take(0, &none));
    assert(az_owner_retire(&slot->gate, gate) == AZ_OK);
    duration_retire(0);
    assert(!(api->capabilities(0) & AZN_CAP_DURATION));
    assert(!duration_repeat(&(duration_state[0]), fixture_load, ((uintptr_t) buffer.bytes), 441002, false));
    atomic_store_explicit(&(duration_enabled), false, memory_order_seq_cst);
    put(core + 0xa8, (uintptr_t) (0));
    slot->unit = prior_unit;
    puts("PASS native exact duration writer, source/rate qualification, reset, loss, stale seed and load retirement");
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the same owner capture, native writer trampolines and receipt lane used on EP147. */
#include "../../src/azstem/sdk_exit.c"
#include <assert.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include "native_abi_fixture.h"
static _Atomic(bool) pause_next = false, parked = false, resume_writer = false;
void azn_test_reserved(unsigned unused_0, unsigned kind) {
    (void) unused_0;
    if (kind == AZN_LOOP && atomic_exchange_explicit(&(pause_next), false, memory_order_seq_cst)) {
        atomic_store_explicit(&(parked), true, memory_order_seq_cst);
        while (!atomic_load_explicit(&(resume_writer), memory_order_seq_cst)) sched_yield();
    }
}
const struct azstatus_native_v1 *azstem_status_service_v1();
static bool inject_scan = false;
void azn_test_scan(unsigned deck, unsigned lane) {
    if (deck != 3 || lane != 4 || !inject_scan) return;
    inject_scan = false;
    azstatus_native_receipt motion = {0}, end = {0};
    motion.deck = end.deck = deck;
    motion.kind = AZN_MOTION;
    motion.motion_order = 2;
    end.kind = AZN_END;
    end.motion_order = 3;
    assert(azn_offer(&motion) && azn_offer(&end));
}
static void test_concurrent_scan() {
    __auto_type api = azstem_status_service_v1();
    azstatus_native_receipt receipt = {0};
    receipt.size = sizeof receipt;
    receipt.version = 1;
    inject_scan = true;
    assert(api->take(3, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == 2);
    assert(api->take(3, &receipt) && receipt.kind == AZN_END && receipt.motion_order == 3);
    assert(!api->take(3, &receipt));
}
const struct azn_analysis_source_api *azn_analysis_sources(void) { return NULL; }
int azn_register_source_capture(azn_source_capture_fn unused_0) {
    (void) unused_0;
    return 0;
}
int azn_sync_seed(unsigned unused_0, uintptr_t unused_1, int *raw) {
    (void) unused_0;
    (void) unused_1;
    *raw = 0;
    return 1;
}
unsigned azn_maps_install(int unused_0, uintptr_t unused_1, azn_setting_patch_fn unused_2) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    return 0;
}
int azn_maps_current(unsigned unused_0, uint64_t unused_1, const aznm_packet *unused_2) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    return 1;
}
uint64_t azn_load_source_pin(unsigned unused_0, uint64_t unused_1, uint64_t unused_2) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    return 0;
}
unsigned azn_load_kind(unsigned unused_0, uint64_t unused_1, uint64_t unused_2) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    return 0;
}
unsigned azn_load_original_bpm(unsigned unused_0, uint64_t unused_1, uint64_t unused_2, uint64_t unused_3, double *unused_4) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    (void) unused_4;
    return AZN_BPM_UNKNOWN;
}
int azn_empty_seed(unsigned unused_0, azstatus_native_receipt *unused_1) {
    (void) unused_0;
    (void) unused_1;
    return 0;
}
int azn_metadata_seed(unsigned unused_0, azstatus_native_receipt *unused_1) {
    (void) unused_0;
    (void) unused_1;
    return 0;
}
static azsdk_load_sample fixture_load = (__typeof__(fixture_load)) {5, 7, 11, 3, 2, 0};
static uint32_t matched_id = 3;
static bool fixture_load_busy = false;
static void (*fixture_track_match_hook)() = NULL;
static void (*fixture_after_track_match_hook)() = NULL;
static uint64_t fixture_pending_request = 0;
static uint32_t fixture_pending_id = 0;
uint64_t azn_load_pending(unsigned d, uint64_t e, const unsigned char source[12]) {
    if (d || e != fixture_load.invalidation_epoch || fixture_load.load_generation) return 0;
    if (source) {
        uint32_t id;
        memcpy(&id, source + 8, sizeof id);
        if (id != fixture_pending_id) return 0;
    }
    return fixture_pending_request;
}
uint64_t azn_lifecycle_fence(unsigned d) { return !d ? fixture_load.invalidation_epoch : 0; }
int azn_load_history_revoked(unsigned d, uint64_t epoch) {
    return d == 0 && epoch && fixture_load.invalidation_epoch > epoch && !fixture_load.load_generation;
}
int azn_load_track_matches(unsigned d, uint64_t e, uint64_t load, const unsigned char id[12]) {
    if (fixture_track_match_hook) {
        __auto_type hook = fixture_track_match_hook;
        fixture_track_match_hook = NULL;
        hook();
    }
    uint32_t n;
    memcpy(&n, id + 8, 4);
    const bool matched = !d && e == fixture_load.invalidation_epoch && load == fixture_load.load_generation && n == matched_id;
    if (matched && fixture_after_track_match_hook) {
        __auto_type hook = fixture_after_track_match_hook;
        fixture_after_track_match_hook = NULL;
        hook();
    }
    return matched;
}
int azsdk_load_read(int deck, azsdk_load_sample *out) {
    if (deck || !fixture_load.load_generation || fixture_load_busy) return 0;
    *out = fixture_load;
    return 1;
}

typedef struct {
    _Alignas(16) unsigned char bytes[0x20];
} Buffer_20;
typedef struct {
    _Alignas(16) unsigned char bytes[64];
} Buffer_40;
typedef struct {
    _Alignas(16) unsigned char bytes[0x50];
} Buffer_50;
typedef struct {
    _Alignas(16) unsigned char bytes[0x60];
} Buffer_60;
typedef struct {
    _Alignas(16) unsigned char bytes[0x80];
} Buffer_80;
typedef struct {
    _Alignas(16) unsigned char bytes[0xc0];
} Buffer_c0;
typedef struct {
    _Alignas(16) unsigned char bytes[0xd0];
} Buffer_d0;
typedef struct {
    _Alignas(16) unsigned char bytes[0xd8];
} Buffer_d8;
typedef struct {
    _Alignas(16) unsigned char bytes[0x100];
} Buffer_100;
typedef struct {
    _Alignas(16) unsigned char bytes[0x120];
} Buffer_120;
typedef struct {
    _Alignas(16) unsigned char bytes[0x160];
} Buffer_160;
typedef struct {
    _Alignas(16) unsigned char bytes[0x1b0];
} Buffer_1b0;
typedef struct {
    _Alignas(16) unsigned char bytes[0x200];
} Buffer_200;
typedef struct {
    _Alignas(16) unsigned char bytes[0x208];
} Buffer_208;
typedef struct {
    _Alignas(16) unsigned char bytes[0x210];
} Buffer_210;
typedef struct {
    _Alignas(16) unsigned char bytes[0x220];
} Buffer_220;
typedef struct {
    _Alignas(16) unsigned char bytes[0x300];
} Buffer_300;
typedef struct {
    _Alignas(16) unsigned char bytes[0x500];
} Buffer_500;
typedef struct {
    _Alignas(16) unsigned char bytes[0x1000];
} Buffer_1000;
#define put(address, value)                                       \
    do {                                                          \
        __auto_type put_value = (value);                          \
        memcpy((void *) (address), &put_value, sizeof put_value); \
    } while (0)
static void fill_samples(double *first, double *last, double value) {
    while (first != last) *first++ = value;
}
static bool point_at(const azn_point point, int64_t expected, unsigned window) {
    const int order = compare_timestamps(point.samples, expected, point.rate, point.rate, window);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static void test_motion_pressure() {
    __auto_type api = azstem_status_service_v1();
    const unsigned rate = 44100, window = 64;
    const unsigned frames = 1162; // observed 1.30 tail backlog, about 1.69 source seconds
    __auto_type routes = AZ_STATUS_TOPIC(AZ_STATUS_POSITION) |
                         AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    api->interest(0, routes);


#define fixture_offer_1(i_argument) ({                                                     \
    unsigned fixture_offer_1_i = (i_argument);                                             \
    azstatus_native_receipt r = {0};                                                       \
    r.kind = AZN_MOTION;                                                                   \
    r.a = AZN_MEASURE;                                                                     \
    r.motion_order = fixture_offer_1_i;                                                    \
    r.motion_generation = 1;                                                               \
    r.observed_window_rate = rate;                                                         \
    r.observed_window_samples = window;                                                    \
    r.points[1] = (__typeof__(r.points[1])) {AZN_COORD, rate, fixture_offer_1_i * window}; \
    azn_offer(&r);                                                                         \
})


    unsigned accepted = 0;
    for (unsigned i = 1; i <= frames; ++i) accepted += fixture_offer_1(i);
    assert(accepted && accepted < frames);
    azstatus_native_receipt receipt = {0};
    receipt.size = sizeof receipt;
    receipt.version = 1;
    for (unsigned i = 1; i <= accepted; ++i) {
        assert(api->take(0, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == i);
        assert(point_at(receipt.points[1], i * window, 1));
    }
    assert(api->take(0, &receipt) && receipt.kind == AZN_GAP && receipt.b == AZN_MOTION &&
           receipt.lost_topics == routes);
    assert(!api->take(0, &receipt));
    assert(fixture_offer_1(frames + 1));
    assert(api->take(0, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == frames + 1);
    printf("PASS motion queue retains %u measured blocks, reports scoped loss under pressure and admits recovery\n", accepted);

#undef fixture_offer_1
}
static void point(uintptr_t p, int64_t samples) {
    memset(((void *) (p)), 0, 64);
    if (samples < 0) {
        put(p + 16, INT64_MAX);
        put(p + 48, INT32_MAX);
        put(p + 56, INT64_MAX);
        return;
    }
    put(p, (uint8_t) (1));
    put(p + 4, (uint8_t) (1));
    put(p + 6, (uint8_t) (1));
    put(p + 8, (uint32_t) (3));
    put(p + 16, samples);
    put(p + 32, (uintptr_t) (0x10000));
    put(p + 48, (uint32_t) (48000));
    put(p + 56, (int64_t) (samples * 1000000 / 48000));
}
static void copy(void *self, uintptr_t in, uintptr_t out) {
    uintptr_t p = ((uintptr_t) (self));
    point(p + 0x30, (int64_t) (in));
    point(p + 0x88, (int64_t) (out));
    put(p + 8, (uint32_t) (integer(p + 8)) + 1u);
}
static azstatus_native_receipt take(const struct azstatus_native_v1 *api) {
    azstatus_native_receipt r = {0};
    r.size = sizeof r;
    r.version = 1;
    assert(api->take(0, &r) == 1);
    return r;
}
static uintptr_t noq_core, noq_target, noq_return;
static bool fake_noqua(void *unused_0, int unused_1, uint32_t unused_2, const void *unused_3) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    STATUS_ACTION(direct, 0, 0, true);
    status_noqua_request(&(direct), ((const void *) (noq_target)), 2, noq_return);
    StatusMotionWrite move __attribute__((cleanup(status_motion_write_drop)));
    status_motion_write_init(&move, ((void *) (noq_core)), true, 1);
    put(noq_core + 0x28, word(noq_target + 0x18));
    status_motion_write_finish(&move, noq_target);
    return true;
}
static bool fake_grid_jump(void *self, int direction, uint32_t beat, const void *ref, bool option) {
    assert(option);
    return fake_noqua(self, direction, beat, ref);
}
static void native_callback_27(void *self, unsigned on, unsigned unused_2) {
    (void) unused_2;
    put(((uintptr_t) (self)) + 8, ((uint8_t) (on)));
    put(((uintptr_t) (self)) + 24, UINT64_C(0x3ff0000000000000));
}
static void native_callback_28(void *self, int32_t step, unsigned unused_2) {
    (void) unused_2;
    if (step == -1 || step == 1)
        put(((uintptr_t) (self)) + 8, step == -1 ? UINT64_C(0x3fee3437e7101347) : UINT64_C(0x3ff0f38f92d97963));
}
static void key_fixture(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t key) {
    const uint64_t zero = UINT64_C(0x3ff0000000000000), up = UINT64_C(0x3ff0f38f92d97963),
                   down = UINT64_C(0x3fee3437e7101347);
    assert(key_semitones(zero) == 0 && key_semitones(up) == 1 && key_semitones(down) == -1);
    assert(key_semitones(0x4000000000000000) == 12 && key_semitones(0x3fe0000000000000) == -12);
    assert(key_semitones(0) == INT32_MAX && key_semitones(UINT64_MAX) == INT32_MAX);
    atomic_store_explicit(&(key_hooked), 1, memory_order_seq_cst);
    put(key + 0x20, zero);
    key_original = ((void *) (native_callback_28));
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_KEY));
    key_frame(0, &((*slot)));
    __auto_type r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.valid && r.a == 0 && r.attempt && r.load_generation == fixture_load.load_generation);
    void *self = ((void *) (key + 0x18));
    key_writer(self, 1, 0);
    assert(take(api).a == 1);
    key_writer(self, 13, 0);
    key_writer(self, 1, 0);
    azstatus_native_receipt none = {0};
    none.size = sizeof none;
    none.version = 1;
    assert(!api->take(0, &none));
    key_writer(self, -1, 0);
    assert(take(api).a == -1);
    setting_original[1] = ((void *) (native_callback_27));
    setting_writer_1(((void *) (key + 8)), 0u, 0u);
    __auto_type reset = take(api);
    assert(reset.kind == AZN_KEY_SHIFT && reset.a == 0);
    assert(take(api).kind == AZN_KEY_LOCK);
    {
        KeyCapture pending __attribute__((cleanup(key_capture_drop)));
        key_capture_init(&pending, key + 0x18);
        ++fixture_load.load_generation;
        key_capture_finish(&pending);
    }
    assert(!api->take(0, &none));
    key_frame(0, &((*slot)));
    r = take(api);
    assert(r.valid && !r.a && r.load_generation == fixture_load.load_generation);
    {
        KeyCapture concurrent __attribute__((cleanup(key_capture_drop)));
        key_capture_init(&concurrent, key + 0x18);
        key_writer(self, 1, 0);
        key_capture_finish(&concurrent);
    }
    r = take(api);
    assert(r.kind == AZN_GAP && r.b == AZN_KEY_SHIFT && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_KEY));
    key_frame(0, &((*slot)));
    r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.a == 1);
    key_retire(0);
    r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.valid == -1);
    atomic_store_explicit(&(key_hooked), 0, memory_order_seq_cst);
}
static void native_callback_25(void *unused_0, int64_t unused_1) {
    (void) unused_0;
    (void) unused_1;
    azstatus_native_receipt barrier = {0};
    barrier.kind = AZN_MOTION;
    barrier.load_generation = fixture_load.load_generation;
    barrier.motion_generation = 2;
    assert(status_offer_motion(&(state().decks[0].motion), &(barrier)));
}
static void native_callback_26(void *self, int64_t frames) {
    __auto_type p = ((uintptr_t) (self));
    put(p + 0x48, word(p + 0x48) + (uint64_t) frames);
}
static void slip_fixture(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t functions) {
    Buffer_120 slip = {0}, table_buffer = {0};
    state().profile.slip = ((uintptr_t) table_buffer.bytes);
    put(((uintptr_t) slip.bytes), ((uintptr_t) table_buffer.bytes));
    put(functions + 0x30, ((uintptr_t) slip.bytes));
    put(slot->facade + 0xa8, ((uintptr_t) slip.bytes));
    __auto_type core = atomic_load_explicit(&(slot->core), memory_order_seq_cst);
    unsigned char id[12] = {1, 0, 0, 0, 2, 1, 1, 0};
    memcpy(id + 8, &matched_id, 4);
    memcpy(((void *) (core + 0x18)), id, sizeof id);
    put(core + 0x28, (uint64_t) (12000));
    put(core + 0x68, (uint32_t) (48000));
    memcpy(slip.bytes + 0x38, id, sizeof id);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    atomic_store_explicit(&(slip_installed), 1, memory_order_seq_cst);
    slip_original = ((void *) (native_callback_26));
    azstatus_native_receipt seed = {0};
    seed.kind = AZN_MOTION;
    seed.load_generation = fixture_load.load_generation;
    seed.motion_generation = 1;
    assert(status_offer_motion(&(slot->motion), &(seed)) && take(api).kind == AZN_MOTION);
    slip_frame(0, &((*slot)));
    __auto_type r = take(api);
    assert(r.kind == AZN_SLIP_HEAD && r.valid == AZ_AVAIL_EMPTY && r.b && r.attempt && r.motion_generation == 1);
    uint64_t owner = r.attempt;
    slip.bytes[0x68] = 1;
    put(((uintptr_t) slip.bytes) + 0x48, (uint64_t) (0));
    slip_frame(0, &((*slot)));
    r = take(api);
    assert(r.valid == AZ_AVAIL_READY && r.b && r.attempt == owner && point_at(r.points[0], 0, 1) && r.points[0].rate == 48000);
    slip_writer(slip.bytes, 24000);
    r = take(api);
    const int audible = compare_timestamps(word(core + 0x28), 12000, 48000, 48000, 1);
    assert(r.valid == AZ_AVAIL_READY && !r.b && r.motion_generation == 1 && point_at(r.points[0], 24000, 1) &&
           audible != AZ_TIME_INVALID && audible == AZ_TIME_EQUAL);
    __auto_type saved_original = slip_original;
    slip_original = ((void *) (native_callback_25));
    slip_writer(slip.bytes, 1);
    assert(take(api).kind == AZN_MOTION);
    r.size = sizeof r;
    assert(!api->take(0, &r)); // A callback begun before the barrier cannot relabel its sample.
    slip_original = saved_original;
    slip_frame(0, &((*slot)));
    r = take(api);
    assert(r.kind == AZN_SLIP_HEAD && r.valid == AZ_AVAIL_READY && r.b &&
           r.motion_generation == 2 && point_at(r.points[0], 24000, 1));
    put(((uintptr_t) slip.bytes) + 0x40, matched_id + 1);
    slip_writer(slip.bytes, 1);
    r = take(api);
    assert(r.valid == AZ_AVAIL_STALE && r.b);
    slip.bytes[0x68] = 0;
    slip_frame(0, &((*slot)));
    r = take(api);
    assert(r.valid == AZ_AVAIL_EMPTY && r.b);
    slip_retire(0);
    r = take(api);
    assert(r.c && r.valid == AZ_AVAIL_STALE && r.attempt == owner);
    atomic_store_explicit(&(slip_installed), 0, memory_order_seq_cst);
}
static void test_sync_role_values() {
    const SyncProfile *p = &(sync_profiles[1]);
    Buffer_210 parent = {0};
    Buffer_c0 slave = {0};
    Buffer_20 master = {0}, server = {0};
    Buffer_60 job = {0};
    put(((uintptr_t) parent.bytes) + 0xb0, ((uintptr_t) server.bytes));
    put(((uintptr_t) server.bytes), p->server);
    put(((uintptr_t) parent.bytes) + 0xbc, (uint32_t) (1));
    put(((uintptr_t) parent.bytes) + 0xc0, (uint32_t) (2));
    put(((uintptr_t) parent.bytes) + 0x1c8, ((uintptr_t) master.bytes));
    put(((uintptr_t) master.bytes), p->master);
    put(((uintptr_t) master.bytes) + 8, ((uintptr_t) parent.bytes));
    put(((uintptr_t) parent.bytes) + 0x1d0, ((uintptr_t) slave.bytes));
    put(((uintptr_t) parent.bytes) + 0x1c0, ((uintptr_t) slave.bytes));
    put(((uintptr_t) slave.bytes), p->slave);
    put(((uintptr_t) slave.bytes) + 8, ((uintptr_t) parent.bytes));
    put(((uintptr_t) slave.bytes) + 0xb0, ((uintptr_t) job.bytes));
    put(((uintptr_t) job.bytes), p->job);
    put(((uintptr_t) job.bytes) + 0x30, ((uintptr_t) slave.bytes));
    put(((uintptr_t) parent.bytes) + 0x118, (uint8_t) (1));
    put(((uintptr_t) parent.bytes) + 0x119, (uint8_t) (1));
    put(((uintptr_t) parent.bytes) + 0x120, (double) (126.5));
    put(((uintptr_t) parent.bytes) + 0x158, (uint32_t) (2));
    put(((uintptr_t) slave.bytes) + 0x10, (uint32_t) (1));
    put(((uintptr_t) slave.bytes) + 0x48, (uint32_t) (2));


#define fixture_expected_2(follow_argument, locked_argument)                                       \
    do {                                                                                           \
        int fixture_expected_2_follow = (follow_argument);                                         \
        int fixture_expected_2_locked = (locked_argument);                                         \
        SyncValues v = sync_values(((uintptr_t) parent.bytes), (*p), 0);                           \
        assert(v.following == fixture_expected_2_follow && v.locked == fixture_expected_2_locked); \
    } while (0)


    fixture_expected_2(0, 0); /* off even with a leftover job */
    put(((uintptr_t) parent.bytes) + 0xb8, (uint8_t) (1));
    fixture_expected_2(1, 1);
    put(((uintptr_t) slave.bytes) + 0x48, (uint32_t) (1));
    fixture_expected_2(1, 0); /* phase acquisition */
    put(((uintptr_t) slave.bytes) + 0x48, (uint32_t) (2));
    put(((uintptr_t) parent.bytes) + 0x158, (uint32_t) (1));
    fixture_expected_2(1, 0);
    put(((uintptr_t) parent.bytes) + 0x158, (uint32_t) (2));
    put(((uintptr_t) parent.bytes) + 0x154, (uint8_t) (3));
    fixture_expected_2(1, 0);
    put(((uintptr_t) parent.bytes) + 0x154, (uint8_t) (0));
    put(((uintptr_t) slave.bytes) + 0x48, (uint32_t) (7));
    fixture_expected_2(1, -1);
    put(((uintptr_t) parent.bytes) + 0xc0, (uint32_t) (0xff));
    fixture_expected_2(0, 0);
    put(((uintptr_t) parent.bytes) + 0xc0, (uint32_t) (2));
    put(((uintptr_t) parent.bytes) + 0xbc, (uint32_t) (0));
    fixture_expected_2(0, 0);
    put(((uintptr_t) parent.bytes) + 0xbc, (uint32_t) (1));
    put(((uintptr_t) job.bytes) + 0x30, ((uintptr_t) master.bytes));
    fixture_expected_2(-1, -1);
    puts("PASS elected sync target, independent tempo follow/phase lock, acquiring, stale beat and owner mismatch");

#undef fixture_expected_2
}

#include "sdkstatus_duration_test.inc"


#include "sdkstatus_stationary_test.inc"

#ifdef AZSDK_MAP_ALIAS_TESTING

#include "sdkstatus_map_alias_test.inc"

#endif

#include "sdkstatus_initial_loop_test.inc"


#include "sdkstatus_direct_loop_test.inc"


#include "sdkstatus_transport_retirement_test.inc"


static void native_callback_18(void) {
    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
}
static void native_callback_19(void) {
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
}
static void test_hot_retirement(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t context) {
    Buffer_100 properties[8] = {0};
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    for (unsigned i = 0; i < 8; ++i) {
        const uintptr_t source = ((uintptr_t) properties[i].bytes);
        put(context + 0x20 + i * 8, source);
        put(source, state().profile.cue_base_table);
        point(source + 0x30, -1);
        point(source + 0x88, -1);
    }
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    __auto_type prior = take(api);
    assert(prior.kind == AZN_HOT_CUES && prior.valid && prior.attempt && !prior.lost_topics);
    fixture_track_match_hook = native_callback_19;
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    azstatus_native_receipt empty = {0};
    empty.size = sizeof empty;
    empty.version = 1;
    assert(!api->take(0, &empty));
    fixture_load.load_generation = prior.load_generation + 1;
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    __auto_type replacement = take(api);
    assert(replacement.kind == AZN_HOT_CUES && replacement.valid && !replacement.lost_topics &&
           replacement.load_generation == fixture_load.load_generation && replacement.attempt != prior.attempt);
    /* A source mismatch with an unchanged lifecycle still loses live history. */
    ++matched_id;
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    --matched_id;
    __auto_type gap = take(api);
    assert(gap.kind == AZN_GAP && gap.b == AZN_HOT_CUES && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    assert(take(api).valid);
    /* A new epoch with an active replacement is not an empty retirement cut. */
    fixture_track_match_hook = native_callback_18;
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    gap = take(api);
    assert(gap.kind == AZN_GAP && gap.b == AZN_HOT_CUES && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    puts("PASS retiring hot-cue source stays quiet; live source uncertainty still reports loss");
}

static int native_callback_16(uintptr_t unused_0, uintptr_t object, uintptr_t unused_2, uintptr_t unused_3, uintptr_t unused_4) {
    (void) unused_0;
    (void) unused_2;
    (void) unused_3;
    (void) unused_4;
    void *self = ((void *) (object));
    put(object + 0xe1, (uint8_t) (0));
    status_writer_0(self, 882, UINTPTR_MAX);
    status_writer_5(self, 0);
    put(object + 0xe8, (uintptr_t) (0));
    put(object + 0xf0, (uintptr_t) (0));
    return 1;
}
static void native_callback_17(void *self, uintptr_t unused_1) {
    (void) unused_1;
    uintptr_t object = ((uintptr_t) (self));
    put(object + 8, integer(object + 8) + 1);
}
static void test_hot_metadata(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t context) {
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    atomic_store_explicit(&(hot_metadata), true, memory_order_seq_cst);
    Buffer_208 properties[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t property = ((uintptr_t) properties[i].bytes);
        put(context + 0x20 + i * 8, property);
        put(property, state().profile.cue_base_table);
        point(property + 0x30, -1);
        point(property + 0x88, -1);
    }
    uintptr_t property = ((uintptr_t) properties[3].bytes);
    Buffer_20 comment = {0}, string = {0};
    char text[] = "Imported \xc3\xa9";
    put(property + 0xe1, (uint8_t) (1));
    put(property + 0xe8, ((uintptr_t) comment.bytes));
    put(property + 0xf0, ((uintptr_t) comment.bytes) + 0x10);
    put(((uintptr_t) comment.bytes), ((uintptr_t) string.bytes));
    put(((uintptr_t) comment.bytes) + 8, ((uintptr_t) comment.bytes) + 0x10);
    put(((uintptr_t) string.bytes), ((uintptr_t) (text)));
    put(((uintptr_t) string.bytes) + 8, (uintptr_t) (sizeof text - 1));
    point(property + 0x30, 480);
    point(property + 0x88, 960);
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    struct azn_hot_receipt r = {0};


#define fixture_next_6() ({ \
    r.base.size = sizeof r; \
    r.base.version = 1;     \
    api->take(0, &r.base);  \
})


    assert(fixture_next_6() && r.base.kind == AZN_HOT_CUES && r.base.size == sizeof r);
    assert(r.base.hot[3].reserved == (AZN_HOT_COMMENT | AZN_HOT_AUTO_ACTIVE | AZN_HOT_AUTO_ON));
    assert(!strcmp(r.details.comments[3], text));
    const uint64_t revision = r.base.revision;

    processing = slot;
    status_original[0] = ((void *) (copy));
    status_original[5] = ((void *) (native_callback_17));
    hot_edit_original = ((void *) (native_callback_16));
    assert(status_hot_edit(0, property, 0, 0, 0));
    processing = NULL;
    assert(fixture_next_6() && r.base.kind == AZN_HOT_CUES && r.base.revision == revision + 1 && !r.base.lost_topics);
    assert(point_at(r.base.hot[3].in, 882, 1) && r.base.hot[3].out.state == AZN_ABSENT);
    assert(!r.base.hot[3].reserved && !r.details.comments[3][0] && !fixture_next_6());

    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
    point(property + 0x30, -1);
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    __auto_type old = take(api);
    assert(old.size == sizeof old && old.hot[3].in.state == AZN_ABSENT && !old.hot[3].reserved);
    assert(!fixture_next_6());
    puts("PASS owned hot comment and automatic flag, unsaved overwrite, load withdrawal and old receipt prefix");

#undef fixture_next_6
}

static void native_callback_11(void) {
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
}
static void native_callback_12(void) {
    fixture_track_match_hook = native_callback_11;
}
static void native_callback_13(void) {
    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
}
static void native_callback_14(void) {
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
}
static void native_callback_15(void) {
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
}
static void test_loop_retirement(const struct azstatus_native_v1 *api, Slot *slot, uintptr_t retained) {
    api->interest(0, status_retained_routes);
    atomic_store_explicit(&(reloop_installed), 15, memory_order_seq_cst);


#define fixture_seed_7()                                                                                          \
    do {                                                                                                          \
        status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);                                                     \
        __auto_type reloop = take(api);                                                                           \
        __auto_type loop = take(api);                                                                             \
        assert(reloop.kind == AZN_RELOOP && loop.kind == AZN_LOOP &&                                              \
               reloop.load_generation == fixture_load.load_generation &&                                          \
               loop.load_generation == fixture_load.load_generation && !reloop.lost_topics && !loop.lost_topics); \
    } while (0)


    fixture_seed_7();
    __auto_type previous = fixture_load.load_generation;
    fixture_track_match_hook = native_callback_15;
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    azstatus_native_receipt empty = {0};
    empty.size = sizeof empty;
    empty.version = 1;
    assert(!api->take(0, &empty));
    assert(!atomic_load_explicit(&(reloop_state[0].retained), memory_order_seq_cst));
    fixture_load.load_generation = previous + 1;
    fixture_seed_7();

    ++matched_id;
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    --matched_id;
    __auto_type loop_gap = take(api);
    __auto_type reloop_gap = take(api);
    assert(loop_gap.kind == AZN_GAP && loop_gap.lost_topics == status_retained_routes);
    assert(reloop_gap.kind == AZN_GAP && reloop_gap.b == AZN_RELOOP && reloop_gap.lost_topics == status_loop_route);
    fixture_seed_7();

    /* The loop owner can lose its load after the first source match and
     * before the residency reader reserves or reads the property. */
    assert(!atomic_flag_test_and_set_explicit(&(reloop_state[0].busy), memory_order_seq_cst));
    fixture_after_track_match_hook = native_callback_14;
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    atomic_flag_clear_explicit(&(reloop_state[0].busy), memory_order_seq_cst);
    assert(!api->take(0, &empty));
    fixture_load.load_generation = previous + 2;
    fixture_seed_7();

    put(retained + 0x178, (uint64_t) (2));
    fixture_after_track_match_hook = native_callback_13;
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    assert(!api->take(0, &empty));
    fixture_seed_7();

    /* Retirement can also land after the first residency read, before the
     * second source match seals its observation. */
    put(retained + 0x178, (uint64_t) (3));
    fixture_track_match_hook = native_callback_12;
    reloop_observe(0, &((*slot)), retained, fixture_load.invalidation_epoch, -1, 0);
    assert(!api->take(0, &empty));
    fixture_load.load_generation = previous + 4;
    fixture_seed_7();

    /* A residency callback can finish on the old load while another native
     * observation owns the publication slot. The old fence still identifies
     * this callback even when it cannot take the reservation. */
    {
        ReloopWrite change __attribute__((cleanup(reloop_write_scope_drop)));
        reloop_write_scope_init(&change, retained + 0xf8, false);
        assert(!atomic_flag_test_and_set_explicit(&(reloop_state[0].busy), memory_order_seq_cst));
        put(retained + 0x178, (uint64_t) (2));
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
        reloop_write_scope_finish(&change);
        atomic_flag_clear_explicit(&(reloop_state[0].busy), memory_order_seq_cst);
        assert(!api->take(0, &empty));
    }
    fixture_load.load_generation = previous + 5;
    fixture_seed_7();
    {
        ReloopWrite change __attribute__((cleanup(reloop_write_scope_drop)));
        reloop_write_scope_init(&change, retained + 0xf8, false);
        assert(!atomic_flag_test_and_set_explicit(&(reloop_state[0].busy), memory_order_seq_cst));
        put(retained + 0x178, (uint64_t) (3));
        reloop_write_scope_finish(&change);
        atomic_flag_clear_explicit(&(reloop_state[0].busy), memory_order_seq_cst);
        __auto_type busy = take(api);
        assert(busy.kind == AZN_GAP && busy.b == AZN_RELOOP && busy.lost_topics == status_loop_route);
    }
    puts("PASS retired loop and residency reads stay quiet; active source mismatch still reports loss");

#undef fixture_seed_7
}

static void *stalled_writer(void *unused) {
    (void) unused;
    azstatus_native_receipt r = {0};
    r.kind = AZN_LOOP;
    r.a = 19;
    azn_emit(&r);
    return NULL;
}
struct Crossing {
    Slot *slot;
    void *core;
    uintptr_t address, destination;
};
static void *crossing_writer(void *opaque) {
    struct Crossing *job = opaque;
    processing = job->slot;
    StatusMotionWrite crossing __attribute__((cleanup(status_motion_write_drop)));
    status_motion_write_init(&crossing, job->core, true, 1);
    put(job->address + 0x28, (uint64_t) (48000));
    status_motion_write_finish(&crossing, job->destination);
    return NULL;
}
static void failing_rate_writer(void *opaque) { rate_writer_0(opaque, 1); }
static void native_callback_1(void *unused_0, unsigned unused_1) {
    (void) unused_0;
    (void) unused_1;
    fixture_raise();
}
static void native_callback_2(void *core) {
    assert(atomic_load_explicit(&(state().decks[0].motion.active_writers), memory_order_seq_cst) == 1);
    put(((uintptr_t) (core)) + 9, (uint8_t) (0));
}
static void native_callback_3(void *core, unsigned unused_1) {
    (void) unused_1;
    assert(atomic_load_explicit(&(state().decks[0].motion.active_writers), memory_order_seq_cst) == 1);
    put(((uintptr_t) (core)) + 9, (uint8_t) (1));
}
static void native_callback_4(void *unused_0, const void *unused_1) {
    (void) unused_0;
    (void) unused_1;
}
static void native_callback_5(void *object) {
    put(((uintptr_t) (object)) + 0x170, (uint32_t) (2));
    slider_lock_writer_3(object);
}
static void native_callback_6(void *object) {
    put(((uintptr_t) (object)) + 0x170, (uint32_t) (1));
}
static void native_callback_7(void *o, unsigned mode) {
    put(((uintptr_t) (o)) + 0xb8, ((uint8_t) (mode == 2)));
}
static void native_callback_8(void *o, unsigned value, unsigned unused_2) {
    (void) unused_2;
    put(((uintptr_t) (o)) + 8, ((uint8_t) (value)));
}
static void native_callback_9(void *o, unsigned value) {
    put(((uintptr_t) (o)) + 16, ((uint8_t) (value)));
}
static __attribute__((noinline)) void native_callback_10(void *self, uintptr_t source, uintptr_t unused_2) {
    (void) unused_2;
    assert(pointer((uintptr_t) self) && pointer(source));
    Buffer_100 *destination = self;
    const Buffer_100 *input = (const void *) source;
    memcpy(destination->bytes + 0x30, input->bytes + 0x30, 64);
    memcpy(destination->bytes + 0x88, input->bytes + 0x88, 64);
    put((uintptr_t) self + 8, (unsigned) integer((uintptr_t) self + 8) + 1u);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "motion-pressure") == 0) {
        test_motion_pressure();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "transport-retirement") == 0) {
        test_transport_retirement();
        return 0;
    }
#ifdef AZSDK_DURATION_TEST_ONLY
    {
        Buffer_300 core = {0};
        Slot *slot = &(state().decks[0]);
        atomic_store_explicit(&(slot->core), ((uintptr_t) core.bytes), memory_order_seq_cst);
        put(((uintptr_t) core.bytes) + 0x18, UINT64_C(0x0001000100000001));
        __auto_type api = azstem_status_service_v1();
        assert(api->wake_fd() >= 0);
        test_native_duration(api, &((*slot)), ((uintptr_t) core.bytes));
        atomic_store_explicit(&(slot->core), 0, memory_order_seq_cst);
        return 0;
    }
#endif
    const bool hot_retirement = argc == 2 && strcmp(argv[1], "hot-retirement") == 0;
    const bool hot_details = argc == 2 && strcmp(argv[1], "hot-metadata") == 0;
    const bool loop_retirement = argc == 2 && strcmp(argv[1], "loop-retirement") == 0;
    const bool delayed_loop_seed = argc == 2 && strcmp(argv[1], "delayed-loop-seed") == 0;
    if (!hot_retirement && !hot_details && !loop_retirement && !delayed_loop_seed) {
        test_concurrent_scan();
        test_sync_role_values();
    }
    Buffer_300 unit = {0}, core = {0}, facade = {0};
    Buffer_100 bundle = {0};
    Buffer_500 context = {0};
    Buffer_208 retained = {0};
    Buffer_100 tables[7] = {0};
    State *s = &(state());
    Slot *slot = &(s->decks[0]);
    __auto_type p = &(s->profile);
    p->unit = ((uintptr_t) tables[0].bytes);
    p->core = ((uintptr_t) tables[1].bytes);
    p->facade = ((uintptr_t) tables[2].bytes);
    p->box = ((uintptr_t) tables[3].bytes);
    p->set_table = ((uintptr_t) tables[4].bytes);
    p->cue_table = ((uintptr_t) tables[5].bytes);
    p->cue_base_table = ((uintptr_t) tables[6].bytes);
    put(((uintptr_t) unit.bytes), p->unit);
    put(((uintptr_t) core.bytes), p->core);
    put(((uintptr_t) facade.bytes), p->facade);
    put(((uintptr_t) unit.bytes) + 0x128, ((uintptr_t) core.bytes));
    put(((uintptr_t) unit.bytes) + 0x190, ((uintptr_t) bundle.bytes));
    put(((uintptr_t) bundle.bytes) + 0x40, ((uintptr_t) facade.bytes));
    put(((uintptr_t) facade.bytes) + 0x18, ((uintptr_t) core.bytes) + 0x110);
    put(((uintptr_t) facade.bytes) + 0x40, ((uintptr_t) core.bytes));
    put(((uintptr_t) core.bytes) + 0x110, p->box);
    put(((uintptr_t) facade.bytes) + 0xc8, p->set_table);
    put(((uintptr_t) facade.bytes) + 0xd8, ((uintptr_t) core.bytes) + 0x110);
    put(((uintptr_t) facade.bytes) + 0x68, ((uintptr_t) context.bytes));
    put(((uintptr_t) facade.bytes) + 0x128, ((uintptr_t) context.bytes));
    uintptr_t active = ((uintptr_t) context.bytes) + 0x160, kept = ((uintptr_t) retained.bytes);
    put(((uintptr_t) context.bytes) + 0x18, kept);
    put(kept, p->cue_table);
    put(active, p->cue_base_table);
    point(kept + 0x30, -1);
    point(kept + 0x88, -1);
    point(active + 0x30, -1);
    point(active + 0x88, -1);
    memcpy(core.bytes + 0x18, ((const void *) (kept + 0x30)), 12);
    put(((uintptr_t) core.bytes) + 0x18, (uint64_t) (0x0001000100000001ULL));
    put(((uintptr_t) core.bytes) + 0x20, (uint32_t) (3));
    slot->unit = ((uintptr_t) unit.bytes);
    slot->facade = ((uintptr_t) facade.bytes);
    atomic_store_explicit(&(slot->core), ((uintptr_t) core.bytes), memory_order_seq_cst);
    atomic_store_explicit(&(s->enabled), AZEXIT_PASSIVE_STATUS, memory_order_seq_cst);
    __auto_type api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    if (hot_retirement) {
        test_hot_retirement(api, &((*slot)), ((uintptr_t) context.bytes));
        return 0;
    }
    if (hot_details) {
        test_hot_metadata(api, &((*slot)), ((uintptr_t) context.bytes));
        return 0;
    }
    if (loop_retirement) {
        test_loop_retirement(api, &((*slot)), kept);
        return 0;
    }
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    processing = slot;
    assert(commands() == 0); // no command capability is needed to observe
    status_original[0] = ((void *) (copy));
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    __auto_type seed = take(api);
    assert(seed.kind == AZN_LOOP && !seed.a && !seed.lost_topics &&
           seed.points[0].state == AZN_ABSENT && seed.points[1].state == AZN_ABSENT);
    status_writer_0(((void *) (kept)), 0, UINTPTR_MAX);
    status_writer_0(((void *) (kept)), 0, 48000);
    status_writer_0(((void *) (active)), 0, 48000);
    status_writer_0(((void *) (active)), UINTPTR_MAX, UINTPTR_MAX);
    status_writer_0(((void *) (active)), 0, 48000);
    __auto_type in = take(api);
    assert(!in.lost_topics && in.points[0].state == AZN_COORD && point_at(in.points[0], 0, 1) && in.points[1].state == AZN_ABSENT);
    assert(in.reloop == AZN_RELOOP_DENIED);
    __auto_type out = take(api);
    assert(!out.a && point_at(out.points[1], 48000, 1) && !out.lost_topics);
    assert(out.reloop == AZN_RELOOP_UNKNOWN);
    assert(take(api).a == 1);
    __auto_type exit = take(api);
    assert(!exit.a && !exit.lost_topics && exit.points[2].state == AZN_ABSENT && exit.points[0].state == AZN_COORD);
    __auto_type again = take(api);
    assert(again.a && !again.lost_topics && again.epoch == 11 && again.load_generation == 7);
    if (delayed_loop_seed) {
        const uint64_t seed_id = azn_seed_id(0);
        api->interest(0, status_retained_routes);
        assert(azn_seed_id(0) > seed_id);
        status_writer_0(((void *) (active)), UINTPTR_MAX, UINTPTR_MAX);
        status_writer_0(((void *) (active)), 0, 48000);
        status_writer_0(((void *) (active)), UINTPTR_MAX, UINTPTR_MAX);
        status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
        {
            int range_values_4[] = {0, 1, 0};
            for (unsigned range_index_4 = 0; range_index_4 < sizeof(range_values_4) / sizeof(range_values_4)[0]; ++range_index_4) {
                int engaged = range_values_4[range_index_4];

                __auto_type edge = take(api);
                assert(edge.kind == AZN_LOOP && edge.a == engaged && edge.load_generation == 7 && !edge.lost_topics);
            }
        }
        azstatus_native_receipt empty = {0};
        empty.size = sizeof empty;
        empty.version = 1;
        assert(!api->take(0, &empty));
        puts("PASS delayed owned loop seed preserves all three discrete edges");
        return 0;
    }
    put(active + 0xc, (uint32_t) (3));
    put(active + 0xc8, (uint32_t) (0));
    status_writer_0(((void *) (active)), 0, 48000);
    assert(take(api).c == AZN_MODE_TIME);
    put(active + 0xc8, (uint32_t) (1));
    put(active + 0xd8, (uint32_t) (0x10004));
    status_writer_0(((void *) (active)), 0, 48000);
    assert(take(api).c == AZN_MODE_BEAT);
    put(active + 0xc8, (uint32_t) (7));
    status_writer_0(((void *) (active)), 0, 48000);
    assert(take(api).c == AZN_MODE_UNKNOWN);
    atomic_store_explicit(&(reloop_installed), 15, memory_order_seq_cst);
    reloop_observe(0, &((*slot)), kept, fixture_load.invalidation_epoch, -1, 0);
    __auto_type waiting = take(api);
    assert(waiting.kind == AZN_RELOOP && waiting.reloop == AZN_RELOOP_DENIED && !waiting.lost_topics);


#define fixture_residency_8(n_argument, value_argument)                       \
    do {                                                                      \
        unsigned fixture_residency_8_n = (n_argument);                        \
        uint32_t fixture_residency_8_value = (value_argument);                \
        uintptr_t lock = kept + (fixture_residency_8_n ? 0x180 : 0xf8);       \
        ReloopWrite change __attribute__((cleanup(reloop_write_scope_drop))); \
        reloop_write_scope_init(&change, lock, false);                        \
        put(lock + 0x80, fixture_residency_8_value);                          \
        reloop_write_scope_finish(&change);                                   \
    } while (0)


    fixture_residency_8(0, 2);
    fixture_residency_8(1, 2);
    __auto_type ready = take(api);
    assert(ready.kind == AZN_RELOOP && ready.reloop == AZN_RELOOP_ALLOWED && !ready.lost_topics);
    fixture_residency_8(1, 3);
    fixture_residency_8(1, 2);
    assert(take(api).reloop == AZN_RELOOP_DENIED && take(api).reloop == AZN_RELOOP_ALLOWED);
    processing = NULL;
    fixture_residency_8(1, 3);
    fixture_residency_8(1, 2);
    processing = slot;
    assert(take(api).reloop == AZN_RELOOP_DENIED && take(api).reloop == AZN_RELOOP_ALLOWED);
    {
        ReloopEdit overlap __attribute__((cleanup(reloop_edit_drop)));
        reloop_edit_init(&overlap, &reloop_state[0]);
        fixture_residency_8(1, 3);
        fixture_residency_8(1, 2);
    }
    __auto_type missed = take(api);
    assert(missed.kind == AZN_GAP && missed.b == AZN_RELOOP && missed.lost_topics == status_loop_route);
    reloop_observe(0, &((*slot)), kept, fixture_load.invalidation_epoch, -1, 0);
    assert(take(api).reloop == AZN_RELOOP_ALLOWED);
    atomic_store_explicit(&(reloop_installed), 0, memory_order_seq_cst);
    puts("PASS retained reloop lock warming, admission, reset/reacquisition and scoped same-value loss");
    // Mutations outside the captured owner interval are visible through the revision gap.
    copy(((void *) (active)), UINTPTR_MAX, UINTPTR_MAX);
    copy(((void *) (active)), 0, 48000);
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    __auto_type gap = take(api);
    assert(gap.a && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    for (unsigned i = 0; i < 300; i++) {
        azstatus_native_receipt r = {0};
        r.kind = AZN_LOOP;
        r.a = i;
        azn_emit(&r);
    }
    for (unsigned i = 0; i < 256; i++) assert((unsigned) (take(api).a) == i);
    assert(take(api).kind == AZN_GAP);
    status_loop_capture(0, &((*slot)), 0, NULL, NULL, 0);
    assert(take(api).a == 1); // full consumer recovery with no further panel event
    pause_next = true;
    pthread_t stalled;
    assert(!pthread_create(&stalled, NULL, stalled_writer, NULL));
    while (!atomic_load_explicit(&(parked), memory_order_seq_cst)) sched_yield();
    azstatus_native_receipt rejected = {0};
    rejected.kind = AZN_LOOP;
    azn_emit(&rejected);
    __auto_type final_loss = take(api);
    assert(final_loss.kind == AZN_GAP && final_loss.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    azstatus_native_receipt independent = {0};
    independent.kind = AZN_METADATA;
    azn_emit(&independent);
    assert(take(api).kind == AZN_METADATA);
    resume_writer = true;
    assert(!pthread_join(stalled, NULL));
    rejected.size = sizeof rejected;
    rejected.version = 1;
    assert(!api->take(0, &rejected));
    s->motion_hooked = true;
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    uintptr_t c = ((uintptr_t) core.bytes);
    matched_id = 2;
    put(c + 0x18, (uint64_t) (0x0001000100000001ULL));
    put(c + 0x20, (uint32_t) (2));
    put(c + 0x28, (uint64_t) (0));
    put(c + 0x68, (uint32_t) (44100));


#define fixture_travel_9(end_argument)                                               \
    do {                                                                             \
        uint64_t fixture_travel_9_end = (end_argument);                              \
        StatusMotionWrite update __attribute__((cleanup(status_motion_write_drop))); \
        status_motion_write_init(&update, core.bytes, false, 64);                    \
        put(c + 0x28, fixture_travel_9_end);                                         \
        status_motion_write_finish(&update, 0);                                      \
    } while (0)


    fixture_travel_9(0);
    rejected.size = sizeof rejected;
    rejected.version = 1;
    assert(!api->take(0, &rejected));
    fixture_travel_9(64);
    __auto_type m = take(api);
    assert(m.kind == AZN_MOTION && m.a == AZN_MOTION_SEED && m.motion_generation == 1 && m.motion_order == 1 &&
           m.points[0].state == AZN_INVALID && m.points[1].state == AZN_COORD && point_at(m.points[1], 64, m.observed_window_samples) && m.points[1].rate == 44100);
    fixture_travel_9(128);
    m = take(api);
    assert(m.a == AZN_MEASURE && m.motion_order == 2 && point_at(m.points[0], 64, m.from_window_samples) && point_at(m.points[1], 128, m.observed_window_samples));
    Buffer_40 destination = {0};
    put(((uintptr_t) destination.bytes) + 8, (uint64_t) (0x0001000100000001ULL));
    put(((uintptr_t) destination.bytes) + 0x10, (uint32_t) (2));
    put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (0));
    {
        StatusMotionWrite reposition __attribute__((cleanup(status_motion_write_drop)));
        status_motion_write_init(&reposition, core.bytes, true, 1);
        put(c + 0x28, (uint64_t) (0));
        status_motion_write_finish(&reposition, ((uintptr_t) destination.bytes));
    }
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.motion_generation == 2 && m.motion_order == 3 &&
           point_at(m.points[0], 128, m.from_window_samples) && point_at(m.points[1], 0, m.observed_window_samples));
    {
        // Park a sample across the reposition. Neither racing copy can publish.
        StatusMotionWrite parked_read __attribute__((cleanup(status_motion_write_drop)));
        status_motion_write_init(&parked_read, core.bytes, false, 64);
        struct Crossing crossing = {slot, core.bytes, c, (uintptr_t) destination.bytes};
        pthread_t other;
        assert(!pthread_create(&other, NULL, crossing_writer, &crossing));
        assert(!pthread_join(other, NULL));
        status_motion_write_finish(&parked_read, 0);
    }
    m = take(api);
    assert(m.kind == AZN_GAP && (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
    fixture_travel_9(48064);
    m = take(api);
    assert(m.kind == AZN_MOTION && m.motion_generation == 3 && point_at(m.points[1], 48064, m.observed_window_samples) &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
    // Witness the actual scheduled loop callback. A coincident landing without
    // the typed command/owner/interval remains an unclassified reposition.
    Buffer_100 typed_tables[6] = {0}, jump = {0}, callback = {0}, basis = {0}, quantize = {0}, beat_owner = {0};
    p->commands[0].table = ((uintptr_t) typed_tables[0].bytes);
    p->commands[3].table = ((uintptr_t) typed_tables[1].bytes);
    p->commands[8].table = ((uintptr_t) typed_tables[2].bytes);
    p->commands[10].table = ((uintptr_t) typed_tables[3].bytes);
    p->interval_table = ((uintptr_t) typed_tables[4].bytes);
    p->interval_source_table = ((uintptr_t) typed_tables[5].bytes);
    put(((uintptr_t) unit.bytes) + 0x180, ((uintptr_t) basis.bytes));
    put(((uintptr_t) jump.bytes), p->commands[0].table);
    put(((uintptr_t) jump.bytes) + 0x20, c);
    put(((uintptr_t) jump.bytes) + 0x18, ((uintptr_t) callback.bytes));
    put(((uintptr_t) callback.bytes), p->commands[3].table);
    put(((uintptr_t) callback.bytes) + 0x20, ((uintptr_t) basis.bytes));
    put(((uintptr_t) callback.bytes) + 0x28, active);
    point(active + 0x30, 0);
    point(active + 0x88, 48000);
    put(active + 0x58, (uintptr_t) (0x20000));


#define fixture_interval_10(address_argument, reverse_argument, sourced_argument)                                     \
    do {                                                                                                              \
        uintptr_t fixture_interval_10_address = (address_argument);                                                   \
        bool fixture_interval_10_reverse = (reverse_argument);                                                        \
        bool fixture_interval_10_sourced = (sourced_argument);                                                        \
        put(fixture_interval_10_address, fixture_interval_10_sourced ? p->interval_source_table : p->interval_table); \
        memcpy(((void *) (fixture_interval_10_address + 8)), ((const void *) (active + 0x30)), 12);                   \
        put(fixture_interval_10_address + 0x18, (uint64_t) (fixture_interval_10_reverse ? 48000 : 0));                \
        put(fixture_interval_10_address + 0x20, (uint64_t) (fixture_interval_10_reverse ? 0 : 48000));                \
        if (fixture_interval_10_sourced) put(fixture_interval_10_address + 0x28, (uintptr_t) (0x20000));              \
    } while (0)


    fixture_interval_10(((uintptr_t) jump.bytes) + 0x28, false, true);
    fixture_interval_10(((uintptr_t) callback.bytes) + 0x38, false, true);
    fixture_interval_10(((uintptr_t) callback.bytes) + 0x68, true, false);
    matched_id = 3;
    put(c + 0x20, (uint32_t) (3));
    put(c + 0x68, (uint32_t) (48000));
    put(((uintptr_t) destination.bytes) + 0x10, (uint32_t) (3));
    Buffer_80 readable = {0}, readable_table = {0};
    s->build = 2;
    put(((uintptr_t) core.bytes) + 0xb8, ((uintptr_t) readable.bytes));
    put(((uintptr_t) readable.bytes), ((uintptr_t) readable_table.bytes));
    put(((uintptr_t) readable_table.bytes) + 0x60, (uintptr_t) (0x7a78b8));
    s->motion_actions = true;


#define fixture_land_11(to_argument)                                                 \
    do {                                                                             \
        uint64_t fixture_land_11_to = (to_argument);                                 \
        STATUS_ACTION(origin, ((uintptr_t) jump.bytes), 0, false);                   \
        StatusMotionWrite action __attribute__((cleanup(status_motion_write_drop))); \
        status_motion_write_init(&action, core.bytes, true, 1);                      \
        put(c + 0x28, fixture_land_11_to);                                           \
        put(((uintptr_t) destination.bytes) + 0x18, fixture_land_11_to);             \
        status_motion_write_finish(&action, ((uintptr_t) destination.bytes));        \
    } while (0)


    // The stock rate-adjusted render can cross OUT before the reserved jump.
    fixture_travel_9(48001);
    take(api);
    fixture_land_11(0);
    m = take(api);
    assert(m.a == AZN_WRAP && m.occurrence_ordinal == 1 && !m.lost_topics &&
           point_at(m.points[0], 48001, m.from_window_samples) && point_at(m.points[1], 0, m.observed_window_samples));
    {
        STATUS_ACTION(origin, ((uintptr_t) jump.bytes), 0, false);
        StatusHead departure = status_head(c), landing = departure;
        slot->motion.window_samples = 64;
        slot->motion.window_rate = 48000;
        // This fixture models a 64-frame native render. One sample past OUT
        // is equivalent within that measured window and still wraps.
        const int overshoot = compare_timestamps(UINT64_C(48001), UINT64_C(48000), 48000, 48000, 64);
        assert(overshoot != AZ_TIME_INVALID && overshoot == AZ_TIME_EQUAL);
        departure.sample = 48000;
        assert(status_action(slot, departure, landing) == AZN_WRAP);
        departure.sample = 47936;
        assert(!status_action(slot, departure, landing));
        departure.sample = 48001;
        landing.sample = 1;
        assert(!status_action(slot, departure, landing));
        // Reverse playback can cross IN in the same integer render split.
        {
            uintptr_t range_values_5[] = {active + 0x40, ((uintptr_t) jump.bytes) + 0x40,
                                          ((uintptr_t) callback.bytes) + 0x50, ((uintptr_t) callback.bytes) + 0x88};
            for (unsigned range_index_5 = 0; range_index_5 < sizeof(range_values_5) / sizeof(range_values_5)[0]; ++range_index_5) {
                uintptr_t address = range_values_5[range_index_5];
                put(address, (uint64_t) (10));
            }
        }
        departure.sample = 9;
        landing.sample = 48000;
        assert(status_action(slot, departure, landing) == AZN_WRAP);
        departure.sample = 74;
        assert(!status_action(slot, departure, landing));
        {
            uintptr_t range_values_6[] = {active + 0x40, ((uintptr_t) jump.bytes) + 0x40,
                                          ((uintptr_t) callback.bytes) + 0x50, ((uintptr_t) callback.bytes) + 0x88};
            for (unsigned range_index_6 = 0; range_index_6 < sizeof(range_values_6) / sizeof(range_values_6)[0]; ++range_index_6) {
                uintptr_t address = range_values_6[range_index_6];
                put(address, (uint64_t) (0));
            }
        }
    }
    test_initial_loop_actions(slot, c, active, ((uintptr_t) jump.bytes), ((uintptr_t) callback.bytes));
    put(((uintptr_t) callback.bytes) + 0x28, (uintptr_t) (0));
    fixture_travel_9(48000);
    take(api);
    fixture_land_11(0);
    m = take(api);
    assert(m.a == AZN_REPOSITION && !m.occurrence_ordinal);
    put(((uintptr_t) callback.bytes), p->commands[8].table);
    put(((uintptr_t) callback.bytes) + 0x20, ((uintptr_t) context.bytes));
    fixture_land_11(48000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 2);
    fixture_land_11(48000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 3);
    put(((uintptr_t) readable_table.bytes) + 0x60, (uintptr_t) (0));
    fixture_land_11(48000);
    m = take(api);
    assert(m.a == AZN_REPOSITION && !m.occurrence_ordinal &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)));
    p->quantize = ((uintptr_t) typed_tables[4].bytes);
    put(((uintptr_t) quantize.bytes), p->quantize);
    put(((uintptr_t) facade.bytes) + 0x50, ((uintptr_t) quantize.bytes));
    put(((uintptr_t) facade.bytes) + 0x110, ((uintptr_t) quantize.bytes));
    put(((uintptr_t) callback.bytes), p->commands[10].table);
    put(((uintptr_t) callback.bytes) + 0x20, ((uintptr_t) beat_owner.bytes));
    put(((uintptr_t) beat_owner.bytes) + 0x10, ((uintptr_t) quantize.bytes));
    fixture_land_11(24000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 4);
    Buffer_100 needle = {0}, needle_cb_vt = {0}, pause_f = {0}, pause_vt = {0};
    p->commands[14].table = ((uintptr_t) needle_cb_vt.bytes);
    p->play_pause_table = ((uintptr_t) pause_vt.bytes);
    put(((uintptr_t) callback.bytes), p->commands[14].table);
    put(((uintptr_t) callback.bytes) + 0x20, ((uintptr_t) needle.bytes));
    put(((uintptr_t) needle.bytes), (uintptr_t) (0x2be8390));
    put(((uintptr_t) needle.bytes) + 0x18, ((uintptr_t) pause_f.bytes));
    put(((uintptr_t) facade.bytes) + 0x58, ((uintptr_t) pause_f.bytes));
    put(((uintptr_t) pause_f.bytes), p->play_pause_table);
    put(((uintptr_t) pause_f.bytes) + 0x58, c);
    fixture_land_11(17000);
    m = take(api);
    assert(m.a == AZN_SEEK && m.occurrence_ordinal == 5 && point_at(m.points[1], 17000, m.observed_window_samples));
    put(((uintptr_t) pause_f.bytes) + 0x58, (uintptr_t) (0));
    fixture_land_11(18000);
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.history_only &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && point_at(m.points[1], 18000, m.observed_window_samples) && !m.occurrence_ordinal);
    Buffer_80 noqua = {0};
    put(((uintptr_t) noqua.bytes), noqua_profiles[1].owner);
    put(((uintptr_t) noqua.bytes) + 8, ((uintptr_t) pause_f.bytes));
    put(((uintptr_t) noqua.bytes) + 0x10, ((uintptr_t) quantize.bytes));
    put(((uintptr_t) noqua.bytes) + 0x20, ((uintptr_t) quantize.bytes) + 0x20);
    put(((uintptr_t) pause_f.bytes) + 0x58, c);
    noq_core = c;
    noq_target = ((uintptr_t) destination.bytes);
    noq_return = noqua_profiles[1].jump_returns[1];
    noqua_original = ((void *) (fake_noqua));
    put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (56000));
    assert(noqua_hook(noqua.bytes, 0, 0x10010, NULL));
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 6 && point_at(m.points[1], 56000, m.observed_window_samples));
    ++noq_return;
    put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (10000));
    assert(noqua_hook(noqua.bytes, 1, 0x10010, NULL));
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.history_only && !m.occurrence_ordinal);
    --noq_return;
    beat_grid_original = ((void *) (fake_grid_jump));
    put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (43000));
    assert(beat_grid_hook(noqua.bytes, 1, 0x10010, NULL, true));
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 7 && point_at(m.points[1], 43000, m.observed_window_samples) && !beat_origin.owner);
    Buffer_80 direct_controller = {0};
    put(((uintptr_t) basis.bytes) + 0x38, ((uintptr_t) direct_controller.bytes));
    put(((uintptr_t) pause_f.bytes) + 0x50, ((uintptr_t) direct_controller.bytes));
    put(((uintptr_t) quantize.bytes) + 0x20, ((uintptr_t) direct_controller.bytes));
    put(((uintptr_t) quantize.bytes) + 0x40, ((uintptr_t) quantize.bytes));
    put(((uintptr_t) quantize.bytes) + 0x48, ((uintptr_t) pause_f.bytes));
    memcpy(((void *) (((uintptr_t) pause_f.bytes) + 0x18)), ((const void *) (((uintptr_t) destination.bytes) + 8)), 12);
    put(((uintptr_t) pause_f.bytes) + 0x28, (uint64_t) (12000));
    const uintptr_t paused_position = ((uintptr_t) pause_f.bytes) + 0x10;
    {
        STATUS_ACTION(direct, 0, 0, true);
        status_direct_request(&(direct), ((void *) (paused_position)), 2, 0x14dd4b0);
        assert(direct.direct_kind == AZN_SEEK);
        StatusMotionWrite action __attribute__((cleanup(status_motion_write_drop)));
        status_motion_write_init(&action, core.bytes, true, 1);
        put(c + 0x28, (uint64_t) (12000));
        status_motion_write_finish(&action, paused_position);
    }
    m = take(api);
    assert(m.a == AZN_SEEK && m.occurrence_ordinal == 8 && !m.lost_topics);
    {
        STATUS_ACTION(direct, 0, 0, true);
        status_direct_request(&(direct), ((void *) (paused_position)), 2, 0x14dd4b4);
        assert(!direct.direct_kind);
        put(((uintptr_t) pause_f.bytes) + 0x50, (uintptr_t) (0));
        status_direct_request(&(direct), ((void *) (paused_position)), 2, 0x14dd4b0);
        assert(!direct.direct_kind);
        put(((uintptr_t) pause_f.bytes) + 0x50, ((uintptr_t) direct_controller.bytes));
    }
    put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (0));
    {
        STATUS_ACTION(direct, 0, 0, true);
        status_direct_request(&(direct), destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(direct.direct_kind == AZN_JUMP);
        StatusMotionWrite action __attribute__((cleanup(status_motion_write_drop)));
        status_motion_write_init(&action, core.bytes, true, 1);
        put(c + 0x28, (uint64_t) (0));
        status_motion_write_finish(&action, ((uintptr_t) destination.bytes));
    }
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 9 && !m.lost_topics);
    {
        STATUS_ACTION(direct, 0, 0, true);
        put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (48001)); // outside the active loop, not an owned interior target
        status_direct_request(&(direct), destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(!direct.direct_kind);
        put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (0));
        put(((uintptr_t) quantize.bytes) + 0x48, (uintptr_t) (0));
        status_direct_request(&(direct), destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(!direct.direct_kind);
        put(((uintptr_t) quantize.bytes) + 0x48, ((uintptr_t) pause_f.bytes));
    }
    fixture_travel_9(1000);
    take(api);
    put(((uintptr_t) pause_f.bytes) + 0x28, (uint64_t) (1000));
    put(((uintptr_t) readable_table.bytes) + 0x60, (uintptr_t) (0x7a7aa0));
    uint64_t same_point_ordinal = 9;
    for (unsigned mode = 0; mode < 8; ++mode) {
        {
            STATUS_ACTION(direct, 0, 0, true);
            status_direct_request(&(direct), ((void *) (paused_position)), 2, 0x14dd4b0);
            assert(direct.direct_kind == AZN_SEEK);
            StatusMotionWrite action __attribute__((cleanup(status_motion_write_drop)));
            status_motion_write_init(&action, core.bytes, true, 1);
            if (mode == 7) {
                /* An unowned nested setter sharing this provider cannot
                 * lend its range proof to the outer source. */
                Buffer_300 foreign_core = {0};
                StatusMotionWrite nested __attribute__((cleanup(status_motion_write_drop)));
                status_motion_write_init(&nested, foreign_core.bytes, true, 1);
                status_motion_write_capture_limit(((uintptr_t) readable.bytes), 1000, 0x1567dc4);
            } else if (mode != 2) {
                status_motion_write_capture_limit(mode == 4 ? ((uintptr_t) readable.bytes) + 8 : ((uintptr_t) readable.bytes),
                                                  mode == 5 ? 999 : mode == 6 ? -1
                                                                              : 1000,
                                                  mode == 3 ? 0x1567dc8 : 0x1567dc4);
            }
            status_motion_write_finish(&action, paused_position);
        }
        m = take(api);
        if (mode < 2) {
            assert(m.a == AZN_SEEK && m.occurrence_ordinal == ++same_point_ordinal && !m.lost_topics);
        } else {
            assert(!m.occurrence_ordinal && (m.lost_topics & motion_routes) == motion_routes);
        }
    }
    put(((uintptr_t) readable_table.bytes) + 0x60, (uintptr_t) (0x7a78b8));
    puts("PASS repeated equal-position actions, actual range return, rejected range and nested owner exclusion");
    Buffer_c0 transport_bundle = {0};
    put(((uintptr_t) facade.bytes) + 0x78, ((uintptr_t) transport_bundle.bytes));
    put(((uintptr_t) transport_bundle.bytes), c);
    put(((uintptr_t) transport_bundle.bytes) + 8, ((uintptr_t) context.bytes));
    put(((uintptr_t) transport_bundle.bytes) + 0xa8, ((uintptr_t) pause_f.bytes));
    atomic_store_explicit(&(transport_installed), 15, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    put(((uintptr_t) context.bytes) + 8, (uint8_t) (1));
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    m = take(api);
    assert(m.kind == AZN_TRANSPORT_STATE && m.valid == AZ_AVAIL_READY && m.a == AZ_PLAYBACK_PAUSED);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (1));
    transport_observe(0, &((*slot)), TransportPlay, NULL);
    m = take(api);
    assert(m.a == AZ_PLAYBACK_PLAYING && !m.lost_topics);
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    assert(!api->take(0, &rejected));
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    transport_observe(0, &((*slot)), TransportPause, NULL);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (1));
    transport_observe(0, &((*slot)), TransportAudition, NULL);
    assert(take(api).a == AZ_PLAYBACK_CUEING);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    transport_observe(0, &((*slot)), TransportPause, NULL);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (1));
    transport_observe(0, &((*slot)), TransportOtherPlay, NULL);
    m = take(api);
    assert(m.valid == AZ_AVAIL_UNSUPPORTED && m.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    atomic_store_explicit(&(transport_installed), 0, memory_order_seq_cst);
    put(((uintptr_t) context.bytes) + 8, (uint8_t) (0));
    Buffer_100 hot_properties[8] = {0}, replacement = {0};
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    for (unsigned i = 0; i < 8; i++) {
        uintptr_t ptr = ((uintptr_t) hot_properties[i].bytes);
        put(((uintptr_t) context.bytes) + 0x20 + i * 8, ptr);
        put(ptr, p->cue_base_table);
        point(ptr + 0x30, -1);
        point(ptr + 0x88, -1);
    }
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    __auto_type hot = take(api);
    assert(hot.kind == AZN_HOT_CUES && hot.valid && hot.attempt && hot.hot[7].in.state == AZN_ABSENT);
    uint64_t provider = hot.attempt;
    put(((uintptr_t) hot_properties[7].bytes) + 0x10, (uint8_t) (255));
    status_writer_0(hot_properties[7].bytes, 0, UINTPTR_MAX);
    hot = take(api);
    assert(hot.kind == AZN_HOT_CUES && hot.valid && hot.attempt == provider && !hot.lost_topics &&
           hot.hot[7].in.state == AZN_COORD && point_at(hot.hot[7].in, 0, 1) && hot.hot[7].rgb == 0xff0000);
    status_writer_0(hot_properties[0].bytes, 480, 960);
    hot = take(api);
    assert(point_at(hot.hot[0].in, 480, 1) && point_at(hot.hot[0].out, 960, 1) && point_at(hot.hot[7].in, 0, 1));
    status_original[0] = ((void *) (native_callback_10));
    status_writer_0(((void *) (active)), ((uintptr_t) hot_properties[0].bytes), 0);
    __auto_type linked = take(api);
    assert(linked.hot_link.state == AZN_HOT_LINK_READY && linked.hot_link.slot == 1 &&
           linked.hot_link.provider == provider && linked.hot_link.stamp == hot.revision);
    memcpy(replacement.bytes, hot_properties[0].bytes, sizeof replacement.bytes);
    status_writer_0(((void *) (active)), ((uintptr_t) replacement.bytes), 0);
    assert(take(api).hot_link.state == AZN_HOT_LINK_UNKNOWN);
    status_original[0] = ((void *) (copy));
    status_writer_0(hot_properties[7].bytes, UINTPTR_MAX, UINTPTR_MAX);
    assert(take(api).hot[7].in.state == AZN_ABSENT);
    uintptr_t a = ((uintptr_t) hot_properties[0].bytes);
    put(a + 8, (unsigned) (integer(a + 8)) + 1); // nonsemantic native revision
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    hot = take(api);
    assert(!hot.lost_topics && hot.b == 1 && !hot.c);
    put(a + 8, (unsigned) (integer(a + 8)) + 1);
    point(a + 0x30, 240);
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    hot = take(api);
    assert(!hot.lost_topics && hot.b == 1 && hot.c == 1 && point_at(hot.hot[0].in, 240, 1));
    put(a + 8, (unsigned) (integer(a + 8)) + 2); // returning to the same value can hide an edit
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    hot = take(api);
    assert(hot.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    put(((uintptr_t) context.bytes) + 0x20, ((uintptr_t) replacement.bytes));
    put(((uintptr_t) replacement.bytes), p->cue_table);
    point(((uintptr_t) replacement.bytes) + 0x30, -1);
    point(((uintptr_t) replacement.bytes) + 0x88, -1);
    status_hot_capture(0, &((*slot)), 0, NULL, NULL);
    hot = take(api);
    assert(hot.attempt != provider && hot.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    Buffer_d0 status_facade = {0}, key_facade = {0}, function_set = {0}, setting_quantize = {0}, key = {0}, alien = {0};
    put(((uintptr_t) bundle.bytes) + 0x30, ((uintptr_t) status_facade.bytes));
    put(((uintptr_t) bundle.bytes) + 0x48, ((uintptr_t) key_facade.bytes));
    put(((uintptr_t) unit.bytes) + 0x188, ((uintptr_t) function_set.bytes));
    const SettingProfile *settings = &(setting_profiles[1]);
    put(((uintptr_t) status_facade.bytes), settings->state_facade);
    put(((uintptr_t) key_facade.bytes), settings->key_facade);
    p->quantize = ((uintptr_t) tables[4].bytes);
    put(((uintptr_t) setting_quantize.bytes), p->quantize);
    put(((uintptr_t) key.bytes), settings->key_function);
    put(((uintptr_t) function_set.bytes) + 0x20, ((uintptr_t) setting_quantize.bytes));
    put(((uintptr_t) status_facade.bytes) + 0x48, ((uintptr_t) setting_quantize.bytes));
    put(((uintptr_t) facade.bytes) + 0x50, ((uintptr_t) setting_quantize.bytes));
    put(((uintptr_t) status_facade.bytes) + 0x78, ((uintptr_t) key.bytes));
    put(((uintptr_t) key_facade.bytes) + 0x78, ((uintptr_t) key.bytes));
    setting_original[0] = ((void *) (native_callback_9));
    setting_original[1] = ((void *) (native_callback_8));
    atomic_store_explicit(&(setting_installed), 3, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS));
    status_settings_frame(0, &((*slot)));
    __auto_type qs = take(api);
    __auto_type ks = take(api);
    assert(qs.kind == AZN_QUANTIZE && qs.valid && !qs.a && ks.kind == AZN_KEY_LOCK && ks.valid && !ks.a);
    assert(qs.unit && ks.unit && qs.unit != ks.unit);
    setting_writer_0(((void *) (((uintptr_t) setting_quantize.bytes))), 1u);
    setting_writer_0(((void *) (((uintptr_t) setting_quantize.bytes))), 0u);
    assert(take(api).a == 1 && take(api).a == 0);
    setting_writer_1(((void *) (((uintptr_t) key.bytes) + 8)), 1u, 0u);
    assert(take(api).a == 1);
    key_fixture(api, &((*slot)), ((uintptr_t) key.bytes));
    setting_writer_0(((void *) (((uintptr_t) alien.bytes))), 1u);
    assert(alien.bytes[16] == 1 && !api->take(0, &rejected));
    put(((uintptr_t) key_facade.bytes) + 0x78, ((uintptr_t) alien.bytes));
    status_settings_frame(0, &((*slot)));
    assert(take(api).kind == AZN_QUANTIZE); // the new KEY subscription reseeded settings
    __auto_type retired_key = take(api);
    assert(retired_key.kind == AZN_KEY_LOCK && retired_key.valid < 0 && retired_key.unit == ks.unit);
    setting_writer_1(((void *) (((uintptr_t) key.bytes) + 8)), 0u, 0u);
    assert(!api->take(0, &rejected));
    Buffer_d0 sync_facade = {0};
    Buffer_220 sync_function = {0};
    put(((uintptr_t) bundle.bytes) + 0x60, ((uintptr_t) sync_facade.bytes));
    put(((uintptr_t) sync_facade.bytes), settings->sync_facade);
    put(((uintptr_t) sync_facade.bytes) + 0x68, ((uintptr_t) sync_function.bytes));
    put(((uintptr_t) status_facade.bytes) + 0x68, ((uintptr_t) sync_function.bytes));
    put(((uintptr_t) function_set.bytes) + 0x78, ((uintptr_t) sync_function.bytes));
    put(((uintptr_t) sync_function.bytes), settings->sync_function);
    setting_original[2] = ((void *) (native_callback_7));
    atomic_store_explicit(&(setting_installed), 7, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_SYNC));
    status_settings_frame(0, &((*slot)));
    __auto_type ss = take(api);
    if (ss.kind == AZN_QUANTIZE) ss = take(api); /* shared legacy interest reseeds the installed setting */
    assert(ss.kind == AZN_SYNC && ss.valid && !ss.a && !ss.b);
    setting_writer_2(sync_function.bytes, 2u);
    setting_writer_2(sync_function.bytes, 1u);
    ss = take(api);
    assert(ss.a == 1 && ss.b == 2);
    ss = take(api);
    assert(!ss.a && !ss.b);
    setting_writer_2(sync_function.bytes, 0u);
    ss = take(api);
    assert(ss.valid && !ss.a && ss.b == -1);
    atomic_store_explicit(&(setting_installed), 15, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    status_settings_frame(0, &((*slot)));
    __auto_type locked = take(api);
    while (locked.kind != AZN_SLIDER_LOCK) locked = take(api);
    assert(locked.valid && locked.a == 0);
    slider_lock_original[3] = ((void *) (native_callback_6));
    slider_lock_original[2] = ((void *) (native_callback_5));
    slider_lock_writer_2(sync_function.bytes);
    locked = take(api);
    assert(locked.kind == AZN_SLIDER_LOCK && locked.a == 1 && locked.valid);
    slider_lock_writer_3(sync_function.bytes);
    assert(!api->take(0, &rejected));
    put(((uintptr_t) function_set.bytes) + 0x78, ((uintptr_t) alien.bytes));
    status_settings_frame(0, &((*slot)));
    ss = take(api);
    assert(ss.kind == AZN_SYNC && ss.valid < 0);
    locked = take(api);
    assert(locked.kind == AZN_SLIDER_LOCK && locked.valid < 0);
    azn_raw_state(0, 17, 0);
    assert(api->capabilities(0) & AZN_CAP_PLAY_RAW);
    azn_raw_state(0, 17, 1);
    azn_raw_state(0, 17, 0);
    assert(!(api->capabilities(0) & AZN_CAP_PLAY_RAW));
    azn_raw_state(0, 18, 0);
    azn_raw_state(0, 17, 1);
    assert(api->capabilities(0) & AZN_CAP_PLAY_RAW);
    fixture_load.invalidation_epoch = 21;
    fixture_load.load_generation = 0;
    {
        StatusLanding landing __attribute__((cleanup(status_landing_drop)));
        status_landing_init(&landing, core.bytes);
        put(c + 0x20, (uint32_t) (2303));
        put(c + 0x28, (uint64_t) (372204));
        put(c + 0x68, (uint32_t) (44100));
        put(((uintptr_t) destination.bytes) + 0x10, (uint32_t) (2303));
        put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (372204));
        status_landing_finish(&landing, ((uintptr_t) destination.bytes));
    }
    status_stationary_seed(0, &((*slot)));
    assert(!api->take(0, &rejected));
    fixture_load.load_generation = 23;
    matched_id = 9;
    status_stationary_seed(0, &((*slot)));
    assert(!api->take(0, &rejected));
    matched_id = 2303;
    status_stationary_seed(0, &((*slot)));
    m = take(api);
    assert(m.a == AZN_MOTION_SEED && m.load_generation == 23 && m.motion_order == 1 &&
           point_at(m.points[1], 372204, m.observed_window_samples) && m.points[1].rate == 44100);
    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
    status_stationary_seed(0, &((*slot)));
    assert(!api->take(0, &rejected));
    {
        StatusLanding same __attribute__((cleanup(status_landing_drop)));
        status_landing_init(&same, core.bytes);
        status_landing_finish(&same, ((uintptr_t) destination.bytes));
    }
    status_stationary_seed(0, &((*slot)));
    assert(!api->take(0, &rejected));
    api->interest(0, motion_routes);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    put(((uintptr_t) context.bytes) + 8, (uint8_t) (0));
    Buffer_40 load_backcue = {0}, load_backcue_table = {0};
    const uintptr_t saved_backcue_table = p->commands[7].table;
    p->commands[7].table = ((uintptr_t) load_backcue_table.bytes);
    put(((uintptr_t) load_backcue.bytes), p->commands[7].table);
    put(((uintptr_t) load_backcue.bytes) + 0x38, ((uintptr_t) pause_f.bytes));
    for (unsigned mode = 0; mode < 12; ++mode) {
        const uint64_t first_sample = mode == 9 ? 0 : 13097;
        const bool expected_opening = mode == 0 || mode == 7 || mode == 9;
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
        matched_id = fixture_pending_id = 2400 + mode;
        fixture_pending_request = 100 + mode;
        atomic_store_explicit(&(slot->motion.landing_order), 1, memory_order_seq_cst);
        put(((uintptr_t) destination.bytes) + 0x10, matched_id);
        put(((uintptr_t) destination.bytes) + 0x18, first_sample);


#define fixture_adopt_12()                                                    \
    do {                                                                      \
        StatusLanding adoption __attribute__((cleanup(status_landing_drop))); \
        status_landing_init(&adoption, core.bytes);                           \
        put(c + 0x20, matched_id);                                            \
        put(c + 0x28, first_sample);                                          \
        put(c + 0x68, (uint32_t) (44100));                                    \
        status_landing_finish(&adoption, ((uintptr_t) destination.bytes));    \
    } while (0)


        if (mode >= 7 && mode <= 9) {
            put(((uintptr_t) load_backcue.bytes) + 0x30, ((uintptr_t) context.bytes) + (mode == 8 ? 16 : 0));
            STATUS_ACTION(loader, 0, ((uintptr_t) load_backcue.bytes), true);
            fixture_adopt_12();
        } else fixture_adopt_12();
        assert(atomic_load_explicit(&(slot->motion.landing_stamp), memory_order_seq_cst) == 2 && atomic_load_explicit(&(slot->motion.opening_landing), memory_order_seq_cst) == 2);
        if (mode == 1) {
            put(c + 0x28, (uint64_t) (14097));
            put(((uintptr_t) destination.bytes) + 0x18, (uint64_t) (14097));
        }
        if (mode == 2) {
            Buffer_20 render = {0};
            put(((uintptr_t) render.bytes) + 8, (int32_t) (64));
            void *saved = status_continuous_original;
            status_continuous_original = ((void *) (native_callback_4));
            status_continuous(core.bytes, render.bytes); // even a net-zero render breaks the copy chain
            status_continuous_original = saved;
            assert(!api->take(0, &rejected));
        }
        if (mode == 4) fixture_load.load_generation = 100 + mode; // a late same-point DJ seek
        if (mode == 5) ++fixture_pending_id;
        if (mode == 6) put(c + 0x68, (uint32_t) (48000));
        if (mode == 11) ++fixture_pending_request;
        if (mode == 3) {
            STATUS_ACTION(action, 0, 0, true);
            action.beat = true; // a real classified action, not the loader's controller plumbing
            StatusLanding copy __attribute__((cleanup(status_landing_drop)));
            status_landing_init(&copy, core.bytes);
            status_landing_finish(&copy, ((uintptr_t) destination.bytes));
        } else if (mode >= 7 && mode <= 9) {
            STATUS_ACTION(loader, 0, 0, true);
            StatusLanding copy __attribute__((cleanup(status_landing_drop)));
            status_landing_init(&copy, core.bytes);
            status_landing_finish(&copy, ((uintptr_t) destination.bytes));
        } else {
            StatusLanding copy __attribute__((cleanup(status_landing_drop)));
            status_landing_init(&copy, core.bytes);
            status_landing_finish(&copy, ((uintptr_t) destination.bytes));
        }
        if (mode >= 7 && mode <= 9) {
            STATUS_ACTION(loader, 0, 0, true);
            StatusLanding copy __attribute__((cleanup(status_landing_drop)));
            status_landing_init(&copy, core.bytes);
            status_landing_finish(&copy, ((uintptr_t) destination.bytes));
        } else {
            StatusLanding copy __attribute__((cleanup(status_landing_drop)));
            status_landing_init(&copy, core.bytes);
            status_landing_finish(&copy, ((uintptr_t) destination.bytes));
        }
        assert(atomic_load_explicit(&(slot->motion.landing_stamp), memory_order_seq_cst) == 4);
        assert(atomic_load_explicit(&(slot->motion.opening_landing), memory_order_seq_cst) == (expected_opening || mode == 10 ? 4u : 2u));
        if (mode == 10) put(c + 0x28, (uint64_t) (14097)); // movement after the last adoption copy
        if (!fixture_load.load_generation) {
            status_stationary_seed(0, &((*slot))); // owner frame began before completion
            assert(!api->take(0, &rejected));
        }
        fixture_load.load_generation = 100 + mode;
        fixture_pending_request = 0;
        if (mode == 7 || mode == 9) {
            put(c + 9, (uint8_t) (1));
            StatusMotionWrite first __attribute__((cleanup(status_motion_write_drop)));
            status_motion_write_init(&first, core.bytes, false, 64);
            m = take(api); // first matched writer seeds under its existing HEAD reservation
            assert(m.a == AZN_MOTION_SEED && m.motion_flags == AZN_AT_OPENING && m.motion_order == 1);
            status_motion_write_finish(&first, 0);
            m = take(api);
            assert(m.a == AZN_MEASURE && m.motion_order == 2 && !m.lost_topics && atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
        } else {
            status_stationary_seed(0, &((*slot)));
            m = take(api);
            assert(m.a == AZN_MOTION_SEED && m.motion_flags == (expected_opening ? (unsigned) (AZN_AT_OPENING) : 0u));
        }
        status_stationary_seed(0, &((*slot)));
        assert(!api->take(0, &rejected)); // late outer-frame seed cannot duplicate the first writer's seed

#undef fixture_adopt_12
    }
    p->commands[7].table = saved_backcue_table;
    puts("PASS pending native loader copies and first-writer zero/nonzero seed race; movement/action/source/owner/request/completion negatives");
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
    status_stationary_seed(0, &((*slot))); // old source remains at request entry
    put(c + 0x20, (uint32_t) (2304));
    put(c + 0x28, (uint64_t) (0));
    matched_id = 2304;
    status_stationary_seed(0, &((*slot)));
    assert(!api->take(0, &rejected));
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    put(((uintptr_t) context.bytes) + 8, (uint8_t) (0));
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    fixture_load.load_generation = 25;
    status_stationary_seed(0, &((*slot)));
    m = take(api);
    assert(m.a == AZN_MOTION_SEED && point_at(m.points[1], 0, m.observed_window_samples) && m.load_generation == 25 && m.motion_flags == AZN_AT_OPENING);
    put(c + 9, (uint8_t) (1)); /* active engine rendering a paused zero-speed source */
    fixture_travel_9(0);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    fixture_travel_9(0);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    atomic_store_explicit(&(map_clocks[0].load), fixture_load.load_generation, memory_order_seq_cst);
    atomic_store_explicit(&(map_clocks[0].serial), 2, memory_order_seq_cst);
    atomic_store_explicit(&(map_mutations[0].active), 1, memory_order_seq_cst);
    fixture_travel_9(0);
    m = take(api);
    assert(m.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) && atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    atomic_store_explicit(&(map_mutations[0].active), 0, memory_order_seq_cst);
    azstatus_native_receipt initial_map = {0};
    initial_map.kind = AZN_MAPS;
    initial_map.load_generation = fixture_load.load_generation;
    initial_map.map.changed = AZNM_PAIR;
    assert(offer_map_cut(&initial_map));
    m = take(api);
    assert(m.kind == AZN_MAPS && m.motion_flags == AZN_AT_OPENING && m.observed_window_samples == 64);
    const uint64_t opening_cut = slot->motion.order;
    atomic_store_explicit(&(transport_installed), 15, memory_order_seq_cst);
    transport_observe(0, &((*slot)), TransportSeed, NULL);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (1));
    transport_observe(0, &((*slot)), TransportPlay, NULL);
    assert(take(api).a == AZ_PLAYBACK_PLAYING);
    m = take(api);
    assert(m.a == AZN_MEASURE && m.motion_flags == AZN_BEGAN_PLAY && point_at(m.points[1], 0, m.observed_window_samples) && m.motion_order == opening_cut + 1);
    transport_observe(0, &((*slot)), TransportPlay, NULL);
    status_opening_play(0, &((*slot)), fixture_load);
    assert(!api->take(0, &rejected));
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
    status_stationary_seed(0, &((*slot)));
    matched_id = 2305;
    put(c + 0x20, matched_id);
    put(((uintptr_t) pause_f.bytes) + 8, (uint8_t) (0));
    fixture_load.load_generation = 26;
    status_stationary_seed(0, &((*slot)));
    assert(take(api).motion_flags == AZN_AT_OPENING);
    fixture_travel_9(31);
    take(api);
    assert(atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    fixture_travel_9(63);
    take(api);
    assert(atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    fixture_travel_9(64);
    take(api);
    assert(!atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    fixture_travel_9(0);
    take(api);
    assert(!atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
    put(c + 9, (uint8_t) (0));
    puts("PASS owned opening at zero, single confirmed PLAY edge and repeated-play suppression");
    atomic_store_explicit(&(transport_installed), 0, memory_order_seq_cst);
    puts("PASS paused source adoption, matched TrackID, stationary sample and same-track replacement exclusion");
    Buffer_100 readable_rate = {0}, manager_rate = {0}, mode_rate = {0}, engine_rate = {0};
    const RateProfile *rp = &(rate_profiles[1]);
    double samples[3] = {-1, -1, -1};
    put(c + 0xa8, ((uintptr_t) readable_rate.bytes));
    put(((uintptr_t) readable_rate.bytes), rp->readable);
    put(((uintptr_t) readable_rate.bytes) + 0x18, ((uintptr_t) manager_rate.bytes));
    put(((uintptr_t) manager_rate.bytes), rp->manager);
    put(((uintptr_t) manager_rate.bytes) + 0x18, ((uintptr_t) mode_rate.bytes));
    put(((uintptr_t) mode_rate.bytes), rp->mode);
    put(((uintptr_t) manager_rate.bytes) + 0x38, ((uintptr_t) engine_rate.bytes));
    put(((uintptr_t) engine_rate.bytes), rp->pitch);
    put(((uintptr_t) engine_rate.bytes) + 0x18, ((uintptr_t) (samples)));
    put(((uintptr_t) engine_rate.bytes) + 0x20, (uint64_t) (3));
    put(((uintptr_t) engine_rate.bytes) + 0x10, -1.0);
    atomic_store_explicit(&(rate_hooks), 15, memory_order_seq_cst);
    atomic_store_explicit(&(rate_generator_hooked), true, memory_order_seq_cst);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    rate_capture(0, &((*slot)), 0, 0);
    __auto_type rate = take(api);
    assert(rate.kind == AZN_RATE && rate.valid == AZ_AVAIL_READY && rate.a == 0);
    put(c + 9, (uint8_t) (1));
    rate_capture(0, &((*slot)), 0, 3);
    rate = take(api);
    double magnitude;
    memcpy(&magnitude, &rate.points[0].samples, 8);
    assert(rate.a == -1 && rate.valid == AZ_AVAIL_READY && magnitude == 1);
    samples[0] = -0.5;
    rate_capture(0, &((*slot)), 0, 3);
    assert(!api->take(0, &rejected)); /* acceleration kept the same current endpoint */
    samples[1] = -0.625;
    samples[2] = -0.75;
    put(((uintptr_t) engine_rate.bytes) + 0x10, -0.75);
    rate_capture(0, &((*slot)), 0, 3);
    rate = take(api);
    memcpy(&magnitude, &rate.points[0].samples, 8);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_READY && rate.a == -1 && magnitude == 0.75 && !rate.lost_topics);
    samples[1] = 0.5; /* an actual opposite-direction excursion is still lost history */
    rate_capture(0, &((*slot)), 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_STALE && rate.lost_topics);
    rate_capture(0, &((*slot)), 0, 0);
    assert(!api->take(0, &rejected));
    samples[1] = -0.625;
    rate_capture(0, &((*slot)), 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_READY && !rate.lost_topics);
    samples[2] = -0.5; /* stale/mismatched buffer cannot qualify the scalar */
    rate_capture(0, &((*slot)), 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_STALE && rate.b == AZ_AVAIL_STALE && rate.lost_topics);
    samples[2] = -0.75;
    rate_capture(0, &((*slot)), 0, 3);
    assert(take(api).valid == AZ_AVAIL_READY);
    samples[0] = 0; /* a zero before reverse acceleration is not a reversal */
    rate_capture(0, &((*slot)), 0, 3);
    assert(!api->take(0, &rejected));
    put(c + 9, (uint8_t) (0));
    rate_capture(0, &((*slot)), 0, 0);
    assert(take(api).a == 0);
    /* A buffering seek commits speed=1 before its first generated block.
     * The old paused vector stays zero and is not fresh render evidence. */
    fill_samples(samples, samples + 3, 0.0);
    put(((uintptr_t) engine_rate.bytes) + 0x10, 1.0);
    put(c + 9, (uint8_t) (1));
    rate_capture(0, &((*slot)), 0, 0);
    assert(take(api).valid == AZ_AVAIL_READY);
    {
        RateRenderScope block __attribute__((cleanup(rate_render_drop)));
        rate_render_init(&block, core.bytes);
        assert(rate_render_measured_frames(&block, 3) == 0);
        rate_capture(0, &((*slot)), 0, rate_render_measured_frames(&block, 3));
        assert(!api->take(0, &rejected));
    }
    {
        RateRenderScope block __attribute__((cleanup(rate_render_drop)));
        rate_render_init(&block, core.bytes);
        rate_render_generated(((uintptr_t) engine_rate.bytes), 0);
        assert(rate_render_measured_frames(&block, 3) == 3);
        rate_capture(0, &((*slot)), 0, rate_render_measured_frames(&block, 3));
        __auto_type invalid = take(api);
        assert(invalid.valid == AZ_AVAIL_STALE && invalid.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    }
    fill_samples(samples, samples + 3, 1.0);
    {
        RateRenderScope block __attribute__((cleanup(rate_render_drop)));
        rate_render_init(&block, core.bytes);
        rate_render_generated(((uintptr_t) engine_rate.bytes), 0);
        rate_capture(0, &((*slot)), 0, rate_render_measured_frames(&block, 3));
        assert(take(api).valid == AZ_AVAIL_READY);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        RateRenderScope block __attribute__((cleanup(rate_render_drop)));
        rate_render_init(&block, core.bytes);
        switch (mode) {
        case 0: ++fixture_load.unit_generation; break;
        case 1: put(c + 0x20, matched_id + 1); break;
        case 2: put(((uintptr_t) manager_rate.bytes) + 0x38, (uintptr_t) (0)); break;
        case 3: {
            RateRenderScope nested __attribute__((cleanup(rate_render_drop)));
            rate_render_init(&nested, ((void *) (c + 8)));
            break;
        }
        case 4: rate_render_generated(((uintptr_t) engine_rate.bytes) + 8, 0); break;
        case 5:
            put(((uintptr_t) engine_rate.bytes), rp->scratch);
            put(((uintptr_t) engine_rate.bytes) + 0x58, (uint8_t) (1));
            break;
        }
        assert(rate_render_measured_frames(&block, 3) == 3);
        if (mode == 0) --fixture_load.unit_generation;
        if (mode == 1) put(c + 0x20, matched_id);
        if (mode == 2) put(((uintptr_t) manager_rate.bytes) + 0x38, ((uintptr_t) engine_rate.bytes));
        if (mode == 5) {
            put(((uintptr_t) engine_rate.bytes), rp->pitch);
            put(((uintptr_t) engine_rate.bytes) + 0x58, (uint8_t) (0));
        }
    }
    atomic_store_explicit(&(rate_hooks), 0, memory_order_seq_cst);
    atomic_store_explicit(&(rate_generator_hooked), false, memory_order_seq_cst);
    puts("PASS source-qualified no-render rate capture, real generator validation and owner/mode negatives");
    __auto_type active_serial = atomic_load_explicit(&(slot->motion.active_serial), memory_order_seq_cst);
    assert(!atomic_load_explicit(&(slot->motion.active_writers), memory_order_seq_cst));
    rate_original[0] = ((void *) (native_callback_3));
    rate_original[1] = ((void *) (native_callback_2));
    rate_writer_0(((void *) (c)), 1);
    rate_writer_1(((void *) (c)));
    assert(!atomic_load_explicit(&(slot->motion.active_writers), memory_order_seq_cst) && atomic_load_explicit(&(slot->motion.active_serial), memory_order_seq_cst) == active_serial + 4);
    rate_original[0] = ((void *) (native_callback_1));
    unsigned deleted = fixture_exception_deletions;
    assert(!az_native_try(failing_rate_writer, (void *) c));
    assert(fixture_exception_deletions == deleted + 1 && !atomic_load_explicit(&(slot->motion.active_writers), memory_order_seq_cst) && atomic_load_explicit(&(slot->motion.active_serial), memory_order_seq_cst) == active_serial + 6);
    rate_original[0] = rate_original[1] = NULL;
    puts("PASS actual start/stop writer depth, serial and exceptional release");
    slip_fixture(api, &((*slot)), ((uintptr_t) function_set.bytes));
    /* Match the stock unit's final audible frame, then its independently
     * committed terminal decision. The same-source length has native units. */
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    put(c + 0x68, (uint32_t) (44100));
    slot->motion.load = fixture_load.load_generation;
    slot->motion.known = true;
    slot->motion.rate = 44100;
    slot->motion.generation = 1;
    slot->motion.order = 10;
    slot->motion.previous = 990;
    Buffer_1000 payload = {0};
    put(((uintptr_t) payload.bytes) + 0x30, matched_id);
    put(((uintptr_t) payload.bytes) + 0x38, (uint64_t) (990));
    put(((uintptr_t) payload.bytes) + 0x15, (uint8_t) (1));
    EndRead frame = {0};
    frame.d = 0;
    frame.core = c;
    frame.load = fixture_load;
    frame.length = 1000;
    frame.rate = 44100;
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    azstatus_native_receipt arriving = {0};
    arriving.kind = AZN_MOTION;
    arriving.a = AZN_MEASURE;
    arriving.load_generation = fixture_load.load_generation;
    arriving.epoch = fixture_load.invalidation_epoch;
    arriving.points[0] = (__typeof__(arriving.points[0])) {AZN_COORD, 44100, 990};
    arriving.points[1] = (__typeof__(arriving.points[1])) {AZN_COORD, 44100, 999};
    arriving.from_window_samples = arriving.observed_window_samples = 32;
    arriving.from_window_rate = arriving.observed_window_rate = 44100;
    arriving.motion_generation = slot->motion.generation;
    arriving.motion_order = ++slot->motion.order;
    end_mark_motion(0, &(arriving));
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && point_at(arriving.points[1], 999, 1));
    arriving.points[0] = arriving.points[1];
    arriving.points[1].samples = 1027;
    arriving.motion_order = ++slot->motion.order;
    slot->motion.previous = 1027;
    end_mark_motion(0, &(arriving));
    assert(!atomic_load_explicit(&(end_state[0].refresh), memory_order_seq_cst));
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && point_at(arriving.points[2], 1000, 1));
    put(c + 0x28, (uint64_t) (1000));
    memcpy(slot->motion.identity, ((const void *) (c + 0x18)), 12);
    put(((uintptr_t) payload.bytes) + 0x38, (uint64_t) (1027));
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    end_accept_clamp(&(frame), 1026, c + 0x10);
    assert(compare_timestamps(slot->motion.previous, 1027, frame.rate, frame.rate, 1) == AZ_TIME_EQUAL);
    end_accept_clamp(&(frame), 1027, c + 0x10);
    assert(compare_timestamps(slot->motion.previous, 1000, frame.rate, frame.rate, 1) == AZ_TIME_EQUAL);
    put(((uintptr_t) payload.bytes) + 0x38, (uint64_t) (1000));
    put(((uintptr_t) payload.bytes) + 0x14, (uint8_t) (1));
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    __auto_type ended = take(api);
    assert(ended.kind == AZN_END && ended.a == 1 && ended.motion_order == 13 &&
           ended.motion_flags == (AZN_TERMINAL_CONFIRMED | AZN_TERMINAL_CONTINUATION));
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    assert(!api->take(0, &rejected));
    /* Real EOF render blocks can continue at the exact clamped endpoint.
     * They must not start a new arrival/confirmation cycle each time. */
    for (unsigned repeat = 0; repeat < 4; ++repeat) {
        arriving.motion_flags = 0;
        arriving.points[0] = arriving.points[1] = (__typeof__(arriving.points[1])) {AZN_COORD, 44100, 1000};
        arriving.motion_order = ++slot->motion.order;
        end_mark_motion(0, &(arriving));
        assert(!(arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && !end_state[0].pending_order);
        end_commit(&(frame), ((uintptr_t) payload.bytes));
        assert(!api->take(0, &rejected));
    }
    arriving.motion_flags = 0;
    arriving.map.owner = 99; /* a replacement map needs its own terminal cut */
    arriving.motion_order = ++slot->motion.order;
    end_mark_motion(0, &(arriving));
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && end_state[0].pending_order == arriving.motion_order);
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    ended = take(api);
    assert(ended.kind == AZN_END && ended.a == 1);
    put(((uintptr_t) payload.bytes) + 0x16, (uint8_t) (1));
    end_commit(&(frame), ((uintptr_t) payload.bytes));
    ended = take(api);
    assert(ended.kind == AZN_END && !ended.a && ended.motion_order == 20);
    /* The map holds the same production reservation used by offer_map_cut.
     * The native inactive branch has no head store. Every negative control
     * reports loss, including an active-latch excursion inside the window. */
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    atomic_store_explicit(&(rate_hooks), 3, memory_order_seq_cst);
    for (unsigned mode = 0; mode < 7; ++mode) {
        put(c + 9, (uint8_t) (mode == 3));
        put(c + 0x20, matched_id);
        put(c + 0x28, (uint64_t) (1000));
        atomic_store_explicit(&(slot->motion.busy), mode == 4 ? MOTION_HEAD : MOTION_MAP, memory_order_seq_cst);
        uint64_t interference = atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst);
        {
            StatusMotionWrite overlapping __attribute__((cleanup(status_motion_write_drop)));
            status_motion_write_init(&overlapping, core.bytes, false, mode == 2 ? 0 : 64);
            if (mode == 1) {
                atomic_fetch_add_explicit(&(slot->motion.active_serial), 2, memory_order_seq_cst);
                put(c + 0x28, (uint64_t) (1001));
            }
            if (mode == 5) put(c + 0x20, matched_id + 1);
            if (mode == 6) atomic_fetch_add_explicit(&(slot->motion.interference), 1, memory_order_seq_cst);
            status_motion_write_finish(&overlapping, 0);
        }
        atomic_store_explicit(&(slot->motion.busy), MOTION_FREE, memory_order_seq_cst);
        if (!mode) {
            assert(atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst) == interference);
            assert(!api->take(0, &rejected));
        } else {
            __auto_type gap = take(api);
            assert(gap.kind == AZN_GAP && (gap.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
            assert(atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst) > interference);
        }
    }
    puts("PASS native inactive map reservation, active-latch excursion and competing writer loss");
    /* Active Core+9 does not imply source travel. Only a completed, owned
     * zero-advance block may preserve the opening across the MAP reservation.
     * A net-zero excursion and a sub-sample advance are both rejected. */
    double stationary_samples[64] = {0};
    for (unsigned mode = 0; mode < 13; ++mode) {
        fill_samples(stationary_samples, stationary_samples + 64, 0.0);
        put(c + 9, (uint8_t) (1));
        put(c + 0x20, matched_id);
        put(c + 0x28, (uint64_t) (1000));
        put(c + 0x68, (uint32_t) (44100));
        put(((uintptr_t) engine_rate.bytes), rp->pitch);
        put(((uintptr_t) manager_rate.bytes) + 0x38, ((uintptr_t) engine_rate.bytes));
        put(((uintptr_t) engine_rate.bytes) + 0x18, ((uintptr_t) (stationary_samples)));
        put(((uintptr_t) engine_rate.bytes) + 0x20, (uint64_t) (64));
        atomic_store_explicit(&(slot->motion.opening), true, memory_order_seq_cst);
        atomic_store_explicit(&(slot->motion.busy), MOTION_MAP, memory_order_seq_cst);
        const uint64_t interference = atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst);
        {
            StatusMotionWrite overlapping __attribute__((cleanup(status_motion_write_drop)));
            status_motion_write_init(&overlapping, core.bytes, false, 64);
            switch (mode) {
            case 1: stationary_samples[0] = 0.125; break;
            case 2:
                stationary_samples[0] = 1;
                stationary_samples[1] = -1;
                break;
            case 3: put(c + 0x28, (uint64_t) (1001)); break;
            case 4: put(c + 0x20, matched_id + 1); break;
            case 5: put(c + 0x68, (uint32_t) (48000)); break;
            case 6: atomic_fetch_add_explicit(&(slot->motion.active_serial), 2, memory_order_seq_cst); break;
            case 7: atomic_fetch_add_explicit(&(slot->motion.landing_order), 1, memory_order_seq_cst); break;
            case 8: put(((uintptr_t) engine_rate.bytes), (uintptr_t) (0)); break;
            case 9: put(((uintptr_t) engine_rate.bytes) + 0x20, (uint64_t) (63)); break;
            case 10: put(((uintptr_t) manager_rate.bytes) + 0x38, (uintptr_t) (0)); break;
            case 11: put(c + 9, (uint8_t) (0)); break;
            }
            if (mode != 12) status_motion_write_finish(&overlapping, 0);
        }
        atomic_store_explicit(&(slot->motion.busy), MOTION_FREE, memory_order_seq_cst);
        if (!mode) {
            assert(atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst) == interference && atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
            assert(!api->take(0, &rejected));
        } else {
            __auto_type gap = take(api);
            assert(gap.kind == AZN_GAP && (gap.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
            assert(atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst) > interference && !atomic_load_explicit(&(slot->motion.opening), memory_order_seq_cst));
        }
    }
    puts("PASS stationary active map overlap, source/owner fences and moving-block loss");
    put(c + 9, (uint8_t) (1));
    put(c + 0x20, matched_id);
    put(c + 0x28, (uint64_t) (0));
    put(c + 0x68, (uint32_t) (44100));
    put(((uintptr_t) engine_rate.bytes), rp->pitch);
    put(((uintptr_t) manager_rate.bytes) + 0x38, ((uintptr_t) engine_rate.bytes));
    put(((uintptr_t) engine_rate.bytes) + 0x18, ((uintptr_t) (stationary_samples)));
    put(((uintptr_t) engine_rate.bytes) + 0x20, (uint64_t) (64));
    slot->motion.load = fixture_load.load_generation;
    slot->motion.known = true;
    slot->motion.previous = slot->motion.opening_sample = 0;
    slot->motion.rate = slot->motion.opening_rate = slot->motion.window_rate = 44100;
    slot->motion.window_samples = 64;
    slot->motion.captured_interference = atomic_load_explicit(&(slot->motion.interference), memory_order_seq_cst);
    slot->motion.seed = azn_seed_id(0);
    memcpy(slot->motion.identity, ((const void *) (c + 0x18)), 12);
    atomic_store_explicit(&(slot->motion.opening), true, memory_order_seq_cst);
    azstatus_native_receipt overlapped_map = {0};
    overlapped_map.kind = AZN_MAPS;
    overlapped_map.load_generation = fixture_load.load_generation;
    overlapped_map.map.owner = 77;
    overlapped_map.map.changed = AZNM_PAIR;
    overlapped_map.map.grid.provider = overlapped_map.map.phrases.provider = 78;
    overlapped_map.map.grid.revision = overlapped_map.map.phrases.revision = 1;
    overlapped_map.map.grid.content = overlapped_map.map.phrases.content = 1;
    atomic_store_explicit(&(slot->motion.busy), MOTION_HEAD, memory_order_seq_cst);
    assert(offer_map_cut(&overlapped_map));
    atomic_store_explicit(&(slot->motion.busy), MOTION_FREE, memory_order_seq_cst);
    m = take(api);
    assert(m.kind == AZN_MAPS && !m.motion_order && !m.motion_flags);
    atomic_store_explicit(&(map_mutations[0].active), 1, memory_order_seq_cst);
    for (unsigned mode = 0; mode < 3; ++mode) {
        fill_samples(stationary_samples, stationary_samples + 64, 0.0);
        if (mode == 1) stationary_samples[0] = 0.125;
        if (mode == 2) {
            stationary_samples[0] = 1;
            stationary_samples[1] = -1;
        }
        fixture_travel_9(0);
        m = take(api);
        if (m.a != AZN_MEASURE || m.lost_topics != (mode ? AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) : 0))
            fprintf(stderr, "opening map mode=%u kind=%d action=%d lost=%llx flags=%u\n", mode, m.kind, m.a,
                    (unsigned long long) m.lost_topics, m.motion_flags);
        assert(m.a == AZN_MEASURE && m.lost_topics == (mode ? AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) : 0));
        assert((bool) (m.motion_flags & AZN_AT_OPENING) == (mode == 0));
    }
    /* A qualified internal render can clear an unreadable block without
     * generating a new advance vector. Its previous nonzero vector is stale. */
    fill_samples(stationary_samples, stationary_samples + 64, 1.0);
    atomic_store_explicit(&(rate_generator_hooked), true, memory_order_seq_cst);
    {
        RateRenderScope block __attribute__((cleanup(rate_render_drop)));
        rate_render_init(&block, core.bytes);
        assert(rate_render_stationary(c, 64));
        fixture_travel_9(0);
    }
    atomic_store_explicit(&(rate_generator_hooked), false, memory_order_seq_cst);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && (m.motion_flags & AZN_AT_OPENING));
    atomic_store_explicit(&(map_mutations[0].active), 0, memory_order_seq_cst);
    puts("PASS opening recovery after paired map overlaps HEAD; nonzero and net-zero movement do not recover opening");
    puts("PASS source-qualified terminal arrival, compiler-checked payload gates and cue withdrawal");
    puts("PASS background slip clock, source/rate fence, zero, motion, release and retirement");
    puts("PASS measured current transport rate through ramps, reverse and corrupt-block recovery");
    puts("PASS matched unit setting writers, A/B/A, independent owner retirement and raw enrollment");
    puts("PASS owned hot bank, H at zero, simultaneous A/H, clear, container identity and scoped loss");
    puts("PASS stock typed loop traversal and cue/beat jump, repeated ordinal and same-boundary negative control");
    puts("PASS native position source, stationary load fencing, zero landing, same-load barrier and delayed writer exclusion");
    puts("PASS native status owner, separate retained/working transitions, epoch, scoped revision loss and overflow");
    test_stationary_measurements(api, &((*slot)), c);
    test_direct_loop_actions(api, &((*slot)), c, active, ((uintptr_t) basis.bytes), ((uintptr_t) pause_f.bytes), word(slot->facade + 0x50), ((uintptr_t) direct_controller.bytes));
#ifdef AZSDK_MAP_ALIAS_TESTING
    test_gui_map_alias(api, &((*slot)), c);
#endif
    test_native_duration(api, &((*slot)), c);

#undef fixture_residency_8

#undef fixture_travel_9

#undef fixture_interval_10

#undef fixture_land_11
}

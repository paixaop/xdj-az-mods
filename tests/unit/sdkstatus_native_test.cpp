/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the same owner capture, native writer trampolines and receipt lane used on EP147. */
#include "../../src/azstem/sdk_exit.cpp"
#include <cassert>
#include <cstdio>
#include <poll.h>
#include <thread>
static std::atomic<bool> pause_next {false}, parked {false}, resume_writer {false};
extern "C" void azn_test_reserved(unsigned, unsigned kind) {
    if (kind == AZN_LOOP && pause_next.exchange(false)) {
        parked.store(true);
        while (!resume_writer.load()) std::this_thread::yield();
    }
}
extern "C" const azstatus_native_v1 *azstem_status_service_v1();
static bool inject_scan = false;
extern "C" void azn_test_scan(unsigned deck, unsigned lane) {
    if (deck != 3 || lane != 4 || !inject_scan) return;
    inject_scan = false;
    azstatus_native_receipt motion {}, end {};
    motion.deck = end.deck = deck;
    motion.kind = AZN_MOTION;
    motion.motion_order = 2;
    end.kind = AZN_END;
    end.motion_order = 3;
    assert(azn_offer(&motion) && azn_offer(&end));
}
static void test_concurrent_scan() {
    const auto *api = azstem_status_service_v1();
    azstatus_native_receipt receipt {};
    receipt.size = sizeof receipt;
    receipt.version = 1;
    inject_scan = true;
    assert(api->take(3, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == 2);
    assert(api->take(3, &receipt) && receipt.kind == AZN_END && receipt.motion_order == 3);
    assert(!api->take(3, &receipt));
}
extern "C" int azn_register_source_capture(azn_source_capture_fn) { return 0; }
extern "C" int azn_sync_seed(unsigned, uintptr_t, int *raw) {
    *raw = 0;
    return 1;
}
extern "C" unsigned azn_maps_install(int, uintptr_t, azn_setting_patch_fn) { return 0; }
extern "C" int azn_maps_current(unsigned, uint64_t, const aznm_packet *) { return 1; }
extern "C" uint64_t azn_load_source_pin(unsigned, uint64_t, uint64_t) { return 0; }
extern "C" unsigned azn_load_kind(unsigned, uint64_t, uint64_t) { return 0; }
extern "C" unsigned azn_load_original_bpm(unsigned, uint64_t, uint64_t, uint64_t, double *) { return AZN_BPM_UNKNOWN; }
extern "C" int azn_empty_seed(unsigned, azstatus_native_receipt *) { return 0; }
extern "C" int azn_metadata_seed(unsigned, azstatus_native_receipt *) { return 0; }
static azsdk_load_sample fixture_load = {5, 7, 11, 3, 2, 0};
static uint32_t matched_id = 3;
static bool fixture_load_busy = false;
static void (*fixture_track_match_hook)() = nullptr;
static void (*fixture_after_track_match_hook)() = nullptr;
static uint64_t fixture_pending_request = 0;
static uint32_t fixture_pending_id = 0;
extern "C" uint64_t azn_load_pending(unsigned d, uint64_t e, const unsigned char source[12]) {
    if (d || e != fixture_load.invalidation_epoch || fixture_load.load_generation) return 0;
    if (source) {
        uint32_t id; memcpy(&id, source + 8, sizeof id);
        if (id != fixture_pending_id) return 0;
    }
    return fixture_pending_request;
}
extern "C" uint64_t azn_lifecycle_fence(unsigned d) { return !d ? fixture_load.invalidation_epoch : 0; }
extern "C" int azn_load_history_revoked(unsigned d, uint64_t epoch) {
    return d == 0 && epoch && fixture_load.invalidation_epoch > epoch && !fixture_load.load_generation;
}
extern "C" int azn_load_track_matches(unsigned d, uint64_t e, uint64_t load, const unsigned char id[12]) {
    if (fixture_track_match_hook) {
        auto hook = fixture_track_match_hook;
        fixture_track_match_hook = nullptr;
        hook();
    }
    uint32_t n;
    memcpy(&n, id + 8, 4);
    const bool matched = !d && e == fixture_load.invalidation_epoch && load == fixture_load.load_generation && n == matched_id;
    if (matched && fixture_after_track_match_hook) {
        auto hook = fixture_after_track_match_hook;
        fixture_after_track_match_hook = nullptr;
        hook();
    }
    return matched;
}
extern "C" int azsdk_load_read(int deck, azsdk_load_sample *out) noexcept {
    if (deck || !fixture_load.load_generation || fixture_load_busy) return 0;
    *out = fixture_load;
    return 1;
}

template <size_t N>
struct alignas(16) Buffer {
    unsigned char bytes[N] = {};
    uintptr_t addr() { return reinterpret_cast<uintptr_t>(bytes); }
};
template <class T>
static void put(uintptr_t p, T value) {
    memcpy(reinterpret_cast<void *>(p), &value, sizeof value);
}
static bool point_at(const azn_point &point, int64_t expected, unsigned window = 1) {
    const int order = compare_timestamps(point.samples, expected, point.rate, point.rate, window);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static void test_motion_pressure() {
    const auto *api = azstem_status_service_v1();
    constexpr unsigned rate = 44100, window = 64;
    constexpr unsigned frames = 1162; // observed 1.30 tail backlog, about 1.69 source seconds
    constexpr auto routes = AZ_STATUS_TOPIC(AZ_STATUS_POSITION) |
                            AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    api->interest(0, routes);
    auto offer = [](unsigned i) {
        azstatus_native_receipt r {};
        r.kind = AZN_MOTION;
        r.a = AZN_MEASURE;
        r.motion_order = i;
        r.motion_generation = 1;
        r.observed_window_rate = rate;
        r.observed_window_samples = window;
        r.points[1] = {AZN_COORD, rate, i * window};
        return azn_offer(&r);
    };
    unsigned accepted = 0;
    for (unsigned i = 1; i <= frames; ++i) accepted += offer(i);
    assert(accepted && accepted < frames);
    azstatus_native_receipt receipt {};
    receipt.size = sizeof receipt;
    receipt.version = 1;
    for (unsigned i = 1; i <= accepted; ++i) {
        assert(api->take(0, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == i);
        assert(point_at(receipt.points[1], i * window, 1));
    }
    assert(api->take(0, &receipt) && receipt.kind == AZN_GAP && receipt.b == AZN_MOTION &&
           receipt.lost_topics == routes);
    assert(!api->take(0, &receipt));
    assert(offer(frames + 1));
    assert(api->take(0, &receipt) && receipt.kind == AZN_MOTION && receipt.motion_order == frames + 1);
    printf("PASS motion queue retains %u measured blocks, reports scoped loss under pressure and admits recovery\n", accepted);
}
static void point(uintptr_t p, int64_t samples) {
    memset(reinterpret_cast<void *>(p), 0, 64);
    if (samples < 0) {
        put(p + 16, INT64_MAX);
        put(p + 48, INT32_MAX);
        put(p + 56, INT64_MAX);
        return;
    }
    put(p, uint8_t(1));
    put(p + 4, uint8_t(1));
    put(p + 6, uint8_t(1));
    put(p + 8, uint32_t(3));
    put(p + 16, samples);
    put(p + 32, uintptr_t(0x10000));
    put(p + 48, uint32_t(48000));
    put(p + 56, int64_t(samples * 1000000 / 48000));
}
static void copy(void *self, uintptr_t in, uintptr_t out) {
    uintptr_t p = reinterpret_cast<uintptr_t>(self);
    point(p + 0x30, int64_t(in));
    point(p + 0x88, int64_t(out));
    put(p + 8, uint32_t(integer(p + 8)) + 1u);
}
static azstatus_native_receipt take(const azstatus_native_v1 *api) {
    azstatus_native_receipt r {};
    r.size = sizeof r;
    r.version = 1;
    assert(api->take(0, &r) == 1);
    return r;
}
static uintptr_t noq_core, noq_target, noq_return;
static bool fake_noqua(void *, int, uint32_t, const void *) {
    StatusActionScope direct(0, 0, true);
    status_noqua_request(direct, reinterpret_cast<const void *>(noq_target), 2, noq_return);
    StatusMotionWrite move(reinterpret_cast<void *>(noq_core), true, 1);
    put(noq_core + 0x28, word(noq_target + 0x18));
    move.finish(noq_target);
    return true;
}
static bool fake_grid_jump(void *self, int direction, uint32_t beat, const void *ref, bool option) {
    assert(option);
    return fake_noqua(self, direction, beat, ref);
}
static void key_fixture(const azstatus_native_v1 *api, Slot &slot, uintptr_t key) {
    constexpr uint64_t zero = UINT64_C(0x3ff0000000000000), up = UINT64_C(0x3ff0f38f92d97963),
                       down = UINT64_C(0x3fee3437e7101347);
    assert(key_semitones(zero) == 0 && key_semitones(up) == 1 && key_semitones(down) == -1);
    assert(key_semitones(0x4000000000000000) == 12 && key_semitones(0x3fe0000000000000) == -12);
    assert(key_semitones(0) == INT32_MAX && key_semitones(UINT64_MAX) == INT32_MAX);
    key_hooked.store(1);
    put(key + 0x20, zero);
    key_original = reinterpret_cast<void *>(+[](void *self, int32_t step, unsigned) {
        if (step == -1 || step == 1)
            put(reinterpret_cast<uintptr_t>(self) + 8, step == -1 ? UINT64_C(0x3fee3437e7101347) : UINT64_C(0x3ff0f38f92d97963));
    });
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_KEY));
    key_frame(0, slot);
    auto r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.valid && r.a == 0 && r.attempt && r.load_generation == fixture_load.load_generation);
    void *self = reinterpret_cast<void *>(key + 0x18);
    key_writer(self, 1, 0);
    assert(take(api).a == 1);
    key_writer(self, 13, 0);
    key_writer(self, 1, 0);
    azstatus_native_receipt none = {};
    none.size = sizeof none;
    none.version = 1;
    assert(!api->take(0, &none));
    key_writer(self, -1, 0);
    assert(take(api).a == -1);
    setting_original[1] = reinterpret_cast<void *>(+[](void *self, unsigned on, unsigned) {
        put(reinterpret_cast<uintptr_t>(self) + 8, static_cast<uint8_t>(on));
        put(reinterpret_cast<uintptr_t>(self) + 24, UINT64_C(0x3ff0000000000000));
    });
    setting_writer<1>(reinterpret_cast<void *>(key + 8), 0u, 0u);
    auto reset = take(api);
    assert(reset.kind == AZN_KEY_SHIFT && reset.a == 0);
    assert(take(api).kind == AZN_KEY_LOCK);
    {
        KeyCapture pending(key + 0x18);
        ++fixture_load.load_generation;
        pending.finish();
    }
    assert(!api->take(0, &none));
    key_frame(0, slot);
    r = take(api);
    assert(r.valid && !r.a && r.load_generation == fixture_load.load_generation);
    {
        KeyCapture concurrent(key + 0x18);
        key_writer(self, 1, 0);
        concurrent.finish();
    }
    r = take(api);
    assert(r.kind == AZN_GAP && r.b == AZN_KEY_SHIFT && r.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_KEY));
    key_frame(0, slot);
    r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.a == 1);
    key_retire(0);
    r = take(api);
    assert(r.kind == AZN_KEY_SHIFT && r.valid == -1);
    key_hooked.store(0);
}
static void slip_fixture(const azstatus_native_v1 *api, Slot &slot, uintptr_t functions) {
    Buffer<0x120> slip, table_buffer;
    state().profile.slip = table_buffer.addr();
    put(slip.addr(), table_buffer.addr());
    put(functions + 0x30, slip.addr());
    put(slot.facade + 0xa8, slip.addr());
    auto core = slot.core.load();
    unsigned char id[12] = {1, 0, 0, 0, 2, 1, 1, 0};
    memcpy(id + 8, &matched_id, 4);
    memcpy(reinterpret_cast<void *>(core + 0x18), id, sizeof id);
    put(core + 0x28, uint64_t(12000));
    put(core + 0x68, uint32_t(48000));
    memcpy(slip.bytes + 0x38, id, sizeof id);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    slip_installed.store(1);
    slip_original = reinterpret_cast<void *>(+[](void *self, int64_t frames) {
        auto p = reinterpret_cast<uintptr_t>(self);
        put(p + 0x48, word(p + 0x48) + (uint64_t) frames);
    });
    azstatus_native_receipt seed {};
    seed.kind = AZN_MOTION;
    seed.load_generation = fixture_load.load_generation;
    seed.motion_generation = 1;
    assert(status_offer_motion(slot.motion, seed) && take(api).kind == AZN_MOTION);
    slip_frame(0, slot);
    auto r = take(api);
    assert(r.kind == AZN_SLIP_HEAD && r.valid == AZ_AVAIL_EMPTY && r.b && r.attempt && r.motion_generation == 1);
    uint64_t owner = r.attempt;
    slip.bytes[0x68] = 1;
    put(slip.addr() + 0x48, uint64_t(0));
    slip_frame(0, slot);
    r = take(api);
    assert(r.valid == AZ_AVAIL_READY && r.b && r.attempt == owner && point_at(r.points[0], 0) && r.points[0].rate == 48000);
    slip_writer(slip.bytes, 24000);
    r = take(api);
    const int audible = compare_timestamps(word(core + 0x28), 12000, 48000, 48000, 1);
    assert(r.valid == AZ_AVAIL_READY && !r.b && r.motion_generation == 1 && point_at(r.points[0], 24000) &&
           audible != AZ_TIME_INVALID && audible == AZ_TIME_EQUAL);
    auto saved_original = slip_original;
    slip_original = reinterpret_cast<void *>(+[](void *, int64_t) {
        azstatus_native_receipt barrier {};
        barrier.kind = AZN_MOTION;
        barrier.load_generation = fixture_load.load_generation;
        barrier.motion_generation = 2;
        assert(status_offer_motion(state().decks[0].motion, barrier));
    });
    slip_writer(slip.bytes, 1);
    assert(take(api).kind == AZN_MOTION);
    r.size = sizeof r;
    assert(!api->take(0, &r)); // A callback begun before the barrier cannot relabel its sample.
    slip_original = saved_original;
    slip_frame(0, slot);
    r = take(api);
    assert(r.kind == AZN_SLIP_HEAD && r.valid == AZ_AVAIL_READY && r.b &&
           r.motion_generation == 2 && point_at(r.points[0], 24000));
    put(slip.addr() + 0x40, matched_id + 1);
    slip_writer(slip.bytes, 1);
    r = take(api);
    assert(r.valid == AZ_AVAIL_STALE && r.b);
    slip.bytes[0x68] = 0;
    slip_frame(0, slot);
    r = take(api);
    assert(r.valid == AZ_AVAIL_EMPTY && r.b);
    slip_retire(0);
    r = take(api);
    assert(r.c && r.valid == AZ_AVAIL_STALE && r.attempt == owner);
    slip_installed.store(0);
}
static void test_sync_role_values() {
    const SyncProfile &p = sync_profiles[1];
    Buffer<0x210> parent;
    Buffer<0xc0> slave;
    Buffer<0x20> master, server;
    Buffer<0x60> job;
    put(parent.addr() + 0xb0, server.addr());
    put(server.addr(), p.server);
    put(parent.addr() + 0xbc, uint32_t(1));
    put(parent.addr() + 0xc0, uint32_t(2));
    put(parent.addr() + 0x1c8, master.addr());
    put(master.addr(), p.master);
    put(master.addr() + 8, parent.addr());
    put(parent.addr() + 0x1d0, slave.addr());
    put(parent.addr() + 0x1c0, slave.addr());
    put(slave.addr(), p.slave);
    put(slave.addr() + 8, parent.addr());
    put(slave.addr() + 0xb0, job.addr());
    put(job.addr(), p.job);
    put(job.addr() + 0x30, slave.addr());
    put(parent.addr() + 0x118, uint8_t(1));
    put(parent.addr() + 0x119, uint8_t(1));
    put(parent.addr() + 0x120, double(126.5));
    put(parent.addr() + 0x158, uint32_t(2));
    put(slave.addr() + 0x10, uint32_t(1));
    put(slave.addr() + 0x48, uint32_t(2));
    auto expected = [&](int follow, int locked) {
        SyncValues v = sync_values(parent.addr(), p, 0);
        assert(v.following == follow && v.locked == locked);
    };
    expected(0, 0); /* off even with a leftover job */
    put(parent.addr() + 0xb8, uint8_t(1));
    expected(1, 1);
    put(slave.addr() + 0x48, uint32_t(1));
    expected(1, 0); /* phase acquisition */
    put(slave.addr() + 0x48, uint32_t(2));
    put(parent.addr() + 0x158, uint32_t(1));
    expected(1, 0);
    put(parent.addr() + 0x158, uint32_t(2));
    put(parent.addr() + 0x154, uint8_t(3));
    expected(1, 0);
    put(parent.addr() + 0x154, uint8_t(0));
    put(slave.addr() + 0x48, uint32_t(7));
    expected(1, -1);
    put(parent.addr() + 0xc0, uint32_t(0xff));
    expected(0, 0);
    put(parent.addr() + 0xc0, uint32_t(2));
    put(parent.addr() + 0xbc, uint32_t(0));
    expected(0, 0);
    put(parent.addr() + 0xbc, uint32_t(1));
    put(job.addr() + 0x30, master.addr());
    expected(-1, -1);
    puts("PASS elected sync target, independent tempo follow/phase lock, acquiring, stale beat and owner mismatch");
}
#include "sdkstatus_duration_test.inc"
#include "sdkstatus_stationary_test.inc"
#ifdef AZSDK_MAP_ALIAS_TESTING
#include "sdkstatus_map_alias_test.inc"
#endif
#include "sdkstatus_initial_loop_test.inc"
#include "sdkstatus_direct_loop_test.inc"
#include "sdkstatus_transport_retirement_test.inc"

static void test_hot_retirement(const azstatus_native_v1 *api, Slot &slot, uintptr_t context) {
    Buffer<0x100> properties[8];
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    for (unsigned i = 0; i < 8; ++i) {
        const uintptr_t source = properties[i].addr();
        put(context + 0x20 + i * 8, source);
        put(source, state().profile.cue_base_table);
        point(source + 0x30, -1);
        point(source + 0x88, -1);
    }
    status_hot_capture(0, slot, 0, nullptr);
    const auto prior = take(api);
    assert(prior.kind == AZN_HOT_CUES && prior.valid && prior.attempt && !prior.lost_topics);
    fixture_track_match_hook = [] {
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
    };
    status_hot_capture(0, slot, 0, nullptr);
    azstatus_native_receipt empty {};
    empty.size = sizeof empty;
    empty.version = 1;
    assert(!api->take(0, &empty));
    fixture_load.load_generation = prior.load_generation + 1;
    status_hot_capture(0, slot, 0, nullptr);
    const auto replacement = take(api);
    assert(replacement.kind == AZN_HOT_CUES && replacement.valid && !replacement.lost_topics &&
           replacement.load_generation == fixture_load.load_generation && replacement.attempt != prior.attempt);
    /* A source mismatch with an unchanged lifecycle still loses live history. */
    ++matched_id;
    status_hot_capture(0, slot, 0, nullptr);
    --matched_id;
    auto gap = take(api);
    assert(gap.kind == AZN_GAP && gap.b == AZN_HOT_CUES && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    status_hot_capture(0, slot, 0, nullptr);
    assert(take(api).valid);
    /* A new epoch with an active replacement is not an empty retirement cut. */
    fixture_track_match_hook = [] {
        ++fixture_load.invalidation_epoch;
        ++fixture_load.load_generation;
    };
    status_hot_capture(0, slot, 0, nullptr);
    gap = take(api);
    assert(gap.kind == AZN_GAP && gap.b == AZN_HOT_CUES && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    puts("PASS retiring hot-cue source stays quiet; live source uncertainty still reports loss");
}

static void test_hot_metadata(const azstatus_native_v1 *api, Slot &slot, uintptr_t context) {
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    hot_metadata.store(true);
    Buffer<0x208> properties[8];
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t property = properties[i].addr();
        put(context + 0x20 + i * 8, property);
        put(property, state().profile.cue_base_table);
        point(property + 0x30, -1);
        point(property + 0x88, -1);
    }
    uintptr_t property = properties[3].addr();
    Buffer<0x20> comment, string;
    char text[] = "Imported \xc3\xa9";
    put(property + 0xe1, uint8_t(1));
    put(property + 0xe8, comment.addr());
    put(property + 0xf0, comment.addr() + 0x10);
    put(comment.addr(), string.addr());
    put(comment.addr() + 8, comment.addr() + 0x10);
    put(string.addr(), reinterpret_cast<uintptr_t>(text));
    put(string.addr() + 8, uintptr_t(sizeof text - 1));
    point(property + 0x30, 480);
    point(property + 0x88, 960);
    status_hot_capture(0, slot, 0, nullptr);
    azn_hot_receipt r {};
    auto next = [&] {
        r.base.size = sizeof r;
        r.base.version = 1;
        return api->take(0, &r.base);
    };
    assert(next() && r.base.kind == AZN_HOT_CUES && r.base.size == sizeof r);
    assert(r.base.hot[3].reserved == (AZN_HOT_COMMENT | AZN_HOT_AUTO_ACTIVE | AZN_HOT_AUTO_ON));
    assert(!std::strcmp(r.details.comments[3], text));
    const uint64_t revision = r.base.revision;

    processing = &slot;
    status_original[0] = reinterpret_cast<void *>(copy);
    status_original[5] = reinterpret_cast<void *>(+[](void *self, uintptr_t) {
        uintptr_t object = reinterpret_cast<uintptr_t>(self);
        put(object + 8, integer(object + 8) + 1);
    });
    hot_edit_original = reinterpret_cast<void *>(+[](uintptr_t, uintptr_t object, uintptr_t, uintptr_t, uintptr_t) {
        void *self = reinterpret_cast<void *>(object);
        put(object + 0xe1, uint8_t(0));
        status_writer<0, uintptr_t, uintptr_t>(self, 882, UINTPTR_MAX);
        status_writer<5, uintptr_t>(self, 0);
        put(object + 0xe8, uintptr_t(0));
        put(object + 0xf0, uintptr_t(0));
        return 1;
    });
    assert(status_hot_edit(0, property, 0, 0, 0));
    processing = nullptr;
    assert(next() && r.base.kind == AZN_HOT_CUES && r.base.revision == revision + 1 && !r.base.lost_topics);
    assert(point_at(r.base.hot[3].in, 882) && r.base.hot[3].out.state == AZN_ABSENT);
    assert(!r.base.hot[3].reserved && !r.details.comments[3][0] && !next());

    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
    point(property + 0x30, -1);
    status_hot_capture(0, slot, 0, nullptr);
    const auto old = take(api);
    assert(old.size == sizeof old && old.hot[3].in.state == AZN_ABSENT && !old.hot[3].reserved);
    assert(!next());
    puts("PASS owned hot comment and automatic flag, unsaved overwrite, load withdrawal and old receipt prefix");
}

static void test_loop_retirement(const azstatus_native_v1 *api, Slot &slot, uintptr_t retained) {
    api->interest(0, status_retained_routes);
    reloop_installed.store(15);
    auto seed = [&] {
        status_loop_capture(0, slot, 0, nullptr);
        auto reloop = take(api), loop = take(api);
        assert(reloop.kind == AZN_RELOOP && loop.kind == AZN_LOOP &&
               reloop.load_generation == fixture_load.load_generation &&
               loop.load_generation == fixture_load.load_generation && !reloop.lost_topics && !loop.lost_topics);
    };
    seed();
    const auto previous = fixture_load.load_generation;
    fixture_track_match_hook = [] {
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
    };
    status_loop_capture(0, slot, 0, nullptr);
    azstatus_native_receipt empty {};
    empty.size = sizeof empty;
    empty.version = 1;
    assert(!api->take(0, &empty));
    assert(!reloop_state[0].retained.load());
    fixture_load.load_generation = previous + 1;
    seed();

    ++matched_id;
    status_loop_capture(0, slot, 0, nullptr);
    --matched_id;
    const auto loop_gap = take(api), reloop_gap = take(api);
    assert(loop_gap.kind == AZN_GAP && loop_gap.lost_topics == status_retained_routes);
    assert(reloop_gap.kind == AZN_GAP && reloop_gap.b == AZN_RELOOP && reloop_gap.lost_topics == status_loop_route);
    seed();

    /* The loop owner can lose its load after the first source match and
     * before the residency reader reserves or reads the property. */
    assert(!reloop_state[0].busy.test_and_set());
    fixture_after_track_match_hook = [] {
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
    };
    status_loop_capture(0, slot, 0, nullptr);
    reloop_state[0].busy.clear();
    assert(!api->take(0, &empty));
    fixture_load.load_generation = previous + 2;
    seed();

    put(retained + 0x178, uint64_t(2));
    fixture_after_track_match_hook = [] {
        ++fixture_load.invalidation_epoch;
        ++fixture_load.load_generation;
    };
    status_loop_capture(0, slot, 0, nullptr);
    assert(!api->take(0, &empty));
    seed();

    /* Retirement can also land after the first residency read, before the
     * second source match seals its observation. */
    put(retained + 0x178, uint64_t(3));
    fixture_track_match_hook = [] {
        fixture_track_match_hook = [] {
            ++fixture_load.invalidation_epoch;
            fixture_load.load_generation = 0;
        };
    };
    reloop_observe(0, slot, retained, fixture_load.invalidation_epoch);
    assert(!api->take(0, &empty));
    fixture_load.load_generation = previous + 4;
    seed();

    /* A residency callback can finish on the old load while another native
     * observation owns the publication slot. The old fence still identifies
     * this callback even when it cannot take the reservation. */
    {
        ReloopWrite change(retained + 0xf8);
        assert(!reloop_state[0].busy.test_and_set());
        put(retained + 0x178, uint64_t(2));
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
        change.finish();
        reloop_state[0].busy.clear();
        assert(!api->take(0, &empty));
    }
    fixture_load.load_generation = previous + 5;
    seed();
    {
        ReloopWrite change(retained + 0xf8);
        assert(!reloop_state[0].busy.test_and_set());
        put(retained + 0x178, uint64_t(3));
        change.finish();
        reloop_state[0].busy.clear();
        const auto busy = take(api);
        assert(busy.kind == AZN_GAP && busy.b == AZN_RELOOP && busy.lost_topics == status_loop_route);
    }
    puts("PASS retired loop and residency reads stay quiet; active source mismatch still reports loss");
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::strcmp(argv[1], "motion-pressure") == 0) {
        test_motion_pressure();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "transport-retirement") == 0) {
        test_transport_retirement();
        return 0;
    }
#ifdef AZSDK_DURATION_TEST_ONLY
    {
        Buffer<0x300> core;
        Slot &slot = state().decks[0];
        slot.core.store(core.addr());
        put(core.addr() + 0x18, UINT64_C(0x0001000100000001));
        const auto *api = azstem_status_service_v1();
        assert(api->wake_fd() >= 0);
        test_native_duration(api, slot, core.addr());
        slot.core.store(0);
        return 0;
    }
#endif
    const bool hot_retirement = argc == 2 && std::strcmp(argv[1], "hot-retirement") == 0;
    const bool hot_details = argc == 2 && std::strcmp(argv[1], "hot-metadata") == 0;
    const bool loop_retirement = argc == 2 && std::strcmp(argv[1], "loop-retirement") == 0;
    const bool delayed_loop_seed = argc == 2 && std::strcmp(argv[1], "delayed-loop-seed") == 0;
    if (!hot_retirement && !hot_details && !loop_retirement && !delayed_loop_seed) {
        test_concurrent_scan();
        test_sync_role_values();
    }
    Buffer<0x300> unit, core, facade;
    Buffer<0x100> bundle;
    Buffer<0x500> context;
    Buffer<0x208> retained;
    Buffer<0x100> tables[7];
    State &s = state();
    Slot &slot = s.decks[0];
    auto &p = s.profile;
    p.unit = tables[0].addr();
    p.core = tables[1].addr();
    p.facade = tables[2].addr();
    p.box = tables[3].addr();
    p.set_table = tables[4].addr();
    p.cue_table = tables[5].addr();
    p.cue_base_table = tables[6].addr();
    put(unit.addr(), p.unit);
    put(core.addr(), p.core);
    put(facade.addr(), p.facade);
    put(unit.addr() + 0x128, core.addr());
    put(unit.addr() + 0x190, bundle.addr());
    put(bundle.addr() + 0x40, facade.addr());
    put(facade.addr() + 0x18, core.addr() + 0x110);
    put(facade.addr() + 0x40, core.addr());
    put(core.addr() + 0x110, p.box);
    put(facade.addr() + 0xc8, p.set_table);
    put(facade.addr() + 0xd8, core.addr() + 0x110);
    put(facade.addr() + 0x68, context.addr());
    put(facade.addr() + 0x128, context.addr());
    uintptr_t active = context.addr() + 0x160, kept = retained.addr();
    put(context.addr() + 0x18, kept);
    put(kept, p.cue_table);
    put(active, p.cue_base_table);
    point(kept + 0x30, -1);
    point(kept + 0x88, -1);
    point(active + 0x30, -1);
    point(active + 0x88, -1);
    memcpy(core.bytes + 0x18, reinterpret_cast<const void *>(kept + 0x30), 12);
    put(core.addr() + 0x18, uint64_t(0x0001000100000001ULL));
    put(core.addr() + 0x20, uint32_t(3));
    slot.unit = unit.addr();
    slot.facade = facade.addr();
    slot.core.store(core.addr());
    s.enabled.store(AZEXIT_PASSIVE_STATUS);
    const auto api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    if (hot_retirement) {
        test_hot_retirement(api, slot, context.addr());
        return 0;
    }
    if (hot_details) {
        test_hot_metadata(api, slot, context.addr());
        return 0;
    }
    if (loop_retirement) {
        test_loop_retirement(api, slot, kept);
        return 0;
    }
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    processing = &slot;
    assert(commands() == 0); // no command capability is needed to observe
    status_original[0] = reinterpret_cast<void *>(copy);
    status_loop_capture(0, slot, 0, nullptr);
    auto seed = take(api);
    assert(seed.kind == AZN_LOOP && !seed.a && !seed.lost_topics &&
           seed.points[0].state == AZN_ABSENT && seed.points[1].state == AZN_ABSENT);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(kept), 0, UINTPTR_MAX);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(kept), 0, 48000);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), UINTPTR_MAX, UINTPTR_MAX);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
    auto in = take(api);
    assert(!in.lost_topics && in.points[0].state == AZN_COORD && point_at(in.points[0], 0) && in.points[1].state == AZN_ABSENT);
    assert(in.reloop == AZN_RELOOP_DENIED);
    auto out = take(api);
    assert(!out.a && point_at(out.points[1], 48000) && !out.lost_topics);
    assert(out.reloop == AZN_RELOOP_UNKNOWN);
    assert(take(api).a == 1);
    auto exit = take(api);
    assert(!exit.a && !exit.lost_topics && exit.points[2].state == AZN_ABSENT && exit.points[0].state == AZN_COORD);
    auto again = take(api);
    assert(again.a && !again.lost_topics && again.epoch == 11 && again.load_generation == 7);
    if (delayed_loop_seed) {
        const uint64_t seed_id = azn_seed_id(0);
        api->interest(0, status_retained_routes);
        assert(azn_seed_id(0) > seed_id);
        status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), UINTPTR_MAX, UINTPTR_MAX);
        status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
        status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), UINTPTR_MAX, UINTPTR_MAX);
        status_loop_capture(0, slot, 0, nullptr);
        for (int engaged : {0, 1, 0}) {
            const auto edge = take(api);
            assert(edge.kind == AZN_LOOP && edge.a == engaged && edge.load_generation == 7 && !edge.lost_topics);
        }
        azstatus_native_receipt empty {};
        empty.size = sizeof empty;
        empty.version = 1;
        assert(!api->take(0, &empty));
        puts("PASS delayed owned loop seed preserves all three discrete edges");
        return 0;
    }
    put(active + 0xc, uint32_t(3));
    put(active + 0xc8, uint32_t(0));
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
    assert(take(api).c == AZN_MODE_TIME);
    put(active + 0xc8, uint32_t(1));
    put(active + 0xd8, uint32_t(0x10004));
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
    assert(take(api).c == AZN_MODE_BEAT);
    put(active + 0xc8, uint32_t(7));
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), 0, 48000);
    assert(take(api).c == AZN_MODE_UNKNOWN);
    reloop_installed.store(15);
    reloop_observe(0, slot, kept, fixture_load.invalidation_epoch);
    auto waiting = take(api);
    assert(waiting.kind == AZN_RELOOP && waiting.reloop == AZN_RELOOP_DENIED && !waiting.lost_topics);
    auto residency = [&](unsigned n, uint32_t value) {
        uintptr_t lock = kept + (n ? 0x180 : 0xf8);
        ReloopWrite change(lock);
        put(lock + 0x80, value);
        change.finish();
    };
    residency(0, 2);
    residency(1, 2);
    auto ready = take(api);
    assert(ready.kind == AZN_RELOOP && ready.reloop == AZN_RELOOP_ALLOWED && !ready.lost_topics);
    residency(1, 3);
    residency(1, 2);
    assert(take(api).reloop == AZN_RELOOP_DENIED && take(api).reloop == AZN_RELOOP_ALLOWED);
    processing = nullptr;
    residency(1, 3);
    residency(1, 2);
    processing = &slot;
    assert(take(api).reloop == AZN_RELOOP_DENIED && take(api).reloop == AZN_RELOOP_ALLOWED);
    {
        ReloopEdit overlap(&reloop_state[0]);
        residency(1, 3);
        residency(1, 2);
    }
    auto missed = take(api);
    assert(missed.kind == AZN_GAP && missed.b == AZN_RELOOP && missed.lost_topics == status_loop_route);
    reloop_observe(0, slot, kept, fixture_load.invalidation_epoch);
    assert(take(api).reloop == AZN_RELOOP_ALLOWED);
    reloop_installed.store(0);
    puts("PASS retained reloop lock warming, admission, reset/reacquisition and scoped same-value loss");
    // Mutations outside the captured owner interval are visible through the revision gap.
    copy(reinterpret_cast<void *>(active), UINTPTR_MAX, UINTPTR_MAX);
    copy(reinterpret_cast<void *>(active), 0, 48000);
    status_loop_capture(0, slot, 0, nullptr);
    auto gap = take(api);
    assert(gap.a && gap.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    for (unsigned i = 0; i < 300; i++) {
        azstatus_native_receipt r {};
        r.kind = AZN_LOOP;
        r.a = i;
        azn_emit(&r);
    }
    for (unsigned i = 0; i < 256; i++) assert(unsigned(take(api).a) == i);
    assert(take(api).kind == AZN_GAP);
    status_loop_capture(0, slot, 0, nullptr);
    assert(take(api).a == 1); // full consumer recovery with no further panel event
    pause_next = true;
    std::thread stalled([] { azstatus_native_receipt r{}; r.kind=AZN_LOOP; r.a=19; azn_emit(&r); });
    while (!parked.load()) std::this_thread::yield();
    azstatus_native_receipt rejected {};
    rejected.kind = AZN_LOOP;
    azn_emit(&rejected);
    auto final_loss = take(api);
    assert(final_loss.kind == AZN_GAP && final_loss.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    azstatus_native_receipt independent {};
    independent.kind = AZN_METADATA;
    azn_emit(&independent);
    assert(take(api).kind == AZN_METADATA);
    resume_writer = true;
    stalled.join();
    rejected.size = sizeof rejected;
    rejected.version = 1;
    assert(!api->take(0, &rejected));
    s.motion_hooked = true;
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    uintptr_t c = core.addr();
    matched_id = 2;
    put(c + 0x18, uint64_t(0x0001000100000001ULL));
    put(c + 0x20, uint32_t(2));
    put(c + 0x28, uint64_t(0));
    put(c + 0x68, uint32_t(44100));
    auto travel = [&](uint64_t end) {
        StatusMotionWrite update(core.bytes, false, 64);
        put(c + 0x28, end);
        update.finish();
    };
    travel(0);
    rejected.size = sizeof rejected;
    rejected.version = 1;
    assert(!api->take(0, &rejected));
    travel(64);
    auto m = take(api);
    assert(m.kind == AZN_MOTION && m.a == AZN_MOTION_SEED && m.motion_generation == 1 && m.motion_order == 1 &&
           m.points[0].state == AZN_INVALID && m.points[1].state == AZN_COORD && point_at(m.points[1], 64, m.observed_window_samples) && m.points[1].rate == 44100);
    travel(128);
    m = take(api);
    assert(m.a == AZN_MEASURE && m.motion_order == 2 && point_at(m.points[0], 64, m.from_window_samples) && point_at(m.points[1], 128, m.observed_window_samples));
    Buffer<0x40> destination;
    put(destination.addr() + 8, uint64_t(0x0001000100000001ULL));
    put(destination.addr() + 0x10, uint32_t(2));
    put(destination.addr() + 0x18, uint64_t(0));
    {
        StatusMotionWrite reposition(core.bytes, true, 1);
        put(c + 0x28, uint64_t(0));
        reposition.finish(destination.addr());
    }
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.motion_generation == 2 && m.motion_order == 3 &&
           point_at(m.points[0], 128, m.from_window_samples) && point_at(m.points[1], 0, m.observed_window_samples));
    {
        // Park a sample across the reposition. Neither racing copy can publish.
        StatusMotionWrite parked_read(core.bytes, false, 64);
        std::thread other([&] {
            processing = &slot;
            StatusMotionWrite crossing(core.bytes, true, 1);
            put(c + 0x28, uint64_t(48000));
            crossing.finish(destination.addr());
        });
        other.join();
        parked_read.finish();
    }
    m = take(api);
    assert(m.kind == AZN_GAP && (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
    travel(48064);
    m = take(api);
    assert(m.kind == AZN_MOTION && m.motion_generation == 3 && point_at(m.points[1], 48064, m.observed_window_samples) &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
    // Witness the actual scheduled loop callback. A coincident landing without
    // the typed command/owner/interval remains an unclassified reposition.
    Buffer<0x100> typed_tables[6], jump, callback, basis, quantize, beat_owner;
    p.commands[0].table = typed_tables[0].addr();
    p.commands[3].table = typed_tables[1].addr();
    p.commands[8].table = typed_tables[2].addr();
    p.commands[10].table = typed_tables[3].addr();
    p.interval_table = typed_tables[4].addr();
    p.interval_source_table = typed_tables[5].addr();
    put(unit.addr() + 0x180, basis.addr());
    put(jump.addr(), p.commands[0].table);
    put(jump.addr() + 0x20, c);
    put(jump.addr() + 0x18, callback.addr());
    put(callback.addr(), p.commands[3].table);
    put(callback.addr() + 0x20, basis.addr());
    put(callback.addr() + 0x28, active);
    point(active + 0x30, 0);
    point(active + 0x88, 48000);
    put(active + 0x58, uintptr_t(0x20000));
    auto interval = [&](uintptr_t address, bool reverse, bool sourced) {
        put(address, sourced ? p.interval_source_table : p.interval_table);
        memcpy(reinterpret_cast<void *>(address + 8), reinterpret_cast<const void *>(active + 0x30), 12);
        put(address + 0x18, uint64_t(reverse ? 48000 : 0));
        put(address + 0x20, uint64_t(reverse ? 0 : 48000));
        if (sourced) put(address + 0x28, uintptr_t(0x20000));
    };
    interval(jump.addr() + 0x28, false, true);
    interval(callback.addr() + 0x38, false, true);
    interval(callback.addr() + 0x68, true, false);
    matched_id = 3;
    put(c + 0x20, uint32_t(3));
    put(c + 0x68, uint32_t(48000));
    put(destination.addr() + 0x10, uint32_t(3));
    Buffer<0x80> readable, readable_table;
    s.build = 2;
    put(core.addr() + 0xb8, readable.addr());
    put(readable.addr(), readable_table.addr());
    put(readable_table.addr() + 0x60, uintptr_t(0x7a78b8));
    s.motion_actions = true;
    auto land = [&](uint64_t to) {
        StatusActionScope origin(jump.addr(), 0);
        StatusMotionWrite action(core.bytes, true, 1);
        put(c + 0x28, to);
        put(destination.addr() + 0x18, to);
        action.finish(destination.addr());
    };
    // The stock rate-adjusted render can cross OUT before the reserved jump.
    travel(48001);
    take(api);
    land(0);
    m = take(api);
    assert(m.a == AZN_WRAP && m.occurrence_ordinal == 1 && !m.lost_topics &&
           point_at(m.points[0], 48001, m.from_window_samples) && point_at(m.points[1], 0, m.observed_window_samples));
    {
        StatusActionScope origin(jump.addr(), 0);
        StatusHead departure = status_head(c), landing = departure;
        slot.motion.window_samples = 64;
        slot.motion.window_rate = 48000;
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
        for (uintptr_t address : {active + 0x40, jump.addr() + 0x40,
                                  callback.addr() + 0x50, callback.addr() + 0x88})
            put(address, uint64_t(10));
        departure.sample = 9;
        landing.sample = 48000;
        assert(status_action(slot, departure, landing) == AZN_WRAP);
        departure.sample = 74;
        assert(!status_action(slot, departure, landing));
        for (uintptr_t address : {active + 0x40, jump.addr() + 0x40,
                                  callback.addr() + 0x50, callback.addr() + 0x88})
            put(address, uint64_t(0));
    }
    test_initial_loop_actions(slot, c, active, jump.addr(), callback.addr());
    put(callback.addr() + 0x28, uintptr_t(0));
    travel(48000);
    take(api);
    land(0);
    m = take(api);
    assert(m.a == AZN_REPOSITION && !m.occurrence_ordinal);
    put(callback.addr(), p.commands[8].table);
    put(callback.addr() + 0x20, context.addr());
    land(48000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 2);
    land(48000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 3);
    put(readable_table.addr() + 0x60, uintptr_t(0));
    land(48000);
    m = take(api);
    assert(m.a == AZN_REPOSITION && !m.occurrence_ordinal &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)));
    p.quantize = typed_tables[4].addr();
    put(quantize.addr(), p.quantize);
    put(facade.addr() + 0x50, quantize.addr());
    put(facade.addr() + 0x110, quantize.addr());
    put(callback.addr(), p.commands[10].table);
    put(callback.addr() + 0x20, beat_owner.addr());
    put(beat_owner.addr() + 0x10, quantize.addr());
    land(24000);
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 4);
    Buffer<0x100> needle, needle_cb_vt, pause_f, pause_vt;
    p.commands[14].table = needle_cb_vt.addr();
    p.play_pause_table = pause_vt.addr();
    put(callback.addr(), p.commands[14].table);
    put(callback.addr() + 0x20, needle.addr());
    put(needle.addr(), uintptr_t(0x2be8390));
    put(needle.addr() + 0x18, pause_f.addr());
    put(facade.addr() + 0x58, pause_f.addr());
    put(pause_f.addr(), p.play_pause_table);
    put(pause_f.addr() + 0x58, c);
    land(17000);
    m = take(api);
    assert(m.a == AZN_SEEK && m.occurrence_ordinal == 5 && point_at(m.points[1], 17000, m.observed_window_samples));
    put(pause_f.addr() + 0x58, uintptr_t(0));
    land(18000);
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.history_only &&
           (m.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && point_at(m.points[1], 18000, m.observed_window_samples) && !m.occurrence_ordinal);
    Buffer<0x80> noqua;
    put(noqua.addr(), noqua_profiles[1].owner);
    put(noqua.addr() + 8, pause_f.addr());
    put(noqua.addr() + 0x10, quantize.addr());
    put(noqua.addr() + 0x20, quantize.addr() + 0x20);
    put(pause_f.addr() + 0x58, c);
    noq_core = c;
    noq_target = destination.addr();
    noq_return = noqua_profiles[1].jump_returns[1];
    noqua_original = reinterpret_cast<void *>(fake_noqua);
    put(destination.addr() + 0x18, uint64_t(56000));
    assert(noqua_hook(noqua.bytes, 0, 0x10010, nullptr));
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 6 && point_at(m.points[1], 56000, m.observed_window_samples));
    ++noq_return;
    put(destination.addr() + 0x18, uint64_t(10000));
    assert(noqua_hook(noqua.bytes, 1, 0x10010, nullptr));
    m = take(api);
    assert(m.a == AZN_REPOSITION && m.history_only && !m.occurrence_ordinal);
    --noq_return;
    beat_grid_original = reinterpret_cast<void *>(fake_grid_jump);
    put(destination.addr() + 0x18, uint64_t(43000));
    assert(beat_grid_hook(noqua.bytes, 1, 0x10010, nullptr, true));
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 7 && point_at(m.points[1], 43000, m.observed_window_samples) && !beat_origin.owner);
    Buffer<0x80> direct_controller;
    put(basis.addr() + 0x38, direct_controller.addr());
    put(pause_f.addr() + 0x50, direct_controller.addr());
    put(quantize.addr() + 0x20, direct_controller.addr());
    put(quantize.addr() + 0x40, quantize.addr());
    put(quantize.addr() + 0x48, pause_f.addr());
    memcpy(reinterpret_cast<void *>(pause_f.addr() + 0x18), reinterpret_cast<const void *>(destination.addr() + 8), 12);
    put(pause_f.addr() + 0x28, uint64_t(12000));
    const uintptr_t paused_position = pause_f.addr() + 0x10;
    {
        StatusActionScope direct(0, 0, true);
        status_direct_request(direct, reinterpret_cast<void *>(paused_position), 2, 0x14dd4b0);
        assert(direct.direct_kind == AZN_SEEK);
        StatusMotionWrite action(core.bytes, true, 1);
        put(c + 0x28, uint64_t(12000));
        action.finish(paused_position);
    }
    m = take(api);
    assert(m.a == AZN_SEEK && m.occurrence_ordinal == 8 && !m.lost_topics);
    {
        StatusActionScope direct(0, 0, true);
        status_direct_request(direct, reinterpret_cast<void *>(paused_position), 2, 0x14dd4b4);
        assert(!direct.direct_kind);
        put(pause_f.addr() + 0x50, uintptr_t(0));
        status_direct_request(direct, reinterpret_cast<void *>(paused_position), 2, 0x14dd4b0);
        assert(!direct.direct_kind);
        put(pause_f.addr() + 0x50, direct_controller.addr());
    }
    put(destination.addr() + 0x18, uint64_t(0));
    {
        StatusActionScope direct(0, 0, true);
        status_direct_request(direct, destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(direct.direct_kind == AZN_JUMP);
        StatusMotionWrite action(core.bytes, true, 1);
        put(c + 0x28, uint64_t(0));
        action.finish(destination.addr());
    }
    m = take(api);
    assert(m.a == AZN_JUMP && m.occurrence_ordinal == 9 && !m.lost_topics);
    {
        StatusActionScope direct(0, 0, true);
        put(destination.addr() + 0x18, uint64_t(48001)); // outside the active loop, not an owned interior target
        status_direct_request(direct, destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(!direct.direct_kind);
        put(destination.addr() + 0x18, uint64_t(0));
        put(quantize.addr() + 0x48, uintptr_t(0));
        status_direct_request(direct, destination.bytes, 0, noqua_profiles[1].jump_returns[1]);
        assert(!direct.direct_kind);
        put(quantize.addr() + 0x48, pause_f.addr());
    }
    travel(1000);
    take(api);
    put(pause_f.addr() + 0x28, uint64_t(1000));
    put(readable_table.addr() + 0x60, uintptr_t(0x7a7aa0));
    uint64_t same_point_ordinal = 9;
    for (unsigned mode = 0; mode < 8; ++mode) {
        {
            StatusActionScope direct(0, 0, true);
            status_direct_request(direct, reinterpret_cast<void *>(paused_position), 2, 0x14dd4b0);
            assert(direct.direct_kind == AZN_SEEK);
            StatusMotionWrite action(core.bytes, true, 1);
            if (mode == 7) {
                /* An unowned nested setter sharing this provider cannot
                 * lend its range proof to the outer source. */
                Buffer<0x300> foreign_core;
                StatusMotionWrite nested(foreign_core.bytes, true, 1);
                StatusMotionWrite::capture_limit(readable.addr(), 1000, 0x1567dc4);
            } else if (mode != 2) {
                StatusMotionWrite::capture_limit(mode == 4 ? readable.addr() + 8 : readable.addr(),
                                                mode == 5 ? 999 : mode == 6 ? -1 : 1000,
                                                mode == 3 ? 0x1567dc8 : 0x1567dc4);
            }
            action.finish(paused_position);
        }
        m = take(api);
        if (mode < 2) {
            assert(m.a == AZN_SEEK && m.occurrence_ordinal == ++same_point_ordinal && !m.lost_topics);
        } else {
            assert(!m.occurrence_ordinal && (m.lost_topics & motion_routes) == motion_routes);
        }
    }
    put(readable_table.addr() + 0x60, uintptr_t(0x7a78b8));
    puts("PASS repeated equal-position actions, actual range return, rejected range and nested owner exclusion");
    Buffer<0xc0> transport_bundle;
    put(facade.addr() + 0x78, transport_bundle.addr());
    put(transport_bundle.addr(), c);
    put(transport_bundle.addr() + 8, context.addr());
    put(transport_bundle.addr() + 0xa8, pause_f.addr());
    transport_installed.store(15);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    put(context.addr() + 8, uint8_t(1));
    put(pause_f.addr() + 8, uint8_t(0));
    transport_observe(0, slot, TransportSeed);
    m = take(api);
    assert(m.kind == AZN_TRANSPORT_STATE && m.valid == AZ_AVAIL_READY && m.a == AZ_PLAYBACK_PAUSED);
    put(pause_f.addr() + 8, uint8_t(1));
    transport_observe(0, slot, TransportPlay);
    m = take(api);
    assert(m.a == AZ_PLAYBACK_PLAYING && !m.lost_topics);
    transport_observe(0, slot, TransportSeed);
    assert(!api->take(0, &rejected));
    put(pause_f.addr() + 8, uint8_t(0));
    transport_observe(0, slot, TransportPause);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(pause_f.addr() + 8, uint8_t(1));
    transport_observe(0, slot, TransportAudition);
    assert(take(api).a == AZ_PLAYBACK_CUEING);
    put(pause_f.addr() + 8, uint8_t(0));
    transport_observe(0, slot, TransportPause);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(pause_f.addr() + 8, uint8_t(1));
    transport_observe(0, slot, TransportOtherPlay);
    m = take(api);
    assert(m.valid == AZ_AVAIL_UNSUPPORTED && m.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    transport_installed.store(0);
    put(context.addr() + 8, uint8_t(0));
    Buffer<0x100> hot_properties[8], replacement;
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    for (unsigned i = 0; i < 8; i++) {
        uintptr_t ptr = hot_properties[i].addr();
        put(context.addr() + 0x20 + i * 8, ptr);
        put(ptr, p.cue_base_table);
        point(ptr + 0x30, -1);
        point(ptr + 0x88, -1);
    }
    status_hot_capture(0, slot, 0, nullptr);
    auto hot = take(api);
    assert(hot.kind == AZN_HOT_CUES && hot.valid && hot.attempt && hot.hot[7].in.state == AZN_ABSENT);
    uint64_t provider = hot.attempt;
    put(hot_properties[7].addr() + 0x10, uint8_t(255));
    status_writer<0, uintptr_t, uintptr_t>(hot_properties[7].bytes, 0, UINTPTR_MAX);
    hot = take(api);
    assert(hot.kind == AZN_HOT_CUES && hot.valid && hot.attempt == provider && !hot.lost_topics &&
           hot.hot[7].in.state == AZN_COORD && point_at(hot.hot[7].in, 0) && hot.hot[7].rgb == 0xff0000);
    status_writer<0, uintptr_t, uintptr_t>(hot_properties[0].bytes, 480, 960);
    hot = take(api);
    assert(point_at(hot.hot[0].in, 480) && point_at(hot.hot[0].out, 960) && point_at(hot.hot[7].in, 0));
    status_original[0] = reinterpret_cast<void *>(+[](void *self, uintptr_t source, uintptr_t) {
        uintptr_t dst = reinterpret_cast<uintptr_t>(self);
        memcpy(reinterpret_cast<void *>(dst + 0x30), reinterpret_cast<void *>(source + 0x30), 64);
        memcpy(reinterpret_cast<void *>(dst + 0x88), reinterpret_cast<void *>(source + 0x88), 64);
        put(dst + 8, unsigned(integer(dst + 8)) + 1u);
    });
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), hot_properties[0].addr(), 0);
    auto linked = take(api);
    assert(linked.hot_link.state == AZN_HOT_LINK_READY && linked.hot_link.slot == 1 &&
           linked.hot_link.provider == provider && linked.hot_link.stamp == hot.revision);
    memcpy(replacement.bytes, hot_properties[0].bytes, sizeof replacement.bytes);
    status_writer<0, uintptr_t, uintptr_t>(reinterpret_cast<void *>(active), replacement.addr(), 0);
    assert(take(api).hot_link.state == AZN_HOT_LINK_UNKNOWN);
    status_original[0] = reinterpret_cast<void *>(copy);
    status_writer<0, uintptr_t, uintptr_t>(hot_properties[7].bytes, UINTPTR_MAX, UINTPTR_MAX);
    assert(take(api).hot[7].in.state == AZN_ABSENT);
    uintptr_t a = hot_properties[0].addr();
    put(a + 8, unsigned(integer(a + 8)) + 1); // nonsemantic native revision
    status_hot_capture(0, slot, 0, nullptr);
    hot = take(api);
    assert(!hot.lost_topics && hot.b == 1 && !hot.c);
    put(a + 8, unsigned(integer(a + 8)) + 1);
    point(a + 0x30, 240);
    status_hot_capture(0, slot, 0, nullptr);
    hot = take(api);
    assert(!hot.lost_topics && hot.b == 1 && hot.c == 1 && point_at(hot.hot[0].in, 240));
    put(a + 8, unsigned(integer(a + 8)) + 2); // returning to the same value can hide an edit
    status_hot_capture(0, slot, 0, nullptr);
    hot = take(api);
    assert(hot.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    put(context.addr() + 0x20, replacement.addr());
    put(replacement.addr(), p.cue_table);
    point(replacement.addr() + 0x30, -1);
    point(replacement.addr() + 0x88, -1);
    status_hot_capture(0, slot, 0, nullptr);
    hot = take(api);
    assert(hot.attempt != provider && hot.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    Buffer<0xd0> status_facade, key_facade, function_set, setting_quantize, key, alien;
    put(bundle.addr() + 0x30, status_facade.addr());
    put(bundle.addr() + 0x48, key_facade.addr());
    put(unit.addr() + 0x188, function_set.addr());
    const SettingProfile &settings = setting_profiles[1];
    put(status_facade.addr(), settings.state_facade);
    put(key_facade.addr(), settings.key_facade);
    p.quantize = tables[4].addr();
    put(setting_quantize.addr(), p.quantize);
    put(key.addr(), settings.key_function);
    put(function_set.addr() + 0x20, setting_quantize.addr());
    put(status_facade.addr() + 0x48, setting_quantize.addr());
    put(facade.addr() + 0x50, setting_quantize.addr());
    put(status_facade.addr() + 0x78, key.addr());
    put(key_facade.addr() + 0x78, key.addr());
    setting_original[0] = reinterpret_cast<void *>(+[](void *o, unsigned value) {
        put(reinterpret_cast<uintptr_t>(o) + 16, static_cast<uint8_t>(value));
    });
    setting_original[1] = reinterpret_cast<void *>(+[](void *o, unsigned value, unsigned) {
        put(reinterpret_cast<uintptr_t>(o) + 8, static_cast<uint8_t>(value));
    });
    setting_installed.store(3);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS));
    status_settings_frame(0, slot);
    auto qs = take(api), ks = take(api);
    assert(qs.kind == AZN_QUANTIZE && qs.valid && !qs.a && ks.kind == AZN_KEY_LOCK && ks.valid && !ks.a);
    assert(qs.unit && ks.unit && qs.unit != ks.unit);
    setting_writer<0>(reinterpret_cast<void *>(setting_quantize.addr()), 1u);
    setting_writer<0>(reinterpret_cast<void *>(setting_quantize.addr()), 0u);
    assert(take(api).a == 1 && take(api).a == 0);
    setting_writer<1>(reinterpret_cast<void *>(key.addr() + 8), 1u, 0u);
    assert(take(api).a == 1);
    key_fixture(api, slot, key.addr());
    setting_writer<0>(reinterpret_cast<void *>(alien.addr()), 1u);
    assert(alien.bytes[16] == 1 && !api->take(0, &rejected));
    put(key_facade.addr() + 0x78, alien.addr());
    status_settings_frame(0, slot);
    assert(take(api).kind == AZN_QUANTIZE); // the new KEY subscription reseeded settings
    auto retired_key = take(api);
    assert(retired_key.kind == AZN_KEY_LOCK && retired_key.valid < 0 && retired_key.unit == ks.unit);
    setting_writer<1>(reinterpret_cast<void *>(key.addr() + 8), 0u, 0u);
    assert(!api->take(0, &rejected));
    Buffer<0xd0> sync_facade;
    Buffer<0x220> sync_function;
    put(bundle.addr() + 0x60, sync_facade.addr());
    put(sync_facade.addr(), settings.sync_facade);
    put(sync_facade.addr() + 0x68, sync_function.addr());
    put(status_facade.addr() + 0x68, sync_function.addr());
    put(function_set.addr() + 0x78, sync_function.addr());
    put(sync_function.addr(), settings.sync_function);
    setting_original[2] = reinterpret_cast<void *>(+[](void *o, unsigned mode) {
        put(reinterpret_cast<uintptr_t>(o) + 0xb8, static_cast<uint8_t>(mode == 2));
    });
    setting_installed.store(7);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_SYNC));
    status_settings_frame(0, slot);
    auto ss = take(api);
    if (ss.kind == AZN_QUANTIZE) ss = take(api); /* shared legacy interest reseeds the installed setting */
    assert(ss.kind == AZN_SYNC && ss.valid && !ss.a && !ss.b);
    setting_writer<2>(sync_function.bytes, 2u);
    setting_writer<2>(sync_function.bytes, 1u);
    ss = take(api);
    assert(ss.a == 1 && ss.b == 2);
    ss = take(api);
    assert(!ss.a && !ss.b);
    setting_writer<2>(sync_function.bytes, 0u);
    ss = take(api);
    assert(ss.valid && !ss.a && ss.b == -1);
    setting_installed.store(15);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    status_settings_frame(0, slot);
    auto locked = take(api);
    while (locked.kind != AZN_SLIDER_LOCK) locked = take(api);
    assert(locked.valid && locked.a == 0);
    slider_lock_original[3] = reinterpret_cast<void *>(+[](void *object) {
        put(reinterpret_cast<uintptr_t>(object) + 0x170, uint32_t(1));
    });
    slider_lock_original[2] = reinterpret_cast<void *>(+[](void *object) {
        put(reinterpret_cast<uintptr_t>(object) + 0x170, uint32_t(2));
        slider_lock_writer<3>(object);
    });
    slider_lock_writer<2>(sync_function.bytes);
    locked = take(api);
    assert(locked.kind == AZN_SLIDER_LOCK && locked.a == 1 && locked.valid);
    slider_lock_writer<3>(sync_function.bytes);
    assert(!api->take(0, &rejected));
    put(function_set.addr() + 0x78, alien.addr());
    status_settings_frame(0, slot);
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
        StatusLanding landing(core.bytes);
        put(c + 0x20, uint32_t(2303));
        put(c + 0x28, uint64_t(372204));
        put(c + 0x68, uint32_t(44100));
        put(destination.addr() + 0x10, uint32_t(2303));
        put(destination.addr() + 0x18, uint64_t(372204));
        landing.finish(destination.addr());
    }
    status_stationary_seed(0, slot);
    assert(!api->take(0, &rejected));
    fixture_load.load_generation = 23;
    matched_id = 9;
    status_stationary_seed(0, slot);
    assert(!api->take(0, &rejected));
    matched_id = 2303;
    status_stationary_seed(0, slot);
    m = take(api);
    assert(m.a == AZN_MOTION_SEED && m.load_generation == 23 && m.motion_order == 1 &&
           point_at(m.points[1], 372204, m.observed_window_samples) && m.points[1].rate == 44100);
    ++fixture_load.invalidation_epoch;
    ++fixture_load.load_generation;
    status_stationary_seed(0, slot);
    assert(!api->take(0, &rejected));
    {
        StatusLanding same(core.bytes);
        same.finish(destination.addr());
    }
    status_stationary_seed(0, slot);
    assert(!api->take(0, &rejected));
    api->interest(0, motion_routes);
    put(pause_f.addr() + 8, uint8_t(0));
    put(context.addr() + 8, uint8_t(0));
    Buffer<64> load_backcue, load_backcue_table;
    const uintptr_t saved_backcue_table = p.commands[7].table;
    p.commands[7].table = load_backcue_table.addr();
    put(load_backcue.addr(), p.commands[7].table);
    put(load_backcue.addr() + 0x38, pause_f.addr());
    for (unsigned mode = 0; mode < 12; ++mode) {
        const uint64_t first_sample = mode == 9 ? 0 : 13097;
        const bool expected_opening = mode == 0 || mode == 7 || mode == 9;
        ++fixture_load.invalidation_epoch;
        fixture_load.load_generation = 0;
        matched_id = fixture_pending_id = 2400 + mode;
        fixture_pending_request = 100 + mode;
        slot.motion.landing_order.store(1);
        put(destination.addr() + 0x10, matched_id);
        put(destination.addr() + 0x18, first_sample);
        auto adopt = [&] {
            StatusLanding adoption(core.bytes);
            put(c + 0x20, matched_id);
            put(c + 0x28, first_sample);
            put(c + 0x68, uint32_t(44100));
            adoption.finish(destination.addr());
        };
        if (mode >= 7 && mode <= 9) {
            put(load_backcue.addr() + 0x30, context.addr() + (mode == 8 ? 16 : 0));
            StatusActionScope loader(0, load_backcue.addr(), true);
            adopt();
        } else adopt();
        assert(slot.motion.landing_stamp.load() == 2 && slot.motion.opening_landing.load() == 2);
        if (mode == 1) {
            put(c + 0x28, uint64_t(14097));
            put(destination.addr() + 0x18, uint64_t(14097));
        }
        if (mode == 2) {
            Buffer<32> render;
            put(render.addr() + 8, int32_t(64));
            void *saved = status_continuous_original;
            status_continuous_original = reinterpret_cast<void *>(+[](void *, const void *) {});
            status_continuous(core.bytes, render.bytes); // even a net-zero render breaks the copy chain
            status_continuous_original = saved;
            assert(!api->take(0, &rejected));
        }
        if (mode == 4) fixture_load.load_generation = 100 + mode; // a late same-point DJ seek
        if (mode == 5) ++fixture_pending_id;
        if (mode == 6) put(c + 0x68, uint32_t(48000));
        if (mode == 11) ++fixture_pending_request;
        if (mode == 3) {
            StatusActionScope action(0, 0, true);
            action.beat = true; // a real classified action, not the loader's controller plumbing
            StatusLanding copy(core.bytes);
            copy.finish(destination.addr());
        } else if (mode >= 7 && mode <= 9) {
            StatusActionScope loader(0, 0, true);
            StatusLanding copy(core.bytes);
            copy.finish(destination.addr());
        } else {
            StatusLanding copy(core.bytes);
            copy.finish(destination.addr());
        }
        if (mode >= 7 && mode <= 9) {
            StatusActionScope loader(0, 0, true);
            StatusLanding copy(core.bytes);
            copy.finish(destination.addr());
        } else {
            StatusLanding copy(core.bytes);
            copy.finish(destination.addr());
        }
        assert(slot.motion.landing_stamp.load() == 4);
        assert(slot.motion.opening_landing.load() == (expected_opening || mode == 10 ? 4u : 2u));
        if (mode == 10) put(c + 0x28, uint64_t(14097)); // movement after the last adoption copy
        if (!fixture_load.load_generation) {
            status_stationary_seed(0, slot); // owner frame began before completion
            assert(!api->take(0, &rejected));
        }
        fixture_load.load_generation = 100 + mode;
        fixture_pending_request = 0;
        if (mode == 7 || mode == 9) {
            put(c + 9, uint8_t(1));
            StatusMotionWrite first(core.bytes, false, 64);
            m = take(api); // first matched writer seeds under its existing HEAD reservation
            assert(m.a == AZN_MOTION_SEED && m.motion_flags == AZN_AT_OPENING && m.motion_order == 1);
            first.finish();
            m = take(api);
            assert(m.a == AZN_MEASURE && m.motion_order == 2 && !m.lost_topics && slot.motion.opening.load());
        } else {
            status_stationary_seed(0, slot);
            m = take(api);
            assert(m.a == AZN_MOTION_SEED && m.motion_flags == (expected_opening ? unsigned(AZN_AT_OPENING) : 0u));
        }
        status_stationary_seed(0, slot);
        assert(!api->take(0, &rejected)); // late outer-frame seed cannot duplicate the first writer's seed
    }
    p.commands[7].table = saved_backcue_table;
    puts("PASS pending native loader copies and first-writer zero/nonzero seed race; movement/action/source/owner/request/completion negatives");
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
    status_stationary_seed(0, slot); // old source remains at request entry
    put(c + 0x20, uint32_t(2304));
    put(c + 0x28, uint64_t(0));
    matched_id = 2304;
    status_stationary_seed(0, slot);
    assert(!api->take(0, &rejected));
    put(pause_f.addr() + 8, uint8_t(0));
    put(context.addr() + 8, uint8_t(0));
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    fixture_load.load_generation = 25;
    status_stationary_seed(0, slot);
    m = take(api);
    assert(m.a == AZN_MOTION_SEED && point_at(m.points[1], 0, m.observed_window_samples) && m.load_generation == 25 && m.motion_flags == AZN_AT_OPENING);
    put(c + 9, uint8_t(1)); /* active engine rendering a paused zero-speed source */
    travel(0);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && slot.motion.opening.load());
    travel(0);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && slot.motion.opening.load());
    map_clocks[0].load.store(fixture_load.load_generation);
    map_clocks[0].serial.store(2);
    map_mutations[0].active.store(1);
    travel(0);
    m = take(api);
    assert(m.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) && slot.motion.opening.load());
    map_mutations[0].active.store(0);
    azstatus_native_receipt initial_map {};
    initial_map.kind = AZN_MAPS;
    initial_map.load_generation = fixture_load.load_generation;
    initial_map.map.changed = AZNM_PAIR;
    assert(offer_map_cut(&initial_map));
    m = take(api);
    assert(m.kind == AZN_MAPS && m.motion_flags == AZN_AT_OPENING && m.observed_window_samples == 64);
    const uint64_t opening_cut = slot.motion.order;
    transport_installed.store(15);
    transport_observe(0, slot, TransportSeed);
    assert(take(api).a == AZ_PLAYBACK_PAUSED);
    put(pause_f.addr() + 8, uint8_t(1));
    transport_observe(0, slot, TransportPlay);
    assert(take(api).a == AZ_PLAYBACK_PLAYING);
    m = take(api);
    assert(m.a == AZN_MEASURE && m.motion_flags == AZN_BEGAN_PLAY && point_at(m.points[1], 0, m.observed_window_samples) && m.motion_order == opening_cut + 1);
    transport_observe(0, slot, TransportPlay);
    status_opening_play(0, slot, fixture_load);
    assert(!api->take(0, &rejected));
    ++fixture_load.invalidation_epoch;
    fixture_load.load_generation = 0;
    status_stationary_seed(0, slot);
    matched_id = 2305;
    put(c + 0x20, matched_id);
    put(pause_f.addr() + 8, uint8_t(0));
    fixture_load.load_generation = 26;
    status_stationary_seed(0, slot);
    assert(take(api).motion_flags == AZN_AT_OPENING);
    travel(31);
    take(api);
    assert(slot.motion.opening.load());
    travel(63);
    take(api);
    assert(slot.motion.opening.load());
    travel(64);
    take(api);
    assert(!slot.motion.opening.load());
    travel(0);
    take(api);
    assert(!slot.motion.opening.load());
    put(c + 9, uint8_t(0));
    puts("PASS owned opening at zero, single confirmed PLAY edge and repeated-play suppression");
    transport_installed.store(0);
    puts("PASS paused source adoption, matched TrackID, stationary sample and same-track replacement exclusion");
    Buffer<0x100> readable_rate, manager_rate, mode_rate, engine_rate;
    const RateProfile &rp = rate_profiles[1];
    double samples[3] = {-1, -1, -1};
    put(c + 0xa8, readable_rate.addr());
    put(readable_rate.addr(), rp.readable);
    put(readable_rate.addr() + 0x18, manager_rate.addr());
    put(manager_rate.addr(), rp.manager);
    put(manager_rate.addr() + 0x18, mode_rate.addr());
    put(mode_rate.addr(), rp.mode);
    put(manager_rate.addr() + 0x38, engine_rate.addr());
    put(engine_rate.addr(), rp.pitch);
    put(engine_rate.addr() + 0x18, reinterpret_cast<uintptr_t>(samples));
    put(engine_rate.addr() + 0x20, uint64_t(3));
    put(engine_rate.addr() + 0x10, -1.0);
    rate_hooks.store(15);
    rate_generator_hooked.store(true);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    rate_capture(0, slot);
    auto rate = take(api);
    assert(rate.kind == AZN_RATE && rate.valid == AZ_AVAIL_READY && rate.a == 0);
    put(c + 9, uint8_t(1));
    rate_capture(0, slot, 0, 3);
    rate = take(api);
    double magnitude;
    memcpy(&magnitude, &rate.points[0].samples, 8);
    assert(rate.a == -1 && rate.valid == AZ_AVAIL_READY && magnitude == 1);
    samples[0] = -0.5;
    rate_capture(0, slot, 0, 3);
    assert(!api->take(0, &rejected)); /* acceleration kept the same current endpoint */
    samples[1] = -0.625;
    samples[2] = -0.75;
    put(engine_rate.addr() + 0x10, -0.75);
    rate_capture(0, slot, 0, 3);
    rate = take(api);
    memcpy(&magnitude, &rate.points[0].samples, 8);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_READY && rate.a == -1 && magnitude == 0.75 && !rate.lost_topics);
    samples[1] = 0.5; /* an actual opposite-direction excursion is still lost history */
    rate_capture(0, slot, 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_STALE && rate.lost_topics);
    rate_capture(0, slot);
    assert(!api->take(0, &rejected));
    samples[1] = -0.625;
    rate_capture(0, slot, 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_READY && rate.b == AZ_AVAIL_READY && !rate.lost_topics);
    samples[2] = -0.5; /* stale/mismatched buffer cannot qualify the scalar */
    rate_capture(0, slot, 0, 3);
    rate = take(api);
    assert(rate.valid == AZ_AVAIL_STALE && rate.b == AZ_AVAIL_STALE && rate.lost_topics);
    samples[2] = -0.75;
    rate_capture(0, slot, 0, 3);
    assert(take(api).valid == AZ_AVAIL_READY);
    samples[0] = 0; /* a zero before reverse acceleration is not a reversal */
    rate_capture(0, slot, 0, 3);
    assert(!api->take(0, &rejected));
    put(c + 9, uint8_t(0));
    rate_capture(0, slot);
    assert(take(api).a == 0);
    /* A buffering seek commits speed=1 before its first generated block.
     * The old paused vector stays zero and is not fresh render evidence. */
    std::fill(samples, samples + 3, 0.0);
    put(engine_rate.addr() + 0x10, 1.0);
    put(c + 9, uint8_t(1));
    rate_capture(0, slot);
    assert(take(api).valid == AZ_AVAIL_READY);
    {
        RateRenderScope block(core.bytes);
        assert(block.measured_frames(3) == 0);
        rate_capture(0, slot, 0, block.measured_frames(3));
        assert(!api->take(0, &rejected));
    }
    {
        RateRenderScope block(core.bytes);
        RateRenderScope::generated(engine_rate.addr());
        assert(block.measured_frames(3) == 3);
        rate_capture(0, slot, 0, block.measured_frames(3));
        auto invalid = take(api);
        assert(invalid.valid == AZ_AVAIL_STALE && invalid.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    }
    std::fill(samples, samples + 3, 1.0);
    {
        RateRenderScope block(core.bytes);
        RateRenderScope::generated(engine_rate.addr());
        rate_capture(0, slot, 0, block.measured_frames(3));
        assert(take(api).valid == AZ_AVAIL_READY);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        RateRenderScope block(core.bytes);
        switch (mode) {
            case 0: ++fixture_load.unit_generation; break;
            case 1: put(c + 0x20, matched_id + 1); break;
            case 2: put(manager_rate.addr() + 0x38, uintptr_t(0)); break;
            case 3: { RateRenderScope nested(reinterpret_cast<void *>(c + 8)); break; }
            case 4: RateRenderScope::generated(engine_rate.addr() + 8); break;
            case 5: put(engine_rate.addr(), rp.scratch); put(engine_rate.addr() + 0x58, uint8_t(1)); break;
        }
        assert(block.measured_frames(3) == 3);
        if (mode == 0) --fixture_load.unit_generation;
        if (mode == 1) put(c + 0x20, matched_id);
        if (mode == 2) put(manager_rate.addr() + 0x38, engine_rate.addr());
        if (mode == 5) { put(engine_rate.addr(), rp.pitch); put(engine_rate.addr() + 0x58, uint8_t(0)); }
    }
    rate_hooks.store(0);
    rate_generator_hooked.store(false);
    puts("PASS source-qualified no-render rate capture, real generator validation and owner/mode negatives");
    const auto active_serial = slot.motion.active_serial.load();
    assert(!slot.motion.active_writers.load());
    rate_original[0] = reinterpret_cast<void *>(+[](void *core, unsigned) {
        assert(state().decks[0].motion.active_writers.load() == 1);
        put(reinterpret_cast<uintptr_t>(core) + 9, uint8_t(1));
    });
    rate_original[1] = reinterpret_cast<void *>(+[](void *core) {
        assert(state().decks[0].motion.active_writers.load() == 1);
        put(reinterpret_cast<uintptr_t>(core) + 9, uint8_t(0));
    });
    rate_writer<0, unsigned>(reinterpret_cast<void *>(c), 1);
    rate_writer<1>(reinterpret_cast<void *>(c));
    assert(!slot.motion.active_writers.load() && slot.motion.active_serial.load() == active_serial + 4);
    rate_original[0] = reinterpret_cast<void *>(+[](void *, unsigned) { throw 7; });
    try {
        rate_writer<0, unsigned>(reinterpret_cast<void *>(c), 1);
        assert(false);
    } catch (int value) {
        assert(value == 7 && !slot.motion.active_writers.load() && slot.motion.active_serial.load() == active_serial + 6);
    }
    rate_original[0] = rate_original[1] = nullptr;
    puts("PASS actual start/stop writer depth, serial and exceptional release");
    slip_fixture(api, slot, function_set.addr());
    /* Match the stock unit's final audible frame, then its independently
     * committed terminal decision. The same-source length has native units. */
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
    put(c + 0x68, uint32_t(44100));
    slot.motion.load = fixture_load.load_generation;
    slot.motion.known = true;
    slot.motion.rate = 44100;
    slot.motion.generation = 1;
    slot.motion.order = 10;
    slot.motion.previous = 990;
    Buffer<0x1000> payload;
    put(payload.addr() + 0x30, matched_id);
    put(payload.addr() + 0x38, uint64_t(990));
    put(payload.addr() + 0x15, uint8_t(1));
    EndRead frame {};
    frame.d = 0;
    frame.core = c;
    frame.load = fixture_load;
    frame.length = 1000;
    frame.rate = 44100;
    end_commit(frame, payload.addr());
    azstatus_native_receipt arriving {};
    arriving.kind = AZN_MOTION;
    arriving.a = AZN_MEASURE;
    arriving.load_generation = fixture_load.load_generation;
    arriving.epoch = fixture_load.invalidation_epoch;
    arriving.points[0] = {AZN_COORD, 44100, 990};
    arriving.points[1] = {AZN_COORD, 44100, 999};
    arriving.from_window_samples = arriving.observed_window_samples = 32;
    arriving.from_window_rate = arriving.observed_window_rate = 44100;
    arriving.motion_generation = slot.motion.generation;
    arriving.motion_order = ++slot.motion.order;
    end_mark_motion(0, arriving);
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && point_at(arriving.points[1], 999));
    arriving.points[0] = arriving.points[1];
    arriving.points[1].samples = 1027;
    arriving.motion_order = ++slot.motion.order;
    slot.motion.previous = 1027;
    end_mark_motion(0, arriving);
    assert(!end_state[0].refresh.load());
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && point_at(arriving.points[2], 1000));
    put(c + 0x28, uint64_t(1000));
    memcpy(slot.motion.identity, reinterpret_cast<const void *>(c + 0x18), 12);
    put(payload.addr() + 0x38, uint64_t(1027));
    end_commit(frame, payload.addr());
    end_accept_clamp(frame, 1026, c + 0x10);
    assert(compare_timestamps(slot.motion.previous, 1027, frame.rate, frame.rate, 1) == AZ_TIME_EQUAL);
    end_accept_clamp(frame, 1027, c + 0x10);
    assert(compare_timestamps(slot.motion.previous, 1000, frame.rate, frame.rate, 1) == AZ_TIME_EQUAL);
    put(payload.addr() + 0x38, uint64_t(1000));
    put(payload.addr() + 0x14, uint8_t(1));
    end_commit(frame, payload.addr());
    auto ended = take(api);
    assert(ended.kind == AZN_END && ended.a == 1 && ended.motion_order == 13 &&
           ended.motion_flags == (AZN_TERMINAL_CONFIRMED | AZN_TERMINAL_CONTINUATION));
    end_commit(frame, payload.addr());
    assert(!api->take(0, &rejected));
    /* Real EOF render blocks can continue at the exact clamped endpoint.
     * They must not start a new arrival/confirmation cycle each time. */
    for (unsigned repeat = 0; repeat < 4; ++repeat) {
        arriving.motion_flags = 0;
        arriving.points[0] = arriving.points[1] = {AZN_COORD, 44100, 1000};
        arriving.motion_order = ++slot.motion.order;
        end_mark_motion(0, arriving);
        assert(!(arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && !end_state[0].pending_order);
        end_commit(frame, payload.addr());
        assert(!api->take(0, &rejected));
    }
    arriving.motion_flags = 0;
    arriving.map.owner = 99; /* a replacement map needs its own terminal cut */
    arriving.motion_order = ++slot.motion.order;
    end_mark_motion(0, arriving);
    assert((arriving.motion_flags & AZN_TERMINAL_ARRIVAL) && end_state[0].pending_order == arriving.motion_order);
    end_commit(frame, payload.addr());
    ended = take(api);
    assert(ended.kind == AZN_END && ended.a == 1);
    put(payload.addr() + 0x16, uint8_t(1));
    end_commit(frame, payload.addr());
    ended = take(api);
    assert(ended.kind == AZN_END && !ended.a && ended.motion_order == 20);
    /* The map holds the same production reservation used by offer_map_cut.
     * The native inactive branch has no head store. Every negative control
     * reports loss, including an active-latch excursion inside the window. */
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    rate_hooks.store(3);
    for (unsigned mode = 0; mode < 7; ++mode) {
        put(c + 9, uint8_t(mode == 3));
        put(c + 0x20, matched_id);
        put(c + 0x28, uint64_t(1000));
        slot.motion.busy.store(mode == 4 ? Slot::Motion::HEAD : Slot::Motion::MAP);
        uint64_t interference = slot.motion.interference.load();
        {
            StatusMotionWrite overlapping(core.bytes, false, mode == 2 ? 0 : 64);
            if (mode == 1) {
                slot.motion.active_serial.fetch_add(2);
                put(c + 0x28, uint64_t(1001));
            }
            if (mode == 5) put(c + 0x20, matched_id + 1);
            if (mode == 6) slot.motion.interference.fetch_add(1);
            overlapping.finish();
        }
        slot.motion.busy.store(Slot::Motion::FREE);
        if (!mode) {
            assert(slot.motion.interference.load() == interference);
            assert(!api->take(0, &rejected));
        } else {
            auto gap = take(api);
            assert(gap.kind == AZN_GAP && (gap.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
            assert(slot.motion.interference.load() > interference);
        }
    }
    puts("PASS native inactive map reservation, active-latch excursion and competing writer loss");
    /* Active Core+9 does not imply source travel. Only a completed, owned
     * zero-advance block may preserve the opening across the MAP reservation.
     * A net-zero excursion and a sub-sample advance are both rejected. */
    double stationary_samples[64] = {};
    for (unsigned mode = 0; mode < 13; ++mode) {
        std::fill(stationary_samples, stationary_samples + 64, 0.0);
        put(c + 9, uint8_t(1));
        put(c + 0x20, matched_id);
        put(c + 0x28, uint64_t(1000));
        put(c + 0x68, uint32_t(44100));
        put(engine_rate.addr(), rp.pitch);
        put(manager_rate.addr() + 0x38, engine_rate.addr());
        put(engine_rate.addr() + 0x18, reinterpret_cast<uintptr_t>(stationary_samples));
        put(engine_rate.addr() + 0x20, uint64_t(64));
        slot.motion.opening.store(true);
        slot.motion.busy.store(Slot::Motion::MAP);
        const uint64_t interference = slot.motion.interference.load();
        {
            StatusMotionWrite overlapping(core.bytes, false, 64);
            switch (mode) {
                case 1: stationary_samples[0] = 0.125; break;
                case 2: stationary_samples[0] = 1; stationary_samples[1] = -1; break;
                case 3: put(c + 0x28, uint64_t(1001)); break;
                case 4: put(c + 0x20, matched_id + 1); break;
                case 5: put(c + 0x68, uint32_t(48000)); break;
                case 6: slot.motion.active_serial.fetch_add(2); break;
                case 7: slot.motion.landing_order.fetch_add(1); break;
                case 8: put(engine_rate.addr(), uintptr_t(0)); break;
                case 9: put(engine_rate.addr() + 0x20, uint64_t(63)); break;
                case 10: put(manager_rate.addr() + 0x38, uintptr_t(0)); break;
                case 11: put(c + 9, uint8_t(0)); break;
            }
            if (mode != 12) overlapping.finish();
        }
        slot.motion.busy.store(Slot::Motion::FREE);
        if (!mode) {
            assert(slot.motion.interference.load() == interference && slot.motion.opening.load());
            assert(!api->take(0, &rejected));
        } else {
            auto gap = take(api);
            assert(gap.kind == AZN_GAP && (gap.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)));
            assert(slot.motion.interference.load() > interference && !slot.motion.opening.load());
        }
    }
    puts("PASS stationary active map overlap, source/owner fences and moving-block loss");
    put(c + 9, uint8_t(1));
    put(c + 0x20, matched_id);
    put(c + 0x28, uint64_t(0));
    put(c + 0x68, uint32_t(44100));
    put(engine_rate.addr(), rp.pitch);
    put(manager_rate.addr() + 0x38, engine_rate.addr());
    put(engine_rate.addr() + 0x18, reinterpret_cast<uintptr_t>(stationary_samples));
    put(engine_rate.addr() + 0x20, uint64_t(64));
    slot.motion.load = fixture_load.load_generation;
    slot.motion.known = true;
    slot.motion.previous = slot.motion.opening_sample = 0;
    slot.motion.rate = slot.motion.opening_rate = slot.motion.window_rate = 44100;
    slot.motion.window_samples = 64;
    slot.motion.captured_interference = slot.motion.interference.load();
    slot.motion.seed = azn_seed_id(0);
    memcpy(slot.motion.identity, reinterpret_cast<const void *>(c + 0x18), 12);
    slot.motion.opening.store(true);
    azstatus_native_receipt overlapped_map {};
    overlapped_map.kind = AZN_MAPS;
    overlapped_map.load_generation = fixture_load.load_generation;
    overlapped_map.map.owner = 77;
    overlapped_map.map.changed = AZNM_PAIR;
    overlapped_map.map.grid.provider = overlapped_map.map.phrases.provider = 78;
    overlapped_map.map.grid.revision = overlapped_map.map.phrases.revision = 1;
    overlapped_map.map.grid.content = overlapped_map.map.phrases.content = 1;
    slot.motion.busy.store(Slot::Motion::HEAD);
    assert(offer_map_cut(&overlapped_map));
    slot.motion.busy.store(Slot::Motion::FREE);
    m = take(api);
    assert(m.kind == AZN_MAPS && !m.motion_order && !m.motion_flags);
    map_mutations[0].active.store(1);
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::fill(stationary_samples, stationary_samples + 64, 0.0);
        if (mode == 1) stationary_samples[0] = 0.125;
        if (mode == 2) { stationary_samples[0] = 1; stationary_samples[1] = -1; }
        travel(0);
        m = take(api);
        if (m.a != AZN_MEASURE || m.lost_topics != (mode ? AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) : 0))
            fprintf(stderr, "opening map mode=%u kind=%d action=%d lost=%llx flags=%u\n", mode, m.kind, m.a,
                    (unsigned long long) m.lost_topics, m.motion_flags);
        assert(m.a == AZN_MEASURE && m.lost_topics == (mode ? AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) : 0));
        assert(bool(m.motion_flags & AZN_AT_OPENING) == (mode == 0));
    }
    /* A qualified internal render can clear an unreadable block without
     * generating a new advance vector. Its previous nonzero vector is stale. */
    std::fill(stationary_samples, stationary_samples + 64, 1.0);
    rate_generator_hooked.store(true);
    {
        RateRenderScope block(core.bytes);
        assert(RateRenderScope::stationary(c, 64));
        travel(0);
    }
    rate_generator_hooked.store(false);
    m = take(api);
    assert(m.a == AZN_MEASURE && !m.lost_topics && (m.motion_flags & AZN_AT_OPENING));
    map_mutations[0].active.store(0);
    puts("PASS opening recovery after paired map overlaps HEAD; nonzero and net-zero movement do not recover opening");
    puts("PASS source-qualified terminal arrival, compiler-checked payload gates and cue withdrawal");
    puts("PASS background slip clock, source/rate fence, zero, motion, release and retirement");
    puts("PASS measured current transport rate through ramps, reverse and corrupt-block recovery");
    puts("PASS matched unit setting writers, A/B/A, independent owner retirement and raw enrollment");
    puts("PASS owned hot bank, H at zero, simultaneous A/H, clear, container identity and scoped loss");
    puts("PASS stock typed loop traversal and cue/beat jump, repeated ordinal and same-boundary negative control");
    puts("PASS native position source, stationary load fencing, zero landing, same-load barrier and delayed writer exclusion");
    puts("PASS native status owner, separate retained/working transitions, epoch, scoped revision loss and overflow");
    test_stationary_measurements(api, slot, c);
    test_direct_loop_actions(api, slot, c, active, basis.addr(), pause_f.addr(), word(slot.facade + 0x50), direct_controller.addr());
#ifdef AZSDK_MAP_ALIAS_TESTING
    test_gui_map_alias(api, slot, c);
#endif
    test_native_duration(api, slot, c);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* UpdateBGPositionTask commits a source TrackID and a signed source-sample
 * coordinate at SlipFunction +0x38/+0x48. The audible core supplies the rate
 * only for the same TrackID, unit and matched load. */
struct SlipProjection {
    std::atomic<uintptr_t> address {0};
    std::atomic<uint64_t> owner {0}, stamp {0};
    std::atomic<unsigned> writers {0};
    std::atomic<int64_t> elapsed_samples {0};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    uint64_t epoch = 0, load = 0, unit = 0, seed = 0, rate_load = 0, sample = 0;
    uint64_t published_load = 0, published_generation = 0;
    uint32_t rate = 0, published_rate = 0;
    int availability = -1;
};
static SlipProjection slip_projection[4];
static std::atomic<uint64_t> slip_owners {0};
static std::atomic<unsigned> slip_installed {0};
static void *slip_original;
struct SlipMotionCut {
    uint64_t load, generation, landing;
};
static SlipMotionCut slip_motion_cut(const Slot::Motion &motion) noexcept {
    const uint64_t load = motion.slip_load.load(std::memory_order_acquire);
    const uint64_t generation = motion.slip_generation.load(std::memory_order_acquire);
    const uint64_t landing = motion.landing_order.load(std::memory_order_acquire);
    if (!load || !generation || motion.foreign.load(std::memory_order_acquire) ||
        motion.landing_writers.load(std::memory_order_acquire) ||
        motion.slip_interference.load(std::memory_order_acquire) != motion.interference.load(std::memory_order_acquire) ||
        motion.slip_load.load(std::memory_order_acquire) != load) return {};
    return {load, generation, landing};
}
static void slip_retire(unsigned d) noexcept {
    auto &s = slip_projection[d];
    s.address.store(0, std::memory_order_release);
    uint64_t owner = s.owner.exchange(0, std::memory_order_acq_rel);
    if (!owner) return;
    azsdk_load_sample load {};
    if (azsdk_load_read(int(d), &load)) {
        azstatus_native_receipt r {};
        r.kind = AZN_SLIP_HEAD;
        r.deck = d;
        r.unit = load.unit_generation;
        r.epoch = load.invalidation_epoch;
        r.load_generation = load.load_generation;
        r.attempt = owner;
        r.valid = AZ_AVAIL_STALE;
        r.b = r.c = 1;
        azn_emit(&r);
    }
    azn_capability(d, AZN_CAP_SLIP_HEAD, 0);
}
static void slip_sample(unsigned d, uintptr_t address, SlipProjection &s, SlipMotionCut motion) noexcept {
    if (!address || !motion.load || motion.load != s.load || s.writers.load(std::memory_order_acquire)) return;
    uint64_t stamp = s.stamp.load(std::memory_order_acquire), owner = s.owner.load(std::memory_order_acquire);
    if (!owner || s.address.load(std::memory_order_acquire) != address) return;
    unsigned running = byte(address + 0x68), paused = byte(address + 0x6a);
    int avail = running == 0 ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
    uint64_t sample = 0;
    if (running == 1 && !paused && s.rate && s.rate_load == s.load) {
        unsigned char track[12];
        memcpy(track, reinterpret_cast<const void *>(address + 0x38), sizeof track);
        sample = word(address + 0x48);
        const int range = compare_timestamps(sample, UINT64_C(9007199254740992), s.rate, s.rate, 1);
        if (range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL && azn_load_track_matches(d, s.epoch, s.load, track))
            avail = AZ_AVAIL_READY;
    }
    azsdk_load_sample live {};
    if (!azsdk_load_read(int(d), &live) || live.unit_generation != s.unit || live.invalidation_epoch != s.epoch ||
        live.load_generation != s.load || s.stamp.load(std::memory_order_acquire) != stamp ||
        s.writers.load(std::memory_order_acquire) || s.owner.load(std::memory_order_acquire) != owner) return;
    const SlipMotionCut after = slip_motion_cut(state().decks[d].motion);
    if (after.load != motion.load || after.generation != motion.generation || after.landing != motion.landing) return;
    uint64_t seed = azn_seed_id(d);
    bool cut = s.availability != avail || s.load != s.published_load || motion.generation != s.published_generation;
    const int64_t elapsed = s.elapsed_samples.load(std::memory_order_acquire);
    unsigned window = 0;
    const int positive = compare_timestamps(elapsed, 0, s.rate, s.rate, 1);
    const int capacity = compare_timestamps(elapsed, UINT32_MAX, s.rate, s.rate, 1);
    if (positive != AZ_TIME_INVALID && capacity != AZ_TIME_INVALID && positive == AZ_TIME_AFTER && capacity <= AZ_TIME_EQUAL)
        window = unsigned(elapsed);
    const int unchanged = compare_timestamps(s.sample, sample, s.rate, s.rate, window);
    if (!cut && s.seed == seed && (avail != AZ_AVAIL_READY || (window && s.published_rate == s.rate && unchanged != AZ_TIME_INVALID && unchanged == AZ_TIME_EQUAL))) return;
    azstatus_native_receipt r {};
    r.kind = AZN_SLIP_HEAD;
    r.deck = d;
    r.unit = s.unit;
    r.epoch = s.epoch;
    r.load_generation = s.load;
    r.motion_generation = motion.generation;
    r.attempt = owner;
    r.valid = avail;
    r.b = cut || s.seed != seed;
    r.observed_window_samples = window;
    r.observed_window_rate = window ? s.rate : 0;
    if (avail == AZ_AVAIL_READY) r.points[0] = {AZN_COORD, s.rate, static_cast<int64_t>(sample)};
    if (azn_offer(&r)) {
        s.availability = avail;
        s.published_load = s.load;
        s.published_generation = motion.generation;
        s.published_rate = s.rate;
        s.sample = sample;
        s.seed = seed;
    }
}
static void slip_frame(unsigned d, Slot &slot) noexcept {
    if (!slip_installed.load(std::memory_order_acquire) || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))) return;
    uintptr_t address = 0;
    if (coherent(slot)) {
        uintptr_t functions = word(slot.unit + 0x188), child = word(slot.facade + 0xa8);
        if (pointer(functions) && word(functions + 0x30) == child && table(child, state().profile.slip)) address = child;
    }
    auto &s = slip_projection[d];
    if (s.busy.test_and_set(std::memory_order_acquire)) return;
    if (s.address.load(std::memory_order_acquire) != address) {
        slip_retire(d);
        s.availability = -1;
        s.published_load = 0;
        s.rate_load = 0;
        s.rate = 0;
        s.elapsed_samples.store(0, std::memory_order_release);
        if (address) {
            uint64_t id = slip_owners.fetch_add(1, std::memory_order_relaxed) + 1;
            if (id && id <= AZSP_SERIAL_MAX) {
                s.owner.store(AZSP_SLIP | id, std::memory_order_release);
                s.address.store(address, std::memory_order_release);
                azn_capability(d, AZN_CAP_SLIP_HEAD, 1);
            }
        }
    }
    azsdk_load_sample live {};
    if (address && azsdk_load_read(int(d), &live)) {
        s.unit = live.unit_generation;
        s.epoch = live.invalidation_epoch;
        s.load = live.load_generation;
        const SlipMotionCut motion = slip_motion_cut(slot.motion);
        if (s.published_load != s.load || s.published_generation != motion.generation ||
            s.availability != AZ_AVAIL_EMPTY || byte(address + 0x68) || s.seed != azn_seed_id(d)) {
            auto head = status_head(slot.core.load(std::memory_order_acquire));
            s.rate = head.ready && azn_load_track_matches(d, s.epoch, s.load, head.source) ? head.rate : 0;
            s.rate_load = s.rate ? s.load : 0;
            slip_sample(d, address, s, motion);
        }
    } else s.load = 0;
    s.busy.clear(std::memory_order_release);
}
static void slip_writer(void *self, int64_t elapsed) {
    auto original = trampoline<void (*)(void *, int64_t)>(&slip_original);
    unsigned d = 0;
    uintptr_t address = reinterpret_cast<uintptr_t>(self);
    if (!byte(address + 0x68) && !byte(address + 0x69)) {
        original(self, elapsed);
        return;
    }
    for (; d < 4; d++)
        if (slip_projection[d].address.load(std::memory_order_acquire) == address) break;
    if (d == 4) {
        original(self, elapsed);
        return;
    }
    auto &s = slip_projection[d];
    const SlipMotionCut motion = slip_motion_cut(state().decks[d].motion);
    struct Writer {
        SlipProjection &s;
        Writer(SlipProjection &s, unsigned d) : s(s) {
            if (s.writers.fetch_add(1, std::memory_order_acq_rel)) azn_slip_gap(d);
        }
        ~Writer() {
            s.stamp.fetch_add(1, std::memory_order_release);
            s.writers.fetch_sub(1, std::memory_order_release);
        }
    };
    {
        Writer guard(s, d);
        original(self, elapsed);
        /* UpdateBGPositionTask::doPeriodicTask(meow::actual_time::Sample)
         * forwards this elapsed frame count directly. updateBGPosition scales
         * it by the slip speed before adding it to the source-sample clock. */
        s.elapsed_samples.store(elapsed, std::memory_order_release);
    }
    if (!s.busy.test_and_set(std::memory_order_acquire)) {
        slip_sample(d, address, s, motion);
        s.busy.clear(std::memory_order_release);
    }
}
static void slip_install(State &s, int build, azn_setting_patch_fn patch) noexcept {
    if (slip_installed.load(std::memory_order_acquire)) return;
    uintptr_t writer = s.bias + (build == 1 ? 0x146a6b0 : 0x1525d38);
    const uint32_t signature[] = {0x3941a002, 0x3941a403, 0x6b02007f, 0x54000080};
    if (!memcmp(reinterpret_cast<const void *>(writer), signature, sizeof signature) &&
        patch(writer, reinterpret_cast<void *>(slip_writer), &slip_original, "status-slip-head") == 1 && slip_original)
        slip_installed.store(1, std::memory_order_release);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* UpdateBGPositionTask commits a source TrackID and a signed source-sample
 * coordinate at SlipFunction +0x38/+0x48. The audible core supplies the rate
 * only for the same TrackID, unit and matched load. */
struct SlipProjection {
    _Atomic(uintptr_t) address;
    _Atomic(uint64_t) owner;
    _Atomic(uint64_t) stamp;
    _Atomic(unsigned) writers;
    _Atomic(int64_t) elapsed_samples;
    atomic_flag busy;
    uint64_t epoch;
    uint64_t load;
    uint64_t unit;
    uint64_t seed;
    uint64_t rate_load;
    uint64_t sample;
    uint64_t published_load;
    uint64_t published_generation;
    uint32_t rate;
    uint32_t published_rate;
    int availability;
};
static SlipProjection slip_projection[4] = {[0 ... 3] = {.address = 0, .owner = 0, .stamp = 0, .writers = 0, .elapsed_samples = 0, .busy = ATOMIC_FLAG_INIT, .epoch = 0, .load = 0, .unit = 0, .seed = 0, .rate_load = 0, .sample = 0, .published_load = 0, .published_generation = 0, .rate = 0, .published_rate = 0, .availability = -1}};
static _Atomic(uint64_t) slip_owners = 0;
static _Atomic(unsigned) slip_installed = 0;
static void *slip_original;
struct SlipMotionCut {
    uint64_t load, generation, landing;
};
static SlipMotionCut slip_motion_cut(const Motion *motion) {
    const uint64_t load = atomic_load_explicit(&(motion->slip_load), memory_order_acquire);
    const uint64_t generation = atomic_load_explicit(&(motion->slip_generation), memory_order_acquire);
    const uint64_t landing = atomic_load_explicit(&(motion->landing_order), memory_order_acquire);
    if (!load || !generation || atomic_load_explicit(&(motion->foreign), memory_order_acquire) ||
        atomic_load_explicit(&(motion->landing_writers), memory_order_acquire) ||
        atomic_load_explicit(&(motion->slip_interference), memory_order_acquire) != atomic_load_explicit(&(motion->interference), memory_order_acquire) ||
        atomic_load_explicit(&(motion->slip_load), memory_order_acquire) != load) return (SlipMotionCut) {0};
    return (SlipMotionCut) {load, generation, landing};
}
static void slip_retire(unsigned d) {
    __auto_type s = &(slip_projection[d]);
    atomic_store_explicit(&(s->address), 0, memory_order_release);
    uint64_t owner = atomic_exchange_explicit(&(s->owner), 0, memory_order_acq_rel);
    if (!owner) return;
    azsdk_load_sample load = {0};
    if (azsdk_load_read((int) (d), &load)) {
        azstatus_native_receipt r = {0};
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
static void slip_sample(unsigned d, uintptr_t address, SlipProjection *s, SlipMotionCut motion) {
    if (!address || !motion.load || motion.load != s->load || atomic_load_explicit(&(s->writers), memory_order_acquire)) return;
    uint64_t stamp = atomic_load_explicit(&(s->stamp), memory_order_acquire), owner = atomic_load_explicit(&(s->owner), memory_order_acquire);
    if (!owner || atomic_load_explicit(&(s->address), memory_order_acquire) != address) return;
    unsigned running = byte(address + 0x68), paused = byte(address + 0x6a);
    int avail = running == 0 ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
    uint64_t sample = 0;
    if (running == 1 && !paused && s->rate && s->rate_load == s->load) {
        unsigned char track[12];
        memcpy(track, ((const void *) (address + 0x38)), sizeof track);
        sample = word(address + 0x48);
        const int range = compare_timestamps(sample, UINT64_C(9007199254740992), s->rate, s->rate, 1);
        if (range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL && azn_load_track_matches(d, s->epoch, s->load, track))
            avail = AZ_AVAIL_READY;
    }
    azsdk_load_sample live = {0};
    if (!azsdk_load_read((int) (d), &live) || live.unit_generation != s->unit || live.invalidation_epoch != s->epoch ||
        live.load_generation != s->load || atomic_load_explicit(&(s->stamp), memory_order_acquire) != stamp ||
        atomic_load_explicit(&(s->writers), memory_order_acquire) || atomic_load_explicit(&(s->owner), memory_order_acquire) != owner) return;
    const SlipMotionCut after = slip_motion_cut(&(state().decks[d].motion));
    if (after.load != motion.load || after.generation != motion.generation || after.landing != motion.landing) return;
    uint64_t seed = azn_seed_id(d);
    bool cut = s->availability != avail || s->load != s->published_load || motion.generation != s->published_generation;
    const int64_t elapsed = atomic_load_explicit(&(s->elapsed_samples), memory_order_acquire);
    unsigned window = 0;
    const int positive = compare_timestamps(elapsed, 0, s->rate, s->rate, 1);
    const int capacity = compare_timestamps(elapsed, UINT32_MAX, s->rate, s->rate, 1);
    if (positive != AZ_TIME_INVALID && capacity != AZ_TIME_INVALID && positive == AZ_TIME_AFTER && capacity <= AZ_TIME_EQUAL)
        window = (unsigned) (elapsed);
    const int unchanged = compare_timestamps(s->sample, sample, s->rate, s->rate, window);
    if (!cut && s->seed == seed && (avail != AZ_AVAIL_READY || (window && s->published_rate == s->rate && unchanged != AZ_TIME_INVALID && unchanged == AZ_TIME_EQUAL))) return;
    azstatus_native_receipt r = {0};
    r.kind = AZN_SLIP_HEAD;
    r.deck = d;
    r.unit = s->unit;
    r.epoch = s->epoch;
    r.load_generation = s->load;
    r.motion_generation = motion.generation;
    r.attempt = owner;
    r.valid = avail;
    r.b = cut || s->seed != seed;
    r.observed_window_samples = window;
    r.observed_window_rate = window ? s->rate : 0;
    if (avail == AZ_AVAIL_READY) r.points[0] = (__typeof__(r.points[0])) {AZN_COORD, s->rate, ((int64_t) (sample))};
    if (azn_offer(&r)) {
        s->availability = avail;
        s->published_load = s->load;
        s->published_generation = motion.generation;
        s->published_rate = s->rate;
        s->sample = sample;
        s->seed = seed;
    }
}
static void slip_frame(unsigned d, Slot *slot) {
    if (!atomic_load_explicit(&(slip_installed), memory_order_acquire) || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))) return;
    uintptr_t address = 0;
    if (coherent(slot)) {
        uintptr_t functions = word(slot->unit + 0x188), child = word(slot->facade + 0xa8);
        if (pointer(functions) && word(functions + 0x30) == child && table(child, state().profile.slip)) address = child;
    }
    __auto_type s = &(slip_projection[d]);
    if (atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) return;
    if (atomic_load_explicit(&(s->address), memory_order_acquire) != address) {
        slip_retire(d);
        s->availability = -1;
        s->published_load = 0;
        s->rate_load = 0;
        s->rate = 0;
        atomic_store_explicit(&(s->elapsed_samples), 0, memory_order_release);
        if (address) {
            uint64_t id = atomic_fetch_add_explicit(&(slip_owners), 1, memory_order_relaxed) + 1;
            if (id && id <= AZSP_SERIAL_MAX) {
                atomic_store_explicit(&(s->owner), AZSP_SLIP | id, memory_order_release);
                atomic_store_explicit(&(s->address), address, memory_order_release);
                azn_capability(d, AZN_CAP_SLIP_HEAD, 1);
            }
        }
    }
    azsdk_load_sample live = {0};
    if (address && azsdk_load_read((int) (d), &live)) {
        s->unit = live.unit_generation;
        s->epoch = live.invalidation_epoch;
        s->load = live.load_generation;
        const SlipMotionCut motion = slip_motion_cut(&(slot->motion));
        if (s->published_load != s->load || s->published_generation != motion.generation ||
            s->availability != AZ_AVAIL_EMPTY || byte(address + 0x68) || s->seed != azn_seed_id(d)) {
            __auto_type head = status_head(atomic_load_explicit(&(slot->core), memory_order_acquire));
            s->rate = head.ready && azn_load_track_matches(d, s->epoch, s->load, head.source) ? head.rate : 0;
            s->rate_load = s->rate ? s->load : 0;
            slip_sample(d, address, &((*s)), motion);
        }
    } else s->load = 0;
    atomic_flag_clear_explicit(&(s->busy), memory_order_release);
}
typedef struct SlipWriter SlipWriter;
struct SlipWriter {
    SlipProjection *s;
};
static void slip_writer_scope_init(SlipWriter *ctx, SlipProjection *s, unsigned d);
static void slip_writer_scope_drop(SlipWriter *ctx);
static void slip_writer_scope_init(SlipWriter *ctx, SlipProjection *s, unsigned d) {
    (void) ctx;
    *ctx = (SlipWriter) {.s = s};

    if (atomic_fetch_add_explicit(&(s->writers), 1, memory_order_acq_rel)) azn_slip_gap(d);
}
static void slip_writer_scope_drop(SlipWriter *ctx) {
    (void) ctx;

    atomic_fetch_add_explicit(&(ctx->s->stamp), 1, memory_order_release);
    atomic_fetch_sub_explicit(&(ctx->s->writers), 1, memory_order_release);
}

static void slip_writer(void *self, int64_t elapsed) {
    __auto_type original = ((void (*)(void *, int64_t)) trampoline_address(&slip_original));
    unsigned d = 0;
    uintptr_t address = ((uintptr_t) (self));
    if (!byte(address + 0x68) && !byte(address + 0x69)) {
        original(self, elapsed);
        return;
    }
    for (; d < 4; d++)
        if (atomic_load_explicit(&(slip_projection[d].address), memory_order_acquire) == address) break;
    if (d == 4) {
        original(self, elapsed);
        return;
    }
    __auto_type s = &(slip_projection[d]);
    const SlipMotionCut motion = slip_motion_cut(&(state().decks[d].motion));

    {
        SlipWriter guard __attribute__((cleanup(slip_writer_scope_drop)));
        slip_writer_scope_init(&guard, &((*s)), d);
        original(self, elapsed);
        /* UpdateBGPositionTask::doPeriodicTask(meow::actual_time::Sample)
         * forwards this elapsed frame count directly. updateBGPosition scales
         * it by the slip speed before adding it to the source-sample clock. */
        atomic_store_explicit(&(s->elapsed_samples), elapsed, memory_order_release);
    }
    if (!atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) {
        slip_sample(d, address, &((*s)), motion);
        atomic_flag_clear_explicit(&(s->busy), memory_order_release);
    }
}
static void slip_install(State *s, int build, azn_setting_patch_fn patch) {
    if (atomic_load_explicit(&(slip_installed), memory_order_acquire)) return;
    uintptr_t writer = s->bias + (build == 1 ? 0x146a6b0 : 0x1525d38);
    const uint32_t signature[] = {0x3941a002, 0x3941a403, 0x6b02007f, 0x54000080};
    if (!memcmp(((const void *) (writer)), signature, sizeof signature) &&
        patch(writer, ((void *) (slip_writer)), &slip_original, "status-slip-head") == 1 && slip_original)
        atomic_store_explicit(&(slip_installed), 1, memory_order_release);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* The manager's speed getter returns a queued target. The selected engine
 * commits its current signed speed separately at +0x10. Its render uses the
 * actual per-sample speeds for source traversal, including vinyl ramps. */
struct RateProfile {
    uintptr_t readable, manager, mode, pitch, scratch;
    uintptr_t sites[4];
    uint32_t first[4];
};
static const RateProfile rate_profiles[2] = {
    {0x27b82e8, 0x27b8a50, 0x27b8c28, 0x27bb150, 0x27bb7b0, {0x1359b40, 0x1359bf8, 0xda9950, 0xdad478}, {0x39406002, 0xf9415801, 0x1e624001, 0xfd000400}},
    {0x2b04fa0, 0x2b05708, 0x2b058e0, 0x2b07df0, 0x2b08450, {0x1414eb0, 0x1414f68, 0xe343c8, 0xe37ed0}, {0x39406002, 0xf9415801, 0x1e624001, 0xfd000400}},
};
struct RateState {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<uintptr_t> bound_engine {0};
    std::atomic<uint64_t> fault {0};
    uint64_t load = 0, seed = 0, observed_fault = 0, bits = 0;
    unsigned availability = AZ_AVAIL_UNSUPPORTED, direction_avail = AZ_AVAIL_UNSUPPORTED;
    int direction = 0;
    bool rate_ramp = false, direction_ramp = false;
};
static RateState rate_state[4];
static std::atomic<unsigned> rate_hooks {0};
static void *rate_original[4];
static std::atomic<bool> rate_generator_hooked {false};
static void *rate_generator_original;
static uintptr_t rate_engine(uintptr_t core, const State &s, const RateProfile &p) noexcept {
    uintptr_t readable = word(core + 0xa8);
    if (!table(readable, s.bias + p.readable)) return 0;
    uintptr_t manager = word(readable + 0x18);
    if (!table(manager, s.bias + p.manager)) return 0;
    uintptr_t mode = word(manager + 0x18);
    if (!table(mode, s.bias + p.mode)) return 0;
    unsigned which = unsigned(integer(mode + 0x18));
    if (which > 2) return 0;
    uintptr_t engine = word(manager + (which == 2 ? 0x40u : 0x38u));
    return (table(engine, s.bias + p.scratch) || table(engine, s.bias + p.pitch)) ? engine : 0;
}
static int rate_sign(double r) noexcept { return (r > 0) - (r < 0); }
static bool rate_number(double r) noexcept { return std::isfinite(r) && r >= -8 && r <= 8; }
static double rate_double(uintptr_t where) noexcept {
    uint64_t bits = __atomic_load_n(reinterpret_cast<const uint64_t *>(where), __ATOMIC_ACQUIRE);
    double r;
    std::memcpy(&r, &bits, sizeof r);
    return r;
}
/* An active PlayerCore can clear an unreadable block without invoking the
 * source-rate generator. Its prior vector is not evidence for that call. */
class RateRenderScope {
    static thread_local RateRenderScope *current;
    RateRenderScope *previous;
    Slot *slot = nullptr;
    uintptr_t core = 0, engine = 0;
    azsdk_load_sample load {};
    unsigned char source[12] = {};
    unsigned deck = 0, calls = 0;
    int64_t generated_count = 0;
    bool internal = false, nested = false, other = false;
    bool unchanged_source() const noexcept {
        if (!internal || !slot || processing != slot || slot->core.load(std::memory_order_acquire) != core) return false;
        const auto &s = state();
        if (s.build < 1 || s.build > 2) return false;
        const auto &profile = rate_profiles[s.build - 1];
        azsdk_load_sample after {};
        return rate_engine(core, s, profile) == engine &&
            !(word(engine) == s.bias + profile.scratch && byte(engine + 0x58)) &&
            !std::memcmp(source, reinterpret_cast<const void *>(core + 0x18), sizeof source) &&
            azsdk_load_read(int(deck), &after) && after.unit_generation == load.unit_generation &&
            after.invalidation_epoch == load.invalidation_epoch && after.load_generation == load.load_generation &&
            azn_load_track_matches(deck, after.invalidation_epoch, after.load_generation, source);
    }
   public:
    explicit RateRenderScope(void *self) noexcept : previous(current), core(reinterpret_cast<uintptr_t>(self)) {
        if (previous) previous->nested = true;
        current = this;
        const auto &s = state();
        if (!rate_generator_hooked.load(std::memory_order_acquire) || !processing ||
            processing->core.load(std::memory_order_acquire) != core || s.build < 1 || s.build > 2) return;
        slot = processing;
        deck = unsigned(slot - s.decks);
        if (!azsdk_load_read(int(deck), &load)) return;
        std::memcpy(source, reinterpret_cast<const void *>(core + 0x18), sizeof source);
        const auto &profile = rate_profiles[s.build - 1];
        engine = rate_engine(core, s, profile);
        internal = engine && !(word(engine) == s.bias + profile.scratch && byte(engine + 0x58)) &&
            azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation, source);
    }
    ~RateRenderScope() { current = previous; }
    static void generated(uintptr_t self, int64_t frames = 0) noexcept {
        if (!current) return;
        if (current->engine == self) {
            ++current->calls;
            current->generated_count = frames;
        }
        else current->other = true;
    }
    int measured_frames(int requested) const noexcept {
        if (requested <= 0 || calls || nested || other || !unchanged_source()) return requested;
        return 0;
    }
    static bool stationary(uintptr_t core, unsigned frames) noexcept {
        const auto *r = current;
        if (!r || r->core != core || !frames || frames > 1024 || r->calls > 1 || r->nested || r->other ||
            !r->unchanged_source()) return false;
        /* The qualified internal no-generator branch clears an unreadable
         * block. Otherwise require this call's actual generated advances;
         * an old zero vector cannot hide a source excursion. */
        if (!r->calls) return true;
        if (r->generated_count <= 0 || uint64_t(r->generated_count) != frames) return false;
        uintptr_t samples = word(r->engine + 0x18);
        if (!pointer(samples) || frames > word(r->engine + 0x20)) return false;
        for (unsigned n = 0; n < frames; ++n)
            if (rate_double(samples + sizeof(double) * n) != 0) return false;
        return true;
    }
};
thread_local RateRenderScope *RateRenderScope::current = nullptr;
static void rate_generator(void *self, int64_t frames) {
    trampoline<void (*)(void *, int64_t)>(&rate_generator_original)(self, frames);
    RateRenderScope::generated(reinterpret_cast<uintptr_t>(self), frames);
}
static void rate_capture(unsigned deck, Slot &slot, uintptr_t event_engine = 0, int frames = 0) noexcept {
    State &s = state();
    if (rate_hooks.load(std::memory_order_acquire) != 15 || !rate_generator_hooked.load(std::memory_order_acquire) ||
        !(azn_interest(deck) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) return;
    azsdk_load_sample load {};
    if (!azsdk_load_read(int(deck), &load)) return;
    auto &r = rate_state[deck];
    if (r.busy.test_and_set(std::memory_order_acquire)) {
        r.fault.fetch_add(1, std::memory_order_acq_rel);
        azn_rate_gap(deck);
        return;
    }
    struct Unlock {
        RateState &s;
        ~Unlock() { s.busy.clear(std::memory_order_release); }
    } guard {r};
    const uintptr_t core = slot.core.load(std::memory_order_acquire);
    if (!core || s.build < 1 || s.build > 2) return;
    const RateProfile &profile = rate_profiles[s.build - 1];
    const uintptr_t engine = rate_engine(core, s, profile);
    r.bound_engine.store(engine, std::memory_order_release);
    if (event_engine && event_engine != engine) return;
    if (!azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation,
                                reinterpret_cast<const unsigned char *>(core + 0x18))) return;
    uint64_t fault = r.fault.load(std::memory_order_acquire), seed = azn_seed_id(deck);
    unsigned avail = AZ_AVAIL_READY, dir_avail = AZ_AVAIL_READY;
    bool loss = false;
    double speed = 0;
    if (load.load_generation != r.load || !byte(core + 9)) r.rate_ramp = r.direction_ramp = false;
    if (byte(core + 9)) {
        if (!engine) avail = dir_avail = AZ_AVAIL_UNSUPPORTED;
        else {
            speed = rate_double(engine + 0x10);
            if (!rate_number(speed)) avail = dir_avail = AZ_AVAIL_STALE;
            if (avail == AZ_AVAIL_READY && frames > 0) {
                const bool external = word(engine) == s.bias + profile.scratch && byte(engine + 0x58);
                uintptr_t samples = word(engine + (external ? 0x60u : 0x18u));
                if (!pointer(samples) || frames > 1024 || uint64_t(frames) > word(engine + 0x20)) avail = dir_avail = AZ_AVAIL_STALE;
                else {
                    /* +0x10 is the last committed advance, not a promise that
                     * the whole block used one speed. The native ramp writer
                     * stores every advance here and leaves the final one in
                     * +0x10. Publish that measured current rate through ramps.
                     * Opposite nonzero signs still invalidate the block's
                     * direction history; pauses within one direction do not. */
                    int direction_seen = 0;
                    double last = 0;
                    for (int n = 0; n < frames; ++n) {
                        double sample = rate_double(samples + sizeof(double) * unsigned(n));
                        if (!rate_number(sample)) {
                            avail = dir_avail = AZ_AVAIL_STALE;
                            break;
                        }
                        int sign = rate_sign(sample);
                        if (sign && direction_seen && sign != direction_seen) dir_avail = AZ_AVAIL_STALE;
                        if (sign) direction_seen = sign;
                        last = sample;
                    }
                    if (last != speed) avail = dir_avail = AZ_AVAIL_STALE;
                }
                r.rate_ramp = avail == AZ_AVAIL_STALE;
                r.direction_ramp = dir_avail == AZ_AVAIL_STALE;
            }
            if (r.rate_ramp) avail = AZ_AVAIL_STALE;
            if (r.direction_ramp) dir_avail = AZ_AVAIL_STALE;
        }
    }
    if (avail != AZ_AVAIL_READY) speed = dir_avail == AZ_AVAIL_READY ? rate_sign(speed) : 0;
    int dir = rate_sign(speed);
    double magnitude = avail == AZ_AVAIL_READY ? std::fabs(speed) : 0;
    uint64_t bits;
    std::memcpy(&bits, &magnitude, sizeof bits);
    azsdk_load_sample after {};
    if (!azsdk_load_read(int(deck), &after) || after.load_generation != load.load_generation ||
        after.invalidation_epoch != load.invalidation_epoch || after.unit_generation != load.unit_generation ||
        !azn_load_track_matches(deck, after.invalidation_epoch, after.load_generation, reinterpret_cast<const unsigned char *>(core + 0x18)) ||
        r.fault.load(std::memory_order_acquire) != fault) return;
    if (r.load == load.load_generation) {
        loss = (r.observed_fault != fault || (r.availability == AZ_AVAIL_READY && avail == AZ_AVAIL_STALE) ||
                (r.direction_avail == AZ_AVAIL_READY && dir_avail == AZ_AVAIL_STALE));
        if (!loss && r.seed == seed && r.availability == avail && r.direction_avail == dir_avail && r.direction == dir && r.bits == bits) return;
    }
    azstatus_native_receipt out {};
    out.kind = AZN_RATE;
    out.deck = deck;
    out.unit = load.unit_generation;
    out.epoch = load.invalidation_epoch;
    out.load_generation = load.load_generation;
    out.attempt = load.unit_generation;
    out.valid = int(avail);
    out.b = int(dir_avail);
    out.a = dir;
    std::memcpy(&out.points[0].samples, &bits, sizeof bits);
    if (loss) out.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    if (!azn_offer(&out)) return;
    r.load = load.load_generation;
    r.seed = seed;
    r.observed_fault = fault;
    r.availability = avail;
    r.direction_avail = dir_avail;
    r.direction = dir;
    r.bits = bits;
}
static void rate_writer_done(unsigned kind, uintptr_t self) noexcept {
    State &s = state();
    if (processing) {
        const unsigned d = unsigned(processing - s.decks);
        if (kind >= 2 || processing->core.load(std::memory_order_acquire) == self)
            rate_capture(d, *processing, kind >= 2 ? self : 0);
        return;
    }
    for (unsigned d = 0; d < 4; ++d) {
        bool ours = kind < 2 ? s.decks[d].core.load(std::memory_order_acquire) == self : rate_state[d].bound_engine.load(std::memory_order_acquire) == self;
        if (ours && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
            rate_state[d].fault.fetch_add(1, std::memory_order_acq_rel);
            azn_rate_gap(d);
        }
    }
}
template <unsigned I, class... Args>
static void rate_writer(void *self, Args... args) {
    /* These two originals own Core+9. Fence every actual start/stop call,
     * including calls outside the current render owner and with no watch. */
    Slot::Motion *motion = nullptr;
    if (I < 2) {
        for (auto &slot : state().decks) {
            if (slot.core.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(self)) {
                motion = &slot.motion;
                break;
            }
        }
    }
    {
        struct ActiveWrite {
            Slot::Motion *motion;
            explicit ActiveWrite(Slot::Motion *value) : motion(value) {
                if (motion) {
                    motion->active_writers.fetch_add(1, std::memory_order_acq_rel);
                    motion->active_serial.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            ~ActiveWrite() {
                if (motion) {
                    motion->active_serial.fetch_add(1, std::memory_order_release);
                    motion->active_writers.fetch_sub(1, std::memory_order_release);
                }
            }
        } guard(motion);
        trampoline<void (*)(void *, Args...)> (&rate_original[I])(self, args...);
    }
    rate_writer_done(I, reinterpret_cast<uintptr_t>(self));
}
static void rate_install(State &s, int build, azn_setting_patch_fn patch) {
    const RateProfile &p = rate_profiles[build - 1];
    void *hooks[] = {reinterpret_cast<void *>(rate_writer<0, unsigned>), reinterpret_cast<void *>(rate_writer<1>),
                     reinterpret_cast<void *>(rate_writer<2, double, unsigned>), reinterpret_cast<void *>(rate_writer<3, double, unsigned>)};
    for (unsigned i = 0; i < 4; ++i)
        if (!(rate_hooks.load(std::memory_order_acquire) & (1u << i))) {
            uintptr_t site = s.bias + p.sites[i];
            if (std::memcmp(reinterpret_cast<const void *>(site), &p.first[i], 4) ||
                patch(site, hooks[i], &rate_original[i], "status-rate") != 1 || !rate_original[i]) return;
            rate_hooks.fetch_or(1u << i, std::memory_order_release);
        }
    if (!rate_generator_hooked.load(std::memory_order_acquire)) {
        const uintptr_t site = s.bias + (build == 1 ? 0xdb21e0u : 0xe3ca30u);
        const uint32_t first[2][4] = {{0x6d408002, 0xd2e7d002, 0x9e670043, 0xd10043ff},
                                    {0x6d408402, 0xd2e7d002, 0x9e670044, 0x7ee1d440}};
        if (std::memcmp(reinterpret_cast<const void *>(site), first[build - 1], sizeof first[0]) ||
            patch(site, reinterpret_cast<void *>(rate_generator), &rate_generator_original, "status-rate-render") != 1 ||
            !rate_generator_original) return;
        rate_generator_hooked.store(true, std::memory_order_release);
    }
    for (unsigned d = 0; d < 4; ++d)
        if (s.decks[d].core.load(std::memory_order_acquire)) azn_capability(d, AZN_CAP_RATE, 1);
}

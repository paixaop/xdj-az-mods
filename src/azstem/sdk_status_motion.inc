/* SPDX-License-Identifier: GPL-3.0-only */
/* PlayerCore owns both of these writes. processContinuousBlock compares the
 * returned Readable position's SourceInfo before committing Core+0x28;
 * setCurrentPos commits the explicit target. The GUI copy at UnitStateServer
 * +0x58 has already lost this ordering. */
static constexpr uint64_t motion_routes = AZ_STATUS_TOPIC(AZ_STATUS_POSITION) |
                                          AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
static void *status_continuous_original;
static void *status_limit_original;
static void core_jump_hook(void *, const void *);

struct StatusHead {
    uint64_t sample = 0;
    uint32_t rate = 0;
    unsigned char source[12] = {};
    bool ready = false;
};
static unsigned status_action(Slot &, const StatusHead &, const StatusHead &) noexcept;
static bool status_jump_origin(const Slot &) noexcept;
static StatusHead status_head(uintptr_t core) noexcept {
    StatusHead h;
    uint64_t source = __atomic_load_n(reinterpret_cast<const uint64_t *>(core + 0x18), __ATOMIC_ACQUIRE);
    uint32_t id = __atomic_load_n(reinterpret_cast<const uint32_t *>(core + 0x20), __ATOMIC_ACQUIRE);
    memcpy(h.source, &source, 8);
    memcpy(h.source + 8, &id, 4);
    h.sample = __atomic_load_n(reinterpret_cast<const uint64_t *>(core + 0x28), __ATOMIC_ACQUIRE);
    h.rate = __atomic_load_n(reinterpret_cast<const uint32_t *>(core + 0x68), __ATOMIC_ACQUIRE);
    const int range = compare_timestamps(h.sample, UINT64_C(9007199254740992), h.rate, h.rate, 1);
    h.ready = h.source[0] && (h.source[4] & 0x1f) && h.source[6] && id && h.rate &&
              h.rate < 0x7fffffffU && range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL;
    return h;
}
/* The stock load adopts through its owned BackCue callback, then copies the
 * committed point through controller-only calls. A controller scope alone is
 * not an action classification. These cases still require the exact pending
 * load/source and consecutive same-position commits below. */
static bool status_load_setup_scope(Slot &slot, bool adopted) noexcept {
    if (causal.slot) return false;
    auto scope = StatusActionScope::current;
    if (!scope) return true;
    if (!scope->controller || scope->command || scope->beat || scope->direct_kind) return false;
    if (!scope->callback) return !adopted;
    uintptr_t pause = 0, cue = 0;
    return adopted && transport_owners(slot, pause, cue) &&
        table(scope->callback, state().profile.commands[7].table) &&
        word(scope->callback + 0x30) == cue && word(scope->callback + 0x38) == pause;
}
/* setCurrentPos can adopt the new source before TrackLoadFacade's completion
 * and outside the audio owner frame. Its original owns self and requested for
 * this call. Retain only scalars, and reject an overlapping/delayed setter. */
class StatusLanding {
    Slot *slot = nullptr;
    unsigned deck = 0;
    uintptr_t core;
    uint64_t stamp = 0, epoch = 0, request = 0, render_cut = 0;
    bool exclusive = false;
    StatusHead from {};
   public:
    explicit StatusLanding(void *self) noexcept : core(reinterpret_cast<uintptr_t>(self)) {
        State &s = state();
        if (!(s.enabled.load(std::memory_order_acquire) & AZEXIT_PASSIVE_STATUS) || !s.motion_hooked) return;
        for (deck = 0; deck < 4; ++deck)
            if (s.decks[deck].core.load(std::memory_order_acquire) == core) {
                slot = &s.decks[deck];
                exclusive = slot->motion.landing_writers.fetch_add(1, std::memory_order_acq_rel) == 0;
                stamp = slot->motion.landing_order.fetch_add(1, std::memory_order_acq_rel) + 1;
                epoch = azn_lifecycle_fence(deck);
                request = azn_load_pending(deck, epoch, nullptr);
                render_cut = slot->motion.adoption_render.load(std::memory_order_acquire);
                from = status_head(core);
                break;
            }
    }
    ~StatusLanding() {
        if (slot) slot->motion.landing_writers.fetch_sub(1, std::memory_order_release);
    }
    void finish(uintptr_t requested) noexcept {
        if (!slot || !exclusive || !requested || !stamp || !epoch || slot->core.load(std::memory_order_acquire) != core) return;
        StatusHead to = status_head(core);
        const int target_order = compare_timestamps(word(requested + 0x18), to.sample, to.rate, to.rate, 1);
        if (!to.ready || target_order == AZ_TIME_INVALID || target_order != AZ_TIME_EQUAL ||
            std::memcmp(reinterpret_cast<const void *>(requested + 8), to.source, 12) ||
            azn_lifecycle_fence(deck) != epoch) return;
        auto &m = slot->motion;
        if (m.landing_publish.test_and_set(std::memory_order_acquire)) return;
        struct Unlock {
            Slot::Motion &m;
            ~Unlock() { m.landing_publish.clear(std::memory_order_release); }
        } unlock {m};
        if (m.landing_order.load(std::memory_order_acquire) != stamp || m.landing_writers.load(std::memory_order_acquire) != 1) return;
        uint64_t lo;
        uint32_t hi;
        std::memcpy(&lo, to.source, 8);
        std::memcpy(&hi, to.source + 8, 4);
        if (!std::memcmp(from.source, to.source, 12) &&
            !(stamp > 1 && m.landing_stamp.load(std::memory_order_acquire) == stamp - 1 && m.landing_epoch.load(std::memory_order_acquire) == epoch &&
              m.landing_lo.load(std::memory_order_acquire) == lo && m.landing_hi.load(std::memory_order_acquire) == hi)) return;
        m.landing_lo.store(lo, std::memory_order_release);
        m.landing_hi.store(hi, std::memory_order_release);
        const bool adopted = std::memcmp(from.source, to.source, 12) != 0;
        const bool pending = request && azn_load_pending(deck, epoch, to.source) == request &&
                             status_load_setup_scope(*slot, adopted) &&
                             !m.adoption_render_writers.load(std::memory_order_acquire) &&
                             m.adoption_render.load(std::memory_order_acquire) == render_cut;
        if (adopted && (azn_interest(deck) & motion_routes)) {
            m.opening_landing.store(stamp, std::memory_order_release);
            m.adoption_sample.store(to.sample, std::memory_order_release);
            m.adoption_rate.store(to.rate, std::memory_order_release);
            m.adoption_render_cut = render_cut;
            m.adoption_request = pending ? request : 0;
        } else if (!adopted && pending && m.adoption_request == request &&
                   m.adoption_render_cut == render_cut && m.opening_landing.load(std::memory_order_acquire) == stamp - 1 &&
                   from.ready && from.rate == to.rate && m.adoption_rate.load(std::memory_order_acquire) == to.rate) {
            /* Stock load setup can copy its first nonzero source position
             * several times before completion. Preserve only consecutive
             * exact copies of that adoption, with no intervening render or
             * action. A later seek or excursion can never extend this cut. */
            const int before = compare_timestamps(from.sample, m.adoption_sample.load(std::memory_order_acquire), to.rate, to.rate, 1);
            const int after = compare_timestamps(to.sample, m.adoption_sample.load(std::memory_order_acquire), to.rate, to.rate, 1);
            if (before != AZ_TIME_INVALID && after != AZ_TIME_INVALID &&
                before == AZ_TIME_EQUAL && after == AZ_TIME_EQUAL)
                m.opening_landing.store(stamp, std::memory_order_release);
        }
        m.landing_epoch.store(epoch, std::memory_order_release);
        m.landing_stamp.store(stamp, std::memory_order_release);
    }
};

#include "sdk_status_map_clock.inc"
#include "sdk_status_end.inc"

/* A background-clock reader can borrow a generation only after the motion
 * receipt is sealed. A failed barrier leaves that generation unavailable. */
static bool status_offer_motion(Slot::Motion &motion, azstatus_native_receipt &receipt) noexcept {
    const bool renew = motion.slip_load.load(std::memory_order_relaxed) != receipt.load_generation ||
                       motion.slip_generation.load(std::memory_order_relaxed) != receipt.motion_generation ||
                       motion.slip_interference.load(std::memory_order_relaxed) != motion.captured_interference;
    if (renew) motion.slip_load.store(0, std::memory_order_release);
    if (!azn_offer(&receipt)) return false;
    if (renew) {
        motion.slip_interference.store(motion.captured_interference, std::memory_order_relaxed);
        motion.slip_generation.store(receipt.motion_generation, std::memory_order_relaxed);
        motion.slip_load.store(receipt.load_generation, std::memory_order_release);
    }
    return true;
}

/* Caller owns OBSERVE or HEAD. Load completion may occur inside the outer
 * owner frame; its first matched continuous writer must establish the same
 * baseline before an ordinary motion receipt can make a later seed obsolete. */
static void status_stationary_seed_locked(unsigned deck, Slot &slot, const azsdk_load_sample *snapshot = nullptr) noexcept {
    if (!(azn_interest(deck) & motion_routes)) return;
    auto &m = slot.motion;
    const uint64_t epoch = azn_lifecycle_fence(deck);
    if (!epoch || m.foreign.load(std::memory_order_acquire) || m.landing_writers.load(std::memory_order_acquire)) return;
    const uint64_t order = m.landing_order.load(std::memory_order_acquire);
    azsdk_load_sample load {};
    if (snapshot) load = *snapshot;
    bool matched = (snapshot || azsdk_load_read(int(deck), &load)) && load.invalidation_epoch == epoch;
    if (matched && m.load == load.load_generation && m.order) return;
    auto head = status_head(slot.core.load(std::memory_order_acquire));
    if (azn_lifecycle_fence(deck) != epoch || m.landing_order.load(std::memory_order_acquire) != order ||
        m.landing_writers.load(std::memory_order_acquire)) return;
    // A baseline can also come from two real owner frames under this one
    // fence: the prior source was still active, then the requested one was
    // adopted. No path or occurrence is reconstructed between these frames.
    if (m.frame_epoch != epoch) {
        m.frame_epoch = epoch;
        m.frame_landing_order = order;
        std::memcpy(m.first_source, head.source, 12);
    }
    if (!matched || !head.ready) return;
    uint64_t lo;
    uint32_t hi;
    std::memcpy(&lo, head.source, 8);
    std::memcpy(&hi, head.source + 8, 4);
    bool landed = order && m.landing_stamp.load(std::memory_order_acquire) == order && m.landing_epoch.load(std::memory_order_acquire) == epoch &&
                  m.landing_lo.load(std::memory_order_acquire) == lo && m.landing_hi.load(std::memory_order_acquire) == hi;
    if ((!landed && !std::memcmp(m.first_source, head.source, 12)) ||
        !azn_load_track_matches(deck, epoch, load.load_generation, head.source) ||
        m.landing_order.load(std::memory_order_acquire) != order || m.foreign.load(std::memory_order_acquire)) return;
    azstatus_native_receipt r {};
    r.kind = AZN_MOTION;
    r.a = AZN_MOTION_SEED;
    r.deck = deck;
    r.unit = load.unit_generation;
    r.epoch = load.invalidation_epoch;
    r.load_generation = load.load_generation;
    r.motion_order = 1;
    r.motion_generation = 1;
    r.points[0].state = AZN_INVALID;
    r.points[1] = {AZN_COORD, head.rate, static_cast<int64_t>(head.sample)};
    r.observed_window_samples = 1;
    r.observed_window_rate = head.rate;
    uintptr_t pause = 0, cue = 0;
    /* The stationary seed above already proved that this load adopted the
     * source. A cued zero is the opening even when several native setters
     * copied that same initial zero before completion. Nonzero openings use
     * the original adoption or its proven pre-completion copies. The reducer
     * still requires the actual first phrase boundary, never a middle cue. */
    const int opening_order = compare_timestamps(head.sample, 0, head.rate, head.rate, 1);
    const int adopted_order = compare_timestamps(head.sample, m.adoption_sample.load(std::memory_order_acquire), head.rate, head.rate, 1);
    bool at_adoption = m.adoption_rate.load(std::memory_order_acquire) == head.rate &&
                       adopted_order != AZ_TIME_INVALID && adopted_order == AZ_TIME_EQUAL;
    bool initial = opening_order != AZ_TIME_INVALID && (opening_order == AZ_TIME_EQUAL ||
        (landed ? at_adoption && m.opening_landing.load(std::memory_order_acquire) == order : order == m.frame_landing_order));
    bool opening = initial && transport_owners(slot, pause, cue) && !byte(pause + 8);
    m.opening_sample = head.sample;
    m.opening_rate = head.rate;
    m.opening.store(opening, std::memory_order_release);
    if (opening) r.motion_flags = AZN_AT_OPENING;
    if (read_map_clock(deck, load.load_generation, r.map) == UINT64_MAX || map_mutations[deck].active.load(std::memory_order_acquire))
        r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    m.load = load.load_generation;
    m.order = 1;
    m.generation = 1;
    m.known = true;
    map_mutations[deck].last_order.store(1, std::memory_order_release);
    m.previous = head.sample;
    m.rate = head.rate;
    m.window_samples = 1;
    m.window_rate = head.rate;
    std::memcpy(m.identity, head.source, 12);
    m.seed = azn_seed_id(deck);
    m.captured_interference = m.interference.load(std::memory_order_acquire);
    status_offer_motion(m, r);
}
static void status_stationary_seed(unsigned deck, Slot &slot) noexcept {
    if (!(azn_interest(deck) & motion_routes)) return;
    auto &m = slot.motion;
    unsigned reservation = Slot::Motion::FREE;
    if (!m.busy.compare_exchange_strong(reservation, Slot::Motion::OBSERVE, std::memory_order_acquire)) return;
    struct Unlock {
        Slot::Motion &m;
        ~Unlock() { m.busy.store(Slot::Motion::FREE, std::memory_order_release); }
    } unlock {m};
    status_stationary_seed_locked(deck, slot);
}
/* A real PAUSED -> PLAYING commit at the load position can precede the first
 * advancing render block. This consumes the opening exactly once. The next
 * render remains an ordinary travel measurement. */
static void status_opening_play(unsigned d, Slot &slot, const azsdk_load_sample &load) noexcept {
    auto &m = slot.motion;
    if (!m.opening.load(std::memory_order_acquire)) return;
    unsigned reservation = Slot::Motion::FREE;
    if (!m.busy.compare_exchange_strong(reservation, Slot::Motion::OBSERVE, std::memory_order_acquire)) {
        if (m.opening.exchange(false, std::memory_order_acq_rel)) azn_motion_gap(d, 0);
        return;
    }
    struct Unlock {
        Slot::Motion &m;
        ~Unlock() { m.busy.store(Slot::Motion::FREE, std::memory_order_release); }
    } unlock {m};
    if (!m.opening.exchange(false, std::memory_order_acq_rel) || m.load != load.load_generation || !m.known ||
        !m.rate || !m.window_samples || !m.window_rate || !m.generation || m.order == UINT64_MAX) return;
    azstatus_native_receipt r {};
    auto head = status_head(slot.core.load(std::memory_order_acquire));
    azsdk_load_sample after {};
    const int head_order = compare_timestamps(head.sample, m.previous, head.rate, m.window_rate, m.window_samples);
    if (!head.ready || head_order == AZ_TIME_INVALID || head_order != AZ_TIME_EQUAL || head.rate != m.rate || memcmp(head.source, m.identity, 12) ||
        !azsdk_load_read(int(d), &after) || after.unit_generation != load.unit_generation ||
        after.invalidation_epoch != load.invalidation_epoch || after.load_generation != load.load_generation ||
        m.foreign.load(std::memory_order_acquire) || m.captured_interference != m.interference.load(std::memory_order_acquire) ||
        !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, head.source)) return;
    r.kind = AZN_MOTION;
    r.a = AZN_MEASURE;
    r.deck = d;
    r.unit = load.unit_generation;
    r.epoch = load.invalidation_epoch;
    r.load_generation = m.load;
    r.motion_order = ++m.order;
    r.motion_generation = m.generation;
    r.motion_flags = AZN_BEGAN_PLAY;
    r.points[0] = r.points[1] = {AZN_COORD, m.rate, static_cast<int64_t>(head.sample)};
    r.observed_window_samples = r.from_window_samples = m.window_samples;
    r.observed_window_rate = r.from_window_rate = m.window_rate;
    if (read_map_clock(d, m.load, r.map) == UINT64_MAX || map_mutations[d].active.load(std::memory_order_acquire))
        r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    map_mutations[d].last_order.store(m.order, std::memory_order_release);
    azn_emit(&r);
}
class StatusMotionWrite {
    static thread_local StatusMotionWrite *current_setter;
    StatusMotionWrite *previous_setter = nullptr;
    Slot *slot = nullptr;
    uintptr_t core = 0;
    unsigned deck = 0, window_samples = 0;
    bool rendering = false;
    uint64_t fence = 0, map_before = 0, mutation_before = 0, active_before = 0, landing_before = 0;
    uintptr_t fixed_max_owner = 0, stationary_engine = 0;
    uintptr_t limit_owner = 0;
    int64_t observed_limit = 0;
    unsigned limit_calls = 0;
    bool setter_scope = false;
    azsdk_load_sample load {};
    StatusHead from {};
    bool locked = false, competing = false, reposition = false, paused_map = false;
    bool stationary_block() const noexcept {
        const auto &s = state();
        if (!rendering) return !byte(core + 9);
        if (!byte(core + 9) || !stationary_engine || !window_samples || window_samples > 1024 || s.build < 1 || s.build > 2) return false;
        if (RateRenderScope::stationary(core, window_samples)) return true;
        const auto &profile = rate_profiles[s.build - 1];
        if (rate_engine(core, s, profile) != stationary_engine) return false;
        const bool external = word(stationary_engine) == s.bias + profile.scratch && byte(stationary_engine + 0x58);
        const uintptr_t samples = word(stationary_engine + (external ? 0x60u : 0x18u));
        if (!pointer(samples) || window_samples > word(stationary_engine + 0x20)) return false;
        /* These are dimensionless signed source advances from this completed
         * render. A net-zero forward/reverse block is not stationary. */
        for (unsigned n = 0; n < window_samples; ++n)
            if (rate_double(samples + sizeof(double) * n) != 0) return false;
        return true;
    }
    uintptr_t fixed_max_provider() const noexcept {
        /* setCurrentPos can return before its store when its provider rejects
         * an out-of-range target. For equal-coordinate actions prove the
         * built-in maximum branch without calling a getter on the writer. */
        const auto &s = state();
        if (s.build < 1 || s.build > 2) return 0;
        uintptr_t provider = word(core + 0xb8);
        if (!pointer(provider)) return 0;
        uintptr_t table = word(provider);
        const uintptr_t getter = s.bias + (s.build == 1 ? 0x765ff0u : 0x7a78b8u);
        return pointer(table) && word(table + 0x60) == getter ? provider : 0;
    }
   public:
    static void capture_limit(uintptr_t owner, int64_t limit, uintptr_t caller) noexcept {
        auto writer = current_setter;
        const auto &s = state();
        if (!writer || !writer->locked || writer->slot != processing || s.build < 1 || s.build > 2 ||
            caller != s.bias + (s.build == 1 ? 0x14ac994u : 0x1567dc4u) ||
            writer->slot->core.load(std::memory_order_acquire) != writer->core || word(writer->core + 0xb8) != owner) return;
        ++writer->limit_calls;
        writer->limit_owner = owner;
        writer->observed_limit = limit;
    }
    StatusMotionWrite(void *self, bool explicit_position, unsigned samples) noexcept : core(reinterpret_cast<uintptr_t>(self)), window_samples(samples), reposition(explicit_position) {
        if (reposition) {
            /* An unowned nested setter must shadow the outer scope too: all
             * decks can share one buffering scheduler. */
            previous_setter = current_setter;
            current_setter = this;
            setter_scope = true;
        }
        auto &s = state();
        if (!(s.enabled.load(std::memory_order_acquire) & AZEXIT_PASSIVE_STATUS) || !s.motion_hooked) return;
        for (deck = 0; deck < 4; ++deck)
            if (s.decks[deck].core.load(std::memory_order_acquire) == core) {
                slot = &s.decks[deck];
                break;
            }
        if (!slot || !(azn_interest(deck) & (motion_routes | AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)))) return;
        /* A competing or unowned mutation never dereferences a saved pointer
         * and never stamps a preexisting coordinate with the newer fence. */
        unsigned reservation = Slot::Motion::FREE;
        if (slot != processing || !slot->motion.busy.compare_exchange_strong(reservation, Slot::Motion::HEAD, std::memory_order_acquire)) {
            /* A map cut can overlap a paused render even while Core+9 is
             * active. Defer this one verdict until the completed block: the
             * native sample advances, source point and all writer fences
             * must prove that no source travel occurred. */
            if (slot == processing && reservation == Slot::Motion::MAP && !reposition && samples &&
                (rate_hooks.load(std::memory_order_acquire) & 3) == 3 &&
                !slot->motion.active_writers.load(std::memory_order_acquire) &&
                !slot->motion.landing_writers.load(std::memory_order_acquire) &&
                !slot->motion.foreign.load(std::memory_order_acquire) && azsdk_load_read(int(deck), &load)) {
                active_before = slot->motion.active_serial.load(std::memory_order_acquire);
                landing_before = slot->motion.landing_order.load(std::memory_order_acquire);
                fence = slot->motion.interference.load(std::memory_order_acquire);
                from = status_head(core);
                rendering = byte(core + 9);
                if (rendering && s.build >= 1 && s.build <= 2)
                    stationary_engine = rate_engine(core, s, rate_profiles[s.build - 1]);
                paused_map = true;
                return;
            }
            competing = true;
            slot->motion.opening.store(false, std::memory_order_release);
            slot->motion.foreign.fetch_add(1, std::memory_order_acq_rel);
            slot->motion.interference.fetch_add(1, std::memory_order_acq_rel);
            azsdk_load_sample active {};
            if (azsdk_load_read(int(deck), &active)) azn_motion_gap(deck, reposition);
            return;
        }
        locked = true;
        fence = slot->motion.interference.load(std::memory_order_acquire);
        if (!slot->motion.foreign.load(std::memory_order_acquire) && azsdk_load_read(int(deck), &load)) {
            if (!reposition && (!slot->motion.order || slot->motion.load != load.load_generation))
                status_stationary_seed_locked(deck, *slot, &load);
            mutation_before = map_mutations[deck].generation.load(std::memory_order_acquire);
            aznm_packet stamp {};
            map_before = read_map_clock(deck, load.load_generation, stamp);
            from = status_head(core);
            rendering = !reposition && samples && byte(core + 9);
            if (!reposition) {
                active_before = slot->motion.active_serial.load(std::memory_order_acquire);
                landing_before = slot->motion.landing_order.load(std::memory_order_acquire);
                if (rendering && (rate_hooks.load(std::memory_order_acquire) & 3) == 3 && s.build >= 1 && s.build <= 2)
                    stationary_engine = rate_engine(core, s, rate_profiles[s.build - 1]);
            }
            if (reposition) fixed_max_owner = fixed_max_provider();
        }
    }
    ~StatusMotionWrite() {
        if (setter_scope) current_setter = previous_setter;
        if (paused_map) {
            slot->motion.opening.store(false, std::memory_order_release);
            slot->motion.interference.fetch_add(1, std::memory_order_acq_rel);
            azn_motion_gap(deck, 0);
        }
        if (competing) {
            slot->motion.interference.fetch_add(1, std::memory_order_acq_rel);
            slot->motion.foreign.fetch_sub(1, std::memory_order_release);
        }
        if (locked) slot->motion.busy.store(Slot::Motion::FREE, std::memory_order_release);
    }
    void finish(uintptr_t requested = 0) noexcept {
        if (paused_map) {
            const StatusHead to = status_head(core);
            azsdk_load_sample after {};
            const int stationary = compare_timestamps(from.sample, to.sample, from.rate, from.rate, 1);
            const bool inactive = from.ready && to.ready && stationary != AZ_TIME_INVALID && stationary == AZ_TIME_EQUAL && from.rate == to.rate &&
                                  !memcmp(from.source, to.source, 12) &&
                                  slot == processing && slot->core.load(std::memory_order_acquire) == core && stationary_block() &&
                                  !slot->motion.active_writers.load(std::memory_order_acquire) &&
                                  slot->motion.active_serial.load(std::memory_order_acquire) == active_before &&
                                  !slot->motion.landing_writers.load(std::memory_order_acquire) &&
                                  slot->motion.landing_order.load(std::memory_order_acquire) == landing_before &&
                                  !slot->motion.foreign.load(std::memory_order_acquire) &&
                                  slot->motion.interference.load(std::memory_order_acquire) == fence &&
                                  azsdk_load_read(int(deck), &after) && after.unit_generation == load.unit_generation &&
                                  after.invalidation_epoch == load.invalidation_epoch && after.load_generation == load.load_generation &&
                                  azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation, to.source);
            if (!inactive) {
                slot->motion.opening.store(false, std::memory_order_release);
                slot->motion.interference.fetch_add(1, std::memory_order_acq_rel);
                azn_motion_gap(deck, 0);
            }
            paused_map = false;
            return;
        }
        if (!locked || !load.load_generation) return;
        StatusHead to = status_head(core);
        azsdk_load_sample after {};
        if (!azsdk_load_read(int(deck), &after) || load.unit_generation != after.unit_generation ||
            load.load_generation != after.load_generation || load.invalidation_epoch != after.invalidation_epoch ||
            slot->motion.foreign.load(std::memory_order_acquire) ||
            slot->motion.interference.load(std::memory_order_acquire) != fence) return;
        from.ready = from.ready && azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation, from.source);
        to.ready = to.ready && azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation, to.source);
        auto &m = slot->motion;
        const bool same_load = m.load == load.load_generation;
        const bool same_source = from.ready && to.ready && from.rate == to.rate && !memcmp(from.source, to.source, 12);
        const int movement = compare_timestamps(from.sample, to.sample, from.rate, from.rate, window_samples);
        const int prior_order = compare_timestamps(m.previous, from.sample, m.rate, m.window_rate, m.window_samples);
        const bool moved = same_source && movement != AZ_TIME_INVALID && movement != AZ_TIME_EQUAL;
        const bool previous = same_load && m.known && from.ready && m.rate == from.rate &&
                              !memcmp(m.identity, from.source, 12) &&
                              m.window_samples && m.window_rate &&
                              prior_order != AZ_TIME_INVALID && prior_order == AZ_TIME_EQUAL;
        bool target = false;
        if (requested && to.ready) {
            const int target_order = compare_timestamps(word(requested + 0x18), to.sample, to.rate, to.rate, 1);
            target = target_order != AZ_TIME_INVALID && target_order == AZ_TIME_EQUAL &&
                     !memcmp(reinterpret_cast<const void *>(requested + 8), to.source, 12);
        }
        const uint64_t seed = azn_seed_id(deck);
        /* An active engine can render a paused source at zero speed. Keep
         * its opening until cumulative travel leaves the observed window,
         * rather than treating render activity itself as a departure. */
        const int from_opening = compare_timestamps(m.opening_sample, to.sample, to.rate, to.rate, window_samples);
        if (!same_load || !same_source || m.opening_rate != to.rate || from_opening == AZ_TIME_INVALID ||
            from_opening != AZ_TIME_EQUAL || m.captured_interference != fence || (reposition && (!previous || !target)))
            m.opening.store(false, std::memory_order_release);
        if (!same_load) {
            m.load = load.load_generation;
            m.generation = 1;
            m.order = 0;
            m.known = false;
        }
        bool gap = m.order && ((m.known && !previous) || m.captured_interference != fence);
        if (!reposition && !rendering && !moved && !gap && !(same_load && m.known && seed != m.seed)) return;
        /* A stationary copy left behind by a previous load is never a first
         * seed. A verified post-load advance or a matched setter establishes it. */
        if (!m.order && !(rendering || moved || target)) return;
        if (m.order == UINT64_MAX || m.generation == UINT64_MAX) {
            azn_motion_gap(deck, reposition);
            return;
        }
        azstatus_native_receipt r {};
        r.kind = AZN_MOTION;
        r.deck = deck;
        r.epoch = load.invalidation_epoch;
        r.load_generation = load.load_generation;
        r.unit = load.unit_generation;
        r.observed_window_samples = window_samples;
        r.observed_window_rate = window_samples ? to.rate : 0;
        if (previous) {
            r.from_window_samples = m.window_samples;
            r.from_window_rate = m.window_rate;
        }
        r.a = reposition || gap ? AZN_REPOSITION : previous ? AZN_MEASURE
                                                            : AZN_MOTION_SEED;
        if (reposition && target && state().motion_actions) {
            unsigned action = status_action(*slot, from, to);
            const int fixed_range = compare_timestamps(to.sample, UINT64_C(0x5ea0da80), to.rate, to.rate, 1);
            const int actual_range = compare_timestamps(to.sample, observed_limit, to.rate, to.rate, 1);
            /* The actual setter's getter call proves its range guard. Equal
             * coordinates alone cannot distinguish a store from rejection. */
            bool stored = moved || !same_source ||
                          (fixed_max_owner && fixed_max_owner == fixed_max_provider() && fixed_range != AZ_TIME_INVALID && fixed_range <= AZ_TIME_EQUAL) ||
                          (limit_calls == 1 && limit_owner && word(core + 0xb8) == limit_owner && actual_range != AZ_TIME_INVALID && actual_range <= AZ_TIME_EQUAL);
            if (action && stored && m.occurrence < UINT64_MAX) {
                r.a = action;
                r.occurrence_ordinal = ++m.occurrence;
            } else if (action) {
                r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | motion_routes;
            } else if (previous && moved && same_source && status_jump_origin(*slot)) {
                /* The stock controller moved the active source. Preserve its
                 * measured landing while reporting the unclassified action. */
                r.history_only = 1;
                r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
            }
        }
        if ((reposition || gap) && m.order) ++m.generation;
        r.motion_generation = m.generation;
        uint64_t map_after = read_map_clock(deck, load.load_generation, r.map);
        const bool map_ambiguous = (map_before || map_after) &&
            (map_before == UINT64_MAX || map_after == UINT64_MAX || map_before != map_after ||
             mutation_before != map_mutations[deck].generation.load(std::memory_order_acquire) ||
             map_mutations[deck].active.load(std::memory_order_acquire));
        if (gap || (previous && reposition && !target))
            r.lost_topics |= motion_routes;
        if (seed != m.seed && m.order && previous) r.c = 1; /* a fresh recovery baseline */
        /* The first paired map can be published while this writer owns HEAD,
         * so its receipt may have no opening cut. Reannounce the intact cued
         * opening on a proven stationary block once the map becomes usable. */
        const int opening_motion = compare_timestamps(from.sample, to.sample, to.rate, to.rate, 1);
        const bool stationary_opening = !reposition && previous && m.opening.load(std::memory_order_acquire) &&
            opening_motion != AZ_TIME_INVALID && opening_motion == AZ_TIME_EQUAL && stationary_block() &&
            !m.active_writers.load(std::memory_order_acquire) && !m.landing_writers.load(std::memory_order_acquire) &&
            m.active_serial.load(std::memory_order_acquire) == active_before && m.landing_order.load(std::memory_order_acquire) == landing_before;
        /* Qualifying the initial map cannot omit a crossing while this
         * opening's actual source advances are all zero. Unknown or moving
         * blocks retain their mapping-history loss. */
        if (map_ambiguous && !stationary_opening) r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        /* Mapping history has its own loss domain. An intact cued source
         * can remain at its opening while its first map is qualified. */
        if (r.occurrence_ordinal || (r.lost_topics & ~AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)))
            m.opening.store(false, std::memory_order_release);
        else if ((reposition || r.c || stationary_opening) && m.opening.load(std::memory_order_acquire)) r.motion_flags |= AZN_AT_OPENING;
        if (previous) r.points[0] = {AZN_COORD, from.rate, static_cast<int64_t>(from.sample)};
        else r.points[0].state = AZN_INVALID;
        if (to.ready && (!reposition || target))
            r.points[1] = {AZN_COORD, to.rate, static_cast<int64_t>(to.sample)};
        else r.points[1].state = AZN_INVALID;
        const int exact_previous = compare_timestamps(m.previous, from.sample, from.rate, from.rate, 1);
        const auto &end = end_state[deck];
        const int terminal = compare_timestamps(to.sample, end.length, to.rate, to.rate, window_samples);
        const bool before_terminal = end.load != load.load_generation || end.epoch != load.invalidation_epoch ||
            !end.length || (end.length_rate == to.rate && terminal != AZ_TIME_INVALID && terminal == AZ_TIME_BEFORE);
        const bool stationary = !reposition && !gap && previous && same_source && !r.c && !r.lost_topics &&
            !r.occurrence_ordinal && !r.history_only && !map_ambiguous && map_after != UINT64_MAX &&
            exact_previous != AZ_TIME_INVALID && exact_previous == AZ_TIME_EQUAL &&
            opening_motion != AZ_TIME_INVALID && opening_motion == AZ_TIME_EQUAL &&
            window_samples == m.window_samples && to.rate == m.window_rate &&
            !m.active_writers.load(std::memory_order_acquire) && !m.landing_writers.load(std::memory_order_acquire) &&
            m.active_serial.load(std::memory_order_acquire) == active_before && m.landing_order.load(std::memory_order_acquire) == landing_before &&
            before_terminal && !end.relevant.load(std::memory_order_acquire) && !end.refresh.load(std::memory_order_acquire) &&
            RateRenderScope::stationary(core, window_samples);
        /* A successful unchanged measurement already carries this point,
         * window, opening and map. Keep its shared order while parked, so
         * queued copies cannot crowd out real actions during file loading. */
        if (stationary && m.stationary_order == m.order && m.stationary_load == load.load_generation &&
            m.stationary_map == map_after && m.stationary_flags == r.motion_flags) return;
        r.motion_order = ++m.order;
        map_mutations[deck].last_order.store(r.motion_order, std::memory_order_release);
        m.known = r.points[1].state == AZN_COORD;
        m.previous = to.sample;
        m.rate = to.rate;
        m.window_samples = window_samples;
        m.window_rate = window_samples ? to.rate : 0;
        memcpy(m.identity, to.source, 12);
        m.seed = seed;
        m.captured_interference = fence;
        end_mark_motion(deck, r);
        m.stationary_order = 0;
        if (status_offer_motion(m, r) && stationary && !(r.motion_flags & ~AZN_AT_OPENING)) {
            m.stationary_order = m.order;
            m.stationary_load = load.load_generation;
            m.stationary_map = map_after;
            m.stationary_flags = r.motion_flags;
        }
    }
};
thread_local StatusMotionWrite *StatusMotionWrite::current_setter = nullptr;
static int64_t status_limit(uintptr_t self) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const int64_t value = trampoline<int64_t (*)(uintptr_t)>(&status_limit_original)(self);
    StatusMotionWrite::capture_limit(self, value, caller);
    return value;
}
static void status_continuous(void *core, const void *frames) {
    RateRenderScope rate_render(core);
    const int count = frames ? integer(reinterpret_cast<uintptr_t>(frames) + 8) : 0;
    /* Even before load completion (when no public trajectory can be
     * emitted), rendering breaks a chain of copied adoption coordinates.
     * Entry/exit fences also reject a concurrent or net-zero excursion. */
    struct AdoptionRender {
        Slot::Motion *motion = nullptr;
        AdoptionRender(void *core, int count) {
            if (count <= 0) return;
            for (auto &slot : state().decks) {
                if (slot.core.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(core)) continue;
                motion = &slot.motion;
                motion->adoption_render_writers.fetch_add(1, std::memory_order_acq_rel);
                motion->adoption_render.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
        }
        ~AdoptionRender() {
            if (!motion) return;
            motion->adoption_render.fetch_add(1, std::memory_order_release);
            motion->adoption_render_writers.fetch_sub(1, std::memory_order_release);
        }
    } adoption_render(core, count);
    StatusMotionWrite receipt(core, false, count > 0 ? unsigned(count) : 0);
    trampoline<void (*)(void *, const void *)> (&status_continuous_original)(core, frames);
    receipt.finish();
    if (processing && frames && processing->core.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(core))
        rate_capture(unsigned(processing - state().decks), *processing, 0,
                     rate_render.measured_frames(integer(reinterpret_cast<uintptr_t>(frames) + 8)));
}
static bool status_install_motion(State &s, int build, azexit_install_fn install) {
    static const struct {
        uintptr_t address;
        uint32_t first[4];
    } entries[2] = {
        {0x1358ea8, {0xa9b37bfd, 0xb000a302, 0x91240042, 0x910003fd}},
        {0x1414218, {0xa9b37bfd, 0xd000b782, 0x9116c042, 0x910003fd}}};
    const auto &e = entries[build - 1];
    const auto &jump = s.profile.core_jump;
    const uintptr_t limit = s.bias + (build == 1 ? 0x766220u : 0x7a7aa0u);
    static const uint32_t limit_first[] = {0xf9418001, 0xb9400020, 0xb9402021, 0x9b207c20};
    if (!jump.address || std::memcmp(reinterpret_cast<const void *>(jump.address), jump.prologue, sizeof jump.prologue) ||
        std::memcmp(reinterpret_cast<const void *>(limit), limit_first, sizeof limit_first) ||
        std::memcmp(reinterpret_cast<const void *>(s.bias + e.address), e.first, sizeof e.first)) return false;
    return install(limit, reinterpret_cast<void *>(status_limit), &status_limit_original) && status_limit_original &&
           install(s.bias + e.address, reinterpret_cast<void *>(status_continuous), &status_continuous_original) && status_continuous_original &&
           install(jump.address, reinterpret_cast<void *>(core_jump_hook), &s.core_jump_original) && s.core_jump_original;
}

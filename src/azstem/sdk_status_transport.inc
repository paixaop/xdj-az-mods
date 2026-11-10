/* SPDX-License-Identifier: GPL-3.0-only */
/* The play latch and the cue-play context are independently committed. The
 * normal PLAY command sets the latch before clearing an existing cue. Its
 * specific caller, and the distinct cue-audition caller, disambiguate that
 * intermediate cut. A button request alone never produces a receipt. */
struct TransportProfile {
    uintptr_t writers[4], normal_play, audition;
};
static const TransportProfile transport_profiles[2] = {
    {{0x1421298, 0x1424f58, 0x13e3bd8, 0x13e3da8}, 0x13c32fc, 0x1369cd8},
    {{0x14dca40, 0x14e06e8, 0x149eed0, 0x149f0a0}, 0x147e514, 0x1425028}};
static void *transport_original[4];
static std::atomic<unsigned> transport_installed {0};
enum TransportCause {
    TransportSeed,
    TransportPlay,
    TransportAudition,
    TransportOtherPlay,
    TransportPause,
    TransportCue
};
struct TransportState {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<uintptr_t> pause {0}, cue {0};
    std::atomic<uint64_t> owner_epoch {0};
    std::atomic<uint64_t> fault {0};
    uint64_t load = 0, seed = 0, last_fault = 0;
    unsigned raw = 0, availability = AZ_AVAIL_UNSUPPORTED;
    int mode = 0;
};
static TransportState transport_state[4];
static void transport_unobserved(unsigned d, uint64_t epoch) noexcept {
    if (azn_load_history_revoked(d, epoch)) return;
    transport_state[d].fault.fetch_add(1, std::memory_order_acq_rel);
    azn_transport_gap(d);
}
static bool transport_owners(Slot &s, uintptr_t &pause, uintptr_t &cue) noexcept {
    if (!coherent(s)) return false;
    pause = word(s.facade + 0x58);
    cue = word(s.facade + 0x68);
    uintptr_t bundle = word(s.facade + 0x78), core = s.core.load(std::memory_order_acquire);
    return table(pause, state().profile.play_pause_table) && pointer(cue) && pointer(bundle) &&
           word(pause + 0x58) == core && word(bundle) == core && word(bundle + 8) == cue && word(bundle + 0xa8) == pause;
}
static void status_opening_play(unsigned, Slot &, const azsdk_load_sample &) noexcept;
static void transport_observe(unsigned d, Slot &slot, TransportCause why, const azsdk_load_sample *before = nullptr) noexcept {
    if (transport_installed.load(std::memory_order_acquire) != 15 || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) return;
    auto &r = transport_state[d];
    if (r.busy.test_and_set(std::memory_order_acquire)) {
        transport_unobserved(d, before ? before->invalidation_epoch : r.owner_epoch.load(std::memory_order_acquire));
        return;
    }
    struct Unlock {
        TransportState &r;
        ~Unlock() { r.busy.clear(std::memory_order_release); }
    } guard {r};
    azsdk_load_sample load {};
    if (!azsdk_load_read(int(d), &load) || (before && (load.load_generation != before->load_generation ||
                                                       load.invalidation_epoch != before->invalidation_epoch || load.unit_generation != before->unit_generation))) return;
    uintptr_t pause = 0, cue = 0;
    unsigned raw = ~0u, avail = r.load == load.load_generation ? AZ_AVAIL_STALE : AZ_AVAIL_LOADING;
    int mode = 0;
    const auto core = slot.core.load(std::memory_order_acquire);
    const uint64_t fault = r.fault.load(std::memory_order_acquire), seed = azn_seed_id(d);
    const bool same = r.load == load.load_generation;
    if (transport_owners(slot, pause, cue) &&
        azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, reinterpret_cast<const unsigned char *>(core + 0x18))) {
        r.pause.store(pause, std::memory_order_release);
        r.cue.store(cue, std::memory_order_release);
        r.owner_epoch.store(load.invalidation_epoch, std::memory_order_release);
        unsigned play = byte(pause + 8), extra = byte(pause + 9), cue_play = byte(cue + 8), native_mode = unsigned(integer(pause + 0xc));
        if (play <= 1 && extra <= 1 && cue_play <= 1 && native_mode <= 2) {
            raw = play | (cue_play << 1) | (extra << 2) | (native_mode << 3);
            avail = AZ_AVAIL_UNSUPPORTED;
            if (!extra && !native_mode) {
                if (!play) mode = AZ_PLAYBACK_PAUSED;
                else if (!cue_play || why == TransportPlay) mode = AZ_PLAYBACK_PLAYING;
                else if (why == TransportAudition) mode = AZ_PLAYBACK_CUEING;
                else if (same && fault == r.last_fault && raw == r.raw && why != TransportOtherPlay) mode = r.mode;
                if (mode) avail = AZ_AVAIL_READY;
            }
        }
    }
    azsdk_load_sample end {};
    if (!azsdk_load_read(int(d), &end) || load.invalidation_epoch != end.invalidation_epoch ||
        load.load_generation != end.load_generation || load.unit_generation != end.unit_generation ||
        fault != r.fault.load(std::memory_order_acquire)) return;
    bool loss = same && r.availability == AZ_AVAIL_READY && (avail != AZ_AVAIL_READY || (why == TransportSeed && raw != r.raw && mode != r.mode));
    if (same && r.seed == seed && r.last_fault == fault && r.mode == mode && r.availability == avail && !loss) {
        r.raw = raw;
        return;
    }
    azstatus_native_receipt out {};
    out.kind = AZN_TRANSPORT_STATE;
    out.deck = d;
    out.unit = load.unit_generation;
    out.epoch = load.invalidation_epoch;
    out.load_generation = load.load_generation;
    out.valid = int(avail);
    out.a = mode;
    if (loss) out.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    if (!azn_offer(&out)) return;
    if (why == TransportPlay && same && !loss && r.availability == AZ_AVAIL_READY && r.mode == AZ_PLAYBACK_PAUSED &&
        avail == AZ_AVAIL_READY && mode == AZ_PLAYBACK_PLAYING) status_opening_play(d, slot, load);
    r.load = load.load_generation;
    r.seed = seed;
    r.last_fault = fault;
    r.raw = raw;
    r.mode = mode;
    r.availability = avail;
}
template <unsigned I, class... Args>
static void transport_writer(void *self, Args... args) {
    uintptr_t owner = reinterpret_cast<uintptr_t>(self), caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    auto &s = state();
    Slot *slot = processing;
    unsigned d = slot ? unsigned(slot - s.decks) : 4;
    uintptr_t pause = 0, cue = 0;
    azsdk_load_sample load {};
    bool bound = slot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && transport_owners(*slot, pause, cue) &&
                 owner == (I < 2 ? pause : cue) && azsdk_load_read(int(d), &load);
    trampoline<void (*)(void *, Args...)> (&transport_original[I])(self, args...);
    if (bound) {
        uintptr_t current_pause = 0, current_cue = 0;
        if (!transport_owners(*slot, current_pause, current_cue) || pause != current_pause || cue != current_cue) {
            transport_unobserved(d, load.invalidation_epoch);
            return;
        }
        const auto &p = transport_profiles[s.build - 1];
        TransportCause why = I == 0 ? TransportOtherPlay : I == 1 ? TransportPause
                                                                  : TransportCue;
        if (I == 0 && caller == s.bias + p.normal_play) why = TransportPlay;
        if (I == 0 && caller == s.bias + p.audition) why = TransportAudition;
        transport_observe(d, *slot, why, &load);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            auto &r = transport_state[i];
            if ((I < 2 ? r.pause : r.cue).load(std::memory_order_acquire) == owner &&
                (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
                transport_unobserved(i, r.owner_epoch.load(std::memory_order_acquire));
            }
        }
    }
}
static void transport_install(State &s, int build, azn_setting_patch_fn patch) {
    const auto &p = transport_profiles[build - 1];
    static constexpr uint32_t first[] = {0xa9be7bfd, 0xa9b57bfd, 0x12001c21, 0xa9be7bfd};
    void *hooks[] = {reinterpret_cast<void *>(transport_writer<0, unsigned>), reinterpret_cast<void *>(transport_writer<1, unsigned, unsigned>),
                     reinterpret_cast<void *>(transport_writer<2, unsigned>), reinterpret_cast<void *>(transport_writer<3, unsigned>)};
    for (unsigned i = 0; i < 4; ++i)
        if (!(transport_installed.load(std::memory_order_acquire) & (1u << i))) {
            uintptr_t site = s.bias + p.writers[i];
            if (std::memcmp(reinterpret_cast<void *>(site), &first[i], 4) ||
                patch(site, hooks[i], &transport_original[i], "status-transport") != 1 || !transport_original[i]) return;
            transport_installed.fetch_or(1u << i, std::memory_order_release);
        }
    for (unsigned d = 0; d < 4; ++d)
        if (s.decks[d].core.load(std::memory_order_acquire)) azn_capability(d, AZN_CAP_TRANSPORT_STATE, 1);
}

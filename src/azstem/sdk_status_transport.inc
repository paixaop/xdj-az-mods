/* SPDX-License-Identifier: GPL-3.0-only */
/* The play latch and the cue-play context are independently committed. The
 * normal PLAY command sets the latch before clearing an existing cue. Its
 * specific caller, and the distinct cue-audition caller, disambiguate that
 * intermediate cut. uintptr_t button request alone never produces a receipt. */
struct TransportProfile {
    uintptr_t writers[4], normal_play, audition;
};
static const TransportProfile transport_profiles[2] = {
    {{0x1421298, 0x1424f58, 0x13e3bd8, 0x13e3da8}, 0x13c32fc, 0x1369cd8},
    {{0x14dca40, 0x14e06e8, 0x149eed0, 0x149f0a0}, 0x147e514, 0x1425028}};
static void *transport_original[4];
static _Atomic(unsigned) transport_installed = 0;
enum TransportCause {
    TransportSeed,
    TransportPlay,
    TransportAudition,
    TransportOtherPlay,
    TransportPause,
    TransportCue
};
struct TransportState {
    atomic_flag busy;
    _Atomic(uintptr_t) pause;
    _Atomic(uintptr_t) cue;
    _Atomic(uint64_t) owner_epoch;
    _Atomic(uint64_t) fault;
    uint64_t load;
    uint64_t seed;
    uint64_t last_fault;
    unsigned raw;
    unsigned availability;
    int mode;
};
static TransportState transport_state[4] = {[0 ... 3] = {.busy = ATOMIC_FLAG_INIT, .pause = 0, .cue = 0, .owner_epoch = 0, .fault = 0, .load = 0, .seed = 0, .last_fault = 0, .raw = 0, .availability = AZ_AVAIL_UNSUPPORTED, .mode = 0}};
static void transport_unobserved(unsigned d, uint64_t epoch) {
    if (azn_load_history_revoked(d, epoch)) return;
    atomic_fetch_add_explicit(&(transport_state[d].fault), 1, memory_order_acq_rel);
    azn_transport_gap(d);
}
static bool transport_owners(Slot *s, uintptr_t *pause, uintptr_t *cue) {
    if (!coherent(s)) return false;
    (*pause) = word(s->facade + 0x58);
    (*cue) = word(s->facade + 0x68);
    uintptr_t bundle = word(s->facade + 0x78), core = atomic_load_explicit(&(s->core), memory_order_acquire);
    return table((*pause), state().profile.play_pause_table) && pointer((*cue)) && pointer(bundle) &&
           word((*pause) + 0x58) == core && word(bundle) == core && word(bundle + 8) == (*cue) && word(bundle + 0xa8) == (*pause);
}
static void status_opening_play(unsigned, Slot *, const azsdk_load_sample);
struct TransportObservation {
    azsdk_load_sample load;
    uint64_t fault, seed;
    unsigned raw, availability;
    int mode;
    bool same;
};
static bool transport_load_matches(unsigned d, const azsdk_load_sample *before, azsdk_load_sample *load) {
    if (!azsdk_load_read((int) d, load)) return false;
    return !before || (load->load_generation == before->load_generation &&
                       load->invalidation_epoch == before->invalidation_epoch && load->unit_generation == before->unit_generation);
}
static int transport_play_mode(const struct TransportState *r, enum TransportCause why,
                               const struct TransportObservation *value, unsigned play, unsigned cue_play) {
    if (!play) return AZ_PLAYBACK_PAUSED;
    if (!cue_play || why == TransportPlay) return AZ_PLAYBACK_PLAYING;
    if (why == TransportAudition) return AZ_PLAYBACK_CUEING;
    if (value->same && value->fault == r->last_fault && value->raw == r->raw && why != TransportOtherPlay) return r->mode;
    return 0;
}
static void transport_read_latches(const struct TransportState *r, uintptr_t pause, uintptr_t cue,
                                   enum TransportCause why, struct TransportObservation *value) {
    unsigned play = byte(pause + 8), extra = byte(pause + 9), cue_play = byte(cue + 8), native_mode = (unsigned) integer(pause + 0xc);
    if (play > 1 || extra > 1 || cue_play > 1 || native_mode > 2) return;
    value->raw = play | (cue_play << 1) | (extra << 2) | (native_mode << 3);
    value->availability = AZ_AVAIL_UNSUPPORTED;
    if (extra || native_mode) return;
    value->mode = transport_play_mode(r, why, value, play, cue_play);
    if (value->mode) value->availability = AZ_AVAIL_READY;
}
static void transport_read_owned(unsigned d, Slot *slot, enum TransportCause why, struct TransportObservation *value) {
    struct TransportState *r = &transport_state[d];
    uintptr_t pause = 0, cue = 0;
    value->raw = ~0u;
    value->availability = r->load == value->load.load_generation ? AZ_AVAIL_STALE : AZ_AVAIL_LOADING;
    value->mode = 0;
    uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    value->fault = atomic_load_explicit(&(r->fault), memory_order_acquire);
    value->seed = azn_seed_id(d);
    value->same = r->load == value->load.load_generation;
    if (!transport_owners(slot, &pause, &cue) ||
        !azn_load_track_matches(d, value->load.invalidation_epoch, value->load.load_generation, (const unsigned char *) (core + 0x18))) return;
    atomic_store_explicit(&(r->pause), pause, memory_order_release);
    atomic_store_explicit(&(r->cue), cue, memory_order_release);
    atomic_store_explicit(&(r->owner_epoch), value->load.invalidation_epoch, memory_order_release);
    transport_read_latches(r, pause, cue, why, value);
}
static bool transport_lost(const struct TransportState *r, enum TransportCause why, const struct TransportObservation *value) {
    return value->same && r->availability == AZ_AVAIL_READY &&
           (value->availability != AZ_AVAIL_READY || (why == TransportSeed && value->raw != r->raw && value->mode != r->mode));
}
static bool transport_unchanged(const struct TransportState *r, const struct TransportObservation *value, bool loss) {
    return value->same && r->seed == value->seed && r->last_fault == value->fault &&
           r->mode == value->mode && r->availability == value->availability && !loss;
}
static bool transport_began_play(const struct TransportState *r, enum TransportCause why, const struct TransportObservation *value, bool loss) {
    return why == TransportPlay && value->same && !loss && r->availability == AZ_AVAIL_READY && r->mode == AZ_PLAYBACK_PAUSED &&
           value->availability == AZ_AVAIL_READY && value->mode == AZ_PLAYBACK_PLAYING;
}
static void transport_publish(unsigned d, Slot *slot, enum TransportCause why, const struct TransportObservation *value, bool loss) {
    struct TransportState *r = &transport_state[d];
    azstatus_native_receipt out = {0};
    out.kind = AZN_TRANSPORT_STATE;
    out.deck = d;
    out.unit = value->load.unit_generation;
    out.epoch = value->load.invalidation_epoch;
    out.load_generation = value->load.load_generation;
    out.valid = (int) value->availability;
    out.a = value->mode;
    if (loss) out.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    if (!azn_offer(&out)) return;
    if (transport_began_play(r, why, value, loss)) status_opening_play(d, slot, value->load);
    r->load = value->load.load_generation;
    r->seed = value->seed;
    r->last_fault = value->fault;
    r->raw = value->raw;
    r->mode = value->mode;
    r->availability = value->availability;
}
static bool transport_observation_current(unsigned d, const struct TransportObservation *value) {
    azsdk_load_sample end = {0};
    return transport_load_matches(d, &value->load, &end) &&
           value->fault == atomic_load_explicit(&(transport_state[d].fault), memory_order_acquire);
}
static void transport_observe(unsigned d, Slot *slot, enum TransportCause why, const azsdk_load_sample *before) {
    if (atomic_load_explicit(&(transport_installed), memory_order_acquire) != 15 || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) return;
    struct TransportState *r = &transport_state[d];
    if (atomic_flag_test_and_set_explicit(&(r->busy), memory_order_acquire)) {
        transport_unobserved(d, before ? before->invalidation_epoch : atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
        return;
    }
    EXIT_FLAG_GUARD(guard, &r->busy);
    struct TransportObservation value = {0};
    if (!transport_load_matches(d, before, &value.load)) return;
    transport_read_owned(d, slot, why, &value);
    if (!transport_observation_current(d, &value)) return;
    bool loss = transport_lost(r, why, &value);
    if (transport_unchanged(r, &value, loss)) {
        r->raw = value.raw;
        return;
    }
    transport_publish(d, slot, why, &value, loss);
}
struct TransportWrite {
    Slot *slot;
    unsigned deck;
    uintptr_t pause, cue;
    azsdk_load_sample load;
    bool bound;
};
static struct TransportWrite transport_begin_write(uintptr_t owner, unsigned kind) {
    struct TransportWrite write = {0};
    write.slot = processing;
    write.deck = write.slot ? (unsigned) (write.slot - state().decks) : 4;
    write.bound = write.slot && (azn_interest(write.deck) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) &&
                  transport_owners(write.slot, &write.pause, &write.cue) && owner == (kind < 2 ? write.pause : write.cue) &&
                  azsdk_load_read((int) write.deck, &write.load);
    return write;
}
static enum TransportCause transport_write_cause(unsigned kind, uintptr_t caller) {
    if (kind == 1) return TransportPause;
    if (kind != 0) return TransportCue;
    const struct TransportProfile *profile = &transport_profiles[state().build - 1];
    enum TransportCause why = TransportOtherPlay;
    if (caller == state().bias + profile->normal_play) why = TransportPlay;
    if (caller == state().bias + profile->audition) why = TransportAudition;
    return why;
}
static void transport_unbound_write(unsigned kind, uintptr_t owner) {
    for (unsigned i = 0; i < 4; ++i) {
        struct TransportState *r = &transport_state[i];
        if (atomic_load_explicit(kind < 2 ? &r->pause : &r->cue, memory_order_acquire) == owner &&
            (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)))
            transport_unobserved(i, atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
    }
}
static void transport_finish_write(const struct TransportWrite *write, unsigned kind, uintptr_t owner, uintptr_t caller) {
    if (!write->bound) {
        transport_unbound_write(kind, owner);
        return;
    }
    uintptr_t pause = 0, cue = 0;
    if (!transport_owners(write->slot, &pause, &cue) || write->pause != pause || write->cue != cue) {
        transport_unobserved(write->deck, write->load.invalidation_epoch);
        return;
    }
    transport_observe(write->deck, write->slot, transport_write_cause(kind, caller), &write->load);
}
static void transport_writer_0(void *self, unsigned arg0) {
    uintptr_t owner = (uintptr_t) self, caller = (uintptr_t) __builtin_return_address(0);
    struct TransportWrite write = transport_begin_write(owner, 0);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[0]))(self, arg0);
    transport_finish_write(&write, 0, owner, caller);
}

static void transport_writer_1(void *self, unsigned arg0, unsigned arg1) {
    uintptr_t owner = (uintptr_t) self, caller = (uintptr_t) __builtin_return_address(0);
    struct TransportWrite write = transport_begin_write(owner, 1);
    ((void (*)(void *, unsigned, unsigned)) trampoline_address(&transport_original[1]))(self, arg0, arg1);
    transport_finish_write(&write, 1, owner, caller);
}

static void transport_writer_2(void *self, unsigned arg0) {
    uintptr_t owner = (uintptr_t) self, caller = (uintptr_t) __builtin_return_address(0);
    struct TransportWrite write = transport_begin_write(owner, 2);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[2]))(self, arg0);
    transport_finish_write(&write, 2, owner, caller);
}

static void transport_writer_3(void *self, unsigned arg0) {
    uintptr_t owner = (uintptr_t) self, caller = (uintptr_t) __builtin_return_address(0);
    struct TransportWrite write = transport_begin_write(owner, 3);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[3]))(self, arg0);
    transport_finish_write(&write, 3, owner, caller);
}

static void transport_install(State *s, int build, azn_setting_patch_fn patch) {
    __auto_type p = &(transport_profiles[build - 1]);
    static const uint32_t first[] = {0xa9be7bfd, 0xa9b57bfd, 0x12001c21, 0xa9be7bfd};
    void *hooks[] = {((void *) (transport_writer_0)), ((void *) (transport_writer_1)),
                     ((void *) (transport_writer_2)), ((void *) (transport_writer_3))};
    for (unsigned i = 0; i < 4; ++i)
        if (!(atomic_load_explicit(&(transport_installed), memory_order_acquire) & (1u << i))) {
            uintptr_t site = s->bias + p->writers[i];
            if (memcmp(((void *) (site)), &first[i], 4) ||
                patch(site, hooks[i], &transport_original[i], "status-transport") != 1 || !transport_original[i]) return;
            atomic_fetch_or_explicit(&(transport_installed), 1u << i, memory_order_release);
        }
    for (unsigned d = 0; d < 4; ++d)
        if (atomic_load_explicit(&(s->decks[d].core), memory_order_acquire)) azn_capability(d, AZN_CAP_TRANSPORT_STATE, 1);
}

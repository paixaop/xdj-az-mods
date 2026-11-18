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
static void transport_observe(unsigned d, Slot *slot, enum TransportCause why, const azsdk_load_sample *before) {
    if (atomic_load_explicit(&(transport_installed), memory_order_acquire) != 15 || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) return;
    __auto_type r = &(transport_state[d]);
    if (atomic_flag_test_and_set_explicit(&(r->busy), memory_order_acquire)) {
        transport_unobserved(d, before ? before->invalidation_epoch : atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
        return;
    }
    EXIT_FLAG_GUARD(guard, &r->busy);
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (d), &load) || (before && (load.load_generation != before->load_generation ||
                                                          load.invalidation_epoch != before->invalidation_epoch || load.unit_generation != before->unit_generation))) return;
    uintptr_t pause = 0, cue = 0;
    unsigned raw = ~0u, avail = r->load == load.load_generation ? AZ_AVAIL_STALE : AZ_AVAIL_LOADING;
    int mode = 0;
    __auto_type core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    const uint64_t fault = atomic_load_explicit(&(r->fault), memory_order_acquire), seed = azn_seed_id(d);
    const bool same = r->load == load.load_generation;
    if (transport_owners(slot, &(pause), &(cue)) &&
        azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, ((const unsigned char *) (core + 0x18)))) {
        atomic_store_explicit(&(r->pause), pause, memory_order_release);
        atomic_store_explicit(&(r->cue), cue, memory_order_release);
        atomic_store_explicit(&(r->owner_epoch), load.invalidation_epoch, memory_order_release);
        unsigned play = byte(pause + 8), extra = byte(pause + 9), cue_play = byte(cue + 8), native_mode = (unsigned) (integer(pause + 0xc));
        if (play <= 1 && extra <= 1 && cue_play <= 1 && native_mode <= 2) {
            raw = play | (cue_play << 1) | (extra << 2) | (native_mode << 3);
            avail = AZ_AVAIL_UNSUPPORTED;
            if (!extra && !native_mode) {
                if (!play) mode = AZ_PLAYBACK_PAUSED;
                else if (!cue_play || why == TransportPlay) mode = AZ_PLAYBACK_PLAYING;
                else if (why == TransportAudition) mode = AZ_PLAYBACK_CUEING;
                else if (same && fault == r->last_fault && raw == r->raw && why != TransportOtherPlay) mode = r->mode;
                if (mode) avail = AZ_AVAIL_READY;
            }
        }
    }
    azsdk_load_sample end = {0};
    if (!azsdk_load_read((int) (d), &end) || load.invalidation_epoch != end.invalidation_epoch ||
        load.load_generation != end.load_generation || load.unit_generation != end.unit_generation ||
        fault != atomic_load_explicit(&(r->fault), memory_order_acquire)) return;
    bool loss = same && r->availability == AZ_AVAIL_READY && (avail != AZ_AVAIL_READY || (why == TransportSeed && raw != r->raw && mode != r->mode));
    if (same && r->seed == seed && r->last_fault == fault && r->mode == mode && r->availability == avail && !loss) {
        r->raw = raw;
        return;
    }
    azstatus_native_receipt out = {0};
    out.kind = AZN_TRANSPORT_STATE;
    out.deck = d;
    out.unit = load.unit_generation;
    out.epoch = load.invalidation_epoch;
    out.load_generation = load.load_generation;
    out.valid = (int) (avail);
    out.a = mode;
    if (loss) out.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    if (!azn_offer(&out)) return;
    if (why == TransportPlay && same && !loss && r->availability == AZ_AVAIL_READY && r->mode == AZ_PLAYBACK_PAUSED &&
        avail == AZ_AVAIL_READY && mode == AZ_PLAYBACK_PLAYING) status_opening_play(d, &((*slot)), load);
    r->load = load.load_generation;
    r->seed = seed;
    r->last_fault = fault;
    r->raw = raw;
    r->mode = mode;
    r->availability = avail;
}
static void transport_writer_0(void *self, unsigned arg0) {
    uintptr_t owner = ((uintptr_t) (self)), caller = ((uintptr_t) (__builtin_return_address(0)));
    __auto_type s = &(state());
    Slot *slot = processing;
    unsigned d = slot ? (unsigned) (slot - s->decks) : 4;
    uintptr_t pause = 0, cue = 0;
    azsdk_load_sample load = {0};
    bool bound = slot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && transport_owners(slot, &(pause), &(cue)) &&
                 owner == (0 < 2 ? pause : cue) && azsdk_load_read((int) (d), &load);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[0]))(self, arg0);
    if (bound) {
        uintptr_t current_pause = 0, current_cue = 0;
        if (!transport_owners(slot, &(current_pause), &(current_cue)) || pause != current_pause || cue != current_cue) {
            transport_unobserved(d, load.invalidation_epoch);
            return;
        }
        __auto_type p = &(transport_profiles[s->build - 1]);
        enum TransportCause why = 0 == 0 ? TransportOtherPlay : 0 == 1 ? TransportPause
                                                                       : TransportCue;
        if (0 == 0 && caller == s->bias + p->normal_play) why = TransportPlay;
        if (0 == 0 && caller == s->bias + p->audition) why = TransportAudition;
        transport_observe(d, &(*slot), why, &load);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            __auto_type r = &(transport_state[i]);
            if (atomic_load_explicit(0 < 2 ? &r->pause : &r->cue, memory_order_acquire) == owner &&
                (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
                transport_unobserved(i, atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
            }
        }
    }
}
static void transport_writer_1(void *self, unsigned arg0, unsigned arg1) {
    uintptr_t owner = ((uintptr_t) (self)), caller = ((uintptr_t) (__builtin_return_address(0)));
    __auto_type s = &(state());
    Slot *slot = processing;
    unsigned d = slot ? (unsigned) (slot - s->decks) : 4;
    uintptr_t pause = 0, cue = 0;
    azsdk_load_sample load = {0};
    bool bound = slot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && transport_owners(slot, &(pause), &(cue)) &&
                 owner == (1 < 2 ? pause : cue) && azsdk_load_read((int) (d), &load);
    ((void (*)(void *, unsigned, unsigned)) trampoline_address(&transport_original[1]))(self, arg0, arg1);
    if (bound) {
        uintptr_t current_pause = 0, current_cue = 0;
        if (!transport_owners(slot, &(current_pause), &(current_cue)) || pause != current_pause || cue != current_cue) {
            transport_unobserved(d, load.invalidation_epoch);
            return;
        }
        __auto_type p = &(transport_profiles[s->build - 1]);
        enum TransportCause why = 1 == 0 ? TransportOtherPlay : 1 == 1 ? TransportPause
                                                                       : TransportCue;
        if (1 == 0 && caller == s->bias + p->normal_play) why = TransportPlay;
        if (1 == 0 && caller == s->bias + p->audition) why = TransportAudition;
        transport_observe(d, &(*slot), why, &load);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            __auto_type r = &(transport_state[i]);
            if (atomic_load_explicit(1 < 2 ? &r->pause : &r->cue, memory_order_acquire) == owner &&
                (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
                transport_unobserved(i, atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
            }
        }
    }
}
static void transport_writer_2(void *self, unsigned arg0) {
    uintptr_t owner = ((uintptr_t) (self)), caller = ((uintptr_t) (__builtin_return_address(0)));
    __auto_type s = &(state());
    Slot *slot = processing;
    unsigned d = slot ? (unsigned) (slot - s->decks) : 4;
    uintptr_t pause = 0, cue = 0;
    azsdk_load_sample load = {0};
    bool bound = slot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && transport_owners(slot, &(pause), &(cue)) &&
                 owner == (2 < 2 ? pause : cue) && azsdk_load_read((int) (d), &load);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[2]))(self, arg0);
    if (bound) {
        uintptr_t current_pause = 0, current_cue = 0;
        if (!transport_owners(slot, &(current_pause), &(current_cue)) || pause != current_pause || cue != current_cue) {
            transport_unobserved(d, load.invalidation_epoch);
            return;
        }
        __auto_type p = &(transport_profiles[s->build - 1]);
        enum TransportCause why = 2 == 0 ? TransportOtherPlay : 2 == 1 ? TransportPause
                                                                       : TransportCue;
        if (2 == 0 && caller == s->bias + p->normal_play) why = TransportPlay;
        if (2 == 0 && caller == s->bias + p->audition) why = TransportAudition;
        transport_observe(d, &(*slot), why, &load);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            __auto_type r = &(transport_state[i]);
            if (atomic_load_explicit(2 < 2 ? &r->pause : &r->cue, memory_order_acquire) == owner &&
                (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
                transport_unobserved(i, atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
            }
        }
    }
}
static void transport_writer_3(void *self, unsigned arg0) {
    uintptr_t owner = ((uintptr_t) (self)), caller = ((uintptr_t) (__builtin_return_address(0)));
    __auto_type s = &(state());
    Slot *slot = processing;
    unsigned d = slot ? (unsigned) (slot - s->decks) : 4;
    uintptr_t pause = 0, cue = 0;
    azsdk_load_sample load = {0};
    bool bound = slot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)) && transport_owners(slot, &(pause), &(cue)) &&
                 owner == (3 < 2 ? pause : cue) && azsdk_load_read((int) (d), &load);
    ((void (*)(void *, unsigned)) trampoline_address(&transport_original[3]))(self, arg0);
    if (bound) {
        uintptr_t current_pause = 0, current_cue = 0;
        if (!transport_owners(slot, &(current_pause), &(current_cue)) || pause != current_pause || cue != current_cue) {
            transport_unobserved(d, load.invalidation_epoch);
            return;
        }
        __auto_type p = &(transport_profiles[s->build - 1]);
        enum TransportCause why = 3 == 0 ? TransportOtherPlay : 3 == 1 ? TransportPause
                                                                       : TransportCue;
        if (3 == 0 && caller == s->bias + p->normal_play) why = TransportPlay;
        if (3 == 0 && caller == s->bias + p->audition) why = TransportAudition;
        transport_observe(d, &(*slot), why, &load);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            __auto_type r = &(transport_state[i]);
            if (atomic_load_explicit(3 < 2 ? &r->pause : &r->cue, memory_order_acquire) == owner &&
                (azn_interest(i) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
                transport_unobserved(i, atomic_load_explicit(&(r->owner_epoch), memory_order_acquire));
            }
        }
    }
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

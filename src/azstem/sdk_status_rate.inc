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
    atomic_flag busy;
    _Atomic(uintptr_t) bound_engine;
    _Atomic(uint64_t) fault;
    uint64_t load;
    uint64_t seed;
    uint64_t observed_fault;
    uint64_t bits;
    unsigned availability;
    unsigned direction_avail;
    int direction;
    bool rate_ramp;
    bool direction_ramp;
};
static RateState rate_state[4] = {[0 ... 3] = {.busy = ATOMIC_FLAG_INIT, .bound_engine = 0, .fault = 0, .load = 0, .seed = 0, .observed_fault = 0, .bits = 0, .availability = AZ_AVAIL_UNSUPPORTED, .direction_avail = AZ_AVAIL_UNSUPPORTED, .direction = 0, .rate_ramp = false, .direction_ramp = false}};
static _Atomic(unsigned) rate_hooks = 0;
static void *rate_original[4];
static _Atomic(bool) rate_generator_hooked = false;
static void *rate_generator_original;
static uintptr_t rate_engine(uintptr_t core, const State *s, const RateProfile p) {
    uintptr_t readable = word(core + 0xa8);
    if (!table(readable, s->bias + p.readable)) return 0;
    uintptr_t manager = word(readable + 0x18);
    if (!table(manager, s->bias + p.manager)) return 0;
    uintptr_t mode = word(manager + 0x18);
    if (!table(mode, s->bias + p.mode)) return 0;
    unsigned which = (unsigned) (integer(mode + 0x18));
    if (which > 2) return 0;
    uintptr_t engine = word(manager + (which == 2 ? 0x40u : 0x38u));
    return (table(engine, s->bias + p.scratch) || table(engine, s->bias + p.pitch)) ? engine : 0;
}
static int rate_sign(double r) { return (r > 0) - (r < 0); }
static bool rate_number(double r) { return isfinite(r) && r >= -8 && r <= 8; }
static double rate_double(uintptr_t where) {
    uint64_t bits = __atomic_load_n(((const uint64_t *) (where)), __ATOMIC_ACQUIRE);
    double r;
    memcpy(&r, &bits, sizeof r);
    return r;
}
/* An active PlayerCore can clear an unreadable block without invoking the
 * source-rate generator. Its prior vector is not evidence for that call. */
typedef struct RateRenderScope RateRenderScope;
struct RateRenderScope {
    RateRenderScope *previous;
    Slot *slot;
    uintptr_t core;
    uintptr_t engine;
    azsdk_load_sample load;
    unsigned char source[12];
    unsigned deck;
    unsigned calls;
    int64_t generated_count;
    bool internal;
    bool nested;
    bool other;
};
static _Thread_local RateRenderScope *rate_render_current;
static bool rate_render_unchanged_source(RateRenderScope *ctx);
static void rate_render_init(RateRenderScope *ctx, void *self);
static void rate_render_drop(RateRenderScope *ctx);
static void rate_render_generated(uintptr_t self, int64_t frames);
static int rate_render_measured_frames(RateRenderScope *ctx, int requested);
static bool rate_render_stationary(uintptr_t core, unsigned frames);
static bool rate_render_unchanged_source(RateRenderScope *ctx) {
    (void) ctx;

    if (!ctx->internal || !ctx->slot || processing != ctx->slot || atomic_load_explicit(&(ctx->slot->core), memory_order_acquire) != ctx->core) return false;
    __auto_type s = &(state());
    if (s->build < 1 || s->build > 2) return false;
    __auto_type profile = &(rate_profiles[s->build - 1]);
    azsdk_load_sample after = {0};
    return rate_engine(ctx->core, &((*s)), (*profile)) == ctx->engine &&
           !(word(ctx->engine) == s->bias + profile->scratch && byte(ctx->engine + 0x58)) &&
           !memcmp(ctx->source, ((const void *) (ctx->core + 0x18)), sizeof ctx->source) &&
           azsdk_load_read((int) (ctx->deck), &after) && after.unit_generation == ctx->load.unit_generation &&
           after.invalidation_epoch == ctx->load.invalidation_epoch && after.load_generation == ctx->load.load_generation &&
           azn_load_track_matches(ctx->deck, after.invalidation_epoch, after.load_generation, ctx->source);
}
static void rate_render_init(RateRenderScope *ctx, void *self) {
    (void) ctx;
    *ctx = (RateRenderScope) {.slot = NULL, .engine = 0, .source = {0}, .deck = 0, .calls = 0, .generated_count = 0, .internal = false, .nested = false, .other = false, .previous = rate_render_current, .core = ((uintptr_t) (self))};

    if (ctx->previous) ctx->previous->nested = true;
    rate_render_current = ctx;
    __auto_type s = &(state());
    if (!atomic_load_explicit(&(rate_generator_hooked), memory_order_acquire) || !processing ||
        atomic_load_explicit(&(processing->core), memory_order_acquire) != ctx->core || s->build < 1 || s->build > 2) return;
    ctx->slot = processing;
    ctx->deck = (unsigned) (ctx->slot - s->decks);
    if (!azsdk_load_read((int) (ctx->deck), &ctx->load)) return;
    memcpy(ctx->source, ((const void *) (ctx->core + 0x18)), sizeof ctx->source);
    __auto_type profile = &(rate_profiles[s->build - 1]);
    ctx->engine = rate_engine(ctx->core, &((*s)), (*profile));
    ctx->internal = ctx->engine && !(word(ctx->engine) == s->bias + profile->scratch && byte(ctx->engine + 0x58)) &&
                    azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, ctx->source);
}
static void rate_render_drop(RateRenderScope *ctx) {
    (void) ctx;
    rate_render_current = ctx->previous;
}
static void rate_render_generated(uintptr_t self, int64_t frames) {

    if (!rate_render_current) return;
    if (rate_render_current->engine == self) {
        ++rate_render_current->calls;
        rate_render_current->generated_count = frames;
    } else rate_render_current->other = true;
}
static int rate_render_measured_frames(RateRenderScope *ctx, int requested) {
    (void) ctx;

    if (requested <= 0 || ctx->calls || ctx->nested || ctx->other || !rate_render_unchanged_source(ctx)) return requested;
    return 0;
}
static bool rate_render_stationary(uintptr_t core, unsigned frames) {

    __auto_type r = rate_render_current;
    if (!r || r->core != core || !frames || frames > 1024 || r->calls > 1 || r->nested || r->other ||
        !rate_render_unchanged_source(r)) return false;
        /* The qualified internal no-generator branch clears an unreadable
         * block. Otherwise require this call's actual generated advances;
         * an old zero vector cannot hide a source excursion. */
    if (!r->calls) return true;
    if (r->generated_count <= 0 || (uint64_t) (r->generated_count) != frames) return false;
    uintptr_t samples = word(r->engine + 0x18);
    if (!pointer(samples) || frames > word(r->engine + 0x20)) return false;
    for (unsigned n = 0; n < frames; ++n)
        if (rate_double(samples + sizeof(double) * n) != 0) return false;
    return true;
}


static void rate_generator(void *self, int64_t frames) {
    ((void (*)(void *, int64_t)) trampoline_address(&rate_generator_original))(self, frames);
    rate_render_generated(((uintptr_t) (self)), frames);
}
static void rate_capture(unsigned deck, Slot *slot, uintptr_t event_engine, int frames) {
    State *s = &(state());
    if (atomic_load_explicit(&(rate_hooks), memory_order_acquire) != 15 || !atomic_load_explicit(&(rate_generator_hooked), memory_order_acquire) ||
        !(azn_interest(deck) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) return;
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (deck), &load)) return;
    __auto_type r = &(rate_state[deck]);
    if (atomic_flag_test_and_set_explicit(&(r->busy), memory_order_acquire)) {
        atomic_fetch_add_explicit(&(r->fault), 1, memory_order_acq_rel);
        azn_rate_gap(deck);
        return;
    }
    EXIT_FLAG_GUARD(guard, &r->busy);
    const uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    if (!core || s->build < 1 || s->build > 2) return;
    const RateProfile *profile = &(rate_profiles[s->build - 1]);
    const uintptr_t engine = rate_engine(core, &((*s)), (*profile));
    atomic_store_explicit(&(r->bound_engine), engine, memory_order_release);
    if (event_engine && event_engine != engine) return;
    if (!azn_load_track_matches(deck, load.invalidation_epoch, load.load_generation,
                                ((const unsigned char *) (core + 0x18)))) return;
    uint64_t fault = atomic_load_explicit(&(r->fault), memory_order_acquire), seed = azn_seed_id(deck);
    unsigned avail = AZ_AVAIL_READY, dir_avail = AZ_AVAIL_READY;
    bool loss = false;
    double speed = 0;
    if (load.load_generation != r->load || !byte(core + 9)) r->rate_ramp = r->direction_ramp = false;
    if (byte(core + 9)) {
        if (!engine) avail = dir_avail = AZ_AVAIL_UNSUPPORTED;
        else {
            speed = rate_double(engine + 0x10);
            if (!rate_number(speed)) avail = dir_avail = AZ_AVAIL_STALE;
            if (avail == AZ_AVAIL_READY && frames > 0) {
                const bool external = word(engine) == s->bias + profile->scratch && byte(engine + 0x58);
                uintptr_t samples = word(engine + (external ? 0x60u : 0x18u));
                if (!pointer(samples) || frames > 1024 || (uint64_t) (frames) > word(engine + 0x20)) avail = dir_avail = AZ_AVAIL_STALE;
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
                        double sample = rate_double(samples + sizeof(double) * (unsigned) (n));
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
                r->rate_ramp = avail == AZ_AVAIL_STALE;
                r->direction_ramp = dir_avail == AZ_AVAIL_STALE;
            }
            if (r->rate_ramp) avail = AZ_AVAIL_STALE;
            if (r->direction_ramp) dir_avail = AZ_AVAIL_STALE;
        }
    }
    if (avail != AZ_AVAIL_READY) speed = dir_avail == AZ_AVAIL_READY ? rate_sign(speed) : 0;
    int dir = rate_sign(speed);
    double magnitude = avail == AZ_AVAIL_READY ? fabs(speed) : 0;
    uint64_t bits;
    memcpy(&bits, &magnitude, sizeof bits);
    azsdk_load_sample after = {0};
    if (!azsdk_load_read((int) (deck), &after) || after.load_generation != load.load_generation ||
        after.invalidation_epoch != load.invalidation_epoch || after.unit_generation != load.unit_generation ||
        !azn_load_track_matches(deck, after.invalidation_epoch, after.load_generation, ((const unsigned char *) (core + 0x18))) ||
        atomic_load_explicit(&(r->fault), memory_order_acquire) != fault) return;
    if (r->load == load.load_generation) {
        loss = (r->observed_fault != fault || (r->availability == AZ_AVAIL_READY && avail == AZ_AVAIL_STALE) ||
                (r->direction_avail == AZ_AVAIL_READY && dir_avail == AZ_AVAIL_STALE));
        if (!loss && r->seed == seed && r->availability == avail && r->direction_avail == dir_avail && r->direction == dir && r->bits == bits) return;
    }
    azstatus_native_receipt out = {0};
    out.kind = AZN_RATE;
    out.deck = deck;
    out.unit = load.unit_generation;
    out.epoch = load.invalidation_epoch;
    out.load_generation = load.load_generation;
    out.attempt = load.unit_generation;
    out.valid = (int) (avail);
    out.b = (int) (dir_avail);
    out.a = dir;
    memcpy(&out.points[0].samples, &bits, sizeof bits);
    if (loss) out.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    if (!azn_offer(&out)) return;
    r->load = load.load_generation;
    r->seed = seed;
    r->observed_fault = fault;
    r->availability = avail;
    r->direction_avail = dir_avail;
    r->direction = dir;
    r->bits = bits;
}
static void rate_writer_done(unsigned kind, uintptr_t self) {
    State *s = &(state());
    if (processing) {
        const unsigned d = (unsigned) (processing - s->decks);
        if (kind >= 2 || atomic_load_explicit(&(processing->core), memory_order_acquire) == self)
            rate_capture(d, &(*processing), kind >= 2 ? self : 0, 0);
        return;
    }
    for (unsigned d = 0; d < 4; ++d) {
        bool ours = kind < 2 ? atomic_load_explicit(&(s->decks[d].core), memory_order_acquire) == self : atomic_load_explicit(&(rate_state[d].bound_engine), memory_order_acquire) == self;
        if (ours && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))) {
            atomic_fetch_add_explicit(&(rate_state[d].fault), 1, memory_order_acq_rel);
            azn_rate_gap(d);
        }
    }
}
typedef struct RateActiveWrite RateActiveWrite;
struct RateActiveWrite {
    Motion *motion;
};
static void rate_active_write_init(RateActiveWrite *ctx, Motion *value);
static void rate_active_write_drop(RateActiveWrite *ctx);
static void rate_active_write_init(RateActiveWrite *ctx, Motion *value) {
    (void) ctx;
    *ctx = (RateActiveWrite) {.motion = value};

    if (ctx->motion) {
        atomic_fetch_add_explicit(&(ctx->motion->active_writers), 1, memory_order_acq_rel);
        atomic_fetch_add_explicit(&(ctx->motion->active_serial), 1, memory_order_acq_rel);
    }
}
static void rate_active_write_drop(RateActiveWrite *ctx) {
    (void) ctx;

    if (ctx->motion) {
        atomic_fetch_add_explicit(&(ctx->motion->active_serial), 1, memory_order_release);
        atomic_fetch_sub_explicit(&(ctx->motion->active_writers), 1, memory_order_release);
    }
}

static void rate_writer_0(void *self, unsigned arg0) {
    /* These two originals own Core+9. Fence every actual start/stop call,
     * including calls outside the current render owner and with no watch. */
    Motion *motion = NULL;
    if (0 < 2) {
        {
            for (unsigned range_index_1 = 0; range_index_1 < sizeof(state().decks) / sizeof(state().decks)[0]; ++range_index_1) {
                __auto_type slot = &(state().decks[range_index_1]);

                if (atomic_load_explicit(&(slot->core), memory_order_acquire) == ((uintptr_t) (self))) {
                    motion = &slot->motion;
                    break;
                }
            }
        }
    }
    {
        RateActiveWrite guard __attribute__((cleanup(rate_active_write_drop)));
        rate_active_write_init(&guard, motion);
        ((void (*)(void *, unsigned)) trampoline_address(&rate_original[0]))(self, arg0);
    }
    rate_writer_done(0, ((uintptr_t) (self)));
}
static void rate_writer_1(void *self) {
    /* These two originals own Core+9. Fence every actual start/stop call,
     * including calls outside the current render owner and with no watch. */
    Motion *motion = NULL;
    if (1 < 2) {
        {
            for (unsigned range_index_2 = 0; range_index_2 < sizeof(state().decks) / sizeof(state().decks)[0]; ++range_index_2) {
                __auto_type slot = &(state().decks[range_index_2]);

                if (atomic_load_explicit(&(slot->core), memory_order_acquire) == ((uintptr_t) (self))) {
                    motion = &slot->motion;
                    break;
                }
            }
        }
    }
    {
        RateActiveWrite guard __attribute__((cleanup(rate_active_write_drop)));
        rate_active_write_init(&guard, motion);
        ((void (*)(void *)) trampoline_address(&rate_original[1]))(self);
    }
    rate_writer_done(1, ((uintptr_t) (self)));
}
static void rate_writer_2(void *self, double arg0, unsigned arg1) {
    /* These two originals own Core+9. Fence every actual start/stop call,
     * including calls outside the current render owner and with no watch. */
    Motion *motion = NULL;
    if (2 < 2) {
        {
            for (unsigned range_index_3 = 0; range_index_3 < sizeof(state().decks) / sizeof(state().decks)[0]; ++range_index_3) {
                __auto_type slot = &(state().decks[range_index_3]);

                if (atomic_load_explicit(&(slot->core), memory_order_acquire) == ((uintptr_t) (self))) {
                    motion = &slot->motion;
                    break;
                }
            }
        }
    }
    {
        RateActiveWrite guard __attribute__((cleanup(rate_active_write_drop)));
        rate_active_write_init(&guard, motion);
        ((void (*)(void *, double, unsigned)) trampoline_address(&rate_original[2]))(self, arg0, arg1);
    }
    rate_writer_done(2, ((uintptr_t) (self)));
}
static void rate_writer_3(void *self, double arg0, unsigned arg1) {
    /* These two originals own Core+9. Fence every actual start/stop call,
     * including calls outside the current render owner and with no watch. */
    Motion *motion = NULL;
    if (3 < 2) {
        {
            for (unsigned range_index_4 = 0; range_index_4 < sizeof(state().decks) / sizeof(state().decks)[0]; ++range_index_4) {
                __auto_type slot = &(state().decks[range_index_4]);

                if (atomic_load_explicit(&(slot->core), memory_order_acquire) == ((uintptr_t) (self))) {
                    motion = &slot->motion;
                    break;
                }
            }
        }
    }
    {
        RateActiveWrite guard __attribute__((cleanup(rate_active_write_drop)));
        rate_active_write_init(&guard, motion);
        ((void (*)(void *, double, unsigned)) trampoline_address(&rate_original[3]))(self, arg0, arg1);
    }
    rate_writer_done(3, ((uintptr_t) (self)));
}
static void rate_install(State *s, int build, azn_setting_patch_fn patch) {
    const RateProfile *p = &(rate_profiles[build - 1]);
    void *hooks[] = {((void *) (rate_writer_0)), ((void *) (rate_writer_1)),
                     ((void *) (rate_writer_2)), ((void *) (rate_writer_3))};
    for (unsigned i = 0; i < 4; ++i)
        if (!(atomic_load_explicit(&(rate_hooks), memory_order_acquire) & (1u << i))) {
            uintptr_t site = s->bias + p->sites[i];
            if (memcmp(((const void *) (site)), &p->first[i], 4) ||
                patch(site, hooks[i], &rate_original[i], "status-rate") != 1 || !rate_original[i]) return;
            atomic_fetch_or_explicit(&(rate_hooks), 1u << i, memory_order_release);
        }
    if (!atomic_load_explicit(&(rate_generator_hooked), memory_order_acquire)) {
        const uintptr_t site = s->bias + (build == 1 ? 0xdb21e0u : 0xe3ca30u);
        const uint32_t first[2][4] = {{0x6d408002, 0xd2e7d002, 0x9e670043, 0xd10043ff},
                                      {0x6d408402, 0xd2e7d002, 0x9e670044, 0x7ee1d440}};
        if (memcmp(((const void *) (site)), first[build - 1], sizeof first[0]) ||
            patch(site, ((void *) (rate_generator)), &rate_generator_original, "status-rate-render") != 1 ||
            !rate_generator_original) return;
        atomic_store_explicit(&(rate_generator_hooked), true, memory_order_release);
    }
    for (unsigned d = 0; d < 4; ++d)
        if (atomic_load_explicit(&(s->decks[d].core), memory_order_acquire)) azn_capability(d, AZN_CAP_RATE, 1);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* PlayerCore owns both of these writes. processContinuousBlock compares the
 * returned Readable position's SourceInfo before committing Core+0x28;
 * setCurrentPos commits the explicit target. The GUI copy at UnitStateServer
 * +0x58 has already lost this ordering. */
static const uint64_t motion_routes = AZ_STATUS_TOPIC(AZ_STATUS_POSITION) |
                                      AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
static void *status_continuous_original;
static void *status_limit_original;
static void core_jump_hook(void *, const void *);

struct StatusHead {
    uint64_t sample;
    uint32_t rate;
    unsigned char source[12];
    bool ready;
};
static unsigned status_action(Slot *, const StatusHead, const StatusHead);
static bool status_jump_origin(const Slot *);
static StatusHead status_head(uintptr_t core) {
    StatusHead h = {.sample = 0, .rate = 0, .source = {0}, .ready = false};
    uint64_t source = __atomic_load_n(((const uint64_t *) (core + 0x18)), __ATOMIC_ACQUIRE);
    uint32_t id = __atomic_load_n(((const uint32_t *) (core + 0x20)), __ATOMIC_ACQUIRE);
    memcpy(h.source, &source, 8);
    memcpy(h.source + 8, &id, 4);
    h.sample = __atomic_load_n(((const uint64_t *) (core + 0x28)), __ATOMIC_ACQUIRE);
    h.rate = __atomic_load_n(((const uint32_t *) (core + 0x68)), __ATOMIC_ACQUIRE);
    const int range = compare_timestamps(h.sample, UINT64_C(9007199254740992), h.rate, h.rate, 1);
    h.ready = h.source[0] && (h.source[4] & 0x1f) && h.source[6] && id && h.rate &&
              h.rate < 0x7fffffffU && range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL;
    return h;
}
/* The stock load adopts through its owned BackCue callback, then copies the
 * committed point through controller-only calls. uintptr_t controller scope alone is
 * not an action classification. These cases still require the exact pending
 * load/source and consecutive same-position commits below. */
static bool status_load_setup_scope(Slot *slot, bool adopted) {
    if (causal.slot) return false;
    __auto_type scope = status_action_current;
    if (!scope) return true;
    if (!scope->controller || scope->command || scope->beat || scope->direct_kind) return false;
    if (!scope->callback) return !adopted;
    uintptr_t pause = 0, cue = 0;
    return adopted && transport_owners(slot, &(pause), &(cue)) &&
           table(scope->callback, state().profile.commands[7].table) &&
           word(scope->callback + 0x30) == cue && word(scope->callback + 0x38) == pause;
}
/* setCurrentPos can adopt the new source before TrackLoadFacade's completion
 * and outside the audio owner frame. Its original owns self and requested for
 * this call. Retain only scalars, and reject an overlapping/delayed setter. */
typedef struct StatusLanding StatusLanding;
struct StatusLanding {
    Slot *slot;
    unsigned deck;
    uintptr_t core;
    uint64_t stamp;
    uint64_t epoch;
    uint64_t request;
    uint64_t render_cut;
    bool exclusive;
    StatusHead from;
};
static void status_landing_init(StatusLanding *ctx, void *self);
static void status_landing_drop(StatusLanding *ctx);
static void status_landing_finish(StatusLanding *ctx, uintptr_t requested);
static void status_landing_init(StatusLanding *ctx, void *self) {
    (void) ctx;
    *ctx = (StatusLanding) {.slot = NULL, .deck = 0, .stamp = 0, .epoch = 0, .request = 0, .render_cut = 0, .exclusive = false, .core = ((uintptr_t) (self))};

    State *s = &(state());
    if (!(atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS) || !s->motion_hooked) return;
    for (ctx->deck = 0; ctx->deck < 4; ++ctx->deck)
        if (atomic_load_explicit(&(s->decks[ctx->deck].core), memory_order_acquire) == ctx->core) {
            ctx->slot = &s->decks[ctx->deck];
            ctx->exclusive = atomic_fetch_add_explicit(&(ctx->slot->motion.landing_writers), 1, memory_order_acq_rel) == 0;
            ctx->stamp = atomic_fetch_add_explicit(&(ctx->slot->motion.landing_order), 1, memory_order_acq_rel) + 1;
            ctx->epoch = azn_lifecycle_fence(ctx->deck);
            ctx->request = azn_load_pending(ctx->deck, ctx->epoch, NULL);
            ctx->render_cut = atomic_load_explicit(&(ctx->slot->motion.adoption_render), memory_order_acquire);
            ctx->from = status_head(ctx->core);
            break;
        }
}
static void status_landing_drop(StatusLanding *ctx) {
    (void) ctx;

    if (ctx->slot) atomic_fetch_sub_explicit(&(ctx->slot->motion.landing_writers), 1, memory_order_release);
}
static void status_landing_finish(StatusLanding *ctx, uintptr_t requested) {
    (void) ctx;

    if (!ctx->slot || !ctx->exclusive || !requested || !ctx->stamp || !ctx->epoch || atomic_load_explicit(&(ctx->slot->core), memory_order_acquire) != ctx->core) return;
    StatusHead to = status_head(ctx->core);
    const int target_order = compare_timestamps(word(requested + 0x18), to.sample, to.rate, to.rate, 1);
    if (!to.ready || target_order == AZ_TIME_INVALID || target_order != AZ_TIME_EQUAL ||
        memcmp(((const void *) (requested + 8)), to.source, 12) ||
        azn_lifecycle_fence(ctx->deck) != ctx->epoch) return;
    __auto_type m = &(ctx->slot->motion);
    if (atomic_flag_test_and_set_explicit(&(m->landing_publish), memory_order_acquire)) return;
    EXIT_FLAG_GUARD(unlock, &m->landing_publish);
    if (atomic_load_explicit(&(m->landing_order), memory_order_acquire) != ctx->stamp || atomic_load_explicit(&(m->landing_writers), memory_order_acquire) != 1) return;
    uint64_t lo;
    uint32_t hi;
    memcpy(&lo, to.source, 8);
    memcpy(&hi, to.source + 8, 4);
    if (!memcmp(ctx->from.source, to.source, 12) &&
        !(ctx->stamp > 1 && atomic_load_explicit(&(m->landing_stamp), memory_order_acquire) == ctx->stamp - 1 && atomic_load_explicit(&(m->landing_epoch), memory_order_acquire) == ctx->epoch &&
          atomic_load_explicit(&(m->landing_lo), memory_order_acquire) == lo && atomic_load_explicit(&(m->landing_hi), memory_order_acquire) == hi)) return;
    atomic_store_explicit(&(m->landing_lo), lo, memory_order_release);
    atomic_store_explicit(&(m->landing_hi), hi, memory_order_release);
    const bool adopted = memcmp(ctx->from.source, to.source, 12) != 0;
    const bool pending = ctx->request && azn_load_pending(ctx->deck, ctx->epoch, to.source) == ctx->request &&
                         status_load_setup_scope(&(*ctx->slot), adopted) &&
                         !atomic_load_explicit(&(m->adoption_render_writers), memory_order_acquire) &&
                         atomic_load_explicit(&(m->adoption_render), memory_order_acquire) == ctx->render_cut;
    if (adopted && (azn_interest(ctx->deck) & motion_routes)) {
        atomic_store_explicit(&(m->opening_landing), ctx->stamp, memory_order_release);
        atomic_store_explicit(&(m->adoption_sample), to.sample, memory_order_release);
        atomic_store_explicit(&(m->adoption_rate), to.rate, memory_order_release);
        m->adoption_render_cut = ctx->render_cut;
        m->adoption_request = pending ? ctx->request : 0;
    } else if (!adopted && pending && m->adoption_request == ctx->request &&
               m->adoption_render_cut == ctx->render_cut && atomic_load_explicit(&(m->opening_landing), memory_order_acquire) == ctx->stamp - 1 &&
               ctx->from.ready && ctx->from.rate == to.rate && atomic_load_explicit(&(m->adoption_rate), memory_order_acquire) == to.rate) {
            /* Stock load setup can copy its first nonzero source position
             * several times before completion. Preserve only consecutive
             * exact copies of that adoption, with no intervening render or
             * action. uintptr_t later seek or excursion can never extend this cut. */
        const int before = compare_timestamps(ctx->from.sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), to.rate, to.rate, 1);
        const int after = compare_timestamps(to.sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), to.rate, to.rate, 1);
        if (before != AZ_TIME_INVALID && after != AZ_TIME_INVALID &&
            before == AZ_TIME_EQUAL && after == AZ_TIME_EQUAL)
            atomic_store_explicit(&(m->opening_landing), ctx->stamp, memory_order_release);
    }
    atomic_store_explicit(&(m->landing_epoch), ctx->epoch, memory_order_release);
    atomic_store_explicit(&(m->landing_stamp), ctx->stamp, memory_order_release);
}



#include "sdk_status_map_clock.inc"


#include "sdk_status_end.inc"


/* uintptr_t background-clock reader can borrow a generation only after the motion
 * receipt is sealed. uintptr_t failed barrier leaves that generation unavailable. */
static bool status_offer_motion(Motion *motion, azstatus_native_receipt *receipt) {
    const bool renew = atomic_load_explicit(&(motion->slip_load), memory_order_relaxed) != receipt->load_generation ||
                       atomic_load_explicit(&(motion->slip_generation), memory_order_relaxed) != receipt->motion_generation ||
                       atomic_load_explicit(&(motion->slip_interference), memory_order_relaxed) != motion->captured_interference;
    if (renew) atomic_store_explicit(&(motion->slip_load), 0, memory_order_release);
    if (!azn_offer(receipt)) return false;
    if (renew) {
        atomic_store_explicit(&(motion->slip_interference), motion->captured_interference, memory_order_relaxed);
        atomic_store_explicit(&(motion->slip_generation), receipt->motion_generation, memory_order_relaxed);
        atomic_store_explicit(&(motion->slip_load), receipt->load_generation, memory_order_release);
    }
    return true;
}

/* Caller owns OBSERVE or HEAD. Load completion may occur inside the outer
 * owner frame; its first matched continuous writer must establish the same
 * baseline before an ordinary motion receipt can make a later seed obsolete. */
static void status_stationary_seed_locked(unsigned deck, Slot *slot, const azsdk_load_sample *snapshot) {
    if (!(azn_interest(deck) & motion_routes)) return;
    __auto_type m = &(slot->motion);
    const uint64_t epoch = azn_lifecycle_fence(deck);
    if (!epoch || atomic_load_explicit(&(m->foreign), memory_order_acquire) || atomic_load_explicit(&(m->landing_writers), memory_order_acquire)) return;
    const uint64_t order = atomic_load_explicit(&(m->landing_order), memory_order_acquire);
    azsdk_load_sample load = {0};
    if (snapshot) load = *snapshot;
    bool matched = (snapshot || azsdk_load_read((int) (deck), &load)) && load.invalidation_epoch == epoch;
    if (matched && m->load == load.load_generation && m->order) return;
    __auto_type head = status_head(atomic_load_explicit(&(slot->core), memory_order_acquire));
    if (azn_lifecycle_fence(deck) != epoch || atomic_load_explicit(&(m->landing_order), memory_order_acquire) != order ||
        atomic_load_explicit(&(m->landing_writers), memory_order_acquire)) return;
    // uintptr_t baseline can also come from two real owner frames under this one
    // fence: the prior source was still active, then the requested one was
    // adopted. No path or occurrence is reconstructed between these frames.
    if (m->frame_epoch != epoch) {
        m->frame_epoch = epoch;
        m->frame_landing_order = order;
        memcpy(m->first_source, head.source, 12);
    }
    if (!matched || !head.ready) return;
    uint64_t lo;
    uint32_t hi;
    memcpy(&lo, head.source, 8);
    memcpy(&hi, head.source + 8, 4);
    bool landed = order && atomic_load_explicit(&(m->landing_stamp), memory_order_acquire) == order && atomic_load_explicit(&(m->landing_epoch), memory_order_acquire) == epoch &&
                  atomic_load_explicit(&(m->landing_lo), memory_order_acquire) == lo && atomic_load_explicit(&(m->landing_hi), memory_order_acquire) == hi;
    if ((!landed && !memcmp(m->first_source, head.source, 12)) ||
        !azn_load_track_matches(deck, epoch, load.load_generation, head.source) ||
        atomic_load_explicit(&(m->landing_order), memory_order_acquire) != order || atomic_load_explicit(&(m->foreign), memory_order_acquire)) return;
    azstatus_native_receipt r = {0};
    r.kind = AZN_MOTION;
    r.a = AZN_MOTION_SEED;
    r.deck = deck;
    r.unit = load.unit_generation;
    r.epoch = load.invalidation_epoch;
    r.load_generation = load.load_generation;
    r.motion_order = 1;
    r.motion_generation = 1;
    r.points[0].state = AZN_INVALID;
    r.points[1] = (__typeof__(r.points[1])) {AZN_COORD, head.rate, ((int64_t) (head.sample))};
    r.observed_window_samples = 1;
    r.observed_window_rate = head.rate;
    uintptr_t pause = 0, cue = 0;
    /* The stationary seed above already proved that this load adopted the
     * source. uintptr_t cued zero is the opening even when several native setters
     * copied that same initial zero before completion. Nonzero openings use
     * the original adoption or its proven pre-completion copies. The reducer
     * still requires the actual first phrase boundary, never a middle cue. */
    const int opening_order = compare_timestamps(head.sample, 0, head.rate, head.rate, 1);
    const int adopted_order = compare_timestamps(head.sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), head.rate, head.rate, 1);
    bool at_adoption = atomic_load_explicit(&(m->adoption_rate), memory_order_acquire) == head.rate &&
                       adopted_order != AZ_TIME_INVALID && adopted_order == AZ_TIME_EQUAL;
    bool initial = opening_order != AZ_TIME_INVALID && (opening_order == AZ_TIME_EQUAL ||
                                                        (landed ? at_adoption && atomic_load_explicit(&(m->opening_landing), memory_order_acquire) == order : order == m->frame_landing_order));
    bool opening = initial && transport_owners(slot, &(pause), &(cue)) && !byte(pause + 8);
    m->opening_sample = head.sample;
    m->opening_rate = head.rate;
    atomic_store_explicit(&(m->opening), opening, memory_order_release);
    if (opening) r.motion_flags = AZN_AT_OPENING;
    if (read_map_clock(deck, load.load_generation, &(r.map)) == UINT64_MAX || atomic_load_explicit(&(map_mutations[deck].active), memory_order_acquire))
        r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    m->load = load.load_generation;
    m->order = 1;
    m->generation = 1;
    m->known = true;
    atomic_store_explicit(&(map_mutations[deck].last_order), 1, memory_order_release);
    m->previous = head.sample;
    m->rate = head.rate;
    m->window_samples = 1;
    m->window_rate = head.rate;
    memcpy(m->identity, head.source, 12);
    m->seed = azn_seed_id(deck);
    m->captured_interference = atomic_load_explicit(&(m->interference), memory_order_acquire);
    status_offer_motion(m, &(r));
}
static void status_stationary_seed(unsigned deck, Slot *slot) {
    if (!(azn_interest(deck) & motion_routes)) return;
    __auto_type m = &(slot->motion);
    unsigned reservation = MOTION_FREE;
    if (!atomic_compare_exchange_strong_explicit(&(m->busy), &(reservation), MOTION_OBSERVE, memory_order_acquire, memory_order_acquire)) return;
    EXIT_ATOMIC_GUARD(unlock, &m->busy);
    status_stationary_seed_locked(deck, &((*slot)), NULL);
}
/* uintptr_t real PAUSED -> PLAYING commit at the load position can precede the first
 * advancing render block. This consumes the opening exactly once. The next
 * render remains an ordinary travel measurement. */
static void status_opening_play(unsigned d, Slot *slot, const azsdk_load_sample load) {
    __auto_type m = &(slot->motion);
    if (!atomic_load_explicit(&(m->opening), memory_order_acquire)) return;
    unsigned reservation = MOTION_FREE;
    if (!atomic_compare_exchange_strong_explicit(&(m->busy), &(reservation), MOTION_OBSERVE, memory_order_acquire, memory_order_acquire)) {
        if (atomic_exchange_explicit(&(m->opening), false, memory_order_acq_rel)) azn_motion_gap(d, 0);
        return;
    }
    EXIT_ATOMIC_GUARD(unlock, &m->busy);
    if (!atomic_exchange_explicit(&(m->opening), false, memory_order_acq_rel) || m->load != load.load_generation || !m->known ||
        !m->rate || !m->window_samples || !m->window_rate || !m->generation || m->order == UINT64_MAX) return;
    azstatus_native_receipt r = {0};
    __auto_type head = status_head(atomic_load_explicit(&(slot->core), memory_order_acquire));
    azsdk_load_sample after = {0};
    const int head_order = compare_timestamps(head.sample, m->previous, head.rate, m->window_rate, m->window_samples);
    if (!head.ready || head_order == AZ_TIME_INVALID || head_order != AZ_TIME_EQUAL || head.rate != m->rate || memcmp(head.source, m->identity, 12) ||
        !azsdk_load_read((int) (d), &after) || after.unit_generation != load.unit_generation ||
        after.invalidation_epoch != load.invalidation_epoch || after.load_generation != load.load_generation ||
        atomic_load_explicit(&(m->foreign), memory_order_acquire) || m->captured_interference != atomic_load_explicit(&(m->interference), memory_order_acquire) ||
        !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, head.source)) return;
    r.kind = AZN_MOTION;
    r.a = AZN_MEASURE;
    r.deck = d;
    r.unit = load.unit_generation;
    r.epoch = load.invalidation_epoch;
    r.load_generation = m->load;
    r.motion_order = ++m->order;
    r.motion_generation = m->generation;
    r.motion_flags = AZN_BEGAN_PLAY;
    r.points[0] = r.points[1] = (__typeof__(r.points[1])) {AZN_COORD, m->rate, ((int64_t) (head.sample))};
    r.observed_window_samples = r.from_window_samples = m->window_samples;
    r.observed_window_rate = r.from_window_rate = m->window_rate;
    if (read_map_clock(d, m->load, &(r.map)) == UINT64_MAX || atomic_load_explicit(&(map_mutations[d].active), memory_order_acquire))
        r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    atomic_store_explicit(&(map_mutations[d].last_order), m->order, memory_order_release);
    azn_emit(&r);
}
typedef struct StatusMotionWrite StatusMotionWrite;
struct StatusMotionWrite {
    StatusMotionWrite *previous_setter;
    Slot *slot;
    uintptr_t core;
    unsigned deck;
    unsigned window_samples;
    bool rendering;
    uint64_t fence;
    uint64_t map_before;
    uint64_t mutation_before;
    uint64_t active_before;
    uint64_t landing_before;
    uintptr_t fixed_max_owner;
    uintptr_t stationary_engine;
    uintptr_t limit_owner;
    int64_t observed_limit;
    unsigned limit_calls;
    bool setter_scope;
    azsdk_load_sample load;
    StatusHead from;
    bool locked;
    bool competing;
    bool reposition;
    bool paused_map;
};
static _Thread_local StatusMotionWrite *status_motion_write_current_setter;
static bool status_motion_write_stationary_block(StatusMotionWrite *ctx);
static uintptr_t status_motion_write_fixed_max_provider(StatusMotionWrite *ctx);
static void status_motion_write_capture_limit(uintptr_t owner, int64_t limit, uintptr_t caller);
static void status_motion_write_init(StatusMotionWrite *ctx, void *self, bool explicit_position, unsigned samples);
static void status_motion_write_drop(StatusMotionWrite *ctx);
static void status_motion_write_finish(StatusMotionWrite *ctx, uintptr_t requested);
static bool status_motion_write_stationary_block(StatusMotionWrite *ctx) {
    (void) ctx;

    __auto_type s = &(state());
    if (!ctx->rendering) return !byte(ctx->core + 9);
    if (!byte(ctx->core + 9) || !ctx->stationary_engine || !ctx->window_samples || ctx->window_samples > 1024 || s->build < 1 || s->build > 2) return false;
    if (rate_render_stationary(ctx->core, ctx->window_samples)) return true;
    __auto_type profile = &(rate_profiles[s->build - 1]);
    if (rate_engine(ctx->core, &((*s)), (*profile)) != ctx->stationary_engine) return false;
    const bool external = word(ctx->stationary_engine) == s->bias + profile->scratch && byte(ctx->stationary_engine + 0x58);
    const uintptr_t samples = word(ctx->stationary_engine + (external ? 0x60u : 0x18u));
    if (!pointer(samples) || ctx->window_samples > word(ctx->stationary_engine + 0x20)) return false;
        /* These are dimensionless signed source advances from this completed
         * render. uintptr_t net-zero forward/reverse block is not stationary. */
    for (unsigned n = 0; n < ctx->window_samples; ++n)
        if (rate_double(samples + sizeof(double) * n) != 0) return false;
    return true;
}
static uintptr_t status_motion_write_fixed_max_provider(StatusMotionWrite *ctx) {
    (void) ctx;

        /* setCurrentPos can return before its store when its provider rejects
         * an out-of-range target. For equal-coordinate actions prove the
         * built-in maximum branch without calling a getter on the writer. */
    __auto_type s = &(state());
    if (s->build < 1 || s->build > 2) return 0;
    uintptr_t provider = word(ctx->core + 0xb8);
    if (!pointer(provider)) return 0;
    uintptr_t table = word(provider);
    const uintptr_t getter = s->bias + (s->build == 1 ? 0x765ff0u : 0x7a78b8u);
    return pointer(table) && word(table + 0x60) == getter ? provider : 0;
}
static void status_motion_write_capture_limit(uintptr_t owner, int64_t limit, uintptr_t caller) {

    __auto_type writer = status_motion_write_current_setter;
    __auto_type s = &(state());
    if (!writer || !writer->locked || writer->slot != processing || s->build < 1 || s->build > 2 ||
        caller != s->bias + (s->build == 1 ? 0x14ac994u : 0x1567dc4u) ||
        atomic_load_explicit(&(writer->slot->core), memory_order_acquire) != writer->core || word(writer->core + 0xb8) != owner) return;
    ++writer->limit_calls;
    writer->limit_owner = owner;
    writer->observed_limit = limit;
}
static void status_motion_write_init(StatusMotionWrite *ctx, void *self, bool explicit_position, unsigned samples) {
    (void) ctx;
    *ctx = (StatusMotionWrite) {.previous_setter = NULL, .slot = NULL, .deck = 0, .rendering = false, .fence = 0, .map_before = 0, .mutation_before = 0, .active_before = 0, .landing_before = 0, .fixed_max_owner = 0, .stationary_engine = 0, .limit_owner = 0, .observed_limit = 0, .limit_calls = 0, .setter_scope = false, .locked = false, .competing = false, .paused_map = false, .core = ((uintptr_t) (self)), .window_samples = samples, .reposition = explicit_position};

    if (ctx->reposition) {
            /* An unowned nested setter must shadow the outer scope too: all
             * decks can share one buffering scheduler. */
        ctx->previous_setter = status_motion_write_current_setter;
        status_motion_write_current_setter = ctx;
        ctx->setter_scope = true;
    }
    __auto_type s = &(state());
    if (!(atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS) || !s->motion_hooked) return;
    for (ctx->deck = 0; ctx->deck < 4; ++ctx->deck)
        if (atomic_load_explicit(&(s->decks[ctx->deck].core), memory_order_acquire) == ctx->core) {
            ctx->slot = &s->decks[ctx->deck];
            break;
        }
    if (!ctx->slot || !(azn_interest(ctx->deck) & (motion_routes | AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)))) return;
        /* uintptr_t competing or unowned mutation never dereferences a saved pointer
         * and never stamps a preexisting coordinate with the newer fence. */
    unsigned reservation = MOTION_FREE;
    if (ctx->slot != processing || !atomic_compare_exchange_strong_explicit(&(ctx->slot->motion.busy), &(reservation), MOTION_HEAD, memory_order_acquire, memory_order_acquire)) {
            /* uintptr_t map cut can overlap a paused render even while Core+9 is
             * active. Defer this one verdict until the completed block: the
             * native sample advances, source point and all writer fences
             * must prove that no source travel occurred. */
        if (ctx->slot == processing && reservation == MOTION_MAP && !ctx->reposition && samples &&
            (atomic_load_explicit(&(rate_hooks), memory_order_acquire) & 3) == 3 &&
            !atomic_load_explicit(&(ctx->slot->motion.active_writers), memory_order_acquire) &&
            !atomic_load_explicit(&(ctx->slot->motion.landing_writers), memory_order_acquire) &&
            !atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) && azsdk_load_read((int) (ctx->deck), &ctx->load)) {
            ctx->active_before = atomic_load_explicit(&(ctx->slot->motion.active_serial), memory_order_acquire);
            ctx->landing_before = atomic_load_explicit(&(ctx->slot->motion.landing_order), memory_order_acquire);
            ctx->fence = atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire);
            ctx->from = status_head(ctx->core);
            ctx->rendering = byte(ctx->core + 9);
            if (ctx->rendering && s->build >= 1 && s->build <= 2)
                ctx->stationary_engine = rate_engine(ctx->core, &((*s)), rate_profiles[s->build - 1]);
            ctx->paused_map = true;
            return;
        }
        ctx->competing = true;
        atomic_store_explicit(&(ctx->slot->motion.opening), false, memory_order_release);
        atomic_fetch_add_explicit(&(ctx->slot->motion.foreign), 1, memory_order_acq_rel);
        atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
        azsdk_load_sample active = {0};
        if (azsdk_load_read((int) (ctx->deck), &active)) azn_motion_gap(ctx->deck, ctx->reposition);
        return;
    }
    ctx->locked = true;
    ctx->fence = atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire);
    if (!atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) && azsdk_load_read((int) (ctx->deck), &ctx->load)) {
        if (!ctx->reposition && (!ctx->slot->motion.order || ctx->slot->motion.load != ctx->load.load_generation))
            status_stationary_seed_locked(ctx->deck, &(*ctx->slot), &ctx->load);
        ctx->mutation_before = atomic_load_explicit(&(map_mutations[ctx->deck].generation), memory_order_acquire);
        aznm_packet stamp = {0};
        ctx->map_before = read_map_clock(ctx->deck, ctx->load.load_generation, &(stamp));
        ctx->from = status_head(ctx->core);
        ctx->rendering = !ctx->reposition && samples && byte(ctx->core + 9);
        if (!ctx->reposition) {
            ctx->active_before = atomic_load_explicit(&(ctx->slot->motion.active_serial), memory_order_acquire);
            ctx->landing_before = atomic_load_explicit(&(ctx->slot->motion.landing_order), memory_order_acquire);
            if (ctx->rendering && (atomic_load_explicit(&(rate_hooks), memory_order_acquire) & 3) == 3 && s->build >= 1 && s->build <= 2)
                ctx->stationary_engine = rate_engine(ctx->core, &((*s)), rate_profiles[s->build - 1]);
        }
        if (ctx->reposition) ctx->fixed_max_owner = status_motion_write_fixed_max_provider(ctx);
    }
}
static void status_motion_write_drop(StatusMotionWrite *ctx) {
    (void) ctx;

    if (ctx->setter_scope) status_motion_write_current_setter = ctx->previous_setter;
    if (ctx->paused_map) {
        atomic_store_explicit(&(ctx->slot->motion.opening), false, memory_order_release);
        atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
        azn_motion_gap(ctx->deck, 0);
    }
    if (ctx->competing) {
        atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
        atomic_fetch_sub_explicit(&(ctx->slot->motion.foreign), 1, memory_order_release);
    }
    if (ctx->locked) atomic_store_explicit(&(ctx->slot->motion.busy), MOTION_FREE, memory_order_release);
}
static void status_motion_write_finish(StatusMotionWrite *ctx, uintptr_t requested) {
    (void) ctx;

    if (ctx->paused_map) {
        const StatusHead to = status_head(ctx->core);
        azsdk_load_sample after = {0};
        const int stationary = compare_timestamps(ctx->from.sample, to.sample, ctx->from.rate, ctx->from.rate, 1);
        const bool inactive = ctx->from.ready && to.ready && stationary != AZ_TIME_INVALID && stationary == AZ_TIME_EQUAL && ctx->from.rate == to.rate &&
                              !memcmp(ctx->from.source, to.source, 12) &&
                              ctx->slot == processing && atomic_load_explicit(&(ctx->slot->core), memory_order_acquire) == ctx->core && status_motion_write_stationary_block(ctx) &&
                              !atomic_load_explicit(&(ctx->slot->motion.active_writers), memory_order_acquire) &&
                              atomic_load_explicit(&(ctx->slot->motion.active_serial), memory_order_acquire) == ctx->active_before &&
                              !atomic_load_explicit(&(ctx->slot->motion.landing_writers), memory_order_acquire) &&
                              atomic_load_explicit(&(ctx->slot->motion.landing_order), memory_order_acquire) == ctx->landing_before &&
                              !atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) &&
                              atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire) == ctx->fence &&
                              azsdk_load_read((int) (ctx->deck), &after) && after.unit_generation == ctx->load.unit_generation &&
                              after.invalidation_epoch == ctx->load.invalidation_epoch && after.load_generation == ctx->load.load_generation &&
                              azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, to.source);
        if (!inactive) {
            atomic_store_explicit(&(ctx->slot->motion.opening), false, memory_order_release);
            atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
            azn_motion_gap(ctx->deck, 0);
        }
        ctx->paused_map = false;
        return;
    }
    if (!ctx->locked || !ctx->load.load_generation) return;
    StatusHead to = status_head(ctx->core);
    azsdk_load_sample after = {0};
    if (!azsdk_load_read((int) (ctx->deck), &after) || ctx->load.unit_generation != after.unit_generation ||
        ctx->load.load_generation != after.load_generation || ctx->load.invalidation_epoch != after.invalidation_epoch ||
        atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) ||
        atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire) != ctx->fence) return;
    ctx->from.ready = ctx->from.ready && azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, ctx->from.source);
    to.ready = to.ready && azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, to.source);
    __auto_type m = &(ctx->slot->motion);
    const bool same_load = m->load == ctx->load.load_generation;
    const bool same_source = ctx->from.ready && to.ready && ctx->from.rate == to.rate && !memcmp(ctx->from.source, to.source, 12);
    const int movement = compare_timestamps(ctx->from.sample, to.sample, ctx->from.rate, ctx->from.rate, ctx->window_samples);
    const int prior_order = compare_timestamps(m->previous, ctx->from.sample, m->rate, m->window_rate, m->window_samples);
    const bool moved = same_source && movement != AZ_TIME_INVALID && movement != AZ_TIME_EQUAL;
    const bool previous = same_load && m->known && ctx->from.ready && m->rate == ctx->from.rate &&
                          !memcmp(m->identity, ctx->from.source, 12) &&
                          m->window_samples && m->window_rate &&
                          prior_order != AZ_TIME_INVALID && prior_order == AZ_TIME_EQUAL;
    bool target = false;
    if (requested && to.ready) {
        const int target_order = compare_timestamps(word(requested + 0x18), to.sample, to.rate, to.rate, 1);
        target = target_order != AZ_TIME_INVALID && target_order == AZ_TIME_EQUAL &&
                 !memcmp(((const void *) (requested + 8)), to.source, 12);
    }
    const uint64_t seed = azn_seed_id(ctx->deck);
        /* An active engine can render a paused source at zero speed. Keep
         * its opening until cumulative travel leaves the observed window,
         * rather than treating render activity itself as a departure. */
    const int from_opening = compare_timestamps(m->opening_sample, to.sample, to.rate, to.rate, ctx->window_samples);
    if (!same_load || !same_source || m->opening_rate != to.rate || from_opening == AZ_TIME_INVALID ||
        from_opening != AZ_TIME_EQUAL || m->captured_interference != ctx->fence || (ctx->reposition && (!previous || !target)))
        atomic_store_explicit(&(m->opening), false, memory_order_release);
    if (!same_load) {
        m->load = ctx->load.load_generation;
        m->generation = 1;
        m->order = 0;
        m->known = false;
    }
    bool gap = m->order && ((m->known && !previous) || m->captured_interference != ctx->fence);
    if (!ctx->reposition && !ctx->rendering && !moved && !gap && !(same_load && m->known && seed != m->seed)) return;
        /* uintptr_t stationary copy left behind by a previous load is never a first
         * seed. uintptr_t verified post-load advance or a matched setter establishes it. */
    if (!m->order && !(ctx->rendering || moved || target)) return;
    if (m->order == UINT64_MAX || m->generation == UINT64_MAX) {
        azn_motion_gap(ctx->deck, ctx->reposition);
        return;
    }
    azstatus_native_receipt r = {0};
    r.kind = AZN_MOTION;
    r.deck = ctx->deck;
    r.epoch = ctx->load.invalidation_epoch;
    r.load_generation = ctx->load.load_generation;
    r.unit = ctx->load.unit_generation;
    r.observed_window_samples = ctx->window_samples;
    r.observed_window_rate = ctx->window_samples ? to.rate : 0;
    if (previous) {
        r.from_window_samples = m->window_samples;
        r.from_window_rate = m->window_rate;
    }
    r.a = ctx->reposition || gap ? AZN_REPOSITION : previous ? AZN_MEASURE
                                                             : AZN_MOTION_SEED;
    if (ctx->reposition && target && state().motion_actions) {
        unsigned action = status_action(&(*ctx->slot), ctx->from, to);
        const int fixed_range = compare_timestamps(to.sample, UINT64_C(0x5ea0da80), to.rate, to.rate, 1);
        const int actual_range = compare_timestamps(to.sample, ctx->observed_limit, to.rate, to.rate, 1);
            /* The actual setter's getter call proves its range guard. Equal
             * coordinates alone cannot distinguish a store from rejection. */
        bool stored = moved || !same_source ||
                      (ctx->fixed_max_owner && ctx->fixed_max_owner == status_motion_write_fixed_max_provider(ctx) && fixed_range != AZ_TIME_INVALID && fixed_range <= AZ_TIME_EQUAL) ||
                      (ctx->limit_calls == 1 && ctx->limit_owner && word(ctx->core + 0xb8) == ctx->limit_owner && actual_range != AZ_TIME_INVALID && actual_range <= AZ_TIME_EQUAL);
        if (action && stored && m->occurrence < UINT64_MAX) {
            r.a = action;
            r.occurrence_ordinal = ++m->occurrence;
        } else if (action) {
            r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | motion_routes;
        } else if (previous && moved && same_source && status_jump_origin(&(*ctx->slot))) {
                /* The stock controller moved the active source. Preserve its
                 * measured landing while reporting the unclassified action. */
            r.history_only = 1;
            r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        }
    }
    if ((ctx->reposition || gap) && m->order) ++m->generation;
    r.motion_generation = m->generation;
    uint64_t map_after = read_map_clock(ctx->deck, ctx->load.load_generation, &(r.map));
    const bool map_ambiguous = (ctx->map_before || map_after) &&
                               (ctx->map_before == UINT64_MAX || map_after == UINT64_MAX || ctx->map_before != map_after ||
                                ctx->mutation_before != atomic_load_explicit(&(map_mutations[ctx->deck].generation), memory_order_acquire) ||
                                atomic_load_explicit(&(map_mutations[ctx->deck].active), memory_order_acquire));
    if (gap || (previous && ctx->reposition && !target))
        r.lost_topics |= motion_routes;
    if (seed != m->seed && m->order && previous) r.c = 1; /* a fresh recovery baseline */
        /* The first paired map can be published while this writer owns HEAD,
         * so its receipt may have no opening cut. Reannounce the intact cued
         * opening on a proven stationary block once the map becomes usable. */
    const int opening_motion = compare_timestamps(ctx->from.sample, to.sample, to.rate, to.rate, 1);
    const bool stationary_opening = !ctx->reposition && previous && atomic_load_explicit(&(m->opening), memory_order_acquire) &&
                                    opening_motion != AZ_TIME_INVALID && opening_motion == AZ_TIME_EQUAL && status_motion_write_stationary_block(ctx) &&
                                    !atomic_load_explicit(&(m->active_writers), memory_order_acquire) && !atomic_load_explicit(&(m->landing_writers), memory_order_acquire) &&
                                    atomic_load_explicit(&(m->active_serial), memory_order_acquire) == ctx->active_before && atomic_load_explicit(&(m->landing_order), memory_order_acquire) == ctx->landing_before;
        /* Qualifying the initial map cannot omit a crossing while this
         * opening's actual source advances are all zero. Unknown or moving
         * blocks retain their mapping-history loss. */
    if (map_ambiguous && !stationary_opening) r.lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        /* Mapping history has its own loss domain. An intact cued source
         * can remain at its opening while its first map is qualified. */
    if (r.occurrence_ordinal || (r.lost_topics & ~AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)))
        atomic_store_explicit(&(m->opening), false, memory_order_release);
    else if ((ctx->reposition || r.c || stationary_opening) && atomic_load_explicit(&(m->opening), memory_order_acquire)) r.motion_flags |= AZN_AT_OPENING;
    if (previous) r.points[0] = (__typeof__(r.points[0])) {AZN_COORD, ctx->from.rate, ((int64_t) (ctx->from.sample))};
    else r.points[0].state = AZN_INVALID;
    if (to.ready && (!ctx->reposition || target))
        r.points[1] = (__typeof__(r.points[1])) {AZN_COORD, to.rate, ((int64_t) (to.sample))};
    else r.points[1].state = AZN_INVALID;
    const int exact_previous = compare_timestamps(m->previous, ctx->from.sample, ctx->from.rate, ctx->from.rate, 1);
    __auto_type end = &(end_state[ctx->deck]);
    const int terminal = compare_timestamps(to.sample, end->length, to.rate, to.rate, ctx->window_samples);
    const bool before_terminal = end->load != ctx->load.load_generation || end->epoch != ctx->load.invalidation_epoch ||
                                 !end->length || (end->length_rate == to.rate && terminal != AZ_TIME_INVALID && terminal == AZ_TIME_BEFORE);
    const bool stationary = !ctx->reposition && !gap && previous && same_source && !r.c && !r.lost_topics &&
                            !r.occurrence_ordinal && !r.history_only && !map_ambiguous && map_after != UINT64_MAX &&
                            exact_previous != AZ_TIME_INVALID && exact_previous == AZ_TIME_EQUAL &&
                            opening_motion != AZ_TIME_INVALID && opening_motion == AZ_TIME_EQUAL &&
                            ctx->window_samples == m->window_samples && to.rate == m->window_rate &&
                            !atomic_load_explicit(&(m->active_writers), memory_order_acquire) && !atomic_load_explicit(&(m->landing_writers), memory_order_acquire) &&
                            atomic_load_explicit(&(m->active_serial), memory_order_acquire) == ctx->active_before && atomic_load_explicit(&(m->landing_order), memory_order_acquire) == ctx->landing_before &&
                            before_terminal && !atomic_load_explicit(&(end->relevant), memory_order_acquire) && !atomic_load_explicit(&(end->refresh), memory_order_acquire) &&
                            rate_render_stationary(ctx->core, ctx->window_samples);
        /* uintptr_t successful unchanged measurement already carries this point,
         * window, opening and map. Keep its shared order while parked, so
         * queued copies cannot crowd out real actions during file loading. */
    if (stationary && m->stationary_order == m->order && m->stationary_load == ctx->load.load_generation &&
        m->stationary_map == map_after && m->stationary_flags == r.motion_flags) return;
    r.motion_order = ++m->order;
    atomic_store_explicit(&(map_mutations[ctx->deck].last_order), r.motion_order, memory_order_release);
    m->known = r.points[1].state == AZN_COORD;
    m->previous = to.sample;
    m->rate = to.rate;
    m->window_samples = ctx->window_samples;
    m->window_rate = ctx->window_samples ? to.rate : 0;
    memcpy(m->identity, to.source, 12);
    m->seed = seed;
    m->captured_interference = ctx->fence;
    end_mark_motion(ctx->deck, &(r));
    m->stationary_order = 0;
    if (status_offer_motion(m, &(r)) && stationary && !(r.motion_flags & ~AZN_AT_OPENING)) {
        m->stationary_order = m->order;
        m->stationary_load = ctx->load.load_generation;
        m->stationary_map = map_after;
        m->stationary_flags = r.motion_flags;
    }
}


static int64_t status_limit(uintptr_t self) {
    const uintptr_t caller = ((uintptr_t) (__builtin_return_address(0)));
    const int64_t value = ((int64_t (*)(uintptr_t)) trampoline_address(&status_limit_original))(self);
    status_motion_write_capture_limit(self, value, caller);
    return value;
}
typedef struct AdoptionRender AdoptionRender;
struct AdoptionRender {
    Motion *motion;
};
static void adoption_render_init(AdoptionRender *ctx, void *core, int count);
static void adoption_render_drop(AdoptionRender *ctx);
static void adoption_render_init(AdoptionRender *ctx, void *core, int count) {
    (void) ctx;
    *ctx = (AdoptionRender) {.motion = NULL};

    if (count <= 0) return;
    {
        for (unsigned range_index_8 = 0; range_index_8 < sizeof(state().decks) / sizeof(state().decks)[0]; ++range_index_8) {
            __auto_type slot = &(state().decks[range_index_8]);

            if (atomic_load_explicit(&(slot->core), memory_order_acquire) != ((uintptr_t) (core))) continue;
            ctx->motion = &slot->motion;
            atomic_fetch_add_explicit(&(ctx->motion->adoption_render_writers), 1, memory_order_acq_rel);
            atomic_fetch_add_explicit(&(ctx->motion->adoption_render), 1, memory_order_acq_rel);
            break;
        }
    }
}
static void adoption_render_drop(AdoptionRender *ctx) {
    (void) ctx;

    if (!ctx->motion) return;
    atomic_fetch_add_explicit(&(ctx->motion->adoption_render), 1, memory_order_release);
    atomic_fetch_sub_explicit(&(ctx->motion->adoption_render_writers), 1, memory_order_release);
}

static void status_continuous(void *core, const void *frames) {
    RateRenderScope rate_render __attribute__((cleanup(rate_render_drop)));
    rate_render_init(&rate_render, core);
    const int count = frames ? integer(((uintptr_t) (frames)) + 8) : 0;
    /* Even before load completion (when no public trajectory can be
     * emitted), rendering breaks a chain of copied adoption coordinates.
     * Entry/exit fences also reject a concurrent or net-zero excursion. */
    AdoptionRender adoption_render __attribute__((cleanup(adoption_render_drop)));
    adoption_render_init(&adoption_render, core, count);
    StatusMotionWrite receipt __attribute__((cleanup(status_motion_write_drop)));
    status_motion_write_init(&receipt, core, false, count > 0 ? (unsigned) (count) : 0);
    ((void (*)(void *, const void *)) trampoline_address(&status_continuous_original))(core, frames);
    status_motion_write_finish(&receipt, 0);
    if (processing && frames && atomic_load_explicit(&(processing->core), memory_order_acquire) == ((uintptr_t) (core)))
        rate_capture((unsigned) (processing - state().decks), &(*processing), 0,
                     rate_render_measured_frames(&rate_render, integer(((uintptr_t) (frames)) + 8)));
}
static bool status_install_motion(State *s, int build, azexit_install_fn install) {
    static const struct {
        uintptr_t address;
        uint32_t first[4];
    } entries[2] = {
        {0x1358ea8, {0xa9b37bfd, 0xb000a302, 0x91240042, 0x910003fd}},
        {0x1414218, {0xa9b37bfd, 0xd000b782, 0x9116c042, 0x910003fd}}};
    __auto_type e = &(entries[build - 1]);
    __auto_type jump = &(s->profile.core_jump);
    const uintptr_t limit = s->bias + (build == 1 ? 0x766220u : 0x7a7aa0u);
    static const uint32_t limit_first[] = {0xf9418001, 0xb9400020, 0xb9402021, 0x9b207c20};
    if (!jump->address || memcmp(((const void *) (jump->address)), jump->prologue, sizeof jump->prologue) ||
        memcmp(((const void *) (limit)), limit_first, sizeof limit_first) ||
        memcmp(((const void *) (s->bias + e->address)), e->first, sizeof e->first)) return false;
    return install(limit, ((void *) (status_limit)), &status_limit_original) && status_limit_original &&
           install(s->bias + e->address, ((void *) (status_continuous)), &status_continuous_original) && status_continuous_original &&
           install(jump->address, ((void *) (core_jump_hook)), &s->core_jump_original) && s->core_jump_original;
}

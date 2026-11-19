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
static bool status_load_setup_callback(Slot *slot, const StatusActionScope *scope, bool adopted) {
    uintptr_t pause = 0, cue = 0;
    return adopted && transport_owners(slot, &pause, &cue) && table(scope->callback, state().profile.commands[7].table) &&
           word(scope->callback + 0x30) == cue && word(scope->callback + 0x38) == pause;
}
static bool status_load_setup_scope(Slot *slot, bool adopted) {
    if (causal.slot) return false;
    __auto_type scope = status_action_current;
    if (!scope) return true;
    if (!scope->controller || scope->command || scope->beat || scope->direct_kind) return false;
    if (!scope->callback) return !adopted;
    return status_load_setup_callback(slot, scope, adopted);
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
static bool status_landing_eligible(const StatusLanding *ctx, uintptr_t requested) {
    return ctx->slot && ctx->exclusive && requested && ctx->stamp && ctx->epoch &&
           atomic_load_explicit(&(ctx->slot->core), memory_order_acquire) == ctx->core;
}
static bool status_landing_target(const StatusLanding *ctx, uintptr_t requested, const StatusHead *to) {
    const int order = compare_timestamps(word(requested + 0x18), to->sample, to->rate, to->rate, 1);
    return to->ready && order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL &&
           !memcmp((const void *) (requested + 8), to->source, 12) && azn_lifecycle_fence(ctx->deck) == ctx->epoch;
}
static bool status_landing_consecutive(const StatusLanding *ctx, const Motion *m, uint64_t lo, uint32_t hi) {
    return ctx->stamp > 1 && atomic_load_explicit(&(m->landing_stamp), memory_order_acquire) == ctx->stamp - 1 &&
           atomic_load_explicit(&(m->landing_epoch), memory_order_acquire) == ctx->epoch &&
           atomic_load_explicit(&(m->landing_lo), memory_order_acquire) == lo && atomic_load_explicit(&(m->landing_hi), memory_order_acquire) == hi;
}
static bool status_landing_pending(const StatusLanding *ctx, const Motion *m, const StatusHead *to, bool adopted) {
    return ctx->request && azn_load_pending(ctx->deck, ctx->epoch, to->source) == ctx->request && status_load_setup_scope(ctx->slot, adopted) &&
           !atomic_load_explicit(&(m->adoption_render_writers), memory_order_acquire) &&
           atomic_load_explicit(&(m->adoption_render), memory_order_acquire) == ctx->render_cut;
}
static bool status_landing_repeat(const StatusLanding *ctx, const Motion *m, const StatusHead *to, bool adopted, bool pending) {
    return !adopted && pending && m->adoption_request == ctx->request && m->adoption_render_cut == ctx->render_cut &&
           atomic_load_explicit(&(m->opening_landing), memory_order_acquire) == ctx->stamp - 1 && ctx->from.ready &&
           ctx->from.rate == to->rate && atomic_load_explicit(&(m->adoption_rate), memory_order_acquire) == to->rate;
}
static bool status_landing_same_adoption(const StatusLanding *ctx, const Motion *m, const StatusHead *to) {
    const int before = compare_timestamps(ctx->from.sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), to->rate, to->rate, 1);
    const int after = compare_timestamps(to->sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), to->rate, to->rate, 1);
    return before != AZ_TIME_INVALID && after != AZ_TIME_INVALID && before == AZ_TIME_EQUAL && after == AZ_TIME_EQUAL;
}
static void status_landing_opening(const StatusLanding *ctx, Motion *m, const StatusHead *to, bool adopted, bool pending) {
    if (adopted && (azn_interest(ctx->deck) & motion_routes)) {
        atomic_store_explicit(&(m->opening_landing), ctx->stamp, memory_order_release);
        atomic_store_explicit(&(m->adoption_sample), to->sample, memory_order_release);
        atomic_store_explicit(&(m->adoption_rate), to->rate, memory_order_release);
        m->adoption_render_cut = ctx->render_cut;
        m->adoption_request = pending ? ctx->request : 0;
    } else if (status_landing_repeat(ctx, m, to, adopted, pending)) {
        /* Preserve only consecutive exact pre-completion copies, with no
         * intervening render or action. Later seeks cannot extend this cut. */
        if (status_landing_same_adoption(ctx, m, to))
            atomic_store_explicit(&(m->opening_landing), ctx->stamp, memory_order_release);
    }
}
static void status_landing_finish(StatusLanding *ctx, uintptr_t requested) {
    if (!status_landing_eligible(ctx, requested)) return;
    StatusHead to = status_head(ctx->core);
    if (!status_landing_target(ctx, requested, &to)) return;
    Motion *m = &ctx->slot->motion;
    if (atomic_flag_test_and_set_explicit(&(m->landing_publish), memory_order_acquire)) return;
    EXIT_FLAG_GUARD(unlock, &m->landing_publish);
    if (atomic_load_explicit(&(m->landing_order), memory_order_acquire) != ctx->stamp || atomic_load_explicit(&(m->landing_writers), memory_order_acquire) != 1) return;
    uint64_t lo;
    uint32_t hi;
    memcpy(&lo, to.source, 8);
    memcpy(&hi, to.source + 8, 4);
    if (!memcmp(ctx->from.source, to.source, 12) && !status_landing_consecutive(ctx, m, lo, hi)) return;
    atomic_store_explicit(&(m->landing_lo), lo, memory_order_release);
    atomic_store_explicit(&(m->landing_hi), hi, memory_order_release);
    const bool adopted = memcmp(ctx->from.source, to.source, 12) != 0;
    const bool pending = status_landing_pending(ctx, m, &to, adopted);
    status_landing_opening(ctx, m, &to, adopted, pending);
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
struct StationarySeed {
    azsdk_load_sample load;
    StatusHead head;
    uint64_t epoch, order;
    bool matched, landed;
};
static bool status_seed_load(unsigned deck, const azsdk_load_sample *snapshot, struct StationarySeed *seed) {
    if (snapshot) seed->load = *snapshot;
    return (snapshot || azsdk_load_read((int) deck, &seed->load)) && seed->load.invalidation_epoch == seed->epoch;
}
static bool status_seed_established(const Motion *m, const struct StationarySeed *seed) {
    return seed->matched && m->load == seed->load.load_generation && m->order;
}
static bool status_seed_current(unsigned deck, const Motion *m, const struct StationarySeed *seed) {
    return azn_lifecycle_fence(deck) == seed->epoch && atomic_load_explicit(&(m->landing_order), memory_order_acquire) == seed->order &&
           !atomic_load_explicit(&(m->landing_writers), memory_order_acquire);
}
static void status_seed_first_frame(Motion *m, const struct StationarySeed *seed) {
    /* Two real owner frames under one fence can prove source adoption, without
     * reconstructing any path or occurrence between the frames. */
    if (m->frame_epoch == seed->epoch) return;
    m->frame_epoch = seed->epoch;
    m->frame_landing_order = seed->order;
    memcpy(m->first_source, seed->head.source, 12);
}
static bool status_seed_landed(const Motion *m, const struct StationarySeed *seed) {
    uint64_t lo;
    uint32_t hi;
    memcpy(&lo, seed->head.source, 8);
    memcpy(&hi, seed->head.source + 8, 4);
    return seed->order && atomic_load_explicit(&(m->landing_stamp), memory_order_acquire) == seed->order &&
           atomic_load_explicit(&(m->landing_epoch), memory_order_acquire) == seed->epoch &&
           atomic_load_explicit(&(m->landing_lo), memory_order_acquire) == lo && atomic_load_explicit(&(m->landing_hi), memory_order_acquire) == hi;
}
static bool status_seed_qualified(unsigned deck, const Motion *m, const struct StationarySeed *seed) {
    return (seed->landed || memcmp(m->first_source, seed->head.source, 12)) &&
           azn_load_track_matches(deck, seed->epoch, seed->load.load_generation, seed->head.source) &&
           atomic_load_explicit(&(m->landing_order), memory_order_acquire) == seed->order && !atomic_load_explicit(&(m->foreign), memory_order_acquire);
}
static bool status_seed_capture(unsigned deck, Slot *slot, const azsdk_load_sample *snapshot, struct StationarySeed *seed) {
    Motion *m = &slot->motion;
    seed->epoch = azn_lifecycle_fence(deck);
    if (!seed->epoch || atomic_load_explicit(&(m->foreign), memory_order_acquire) || atomic_load_explicit(&(m->landing_writers), memory_order_acquire)) return false;
    seed->order = atomic_load_explicit(&(m->landing_order), memory_order_acquire);
    seed->matched = status_seed_load(deck, snapshot, seed);
    if (status_seed_established(m, seed)) return false;
    seed->head = status_head(atomic_load_explicit(&(slot->core), memory_order_acquire));
    if (!status_seed_current(deck, m, seed)) return false;
    status_seed_first_frame(m, seed);
    if (!seed->matched || !seed->head.ready) return false;
    seed->landed = status_seed_landed(m, seed);
    return status_seed_qualified(deck, m, seed);
}
static bool status_seed_initial(const Motion *m, const struct StationarySeed *seed) {
    /* A cued zero is the opening even after repeated native copies. Nonzero
     * openings need the adoption or proven pre-completion copies. */
    const StatusHead *head = &seed->head;
    const int opening = compare_timestamps(head->sample, 0, head->rate, head->rate, 1);
    const int adopted = compare_timestamps(head->sample, atomic_load_explicit(&(m->adoption_sample), memory_order_acquire), head->rate, head->rate, 1);
    bool at_adoption = atomic_load_explicit(&(m->adoption_rate), memory_order_acquire) == head->rate && adopted != AZ_TIME_INVALID && adopted == AZ_TIME_EQUAL;
    return opening != AZ_TIME_INVALID && (opening == AZ_TIME_EQUAL ||
                                          (seed->landed ? at_adoption && atomic_load_explicit(&(m->opening_landing), memory_order_acquire) == seed->order : seed->order == m->frame_landing_order));
}
static void status_seed_opening(Slot *slot, const struct StationarySeed *seed, azstatus_native_receipt *receipt) {
    Motion *m = &slot->motion;
    uintptr_t pause = 0, cue = 0;
    bool initial = status_seed_initial(m, seed);
    bool opening = initial && transport_owners(slot, &pause, &cue) && !byte(pause + 8);
    m->opening_sample = seed->head.sample;
    m->opening_rate = seed->head.rate;
    atomic_store_explicit(&(m->opening), opening, memory_order_release);
    if (opening) receipt->motion_flags = AZN_AT_OPENING;
}
static void status_seed_receipt(unsigned deck, const struct StationarySeed *seed, azstatus_native_receipt *r) {
    r->kind = AZN_MOTION;
    r->a = AZN_MOTION_SEED;
    r->deck = deck;
    r->unit = seed->load.unit_generation;
    r->epoch = seed->load.invalidation_epoch;
    r->load_generation = seed->load.load_generation;
    r->motion_order = 1;
    r->motion_generation = 1;
    r->points[0].state = AZN_INVALID;
    r->points[1] = (azn_point) {AZN_COORD, seed->head.rate, (int64_t) seed->head.sample};
    r->observed_window_samples = 1;
    r->observed_window_rate = seed->head.rate;
}
static void status_seed_commit(unsigned deck, Motion *m, const struct StationarySeed *seed, azstatus_native_receipt *receipt) {
    if (read_map_clock(deck, seed->load.load_generation, &receipt->map) == UINT64_MAX || atomic_load_explicit(&(map_mutations[deck].active), memory_order_acquire))
        receipt->lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    m->load = seed->load.load_generation;
    m->order = 1;
    m->generation = 1;
    m->known = true;
    atomic_store_explicit(&(map_mutations[deck].last_order), 1, memory_order_release);
    m->previous = seed->head.sample;
    m->rate = seed->head.rate;
    m->window_samples = 1;
    m->window_rate = seed->head.rate;
    memcpy(m->identity, seed->head.source, 12);
    m->seed = azn_seed_id(deck);
    m->captured_interference = atomic_load_explicit(&(m->interference), memory_order_acquire);
    status_offer_motion(m, receipt);
}
static void status_stationary_seed_locked(unsigned deck, Slot *slot, const azsdk_load_sample *snapshot) {
    if (!(azn_interest(deck) & motion_routes)) return;
    struct StationarySeed seed = {0};
    if (!status_seed_capture(deck, slot, snapshot, &seed)) return;
    azstatus_native_receipt receipt = {0};
    status_seed_receipt(deck, &seed, &receipt);
    status_seed_opening(slot, &seed, &receipt);
    status_seed_commit(deck, &slot->motion, &seed, &receipt);
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
static bool status_opening_claim(Motion *m, const azsdk_load_sample load) {
    return atomic_exchange_explicit(&(m->opening), false, memory_order_acq_rel) && m->load == load.load_generation && m->known &&
           m->rate && m->window_samples && m->window_rate && m->generation && m->order != UINT64_MAX;
}
static bool status_opening_head(const Motion *m, const StatusHead *head) {
    const int order = compare_timestamps(head->sample, m->previous, head->rate, m->window_rate, m->window_samples);
    return head->ready && order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL && head->rate == m->rate && !memcmp(head->source, m->identity, 12);
}
static bool status_opening_source(unsigned d, const Motion *m, const azsdk_load_sample load, const StatusHead *head) {
    azsdk_load_sample after = {0};
    return azsdk_load_read((int) d, &after) && after.unit_generation == load.unit_generation && after.invalidation_epoch == load.invalidation_epoch &&
           after.load_generation == load.load_generation && !atomic_load_explicit(&(m->foreign), memory_order_acquire) &&
           m->captured_interference == atomic_load_explicit(&(m->interference), memory_order_acquire) &&
           azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, head->source);
}
static void status_opening_play(unsigned d, Slot *slot, const azsdk_load_sample load) {
    __auto_type m = &(slot->motion);
    if (!atomic_load_explicit(&(m->opening), memory_order_acquire)) return;
    unsigned reservation = MOTION_FREE;
    if (!atomic_compare_exchange_strong_explicit(&(m->busy), &(reservation), MOTION_OBSERVE, memory_order_acquire, memory_order_acquire)) {
        if (atomic_exchange_explicit(&(m->opening), false, memory_order_acq_rel)) azn_motion_gap(d, 0);
        return;
    }
    EXIT_ATOMIC_GUARD(unlock, &m->busy);
    if (!status_opening_claim(m, load)) return;
    azstatus_native_receipt r = {0};
    __auto_type head = status_head(atomic_load_explicit(&(slot->core), memory_order_acquire));
    if (!status_opening_head(m, &head) || !status_opening_source(d, m, load, &head)) return;
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
static bool status_stationary_engine_ready(const StatusMotionWrite *ctx, const State *s) {
    return byte(ctx->core + 9) && ctx->stationary_engine && ctx->window_samples && ctx->window_samples <= 1024 && s->build >= 1 && s->build <= 2;
}
static bool status_stationary_samples(const StatusMotionWrite *ctx, const State *s, const RateProfile *profile) {
    const bool external = word(ctx->stationary_engine) == s->bias + profile->scratch && byte(ctx->stationary_engine + 0x58);
    const uintptr_t samples = word(ctx->stationary_engine + (external ? 0x60u : 0x18u));
    if (!pointer(samples) || ctx->window_samples > word(ctx->stationary_engine + 0x20)) return false;
    /* These are dimensionless signed source advances from this completed
     * render. A net-zero forward/reverse block is not stationary. */
    for (unsigned n = 0; n < ctx->window_samples; ++n)
        if (rate_double(samples + sizeof(double) * n) != 0) return false;
    return true;
}
static bool status_motion_write_stationary_block(StatusMotionWrite *ctx) {
    State *s = &state();
    if (!ctx->rendering) return !byte(ctx->core + 9);
    if (!status_stationary_engine_ready(ctx, s)) return false;
    if (rate_render_stationary(ctx->core, ctx->window_samples)) return true;
    const RateProfile *profile = &rate_profiles[s->build - 1];
    if (rate_engine(ctx->core, s, *profile) != ctx->stationary_engine) return false;
    return status_stationary_samples(ctx, s, profile);
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
static bool status_limit_caller(const State *s, uintptr_t caller) {
    return s->build >= 1 && s->build <= 2 && caller == s->bias + (s->build == 1 ? 0x14ac994u : 0x1567dc4u);
}
static void status_motion_write_capture_limit(uintptr_t owner, int64_t limit, uintptr_t caller) {

    __auto_type writer = status_motion_write_current_setter;
    __auto_type s = &(state());
    if (!writer || !writer->locked || writer->slot != processing || !status_limit_caller(s, caller) ||
        atomic_load_explicit(&(writer->slot->core), memory_order_acquire) != writer->core || word(writer->core + 0xb8) != owner) return;
    ++writer->limit_calls;
    writer->limit_owner = owner;
    writer->observed_limit = limit;
}
static void status_motion_find_slot(StatusMotionWrite *ctx, State *s) {
    for (ctx->deck = 0; ctx->deck < 4; ++ctx->deck)
        if (atomic_load_explicit(&(s->decks[ctx->deck].core), memory_order_acquire) == ctx->core) {
            ctx->slot = &s->decks[ctx->deck];
            break;
        }
}
static bool status_motion_map_window(const StatusMotionWrite *ctx, unsigned reservation) {
    return ctx->slot == processing && reservation == MOTION_MAP && !ctx->reposition && ctx->window_samples &&
           (atomic_load_explicit(&(rate_hooks), memory_order_acquire) & 3) == 3;
}
static bool status_motion_map_idle(StatusMotionWrite *ctx) {
    const Motion *m = &ctx->slot->motion;
    return !atomic_load_explicit(&(m->active_writers), memory_order_acquire) && !atomic_load_explicit(&(m->landing_writers), memory_order_acquire) &&
           !atomic_load_explicit(&(m->foreign), memory_order_acquire) && azsdk_load_read((int) ctx->deck, &ctx->load);
}
static bool status_motion_defer_map(StatusMotionWrite *ctx, State *s, unsigned reservation) {
    /* Defer an overlapping map verdict until actual completed advances and
     * source/writer fences can prove that the block was stationary. */
    if (!status_motion_map_window(ctx, reservation) || !status_motion_map_idle(ctx)) return false;
    Motion *m = &ctx->slot->motion;
    ctx->active_before = atomic_load_explicit(&(m->active_serial), memory_order_acquire);
    ctx->landing_before = atomic_load_explicit(&(m->landing_order), memory_order_acquire);
    ctx->fence = atomic_load_explicit(&(m->interference), memory_order_acquire);
    ctx->from = status_head(ctx->core);
    ctx->rendering = byte(ctx->core + 9);
    if (ctx->rendering && s->build >= 1 && s->build <= 2) ctx->stationary_engine = rate_engine(ctx->core, s, rate_profiles[s->build - 1]);
    ctx->paused_map = true;
    return true;
}
static void status_motion_competing(StatusMotionWrite *ctx) {
    ctx->competing = true;
    atomic_store_explicit(&(ctx->slot->motion.opening), false, memory_order_release);
    atomic_fetch_add_explicit(&(ctx->slot->motion.foreign), 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
    azsdk_load_sample active = {0};
    if (azsdk_load_read((int) ctx->deck, &active)) azn_motion_gap(ctx->deck, ctx->reposition);
}
static bool status_motion_seed_needed(const StatusMotionWrite *ctx) {
    return !ctx->reposition && (!ctx->slot->motion.order || ctx->slot->motion.load != ctx->load.load_generation);
}
static void status_motion_capture_render(StatusMotionWrite *ctx, const State *s) {
    ctx->active_before = atomic_load_explicit(&(ctx->slot->motion.active_serial), memory_order_acquire);
    ctx->landing_before = atomic_load_explicit(&(ctx->slot->motion.landing_order), memory_order_acquire);
    if (ctx->rendering && (atomic_load_explicit(&(rate_hooks), memory_order_acquire) & 3) == 3 && s->build >= 1 && s->build <= 2)
        ctx->stationary_engine = rate_engine(ctx->core, s, rate_profiles[s->build - 1]);
}
static void status_motion_capture_owned(StatusMotionWrite *ctx, const State *s) {
    ctx->fence = atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire);
    if (atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) || !azsdk_load_read((int) ctx->deck, &ctx->load)) return;
    if (status_motion_seed_needed(ctx)) status_stationary_seed_locked(ctx->deck, ctx->slot, &ctx->load);
    ctx->mutation_before = atomic_load_explicit(&(map_mutations[ctx->deck].generation), memory_order_acquire);
    aznm_packet stamp = {0};
    ctx->map_before = read_map_clock(ctx->deck, ctx->load.load_generation, &stamp);
    ctx->from = status_head(ctx->core);
    ctx->rendering = !ctx->reposition && ctx->window_samples && byte(ctx->core + 9);
    if (!ctx->reposition) status_motion_capture_render(ctx, s);
    if (ctx->reposition) ctx->fixed_max_owner = status_motion_write_fixed_max_provider(ctx);
}
static void status_motion_write_init(StatusMotionWrite *ctx, void *self, bool explicit_position, unsigned samples) {
    *ctx = (StatusMotionWrite) {.previous_setter = NULL, .slot = NULL, .deck = 0, .rendering = false, .fence = 0, .map_before = 0, .mutation_before = 0, .active_before = 0, .landing_before = 0, .fixed_max_owner = 0, .stationary_engine = 0, .limit_owner = 0, .observed_limit = 0, .limit_calls = 0, .setter_scope = false, .locked = false, .competing = false, .paused_map = false, .core = ((uintptr_t) (self)), .window_samples = samples, .reposition = explicit_position};
    if (ctx->reposition) {
        /* An unowned nested setter must shadow the outer scope too: decks
         * can share a buffering scheduler. */
        ctx->previous_setter = status_motion_write_current_setter;
        status_motion_write_current_setter = ctx;
        ctx->setter_scope = true;
    }
    State *s = &state();
    if (!(atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS) || !s->motion_hooked) return;
    status_motion_find_slot(ctx, s);
    if (!ctx->slot || !(azn_interest(ctx->deck) & (motion_routes | AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)))) return;
    /* Competing/unowned writes never dereference saved pointers or stamp an
     * old coordinate with a newer fence. */
    unsigned reservation = MOTION_FREE;
    if (ctx->slot != processing || !atomic_compare_exchange_strong_explicit(&(ctx->slot->motion.busy), &reservation, MOTION_HEAD, memory_order_acquire, memory_order_acquire)) {
        if (status_motion_defer_map(ctx, s, reservation)) return;
        status_motion_competing(ctx);
        return;
    }
    ctx->locked = true;
    status_motion_capture_owned(ctx, s);
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
static bool status_paused_map_point(const StatusMotionWrite *ctx, const StatusHead *to, int stationary) {
    return ctx->from.ready && to->ready && stationary != AZ_TIME_INVALID && stationary == AZ_TIME_EQUAL &&
           ctx->from.rate == to->rate && !memcmp(ctx->from.source, to->source, 12);
}
static bool status_paused_map_owner(StatusMotionWrite *ctx) {
    const Motion *m = &ctx->slot->motion;
    return ctx->slot == processing && atomic_load_explicit(&(ctx->slot->core), memory_order_acquire) == ctx->core && status_motion_write_stationary_block(ctx) &&
           !atomic_load_explicit(&(m->active_writers), memory_order_acquire) && atomic_load_explicit(&(m->active_serial), memory_order_acquire) == ctx->active_before &&
           !atomic_load_explicit(&(m->landing_writers), memory_order_acquire) && atomic_load_explicit(&(m->landing_order), memory_order_acquire) == ctx->landing_before;
}
static bool status_paused_map_load(const StatusMotionWrite *ctx, const StatusHead *to) {
    const Motion *m = &ctx->slot->motion;
    azsdk_load_sample after = {0};
    return !atomic_load_explicit(&(m->foreign), memory_order_acquire) && atomic_load_explicit(&(m->interference), memory_order_acquire) == ctx->fence &&
           azsdk_load_read((int) ctx->deck, &after) && after.unit_generation == ctx->load.unit_generation &&
           after.invalidation_epoch == ctx->load.invalidation_epoch && after.load_generation == ctx->load.load_generation &&
           azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, to->source);
}
static void status_finish_paused_map(StatusMotionWrite *ctx) {
    const StatusHead to = status_head(ctx->core);
    const int stationary = compare_timestamps(ctx->from.sample, to.sample, ctx->from.rate, ctx->from.rate, 1);
    const bool inactive = status_paused_map_point(ctx, &to, stationary) && status_paused_map_owner(ctx) && status_paused_map_load(ctx, &to);
    if (!inactive) {
        atomic_store_explicit(&(ctx->slot->motion.opening), false, memory_order_release);
        atomic_fetch_add_explicit(&(ctx->slot->motion.interference), 1, memory_order_acq_rel);
        azn_motion_gap(ctx->deck, 0);
    }
    ctx->paused_map = false;
}
struct MotionFinish {
    StatusHead to;
    azstatus_native_receipt receipt;
    bool same_load, same_source, moved, previous, target, gap;
    bool map_ambiguous, stationary_opening, stationary;
    uint64_t seed, map_after;
    int opening_motion;
};
static bool status_motion_completed_load(const StatusMotionWrite *ctx) {
    azsdk_load_sample after = {0};
    return azsdk_load_read((int) ctx->deck, &after) && ctx->load.unit_generation == after.unit_generation &&
           ctx->load.load_generation == after.load_generation && ctx->load.invalidation_epoch == after.invalidation_epoch &&
           !atomic_load_explicit(&(ctx->slot->motion.foreign), memory_order_acquire) &&
           atomic_load_explicit(&(ctx->slot->motion.interference), memory_order_acquire) == ctx->fence;
}
static void status_motion_qualify_heads(StatusMotionWrite *ctx, struct MotionFinish *value) {
    ctx->from.ready = ctx->from.ready && azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, ctx->from.source);
    value->to.ready = value->to.ready && azn_load_track_matches(ctx->deck, ctx->load.invalidation_epoch, ctx->load.load_generation, value->to.source);
    value->same_source = ctx->from.ready && value->to.ready && ctx->from.rate == value->to.rate && !memcmp(ctx->from.source, value->to.source, 12);
}
static bool status_motion_previous(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value, int prior_order) {
    return value->same_load && m->known && ctx->from.ready && m->rate == ctx->from.rate && !memcmp(m->identity, ctx->from.source, 12) &&
           m->window_samples && m->window_rate && prior_order != AZ_TIME_INVALID && prior_order == AZ_TIME_EQUAL;
}
static bool status_motion_target(uintptr_t requested, const StatusHead *to) {
    if (!requested || !to->ready) return false;
    const int order = compare_timestamps(word(requested + 0x18), to->sample, to->rate, to->rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL && !memcmp((const void *) (requested + 8), to->source, 12);
}
static void status_motion_measure(StatusMotionWrite *ctx, uintptr_t requested, struct MotionFinish *value) {
    status_motion_qualify_heads(ctx, value);
    Motion *m = &ctx->slot->motion;
    value->same_load = m->load == ctx->load.load_generation;
    const int movement = compare_timestamps(ctx->from.sample, value->to.sample, ctx->from.rate, ctx->from.rate, ctx->window_samples);
    const int prior_order = compare_timestamps(m->previous, ctx->from.sample, m->rate, m->window_rate, m->window_samples);
    value->moved = value->same_source && movement != AZ_TIME_INVALID && movement != AZ_TIME_EQUAL;
    value->previous = status_motion_previous(ctx, m, value, prior_order);
    value->target = status_motion_target(requested, &value->to);
    value->seed = azn_seed_id(ctx->deck);
}
static bool status_motion_retains_opening(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value, int order) {
    return value->same_load && value->same_source && m->opening_rate == value->to.rate && order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL &&
           m->captured_interference == ctx->fence && (!ctx->reposition || (value->previous && value->target));
}
static void status_motion_prepare_history(const StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value) {
    /* Rendering at zero speed retains the opening until cumulative source
     * travel leaves its observed window. */
    const int opening = compare_timestamps(m->opening_sample, value->to.sample, value->to.rate, value->to.rate, ctx->window_samples);
    if (!status_motion_retains_opening(ctx, m, value, opening)) atomic_store_explicit(&(m->opening), false, memory_order_release);
    if (!value->same_load) {
        m->load = ctx->load.load_generation;
        m->generation = 1;
        m->order = 0;
        m->known = false;
    }
    value->gap = m->order && ((m->known && !value->previous) || m->captured_interference != ctx->fence);
}
static bool status_motion_idle(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value) {
    return !ctx->reposition && !ctx->rendering && !value->moved && !value->gap && !(value->same_load && m->known && value->seed != m->seed);
}
static bool status_motion_missing_seed(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value) {
    /* A stationary copy left by the prior load is not a first seed. */
    return !m->order && !(ctx->rendering || value->moved || value->target);
}
static bool status_motion_can_publish(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value) {
    if (status_motion_idle(ctx, m, value) || status_motion_missing_seed(ctx, m, value)) return false;
    if (m->order == UINT64_MAX || m->generation == UINT64_MAX) {
        azn_motion_gap(ctx->deck, ctx->reposition);
        return false;
    }
    return true;
}
static void status_motion_receipt(const StatusMotionWrite *ctx, const Motion *m, struct MotionFinish *value) {
    azstatus_native_receipt *r = &value->receipt;
    r->kind = AZN_MOTION;
    r->deck = ctx->deck;
    r->epoch = ctx->load.invalidation_epoch;
    r->load_generation = ctx->load.load_generation;
    r->unit = ctx->load.unit_generation;
    r->observed_window_samples = ctx->window_samples;
    r->observed_window_rate = ctx->window_samples ? value->to.rate : 0;
    if (value->previous) {
        r->from_window_samples = m->window_samples;
        r->from_window_rate = m->window_rate;
    }
    r->a = ctx->reposition || value->gap ? AZN_REPOSITION : value->previous ? AZN_MEASURE
                                                                            : AZN_MOTION_SEED;
}
static bool status_motion_fixed_store(StatusMotionWrite *ctx, int range) {
    return ctx->fixed_max_owner && ctx->fixed_max_owner == status_motion_write_fixed_max_provider(ctx) && range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL;
}
static bool status_motion_observed_store(const StatusMotionWrite *ctx, int range) {
    return ctx->limit_calls == 1 && ctx->limit_owner && word(ctx->core + 0xb8) == ctx->limit_owner && range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL;
}
static bool status_motion_store_witness(StatusMotionWrite *ctx, const struct MotionFinish *value, int fixed, int actual) {
    /* Only the setter's actual getter proves its range guard at an equal
     * coordinate; equality alone cannot distinguish store and rejection. */
    return value->moved || !value->same_source || status_motion_fixed_store(ctx, fixed) || status_motion_observed_store(ctx, actual);
}
static void status_motion_apply_action(const StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value, unsigned action, bool stored) {
    azstatus_native_receipt *r = &value->receipt;
    if (action && stored && m->occurrence < UINT64_MAX) {
        r->a = action;
        r->occurrence_ordinal = ++m->occurrence;
    } else if (action) {
        r->lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | motion_routes;
    } else if (value->previous && value->moved && value->same_source && status_jump_origin(ctx->slot)) {
        /* Preserve the controller's measured landing but report its action
         * history as unknown. */
        r->history_only = 1;
        r->lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    }
}
static void status_motion_classify(StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value) {
    if (!ctx->reposition || !value->target || !state().motion_actions) return;
    unsigned action = status_action(ctx->slot, ctx->from, value->to);
    const int fixed = compare_timestamps(value->to.sample, UINT64_C(0x5ea0da80), value->to.rate, value->to.rate, 1);
    const int actual = compare_timestamps(value->to.sample, ctx->observed_limit, value->to.rate, value->to.rate, 1);
    bool stored = status_motion_store_witness(ctx, value, fixed, actual);
    status_motion_apply_action(ctx, m, value, action, stored);
}
static bool status_motion_map_ambiguous(const StatusMotionWrite *ctx, uint64_t after) {
    return (ctx->map_before || after) && (ctx->map_before == UINT64_MAX || after == UINT64_MAX || ctx->map_before != after ||
                                          ctx->mutation_before != atomic_load_explicit(&(map_mutations[ctx->deck].generation), memory_order_acquire) ||
                                          atomic_load_explicit(&(map_mutations[ctx->deck].active), memory_order_acquire));
}
static void status_motion_map(const StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value) {
    if ((ctx->reposition || value->gap) && m->order) ++m->generation;
    value->receipt.motion_generation = m->generation;
    value->map_after = read_map_clock(ctx->deck, ctx->load.load_generation, &value->receipt.map);
    value->map_ambiguous = status_motion_map_ambiguous(ctx, value->map_after);
}
static void status_motion_recovery(const StatusMotionWrite *ctx, const Motion *m, struct MotionFinish *value) {
    if (value->gap || (value->previous && ctx->reposition && !value->target)) value->receipt.lost_topics |= motion_routes;
    if (value->seed != m->seed && m->order && value->previous) value->receipt.c = 1;
}
static bool status_motion_writers_current(const StatusMotionWrite *ctx, const Motion *m) {
    return !atomic_load_explicit(&(m->active_writers), memory_order_acquire) && !atomic_load_explicit(&(m->landing_writers), memory_order_acquire) &&
           atomic_load_explicit(&(m->active_serial), memory_order_acquire) == ctx->active_before && atomic_load_explicit(&(m->landing_order), memory_order_acquire) == ctx->landing_before;
}
static bool status_motion_stationary_opening(StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value) {
    /* A map first published during HEAD can reannounce an intact opening on
     * a proven stationary block; real source travel retains mapping loss. */
    return !ctx->reposition && value->previous && atomic_load_explicit(&(m->opening), memory_order_acquire) &&
           value->opening_motion != AZ_TIME_INVALID && value->opening_motion == AZ_TIME_EQUAL && status_motion_write_stationary_block(ctx) &&
           status_motion_writers_current(ctx, m);
}
static void status_motion_opening_flags(const StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value) {
    azstatus_native_receipt *r = &value->receipt;
    if (value->map_ambiguous && !value->stationary_opening) r->lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    if (r->occurrence_ordinal || (r->lost_topics & ~AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)))
        atomic_store_explicit(&(m->opening), false, memory_order_release);
    else if ((ctx->reposition || r->c || value->stationary_opening) && atomic_load_explicit(&(m->opening), memory_order_acquire)) r->motion_flags |= AZN_AT_OPENING;
}
static void status_motion_points(const StatusMotionWrite *ctx, struct MotionFinish *value) {
    azstatus_native_receipt *r = &value->receipt;
    if (value->previous) r->points[0] = (azn_point) {AZN_COORD, ctx->from.rate, (int64_t) ctx->from.sample};
    else r->points[0].state = AZN_INVALID;
    if (value->to.ready && (!ctx->reposition || value->target)) r->points[1] = (azn_point) {AZN_COORD, value->to.rate, (int64_t) value->to.sample};
    else r->points[1].state = AZN_INVALID;
}
static bool status_motion_before_terminal(const StatusMotionWrite *ctx, const EndState *end, const StatusHead *to, int terminal) {
    return end->load != ctx->load.load_generation || end->epoch != ctx->load.invalidation_epoch || !end->length ||
           (end->length_rate == to->rate && terminal != AZ_TIME_INVALID && terminal == AZ_TIME_BEFORE);
}
static bool status_motion_plain_measure(const StatusMotionWrite *ctx, const struct MotionFinish *value) {
    const azstatus_native_receipt *r = &value->receipt;
    return !ctx->reposition && !value->gap && value->previous && value->same_source && !r->c && !r->lost_topics;
}
static bool status_motion_complete_history(const struct MotionFinish *value) {
    return !value->receipt.occurrence_ordinal && !value->receipt.history_only && !value->map_ambiguous && value->map_after != UINT64_MAX;
}
static bool status_motion_stationary_window(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value, int exact_previous) {
    return exact_previous != AZ_TIME_INVALID && exact_previous == AZ_TIME_EQUAL && value->opening_motion != AZ_TIME_INVALID && value->opening_motion == AZ_TIME_EQUAL &&
           ctx->window_samples == m->window_samples && value->to.rate == m->window_rate;
}
static bool status_motion_is_stationary(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value, int exact_previous, bool before_terminal) {
    const EndState *end = &end_state[ctx->deck];
    return status_motion_plain_measure(ctx, value) && status_motion_complete_history(value) && status_motion_stationary_window(ctx, m, value, exact_previous) &&
           status_motion_writers_current(ctx, m) && before_terminal && !atomic_load_explicit(&(end->relevant), memory_order_acquire) &&
           !atomic_load_explicit(&(end->refresh), memory_order_acquire) && rate_render_stationary(ctx->core, ctx->window_samples);
}
static void status_motion_check_stationary(const StatusMotionWrite *ctx, const Motion *m, struct MotionFinish *value) {
    const int exact_previous = compare_timestamps(m->previous, ctx->from.sample, ctx->from.rate, ctx->from.rate, 1);
    const EndState *end = &end_state[ctx->deck];
    const int terminal = compare_timestamps(value->to.sample, end->length, value->to.rate, value->to.rate, ctx->window_samples);
    const bool before_terminal = status_motion_before_terminal(ctx, end, &value->to, terminal);
    value->stationary = status_motion_is_stationary(ctx, m, value, exact_previous, before_terminal);
}
static bool status_motion_repeat(const StatusMotionWrite *ctx, const Motion *m, const struct MotionFinish *value) {
    /* Keep the successful unchanged measurement's shared order while parked
     * so queued copies cannot crowd out actual actions during file loading. */
    return value->stationary && m->stationary_order == m->order && m->stationary_load == ctx->load.load_generation &&
           m->stationary_map == value->map_after && m->stationary_flags == value->receipt.motion_flags;
}
static void status_motion_commit(const StatusMotionWrite *ctx, Motion *m, struct MotionFinish *value) {
    azstatus_native_receipt *r = &value->receipt;
    r->motion_order = ++m->order;
    atomic_store_explicit(&(map_mutations[ctx->deck].last_order), r->motion_order, memory_order_release);
    m->known = r->points[1].state == AZN_COORD;
    m->previous = value->to.sample;
    m->rate = value->to.rate;
    m->window_samples = ctx->window_samples;
    m->window_rate = ctx->window_samples ? value->to.rate : 0;
    memcpy(m->identity, value->to.source, 12);
    m->seed = value->seed;
    m->captured_interference = ctx->fence;
    end_mark_motion(ctx->deck, r);
    m->stationary_order = 0;
    if (status_offer_motion(m, r) && value->stationary && !(r->motion_flags & ~AZN_AT_OPENING)) {
        m->stationary_order = m->order;
        m->stationary_load = ctx->load.load_generation;
        m->stationary_map = value->map_after;
        m->stationary_flags = r->motion_flags;
    }
}
static void status_motion_write_finish(StatusMotionWrite *ctx, uintptr_t requested) {
    if (ctx->paused_map) {
        status_finish_paused_map(ctx);
        return;
    }
    if (!ctx->locked || !ctx->load.load_generation) return;
    struct MotionFinish value = {0};
    value.to = status_head(ctx->core);
    if (!status_motion_completed_load(ctx)) return;
    status_motion_measure(ctx, requested, &value);
    Motion *m = &ctx->slot->motion;
    status_motion_prepare_history(ctx, m, &value);
    if (!status_motion_can_publish(ctx, m, &value)) return;
    status_motion_receipt(ctx, m, &value);
    status_motion_classify(ctx, m, &value);
    status_motion_map(ctx, m, &value);
    status_motion_recovery(ctx, m, &value);
    value.opening_motion = compare_timestamps(ctx->from.sample, value.to.sample, value.to.rate, value.to.rate, 1);
    value.stationary_opening = status_motion_stationary_opening(ctx, m, &value);
    status_motion_opening_flags(ctx, m, &value);
    status_motion_points(ctx, &value);
    status_motion_check_stationary(ctx, m, &value);
    if (!status_motion_repeat(ctx, m, &value)) status_motion_commit(ctx, m, &value);
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
static bool status_motion_entries(const State *s, uintptr_t continuous, const uint32_t first[4], uintptr_t limit) {
    __auto_type jump = &s->profile.core_jump;
    static const uint32_t limit_first[] = {0xf9418001, 0xb9400020, 0xb9402021, 0x9b207c20};
    return status_entry_matches(jump->address, jump->prologue, sizeof jump->prologue, s->core_jump_original) &&
           status_entry_matches(limit, limit_first, sizeof limit_first, status_limit_original) &&
           status_entry_matches(continuous, first, 4 * sizeof *first, status_continuous_original);
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
    if (!status_motion_entries(s, s->bias + e->address, e->first, limit)) return false;
    return status_install_entry(limit, ((void *) (status_limit)), &status_limit_original, install) &&
           status_install_entry(s->bias + e->address, ((void *) (status_continuous)), &status_continuous_original, install) &&
           status_install_entry(jump->address, ((void *) (core_jump_hook)), &s->core_jump_original, install);
}

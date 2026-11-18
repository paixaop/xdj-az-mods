/* SPDX-License-Identifier: GPL-3.0-only */
/* The map publisher and both actual head writers share a commit number. The
 * atomic stamp has no pointers and survives retirement of the GUI object. */
struct MapClock {
    _Atomic(uint64_t) serial;
    _Atomic(uint64_t) load;
    _Atomic(uint64_t) owner;
    _Atomic(uint64_t) grid;
    _Atomic(uint64_t) grid_rev;
    _Atomic(uint64_t) phrase;
    _Atomic(uint64_t) phrase_rev;
};
static MapClock map_clocks[4] = {[0 ... 3] = {.serial = 0, .load = 0, .owner = 0, .grid = 0, .grid_rev = 0, .phrase = 0, .phrase_rev = 0}};
struct MapMutation {
    _Atomic(uint64_t) generation;
    _Atomic(uint64_t) last_order;
    _Atomic(unsigned) active;
};
static MapMutation map_mutations[4] = {[0 ... 3] = {.generation = 0, .last_order = 0, .active = 0}};
struct MapBracket {
    uint64_t token;
    uint64_t order[4];
    unsigned mask;
};
static _Thread_local MapBracket map_brackets[8] = {[0 ... 7] = {.token = 0, .order = {0}, .mask = 0}};
static _Thread_local uint64_t bracket_token;
static uint64_t begin_map_mask(unsigned mask) {
    if (!mask || bracket_token == UINT64_MAX) return 0;
    {
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(map_brackets) / sizeof(map_brackets)[0]; ++range_index_5) {
            __auto_type b = &(map_brackets[range_index_5]);

            if (b->token) continue;
            b->mask = mask;
            b->token = ++bracket_token;
            for (unsigned d = 0; d < 4; d++)
                if (mask & (1u << d)) {
                    __auto_type m = &(map_mutations[d]);
                    atomic_fetch_add_explicit(&(m->active), 1, memory_order_acq_rel);
                    atomic_fetch_add_explicit(&(m->generation), 1, memory_order_acq_rel);
                    b->order[d] = atomic_load_explicit(&(m->last_order), memory_order_acquire);
                }
            return b->token;
        }
    }
    return 0;
}
static void end_map_mask(uint64_t token) {
    if (!token) return;
    {
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(map_brackets) / sizeof(map_brackets)[0]; ++range_index_6) {
            __auto_type b = &(map_brackets[range_index_6]);
            if (b->token == token) {
                for (unsigned d = 0; d < 4; d++)
                    if (b->mask & (1u << d)) {
                        __auto_type m = &(map_mutations[d]);
                        atomic_fetch_add_explicit(&(m->generation), 1, memory_order_acq_rel);
                        atomic_fetch_sub_explicit(&(m->active), 1, memory_order_release);
                    }
                b->token = 0;
                return;
            }
        }
    }
}
static bool exact_map_bracket(unsigned d) {
    __auto_type m = &(map_mutations[d]);
    if (atomic_load_explicit(&(m->active), memory_order_acquire) != 1) return false;
    uint64_t order = atomic_load_explicit(&(m->last_order), memory_order_acquire);
    {
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(map_brackets) / sizeof(map_brackets)[0]; ++range_index_7) {
            __auto_type b = &(map_brackets[range_index_7]);
            if (b->token && (b->mask & (1u << d)) && b->order[d] == order) return true;
        }
    }
    return false;
}
static uint64_t read_map_clock(unsigned d, uint64_t load, aznm_packet *out) {
    __auto_type c = &(map_clocks[d]);
    uint64_t seq = atomic_load_explicit(&(c->serial), memory_order_acquire);
    if ((seq & 1) || !seq || atomic_load_explicit(&(c->load), memory_order_relaxed) != load) return 0;
    aznm_packet p = {0};
    p.owner = atomic_load_explicit(&(c->owner), memory_order_relaxed);
    p.grid.provider = atomic_load_explicit(&(c->grid), memory_order_relaxed);
    p.grid.revision = atomic_load_explicit(&(c->grid_rev), memory_order_relaxed);
    p.phrases.provider = atomic_load_explicit(&(c->phrase), memory_order_relaxed);
    p.phrases.revision = atomic_load_explicit(&(c->phrase_rev), memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (seq != atomic_load_explicit(&(c->serial), memory_order_relaxed)) return 0;
    if (p.owner && !azn_maps_current(d, load, &p)) return UINT64_MAX;
    (*out) = p;
    return seq;
}
static int offer_map_cut(azstatus_native_receipt *r) {
    if (!r || r->deck >= 4 || !r->load_generation) return 0;
    unsigned d = r->deck;
    __auto_type m = &(state().decks[d].motion);
    __auto_type c = &(map_clocks[d]);
    uint64_t old = atomic_load_explicit(&(c->serial), memory_order_acquire);
    if ((old & 1) || old >= UINT64_MAX - 1 ||
        !atomic_compare_exchange_strong_explicit(&(c->serial), &(old), old + 1, memory_order_acq_rel, memory_order_acquire)) {
        azn_map_gap(d);
        return 0;
    }
    bool paired = r->map.owner && r->map.grid.provider && r->map.phrases.provider && r->map.grid.content && r->map.phrases.content;
    bool had_map = atomic_load_explicit(&(c->load), memory_order_relaxed) == r->load_generation && atomic_load_explicit(&(c->owner), memory_order_relaxed);
    bool same = had_map && paired && atomic_load_explicit(&(c->owner), memory_order_relaxed) == r->map.owner &&
                atomic_load_explicit(&(c->grid), memory_order_relaxed) == r->map.grid.provider &&
                atomic_load_explicit(&(c->grid_rev), memory_order_relaxed) == r->map.grid.revision &&
                atomic_load_explicit(&(c->phrase), memory_order_relaxed) == r->map.phrases.provider &&
                atomic_load_explicit(&(c->phrase_rev), memory_order_relaxed) == r->map.phrases.revision;
    if (same) {
        atomic_store_explicit(&(c->serial), old, memory_order_release);
        return azn_offer(r);
    }
    atomic_store_explicit(&(c->load), r->load_generation, memory_order_relaxed);
    atomic_store_explicit(&(c->owner), paired ? r->map.owner : 0, memory_order_relaxed);
    atomic_store_explicit(&(c->grid), r->map.grid.provider, memory_order_relaxed);
    atomic_store_explicit(&(c->grid_rev), r->map.grid.revision, memory_order_relaxed);
    atomic_store_explicit(&(c->phrase), r->map.phrases.provider, memory_order_relaxed);
    atomic_store_explicit(&(c->phrase_rev), r->map.phrases.revision, memory_order_relaxed);
    const uint64_t phrase_route = AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    if (had_map && r->map.changed && !exact_map_bracket(d)) r->lost_topics |= phrase_route;
    unsigned reservation = MOTION_FREE;
    if (!atomic_compare_exchange_strong_explicit(&(m->busy), &(reservation), MOTION_MAP, memory_order_acquire, memory_order_acquire)) {
        /* The overlapping native head writer detects the serial change and
         * cannot assign the ambiguous interval to either phrase map. */
        if (had_map) r->lost_topics |= phrase_route;
        int result = azn_offer(r);
        atomic_store_explicit(&(c->serial), old + 2, memory_order_release);
        return result;
    }
    if (m->load == r->load_generation && m->known && m->order < UINT64_MAX && m->generation &&
        !atomic_load_explicit(&(m->foreign), memory_order_acquire) && m->captured_interference == atomic_load_explicit(&(m->interference), memory_order_acquire)) {
        r->motion_order = ++m->order;
        r->motion_generation = m->generation;
        atomic_store_explicit(&(map_mutations[d].last_order), m->order, memory_order_release);
        r->points[1] = (__typeof__(r->points[1])) {AZN_COORD, m->rate, ((int64_t) (m->previous))};
        r->observed_window_samples = m->window_samples;
        r->observed_window_rate = m->window_rate;
        if (atomic_load_explicit(&(m->opening), memory_order_acquire) && !r->lost_topics) r->motion_flags = AZN_AT_OPENING;
    }
    int result = azn_offer(r);
    atomic_store_explicit(&(c->serial), old + 2, memory_order_release);
    atomic_store_explicit(&(m->busy), MOTION_FREE, memory_order_release);
    return result;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* The map publisher and both actual head writers share a commit number. The
 * atomic stamp has no pointers and survives retirement of the GUI object. */
struct MapClock {
    std::atomic<uint64_t> serial {0}, load {0}, owner {0}, grid {0}, grid_rev {0}, phrase {0}, phrase_rev {0};
};
static MapClock map_clocks[4];
struct MapMutation {
    std::atomic<uint64_t> generation {0}, last_order {0};
    std::atomic<unsigned> active {0};
};
static MapMutation map_mutations[4];
struct MapBracket {
    uint64_t token = 0, order[4] = {};
    unsigned mask = 0;
};
static thread_local MapBracket map_brackets[8];
static thread_local uint64_t bracket_token;
static uint64_t begin_map_mask(unsigned mask) {
    if (!mask || bracket_token == UINT64_MAX) return 0;
    for (auto &b : map_brackets) {
        if (b.token) continue;
        b.mask = mask;
        b.token = ++bracket_token;
        for (unsigned d = 0; d < 4; d++)
            if (mask & (1u << d)) {
                auto &m = map_mutations[d];
                m.active.fetch_add(1, std::memory_order_acq_rel);
                m.generation.fetch_add(1, std::memory_order_acq_rel);
                b.order[d] = m.last_order.load(std::memory_order_acquire);
            }
        return b.token;
    }
    return 0;
}
static void end_map_mask(uint64_t token) {
    if (!token) return;
    for (auto &b : map_brackets)
        if (b.token == token) {
            for (unsigned d = 0; d < 4; d++)
                if (b.mask & (1u << d)) {
                    auto &m = map_mutations[d];
                    m.generation.fetch_add(1, std::memory_order_acq_rel);
                    m.active.fetch_sub(1, std::memory_order_release);
                }
            b.token = 0;
            return;
        }
}
static bool exact_map_bracket(unsigned d) {
    const auto &m = map_mutations[d];
    if (m.active.load(std::memory_order_acquire) != 1) return false;
    uint64_t order = m.last_order.load(std::memory_order_acquire);
    for (auto &b : map_brackets)
        if (b.token && (b.mask & (1u << d)) && b.order[d] == order) return true;
    return false;
}
static uint64_t read_map_clock(unsigned d, uint64_t load, aznm_packet &out) noexcept {
    auto &c = map_clocks[d];
    uint64_t seq = c.serial.load(std::memory_order_acquire);
    if ((seq & 1) || !seq || c.load.load(std::memory_order_relaxed) != load) return 0;
    aznm_packet p {};
    p.owner = c.owner.load(std::memory_order_relaxed);
    p.grid.provider = c.grid.load(std::memory_order_relaxed);
    p.grid.revision = c.grid_rev.load(std::memory_order_relaxed);
    p.phrases.provider = c.phrase.load(std::memory_order_relaxed);
    p.phrases.revision = c.phrase_rev.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq != c.serial.load(std::memory_order_relaxed)) return 0;
    if (p.owner && !azn_maps_current(d, load, &p)) return UINT64_MAX;
    out = p;
    return seq;
}
static int offer_map_cut(azstatus_native_receipt *r) noexcept {
    if (!r || r->deck >= 4 || !r->load_generation) return 0;
    unsigned d = r->deck;
    auto &m = state().decks[d].motion;
    auto &c = map_clocks[d];
    uint64_t old = c.serial.load(std::memory_order_acquire);
    if ((old & 1) || old >= UINT64_MAX - 1 ||
        !c.serial.compare_exchange_strong(old, old + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        azn_map_gap(d);
        return 0;
    }
    bool paired = r->map.owner && r->map.grid.provider && r->map.phrases.provider && r->map.grid.content && r->map.phrases.content;
    bool had_map = c.load.load(std::memory_order_relaxed) == r->load_generation && c.owner.load(std::memory_order_relaxed);
    bool same = had_map && paired && c.owner.load(std::memory_order_relaxed) == r->map.owner &&
                c.grid.load(std::memory_order_relaxed) == r->map.grid.provider &&
                c.grid_rev.load(std::memory_order_relaxed) == r->map.grid.revision &&
                c.phrase.load(std::memory_order_relaxed) == r->map.phrases.provider &&
                c.phrase_rev.load(std::memory_order_relaxed) == r->map.phrases.revision;
    if (same) {
        c.serial.store(old, std::memory_order_release);
        return azn_offer(r);
    }
    c.load.store(r->load_generation, std::memory_order_relaxed);
    c.owner.store(paired ? r->map.owner : 0, std::memory_order_relaxed);
    c.grid.store(r->map.grid.provider, std::memory_order_relaxed);
    c.grid_rev.store(r->map.grid.revision, std::memory_order_relaxed);
    c.phrase.store(r->map.phrases.provider, std::memory_order_relaxed);
    c.phrase_rev.store(r->map.phrases.revision, std::memory_order_relaxed);
    const uint64_t phrase_route = AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    if (had_map && r->map.changed && !exact_map_bracket(d)) r->lost_topics |= phrase_route;
    unsigned reservation = Slot::Motion::FREE;
    if (!m.busy.compare_exchange_strong(reservation, Slot::Motion::MAP, std::memory_order_acquire)) {
        /* The overlapping native head writer detects the serial change and
         * cannot assign the ambiguous interval to either phrase map. */
        if (had_map) r->lost_topics |= phrase_route;
        int result = azn_offer(r);
        c.serial.store(old + 2, std::memory_order_release);
        return result;
    }
    if (m.load == r->load_generation && m.known && m.order < UINT64_MAX && m.generation &&
        !m.foreign.load(std::memory_order_acquire) && m.captured_interference == m.interference.load(std::memory_order_acquire)) {
        r->motion_order = ++m.order;
        r->motion_generation = m.generation;
        map_mutations[d].last_order.store(m.order, std::memory_order_release);
        r->points[1] = {AZN_COORD, m.rate, static_cast<int64_t>(m.previous)};
        r->observed_window_samples = m.window_samples;
        r->observed_window_rate = m.window_rate;
        if (m.opening.load(std::memory_order_acquire) && !r->lost_topics) r->motion_flags = AZN_AT_OPENING;
    }
    int result = azn_offer(r);
    c.serial.store(old + 2, std::memory_order_release);
    m.busy.store(Slot::Motion::FREE, std::memory_order_release);
    return result;
}

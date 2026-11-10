/* SPDX-License-Identifier: GPL-3.0-only */
/* CuePropertyWithBufferLock contains two independent resident-range locks.
 * The original checker requires both in state 2 for a retained loop. */
struct ReloopState {
    std::atomic<uintptr_t> retained {0};
    std::atomic<uint64_t> fault {0}, mutation {0};
    std::atomic<unsigned> editing {0}, writers {0};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    azn_point points[2] {};
    uint64_t unit = 0, load = 0, epoch = 0, seed = 0, cut = 0, sealed_fault = 0, raw[2] {};
    unsigned value = 0, property_revision = 0;
};
static ReloopState reloop_state[4];
static std::atomic<unsigned> reloop_installed {0};
static void *reloop_original[4];
static void reloop_loss(unsigned d) noexcept {
    reloop_state[d].fault.fetch_add(1, std::memory_order_acq_rel);
    azn_reloop_gap(d);
}
static unsigned reloop_value(const azn_point *points, uint64_t in, uint64_t out) noexcept {
    if (points[0].state == AZN_INVALID || points[1].state == AZN_INVALID) return AZN_RELOOP_STALE;
    if (points[0].state == AZN_ABSENT || points[1].state == AZN_ABSENT) return AZN_RELOOP_DENIED;
    const unsigned a = uint32_t(in), b = uint32_t(out);
    if (a > 3 || b > 3) return AZN_RELOOP_STALE;
    return a == 2 && b == 2 ? AZN_RELOOP_ALLOWED : AZN_RELOOP_DENIED;
}
static void reloop_observe(unsigned d, Slot &slot, uintptr_t retained, uint64_t load_fence,
                           int changed = -1, uint64_t before = 0) noexcept {
    if (reloop_installed.load(std::memory_order_acquire) != 15 || !(azn_interest(d) & status_loop_route)) return;
    auto &s = reloop_state[d];
    if (s.writers.load(std::memory_order_acquire) || s.editing.load(std::memory_order_acquire)) return;
    const uint64_t mutation = s.mutation.load(std::memory_order_acquire);
    if (s.busy.test_and_set(std::memory_order_acquire)) {
        if (!azn_load_history_revoked(d, load_fence)) reloop_loss(d);
        return;
    }
    struct Unlock {
        ReloopState &s;
        ~Unlock() { s.busy.clear(std::memory_order_release); }
    } guard {s};
    azsdk_load_sample load {}, end {};
    if (!retained || !azsdk_load_read(int(d), &load) || load.invalidation_epoch != load_fence ||
        !status_source_loaded(d, slot, load)) return;
    const auto previous = s.retained.exchange(retained, std::memory_order_acq_rel);
    const uint64_t raw[] = {word(retained + 0x178), word(retained + 0x200)};
    const unsigned rr = unsigned(integer(retained + 8));
    bool same = s.epoch == load.invalidation_epoch && previous == retained;
    uint64_t fault = s.fault.load(std::memory_order_acquire), seed = azn_seed_id(d), cut = azn_reloop_cut(d);
    if (same && s.cut == cut && s.seed == seed && s.sealed_fault == fault && rr == s.property_revision &&
        raw[0] == s.raw[0] && raw[1] == s.raw[1]) return;
    azn_point points[2];
    status_points(retained, points);
    status_qualify_points(slot, retained, points);
    unsigned value = reloop_value(points, raw[0], raw[1]);
    /* A seed can recover a lock moved outside the witnessed writer. No point
     * read, including a later equal value, substitutes for its history. */
    bool loss = false;
    bool could_be_ready = s.points[0].state != AZN_ABSENT && s.points[1].state != AZN_ABSENT;
    if (same && could_be_ready && s.sealed_fault == fault)
        for (unsigned i = 0; i < 2; ++i)
            if (s.raw[i] != raw[i] && (int(i) != changed || before != s.raw[i])) loss = true;
    if (!azsdk_load_read(int(d), &end) || end.invalidation_epoch != load.invalidation_epoch ||
        end.unit_generation != load.unit_generation || end.load_generation != load.load_generation ||
        integer(retained + 8) != int32_t(rr) || raw[0] != word(retained + 0x178) || raw[1] != word(retained + 0x200) ||
        s.fault.load(std::memory_order_acquire) != fault || mutation != s.mutation.load(std::memory_order_acquire) ||
        s.writers.load(std::memory_order_acquire) || s.editing.load(std::memory_order_acquire) || !status_source_loaded(d, slot, end)) {
        if (!azn_load_history_revoked(d, load.invalidation_epoch)) reloop_loss(d);
        return;
    }
    if (!same || cut != s.cut || seed != s.seed || fault != s.sealed_fault || value != s.value || loss) {
        azstatus_native_receipt r {};
        r.kind = AZN_RELOOP;
        r.deck = d;
        r.epoch = load.invalidation_epoch;
        r.unit = load.unit_generation;
        r.load_generation = load.load_generation;
        r.reloop = value;
        if (loss) r.lost_topics = status_loop_route;
        if (!azn_offer(&r)) return;
    }
    std::memcpy(s.points, points, sizeof points);
    s.unit = load.unit_generation;
    s.load = load.load_generation;
    s.epoch = load.invalidation_epoch;
    s.seed = seed;
    s.cut = cut;
    s.sealed_fault = fault;
    s.value = value;
    s.property_revision = rr;
    s.raw[0] = raw[0];
    s.raw[1] = raw[1];
}
/* A residency callback can run on the stock StateServer thread. The getter
 * owns the embedded lock until return, but does not grant ownership of the
 * PlayerCore or the other endpoint. Use only its before/after word and the
 * last sealed property edition. Never acquire the render thread's exclusive
 * OwnerGate from this callback. */
struct ReloopEdit {
    ReloopState *s = nullptr;
    explicit ReloopEdit(ReloopState *state) : s(state) {
        if (s) {
            s->editing.fetch_add(1, std::memory_order_acq_rel);
            s->mutation.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    void end() noexcept {
        if (s) {
            s->editing.fetch_sub(1, std::memory_order_release);
            s = nullptr;
        }
    }
    ~ReloopEdit() { end(); }
};
struct ReloopWrite {
    ReloopState *state = nullptr;
    unsigned deck = 0, index = 0;
    uintptr_t retained = 0, lock = 0;
    uint64_t before = 0, mutation = 0, load_fence = 0;
    bool overlap = false;
    explicit ReloopWrite(uintptr_t owner, bool probe = false) : lock(owner) {
        if (reloop_installed.load(std::memory_order_acquire) != 15) return;
        for (unsigned d = 0; d < 4; ++d) {
            auto &s = reloop_state[d];
            uintptr_t p = s.retained.load(std::memory_order_acquire);
            if (!p || (owner != p + 0xf8 && owner != p + 0x180) || !(azn_interest(d) & status_loop_route)) continue;
            before = word(owner + 0x80);
            if (probe && (uint32_t(before) & ~2u) != 1u) return; // this original branch cannot write
            state = &s;
            deck = d;
            retained = p;
            index = owner == p + 0x180;
            overlap = s.writers.fetch_add(1, std::memory_order_acq_rel) != 0;
            mutation = s.mutation.fetch_add(1, std::memory_order_acq_rel) + 1;
            load_fence = azn_lifecycle_fence(d);
            break;
        }
    }
    ~ReloopWrite() {
        if (state) state->writers.fetch_sub(1, std::memory_order_release);
    }
    void finish() noexcept {
        if (!state) return;
        auto &s = *state;
        const uint64_t after = word(lock + 0x80);
        if (before == after) return;
        if (s.busy.test_and_set(std::memory_order_acquire)) {
            if (!azn_load_history_revoked(deck, load_fence)) reloop_loss(deck);
            return;
        }
        struct Unlock {
            ReloopState &s;
            ~Unlock() { s.busy.clear(std::memory_order_release); }
        } guard {s};
        azsdk_load_sample load {};
        if (!azsdk_load_read(int(deck), &load) || load_fence != load.invalidation_epoch || load.invalidation_epoch != s.epoch ||
            load.unit_generation != s.unit || load.load_generation != s.load || retained != s.retained.load(std::memory_order_acquire)) return;
        /* Before the property writer commits a complete pair, both outcomes of
         * every buffer transition leave re-entry unavailable. Its final owner
         * snapshot supplies the new point edition and both lock states. */
        if (s.points[0].state == AZN_ABSENT || s.points[1].state == AZN_ABSENT) {
            s.raw[index] = after;
            return;
        }
        if (overlap || s.editing.load(std::memory_order_acquire) ||
            s.raw[index] != before || s.sealed_fault != s.fault.load(std::memory_order_acquire) ||
            s.cut != azn_reloop_cut(deck) || s.mutation.load(std::memory_order_acquire) != mutation) {
            reloop_loss(deck);
            return;
        }
        uint64_t raw[] = {s.raw[0], s.raw[1]};
        raw[index] = after;
        unsigned value = reloop_value(s.points, raw[0], raw[1]);
        if (value != s.value) {
            azstatus_native_receipt r {};
            r.kind = AZN_RELOOP;
            r.deck = deck;
            r.epoch = s.epoch;
            r.unit = s.unit;
            r.load_generation = s.load;
            r.reloop = value;
            if (!azn_offer(&r)) return;
        }
        s.raw[index] = after;
        s.value = value;
    }
};
template <unsigned I, class... Args>
static void reloop_write(uintptr_t lock, Args... args) {
    ReloopWrite write(lock);
    trampoline<void (*)(uintptr_t, Args...)> (&reloop_original[I])(lock, args...);
    write.finish();
}
static uintptr_t reloop_resident(uintptr_t lock) {
    ReloopWrite write(lock, true);
    uintptr_t result = trampoline<uintptr_t (*)(uintptr_t)>(&reloop_original[2])(lock);
    write.finish();
    return result;
}
static void reloop_install(State &s, int build, azn_setting_patch_fn patch) {
    static constexpr uintptr_t pc[][4] = {{0x13e4f88, 0x13e5ea0, 0x13e6258, 0x13e6330},
                                          {0x14a03c8, 0x14a12e0, 0x14a1698, 0x14a1770}};
    static constexpr uint32_t first[][2] = {{0x39402022, 0x34001ca2}, {0xb9408001, 0x34000a21}, {0xa9ba7bfd, 0x910003fd}, {0x52800061, 0xb9008001}};
    void *hooks[] = {reinterpret_cast<void *>(reloop_write<0, uintptr_t>), reinterpret_cast<void *>(reloop_write<1>),
                     reinterpret_cast<void *>(reloop_resident), reinterpret_cast<void *>(reloop_write<3>)};
    for (unsigned i = 0; i < 4; ++i)
        if (!(reloop_installed.load(std::memory_order_acquire) & (1u << i))) {
            uintptr_t addr = s.bias + pc[build - 1][i];
            if (!std::memcmp(reinterpret_cast<void *>(addr), first[i], sizeof first[i]) &&
                patch(addr, hooks[i], &reloop_original[i], "status-reloop-lock") == 1 && reloop_original[i])
                reloop_installed.fetch_or(1u << i, std::memory_order_release);
        }
}

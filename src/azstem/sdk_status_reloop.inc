/* SPDX-License-Identifier: GPL-3.0-only */
/* CuePropertyWithBufferLock contains two independent resident-range locks.
 * The original checker requires both in state 2 for a retained loop. */
struct ReloopState {
    _Atomic(uintptr_t) retained;
    _Atomic(uint64_t) fault;
    _Atomic(uint64_t) mutation;
    _Atomic(unsigned) editing;
    _Atomic(unsigned) writers;
    atomic_flag busy;
    azn_point points[2];
    uint64_t unit;
    uint64_t load;
    uint64_t epoch;
    uint64_t seed;
    uint64_t cut;
    uint64_t sealed_fault;
    uint64_t raw[2];
    unsigned value;
    unsigned property_revision;
};
static ReloopState reloop_state[4] = {[0 ... 3] = {.retained = 0, .fault = 0, .mutation = 0, .editing = 0, .writers = 0, .busy = ATOMIC_FLAG_INIT, .unit = 0, .load = 0, .epoch = 0, .seed = 0, .cut = 0, .sealed_fault = 0, .value = 0, .property_revision = 0}};
static _Atomic(unsigned) reloop_installed = 0;
static void *reloop_original[4];
static void reloop_loss(unsigned d) {
    atomic_fetch_add_explicit(&(reloop_state[d].fault), 1, memory_order_acq_rel);
    azn_reloop_gap(d);
}
static unsigned reloop_value(const azn_point *points, uint64_t in, uint64_t out) {
    if (points[0].state == AZN_INVALID || points[1].state == AZN_INVALID) return AZN_RELOOP_STALE;
    if (points[0].state == AZN_ABSENT || points[1].state == AZN_ABSENT) return AZN_RELOOP_DENIED;
    const unsigned a = (uint32_t) (in), b = (uint32_t) (out);
    if (a > 3 || b > 3) return AZN_RELOOP_STALE;
    return a == 2 && b == 2 ? AZN_RELOOP_ALLOWED : AZN_RELOOP_DENIED;
}
static void reloop_observe(unsigned d, Slot *slot, uintptr_t retained, uint64_t load_fence, int changed, uint64_t before) {
    if (atomic_load_explicit(&(reloop_installed), memory_order_acquire) != 15 || !(azn_interest(d) & status_loop_route)) return;
    __auto_type s = &(reloop_state[d]);
    if (atomic_load_explicit(&(s->writers), memory_order_acquire) || atomic_load_explicit(&(s->editing), memory_order_acquire)) return;
    const uint64_t mutation = atomic_load_explicit(&(s->mutation), memory_order_acquire);
    if (atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) {
        if (!azn_load_history_revoked(d, load_fence)) reloop_loss(d);
        return;
    }
    EXIT_FLAG_GUARD(guard, &s->busy);
    azsdk_load_sample load = {0}, end = {0};
    if (!retained || !azsdk_load_read((int) (d), &load) || load.invalidation_epoch != load_fence ||
        !status_source_loaded(d, &((*slot)), load)) return;
    __auto_type previous = atomic_exchange_explicit(&(s->retained), retained, memory_order_acq_rel);
    const uint64_t raw[] = {word(retained + 0x178), word(retained + 0x200)};
    const unsigned rr = (unsigned) (integer(retained + 8));
    bool same = s->epoch == load.invalidation_epoch && previous == retained;
    uint64_t fault = atomic_load_explicit(&(s->fault), memory_order_acquire), seed = azn_seed_id(d), cut = azn_reloop_cut(d);
    if (same && s->cut == cut && s->seed == seed && s->sealed_fault == fault && rr == s->property_revision &&
        raw[0] == s->raw[0] && raw[1] == s->raw[1]) return;
    azn_point points[2];
    status_points(retained, points);
    status_qualify_points(slot, retained, points);
    unsigned value = reloop_value(points, raw[0], raw[1]);
    /* uintptr_t seed can recover a lock moved outside the witnessed writer. No point
     * read, including a later equal value, substitutes for its history. */
    bool loss = false;
    bool could_be_ready = s->points[0].state != AZN_ABSENT && s->points[1].state != AZN_ABSENT;
    if (same && could_be_ready && s->sealed_fault == fault)
        for (unsigned i = 0; i < 2; ++i)
            if (s->raw[i] != raw[i] && ((int) (i) != changed || before != s->raw[i])) loss = true;
    if (!azsdk_load_read((int) (d), &end) || end.invalidation_epoch != load.invalidation_epoch ||
        end.unit_generation != load.unit_generation || end.load_generation != load.load_generation ||
        integer(retained + 8) != (int32_t) (rr) || raw[0] != word(retained + 0x178) || raw[1] != word(retained + 0x200) ||
        atomic_load_explicit(&(s->fault), memory_order_acquire) != fault || mutation != atomic_load_explicit(&(s->mutation), memory_order_acquire) ||
        atomic_load_explicit(&(s->writers), memory_order_acquire) || atomic_load_explicit(&(s->editing), memory_order_acquire) || !status_source_loaded(d, &((*slot)), end)) {
        if (!azn_load_history_revoked(d, load.invalidation_epoch)) reloop_loss(d);
        return;
    }
    if (!same || cut != s->cut || seed != s->seed || fault != s->sealed_fault || value != s->value || loss) {
        azstatus_native_receipt r = {0};
        r.kind = AZN_RELOOP;
        r.deck = d;
        r.epoch = load.invalidation_epoch;
        r.unit = load.unit_generation;
        r.load_generation = load.load_generation;
        r.reloop = value;
        if (loss) r.lost_topics = status_loop_route;
        if (!azn_offer(&r)) return;
    }
    memcpy(s->points, points, sizeof points);
    s->unit = load.unit_generation;
    s->load = load.load_generation;
    s->epoch = load.invalidation_epoch;
    s->seed = seed;
    s->cut = cut;
    s->sealed_fault = fault;
    s->value = value;
    s->property_revision = rr;
    s->raw[0] = raw[0];
    s->raw[1] = raw[1];
}
/* uintptr_t residency callback can run on the stock StateServer thread. The getter
 * owns the embedded lock until return, but does not grant ownership of the
 * PlayerCore or the other endpoint. Use only its before/after word and the
 * last sealed property edition. Never acquire the render thread's exclusive
 * OwnerGate from this callback. */
typedef struct ReloopEdit ReloopEdit;
struct ReloopEdit {
    ReloopState *s;
};
static void reloop_edit_init(ReloopEdit *ctx, ReloopState *state);
static void reloop_edit_end(ReloopEdit *ctx);
static void reloop_edit_drop(ReloopEdit *ctx);
static void reloop_edit_init(ReloopEdit *ctx, ReloopState *state) {
    (void) ctx;
    *ctx = (ReloopEdit) {.s = state};

    if (ctx->s) {
        atomic_fetch_add_explicit(&(ctx->s->editing), 1, memory_order_acq_rel);
        atomic_fetch_add_explicit(&(ctx->s->mutation), 1, memory_order_acq_rel);
    }
}
static void reloop_edit_end(ReloopEdit *ctx) {
    (void) ctx;

    if (ctx->s) {
        atomic_fetch_sub_explicit(&(ctx->s->editing), 1, memory_order_release);
        ctx->s = NULL;
    }
}
static void reloop_edit_drop(ReloopEdit *ctx) {
    (void) ctx;
    reloop_edit_end(ctx);
}

typedef struct ReloopWrite ReloopWrite;
struct ReloopWrite {
    ReloopState *state;
    unsigned deck;
    unsigned index;
    uintptr_t retained;
    uintptr_t lock;
    uint64_t before;
    uint64_t mutation;
    uint64_t load_fence;
    bool overlap;
};
static void reloop_write_scope_init(ReloopWrite *ctx, uintptr_t owner, bool probe);
static void reloop_write_scope_drop(ReloopWrite *ctx);
static void reloop_write_scope_finish(ReloopWrite *ctx);
static void reloop_write_scope_init(ReloopWrite *ctx, uintptr_t owner, bool probe) {
    (void) ctx;
    *ctx = (ReloopWrite) {.state = NULL, .deck = 0, .index = 0, .retained = 0, .before = 0, .mutation = 0, .load_fence = 0, .overlap = false, .lock = owner};

    if (atomic_load_explicit(&(reloop_installed), memory_order_acquire) != 15) return;
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type s = &(reloop_state[d]);
        uintptr_t p = atomic_load_explicit(&(s->retained), memory_order_acquire);
        if (!p || (owner != p + 0xf8 && owner != p + 0x180) || !(azn_interest(d) & status_loop_route)) continue;
        ctx->before = word(owner + 0x80);
        if (probe && ((uint32_t) (ctx->before) & ~2u) != 1u) return; // this original branch cannot write
        ctx->state = s;
        ctx->deck = d;
        ctx->retained = p;
        ctx->index = owner == p + 0x180;
        ctx->overlap = atomic_fetch_add_explicit(&(s->writers), 1, memory_order_acq_rel) != 0;
        ctx->mutation = atomic_fetch_add_explicit(&(s->mutation), 1, memory_order_acq_rel) + 1;
        ctx->load_fence = azn_lifecycle_fence(d);
        break;
    }
}
static void reloop_write_scope_drop(ReloopWrite *ctx) {
    (void) ctx;

    if (ctx->state) atomic_fetch_sub_explicit(&(ctx->state->writers), 1, memory_order_release);
}
static void reloop_write_scope_finish(ReloopWrite *ctx) {
    (void) ctx;

    if (!ctx->state) return;
    __auto_type s = &(*ctx->state);
    const uint64_t after = word(ctx->lock + 0x80);
    if (ctx->before == after) return;
    if (atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) {
        if (!azn_load_history_revoked(ctx->deck, ctx->load_fence)) reloop_loss(ctx->deck);
        return;
    }
    EXIT_FLAG_GUARD(guard, &s->busy);
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (ctx->deck), &load) || ctx->load_fence != load.invalidation_epoch || load.invalidation_epoch != s->epoch ||
        load.unit_generation != s->unit || load.load_generation != s->load || ctx->retained != atomic_load_explicit(&(s->retained), memory_order_acquire)) return;
        /* Before the property writer commits a complete pair, both outcomes of
         * every buffer transition leave re-entry unavailable. Its final owner
         * snapshot supplies the new point edition and both lock states. */
    if (s->points[0].state == AZN_ABSENT || s->points[1].state == AZN_ABSENT) {
        s->raw[ctx->index] = after;
        return;
    }
    if (ctx->overlap || atomic_load_explicit(&(s->editing), memory_order_acquire) ||
        s->raw[ctx->index] != ctx->before || s->sealed_fault != atomic_load_explicit(&(s->fault), memory_order_acquire) ||
        s->cut != azn_reloop_cut(ctx->deck) || atomic_load_explicit(&(s->mutation), memory_order_acquire) != ctx->mutation) {
        reloop_loss(ctx->deck);
        return;
    }
    uint64_t raw[] = {s->raw[0], s->raw[1]};
    raw[ctx->index] = after;
    unsigned value = reloop_value(s->points, raw[0], raw[1]);
    if (value != s->value) {
        azstatus_native_receipt r = {0};
        r.kind = AZN_RELOOP;
        r.deck = ctx->deck;
        r.epoch = s->epoch;
        r.unit = s->unit;
        r.load_generation = s->load;
        r.reloop = value;
        if (!azn_offer(&r)) return;
    }
    s->raw[ctx->index] = after;
    s->value = value;
}

static void reloop_write_0(uintptr_t lock, uintptr_t arg0) {
    ReloopWrite write __attribute__((cleanup(reloop_write_scope_drop)));
    reloop_write_scope_init(&write, lock, false);
    ((void (*)(uintptr_t, uintptr_t)) trampoline_address(&reloop_original[0]))(lock, arg0);
    reloop_write_scope_finish(&write);
}
static void reloop_write_1(uintptr_t lock) {
    ReloopWrite write __attribute__((cleanup(reloop_write_scope_drop)));
    reloop_write_scope_init(&write, lock, false);
    ((void (*)(uintptr_t)) trampoline_address(&reloop_original[1]))(lock);
    reloop_write_scope_finish(&write);
}
static void reloop_write_3(uintptr_t lock) {
    ReloopWrite write __attribute__((cleanup(reloop_write_scope_drop)));
    reloop_write_scope_init(&write, lock, false);
    ((void (*)(uintptr_t)) trampoline_address(&reloop_original[3]))(lock);
    reloop_write_scope_finish(&write);
}
static uintptr_t reloop_resident(uintptr_t lock) {
    ReloopWrite write __attribute__((cleanup(reloop_write_scope_drop)));
    reloop_write_scope_init(&write, lock, true);
    uintptr_t result = ((uintptr_t (*)(uintptr_t)) trampoline_address(&reloop_original[2]))(lock);
    reloop_write_scope_finish(&write);
    return result;
}
static void reloop_install(State *s, int build, azn_setting_patch_fn patch) {
    static const uintptr_t pc[][4] = {{0x13e4f88, 0x13e5ea0, 0x13e6258, 0x13e6330},
                                      {0x14a03c8, 0x14a12e0, 0x14a1698, 0x14a1770}};
    static const uint32_t first[][2] = {{0x39402022, 0x34001ca2}, {0xb9408001, 0x34000a21}, {0xa9ba7bfd, 0x910003fd}, {0x52800061, 0xb9008001}};
    void *hooks[] = {((void *) (reloop_write_0)), ((void *) (reloop_write_1)),
                     ((void *) (reloop_resident)), ((void *) (reloop_write_3))};
    for (unsigned i = 0; i < 4; ++i)
        if (!(atomic_load_explicit(&(reloop_installed), memory_order_acquire) & (1u << i))) {
            uintptr_t addr = s->bias + pc[build - 1][i];
            if (!memcmp(((void *) (addr)), first[i], sizeof first[i]) &&
                patch(addr, hooks[i], &reloop_original[i], "status-reloop-lock") == 1 && reloop_original[i])
                atomic_fetch_or_explicit(&(reloop_installed), 1u << i, memory_order_release);
        }
}

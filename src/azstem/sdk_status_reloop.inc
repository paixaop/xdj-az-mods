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
struct ReloopObservation {
    azsdk_load_sample load;
    uint64_t mutation, raw[2], fault, seed, cut;
    unsigned revision, value;
    azn_point points[2];
    bool same, loss;
};
static bool reloop_capture_ready(unsigned d, const struct ReloopState *s) {
    return atomic_load_explicit(&(reloop_installed), memory_order_acquire) == 15 && (azn_interest(d) & status_loop_route) &&
           !atomic_load_explicit(&(s->writers), memory_order_acquire) && !atomic_load_explicit(&(s->editing), memory_order_acquire);
}
static void reloop_capture_failed(unsigned d, uint64_t epoch) {
    if (!azn_load_history_revoked(d, epoch)) reloop_loss(d);
}
static bool reloop_capture_load(unsigned d, Slot *slot, uintptr_t retained, uint64_t fence, azsdk_load_sample *load) {
    return retained && azsdk_load_read((int) d, load) && load->invalidation_epoch == fence && status_source_loaded(d, slot, *load);
}
static void reloop_read_state(unsigned d, uintptr_t retained, struct ReloopObservation *value) {
    struct ReloopState *s = &reloop_state[d];
    uintptr_t previous = atomic_exchange_explicit(&(s->retained), retained, memory_order_acq_rel);
    value->raw[0] = word(retained + 0x178);
    value->raw[1] = word(retained + 0x200);
    value->revision = (unsigned) integer(retained + 8);
    value->same = s->epoch == value->load.invalidation_epoch && previous == retained;
    value->fault = atomic_load_explicit(&(s->fault), memory_order_acquire);
    value->seed = azn_seed_id(d);
    value->cut = azn_reloop_cut(d);
}
static bool reloop_unchanged(const struct ReloopState *s, const struct ReloopObservation *value) {
    return value->same && s->cut == value->cut && s->seed == value->seed && s->sealed_fault == value->fault &&
           value->revision == s->property_revision && value->raw[0] == s->raw[0] && value->raw[1] == s->raw[1];
}
static bool reloop_unwitnessed_lock(const struct ReloopState *s, const uint64_t raw[2], int changed, uint64_t before) {
    for (unsigned i = 0; i < 2; ++i)
        if (s->raw[i] != raw[i] && ((int) i != changed || before != s->raw[i])) return true;
    return false;
}
static bool reloop_lost(const struct ReloopState *s, const struct ReloopObservation *value, int changed, uint64_t before) {
    /* A seed can recover a lock moved outside the witnessed writer. No point
     * read, including a later equal value, substitutes for its history. */
    bool could_be_ready = s->points[0].state != AZN_ABSENT && s->points[1].state != AZN_ABSENT;
    return value->same && could_be_ready && s->sealed_fault == value->fault && reloop_unwitnessed_lock(s, value->raw, changed, before);
}
static bool reloop_property_current(const struct ReloopState *s, uintptr_t retained, const struct ReloopObservation *value) {
    return integer(retained + 8) == (int32_t) value->revision && value->raw[0] == word(retained + 0x178) && value->raw[1] == word(retained + 0x200) &&
           atomic_load_explicit(&(s->fault), memory_order_acquire) == value->fault && value->mutation == atomic_load_explicit(&(s->mutation), memory_order_acquire) &&
           !atomic_load_explicit(&(s->writers), memory_order_acquire) && !atomic_load_explicit(&(s->editing), memory_order_acquire);
}
static bool reloop_observation_current(unsigned d, Slot *slot, uintptr_t retained, const struct ReloopObservation *value) {
    azsdk_load_sample end = {0};
    if (!azsdk_load_read((int) d, &end) || end.invalidation_epoch != value->load.invalidation_epoch ||
        end.unit_generation != value->load.unit_generation || end.load_generation != value->load.load_generation) return false;
    return reloop_property_current(&reloop_state[d], retained, value) && status_source_loaded(d, slot, end);
}
static bool reloop_needs_receipt(const struct ReloopState *s, const struct ReloopObservation *value) {
    return !value->same || value->cut != s->cut || value->seed != s->seed || value->fault != s->sealed_fault || value->value != s->value || value->loss;
}
static bool reloop_publish(unsigned d, const struct ReloopObservation *value) {
    azstatus_native_receipt receipt = {0};
    receipt.kind = AZN_RELOOP;
    receipt.deck = d;
    receipt.epoch = value->load.invalidation_epoch;
    receipt.unit = value->load.unit_generation;
    receipt.load_generation = value->load.load_generation;
    receipt.reloop = value->value;
    if (value->loss) receipt.lost_topics = status_loop_route;
    return azn_offer(&receipt) != 0;
}
static void reloop_commit(struct ReloopState *s, const struct ReloopObservation *value) {
    memcpy(s->points, value->points, sizeof value->points);
    s->unit = value->load.unit_generation;
    s->load = value->load.load_generation;
    s->epoch = value->load.invalidation_epoch;
    s->seed = value->seed;
    s->cut = value->cut;
    s->sealed_fault = value->fault;
    s->value = value->value;
    s->property_revision = value->revision;
    s->raw[0] = value->raw[0];
    s->raw[1] = value->raw[1];
}
static void reloop_observe(unsigned d, Slot *slot, uintptr_t retained, uint64_t load_fence, int changed, uint64_t before) {
    struct ReloopState *s = &reloop_state[d];
    if (!reloop_capture_ready(d, s)) return;
    struct ReloopObservation value = {0};
    value.mutation = atomic_load_explicit(&(s->mutation), memory_order_acquire);
    if (atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) {
        reloop_capture_failed(d, load_fence);
        return;
    }
    EXIT_FLAG_GUARD(guard, &s->busy);
    if (!reloop_capture_load(d, slot, retained, load_fence, &value.load)) return;
    reloop_read_state(d, retained, &value);
    if (reloop_unchanged(s, &value)) return;
    status_points(retained, value.points);
    status_qualify_points(slot, retained, value.points);
    value.value = reloop_value(value.points, value.raw[0], value.raw[1]);
    value.loss = reloop_lost(s, &value, changed, before);
    if (!reloop_observation_current(d, slot, retained, &value)) {
        reloop_capture_failed(d, value.load.invalidation_epoch);
        return;
    }
    if (reloop_needs_receipt(s, &value) && !reloop_publish(d, &value)) return;
    reloop_commit(s, &value);
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
static bool reloop_write_current(const ReloopWrite *ctx, azsdk_load_sample *load) {
    const struct ReloopState *s = ctx->state;
    return azsdk_load_read((int) ctx->deck, load) && ctx->load_fence == load->invalidation_epoch && load->invalidation_epoch == s->epoch &&
           load->unit_generation == s->unit && load->load_generation == s->load &&
           ctx->retained == atomic_load_explicit(&(s->retained), memory_order_acquire);
}
static bool reloop_write_witnessed(const ReloopWrite *ctx) {
    const struct ReloopState *s = ctx->state;
    return !ctx->overlap && !atomic_load_explicit(&(s->editing), memory_order_acquire) && s->raw[ctx->index] == ctx->before &&
           s->sealed_fault == atomic_load_explicit(&(s->fault), memory_order_acquire) && s->cut == azn_reloop_cut(ctx->deck) &&
           atomic_load_explicit(&(s->mutation), memory_order_acquire) == ctx->mutation;
}
static bool reloop_write_publish(const ReloopWrite *ctx, unsigned value) {
    const struct ReloopState *s = ctx->state;
    if (value == s->value) return true;
    azstatus_native_receipt receipt = {0};
    receipt.kind = AZN_RELOOP;
    receipt.deck = ctx->deck;
    receipt.epoch = s->epoch;
    receipt.unit = s->unit;
    receipt.load_generation = s->load;
    receipt.reloop = value;
    return azn_offer(&receipt) != 0;
}
static void reloop_write_scope_finish(ReloopWrite *ctx) {
    if (!ctx->state) return;
    struct ReloopState *s = ctx->state;
    const uint64_t after = word(ctx->lock + 0x80);
    if (ctx->before == after) return;
    if (atomic_flag_test_and_set_explicit(&(s->busy), memory_order_acquire)) {
        reloop_capture_failed(ctx->deck, ctx->load_fence);
        return;
    }
    EXIT_FLAG_GUARD(guard, &s->busy);
    azsdk_load_sample load = {0};
    if (!reloop_write_current(ctx, &load)) return;
    /* Before the property writer commits a complete pair, both outcomes of
     * every buffer transition leave re-entry unavailable. Its final owner
     * snapshot supplies the new point edition and both lock states. */
    if (s->points[0].state == AZN_ABSENT || s->points[1].state == AZN_ABSENT) {
        s->raw[ctx->index] = after;
        return;
    }
    if (!reloop_write_witnessed(ctx)) {
        reloop_loss(ctx->deck);
        return;
    }
    uint64_t raw[] = {s->raw[0], s->raw[1]};
    raw[ctx->index] = after;
    unsigned value = reloop_value(s->points, raw[0], raw[1]);
    if (!reloop_write_publish(ctx, value)) return;
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

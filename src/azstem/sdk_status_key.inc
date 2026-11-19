/* SPDX-License-Identifier: GPL-3.0-only */
/* The KeyFunction child (verified by both facades) owns master-tempo at +8
 * and the semitone adjustment at +24. Read the latter's committed ratio, so
 * rejected requests, including a refused out-of-range combination, stay quiet. */
struct KeyProjection {
    _Atomic(uintptr_t) address;
    _Atomic(uint64_t) provider;
    _Atomic(uint64_t) fault;
    atomic_flag busy;
    uint64_t sampled_seed;
    uint64_t sampled_load;
    int last;
};
static KeyProjection key_projection[4] = {[0 ... 3] = {.address = 0, .provider = 0, .fault = 0, .busy = ATOMIC_FLAG_INIT, .sampled_seed = 0, .sampled_load = 0, .last = INT32_MAX}};
static _Atomic(unsigned) key_hooked = 0;
static _Atomic(uint64_t) key_incarnations = 0;
static void *key_original;

/* Exact positive-octave mantissas in both admitted firmware tables. The
 * setter uses the same mantissas one exponent lower for negative shifts. */
static int key_semitones(uint64_t ratio) {
    static const uint64_t within_octave[] = {
        UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0f38f92d97963), UINT64_C(0x3ff1f59ac3c7d6c0),
        UINT64_C(0x3ff306fe0a31b716), UINT64_C(0x3ff428a2f98d728c), UINT64_C(0x3ff55b8108f0ec5f),
        UINT64_C(0x3ff6a09e667f3bce), UINT64_C(0x3ff7f910d768cfb2), UINT64_C(0x3ff965fea53d6e3f),
        UINT64_C(0x3ffae89f995ad3b0), UINT64_C(0x3ffc823e074ec12c), UINT64_C(0x3ffe3437e7101347)};
    if (ratio == UINT64_C(0x4000000000000000)) return 12;
    for (int n = 0; n < 12; ++n) {
        if (ratio == within_octave[n]) return n;
        if (ratio == within_octave[n] - (UINT64_C(1) << 52)) return n - 12;
    }
    return INT32_MAX;
}
static bool key_sample_owner(const KeyProjection *p, uintptr_t address, uint64_t owner, uint64_t load, uint64_t fault) {
    if (!owner || !load || atomic_load_explicit(&(p->address), memory_order_acquire) != address ||
        atomic_load_explicit(&(p->provider), memory_order_acquire) != owner || atomic_load_explicit(&(p->fault), memory_order_acquire) != fault) return false;
    return true;
}
static bool key_sample_current(unsigned deck, const KeyProjection *p, uintptr_t address, uint64_t owner,
                               const azsdk_load_sample *before, uint64_t fault, azsdk_load_sample *after) {
    if (!azsdk_load_read((int) (deck), after) || before->load_generation != after->load_generation ||
        before->unit_generation != after->unit_generation || before->invalidation_epoch != after->invalidation_epoch ||
        atomic_load_explicit(&(p->fault), memory_order_acquire) != fault || atomic_load_explicit(&(p->provider), memory_order_acquire) != owner ||
        atomic_load_explicit(&(p->address), memory_order_acquire) != address) return false;
    return true;
}
static bool key_sample(unsigned deck, KeyProjection *p, uintptr_t address, uint64_t owner, const azsdk_load_sample before, uint64_t fault, bool seed) {
    if (!key_sample_owner(p, address, owner, before.load_generation, fault)) return false;
    const int step = key_semitones(word(address + 8));
    azsdk_load_sample after = {0};
    if (!key_sample_current(deck, p, address, owner, &before, fault, &after)) return false;
    if (seed || p->sampled_load != after.load_generation || p->last != step) {
        azstatus_native_receipt r = {0};
        r.deck = deck;
        r.kind = AZN_KEY_SHIFT;
        r.unit = after.unit_generation;
        r.epoch = after.invalidation_epoch;
        r.load_generation = after.load_generation;
        r.attempt = owner;
        r.a = step;
        r.valid = step != INT32_MAX;
        if (!azn_offer(&r)) return false;
    }
    p->sampled_load = after.load_generation;
    p->last = step;
    return true;
}
static void key_retire(unsigned deck) {
    __auto_type p = &(key_projection[deck]);
    atomic_store_explicit(&(p->address), 0, memory_order_release);
    uint64_t owner = atomic_exchange_explicit(&(p->provider), 0, memory_order_acq_rel);
    if (owner) {
        azsdk_load_sample load = {0};
        if (azsdk_load_read((int) (deck), &load)) {
            azstatus_native_receipt r = {0};
            r.deck = deck;
            r.kind = AZN_KEY_SHIFT;
            r.valid = -1;
            r.attempt = owner;
            r.unit = load.unit_generation;
            r.epoch = load.invalidation_epoch;
            r.load_generation = load.load_generation;
            azn_emit(&r);
        }
        azn_capability(deck, AZN_CAP_KEY_SHIFT, 0);
    }
}
static void key_replace_owner(unsigned deck, KeyProjection *p, uintptr_t address) {
    key_retire(deck);
    p->sampled_load = p->sampled_seed = 0;
    p->last = INT32_MAX;
    if (address) {
        uint64_t n = atomic_fetch_add_explicit(&(key_incarnations), 1, memory_order_relaxed) + 1;
        if (n && n <= AZSP_SERIAL_MAX) {
            atomic_store_explicit(&(p->provider), AZSP_KEY | n, memory_order_release);
            atomic_store_explicit(&(p->address), address, memory_order_release);
            azn_capability(deck, AZN_CAP_KEY_SHIFT, 1);
        }
    }
}
static void key_seed_frame(unsigned deck, KeyProjection *p, uintptr_t address, uint64_t fault) {
    azsdk_load_sample live = {0};
    uint64_t seed = azn_seed_id(deck);
    if (address && azsdk_load_read((int) (deck), &live) && (p->sampled_seed != seed || p->sampled_load != live.load_generation)) {
        if (key_sample(deck, p, address, atomic_load_explicit(&(p->provider), memory_order_relaxed), live, fault, true))
            p->sampled_seed = seed;
    }
}
static void key_frame(unsigned deck, Slot *slot) {
    if (!atomic_load_explicit(&(key_hooked), memory_order_acquire) || !(azn_interest(deck) & AZ_STATUS_TOPIC(AZ_STATUS_KEY))) return;
    uintptr_t mt = setting_object(slot, 1), address = mt ? mt + 16 : 0;
    __auto_type p = &(key_projection[deck]);
    if (atomic_flag_test_and_set_explicit(&(p->busy), memory_order_acquire)) return;
    uint64_t fault = atomic_load_explicit(&(p->fault), memory_order_acquire);
    if (address != atomic_load_explicit(&(p->address), memory_order_acquire)) {
        key_replace_owner(deck, p, address);
    }
    key_seed_frame(deck, p, address, fault);
    atomic_flag_clear_explicit(&(p->busy), memory_order_release);
}
typedef struct KeyCapture KeyCapture;
struct KeyCapture {
    uintptr_t address;
    KeyProjection *owner;
    unsigned deck;
    uint64_t provider;
    uint64_t fault;
    azsdk_load_sample before;
};
static void key_capture_init(KeyCapture *ctx, uintptr_t target);
static void key_capture_finish(KeyCapture *ctx);
static void key_capture_drop(KeyCapture *ctx);
static void key_capture_init(KeyCapture *ctx, uintptr_t target) {
    (void) ctx;
    *ctx = (KeyCapture) {.owner = NULL, .deck = 0, .provider = 0, .fault = 0, .address = target};

    if (!target || !atomic_load_explicit(&(key_hooked), memory_order_acquire)) return;
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type p = &(key_projection[d]);
        if (atomic_load_explicit(&(p->address), memory_order_acquire) != target) continue;
        if (atomic_flag_test_and_set_explicit(&(p->busy), memory_order_acquire)) {
            atomic_fetch_add_explicit(&(p->fault), 1, memory_order_acq_rel);
            azn_key_gap(d);
            break;
        }
        ctx->owner = p;
        ctx->deck = d;
        ctx->provider = atomic_load_explicit(&(p->provider), memory_order_acquire);
        ctx->fault = atomic_load_explicit(&(p->fault), memory_order_acquire);
        azsdk_load_read((int) (d), &ctx->before);
        break;
    }
}
static void key_capture_finish(KeyCapture *ctx) {
    (void) ctx;

    if (ctx->owner) key_sample(ctx->deck, &(*ctx->owner), ctx->address, ctx->provider, ctx->before, ctx->fault, false);
}
static void key_capture_drop(KeyCapture *ctx) {
    (void) ctx;
    if (ctx->owner) atomic_flag_clear_explicit(&(ctx->owner->busy), memory_order_release);
}

static void key_writer(void *self, int32_t semitones, unsigned mode) {
    __auto_type original = ((void (*)(void *, int32_t, unsigned)) trampoline_address(&key_original));
    KeyCapture capture __attribute__((cleanup(key_capture_drop)));
    key_capture_init(&capture, ((uintptr_t) (self)));
    original(self, semitones, mode);
    key_capture_finish(&capture);
}
static void key_install(State *s, int build, azn_setting_patch_fn patch) {
    if (atomic_load_explicit(&(key_hooked), memory_order_acquire)) return;
    static const uint32_t signatures[2][4] = {
        {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4}, {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xa9025bf5}};
    uintptr_t target = s->bias + (build == 1 ? 0x141bb08 : 0x14d6ef8);
    if (!memcmp(((const void *) (target)), signatures[build - 1], sizeof signatures[0]) &&
        patch(target, ((void *) (key_writer)), &key_original, "status-key-shift") == 1 && key_original)
        atomic_store_explicit(&(key_hooked), 1, memory_order_release);
}

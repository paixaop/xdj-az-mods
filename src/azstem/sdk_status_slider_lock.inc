/* SPDX-License-Identifier: GPL-3.0-only */
/* The stock PlayerState writer booleanizes BeatSyncFunction +0x170 into
 * payload +0x74 (PlayerState +0x8c), the named TempoSliderLocked field.
 * The constructor initializes it before unit registration. Later stores
 * belong to these six methods or setSyncMode. */
static void *slider_lock_original[6];
static unsigned slider_lock_hooks;
struct SliderLockWrite;
static _Thread_local SliderLockWrite *active_slider_write;
typedef struct SliderLockWrite SliderLockWrite;
struct SliderLockWrite {
    Setting *setting;
    SliderLockWrite *previous;
    uintptr_t object;
    uint64_t owner;
    uint64_t fault;
    unsigned deck;
    bool acquired;
};
static void slider_lock_write_init(SliderLockWrite *ctx, uintptr_t self);
static void slider_lock_write_commit(SliderLockWrite *ctx);
static void slider_lock_write_drop(SliderLockWrite *ctx);
static void slider_lock_write_init(SliderLockWrite *ctx, uintptr_t self) {
    (void) ctx;
    *ctx = (SliderLockWrite) {.setting = NULL, .previous = NULL, .owner = 0, .fault = 0, .deck = 0, .acquired = false, .object = self};

    if (!self) return;
    for (unsigned d = 0; d < 4; d++) {
        __auto_type v = &(state().decks[d].settings[3]);
        if (atomic_load_explicit(&(v->address), memory_order_acquire) != self) continue;
        ctx->previous = active_slider_write;
        if (ctx->previous && ctx->previous->object == self && ctx->previous->setting == v) slider_lock_write_commit(ctx->previous);
        else if (atomic_flag_test_and_set_explicit(&(v->busy), memory_order_acquire)) {
            atomic_fetch_add_explicit(&(v->fault), 1, memory_order_acq_rel);
            azn_setting_gap(d, AZN_SLIDER_LOCK);
            return;
        } else ctx->acquired = true;
        ctx->setting = v;
        ctx->deck = d;
        ctx->owner = atomic_load_explicit(&(v->provider), memory_order_acquire);
        ctx->fault = atomic_load_explicit(&(v->fault), memory_order_acquire);
        active_slider_write = ctx;
        return;
    }
}
static void slider_lock_write_commit(SliderLockWrite *ctx) {
    (void) ctx;

    if (!ctx->setting || !ctx->owner || atomic_load_explicit(&(ctx->setting->address), memory_order_acquire) != ctx->object ||
        atomic_load_explicit(&(ctx->setting->provider), memory_order_acquire) != ctx->owner || atomic_load_explicit(&(ctx->setting->fault), memory_order_acquire) != ctx->fault) return;
    unsigned value = integer(ctx->object + 0x170) != 0;
    if (ctx->setting->last == value) return;
    ctx->setting->last = value;
    setting_receipt(ctx->deck, 3, ctx->owner, value, 1, -1);
}
static void slider_lock_write_drop(SliderLockWrite *ctx) {
    (void) ctx;

    if (ctx->setting) {
        slider_lock_write_commit(ctx);
        active_slider_write = ctx->previous;
        if (ctx->acquired) atomic_flag_clear_explicit(&(ctx->setting->busy), memory_order_release);
    }
}

static void slider_lock_writer_0(void *self) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&slider_lock_original[0]))(self);
}
static void slider_lock_writer_1(void *self) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&slider_lock_original[1]))(self);
}
static void slider_lock_writer_2(void *self) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&slider_lock_original[2]))(self);
}
static void slider_lock_writer_3(void *self) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&slider_lock_original[3]))(self);
}
static void slider_lock_writer_4(void *self) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&slider_lock_original[4]))(self);
}
static void slider_lock_writer_5(void *self, unsigned arg0, unsigned arg1, unsigned arg2) {
    SliderLockWrite capture __attribute__((cleanup(slider_lock_write_drop)));
    slider_lock_write_init(&capture, ((uintptr_t) (self)));
    ((void (*)(void *, unsigned, unsigned, unsigned)) trampoline_address(&slider_lock_original[5]))(self, arg0, arg1, arg2);
}
static void slider_lock_install(State *s, int build, azn_setting_patch_fn patch) {
    const uintptr_t sites[][6] = {{0x13def18, 0x13dfd28, 0x13e0238, 0x13e0290, 0x13e02c8, 0x13e08a0},
                                  {0x1499f50, 0x149ad60, 0x149b2b0, 0x149b308, 0x149b340, 0x149c3e0}};
    const uint32_t signature[][2] = {{0xf9404401, 0x39418821}, {0xf940e002, 0x7901881f}, {0xa9be7bfd, 0x910003fd}, {0xa9be7bfd, 0x910003fd}, {0xa9bc7bfd, 0xd0ffffa1}, {0xa9be7bfd, 0x72001c3f}};
    void *hooks[] = {((void *) (slider_lock_writer_0)), ((void *) (slider_lock_writer_1)),
                     ((void *) (slider_lock_writer_2)), ((void *) (slider_lock_writer_3)),
                     ((void *) (slider_lock_writer_4)), ((void *) (slider_lock_writer_5))};
    for (unsigned i = 0; i < 6; i++) {
        uintptr_t address = s->bias + sites[build - 1][i];
        if (!(slider_lock_hooks & (1u << i)) && !memcmp(((const void *) (address)), signature[i], 8) &&
            patch(address, hooks[i], &slider_lock_original[i], "status-slider-lock") == 1) slider_lock_hooks |= 1u << i;
    }
    if (slider_lock_hooks == 63 && (atomic_load_explicit(&(setting_installed), memory_order_acquire) & 4))
        atomic_fetch_or_explicit(&(setting_installed), 8, memory_order_release);
}

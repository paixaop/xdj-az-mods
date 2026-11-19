/* SPDX-License-Identifier: GPL-3.0-only */
/* The grid-aware BeatJumpFunction and its no-grid fallback can both call
 * Quantize's JumpController synchronously with a null callback. */
struct NoQuaProfile {
    uintptr_t entry, grid_entry, owner, jump_returns[2];
    uint32_t first[4];
};
static const NoQuaProfile noqua_profiles[2] = {
    {0x1455f30, 0x14574b8, 0x2894e78, {0x1445670, 0x14456b8}, {0xa9ab7bfd, 0x910003fd, 0xa90153f3, 0xf000a1f3}},
    {0x1511618, 0x1512b80, 0x2be7938, {0x1500c68, 0x1500cb0}, {0xa9ab7bfd, 0x910003fd, 0xa90153f3, 0xd000b6b3}}};
static void *noqua_original, *beat_grid_original;
struct NoQuaOrigin {
    Slot *owner;
    uint64_t fence, load;
};
static _Thread_local NoQuaOrigin beat_origin = {0};
static bool beat_owner_arguments(const State *s, const Slot *slot, int direction, uint32_t beat) {
    return slot && (direction == 0 || direction == 1) && (beat & 0xffffu) && (beat >> 16) && s->build >= 1 && s->build <= 2;
}
static bool beat_owner_binding(const State *s, const Slot *slot, uintptr_t owner) {
    uintptr_t pause = word(slot->facade + 0x58), q = word(slot->facade + 0x50);
    return table(pause, s->profile.play_pause_table) && table(q, s->profile.quantize) &&
           word(owner + 8) == pause && word(pause + 0x58) == atomic_load_explicit(&(slot->core), memory_order_acquire) &&
           word(owner + 0x10) == q && word(owner + 0x20) == q + 0x20;
}
static NoQuaOrigin beat_owner(void *self, int direction, uint32_t beat) {
    State *s = &state();
    Slot *slot = processing;
    const uintptr_t owner = (uintptr_t) self;
    if (!beat_owner_arguments(s, slot, direction, beat) ||
        !table(owner, s->bias + noqua_profiles[s->build - 1].owner) || !coherent(slot)) return (NoQuaOrigin) {0};
    azsdk_load_sample load = {0};
    if (beat_owner_binding(s, slot, owner) && azsdk_load_read((int) (slot - s->decks), &load))
        return (NoQuaOrigin) {slot, load.invalidation_epoch, load.load_generation};
    return (NoQuaOrigin) {0};
}
typedef struct BeatOwnerFrame BeatOwnerFrame;
struct BeatOwnerFrame {
    NoQuaOrigin prior;
};
static void beat_owner_frame_init(BeatOwnerFrame *ctx, void *self, int direction, uint32_t beat);
static void beat_owner_frame_drop(BeatOwnerFrame *ctx);
static void beat_owner_frame_init(BeatOwnerFrame *ctx, void *self, int direction, uint32_t beat) {
    (void) ctx;
    *ctx = (BeatOwnerFrame) {.prior = beat_origin};

    beat_origin = beat_owner(self, direction, beat);
}
static void beat_owner_frame_drop(BeatOwnerFrame *ctx) {
    (void) ctx;
    beat_origin = ctx->prior;
}

static bool noqua_hook(void *self, int direction, uint32_t beat, const void *ref) {
    BeatOwnerFrame frame __attribute__((cleanup(beat_owner_frame_drop)));
    beat_owner_frame_init(&frame, self, direction, beat);
    return ((bool (*)(void *, int, uint32_t, const void *)) trampoline_address(&noqua_original))(self, direction, beat, ref);
}
static bool beat_grid_hook(void *self, int direction, uint32_t beat, const void *ref, bool option) {
    BeatOwnerFrame frame __attribute__((cleanup(beat_owner_frame_drop)));
    beat_owner_frame_init(&frame, self, direction, beat);
    return ((bool (*)(void *, int, uint32_t, const void *, bool)) trampoline_address(&beat_grid_original))(self, direction, beat, ref, option);
}
static bool noqua_request_frame(const State *s, const Slot *slot, const StatusActionScope *out, int mode) {
    return slot && slot == processing && out->controller && !out->callback && mode == 2 && s->build >= 1 && s->build <= 2;
}
static void status_noqua_request(StatusActionScope *out, const void *position, int mode, uintptr_t caller) {
    __auto_type s = &(state());
    __auto_type slot = beat_origin.owner;
    if (!noqua_request_frame(s, slot, out, mode)) return;
    __auto_type p = &(noqua_profiles[s->build - 1]);
    uintptr_t pos = ((uintptr_t) (position));
    if (!pointer(pos) || (caller != s->bias + p->jump_returns[0] && caller != s->bias + p->jump_returns[1])) return;
    memcpy(out->beat_source, ((const void *) (pos + 8)), 12);
    out->beat = azn_load_track_matches((unsigned) (slot - s->decks), beat_origin.fence, beat_origin.load, out->beat_source);
    out->beat_sample = word(pos + 0x18);
}
static bool status_install_noqua(State *s, int build, azexit_install_fn install) {
    __auto_type p = &(noqua_profiles[build - 1]);
    static const uint32_t grid_first[] = {0xa9a97bfd, 0x910003fd, 0xa90573fb, 0x2a0103fb};
    uintptr_t entry = s->bias + p->entry, grid = s->bias + p->grid_entry;
    if (!status_entry_matches(entry, p->first, 16, noqua_original) ||
        !status_entry_matches(grid, grid_first, sizeof grid_first, beat_grid_original)) return false;
    return status_install_entry(entry, ((void *) (noqua_hook)), &noqua_original, install) &&
           status_install_entry(grid, ((void *) (beat_grid_hook)), &beat_grid_original, install);
}

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
static thread_local NoQuaOrigin beat_origin {};
static NoQuaOrigin beat_owner(void *self, int direction, uint32_t beat) noexcept {
    auto &s = state();
    auto slot = processing;
    const uintptr_t owner = reinterpret_cast<uintptr_t>(self);
    if (slot && (direction == 0 || direction == 1) && (beat & 0xffffu) && (beat >> 16) && s.build >= 1 && s.build <= 2 &&
        table(owner, s.bias + noqua_profiles[s.build - 1].owner) && coherent(*slot)) {
        uintptr_t pause = word(slot->facade + 0x58), q = word(slot->facade + 0x50);
        azsdk_load_sample load {};
        if (table(pause, s.profile.play_pause_table) && table(q, s.profile.quantize) &&
            word(owner + 8) == pause && word(pause + 0x58) == slot->core.load(std::memory_order_acquire) &&
            word(owner + 0x10) == q && word(owner + 0x20) == q + 0x20 && azsdk_load_read(int(slot - s.decks), &load))
            return {slot, load.invalidation_epoch, load.load_generation};
    }
    return {};
}
struct BeatOwnerFrame {
    NoQuaOrigin prior;
    BeatOwnerFrame(void *self, int direction, uint32_t beat) noexcept : prior(beat_origin) {
        beat_origin = beat_owner(self, direction, beat);
    }
    ~BeatOwnerFrame() { beat_origin = prior; }
};
static bool noqua_hook(void *self, int direction, uint32_t beat, const void *ref) {
    BeatOwnerFrame frame(self, direction, beat);
    return trampoline<bool (*)(void *, int, uint32_t, const void *)>(&noqua_original)(self, direction, beat, ref);
}
static bool beat_grid_hook(void *self, int direction, uint32_t beat, const void *ref, bool option) {
    BeatOwnerFrame frame(self, direction, beat);
    return trampoline<bool (*)(void *, int, uint32_t, const void *, bool)>(&beat_grid_original)(self, direction, beat, ref, option);
}
static void status_noqua_request(StatusActionScope &out, const void *position, int mode, uintptr_t caller) noexcept {
    auto &s = state();
    auto slot = beat_origin.owner;
    if (!slot || slot != processing || !out.controller || out.callback || mode != 2 || s.build < 1 || s.build > 2) return;
    const auto &p = noqua_profiles[s.build - 1];
    uintptr_t pos = reinterpret_cast<uintptr_t>(position);
    if (!pointer(pos) || (caller != s.bias + p.jump_returns[0] && caller != s.bias + p.jump_returns[1])) return;
    std::memcpy(out.beat_source, reinterpret_cast<const void *>(pos + 8), 12);
    out.beat = azn_load_track_matches(unsigned(slot - s.decks), beat_origin.fence, beat_origin.load, out.beat_source);
    out.beat_sample = word(pos + 0x18);
}
static bool status_install_noqua(State &s, int build, azexit_install_fn install) {
    const auto &p = noqua_profiles[build - 1];
    static constexpr uint32_t grid_first[] = {0xa9a97bfd, 0x910003fd, 0xa90573fb, 0x2a0103fb};
    uintptr_t entry = s.bias + p.entry, grid = s.bias + p.grid_entry;
    if (std::memcmp(reinterpret_cast<const void *>(entry), p.first, 16) ||
        std::memcmp(reinterpret_cast<const void *>(grid), grid_first, sizeof grid_first)) return false;
    return install(entry, reinterpret_cast<void *>(noqua_hook), &noqua_original) && noqua_original &&
           install(grid, reinterpret_cast<void *>(beat_grid_hook), &beat_grid_original) && beat_grid_original;
}

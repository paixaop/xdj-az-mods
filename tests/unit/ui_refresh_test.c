/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
#define OV_WIDGET_VT 0x12345678u
#define OV_IN_TILE 0x280L
static uintptr_t g_ovtile_w[12];
static struct {
    int deck;
} g_ovtile[12];
static int g_ovtile_n, g_ov_refresh;
static uint64_t g_ovtile_cue_load[12], held[4], loaded[4];
static unsigned g_ovtile_cue_deferred, marker_updates;
static uintptr_t player[4][0x208 + 1], cue_bank[4][3], upper[0x12d + 1];
static int message_thread = 1;
static unsigned reads, slots, paints;
static uintptr_t last_paint;
static int unreadable, no_slot;
static int on_msg_thread(void) { return message_thread; }
static uint64_t sdk_command_cue_presentation_hold(unsigned deck) { return held[deck]; }
static int sdk_command_cue_presentation_current(unsigned deck, uint64_t load) { return loaded[deck] == load; }
static uintptr_t perdeck_block(int deck) { return (uintptr_t) player[deck]; }
static bool marker_update(void *self) { assert(self == upper); ++marker_updates; return true; }
static bool (*g_orig_overview_cue_update)(void *) = marker_update;
static bool (*g_orig_detail_cue_update)(void *) = marker_update;
static unsigned marker_installs;
static int signatures_match = 1;
static void native_overview_update(uintptr_t widget) {
    assert(widget + OV_IN_TILE == g_ovtile_w[0]);
    ++paints;
}
static uintptr_t ep_off(int older, int newer) {
    return older == 0x1c16aa0 ? (uintptr_t) native_overview_update : (uintptr_t) newer;
}
static int own_q(uintptr_t widget, uintptr_t *word) {
    assert(widget + OV_IN_TILE == g_ovtile_w[0]);
    *word = 0x2f1a3b8;
    return 1;
}
static int img_q(uintptr_t addr, uintptr_t *word) {
    if (addr == 0x2f1a3b8 + 5 * sizeof(uintptr_t)) {
        *word = (uintptr_t) native_overview_update;
        return 1;
    }
    *word = addr == 0x1d73808 || addr == 0x1cf6d30 ? UINT64_C(0x910003fda9be7bfd) : UINT64_C(0xa90153f3f944b401);
    return signatures_match;
}
static int azm_install_hook(uintptr_t addr, void *hook, void **original) {
    assert(addr && hook && original);
    ++marker_installs;
    return 1;
}
static uintptr_t wref_get(const uintptr_t *slot) { return *slot; }
static int own_u32(uintptr_t object, unsigned *out) {
    reads++;
    if (unreadable) return 0;
    memcpy(out, (void *) object, sizeof *out);
    return 1;
}
static int ov_slot_for(long object, int deck) {
    assert(object && deck >= 0 && deck < 4);
    slots++;
    return no_slot ? -1 : 0;
}
static void ov_repaint(long object) {
    paints++;
    last_paint = (uintptr_t) object;
}
#include ROUTING_IMPL_INC

int main(void) {
    assert(install_cue_marker_display() && marker_installs == 2);
    signatures_match = 0;
    assert(!install_cue_marker_display() && marker_installs == 2);
    unsigned widget = OV_WIDGET_VT, wrong_type = 0;
    g_ovtile_n = 2;
    g_ovtile[0].deck = 1;
    g_ovtile[1].deck = 2;
    g_ovtile_w[1] = (uintptr_t) &widget;
    ov_refresh_tile(0, 15); /* retired weak slot */
    assert(!reads && !slots && !paints);
    g_ovtile_w[0] = (uintptr_t) &widget;
    g_ovtile[0].deck = -1;
    ov_refresh_tile(0, 15);
    g_ovtile[0].deck = 4;
    ov_refresh_tile(0, 15);
    g_ovtile[0].deck = 1;
    ov_refresh_tile(0, 1);
    assert(!reads && !slots && !paints); /* no dereference outside the requested deck */
    unreadable = 1;
    ov_refresh_tile(0, 2);
    assert(reads == 1 && !slots && !paints);
    unreadable = 0;
    g_ovtile_w[0] = (uintptr_t) &wrong_type;
    ov_refresh_tile(0, 2);
    assert(reads == 2 && !slots && !paints);
    g_ovtile_w[0] = (uintptr_t) &widget;
    no_slot = 1;
    ov_refresh_tile(0, 2);
    assert(reads == 3 && slots == 1 && !paints);
    no_slot = 0;
    g_ov_refresh = 2;
    ov_refresh_if_needed();
    assert(!g_ov_refresh && reads == 4 && slots == 2 && paints == 1);
    assert(last_paint == (uintptr_t) &widget);
    ov_refresh_if_needed();
    assert(reads == 4 && paints == 1); /* a consumed refresh does not repaint twice */
    g_ov_refresh = 6;
    ov_refresh_if_needed();
    assert(reads == 6 && slots == 4 && paints == 3 && !g_ov_refresh);
    held[1] = loaded[1] = 17;
    assert(ov_cue_defer((long) &widget));
    assert(ov_cue_defer((long) &widget));
    ov_refresh_if_needed();
    assert(paints == 3);
    held[1] = 0;
    ov_refresh_if_needed();
    ov_refresh_if_needed();
    assert(paints == 4 && !g_ovtile_cue_deferred);
    held[1] = 17;
    assert(ov_cue_defer((long) &widget));
    held[1] = 0;
    loaded[1] = 18;
    ov_refresh_if_needed();
    assert(paints == 4 && !g_ovtile_cue_deferred); /* newer load owns its own render */
    cue_bank[1][0] = upper[0x958 / sizeof(uintptr_t)] = 0x10;
    cue_bank[1][2] = upper[0x968 / sizeof(uintptr_t)] = 0x20;
    player[1][0x1040 / sizeof(uintptr_t)] = (uintptr_t) cue_bank[1];
    held[1] = 18;
    assert(!overview_cue_update_hook(upper) && !detail_cue_update_hook(upper) && !marker_updates);
    upper[0x958 / sizeof(uintptr_t)] = 0x30;
    assert(overview_cue_update_hook(upper) && marker_updates == 1);
    upper[0x958 / sizeof(uintptr_t)] = 0x10;
    held[1] = 0;
    assert(overview_cue_update_hook(upper) && detail_cue_update_hook(upper) && marker_updates == 3);
    puts("PASS overview refresh: weak lifetime, native type, capacity and per-deck consumption");
    return 0;
}

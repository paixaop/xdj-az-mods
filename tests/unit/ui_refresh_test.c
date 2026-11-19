/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
#define OV_WIDGET_VT 0x12345678u
static uintptr_t g_ovtile_w[12];
static struct {
    int deck;
} g_ovtile[12];
static int g_ovtile_n, g_ov_refresh;
static unsigned reads, slots, paints;
static uintptr_t last_paint;
static int unreadable, no_slot;
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
    puts("PASS overview refresh: weak lifetime, native type, capacity and per-deck consumption");
    return 0;
}

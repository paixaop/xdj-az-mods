/* SPDX-License-Identifier: GPL-3.0-only */
/* Message-thread native frame ownership. Included after the waveform census.
 * The update entry is only a candidate: publish after native rendering returns,
 * never after a deduplicated call. The visible image and current native load
 * fence must still match when the view is painted. */
#ifndef VOCAL_LINES_FRAME_H
#define VOCAL_LINES_FRAME_H
#include "sdk_status_native.h"
#include <math.h>
#include <stdatomic.h>

struct vl_frame {
    struct azstatus_native_receipt load;
    uintptr_t widget, image;
    double position_ms, zoom;
    int deck, slot, mode, center, width, valid, rendered;
};
enum {
    VL_FRAME_DEPTH = 8
};
static struct vl_frame vl_frames[4], vl_pending[VL_FRAME_DEPTH];
static unsigned vl_depth;
static int vx_wave_on(void);

static int vl_load(unsigned deck, struct azstatus_native_receipt *load) {
    if (deck >= 4 || !ds_native_load_seed(deck, load)) return 0;
    return load->a == AZN_LOADED && load->unit && load->epoch && load->load_generation;
}
static int vl_same_load(unsigned deck, const struct azstatus_native_receipt *before) {
    struct azstatus_native_receipt after;
    if (!vl_load(deck, &after)) return 0;
    return after.epoch == before->epoch && after.unit == before->unit &&
           after.load_generation == before->load_generation;
}
static int vl_slot(uintptr_t wwb, int deck) {
    int n = atomic_load_explicit(&g_wwbbase_n, memory_order_relaxed);
    for (int i = 0; i < n && i < 8; ++i)
        if (g_wwbbase[i].base == wwb && g_wwbbase[i].deck == deck) return i;
    return -1;
}
static uintptr_t vl_visible_widget(int slot, int *mode) {
    for (int m = 0; m < 3; ++m) {
        uintptr_t w = wwb_widget_live(slot, m);
        if (!w) continue;
        if (!(*(volatile uint32_t *) (w + WIDGET_COMPONENT_OFF + 0xc0) & 2)) continue;
        *mode = m;
        return w;
    }
    return 0;
}
static int vl_frame_input(struct vl_frame *f, int deck, uintptr_t wwb, uintptr_t position,
                          float zoom) {
    if ((unsigned) deck >= 4 || position < 0x10000 || (position & 7)) return 0;
    if (!isfinite(zoom) || zoom <= 0) return 0;
    f->deck = deck;
    f->slot = vl_slot(wwb, deck);
    if (f->slot < 0 || !vl_load((unsigned) deck, &f->load)) return 0;
    f->position_ms = (double) *(const volatile int64_t *) position;
    f->zoom = zoom < 0.01f ? 0.01 : (double) zoom;
    return 1;
}
static void vx_upd_begin(int deck, long wwb, long position, float zoom) {
    unsigned index = vl_depth++;
    if (index >= VL_FRAME_DEPTH) return;
    struct vl_frame *f = &vl_pending[index];
    *f = (struct vl_frame) {.deck = deck};
    f->valid = vx_wave_on() && vl_frame_input(f, deck, (uintptr_t) wwb, (uintptr_t) position, zoom);
}
/* Called only after the original renderWaveform_new returned. If that hook is
 * unavailable, no candidate is accepted merely because an old image exists. */
static void vx_upd_rendered(void) {
    if (vl_depth && vl_depth <= VL_FRAME_DEPTH) vl_pending[vl_depth - 1].rendered = 1;
}
static int vl_frame_rendered(struct vl_frame *f) {
    f->widget = vl_visible_widget(f->slot, &f->mode);
    if (!f->widget || !vl_same_load((unsigned) f->deck, &f->load)) return 0;
    uintptr_t wwb = g_wwbbase[f->slot].base;
    f->image = *(volatile uintptr_t *) (f->widget + 0x108);
    f->center = *(volatile int *) (wwb + 0x197c8);
    f->width = *(volatile int *) (wwb + 0x60);
    if (!f->image || f->width <= 0 || f->width > 1280) return 0;
    return f->center >= 0 && f->center <= f->width;
}
static void vx_upd_commit(int skipped) {
    if (!vl_depth) return;
    unsigned index = --vl_depth;
    if (index >= VL_FRAME_DEPTH || skipped) return;
    struct vl_frame *f = &vl_pending[index];
    if ((unsigned) f->deck >= 4) return;
    if (!f->rendered) return;
    vl_frames[f->deck].valid = 0;
    if (!f->valid || !vl_frame_rendered(f)) return;
    vl_frames[f->deck] = *f;
}
static int vl_frame_get(unsigned deck, struct vl_frame *out) {
    if (deck >= 4 || !vl_frames[deck].valid) return 0;
    const struct vl_frame *f = &vl_frames[deck];
    int mode = -1;
    uintptr_t widget = vl_visible_widget(f->slot, &mode);
    if (widget != f->widget || mode != f->mode) return 0;
    if (*(volatile uintptr_t *) (widget + 0x108) != f->image) return 0;
    if (!vl_same_load(deck, &f->load)) return 0;
    *out = *f;
    return 1;
}
#endif

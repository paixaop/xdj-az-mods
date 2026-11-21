/* SPDX-License-Identifier: GPL-3.0-only */
/* Vocal Lines, included by extras.c on the native message thread. Track
 * Analysis supplies cached, consolidated intervals through vocal_lines.h.
 * No file access, analysis, allocation, or blocking worker lock runs here. */
#include "vocal_lines.h"
#include "vocal_lines_frame.h"
#include "vocal_lines_phrase.h"

#define VX_H 4
#define VX_WTOP 25
#define VX_MAXW 1280
#define VX_RGB 0x5cb8ffu
#define VX_RGB_LIGHT 0x1060c8u
#define VX_DIF_VT EP_2d0b700
#define VX_DIF2_VT EP_2cee580
#define VX_DIF4_VT EP_2cfe3a8
#define VX_PANEL_W 172

static int vx_wave_on(void) {
    return az_on(AZF_VOCAL_LINES_WAVE) && az_active(AZF_VOCAL_LINES_WAVE);
}
static int vx_phrases_on(void) {
    return az_on(AZF_VOCAL_LINES_PHRASES) && az_active(AZF_VOCAL_LINES_PHRASES);
}
static unsigned vx_color(void) {
#ifdef AZMOD_HAVE_DAYMODE
    if (atomic_load_explicit(&g_dm_on, memory_order_relaxed) && dm_flip_on()) return VX_RGB_LIGHT;
#endif
    return VX_RGB;
}
static void vx_bar(void *g, int x, int y, int w, int h, uint32_t argb) {
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 1;
#endif
    ((void (*)(void *, uint32_t *)) EP_23af140)(g, &argb);
    ((void (*)(void *, int, int, int, int)) EP_23af680)(g, x, y, w, h);
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 0;
#endif
}
static void vx_strip(void *g, const int r[4], const uint8_t *pixels) {
    uint32_t color = 0xff000000u | vx_color();
    for (int s = 0; s < r[2];) {
        int e = s + 1;
        while (e < r[2] && pixels[e] == pixels[s]) ++e;
        if (pixels[s]) vx_bar(g, r[0] + s, r[1], e - s, r[3], color);
        s = e;
    }
}
/* The PlayerInfo panel paints the row outside the nested waveform group. */
static void vx_paint(void *g, uintptr_t panel, int deck) {
    struct azstatus_native_receipt load;
    int r[4], panel_rect[4];
    double duration;
    if (!g || !vx_phrases_on() || !vl_load((unsigned) deck, &load)) return;
    uint32_t rate = vocal_lines_sample_rate((unsigned) deck, load.load_generation);
    if (!vl_phrase_geometry(panel, r, &duration, rate) || !vlp_screen(panel, panel_rect)) return;
    uint8_t pixels[VX_MAXW];
    if (!vocal_lines_pixels((unsigned) deck, load.load_generation, 0, duration, r[2], pixels)) return;
    if (!vl_same_load((unsigned) deck, &load)) return;
    r[0] -= panel_rect[0];
    r[1] -= panel_rect[1];
    vx_strip(g, r, pixels);
}
static uintptr_t vx_view_parent(uintptr_t panel) {
    for (int k = 0; k < 8 && ht_ptr_ok(panel); ++k) {
        if (*(volatile uintptr_t *) panel == PM_VIEW_VT) return panel;
        panel = *(volatile uintptr_t *) (panel + 0x18);
    }
    return 0;
}
static void vx_title(uintptr_t panel, int deck) {
    static uint64_t revisions[4];
    static unsigned enabled[4];
    if ((unsigned) deck >= 4 || !ht_ptr_ok(panel)) return;
    uint64_t revision = vocal_lines_revision((unsigned) deck);
    unsigned flags = (unsigned) vx_wave_on() | ((unsigned) vx_phrases_on() << 1);
    if (revisions[deck] == revision && enabled[deck] == flags) return;
    revisions[deck] = revision;
    enabled[deck] = flags;
    ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) panel);
    uintptr_t view = vx_view_parent(panel);
    if (view) ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) view);
}
static uintptr_t vx_full_vt(uintptr_t component) {
    uintptr_t vt = *(volatile uintptr_t *) component;
    if (!ht_ptr_ok(vt)) return 0;
    uintptr_t full = component + (uintptr_t) *(volatile intptr_t *) (vt - 0x10);
    return ht_ptr_ok(full) ? *(volatile uintptr_t *) full : 0;
}
static void vx_cut_rect(const int r[4], int playhead, int *left, int *right) {
    if (r[0] + r[2] <= playhead) {
        if (r[0] + r[2] > *left) *left = r[0] + r[2];
    } else if (r[0] >= playhead) {
        if (r[0] < *right) *right = r[0];
    } else {
        *right = *left;
    }
}
static int vx_intersects(const int r[4], int y, int left, int right) {
    return r[1] < y + VX_H && r[1] + r[3] > y && r[0] < right && r[0] + r[2] > left;
}
static int vx_container(uintptr_t vt) {
    return vt == VX_DIF_VT || vt == VX_DIF2_VT || vt == VX_DIF4_VT;
}
static void vx_cut(uintptr_t, int, int, int *, int *, int, int *);
static void vx_cut_children(uintptr_t component, int y, int playhead, int *left, int *right,
                            int depth, int *budget) {
    uintptr_t children = *(volatile uintptr_t *) (component + 0x40);
    int count = *(volatile int *) (component + 0x50);
    if (count < 0 || count > 64 || (count && !ht_ptr_ok(children))) {
        *right = *left;
        return;
    }
    for (int k = 0; k < count && *budget >= 0; ++k)
        vx_cut(*(volatile uintptr_t *) (children + 8 * k), y, playhead, left, right, depth - 1, budget);
}
static int vx_cut_visible(uintptr_t component, int y, int *left, int *right, int r[4], int *budget) {
    if (--*budget < 0 || !ht_ptr_ok(component)) {
        *right = *left;
        return 0;
    }
    return vlp_screen(component, r) && vx_intersects(r, y, *left, *right);
}
static void vx_cut(uintptr_t component, int y, int playhead, int *left, int *right,
                   int depth, int *budget) {
    int r[4];
    if (!vx_cut_visible(component, y, left, right, r, budget)) return;
    uintptr_t vt = vx_full_vt(component);
    int panel = vt == KT_DI2_VT || vt == KT_DI4_VT;
    if (depth <= 0 || !(vx_container(vt) || panel)) {
        vx_cut_rect(r, playhead, left, right);
        return;
    }
    if (panel) {
        if (r[2] > VX_PANEL_W) r[2] = VX_PANEL_W;
        vx_cut_rect(r, playhead, left, right);
    }
    vx_cut_children(component, y, playhead, left, right, depth, budget);
}
static int vx_row_inside(const int r[4], int y) {
    return r[1] <= y && (int64_t) r[1] + r[3] >= (int64_t) y + VX_H;
}
static uintptr_t vx_ancestor_clip(uintptr_t box, uintptr_t view, int y, int *left, int *right) {
    for (int depth = 0; depth < 8; ++depth) {
        int r[4];
        if (!vlp_screen(box, r) || !vx_row_inside(r, y)) return 0;
        if (r[0] > *left) *left = r[0];
        if (r[0] + r[2] < *right) *right = r[0] + r[2];
        uintptr_t parent = *(volatile uintptr_t *) (box + 0x18);
        if (parent == view) return box;
        box = parent;
    }
    return 0;
}
static int vx_sibling_clip(uintptr_t view, uintptr_t lane, int y, int playhead, int *left, int *right) {
    uintptr_t children = *(volatile uintptr_t *) (view + 0x40);
    int count = *(volatile int *) (view + 0x50);
    if (!ht_ptr_ok(children) || count < 0 || count > 64) return 0;
    int budget = 512;
    for (int k = 0; k < count; ++k) {
        uintptr_t child = *(volatile uintptr_t *) (children + 8 * k);
        if (child != lane) vx_cut(child, y, playhead, left, right, 4, &budget);
    }
    return *right - *left >= 1;
}
static uintptr_t vx_frame_box(const struct vl_frame *frame, int box_rect[4], int *playhead) {
    uintptr_t component = frame->widget + WIDGET_COMPONENT_OFF;
    int r[4];
    if (!vlp_screen(component, r) || r[2] != frame->width) return 0;
    uintptr_t box = *(volatile uintptr_t *) (component + 0x18);
    if (!ht_ptr_ok(box) || *(volatile uintptr_t *) box != PM_WAVEBOX_VT) return 0;
    if (!vlp_screen(box, box_rect) || box_rect[3] < VX_WTOP + VX_H) return 0;
    *playhead = r[0] + frame->center;
    return box;
}
static int vx_wave_rect(uintptr_t view, const struct vl_frame *frame, int rect[4], int *playhead) {
    int box_rect[4];
    uintptr_t box = vx_frame_box(frame, box_rect, playhead);
    if (!box) return 0;
    int left = box_rect[0], right = left + box_rect[2], y = box_rect[1] + VX_WTOP;
    uintptr_t lane = vx_ancestor_clip(box, view, y, &left, &right);
    if (!lane || !vx_sibling_clip(view, lane, y, *playhead, &left, &right)) return 0;
    if (right - left > VX_MAXW) return 0;
    rect[0] = left;
    rect[1] = y;
    rect[2] = right - left;
    rect[3] = VX_H;
    return 1;
}
static void vx_wave_deck(uintptr_t view, void *g, const int view_rect[4], unsigned deck) {
    struct vl_frame frame;
    int r[4], playhead;
    if (!vl_frame_get(deck, &frame) || !vx_wave_rect(view, &frame, r, &playhead)) return;
    double scale = 150.0 * frame.zoom;
    double start = frame.position_ms / 1000.0 + (r[0] - playhead) / scale;
    double stop = start + r[2] / scale;
    uint8_t pixels[VX_MAXW];
    if (!vocal_lines_pixels(deck, frame.load.load_generation, start, stop, r[2], pixels)) return;
    if (!vl_same_load(deck, &frame.load)) return;
    r[0] -= view_rect[0];
    r[1] -= view_rect[1];
    vx_strip(g, r, pixels);
}
static void vx_wave_paint(uintptr_t view, void *g) {
    int r[4];
    if (!g || !vx_wave_on() || pm_trackinfo_open(view) || !vlp_screen(view, r)) return;
    for (unsigned deck = 0; deck < 4; ++deck) vx_wave_deck(view, g, r, deck);
}

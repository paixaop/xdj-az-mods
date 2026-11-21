/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_VOCAL_LINES_PHRASE_H
#define AZMOD_VOCAL_LINES_PHRASE_H
#include <limits.h>
#include <stdint.h>
#include "../common/aztimestamp.h"

/* Message-thread only, inside a live PlayerInfo panel paint. No native pointer
 * escapes this call. The caller separately binds the vocal intervals to the
 * current load and supplies that source's sample rate.
 *
 * PhraseMeterWidget's Component is at +0x78 on both supported builds. Native
 * paint (1.30 0x1bff1e0 / 2.00 0x1d68968) uses its shared geometry's width/+8
 * inset and duration/+0x68. The update copies duration from model/+0xe8.
 * The two-pixel row lies outside the waveform group, inside the panel border;
 * draw it from the panel's paintOverChildren, using returned SCREEN coordinates.
 */
#define VLP_PRIMARY_VT ((uintptr_t) ep_off(0x2b92aa8, 0x2f0c9d8))
#define VLP_COMPONENT_VT ((uintptr_t) ep_off(0x2b92c78, 0x2f0cba8))
#define VLP_COMPONENT_OFFSET 0x78
#define VLP_HEIGHT 2

static int vlp_coordinates(int64_t x, int64_t y, int r[4]) {
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX)
        return 0;
    r[0] = (int) x;
    r[1] = (int) y;
    return 1;
}

/* Accumulate in 64 bits before narrowing, including malformed native bounds. */
static int vlp_screen(uintptr_t component, int r[4]) {
    int64_t x = 0, y = 0;
    uintptr_t current = component;
    if (!ht_ptr_ok(component))
        return 0;
    r[2] = *(volatile int *) (component + 0x28);
    r[3] = *(volatile int *) (component + 0x2c);
    for (int depth = 0; current && depth < 32; depth++) {
        if (!ht_ptr_ok(current) || !(*(volatile uint32_t *) (current + 0xc0) & 2))
            return 0;
        x += *(volatile int *) (current + 0x20);
        y += *(volatile int *) (current + 0x24);
        current = *(volatile uintptr_t *) (current + 0x18);
    }
    if (current)
        return 0;
    return vlp_coordinates(x, y, r);
}

static int vlp_children(uintptr_t component, uintptr_t *children) {
    int count = *(volatile int *) (component + 0x50);
    int allocated = *(volatile int *) (component + 0x48);
    *children = *(volatile uintptr_t *) (component + 0x40);
    if (count < 0 || count > 64 || allocated < count)
        return -1;
    if (count && !ht_ptr_ok(*children))
        return -1;
    return count;
}

static int vlp_component(uintptr_t component) {
    if (*(volatile uintptr_t *) component != VLP_COMPONENT_VT)
        return 0;
    uintptr_t full = component - VLP_COMPONENT_OFFSET;
    return ht_ptr_ok(full) && *(volatile uintptr_t *) full == VLP_PRIMARY_VT;
}

struct vlp_search {
    uintptr_t found;
    int budget;
};

static int vlp_find(uintptr_t component, int depth, struct vlp_search *search);

static int vlp_find_children(uintptr_t component, int depth, struct vlp_search *search) {
    uintptr_t children;
    int count = vlp_children(component, &children);
    if (count < 0)
        return 0;
    for (int i = 0; i < count; i++) {
        uintptr_t child = *(volatile uintptr_t *) (children + (uintptr_t) i * 8);
        if (!ht_ptr_ok(child) || *(volatile uintptr_t *) (child + 0x18) != component)
            return 0;
        if (!vlp_find(child, depth - 1, search))
            return 0;
    }
    return 1;
}

/* Native panels have one visible phrase widget. An ambiguous or malformed tree
 * fails closed; the shared budget also bounds repeated/cyclic child references. */
static int vlp_find(uintptr_t component, int depth, struct vlp_search *search) {
    int ignored[4];
    if (--search->budget < 0 || !ht_ptr_ok(component))
        return 0;
    if (!(*(volatile uint32_t *) (component + 0xc0) & 2))
        return 1;
    if (!vlp_screen(component, ignored))
        return 0;
    if (vlp_component(component)) {
        if (search->found)
            return 0;
        search->found = component;
        return 1;
    }
    if (!depth)
        return 1;
    return vlp_find_children(component, depth, search);
}

static uintptr_t vlp_view(uintptr_t panel) {
    uintptr_t current = panel;
    for (int depth = 0; ht_ptr_ok(current) && depth < 16; depth++) {
        if (*(volatile uintptr_t *) current == PM_VIEW_VT)
            return current;
        current = *(volatile uintptr_t *) (current + 0x18);
    }
    return 0;
}

static uintptr_t vlp_panel_view(uintptr_t panel) {
    if (!ht_ptr_ok(panel))
        return 0;
    uintptr_t type = *(volatile uintptr_t *) panel;
    if (type != HT_PI2D_CVT && type != HT_PI4D_CVT)
        return 0;
    uintptr_t view = vlp_view(panel);
    if (!view || pm_trackinfo_open(view))
        return 0;
    return view;
}

static int vlp_duration(uintptr_t full, uint32_t sample_rate, double *seconds) {
    uintptr_t model = *(volatile uintptr_t *) (full + 0x58);
    int64_t cached = *(volatile int64_t *) (full + 0x68);
    if (!ht_ptr_ok(model) || cached <= 0 || cached > 3600000)
        return 0;
    int64_t current = *(volatile int64_t *) (model + 0xe8);
    int order = compare_timestamps(cached, current, 1000, sample_rate, 1);
    if (order == AZ_TIME_INVALID || order != AZ_TIME_EQUAL)
        return 0;
    *seconds = (double) cached / 1000.0;
    return 1;
}

static int vlp_axis(uintptr_t component, int r[4]) {
    uintptr_t geometry = *(volatile uintptr_t *) (component - VLP_COMPONENT_OFFSET + 0x40);
    int bounds[4];
    if (!ht_ptr_ok(geometry) || !vlp_screen(component, bounds))
        return 0;
    int width = *(volatile int *) geometry;
    int inset = *(volatile int *) (geometry + 8);
    if (width <= 0 || inset < 0 || (int64_t) inset + width > bounds[2] || bounds[3] <= 0)
        return 0;
    int64_t x = (int64_t) bounds[0] + inset, y = (int64_t) bounds[1] + bounds[3];
    if (x > INT_MAX || y > INT_MAX)
        return 0;
    r[0] = (int) x;
    r[1] = (int) y;
    r[2] = width;
    r[3] = VLP_HEIGHT;
    return 1;
}

static int vlp_inside(const int outer[4], const int inner[4], int border) {
    return inner[0] >= outer[0] && inner[1] >= outer[1] &&
           (int64_t) inner[0] + inner[2] <= (int64_t) outer[0] + outer[2] &&
           (int64_t) inner[1] + inner[3] <= (int64_t) outer[1] + outer[3] - border;
}

static int vlp_intersects(const int a[4], const int b[4]) {
    return a[2] > 0 && a[3] > 0 && b[2] > 0 && b[3] > 0 &&
           (int64_t) a[0] < (int64_t) b[0] + b[2] &&
           (int64_t) b[0] < (int64_t) a[0] + a[2] &&
           (int64_t) a[1] < (int64_t) b[1] + b[3] &&
           (int64_t) b[1] < (int64_t) a[1] + a[3];
}

static int vlp_scan_siblings(uintptr_t parent, uintptr_t branch, const int r[4]) {
    uintptr_t children;
    int count = vlp_children(parent, &children);
    if (count < 0)
        return 0;
    for (int i = 0; i < count; i++) {
        uintptr_t child = *(volatile uintptr_t *) (children + (uintptr_t) i * 8);
        int bounds[4];
        if (child == branch)
            continue;
        if (!ht_ptr_ok(child))
            return 0;
        if (*(volatile uintptr_t *) (child + 0x18) != parent)
            return 0;
        if (!(*(volatile uint32_t *) (child + 0xc0) & 2))
            continue;
        if (!vlp_screen(child, bounds) || vlp_intersects(bounds, r))
            return 0;
    }
    return 1;
}

static int vlp_clear_siblings(uintptr_t parent, uintptr_t branch, const int r[4]) {
    int bounds[4];
    if (!vlp_screen(parent, bounds))
        return 0;
    /* JUCE clips every child to its parent. The native phrase-adjacent
     * decoration extends below the waveform group, but those pixels never
     * reach the row painted by the enclosing PlayerInfo panel. */
    if (!vlp_intersects(bounds, r))
        return 1;
    return vlp_scan_siblings(parent, branch, r);
}

static int vlp_path(uintptr_t component, uintptr_t panel, uintptr_t view, const int r[4]) {
    int enclosing = 0;
    for (int depth = 0; ht_ptr_ok(component) && depth < 16; depth++) {
        int bounds[4];
        if (!vlp_screen(component, bounds))
            return 0;
        enclosing |= component == panel;
        if (enclosing && !vlp_inside(bounds, r, component == panel))
            return 0;
        if (component == view)
            return enclosing;
        uintptr_t parent = *(volatile uintptr_t *) (component + 0x18);
        if (!ht_ptr_ok(parent) || !vlp_clear_siblings(parent, component, r))
            return 0;
        component = parent;
    }
    return 0;
}

/* Full-axis containment is required: clipping its width would change the
 * time-to-x scale because the returned duration covers the entire track. */
static int vlp_geometry(uintptr_t panel, uintptr_t view, int rect[4], double *duration_s,
                        uint32_t sample_rate) {
    struct vlp_search search = {0, 512};
    if (!vlp_find(panel, 4, &search) || !search.found)
        return 0;
    int candidate[4];
    double duration;
    if (!vlp_axis(search.found, candidate))
        return 0;
    if (!vlp_path(search.found, panel, view, candidate))
        return 0;
    if (!vlp_duration(search.found - VLP_COMPONENT_OFFSET, sample_rate, &duration))
        return 0;
    for (int i = 0; i < 4; i++)
        rect[i] = candidate[i];
    *duration_s = duration;
    return 1;
}

static int vl_phrase_geometry(uintptr_t panel, int rect[4], double *duration_s,
                              uint32_t sample_rate) {
    if (!rect || !duration_s)
        return 0;
    uintptr_t view = vlp_panel_view(panel);
    return view && vlp_geometry(panel, view, rect, duration_s, sample_rate);
}
#endif

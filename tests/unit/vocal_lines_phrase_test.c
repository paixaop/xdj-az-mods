/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int firmware, modal;
static int ep_off(int old, int current) { return firmware == 2 ? current : old; }
static int ht_ptr_ok(uintptr_t p) { return p >= 0x10000 && !(p & 7); }
#define HT_PI2D_CVT ((uintptr_t) 0x70100)
#define HT_PI4D_CVT ((uintptr_t) 0x70200)
#define PM_VIEW_VT ((uintptr_t) 0x70300)
static int pm_trackinfo_open(uintptr_t view) {
    assert(view);
    return modal;
}
#include "../../src/azmod/vocal_lines_phrase.h"

union native_object {
    uint64_t alignment;
    unsigned char bytes[0x300];
};
enum {
    VIEW,
    FIELD,
    PANEL,
    GROUP,
    PHRASE,
    OVERLAY,
    EXTRA,
    COUNT
};
static union native_object objects[COUNT], model, shared;
static uintptr_t children[COUNT][65];
static uintptr_t node[COUNT];

static void put_int(uintptr_t p, size_t offset, int value) {
    memcpy((void *) (p + offset), &value, sizeof value);
}
static void put_ptr(uintptr_t p, size_t offset, uintptr_t value) {
    memcpy((void *) (p + offset), &value, sizeof value);
}
static void put_time(uintptr_t p, size_t offset, int64_t value) {
    memcpy((void *) (p + offset), &value, sizeof value);
}
static void bounds(int which, int x, int y, int w, int h) {
    put_int(node[which], 0x20, x);
    put_int(node[which], 0x24, y);
    put_int(node[which], 0x28, w);
    put_int(node[which], 0x2c, h);
}
static void link_child(int parent, int child) {
    int count;
    memcpy(&count, (void *) (node[parent] + 0x50), sizeof count);
    children[parent][count] = node[child];
    put_int(node[parent], 0x50, count + 1);
    put_ptr(node[child], 0x18, node[parent]);
}
static uintptr_t phrase_full(void) { return node[PHRASE] - VLP_COMPONENT_OFFSET; }

static void fixture(int compact, int right, int bottom) {
    memset(objects, 0, sizeof objects);
    memset(children, 0, sizeof children);
    memset(&model, 0, sizeof model);
    memset(&shared, 0, sizeof shared);
    modal = 0;
    for (int i = 0; i < COUNT; i++) {
        node[i] = (uintptr_t) &objects[i];
        if (i == PHRASE) node[i] += VLP_COMPONENT_OFFSET;
        put_ptr(node[i], 0, 0x70400);
        put_int(node[i], 0xc0, 2);
        put_ptr(node[i], 0x40, (uintptr_t) children[i]);
        put_int(node[i], 0x48, 65);
    }
    put_ptr(node[VIEW], 0, PM_VIEW_VT);
    put_ptr(node[PANEL], 0, compact ? HT_PI4D_CVT : HT_PI2D_CVT);
    put_ptr(node[PHRASE], 0, VLP_COMPONENT_VT);
    put_ptr(phrase_full(), 0, VLP_PRIMARY_VT);
    put_ptr(phrase_full(), 0x40, (uintptr_t) &shared);
    put_ptr(phrase_full(), 0x58, (uintptr_t) &model);
    put_time(phrase_full(), 0x68, 240123);
    put_time((uintptr_t) &model, 0xe8, 240123);
    put_int((uintptr_t) &shared, 0, compact ? 368 : 590);
    put_int((uintptr_t) &shared, 8, 8);
    bounds(VIEW, 0, 0, 1280, 800);
    bounds(FIELD, 0, 560, 1280, 240);
    bounds(PANEL, right ? 650 : 10, bottom ? 120 : 0, 620, compact ? 115 : 235);
    bounds(GROUP, compact ? 125 : 7, compact ? 41 : 147, compact ? 385 : 607,
           compact ? 71 : 85);
    bounds(PHRASE, 0, compact ? 58 : 72, compact ? 385 : 607, 13);
    link_child(VIEW, FIELD);
    link_child(FIELD, PANEL);
    link_child(PANEL, GROUP);
    link_child(GROUP, PHRASE);
}

static void success(int x, int y, int width) {
    int rect[4] = {0};
    double duration = 0;
    assert(vl_phrase_geometry(node[PANEL], rect, &duration, 48000));
    assert(rect[0] == x && rect[1] == y && rect[2] == width && rect[3] == 2);
    assert(compare_timestamps(duration, 240.123, 1, 48000, 1) == AZ_TIME_EQUAL);
}
static void rejected(uint32_t rate) {
    int rect[4] = {1, 2, 3, 4};
    double duration = 12;
    assert(!vl_phrase_geometry(node[PANEL], rect, &duration, rate));
    assert(rect[0] == 1 && rect[1] == 2 && rect[2] == 3 && rect[3] == 4);
    assert(compare_timestamps(duration, 12.0, 1, 48000, 1) == AZ_TIME_EQUAL);
}

static void layouts(void) {
    for (firmware = 1; firmware <= 2; firmware++) {
        fixture(0, 0, 0);
        success(25, 792, 590);
        fixture(0, 1, 0);
        success(665, 792, 590);
        fixture(1, 0, 0);
        success(143, 672, 368);
        fixture(1, 1, 0);
        success(783, 672, 368);
        fixture(1, 0, 1);
        success(143, 792, 368);
        fixture(1, 1, 1);
        success(783, 792, 368);
        bounds(VIEW, 7, -3, 1280, 800);
        success(790, 789, 368);
    }
    firmware = 2;
}
static void visibility(void) {
    for (int i = VIEW; i <= PHRASE; i++) {
        fixture(0, 0, 0);
        put_int(node[i], 0xc0, 0);
        rejected(48000);
    }
    fixture(0, 0, 0);
    modal = 1;
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[VIEW], 0, 0x77700);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[PANEL], 0, 0x77700);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[PHRASE], 0, 0x77700);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(phrase_full(), 0, 0x77700);
    rejected(48000);
}
static void obstacles(void) {
    fixture(0, 0, 0);
    bounds(OVERLAY, 20, 230, 100, 5);
    link_child(PANEL, OVERLAY);
    rejected(48000);
    put_int(node[OVERLAY], 0xc0, 0);
    success(25, 792, 590);
    put_int(node[OVERLAY], 0xc0, 2);
    bounds(OVERLAY, 20, 100, 100, 5);
    success(25, 792, 590);
    bounds(OVERLAY, 30, 230, 0, 5);
    success(25, 792, 590);
    fixture(0, 0, 0);
    bounds(OVERLAY, 0, 0, 1280, 240);
    link_child(FIELD, OVERLAY);
    rejected(48000); /* replacing track-info panel */
    fixture(0, 0, 0);
    /* Retained 2.00 native sibling: raw bounds cross the phrase's bottom,
     * but the waveform group's clip ends at screen y792. */
    bounds(OVERLAY, 0, 72, 607, 18);
    link_child(GROUP, OVERLAY);
    success(25, 792, 590);
    bounds(GROUP, 7, 147, 607, 87);
    rejected(48000);
    fixture(0, 0, 0);
    bounds(PANEL, 10, 0, 620, 234);
    rejected(48000);
    fixture(0, 0, 0);
    bounds(FIELD, 0, 560, 600, 240);
    rejected(48000);
    fixture(0, 0, 0);
    bounds(VIEW, 0, 0, 1280, 793);
    rejected(48000);
}
static void clocks(void) {
    fixture(0, 0, 0);
    rejected(0);
    put_time((uintptr_t) &model, 0xe8, 240124);
    rejected(48000);
    put_time((uintptr_t) &model, 0xe8, -1);
    rejected(48000);
    put_time(phrase_full(), 0x68, 0);
    rejected(48000);
    put_time(phrase_full(), 0x68, 3600001);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(phrase_full(), 0x58, 0);
    rejected(48000);
}
static void invalid_geometry(void) {
    fixture(0, 0, 0);
    put_ptr(phrase_full(), 0x40, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_int((uintptr_t) &shared, 0, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_int((uintptr_t) &shared, 8, -1);
    rejected(48000);
    fixture(0, 0, 0);
    put_int((uintptr_t) &shared, 0, INT_MAX);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[PHRASE], 0x2c, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[PHRASE], 0x24, INT_MAX);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[PHRASE], 0x20, INT_MAX);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[VIEW], 0x20, INT_MIN);
    put_int(node[FIELD], 0x20, -1);
    rejected(48000);
}
static void invalid_trees(void) {
    fixture(0, 0, 0);
    put_int(node[PANEL], 0x50, -1);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[PANEL], 0x50, 65);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[PANEL], 0x48, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[PANEL], 0x40, 0);
    rejected(48000);
    fixture(0, 0, 0);
    children[PANEL][0] = 0;
    rejected(48000);
    fixture(0, 0, 0);
    link_child(GROUP, PHRASE);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[GROUP], 0x18, node[GROUP]);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[FIELD], 0x18, node[FIELD]);
    rejected(48000);
    fixture(0, 0, 0);
    put_ptr(node[PHRASE], 0x18, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[GROUP], 0x50, 0);
    rejected(48000);
    fixture(0, 0, 0);
    put_int(node[VIEW], 0x50, 65);
    rejected(48000);
    fixture(0, 0, 0);
    children[VIEW][1] = 0;
    put_int(node[VIEW], 0x50, 2);
    rejected(48000);
    fixture(0, 0, 0);
    link_child(VIEW, OVERLAY);
    put_ptr(node[OVERLAY], 0x18, node[OVERLAY]);
    rejected(48000);
    fixture(0, 0, 0);
    int rect[4];
    double duration;
    assert(!vl_phrase_geometry(0, rect, &duration, 48000));
    assert(!vl_phrase_geometry(node[PANEL], 0, &duration, 48000));
    assert(!vl_phrase_geometry(node[PANEL], rect, 0, 48000));
    struct vlp_search search = {0, 0};
    assert(!vlp_find(node[PANEL], 4, &search));
}
int main(void) {
    layouts();
    visibility();
    obstacles();
    clocks();
    invalid_geometry();
    invalid_trees();
    puts("vocal phrase geometry: passed");
    return 0;
}

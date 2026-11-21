/* SPDX-License-Identifier: GPL-3.0-only */
#define main vocal_phrase_suite
#include "vocal_lines_phrase_test.c"
#undef main
#include <math.h>
#include <stdatomic.h>
#include "../../src/azmod/sdk_status_native.h"
#include "../../src/azmod/vocal_lines_projection.h"

#define WIDGET_COMPONENT_OFF 0x86a0
static const size_t widget_offsets[3] = {0x90, 0x87f8, 0x10f60};
static struct {
    uintptr_t base;
    int deck;
} g_wwbbase[8];
static _Atomic int g_wwbbase_n;
static int widget_live[8];
static uintptr_t wwb_widget_live(int slot, int mode) {
    if (slot < 0 || slot >= 8 || !widget_live[slot]) return 0;
    return g_wwbbase[slot].base + widget_offsets[mode];
}
static struct azstatus_native_receipt loads[4];
static int load_available = 1, flip_on_pixels;
int ds_native_load_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (!load_available || deck >= 4) return 0;
    *out = loads[deck];
    return 1;
}
static int settings[2] = {1, 1}, active = 1, light;
#define AZF_VOCAL_LINES_WAVE 0
#define AZF_VOCAL_LINES_PHRASES 1
static int az_on(int feature) { return settings[feature]; }
static int az_active(int feature) {
    (void) feature;
    return active;
}
#define AZMOD_HAVE_DAYMODE 1
static _Atomic int g_dm_on;
static int t_dm_bypass;
static int dm_flip_on(void) { return light; }
static int paints, repaints;
static int painted[128][4];
static uint32_t ink;
static void set_color(void *g, uint32_t *value) {
    assert(g && t_dm_bypass);
    ink = *value;
}
static void fill_rect(void *g, int x, int y, int w, int h) {
    assert(g && t_dm_bypass && paints < 128);
    painted[paints][0] = x;
    painted[paints][1] = y;
    painted[paints][2] = w;
    painted[paints++][3] = h;
}
static void repaint(void *component) {
    assert(component);
    ++repaints;
}
#define EP_23af140 ((uintptr_t) set_color)
#define EP_23af680 ((uintptr_t) fill_rect)
#define JUCE_COMPONENT_REPAINT ((uintptr_t) repaint)
static uintptr_t vtables[6][4];
#define EP_2d0b700 ((uintptr_t) &vtables[0][2])
#define EP_2cee580 ((uintptr_t) &vtables[1][2])
#define EP_2cfe3a8 ((uintptr_t) &vtables[2][2])
#define KT_DI2_VT ((uintptr_t) &vtables[3][2])
#define KT_DI4_VT ((uintptr_t) &vtables[4][2])
#define PM_WAVEBOX_VT ((uintptr_t) &vtables[5][2])
/* Real owned native-memory coordinate walker used by the geometry implementation. */

static az_vocal_interval intervals[2] = {{10, 12}, {14, 15}};
static uint64_t revision = 1;
static int cache_available = 1;
uint64_t vocal_lines_revision(unsigned deck) { return deck < 4 ? revision : 0; }
uint32_t vocal_lines_sample_rate(unsigned deck, uint64_t epoch) {
    return cache_available && deck < 4 && epoch == loads[deck].load_generation ? 48000 : 0;
}
int vocal_lines_pixels(unsigned deck, uint64_t epoch, double start, double stop, int width,
                       uint8_t out[VOCAL_LINES_WIDTH]) {
    if (!vocal_lines_sample_rate(deck, epoch)) {
        memset(out, 0, VOCAL_LINES_WIDTH);
        return 0;
    }
    int result = vocal_lines_project(intervals, 2, 48000, start, stop, width, out);
    if (flip_on_pixels) ++loads[deck].load_generation;
    return result;
}
#include "../../src/azmod/vocal_indicator.c"

static union {
    uint64_t aligned;
    unsigned char bytes[0x1a000];
} wave[4];
static union native_object boxes[4], obstruction;
static uintptr_t box_children[4][3];
static int64_t position = 10000;
static uintptr_t rgb(unsigned deck) { return g_wwbbase[deck].base + widget_offsets[1]; }
static void component(uintptr_t at, uintptr_t parent, uintptr_t vt, int x, int y, int w, int h) {
    put_ptr(at, 0, vt);
    put_ptr(at, 0x18, parent);
    put_int(at, 0xc0, 2);
    put_int(at, 0x20, x);
    put_int(at, 0x24, y);
    put_int(at, 0x28, w);
    put_int(at, 0x2c, h);
}
static void draw_fixture(void) {
    firmware = 2;
    fixture(0, 0, 0);
    paints = 0;
    repaints = 0;
    memset(wave, 0, sizeof wave);
    memset(boxes, 0, sizeof boxes);
    memset(vl_frames, 0, sizeof vl_frames);
    memset(vl_pending, 0, sizeof vl_pending);
    vl_depth = 0;
    memset(widget_live, 0, sizeof widget_live);
    load_available = cache_available = active = 1;
    flip_on_pixels = 0;
    settings[0] = settings[1] = 1;
    light = 0;
    g_dm_on = 0;
    position = 10000;
    intervals[0] = (az_vocal_interval) {10, 12};
    intervals[1] = (az_vocal_interval) {14, 15};
    g_wwbbase_n = 4;
    for (unsigned d = 0; d < 4; d++) {
        g_wwbbase[d].base = (uintptr_t) &wave[d];
        g_wwbbase[d].deck = (int) d;
        widget_live[d] = 1;
        loads[d] = (struct azstatus_native_receipt) {.a = AZN_LOADED, .unit = 20 + d, .epoch = 30 + d, .load_generation = 40 + d};
        uintptr_t box = (uintptr_t) &boxes[d];
        component(box, node[VIEW], PM_WAVEBOX_VT, 100, 100 + (int) d * 105, 500, 100);
        component(rgb(d) + WIDGET_COMPONENT_OFF, box, (uintptr_t) &vtables[5][2], 0, 0, 500, 100);
        put_ptr(rgb(d), 0x108, 100 + d);
        put_int(g_wwbbase[d].base, 0x197c8, 250);
        put_int(g_wwbbase[d].base, 0x60, 500);
        box_children[d][0] = rgb(d) + WIDGET_COMPONENT_OFF;
        put_ptr(box, 0x40, (uintptr_t) box_children[d]);
        put_int(box, 0x48, 3);
        put_int(box, 0x50, 1);
        children[VIEW][d + 1] = box;
    }
    put_int(node[VIEW], 0x50, 5);
}
static void begin(unsigned d, float zoom) { vx_upd_begin((int) d, (long) g_wwbbase[d].base, (long) &position, zoom); }
static void commit(unsigned d, float zoom) {
    begin(d, zoom);
    vx_upd_rendered();
    vx_upd_commit(0);
}
static void frame_lifecycle(void) {
    draw_fixture();
    struct vl_frame frame;
    assert(!vl_frame_get(4, &frame));
    assert(!vl_frame_get(0, &frame));
    begin(0, .25f);
    vx_upd_commit(0);
    assert(!vl_frame_get(0, &frame)); /* old image is no render proof */
    commit(0, .25f);
    assert(vl_frame_get(0, &frame));
    position = 20000;
    begin(0, .5f);
    vx_upd_commit(1);
    assert(vl_frame_get(0, &frame));
    assert(compare_timestamps(frame.position_ms, 10000, 1000, 48000, 1) == AZ_TIME_EQUAL);
    begin(0, .5f);
    ++loads[0].epoch;
    vx_upd_rendered();
    vx_upd_commit(0);
    assert(!vl_frame_get(0, &frame));
    commit(0, .5f);
    assert(vl_frame_get(0, &frame));
    put_ptr(rgb(0), 0x108, 99);
    assert(!vl_frame_get(0, &frame));
    commit(0, .5f);
    widget_live[0] = 0;
    assert(!vl_frame_get(0, &frame));
    widget_live[0] = 1;
    put_int(rgb(0) + WIDGET_COMPONENT_OFF, 0xc0, 0);
    commit(0, .5f);
    assert(!vl_frame_get(0, &frame));
    draw_fixture();
    begin(0, .25f);
    begin(1, .5f);
    vx_upd_rendered();
    vx_upd_commit(0);
    vx_upd_rendered();
    vx_upd_commit(0);
    assert(vl_frame_get(0, &frame));
    assert(vl_frame_get(1, &frame));
    for (int i = 0; i < 10; i++) begin(0, .25f);
    vx_upd_rendered();
    for (int i = 0; i < 10; i++) vx_upd_commit(1);
    vx_upd_commit(0);
    vx_upd_rendered();
    assert(vl_depth == 0);
}
static void rejected_frames(void) {
    draw_fixture();
    struct vl_frame frame = {0};
    assert(!vl_frame_input(&frame, -1, g_wwbbase[0].base, (uintptr_t) &position, .25f));
    assert(!vl_frame_input(&frame, 0, g_wwbbase[0].base, 8, .25f));
    assert(!vl_frame_input(&frame, 0, g_wwbbase[0].base, (uintptr_t) &position + 1, .25f));
    assert(!vl_frame_input(&frame, 0, g_wwbbase[0].base, (uintptr_t) &position, NAN));
    assert(!vl_frame_input(&frame, 0, 1234, (uintptr_t) &position, .25f));
    load_available = 0;
    commit(0, .25f);
    assert(!vl_frame_get(0, &frame));
    load_available = 1;
    loads[0].a = AZN_LOADING;
    commit(0, .25f);
    assert(!vl_frame_get(0, &frame));
    loads[0].a = AZN_LOADED;
    put_ptr(rgb(0), 0x108, 0);
    commit(0, .25f);
    assert(!vl_frame_get(0, &frame));
    put_ptr(rgb(0), 0x108, 100);
    put_int(g_wwbbase[0].base, 0x60, 1281);
    commit(0, .25f);
    assert(!vl_frame_get(0, &frame));
    put_int(g_wwbbase[0].base, 0x60, 500);
    put_int(g_wwbbase[0].base, 0x197c8, -1);
    commit(0, .25f);
    assert(!vl_frame_get(0, &frame));
    put_int(g_wwbbase[0].base, 0x197c8, 250);
    commit(0, .001f);
    assert(vl_frame_get(0, &frame));
    assert(frame.zoom == .01);
    begin(7, .25f);
    vx_upd_rendered();
    vx_upd_commit(0);
}
static void scrolling(void) {
    draw_fixture();
    commit(0, .25f);
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(paints == 2 && painted[0][1] == 125);
    assert(painted[0][0] >= 349 && painted[0][0] <= 350 && painted[0][0] + painted[0][2] == 425);
    position = 11000;
    commit(0, .25f);
    paints = 0;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(paints == 2 && painted[0][0] == 312 && painted[0][2] == 76);
    paints = 0;
    modal = 1;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(!paints);
    modal = 0;
    paints = 0;
    flip_on_pixels = 1;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(!paints);
    flip_on_pixels = 0;
    commit(0, .25f);
    settings[0] = 0;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(!paints);
    settings[0] = 1;
    g_dm_on = light = 1;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(ink == (0xff000000u | VX_RGB_LIGHT));
    put_int(rgb(0) + WIDGET_COMPONENT_OFF, 0x28, 499);
    paints = 0;
    vx_wave_paint(node[VIEW], (void *) 1);
    assert(!paints);
}
static void phrases_and_repaint(void) {
    draw_fixture();
    vx_paint((void *) 1, node[PANEL], 0);
    assert(paints == 2);
    assert(painted[0][1] == 232 && painted[0][3] == 2);
    paints = 0;
    settings[1] = 0;
    vx_paint((void *) 1, node[PANEL], 0);
    assert(!paints);
    settings[1] = 1;
    cache_available = 0;
    vx_paint((void *) 1, node[PANEL], 0);
    assert(!paints);
    cache_available = 1;
    flip_on_pixels = 1;
    vx_paint((void *) 1, node[PANEL], 0);
    assert(!paints);
    flip_on_pixels = 0;
    ++revision;
    vx_title(node[PANEL], 0);
    assert(repaints == 2);
    vx_title(node[PANEL], 0);
    assert(repaints == 2);
    settings[0] = 0;
    vx_title(node[PANEL], 0);
    assert(repaints == 4);
    vx_title(node[PANEL], -1);
    vx_paint(NULL, node[PANEL], 0);
    vx_wave_paint(node[VIEW], NULL);
}
static void clipping(void) {
    draw_fixture();
    commit(0, .25f);
    uintptr_t obstacle = (uintptr_t) &obstruction;
    component(obstacle, node[VIEW], KT_DI2_VT, 100, 100, 250, 100);
    put_int(obstacle, 0x50, 0);
    children[VIEW][5] = obstacle;
    put_int(node[VIEW], 0x50, 6);
    int r[4], ph;
    assert(vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    assert(r[0] == 272);
    component(obstacle, node[VIEW], (uintptr_t) &vtables[5][2], 400, 100, 100, 100);
    assert(vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    assert(r[0] + r[2] == 400);
    component(obstacle, node[VIEW], (uintptr_t) &vtables[5][2], 300, 100, 100, 100);
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    component(obstacle, node[VIEW], EP_2d0b700, 0, 100, 600, 100);
    component(node[EXTRA], obstacle, PM_WAVEBOX_VT, 400, 0, 100, 100);
    children[OVERLAY][0] = node[EXTRA];
    put_ptr(obstacle, 0x40, (uintptr_t) children[OVERLAY]);
    put_int(obstacle, 0x50, 1);
    assert(vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    assert(r[0] + r[2] == 400);
    put_int(obstacle, 0x50, -1);
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    put_int(obstacle, 0x50, 1);
    children[OVERLAY][0] = 0;
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    int left = 100, right = 600, budget = 0;
    vx_cut(obstacle, 125, 350, &left, &right, 4, &budget);
    assert(left == right);
    put_int(node[VIEW], 0x50, 5);
    put_int((uintptr_t) &boxes[0], 0x2c, 20);
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    put_int((uintptr_t) &boxes[0], 0x2c, 100);
    put_int(node[VIEW], 0x50, 65);
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
    put_int(node[VIEW], 0x50, 5);
    put_ptr((uintptr_t) &boxes[0], 0x18, 0);
    assert(!vx_wave_rect(node[VIEW], &vl_frames[0], r, &ph));
}
int main(void) {
    vocal_phrase_suite();
    frame_lifecycle();
    rejected_frames();
    scrolling();
    phrases_and_repaint();
    clipping();
    puts("vocal lines native rendering: passed");
    return 0;
}
